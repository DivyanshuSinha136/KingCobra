#ifndef KC_VALUE_H
#define KC_VALUE_H
#include "kc_common.h"
#include "ast.h"

typedef enum {
    VAL_NULL, VAL_BOOL, VAL_INT, VAL_FLOAT, VAL_STRING, VAL_ARRAY,
    VAL_CLOSURE, VAL_NATIVE, VAL_MODULE, VAL_HANDLE, VAL_EXTERN, VAL_STRUCT,
    VAL_NATIVE_LIB,
    VAL_PTR, VAL_BUFFER,
} KcValType;

typedef struct KcValue KcValue;
typedef struct KcEnv KcEnv;
typedef struct KcInterp KcInterp;
typedef struct KcPtr KcPtr;       /* full definition: ptr.h */
typedef struct KcBuffer KcBuffer; /* full definition: ptr.h */

/* Object state bits. A freed object is a TOMBSTONE: its storage is returned
 * to the heap, it reads as empty (len 0 / count 0) to code that does not
 * check, and the interpreter reports "use after free" for any real use. */
#define KC_STR_INLINE   1u   /* bytes live in the same block as the header */
#define KC_STR_IMMORTAL 2u   /* static data (a string literal): cannot be freed */
#define KC_STR_FREED    4u
#define KC_STR_OWNED    8u   /* `data` is a malloc'd buffer handed over to the string */
#define KC_OBJ_FREED    1u   /* arrays and structs */

typedef struct { char *data; int len; uint32_t flags; } KcString;
typedef struct { KcValue *items; int count, cap; uint32_t flags; } KcArray;
typedef struct {
    KcNode *fnnode; KcEnv *env; char *name;
    int call_count;      /* invocation counter, drives tiering */
    int jit_tried;        /* 1 once we've attempted compilation (success or not) */
    KcEnv *statics;       /* `static` locals of this function (v0.8): a scope between the frame and the closure env */
    void *jit_code;        /* executable buffer if the closure JIT'd successfully */
    size_t jit_size;
} KcClosure;

typedef KcValue (*KcNativeFn)(KcInterp *ip, KcValue *args, int nargs, KcPos pos);
typedef struct { char *name; KcNativeFn fn; } KcModuleEntry;
typedef struct { char *name; KcModuleEntry *entries; int count; } KcModule;

/* A struct instance: reference type (like arrays), so copies of a
 * KcValue share the same underlying fields — mutating one field
 * through any copy is visible through every other. */
typedef struct { char *type_name; char **names; KcValue *values; int count; uint32_t flags; } KcStructInst;

/* A native library opened via `import "lib.so" as alias;` (or a .o file
 * linked into one first — see bind_native_module in interp.c). Member
 * calls on this value (`alias.symbol(args)`) resolve the symbol with
 * dlsym at call time and call it with a calling convention inferred
 * from the actual argument values — see kc_ffi_call_dynamic in ffi.c. */
typedef struct { void *dl_handle; char *label; } KcNativeLib;

/* opaque native handle — memory buffers, sockets, jit buffers, etc. */
typedef enum { H_MEMBUF, H_SOCKET, H_JITBUF, H_FILE, H_REGEX, H_NLP, H_NN, H_PROC, H_HTTP, H_CSV, H_FBIN, H_MMU, H_AUDIO, H_SCORE } KcHandleKind;
typedef struct { KcHandleKind kind; void *ptr; size_t size; intptr_t extra; } KcHandle;

/* extern (FFI) function descriptor */
typedef struct {
    char *name, *symbol, *from_lib;
    int nargs;
    char ptypes[8];     /* 'i' = int-like, 'f' = double-like, per parameter */
    char ret_type[16];  /* "int" | "float" | "void" */
    void *fnptr;
    void *dl_handle;
    char ptr_ret[40];   /* non-empty when declared `-> *T`: the int result is turned into a pointer */
} KcExternFn;

struct KcValue {
    KcValType type;
    union {
        bool b;
        int64_t i;
        double f;
        KcString *s;
        KcArray *arr;
        KcClosure *closure;
        KcNativeFn native;
        KcModule *module;
        KcHandle *handle;
        KcExternFn *ext;
        KcStructInst *strct;
        KcNativeLib *native_lib;
        KcPtr *ptr;
        KcBuffer *buf;
    } as;
};

static inline KcValue kc_null(void) { KcValue v; v.type = VAL_NULL; return v; }
static inline KcValue kc_bool(bool b) { KcValue v; v.type = VAL_BOOL; v.as.b = b; return v; }
static inline KcValue kc_int(int64_t i) { KcValue v; v.type = VAL_INT; v.as.i = i; return v; }
static inline KcValue kc_float(double f) { KcValue v; v.type = VAL_FLOAT; v.as.f = f; return v; }
KcValue kc_string(const char *s);
KcValue kc_string_take(char *s, int len); /* takes ownership */
KcValue kc_array_new(void);
void kc_array_push(KcValue arr, KcValue v);
bool kc_ptr_equal(KcValue a, KcValue b);           /* ptr.c */
char *kc_ptr_display(KcValue v);                  /* ptr.c, caller frees */
char *kc_buffer_display(KcValue v);               /* ptr.c, caller frees */
KcStructInst *kc_struct_alloc(void);               /* zeroed flags; header from the heap */
KcValue kc_string_adopt(char *s, int len);        /* like kc_string_take, and the string owns (and frees) `s` */

/* Live-object accounting for mem_stats()/--leaks (created minus freed). */
typedef struct {
    uint64_t strings, arrays, structs;              /* ever created */
    uint64_t strings_freed, arrays_freed, structs_freed;
} KcLiveCounts;
extern KcLiveCounts kc_live;
/* Set the first time any string/array/struct is freed. Until then no tombstone can exist, so the
 * interpreter skips every use-after-free check with a single predictable branch. */
extern bool kc_any_freed;

static inline bool kc_value_freed(KcValue v) {
    switch (v.type) {
        case VAL_STRING: return (v.as.s->flags & KC_STR_FREED) != 0;
        case VAL_ARRAY:  return (v.as.arr->flags & KC_OBJ_FREED) != 0;
        case VAL_STRUCT: return (v.as.strct->flags & KC_OBJ_FREED) != 0;
        default: return false;
    }
}
bool kc_truthy_slow(KcValue v);
static inline bool kc_truthy(KcValue v) {
    if (v.type == VAL_BOOL) return v.as.b;
    if (v.type == VAL_INT) return v.as.i != 0;
    return kc_truthy_slow(v);
}
/* Single-allocation string: header and bytes in one block (len+1 bytes, NUL-terminated). */
KcValue kc_string_alloc(int len, char **data_out);
bool kc_equal(KcValue a, KcValue b);
void kc_print_value(KcValue v, bool nl);
const char *kc_type_name(KcValue v);
char *kc_to_display_string(KcValue v); /* caller frees */

/* ---- dynamic objects ----------------------------------------------------
 * An "object" is an ordinary VAL_STRUCT whose type_name points at the
 * shared kc_object_type_name string. Unlike a declared `struct`, its field
 * set is open: keys can be added and removed at run time. This is what
 * std.json produces for `{...}` and what std.csv produces for header
 * records. Field syntax (`o.name`) works as for any struct; the
 * interpreter additionally allows `o["key"]` reads/writes on objects.
 * Keys are NUL-terminated C strings (a key containing a NUL byte is not
 * representable). Insertion order is preserved. Lookup is linear. */
extern const char kc_object_type_name[];
bool kc_is_object(KcValue v);
KcValue kc_object_new(void);
int kc_object_index(KcValue obj, const char *key, size_t klen);       /* -1 if absent */
KcValue *kc_object_get(KcValue obj, const char *key, size_t klen);    /* NULL if absent */
void kc_object_set(KcValue obj, const char *key, size_t klen, KcValue v); /* replace or append */
bool kc_object_remove(KcValue obj, const char *key, size_t klen);     /* true if it existed */

#endif
