#ifndef KC_INTERP_H
#define KC_INTERP_H
#include "value.h"
#include "env.h"
#include "ast.h"

typedef struct {
    char *name; char **field_names; int field_count;
    char **field_types;   /* declared type text per field ("u8", "*Node", "u8[16]", ...) */
    bool packed;          /* `packed struct`: no padding, alignment 1 */
    bool is_union;        /* `union`: every field at offset 0 (memory layout only) */
    void *layout;         /* KcLayout*, built lazily by ptr.c */
    const char *doc;      /* `///` lines before the declaration (help()) */
} KcStructTypeDef;

typedef struct KcDeferEntry { KcNode *stmt; KcEnv *env; } KcDeferEntry;   /* a pending `defer`, with the scope it runs in */
typedef struct KcMethodEntry { const char *type, *name; KcValue fn; bool instance; } KcMethodEntry;
struct KcInterp {
    KcEnv *builtins_env; /* print/len/input/etc. — shared base scope for the
                            main script AND every file imported via
                            `import "path.kc";`, so imported files can use
                            builtins without inheriting the importer's own
                            globals (true per-file isolation otherwise). */
    KcEnv *globals;
    const char *filename;
    int call_depth;
    const char *sig_label;        /* label carried by a pending `break label;` / `continue label;` (interned) */
    struct KcMethodEntry *methods; /* impl-block methods: (type, name) -> closure */
    int nmethods, methods_cap;
    KcDeferEntry *defers;         /* every pending `defer`, innermost last: a caught error runs the ones above its try */
    int ndefers, defers_cap;
    KcValue thrown;               /* the value of the `throw` being propagated */
    bool has_thrown;
    KcPos thrown_pos;             /* where the `throw` that produced `thrown` executed (for test reports) */
    int in_unsafe; /* >0 while executing inside an `unsafe { }` block */
    int in_comptime; /* >0 while executing inside a `comptime { }` block */
    /* JIT hot-loop counters: node pointer -> hit count (simple linear table) */
    void *jit_table;
    /* `struct Name { ... }` declarations, registered as they're executed;
       struct literals validate their field set against this. */
    KcStructTypeDef *struct_types;
    int struct_types_count, struct_types_cap;
};

KcInterp *kc_interp_new(const char *filename);
void kc_register_builtins(KcInterp *ip);
KcStructTypeDef *kc_find_struct_type(KcInterp *ip, const char *name);
void kc_interp_register_module(KcInterp *ip, KcModule *mod, const char *alias);
KcValue kc_call_value(KcInterp *ip, KcValue callee, KcValue *args, int nargs, KcPos pos);
void kc_interp_run(KcInterp *ip, KcNode *program);
extern int kc_script_argc; extern char **kc_script_argv;   /* argv[0] = script path, then its arguments */
extern const char *const kc_std_module_names[];            /* NULL-terminated: the `import std.<name>` modules */
extern int kc_main_exit_code;                              /* status returned by main(), if it returned an int */
bool kc_interp_try_call(KcInterp *ip, KcValue fn, KcValue *err_out);   /* contained call; see `try` */

/* REPL support (compiler/src/repl.c) */
bool kc_interp_exec_repl(KcInterp *ip, KcNode *stmt, KcValue *out);
void kc_interp_run_nomain(KcInterp *ip, KcNode *program);
void kc_interp_reset_state(KcInterp *ip);

/* Call at the top of any native function with an externally-observable
 * side effect (file/network/process I/O, raw machine code) so it
 * refuses to run inside a `comptime { }` block instead of silently
 * treating something non-deterministic or effectful as "the compile-
 * time constant". `what` is used in the error message, e.g. "io.write_file". */
void kc_forbid_in_comptime(KcInterp *ip, KcPos pos, const char *what);

/* std module registration entry points, implemented under std/ */
KcModule *kc_mod_system(void);
KcModule *kc_mod_memory(void);
KcModule *kc_mod_network(void);
KcModule *kc_mod_http(void);
KcModule *kc_mod_rhex(void);
KcModule *kc_mod_music_note(void);
KcModule *kc_mod_io(void);
KcModule *kc_mod_vector(void);
KcModule *kc_mod_regx(void);
KcModule *kc_mod_nlp(void);
KcModule *kc_mod_nn(void);
KcModule *kc_mod_gui(void);
KcModule *kc_mod_mmio(void);
KcModule *kc_mod_acpi(void);
KcModule *kc_mod_json(void);
KcModule *kc_mod_csv(void);
KcModule *kc_mod_fbin(void);
KcModule *kc_mod_mmu(void);
KcModule *kc_mod_game(void);
KcModule *kc_mod_ascii_screen(void);
KcModule *kc_mod_tts(void);

#endif
