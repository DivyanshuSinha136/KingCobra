#include "ffi.h"
#include "kc_platform.h"

static char classify_type(const char *t) {
    if (!t) return 'i';
    if (strcmp(t, "float") == 0 || strcmp(t, "double") == 0) return 'f';
    return 'i'; /* int, bool, string(as ptr-ish is NOT supported — treated as int-classified but disallowed at call time) */
}

KcExternFn *kc_ffi_declare(KcNode *decl, KcPos pos) {
    KcExternFn *ext = kc_malloc(sizeof(KcExternFn));
    memset(ext, 0, sizeof(*ext));
    ext->name = kc_strdup(decl->as.externdecl.name);
    ext->symbol = kc_strdup(decl->as.externdecl.real_symbol);
    ext->from_lib = decl->as.externdecl.from_lib ? kc_strdup(decl->as.externdecl.from_lib) : NULL;
    ext->nargs = decl->as.externdecl.param_types.count;
    if (ext->nargs > 6) kc_error_at(pos, "extern '%s': KingCobra's native FFI supports at most 6 parameters", ext->name);
    if (decl->as.externdecl.ret_type[0] == '*') snprintf(ext->ptr_ret, sizeof ext->ptr_ret, "%s", decl->as.externdecl.ret_type);
    char kind = classify_type(decl->as.externdecl.ret_type);
    if (strcmp(decl->as.externdecl.ret_type, "void") == 0) strcpy(ext->ret_type, "void");
    else strcpy(ext->ret_type, kind == 'f' ? "float" : "int");
    char first_kind = 0;
    for (int i = 0; i < ext->nargs; i++) {
        char k = classify_type(decl->as.externdecl.param_types.items[i]->as.ident.name);
        ext->ptypes[i] = k;
        if (!first_kind) first_kind = k;
        else if (k != first_kind)
            kc_error_at(pos, "extern '%s': v0.1 FFI requires all parameters to be the same kind (all int-like or all float-like); mixed signatures need libffi (see ROADMAP)", ext->name);
    }

    kc_dl_handle handle = kc_dlopen_portable(ext->from_lib);
    if (!handle) kc_error_at(pos, "extern '%s': could not load library '%s': %s", ext->name, ext->from_lib ? ext->from_lib : "(self)", kc_dlerror());
    void *sym = kc_dlsym(handle, ext->symbol);
    if (!sym) kc_error_at(pos, "extern '%s': symbol '%s' not found: %s", ext->name, ext->symbol, kc_dlerror());
    ext->fnptr = sym;
    ext->dl_handle = (void *)handle;
    return ext;
}

/* fixed-arity typed trampolines — this is what a real libffi would
   generate dynamically; here we hand-write the signatures we support. */
/* Integer arguments travel as ONE MACHINE WORD each, not as int64_t.
 * On 64-bit hosts (x86-64, aarch64, riscv64) the two are the same thing. On
 * 32-bit hosts (armhf, riscv32, x86) they are not, and the difference is
 * not cosmetic: the 32-bit AAPCS puts an int64_t argument in an EVEN
 * register PAIR (r0:r1, then r2:r3), so a callee declared `int f(int a,
 * int b)` would read `b` from r1 — the high half of our first argument —
 * and get garbage. Passing word-sized values lines the registers up with
 * what a C callee expects. The matching limitation: on a 32-bit host an
 * extern's integer arguments and result are 32-bit values (a function
 * returning a 64-bit integer cannot be called through this fixed-arity
 * trampoline; use a double or a pointer to an out-parameter). Pointer-typed
 * results are widened as UNSIGNED so addresses >= 2 GiB stay positive. */
typedef intptr_t KcW;
typedef KcW (*IFn0)(void); typedef KcW (*IFn1)(KcW); typedef KcW (*IFn2)(KcW,KcW);
typedef KcW (*IFn3)(KcW,KcW,KcW); typedef KcW (*IFn4)(KcW,KcW,KcW,KcW);
typedef KcW (*IFn5)(KcW,KcW,KcW,KcW,KcW);
typedef KcW (*IFn6)(KcW,KcW,KcW,KcW,KcW,KcW);
static inline int64_t kc_word_result(KcW r, bool is_ptr) {
    return is_ptr ? (int64_t)(uintptr_t)r : (int64_t)r;
}
typedef double (*FFn0)(void); typedef double (*FFn1)(double); typedef double (*FFn2)(double,double);
typedef double (*FFn3)(double,double,double); typedef double (*FFn4)(double,double,double,double);
typedef double (*FFn5)(double,double,double,double,double);
typedef double (*FFn6)(double,double,double,double,double,double);

KcValue kc_ffi_call(KcExternFn *ext, KcValue *args, int nargs, KcPos pos) {
    if (nargs != ext->nargs)
        kc_error_at(pos, "extern '%s' expects %d argument(s), got %d", ext->name, ext->nargs, nargs);
    bool is_float = ext->nargs > 0 && ext->ptypes[0] == 'f';

    if (is_float) {
        double a[6] = {0};
        for (int i = 0; i < nargs; i++) {
            if (args[i].type != VAL_INT && args[i].type != VAL_FLOAT)
                kc_error_at(pos, "extern '%s': argument %d must be numeric", ext->name, i + 1);
            a[i] = args[i].type == VAL_INT ? (double)args[i].as.i : args[i].as.f;
        }
        double r = 0;
        switch (ext->nargs) {
            case 0: r = ((FFn0)ext->fnptr)(); break;
            case 1: r = ((FFn1)ext->fnptr)(a[0]); break;
            case 2: r = ((FFn2)ext->fnptr)(a[0],a[1]); break;
            case 3: r = ((FFn3)ext->fnptr)(a[0],a[1],a[2]); break;
            case 4: r = ((FFn4)ext->fnptr)(a[0],a[1],a[2],a[3]); break;
            case 5: r = ((FFn5)ext->fnptr)(a[0],a[1],a[2],a[3],a[4]); break;
            case 6: r = ((FFn6)ext->fnptr)(a[0],a[1],a[2],a[3],a[4],a[5]); break;
        }
        if (strcmp(ext->ret_type, "void") == 0) return kc_null();
        return kc_float(r);
    } else {
        KcW a[6] = {0};
        for (int i = 0; i < nargs; i++) {
            if (args[i].type == VAL_INT) a[i] = (KcW)args[i].as.i;
            else if (args[i].type == VAL_BOOL) a[i] = (KcW)args[i].as.b;
            else if (args[i].type == VAL_STRING) a[i] = (KcW)(intptr_t)args[i].as.s->data;
            else kc_error_at(pos, "extern '%s': argument %d must be int, bool, or string", ext->name, i + 1);
        }
        KcW r = 0;
        switch (ext->nargs) {
            case 0: r = ((IFn0)ext->fnptr)(); break;
            case 1: r = ((IFn1)ext->fnptr)(a[0]); break;
            case 2: r = ((IFn2)ext->fnptr)(a[0],a[1]); break;
            case 3: r = ((IFn3)ext->fnptr)(a[0],a[1],a[2]); break;
            case 4: r = ((IFn4)ext->fnptr)(a[0],a[1],a[2],a[3]); break;
            case 5: r = ((IFn5)ext->fnptr)(a[0],a[1],a[2],a[3],a[4]); break;
            case 6: r = ((IFn6)ext->fnptr)(a[0],a[1],a[2],a[3],a[4],a[5]); break;
        }
        if (strcmp(ext->ret_type, "void") == 0) return kc_null();
        return kc_int(kc_word_result(r, ext->ptr_ret[0] != 0));
    }
}

KcValue kc_ffi_call_dynamic(void *fnptr, KcValue *args, int nargs, KcPos pos) {
    if (nargs > 6) kc_error_at(pos, "native import call: at most 6 arguments are supported (got %d)", nargs);
    bool is_float = nargs > 0 && args[0].type == VAL_FLOAT;

    if (is_float) {
        double a[6] = {0};
        for (int i = 0; i < nargs; i++) {
            if (args[i].type != VAL_INT && args[i].type != VAL_FLOAT)
                kc_error_at(pos, "native import call: argument %d must be numeric (argument 1 was a float, so all arguments are passed as doubles)", i + 1);
            a[i] = args[i].type == VAL_INT ? (double)args[i].as.i : args[i].as.f;
        }
        double r = 0;
        switch (nargs) {
            case 0: r = ((FFn0)fnptr)(); break;
            case 1: r = ((FFn1)fnptr)(a[0]); break;
            case 2: r = ((FFn2)fnptr)(a[0],a[1]); break;
            case 3: r = ((FFn3)fnptr)(a[0],a[1],a[2]); break;
            case 4: r = ((FFn4)fnptr)(a[0],a[1],a[2],a[3]); break;
            case 5: r = ((FFn5)fnptr)(a[0],a[1],a[2],a[3],a[4]); break;
            case 6: r = ((FFn6)fnptr)(a[0],a[1],a[2],a[3],a[4],a[5]); break;
        }
        return kc_float(r);
    } else {
        KcW a[6] = {0};
        for (int i = 0; i < nargs; i++) {
            if (args[i].type == VAL_INT) a[i] = (KcW)args[i].as.i;
            else if (args[i].type == VAL_BOOL) a[i] = (KcW)args[i].as.b;
            else if (args[i].type == VAL_STRING) a[i] = (KcW)(intptr_t)args[i].as.s->data;
            else kc_error_at(pos, "native import call: argument %d must be int, bool, or string", i + 1);
        }
        KcW r = 0;
        switch (nargs) {
            case 0: r = ((IFn0)fnptr)(); break;
            case 1: r = ((IFn1)fnptr)(a[0]); break;
            case 2: r = ((IFn2)fnptr)(a[0],a[1]); break;
            case 3: r = ((IFn3)fnptr)(a[0],a[1],a[2]); break;
            case 4: r = ((IFn4)fnptr)(a[0],a[1],a[2],a[3]); break;
            case 5: r = ((IFn5)fnptr)(a[0],a[1],a[2],a[3],a[4]); break;
            case 6: r = ((IFn6)fnptr)(a[0],a[1],a[2],a[3],a[4],a[5]); break;
        }
        return kc_int(kc_word_result(r, false));
    }
}
