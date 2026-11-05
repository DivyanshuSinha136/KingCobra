#include "interp.h"
#include "kc_platform.h"
#include "ptr.h"
#include "mem.h"
#include "lib.h"
#include "preproc.h"
#include <math.h>

static KcValue b_print(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; (void)pos;
    for (int i = 0; i < n; i++) { if (i) fputc(' ', stdout); kc_print_value(a[i], false); }
    fputc('\n', stdout);
    return kc_null();
}
static KcValue b_len(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    if (n != 1) kc_error_at(pos, "len() takes exactly 1 argument");
    if (a[0].type == VAL_ARRAY) return kc_int(a[0].as.arr->count);
    if (a[0].type == VAL_STRING) return kc_int(a[0].as.s->len);
    if (a[0].type == VAL_STRUCT) return kc_int(a[0].as.strct->count);
    if (a[0].type == VAL_BUFFER) { KcValue out; if (kc_ptr_property(ip, a[0], "len", pos, &out)) return out; }
    kc_error_at(pos, "len() requires an array, string, buffer, or object");
    return kc_null();
}
static KcValue b_str(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; (void)pos;
    if (n != 1) kc_error_at(pos, "str() takes exactly 1 argument");
    if (a[0].type == VAL_INT) {                 /* hot path: no snprintf, one allocation */
        char tmp[24]; int k = 0;
        uint64_t u = a[0].as.i < 0 ? (uint64_t)0 - (uint64_t)a[0].as.i : (uint64_t)a[0].as.i;
        do { tmp[k++] = (char)('0' + u % 10); u /= 10; } while (u);
        int len = k + (a[0].as.i < 0), j = 0; char *dst;
        KcValue r = kc_string_alloc(len, &dst);
        if (a[0].as.i < 0) dst[j++] = '-';
        while (k) dst[j++] = tmp[--k];
        return r;
    }
    char *s = kc_to_display_string(a[0]);
    return kc_string_adopt(s, (int)strlen(s));
}
static KcValue b_int(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    if (n != 1) kc_error_at(pos, "int() takes exactly 1 argument");
    if (a[0].type == VAL_INT) return a[0];
    if (a[0].type == VAL_FLOAT) return kc_int(kc_f2i(a[0].as.f));
    if (a[0].type == VAL_STRING) return kc_int(strtoll(a[0].as.s->data, NULL, 10));
    if (a[0].type == VAL_BOOL) return kc_int(a[0].as.b ? 1 : 0);
    kc_error_at(pos, "cannot convert %s to int", kc_type_name(a[0]));
    return kc_null();
}
static KcValue b_float(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    if (n != 1) kc_error_at(pos, "float() takes exactly 1 argument");
    if (a[0].type == VAL_FLOAT) return a[0];
    if (a[0].type == VAL_INT) return kc_float((double)a[0].as.i);
    if (a[0].type == VAL_STRING) return kc_float(strtod(a[0].as.s->data, NULL));
    kc_error_at(pos, "cannot convert %s to float", kc_type_name(a[0]));
    return kc_null();
}
static KcValue b_typeof(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; if (n != 1) kc_error_at(pos, "typeof() takes exactly 1 argument");
    return kc_string(kc_type_name(a[0]));
}
static KcValue b_push(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; if (n != 2 || a[0].type != VAL_ARRAY) kc_error_at(pos, "push(array, value)");
    kc_array_push(a[0], a[1]); return kc_null();
}
static KcValue b_range(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; if (n != 1 || a[0].type != VAL_INT) kc_error_at(pos, "range(n)");
    KcValue arr = kc_array_new();
    for (int64_t i = 0; i < a[0].as.i; i++) kc_array_push(arr, kc_int(i));
    return arr;
}
static KcValue b_clock(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; (void)a; (void)n; (void)pos;
    return kc_float(kc_monotonic_seconds());
}
static KcValue b_sqrt(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; if (n != 1) kc_error_at(pos, "sqrt(x)");
    return kc_float(sqrt(a[0].type == VAL_INT ? (double)a[0].as.i : a[0].as.f));
}

/* input([prompt]) -> string : prints an optional prompt (no trailing
 * newline), reads one line from stdin, trailing newline stripped. */
static KcValue b_input(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    if (n >= 1) { if (a[0].type != VAL_STRING) kc_error_at(pos, "input([prompt: string]) -> string"); fputs(a[0].as.s->data, stdout); fflush(stdout); }
    char buf[65536];
    if (!fgets(buf, sizeof buf, stdin)) return kc_string("");
    size_t len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) buf[--len] = 0;
    return kc_string(buf);
}

/* preprocess(src, filename) -> string : run the C-style preprocessor (#define/#if/#include) over source text.
 * Used by kcc so the native compiler sees exactly the text the interpreter would. */
static KcValue b_preprocess(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    if (n != 2 || a[0].type != VAL_STRING || a[1].type != VAL_STRING) kc_error_at(pos, "preprocess(source: string, filename: string) -> string");
    const char *out = kc_preprocess(a[0].as.s->data, a[1].as.s->data, NULL);
    return kc_string(out ? out : "");
}
/* pp_define("NAME") / pp_define("NAME=value") : same as the -D command-line option, for later preprocess() calls */
static KcValue b_pp_define(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    if (n != 1 || a[0].type != VAL_STRING) kc_error_at(pos, "pp_define(\"NAME[=value]\")");
    kc_pp_add_cmdline_define(a[0].as.s->data);
    return kc_null();
}

void kc_register_builtins(KcInterp *ip) {
    struct { const char *name; KcNativeFn fn; } fns[] = {
        {"print", b_print}, {"println", b_print}, {"len", b_len}, {"str", b_str},
        {"int", b_int}, {"float", b_float}, {"typeof", b_typeof}, {"push", b_push},
        {"range", b_range}, {"clock", b_clock}, {"sqrt", b_sqrt}, {"input", b_input},
        {"preprocess", b_preprocess}, {"pp_define", b_pp_define},
    };
    for (size_t i = 0; i < sizeof(fns)/sizeof(fns[0]); i++) {
        KcValue v; v.type = VAL_NATIVE; v.as.native = fns[i].fn;
        kc_env_define(ip->builtins_env, fns[i].name, v, false);
    }
    kc_register_ptr_builtins(ip);
    kc_register_mem_builtins(ip);
    kc_register_lib_builtins(ip);
    kc_register_clib_builtins(ip);
    kc_register_help_builtin(ip);
}
