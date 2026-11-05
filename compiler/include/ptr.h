/* KingCobra pointers, buffers and offsets — language-level, C-style.
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com>
 * License: MIT
 *
 * Memory model (full text in docs/POINTERS.md):
 *
 *   region   a block of bytes (alloc(), buffer(), or a string's bytes).
 *            It records its size, whether it is still alive and whether it
 *            is read-only. Regions are never un-allocated at the C level
 *            while a pointer to them could exist, so use-after-free is
 *            DETECTED (error) instead of being undefined behaviour.
 *   pointer  VAL_PTR. Five kinds:
 *              PK_REGION  region + byte offset + accessible window [lo,hi)
 *                         -> every load/store is bounds+liveness checked,
 *                            no `unsafe` needed.
 *              PK_RAW     a bare machine address (from FFI or an int cast)
 *                         -> dereferencing demands `unsafe { }`.
 *              PK_SLOT    &variable        (env + slot index)
 *              PK_ELEM    &array[i]        (array + index)
 *              PK_FIELD   &struct.field    (instance + field index)
 *            The last three point at KingCobra VALUES, not raw bytes.
 *   buffer   VAL_BUFFER: a (possibly sliced) view of a region with a
 *            read/write cursor and typed, endian-aware accessors.
 *   null     the ordinary `null` value is the null pointer.
 */
#ifndef KC_PTR_H
#define KC_PTR_H
#include "value.h"
#include "interp.h"
#include "env.h"

typedef enum {
    PT_VOID, PT_ANY,
    PT_U8, PT_I8, PT_U16, PT_I16, PT_U32, PT_I32, PT_U64, PT_I64,
    PT_F32, PT_F64, PT_BOOL, PT_PTR, PT_STRUCT,
} KcPType;

typedef struct KcLayout KcLayout;
typedef struct KcType KcType;
struct KcType { KcPType k; KcType *to; KcLayout *lay; };

typedef struct {
    char *name;
    KcType *ty;        /* element type (for arrays: the element) */
    int64_t off;       /* byte offset inside the struct */
    int64_t count;     /* 1, or N for `T[N]` */
    int64_t esize;     /* size of one element */
    bool is_array;
} KcField;

struct KcLayout {
    char *name;
    int n;
    KcField *f;
    int64_t size, align;
    bool packed, is_union, computing, done;
    /* A private view of the declaration. The KcStructTypeDef itself lives in a table that
     * realloc()s as structs are declared, so a layout must never point into it. These arrays
     * are separately allocated and stable. */
    char **fnames, **ftypes;
    int fcount;
};

typedef struct KcRegion {
    uint8_t *base;
    size_t size;
    bool alive, owned, readonly, heap;
    uint64_t id;
    int reg_index;      /* slot in the live-region registry, -1 if unregistered */
    KcPos site;         /* where it was allocated (for leak reports) */
    const char *origin; /* "alloc()", "buffer()", ... ; NULL for non-tracked views */
} KcRegion;

typedef enum { PK_REGION, PK_RAW, PK_SLOT, PK_ELEM, PK_FIELD } KcPtrKind;

struct KcPtr {
    KcPtrKind kind;
    KcType *et;          /* pointee type (PT_ANY for reference kinds) */
    bool readonly;       /* &immutable_variable */
    union {
        struct { KcRegion *reg; int64_t off, lo, hi; } mem;
        struct { uint64_t addr; } raw;
        struct { KcEnv *env; int idx; } slot;
        struct { KcArray *arr; int64_t idx; } elem;
        struct { KcStructInst *st; int idx; } field;
    } u;
};

struct KcBuffer {
    KcRegion *reg;
    int64_t start, len, pos;
    bool is_view;        /* made by slice(): cannot resize or free */
};

/* ---- types & layouts ---- */
KcType *kc_type_parse(KcInterp *ip, const char *text, KcPos pos);
int64_t kc_type_size(KcInterp *ip, KcType *t, KcPos pos);
int64_t kc_type_align(KcInterp *ip, KcType *t, KcPos pos);
int64_t kc_sizeof_text(KcInterp *ip, const char *text, int which /* 0 size, 1 align */, KcPos pos);
int kc_ptr_live_blocks(void);                       /* alloc()/buffer() blocks not yet freed */
void kc_layout_invalidate(KcStructTypeDef *def);   /* struct redeclared: recompute its layout lazily */
int64_t kc_offsetof(KcInterp *ip, const char *struct_name, const char *field, KcPos pos);

/* ---- operators ---- */
KcValue kc_value_cast(KcInterp *ip, KcValue v, const char *type_text, KcPos pos);
KcValue kc_ptr_binop(KcInterp *ip, KcOp op, KcValue a, KcValue b, KcPos pos);
KcValue kc_ptr_load(KcInterp *ip, KcValue p, KcPos pos);                 /* *p */
void    kc_ptr_store(KcInterp *ip, KcValue p, KcValue v, KcPos pos);     /* *p = v */
KcValue kc_ptr_member(KcInterp *ip, KcValue base, const char *name, KcPos pos, bool *is_array); /* &p->name */
bool    kc_ptr_is_memory_struct(KcValue p);
bool    kc_ptr_member_into(KcInterp *ip, KcValue base, const char *name, KcPos pos, KcPtr *out, bool *is_array);
KcValue kc_ptr_load_idx(KcInterp *ip, KcValue p, KcValue idx, KcPos pos);
void    kc_ptr_store_idx(KcInterp *ip, KcValue p, KcValue idx, KcValue v, KcPos pos);
KcValue kc_ptr_index(KcInterp *ip, KcValue p, KcValue idx, KcPos pos);   /* p + idx (a place) */
KcValue kc_buffer_elem(KcInterp *ip, KcValue b, KcValue idx, KcPos pos); /* &b[idx] as *u8 */

/* ---- reference pointers (&variable, &array[i], &struct.field) ---- */
KcValue kc_ptr_slot(KcEnv *env, int idx, bool readonly);
KcValue kc_ptr_elem_ref(KcArray *arr, int64_t idx);
KcValue kc_ptr_field_ref(KcStructInst *st, const char *name, KcPos pos);

/* ---- properties / methods / builtins ---- */
bool kc_ptr_property(KcInterp *ip, KcValue obj, const char *name, KcPos pos, KcValue *out);
bool kc_ptr_method(KcInterp *ip, KcValue obj, const char *name, KcValue *args, int nargs, KcPos pos, KcValue *out);
void kc_ffi_prepare_args(KcInterp *ip, KcValue *args, int nargs, KcPos pos);
KcValue kc_ptr_from_address(KcInterp *ip, uint64_t addr, const char *ptr_type_text, KcPos pos);
void kc_register_ptr_builtins(KcInterp *ip);
KcValue kc_free_memory_value(KcInterp *ip, KcValue v, KcPos pos);   /* free() of a pointer or buffer */
bool kc_value_memory_freed(KcValue v);                              /* region/buffer already freed? */
void kc_ptr_live_regions(void (*cb)(const KcRegion *r, void *ud), void *ud);

#endif
