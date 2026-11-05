#include "interp.h"
#include "ffi.h"
#include "jit.h"
#include "debug.h"
#include "parser.h"
#include "methods.h"
#include "kc_platform.h"
#include "ptr.h"
#include "mem.h"
#include "lang.h"
#include "lib.h"
#include <math.h>
#include <ctype.h>

void kc_forbid_in_comptime(KcInterp *ip, KcPos pos, const char *what) {
    if (ip->in_comptime > 0)
        kc_error_at(pos, "%s: not allowed inside a comptime block (has an external side effect)", what);
}

typedef enum { SIG_NONE, SIG_RETURN, SIG_BREAK, SIG_CONTINUE, SIG_GOTO } Signal;

static KcValue eval_expr(KcInterp *ip, KcEnv *env, KcNode *n);
static Signal exec_stmt(KcInterp *ip, KcEnv *env, KcNode *n, KcValue *ret_out);
static Signal exec_stmts_in(KcInterp *ip, KcEnv *env, KcNode *block, KcValue *ret_out);
static bool block_needs_scope(KcNode *b);
static __attribute__((noinline)) Signal exec_stmts_defer(KcInterp *ip, KcEnv *env, KcNode *block, KcValue *ret_out);

/* A function/loop body runs directly in the frame/loop scope: parameters
 * and body locals share one scope (a later `let x` simply shadows, exactly
 * as a nested scope would, because lookup is newest-first). */
static inline Signal exec_body(KcInterp *ip, KcEnv *env, KcNode *b, KcValue *ret_out) {
    return b->kind == N_BLOCK ? exec_stmts_in(ip, env, b, ret_out) : exec_stmt(ip, env, b, ret_out);
}

/* Out-of-range array access. If the array was freed it is a use-after-free, not a bounds error. */
static void array_oob(KcArray *a, int64_t idx, KcPos pos) {
    if (a->flags & KC_OBJ_FREED) kc_error_at(pos, "use after free (this array was freed)");
    kc_error_at(pos, "index %lld out of bounds (len %d)", (long long)idx, a->count);
}

static inline const char *node_name(KcNode *n) { /* interned identifier text, cached on the node */
    if (!(n->flags & NF_INTERNED)) { n->as.ident.name = (char *)kc_intern(n->as.ident.name); n->flags |= NF_INTERNED; }
    return n->as.ident.name;
}

static void stack_guard_init(void);
KcInterp *kc_interp_new(const char *filename) {
    stack_guard_init();
    KcInterp *ip = kc_malloc(sizeof(KcInterp));
    memset(ip, 0, sizeof *ip);                 /* every field starts zeroed, including ones added later */
    ip->builtins_env = kc_env_new(NULL);
    ip->globals = kc_env_new(ip->builtins_env);
    ip->filename = filename;
    ip->call_depth = 0;
    ip->in_unsafe = 0;
    ip->in_comptime = 0;
    ip->jit_table = NULL;
    ip->struct_types = NULL;
    ip->struct_types_count = 0;
    ip->struct_types_cap = 0;
    return ip;
}

static KcValue mod_value(KcModule *m) { KcValue v; v.type = VAL_MODULE; v.as.module = m; return v; }

void kc_interp_register_module(KcInterp *ip, KcModule *mod, const char *alias) {
    kc_env_define(ip->globals, alias ? alias : mod->name, mod_value(mod), false);
}

static KcValue mod_lookup(KcModule *m, const char *name, KcPos pos) {
    for (int i = 0; i < m->count; i++)
        if (strcmp(m->entries[i].name, name) == 0) { KcValue v; v.type = VAL_NATIVE; v.as.native = m->entries[i].fn; return v; }
    const char *names[256];
    int nn = m->count < 256 ? m->count : 256;
    for (int i = 0; i < nn; i++) names[i] = m->entries[i].name;
    kc_error_at(pos, "module '%s' has no member '%s'%s", m->name, name, kc_hint_for_module(names, nn, name));
    return kc_null();
}

static KcValue member_access(KcInterp *ip, KcValue obj, const char *name, KcPos pos) {
    if (obj.type == VAL_PTR || obj.type == VAL_BUFFER) {
        KcValue out;
        if (kc_ptr_property(ip, obj, name, pos, &out)) return out;
        kc_error_at(pos, "a %s has no member '%s'", kc_type_name(obj), name);
    }
    if (obj.type == VAL_MODULE) return mod_lookup(obj.as.module, name, pos);
    if (obj.type == VAL_ARRAY && strcmp(name, "len") == 0) return kc_int(obj.as.arr->count);
    if (obj.type == VAL_STRING && strcmp(name, "len") == 0) return kc_int(obj.as.s->len);
    if (obj.type == VAL_STRUCT) {
        KcStructInst *st = obj.as.strct;
        if (__builtin_expect(st->flags & KC_OBJ_FREED, 0)) kc_error_freed(obj, pos);
        for (int i = 0; i < st->count; i++)           /* interned names: one pointer compare */
            if (st->names[i] == name) return st->values[i];
        for (int i = 0; i < st->count; i++)           /* names from outside the AST (json, memory) */
            if (st->names[i][0] == name[0] && strcmp(st->names[i], name) == 0) return st->values[i];
        kc_error_at(pos, "struct '%s' has no field '%s'%s", st->type_name, name, kc_hint_for_field(st, name));
    }
    kc_error_at(pos, "value of type '%s' has no member '%s'%s", kc_type_name(obj), name, kc_method_hint(name));
    return kc_null();
}

/* ---------------- struct types ---------------- */

static void register_struct_type(KcInterp *ip, const char *name, KcNodeList *fields, char **field_types, bool packed, bool is_union, KcPos pos) {
    /* Redeclaring a name replaces the old definition (matters for the REPL;
       in a file the last declaration wins, as it does for functions). */
    KcStructTypeDef *def = NULL;
    for (int i = 0; i < ip->struct_types_count; i++)
        if (strcmp(ip->struct_types[i].name, name) == 0) { def = &ip->struct_types[i]; break; }
    bool redeclared = def != NULL;
    if (redeclared) {
        /* Memory already allocated under the old layout is NOT converted: it would be silently
           reinterpreted with the new one. Warn when the layout really changes and such memory exists. */
        bool same = def->field_count == fields->count && def->packed == packed && def->is_union == is_union;
        for (int i = 0; same && i < fields->count; i++) {
            const char *a = def->field_types ? def->field_types[i] : NULL, *b = field_types ? field_types[i] : NULL;
            if (strcmp(def->field_names[i], fields->items[i]->as.ident.name) != 0 || (a != b && (!a || !b || strcmp(a, b) != 0))) same = false;
        }
        int live = kc_ptr_live_blocks();
        if (!same && live > 0)
            kc_warn_at(pos, "struct '%s' redeclared with a different layout while %d alloc'd block(s) are still live; "
                            "that memory keeps its old bytes and will be reinterpreted with the new layout", name, live);
    }
    if (!def) {
        if (ip->struct_types_count + 1 > ip->struct_types_cap) {
            ip->struct_types_cap = ip->struct_types_cap ? ip->struct_types_cap * 2 : 4;
            ip->struct_types = kc_realloc(ip->struct_types, sizeof(KcStructTypeDef) * ip->struct_types_cap);
        }
        def = &ip->struct_types[ip->struct_types_count++];
        def->layout = NULL; def->doc = NULL;
    }
    def->name = kc_strdup(name);
    def->field_count = fields->count;
    def->field_names = kc_malloc(sizeof(char *) * fields->count);
    for (int i = 0; i < fields->count; i++) def->field_names[i] = (char *)node_name(fields->items[i]);
    def->field_types = field_types; def->packed = packed; def->is_union = is_union;
    if (redeclared) kc_layout_invalidate(def);   /* memory layout follows the new declaration */
}

KcStructTypeDef *kc_find_struct_type(KcInterp *ip, const char *name) {
    for (int i = 0; i < ip->struct_types_count; i++)
        if (strcmp(ip->struct_types[i].name, name) == 0) return &ip->struct_types[i];
    return NULL;
}

/* ---------------- arithmetic helpers ---------------- */

/* Text of a value for `+` concatenation without heap traffic where possible:
 * strings are used in place, ints are formatted into the caller's buffer. */
static const char *concat_piece(KcValue v, char *buf, int *len, char **owned, KcPos pos) {
    *owned = NULL;
    if (v.type == VAL_STRING) {
        if (__builtin_expect(kc_any_freed, 0) && (v.as.s->flags & KC_STR_FREED)) kc_error_freed(v, pos);
        *len = v.as.s->len;
        return v.as.s->data;
    }
    if (v.type == VAL_INT) {
        char tmp[24]; int n = 0; uint64_t u = v.as.i < 0 ? (uint64_t)0 - (uint64_t)v.as.i : (uint64_t)v.as.i;
        do { tmp[n++] = (char)('0' + u % 10); u /= 10; } while (u);
        int k = 0; if (v.as.i < 0) buf[k++] = '-';
        while (n) buf[k++] = tmp[--n];
        buf[k] = 0; *len = k; return buf;
    }
    *owned = kc_to_display_string(v);
    *len = (int)strlen(*owned);
    return *owned;
}

static KcValue str_concat(KcValue a, KcValue b, KcPos pos) {
    char ba[24], bb[24], *oa, *ob; int la, lb;
    const char *pa = concat_piece(a, ba, &la, &oa, pos);
    const char *pb = concat_piece(b, bb, &lb, &ob, pos);
    char *dst;
    KcValue r = kc_string_alloc(la + lb, &dst);
    memcpy(dst, pa, (size_t)la); memcpy(dst + la, pb, (size_t)lb);
    free(oa); free(ob);
    return r;
}

static bool is_numeric(KcValue v) { return v.type == VAL_INT || v.type == VAL_FLOAT; }
static double as_f(KcValue v) { return v.type == VAL_INT ? (double)v.as.i : v.as.f; }

static KcValue eval_binop(KcInterp *ip, KcOp op, KcValue a, KcValue b, KcPos pos) {
    if ((a.type == VAL_PTR || b.type == VAL_PTR) && op != OP_EQ && op != OP_NEQ)
        return kc_ptr_binop(ip, op, a, b, pos);
    switch (op) {
        case OP_ADD:
            if (a.type == VAL_STRING || b.type == VAL_STRING) return str_concat(a, b, pos);
            if (a.type == VAL_INT && b.type == VAL_INT) return kc_int(a.as.i + b.as.i);
            if (is_numeric(a) && is_numeric(b)) return kc_float(as_f(a) + as_f(b));
            break;
        case OP_SUB:
            if (a.type == VAL_INT && b.type == VAL_INT) return kc_int(a.as.i - b.as.i);
            if (is_numeric(a) && is_numeric(b)) return kc_float(as_f(a) - as_f(b));
            break;
        case OP_MUL:
            if (a.type == VAL_INT && b.type == VAL_INT) return kc_int(a.as.i * b.as.i);
            if (is_numeric(a) && is_numeric(b)) return kc_float(as_f(a) * as_f(b));
            break;
        case OP_DIV:
            if (is_numeric(a) && is_numeric(b)) {
                if (as_f(b) == 0.0) kc_error_at(pos, "division by zero");
                if (a.type == VAL_INT && b.type == VAL_INT && b.as.i == -1) return kc_int((int64_t)(0 - (uint64_t)a.as.i));   /* INT64_MIN / -1 would trap */
                if (a.type == VAL_INT && b.type == VAL_INT && a.as.i % b.as.i == 0) return kc_int(a.as.i / b.as.i);
                return kc_float(as_f(a) / as_f(b));
            }
            break;
        case OP_MOD:
            if (a.type == VAL_INT && b.type == VAL_INT) {
                if (b.as.i == 0) kc_error_at(pos, "modulo by zero");
                if (b.as.i == -1) return kc_int(0);                                             /* INT64_MIN % -1 would trap */
                return kc_int(a.as.i % b.as.i);
            }
            if (is_numeric(a) && is_numeric(b)) return kc_float(fmod(as_f(a), as_f(b)));
            break;
        case OP_BAND: case OP_BOR: case OP_BXOR:
            if (a.type == VAL_INT && b.type == VAL_INT)
                return kc_int(op == OP_BAND ? (a.as.i & b.as.i) : op == OP_BOR ? (a.as.i | b.as.i) : (a.as.i ^ b.as.i));
            break;
        case OP_SHL: case OP_SHR: case OP_USHR:
            if (a.type == VAL_INT && b.type == VAL_INT) {
                if (b.as.i < 0 || b.as.i > 63) kc_error_at(pos, "shift count %lld is out of range (0..63)", (long long)b.as.i);
                if (op == OP_SHL) return kc_int((int64_t)((uint64_t)a.as.i << b.as.i));
                if (op == OP_USHR) return kc_int((int64_t)((uint64_t)a.as.i >> b.as.i));
                return kc_int(a.as.i >> b.as.i);
            }
            break;
        case OP_IN: return kc_bool(kc_value_in(ip, a, b, pos));
        case OP_EQ: return kc_bool(kc_equal(a, b));
        case OP_NEQ: return kc_bool(!kc_equal(a, b));
        case OP_LT: if (is_numeric(a) && is_numeric(b)) return kc_bool(as_f(a) < as_f(b)); break;
        case OP_LE: if (is_numeric(a) && is_numeric(b)) return kc_bool(as_f(a) <= as_f(b)); break;
        case OP_GT: if (is_numeric(a) && is_numeric(b)) return kc_bool(as_f(a) > as_f(b)); break;
        case OP_GE: if (is_numeric(a) && is_numeric(b)) return kc_bool(as_f(a) >= as_f(b)); break;
        default: break;
    }
    kc_error_at(pos, "type error: cannot apply operator to %s and %s", kc_type_name(a), kc_type_name(b));
    return kc_null();
}

/* ---------------- calling ---------------- */

/* The 4000-frame limit alone is not enough: a tree-walking evaluator uses
 * well over 2 KB of C stack per KingCobra call, so runaway recursion hit the
 * real stack limit (SIGSEGV) first. On POSIX also compare against how much
 * C stack is actually in use, so deep recursion is a catchable error. */
/* Under AddressSanitizer locals live on a relocated "fake stack", so their addresses say nothing about
 * real stack depth; the frame-count limit alone applies there. */
#if defined(__SANITIZE_ADDRESS__)
#define KC_NO_STACK_GUARD 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define KC_NO_STACK_GUARD 1
#endif
#endif
#if !defined(_WIN32) && !defined(KC_NO_STACK_GUARD)
#include <sys/resource.h>
static char *g_stack_top = NULL;
static size_t g_stack_budget = 0;
static void stack_guard_init(void) {
    char marker;
    if (g_stack_top) return;
    size_t lim = 8u << 20;
    struct rlimit rl;
    if (getrlimit(RLIMIT_STACK, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY) lim = (size_t)rl.rlim_cur;
    g_stack_top = &marker;
    g_stack_budget = lim - lim / 4;      /* keep 25% headroom for natives and error paths */
    kc_install_stack_overflow_handler(&marker, lim);   /* machine-code (JIT) recursion: overflow becomes an error */
}
static inline bool stack_exhausted(void) {
    char here;
    if (!g_stack_top || &here >= g_stack_top) return false;   /* not deeper than the reference point: nothing used */
    return (size_t)(g_stack_top - &here) > g_stack_budget;
}
#else
static void stack_guard_init(void) {}
static inline bool stack_exhausted(void) { return false; }
#endif

/* A call frame's parent scope: the closure's environment, or - for a function that declares `static`
 * locals - a per-closure scope that holds them, so they outlive the call (C `static`). */
static inline KcEnv *frame_parent(KcClosure *cl) {
    if (__builtin_expect(cl->fnnode->flags & NF_HAS_STATIC, 0)) {
        if (!cl->statics) { cl->statics = kc_env_new(cl->env); cl->statics->is_statics = true; kc_env_capture(cl->statics); }
        return cl->statics;
    }
    return cl->env;
}

/* Argument binding for everything that is not "exactly the declared parameters": defaults,
 * `...rest`, and arity errors. Kept out of line so kc_call_value's hot path stays small. */
static __attribute__((noinline)) KcEnv *bind_args_general(KcInterp *ip, KcClosure *cl, KcValue *args, int nargs, KcPos pos) {
    KcNode *fnn = cl->fnnode;
    int nparams = fnn->as.fn.params.count;
    KcEnv *fenv;
    bool variadic = fnn->as.fn.variadic;
    int fixed = variadic ? nparams - 1 : nparams;             /* parameters filled one-to-one */
    if (__builtin_expect(nargs < fnn->as.fn.min_args || (!variadic && nargs > nparams), 0)) {
        const char *nm = cl->name ? cl->name : "<closure>";
        if (variadic) kc_error_at(pos, "'%s' expects at least %d argument(s), got %d", nm, fnn->as.fn.min_args, nargs);
        else if (fnn->as.fn.min_args != nparams) kc_error_at(pos, "'%s' expects %d to %d argument(s), got %d", nm, fnn->as.fn.min_args, nparams, nargs);
        else kc_error_at(pos, "'%s' expects %d argument(s), got %d", nm, nparams, nargs);
    }
    fenv = kc_env_new(frame_parent(cl));
    KcNode **defs = FN_DEFAULTS(fnn);
    for (int i = 0; i < fixed; i++) {
        KcValue av;
        if (i < nargs) av = args[i];
        else av = eval_expr(ip, fenv, defs[i]);             /* default: evaluated now, in the callee's scope */
        kc_env_define_i(fenv, fnn->as.fn.params.items[i]->as.ident.name, av, true);
    }
    if (variadic) {                                           /* ...rest collects the remaining arguments */
        KcValue rest = kc_array_new();
        for (int i = fixed; i < nargs; i++) kc_array_push(rest, args[i]);
        kc_env_define_i(fenv, fnn->as.fn.params.items[fixed]->as.ident.name, rest, true);
    }
    return fenv;
}

KcValue kc_call_value(KcInterp *ip, KcValue callee, KcValue *args, int nargs, KcPos pos) {
    if (ip->call_depth > 4000 || stack_exhausted()) kc_error_at(pos, "stack overflow (recursion too deep)");
    if (callee.type == VAL_NATIVE) {
        if (__builtin_expect(kc_any_freed, 0))              /* no natives on tombstones (free/is_freed exempt) */
            for (int i = 0; i < nargs; i++)
                if (kc_value_freed(args[i])) { kc_check_native_args(callee.as.native, args, nargs, pos); break; }
        return callee.as.native(ip, args, nargs, pos);
    }
    if (callee.type == VAL_EXTERN) {
        kc_forbid_in_comptime(ip, pos, callee.as.ext->name);
        if (nargs) kc_check_live_args(args, nargs, pos);
        kc_ffi_prepare_args(ip, args, nargs, pos);
        KcValue r = kc_ffi_call(callee.as.ext, args, nargs, pos);
        if (callee.as.ext->ptr_ret[0] && r.type == VAL_INT) return kc_ptr_from_address(ip, (uint64_t)r.as.i, callee.as.ext->ptr_ret, pos);
        return r;
    }
    if (callee.type != VAL_CLOSURE) {
        kc_error_at(pos, "value of type '%s' is not callable", kc_type_name(callee));
    }
    KcClosure *cl = callee.as.closure;

    cl->call_count++;
    if (!cl->jit_tried && cl->call_count >= KC_JIT_THRESHOLD && !kc_dbg_on) {   /* the debugger needs every statement to run in the interpreter */
        if (cl->fnnode->as.fn.variadic) cl->jit_tried = 1;      /* `...rest` is an array: never an int-only JIT shape */
        else kc_jit_try_compile(cl);
    }
    if (cl->jit_code) {
        bool all_int = true;
        int64_t iargs[4] = {0};
        int np = cl->fnnode->as.fn.params.count;
        if (nargs == np && np <= 4) {
            for (int i = 0; i < nargs; i++) { if (args[i].type != VAL_INT) { all_int = false; break; } iargs[i] = args[i].as.i; }
            if (all_int) return kc_int(kc_jit_invoke(cl, iargs, nargs));
        }
    }

    KcNode *fnn = cl->fnnode;
    if (!(fnn->flags & NF_INTERNED)) {
        for (int i = 0; i < fnn->as.fn.params.count; i++) node_name(fnn->as.fn.params.items[i]);
        fnn->flags |= NF_INTERNED;
    }
    int nparams = fnn->as.fn.params.count;
    KcEnv *fenv;
    if (__builtin_expect(nargs == nparams && !fnn->as.fn.variadic, 1)) {
        /* the overwhelmingly common case: exactly the declared arguments. No defaults, no rest
         * array, nothing to validate: bind them in one tight loop. */
        fenv = kc_env_new(frame_parent(cl));
        KcNode **ps = fnn->as.fn.params.items;
        for (int i = 0; i < nparams; i++) kc_env_define_i(fenv, ps[i]->as.ident.name, args[i], true);
        goto bound;
    }
    fenv = bind_args_general(ip, cl, args, nargs, pos);
  bound:
    KcValue ret = kc_null();
    ip->call_depth++;
    if (__builtin_expect(kc_dbg_on, 0)) kc_dbg_enter(ip, fenv, cl, pos);
    Signal s = exec_body(ip, fenv, fnn->as.fn.body, &ret);
    ip->call_depth--;
    if (__builtin_expect(s == SIG_GOTO, 0)) { const char *lb = ip->sig_label; ip->sig_label = NULL; kc_error_at(pos, "goto: no label '%s' reachable in this function (a label must be in the same or an enclosing block)", lb ? lb : "?"); }
    if (!fenv->captured) kc_env_release(fenv); /* no closure/&ptr kept it: recycle the frame */
    return ret;
}

/* ---------------- places (pointer-aware lvalues) ---------------- */

enum { PL_VALUE, PL_PLACE, PL_DECAYED };

static bool placeish(KcNode *n) {
    return n->kind == N_DEREF || n->kind == N_ARROW || n->kind == N_INDEX || n->kind == N_MEMBER;
}

static KcValue index_value(KcInterp *ip, KcValue obj, KcValue idx, KcPos pos) {
    (void)ip;
    if (kc_value_freed(obj)) kc_error_freed(obj, pos);
    if (obj.type == VAL_ARRAY) {
        if (idx.type != VAL_INT) kc_error_at(pos, "array index must be int");
        if (idx.as.i < 0 || idx.as.i >= obj.as.arr->count)
            array_oob(obj.as.arr, idx.as.i, pos);
        return obj.as.arr->items[idx.as.i];
    }
    if (obj.type == VAL_STRING) {
        if (idx.as.i < 0 || idx.as.i >= obj.as.s->len) kc_error_at(pos, "string index out of bounds");
        char buf[2] = { obj.as.s->data[idx.as.i], 0 };
        return kc_string(buf);
    }
    if (obj.type == VAL_STRUCT && idx.type == VAL_STRING) {
        /* o["key"]: field read. Missing key -> null on an open object
           (std.json / std.csv), a clear error on a declared struct. */
        KcValue *slot = kc_object_get(obj, idx.as.s->data, (size_t)idx.as.s->len);
        if (slot) return *slot;
        if (kc_is_object(obj)) return kc_null();
        kc_error_at(pos, "struct '%s' has no field '%s'", obj.as.strct->type_name, idx.as.s->data);
    }
    kc_error_at(pos, "cannot index a %s", kc_type_name(obj));
    return kc_null();
}

static __attribute__((noinline)) int index_dispatch(KcInterp *ip, KcValue o, KcValue i, KcPos pos, bool want_ref, KcValue *res) {
    if (o.type == VAL_PTR) { *res = kc_ptr_index(ip, o, i, pos); return PL_PLACE; }
    if (o.type == VAL_BUFFER) { *res = kc_buffer_elem(ip, o, i, pos); return PL_PLACE; }
    if (want_ref && o.type == VAL_ARRAY) {
        if (i.type != VAL_INT) kc_error_at(pos, "array index must be int");
        if (i.as.i < 0 || i.as.i >= o.as.arr->count)
            array_oob(o.as.arr, i.as.i, pos);
        *res = kc_ptr_elem_ref(o.as.arr, i.as.i); return PL_PLACE;
    }
    *res = index_value(ip, o, i, pos); return PL_VALUE;
}

/* Evaluates `n` as a place when it denotes memory reachable through a
 * pointer. Returns PL_PLACE with *res = a pointer TO the thing, PL_DECAYED
 * with *res = a pointer to the first element of an array field, or
 * PL_VALUE with *res = the plain value. `want_ref` additionally turns
 * variables, array elements and struct fields into reference pointers
 * (used by `&`). Every sub-expression is evaluated exactly once. */
static KcValue eval_expr(KcInterp *ip, KcEnv *env, KcNode *n);

/* Leaf fast path: variables and int literals are resolved at the call site
 * without a recursive eval_expr call. Everything else falls through. */
static inline __attribute__((always_inline)) KcValue eval_fast(KcInterp *ip, KcEnv *env, KcNode *n) {
    if (n->kind == N_IDENT) {
        KcEnvEntry *e = kc_env_lookup_i(env, node_name(n));
        if (__builtin_expect(e != NULL, 1)) return e->value;
    } else if (n->kind == N_INT) {
        return kc_int(n->ival);
    }
    return eval_expr(ip, env, n);
}

static int eval_place(KcInterp *ip, KcEnv *env, KcNode *n, bool want_ref, KcValue *res) {
    switch (n->kind) {
        case N_DEREF: {
            KcValue p = eval_expr(ip, env, n->as.unop.rhs);
            if (p.type == VAL_NULL) kc_error_at(n->pos, "null pointer dereference");
            if (p.type != VAL_PTR) kc_error_at(n->pos, "cannot dereference a %s (expected a pointer)", kc_type_name(p));
            *res = p; return PL_PLACE;
        }
        case N_ARROW: {
            KcValue base = eval_expr(ip, env, n->as.member.obj);
            bool arr;
            *res = kc_ptr_member(ip, base, n->as.member.name, n->pos, &arr);
            return arr ? PL_DECAYED : PL_PLACE;
        }
        case N_INDEX: {
            KcValue o = eval_expr(ip, env, n->as.index.obj);
            KcValue i = eval_expr(ip, env, n->as.index.index);
            return index_dispatch(ip, o, i, n->pos, want_ref, res);
        }
        case N_MEMBER: {
            KcNode *on = n->as.member.obj;
            KcValue b; int pk;
            if (placeish(on)) pk = eval_place(ip, env, on, want_ref, &b);
            else { b = eval_expr(ip, env, on); pk = PL_VALUE; }
            if (pk == PL_PLACE && kc_ptr_is_memory_struct(b)) {
                bool arr;
                *res = kc_ptr_member(ip, b, n->as.member.name, n->pos, &arr);
                return arr ? PL_DECAYED : PL_PLACE;
            }
            if (pk == PL_PLACE) b = kc_ptr_load(ip, b, n->pos);
            if (want_ref && b.type == VAL_STRUCT) { *res = kc_ptr_field_ref(b.as.strct, n->as.member.name, n->pos); return PL_PLACE; }
            *res = member_access(ip, b, n->as.member.name, n->pos);
            return PL_VALUE;
        }
        case N_IDENT:
            if (want_ref) {
                KcEnv *e; int idx;
                if (!kc_env_find(env, n->as.ident.name, &e, &idx)) kc_error_at(n->pos, "undefined variable '%s'%s", n->as.ident.name, kc_hint_for_name(env, n->as.ident.name));
                *res = kc_ptr_slot(e, idx, !e->entries[idx].is_mutable);
                return PL_PLACE;
            }
            /* fallthrough */
        default:
            *res = eval_expr(ip, env, n);
            return PL_VALUE;
    }
}

/* `*p op= v` / `p->f op= v` / `p[i] op= v`: resolve once, read-modify-write */
static KcValue assign_through(KcInterp *ip, KcValue pp, int op, KcValue v, KcPos pos) {
    if (op != -1) v = eval_binop(ip, (KcOp)op, kc_ptr_load(ip, pp, pos), v, pos);
    kc_ptr_store(ip, pp, v, pos);
    return v;
}

/* ---------------- expression evaluation ---------------- */

static __attribute__((noinline)) KcValue eval_assign(KcInterp *ip, KcEnv *env, KcNode *n) {
            KcValue v = eval_fast(ip, env, n->as.assign.value);
            int aop = n->as.assign.op;
            KcNode *t = n->as.assign.target;
            if (t->kind == N_DEREF) {
                KcValue p; eval_place(ip, env, t, false, &p);
                return assign_through(ip, p, aop, v, n->pos);
            }
            if (t->kind == N_ARROW) {
                KcValue fp; bool arr;
                KcValue base = eval_fast(ip, env, t->as.member.obj);
                KcPtr tmp;
                if (kc_ptr_member_into(ip, base, t->as.member.name, t->pos, &tmp, &arr) && !arr) {   /* hot path: p->f (op)= v */
                    KcValue tv; tv.type = VAL_PTR; tv.as.ptr = &tmp;
                    return assign_through(ip, tv, aop, v, n->pos);
                }
                fp = kc_ptr_member(ip, base, t->as.member.name, t->pos, &arr);
                if (arr) kc_error_at(n->pos, "cannot assign to an array field as a whole (assign its elements, or use memcpy)");
                return assign_through(ip, fp, aop, v, n->pos);
            }
            if (t->kind == N_IDENT) {
                KcEnvEntry *e = kc_env_lookup_i(env, node_name(t));
                if (aop != -1) {
                    if (!e) kc_error_at(t->pos, "undefined variable '%s'%s", t->as.ident.name, kc_hint_for_name(env, t->as.ident.name));
                    KcValue cur = e->value;
                    if (cur.type == VAL_INT && v.type == VAL_INT && (aop == OP_ADD || aop == OP_SUB))
                        v = kc_int((int64_t)(aop == OP_ADD ? (uint64_t)cur.as.i + (uint64_t)v.as.i : (uint64_t)cur.as.i - (uint64_t)v.as.i));
                    else v = eval_binop(ip, (KcOp)aop, cur, v, n->pos);
                }
                if (!e) kc_error_at(n->pos, "assignment to undeclared variable '%s'", t->as.ident.name);
                if (!e->is_mutable) kc_error_at(n->pos, "cannot assign to '%s': it is immutable (declare it with `var`)", t->as.ident.name);
                e->value = v;
            } else if (t->kind == N_INDEX) {
                KcValue obj = eval_fast(ip, env, t->as.index.obj);
                KcValue idx = eval_fast(ip, env, t->as.index.index);
                if (obj.type == VAL_ARRAY && idx.type == VAL_INT && aop == -1) {   /* hot path: a[i] = v */
                    if ((uint64_t)idx.as.i >= (uint64_t)obj.as.arr->count)
                        array_oob(obj.as.arr, idx.as.i, n->pos);
                    obj.as.arr->items[idx.as.i] = v;
                    return v;
                }
                if (obj.type == VAL_PTR) {                                   /* hot path: p[i] (op)= v */
                    if (aop != -1) v = eval_binop(ip, (KcOp)aop, kc_ptr_load_idx(ip, obj, idx, n->pos), v, n->pos);
                    kc_ptr_store_idx(ip, obj, idx, v, n->pos);
                    return v;
                }
                if (obj.type == VAL_BUFFER) return assign_through(ip, kc_buffer_elem(ip, obj, idx, n->pos), aop, v, n->pos);
                if (aop != -1) v = eval_binop(ip, (KcOp)aop, index_value(ip, obj, idx, n->pos), v, n->pos);
                if (kc_value_freed(obj)) kc_error_freed(obj, n->pos);
                if (obj.type == VAL_STRUCT && idx.type == VAL_STRING) {
                    /* o["key"] = v: replace a field; an open object may also gain new keys */
                    if (!kc_is_object(obj) && !kc_object_get(obj, idx.as.s->data, (size_t)idx.as.s->len))
                        kc_error_at(n->pos, "struct '%s' has no field '%s'", obj.as.strct->type_name, idx.as.s->data);
                    kc_object_set(obj, idx.as.s->data, (size_t)idx.as.s->len, v);
                    return v;
                }
                if (obj.type != VAL_ARRAY || idx.type != VAL_INT)
                    kc_error_at(n->pos, "invalid index assignment");
                if (idx.as.i < 0 || idx.as.i >= obj.as.arr->count)
                    array_oob(obj.as.arr, idx.as.i, n->pos);
                obj.as.arr->items[idx.as.i] = v;
            } else if (t->kind == N_MEMBER) {
                KcValue obj;
                if (placeish(t->as.member.obj)) {
                    int pk = eval_place(ip, env, t->as.member.obj, false, &obj);
                    if (pk == PL_PLACE && kc_ptr_is_memory_struct(obj)) {
                        bool arr;
                        KcValue fp = kc_ptr_member(ip, obj, t->as.member.name, n->pos, &arr);
                        if (arr) kc_error_at(n->pos, "cannot assign to an array field as a whole (assign its elements, or use memcpy)");
                        return assign_through(ip, fp, aop, v, n->pos);
                    }
                    if (pk == PL_PLACE) obj = kc_ptr_load(ip, obj, n->pos);
                } else obj = eval_expr(ip, env, t->as.member.obj);
                if (obj.type != VAL_STRUCT)
                    kc_error_at(n->pos, "cannot assign a field on a %s", kc_type_name(obj));
                if (obj.as.strct->flags & KC_OBJ_FREED) kc_error_freed(obj, n->pos);
                if (!(t->flags & NF_INTERNED)) { t->as.member.name = (char *)kc_intern(t->as.member.name); t->flags |= NF_INTERNED; }
                bool found = false;
                for (int i = 0; i < obj.as.strct->count; i++) {
                    const char *fnm = obj.as.strct->names[i];
                    if (fnm == t->as.member.name || (fnm[0] == t->as.member.name[0] && strcmp(fnm, t->as.member.name) == 0)) {
                        if (aop != -1) v = eval_binop(ip, (KcOp)aop, obj.as.strct->values[i], v, n->pos);
                        obj.as.strct->values[i] = v;
                        found = true;
                        break;
                    }
                }
                if (!found) kc_error_at(n->pos, "struct '%s' has no field '%s'%s", obj.as.strct->type_name, t->as.member.name, kc_hint_for_field(obj.as.strct, t->as.member.name));
            } else {
                kc_error_at(n->pos, "invalid assignment target");
            }
            return v;
}

/* ++x --x x++ x--: the update is an ordinary `x += 1` / `x -= 1` (so pointers step by their element size
 * and every place kind works); the postfix forms yield the value from before the update. */
static __attribute__((noinline)) KcValue eval_incdec(KcInterp *ip, KcEnv *env, KcNode *n) {
    KcNode *t = n->as.assign.target;
    int op = n->as.assign.op;
    bool post = (n->flags & NF_POSTFIX) != 0;
    KcValue old = kc_null(); bool have_old = false;
    if (t->kind == N_IDENT) {
        KcEnvEntry *e = kc_env_lookup_i(env, node_name(t));
        if (e) {
            if (!e->is_mutable) kc_error_at(t->pos, "cannot modify '%s': it is immutable (declare it with `var`)", t->as.ident.name);
            if (e->value.type == VAL_INT) {                                   /* hot path: integer counter */
                int64_t o = e->value.as.i;
                int64_t nw = (int64_t)(op == OP_ADD ? (uint64_t)o + 1u : (uint64_t)o - 1u);
                e->value.as.i = nw;
                return kc_int(post ? o : nw);
            }
            old = e->value; have_old = true;
        }
    }
    KcValue nw = eval_assign(ip, env, n);
    if (!post) return nw;
    if (have_old) return old;
    return eval_binop(ip, op == OP_ADD ? OP_SUB : OP_ADD, nw, kc_int(1), n->pos);   /* undo the step: exact for ints and pointers */
}

/* What a loop does with the signal its body produced. A `break label;` or `continue label;`
 * aimed at an OUTER loop leaves this one and stays pending (ip->sig_label) until the loop that
 * owns the label sees it. Returns 1 to leave the loop (result set), 0 to keep going. */
static inline int loop_ctl(KcInterp *ip, Signal s, const char *label, Signal *out) {
    if (__builtin_expect(s == SIG_NONE, 1)) return 0;
    if (s == SIG_BREAK) {
        if (!ip->sig_label || ip->sig_label == label) { ip->sig_label = NULL; return 1; }
        *out = s; return 1;
    }
    if (s == SIG_CONTINUE) {
        if (!ip->sig_label || ip->sig_label == label) { ip->sig_label = NULL; return 0; }
        *out = s; return 1;
    }
    if (s == SIG_RETURN || s == SIG_GOTO) { *out = s; return 1; }   /* a goto leaves the loop; an enclosing block may own the label */
    return 0;
}

static __attribute__((noinline)) Signal exec_while(KcInterp *ip, KcEnv *env, KcNode *n, KcValue *ret_out) {
    Signal result = SIG_NONE;
    while (kc_truthy(eval_expr(ip, env, n->as.whilestmt.cond))) {
        Signal s = exec_stmt(ip, env, n->as.whilestmt.body, ret_out);
        if (loop_ctl(ip, s, n->label, &result)) break;
    }
    return result;
}

/* for (x in range | array | string | object), and the two-variable forms
 * (i, x in array) / (i, ch in string) / (key, value in object). One scope is reused for every
 * iteration unless a closure or &pointer captured it. */
static __attribute__((noinline)) Signal exec_for(KcInterp *ip, KcEnv *env, KcNode *n, KcValue *ret_out) {
    KcNode *it = n->as.forstmt.iterable, *body = n->as.forstmt.body;
    if (!(n->flags & NF_INTERNED)) {
        n->as.forstmt.var = (char *)kc_intern(n->as.forstmt.var);
        if (n->as.forstmt.var2) n->as.forstmt.var2 = (char *)kc_intern(n->as.forstmt.var2);
        n->flags |= NF_INTERNED;
    }
    const char *var = n->as.forstmt.var, *var2 = n->as.forstmt.var2;
    KcEnv *loopenv = NULL;
    Signal result = SIG_NONE;
    #define RUN_BODY(DEFINE_VARS) do {                                                              \
        if (!loopenv) loopenv = kc_env_new(env); else loopenv->count = 0;                            \
        DEFINE_VARS;                                                                                 \
        Signal s_ = exec_body(ip, loopenv, body, ret_out);                                           \
        if (loopenv->captured) loopenv = NULL;                                                       \
        if (loop_ctl(ip, s_, n->label, &result)) goto done;                                          \
    } while (0)

    if (it->kind == N_RANGE) {
        if (var2) kc_error_at(n->pos, "a range loop takes a single variable: for (i in lo..hi)");
        KcValue lo = eval_expr(ip, env, it->as.range.lo), hi = eval_expr(ip, env, it->as.range.hi);
        if (lo.type != VAL_INT || hi.type != VAL_INT) kc_error_at(n->pos, "range bounds must be ints, got %s..%s", kc_type_name(lo), kc_type_name(hi));
        for (int64_t i = lo.as.i; i < hi.as.i; i++) RUN_BODY(kc_env_define_i(loopenv, var, kc_int(i), true));
    } else {
        KcValue c = eval_expr(ip, env, it);
        if (kc_value_freed(c)) kc_error_freed(c, n->pos);
        switch (c.type) {
            case VAL_ARRAY:
                for (int i = 0; i < c.as.arr->count; i++)           /* count is re-read: the body may push */
                    RUN_BODY({ if (var2) { kc_env_define_i(loopenv, var, kc_int(i), true); kc_env_define_i(loopenv, var2, c.as.arr->items[i], true); }
                               else kc_env_define_i(loopenv, var, c.as.arr->items[i], true); });
                break;
            case VAL_STRING:
                for (int i = 0; i < c.as.s->len; i++) {              /* bytes, like s[i] */
                    char *d; KcValue ch = kc_string_alloc(1, &d); d[0] = c.as.s->data[i];
                    RUN_BODY({ if (var2) { kc_env_define_i(loopenv, var, kc_int(i), true); kc_env_define_i(loopenv, var2, ch, true); }
                               else kc_env_define_i(loopenv, var, ch, true); });
                }
                break;
            case VAL_STRUCT:
                for (int i = 0; i < c.as.strct->count; i++) {
                    KcValue key = kc_string(c.as.strct->names[i]);
                    RUN_BODY({ kc_env_define_i(loopenv, var, key, true);
                               if (var2) kc_env_define_i(loopenv, var2, c.as.strct->values[i], true); });
                }
                break;
            default:
                kc_error_at(n->pos, "for-in needs a range, array, string or object, got type %s", kc_type_name(c));
        }
    }
    #undef RUN_BODY
done:
    if (loopenv) kc_env_release(loopenv);
    return result;
}

static __attribute__((noinline)) KcValue field_or_null(KcValue obj, const char *name, KcPos pos) {
    KcStructInst *st = obj.as.strct;
    for (int i = 0; i < st->count; i++) if (strcmp(st->names[i], name) == 0) return st->values[i];
    if (kc_is_object(obj)) return kc_null();                 /* open objects (json): a missing key is null */
    kc_error_at(pos, "struct '%s' has no field '%s'%s", st->type_name, name, kc_hint_for_field(st, name));
    return kc_null();
}

/* let [a, b, ...rest] = array;     let {x, y: renamed} = object_or_struct; */
static __attribute__((noinline)) void exec_destruct(KcInterp *ip, KcEnv *env, KcNode *n) {
    KcValue v = eval_expr(ip, env, n->as.destruct.init);
    if (kc_value_freed(v)) kc_error_freed(v, n->pos);
    int cnt = n->as.destruct.names.count;
    bool mut = n->as.destruct.is_mutable;
    if (!n->as.destruct.is_obj) {
        if (v.type != VAL_ARRAY) kc_error_at(n->pos, "cannot destructure a value of type %s as an array", kc_type_name(v));
        if (v.as.arr->count < cnt)
            kc_error_at(n->pos, "destructuring needs %d element(s) but the array has %d", cnt, v.as.arr->count);
        for (int i = 0; i < cnt; i++) {
            const char *nm = node_name(n->as.destruct.names.items[i]);
            if (nm[0] == '_' && nm[1] == 0) continue;           /* `_` skips an element */
            kc_env_define_i(env, nm, v.as.arr->items[i], mut);
        }
        if (n->as.destruct.rest) {
            KcValue rest = kc_array_new();
            for (int i = cnt; i < v.as.arr->count; i++) kc_array_push(rest, v.as.arr->items[i]);
            kc_env_define(env, n->as.destruct.rest, rest, mut);
        }
    } else {
        if (v.type != VAL_STRUCT) kc_error_at(n->pos, "cannot destructure a value of type %s as an object or struct", kc_type_name(v));
        for (int i = 0; i < cnt; i++)
            kc_env_define_i(env, node_name(n->as.destruct.names.items[i]), field_or_null(v, n->as.destruct.fields.items[i]->as.str, n->pos), mut);
    }
}

/* enum Color { Red, Green = 5, Blue }  ->  an immutable object: Color.Red == 0, Color.Green == 5, Color.Blue == 6 */
static __attribute__((noinline)) void exec_enum(KcInterp *ip, KcEnv *env, KcNode *n) {
    KcValue obj = kc_object_new();
    int64_t next = 0;
    for (int i = 0; i < n->as.enumdecl.names.count; i++) {
        const char *nm = n->as.enumdecl.names.items[i]->as.ident.name;
        if (n->as.enumdecl.values.items[i]) {
            KcValue v = eval_expr(ip, env, n->as.enumdecl.values.items[i]);
            if (v.type != VAL_INT) kc_error_at(n->pos, "enum '%s': the value of '%s' must be an int, got %s", n->as.enumdecl.name, nm, kc_type_name(v));
            next = v.as.i;
        }
        if (kc_object_get(obj, nm, strlen(nm))) kc_error_at(n->pos, "enum '%s' has '%s' twice", n->as.enumdecl.name, nm);
        kc_object_set(obj, nm, strlen(nm), kc_int(next));
        next++;
    }
    kc_env_define(env, n->as.enumdecl.name, obj, false);
}

static __attribute__((noinline)) void exec_impl(KcInterp *ip, KcEnv *env, KcNode *n) {
    for (int i = 0; i < n->as.impl.methods.count; i++) {
        KcNode *fn = n->as.impl.methods.items[i];
        KcClosure *cl = kc_malloc(sizeof(KcClosure));
        memset(cl, 0, sizeof(KcClosure));
        cl->fnnode = fn; cl->env = env; cl->name = fn->as.fn.name;
        kc_env_capture(env);
        KcValue v; v.type = VAL_CLOSURE; v.as.closure = cl;
        bool instance = fn->as.fn.params.count > 0 && strcmp(fn->as.fn.params.items[0]->as.ident.name, "self") == 0;
        kc_method_add(ip, n->as.impl.type_name, fn->as.fn.name, v, instance);
    }
}

/* Calls a method with `self` in front of the arguments. */
static __attribute__((noinline)) KcValue call_with_self(KcInterp *ip, KcValue fn, KcValue self, KcValue *argv, int nargs, KcPos pos) {
    KcValue buf[9];
    KcValue *a2 = nargs < 8 ? buf : kc_malloc(sizeof(KcValue) * (size_t)(nargs + 1));
    a2[0] = self;
    for (int i = 0; i < nargs; i++) a2[i + 1] = argv[i];
    KcValue r = kc_call_value(ip, fn, a2, nargs + 1, pos);
    if (a2 != buf) free(a2);
    return r;
}

static bool struct_has_field(KcValue v, const char *name) {
    if (v.type != VAL_STRUCT) return false;
    for (int i = 0; i < v.as.strct->count; i++) if (strcmp(v.as.strct->names[i], name) == 0) return true;
    return false;
}

static __attribute__((noinline)) KcValue eval_call(KcInterp *ip, KcEnv *env, KcNode *n) {
            int nargs = n->as.call.args.count;
            KcValue argbuf[8];                       /* no malloc/free per call for the common case */
            KcValue *argv = nargs <= 8 ? argbuf : kc_malloc(sizeof(KcValue) * (size_t)nargs);
            bool heap_args = nargs > 8;
            /* Evaluation order is left to right: for `obj.method(args)` the
               receiver is evaluated BEFORE the arguments (filled in below). */
            #define KC_EVAL_ARGS() do { for (int i_ = 0; i_ < nargs; i_++) argv[i_] = eval_fast(ip, env, n->as.call.args.items[i_]); } while (0)

            /* `obj.method(args)` first tries a built-in string/array method
               (s.split(","), a.push(x), ...) before falling back to normal
               member access — this lets method syntax and module/struct
               access (sys.platform(), p.some_closure_field()) share the
               same `.` without the grammar needing to distinguish them. */
            if (n->as.call.callee->kind == N_MEMBER) {
                KcNode *mem = n->as.call.callee;
                if (ip->nmethods && mem->as.member.obj->kind == N_IDENT && !kc_env_lookup_i(env, node_name(mem->as.member.obj))) {
                    KcValue f; bool inst;           /* Type.make(args): a function from `impl Type { ... }` */
                    if (kc_method_find(ip, mem->as.member.obj->as.ident.name, mem->as.member.name, &f, &inst) && !inst) {
                        KC_EVAL_ARGS();
                        KcValue r = kc_call_value(ip, f, argv, nargs, n->pos);
                        if (heap_args) free(argv);
                        return r;
                    }
                }
                KcValue obj = eval_expr(ip, env, mem->as.member.obj);
                KC_EVAL_ARGS();

                /* `alias.symbol(args)` on a value from `import "lib.so"
                   as alias;` — dlsym the symbol fresh each call (no
                   declared signature to cache against) and dispatch
                   through kc_ffi_call_dynamic. */
                if (obj.type == VAL_NATIVE_LIB) {
                    kc_forbid_in_comptime(ip, n->pos, obj.as.native_lib->label);
                    void *sym = kc_dlsym((kc_dl_handle)obj.as.native_lib->dl_handle, mem->as.member.name);
                    if (!sym) kc_error_at(n->pos, "native module '%s' has no symbol '%s'", obj.as.native_lib->label, mem->as.member.name);
                    if (nargs) kc_check_live_args(argv, nargs, n->pos);
                    kc_ffi_prepare_args(ip, argv, nargs, n->pos);
                    KcValue r = kc_ffi_call_dynamic(sym, argv, nargs, n->pos);
                    if (heap_args) free(argv);
                    return r;
                }

                KcValue out;
                int mid = (int)mem->ival - 1;                 /* method id: resolved once per call site */
                if (__builtin_expect(mid < 0, 0)) { mid = kc_method_id(mem->as.member.name); mem->ival = mid + 1; }
                if (kc_try_builtin_method_id(ip, obj, mem->as.member.name, mid, argv, nargs, n->pos, &out)) {
                    if (heap_args) free(argv);
                    return out;
                }
                if (ip->nmethods && !struct_has_field(obj, mem->as.member.name)) {
                    KcValue f; bool inst;           /* value.method(args): a method from `impl Type { ... }` */
                    if (kc_method_find(ip, kc_value_type_key(obj), mem->as.member.name, &f, &inst) && inst) {
                        KcValue r = call_with_self(ip, f, obj, argv, nargs, n->pos);
                        if (heap_args) free(argv);
                        return r;
                    }
                }
                KcValue callee = member_access(ip, obj, mem->as.member.name, n->pos);
                KcValue r = kc_call_value(ip, callee, argv, nargs, n->pos);
                if (heap_args) free(argv);
                return r;
            }

            KcValue callee = eval_fast(ip, env, n->as.call.callee);
            KC_EVAL_ARGS();
            KcValue r = kc_call_value(ip, callee, argv, nargs, n->pos);
            if (heap_args) free(argv);
            return r;
            #undef KC_EVAL_ARGS
}

static KcValue eval_expr(KcInterp *ip, KcEnv *env, KcNode *n) {
    switch (n->kind) {
        case N_INT: return kc_int(n->ival);
        case N_FLOAT: return kc_float(n->as.num);
        case N_STRING: {
            /* string values are immutable, so a literal is built once and shared */
            if (!n->cache) { KcValue *c = kc_malloc(sizeof(KcValue)); *c = kc_string(n->as.str); c->as.s->flags |= KC_STR_IMMORTAL; kc_live.strings--; /* static data, not program-managed */ n->cache = c; }
            return *(KcValue *)n->cache;
        }
        case N_BOOL: return kc_bool(n->as.boolean);
        case N_NULL: return kc_null();
        case N_IDENT: {
            KcEnvEntry *e = kc_env_lookup_i(env, node_name(n));
            if (__builtin_expect(!e, 0)) kc_error_at(n->pos, "undefined variable '%s'%s", n->as.ident.name, kc_hint_for_name(env, n->as.ident.name));
            return e->value;
        }
        case N_ARRAY: {
            KcValue arr = kc_array_new();
            for (int i = 0; i < n->as.array.items.count; i++)
                kc_array_push(arr, eval_expr(ip, env, n->as.array.items.items[i]));
            return arr;
        }
        case N_UNOP: {
            KcValue r = eval_expr(ip, env, n->as.unop.rhs);
            if (n->as.unop.op == OP_NEG) {
                if (r.type == VAL_INT) return kc_int(-r.as.i);
                if (r.type == VAL_FLOAT) return kc_float(-r.as.f);
                kc_error_at(n->pos, "cannot negate a %s", kc_type_name(r));
            }
            if (n->as.unop.op == OP_NOT) return kc_bool(!kc_truthy(r));
            if (n->as.unop.op == OP_BNOT) {
                if (r.type == VAL_INT) return kc_int(~r.as.i);
                kc_error_at(n->pos, "cannot apply '~' to a %s", kc_type_name(r));
            }
            return kc_null();
        }
        case N_BINOP: {
            if (__builtin_expect((unsigned)(n->as.binop.op - OP_AND) <= (unsigned)(OP_COALESCE - OP_AND), 0)) {
                /* && || ??  : the right side is evaluated only if needed. One range test here replaces
                   three comparisons that every ordinary binary operator would otherwise pay. */
                KcValue l = eval_expr(ip, env, n->as.binop.lhs);
                switch (n->as.binop.op) {
                    case OP_AND: if (!kc_truthy(l)) return kc_bool(false);
                                 return kc_bool(kc_truthy(eval_expr(ip, env, n->as.binop.rhs)));
                    case OP_OR:  if (kc_truthy(l)) return kc_bool(true);
                                 return kc_bool(kc_truthy(eval_expr(ip, env, n->as.binop.rhs)));
                    default:     if (l.type != VAL_NULL) return l;                 /* a ?? b */
                                 return eval_expr(ip, env, n->as.binop.rhs);
                }
            }
            KcValue l = eval_fast(ip, env, n->as.binop.lhs);
            KcValue r = eval_fast(ip, env, n->as.binop.rhs);
            if (l.type == VAL_INT && r.type == VAL_INT) {        /* hot path: int op int */
                int64_t x = l.as.i, y = r.as.i;
                switch (n->as.binop.op) {
                    case OP_ADD: return kc_int((int64_t)((uint64_t)x + (uint64_t)y));
                    case OP_SUB: return kc_int((int64_t)((uint64_t)x - (uint64_t)y));
                    case OP_MUL: return kc_int((int64_t)((uint64_t)x * (uint64_t)y));
                    case OP_LT: return kc_bool(x < y);
                    case OP_LE: return kc_bool(x <= y);
                    case OP_GT: return kc_bool(x > y);
                    case OP_GE: return kc_bool(x >= y);
                    case OP_EQ: return kc_bool(x == y);
                    case OP_NEQ: return kc_bool(x != y);
                    case OP_BAND: return kc_int(x & y);
                    case OP_BOR: return kc_int(x | y);
                    case OP_BXOR: return kc_int(x ^ y);
                    case OP_MOD: if (y > 0 || y < -1) return kc_int(x % y); break;     /* 0 and -1: slow path (error / INT64_MIN edge) */
                    case OP_DIV: if (y > 0 || y < -1) return x % y == 0 ? kc_int(x / y) : kc_float((double)x / (double)y); break;
                    default: break;
                }
            }
            return eval_binop(ip, (KcOp)n->as.binop.op, l, r, n->pos);
        }
        case N_INCDEC: return eval_incdec(ip, env, n);
        case N_COMMA: eval_expr(ip, env, n->as.binop.lhs); return eval_expr(ip, env, n->as.binop.rhs);
        case N_REPEATARR: {
            KcValue cnt = eval_expr(ip, env, n->as.index.index);
            if (cnt.type != VAL_INT || cnt.as.i < 0 || cnt.as.i > (int64_t)1 << 28)
                kc_error_at(n->pos, "[value; count]: count must be an int between 0 and 268435456, got %s", cnt.type == VAL_INT ? "an out-of-range value" : kc_type_name(cnt));
            KcValue arr = kc_array_new();
            for (int64_t i = 0; i < cnt.as.i; i++) kc_array_push(arr, eval_expr(ip, env, n->as.index.obj));   /* evaluated per element: [[0; 3]; 3] makes 3 distinct rows */
            return arr;
        }
        case N_TERNARY:
            return kc_truthy(eval_expr(ip, env, n->as.ternary.cond)) ? eval_expr(ip, env, n->as.ternary.a)
                                                                      : eval_expr(ip, env, n->as.ternary.b);
        case N_MAPLIT: {
            KcValue obj = kc_object_new();
            for (int i = 0; i < n->as.maplit.keys.count; i++) {
                const char *k = n->as.maplit.keys.items[i]->as.str;
                kc_object_set(obj, k, strlen(k), eval_expr(ip, env, n->as.maplit.values.items[i]));
            }
            return obj;
        }
        case N_SLICE: {
            KcValue o = eval_expr(ip, env, n->as.index.obj);
            KcNode *r = n->as.index.index;
            KcValue lo = r->as.range.lo ? eval_expr(ip, env, r->as.range.lo) : kc_null();
            KcValue hi = r->as.range.hi ? eval_expr(ip, env, r->as.range.hi) : kc_null();
            return kc_slice_value(ip, o, lo, hi, n->pos);
        }
        case N_ADDR: {
            KcValue r;
            int k = eval_place(ip, env, n->as.unop.rhs, true, &r);
            if (k == PL_VALUE) kc_error_at(n->pos, "cannot take the address of this expression (use a variable, array element, struct field, or dereference)");
            return r;
        }
        case N_DEREF: {
            KcValue r;
            eval_place(ip, env, n, false, &r);
            return kc_ptr_load(ip, r, n->pos);
        }
        case N_ARROW: {
            KcValue base = eval_fast(ip, env, n->as.member.obj);
            KcPtr tmp; bool arr;
            if (kc_ptr_member_into(ip, base, n->as.member.name, n->pos, &tmp, &arr) && !arr) {   /* hot path: p->f, no allocation */
                KcValue tv; tv.type = VAL_PTR; tv.as.ptr = &tmp;
                return kc_ptr_load(ip, tv, n->pos);
            }
            KcValue r = kc_ptr_member(ip, base, n->as.member.name, n->pos, &arr);
            return arr ? r : kc_ptr_load(ip, r, n->pos);   /* array fields decay to a pointer */
        }
        case N_CAST:
            return kc_value_cast(ip, eval_expr(ip, env, n->as.cast.expr), n->as.cast.type_name, n->pos);
        case N_SIZEOF:
            if (n->as.sizeof_.which == 2) return kc_int(kc_offsetof(ip, n->as.sizeof_.type_name, n->as.sizeof_.field, n->pos));
            return kc_int(kc_sizeof_text(ip, n->as.sizeof_.type_name, n->as.sizeof_.which, n->pos));
        case N_ASSIGN: return eval_assign(ip, env, n);
        case N_CALL: return eval_call(ip, env, n);
        case N_INDEX: {
            KcValue o = eval_fast(ip, env, n->as.index.obj);
            KcValue i = eval_fast(ip, env, n->as.index.index);
            if (o.type == VAL_ARRAY && i.type == VAL_INT) {   /* hot path: a[i] */
                KcArray *arr = o.as.arr;
                if ((uint64_t)i.as.i >= (uint64_t)arr->count)
                    array_oob(arr, i.as.i, n->pos);
                return arr->items[i.as.i];
            }
            if (o.type == VAL_PTR) return kc_ptr_load_idx(ip, o, i, n->pos);   /* hot path: p[i] */
            KcValue r;
            int k = index_dispatch(ip, o, i, n->pos, false, &r);
            return k == PL_PLACE ? kc_ptr_load(ip, r, n->pos) : r;
        }
        case N_MEMBER: {
            if (placeish(n->as.member.obj)) {
                KcValue r;
                int k = eval_place(ip, env, n, false, &r);
                return k == PL_PLACE ? kc_ptr_load(ip, r, n->pos) : r;
            }
            if (!(n->flags & NF_INTERNED)) { n->as.member.name = (char *)kc_intern(n->as.member.name); n->flags |= NF_INTERNED; }
            KcValue obj = eval_fast(ip, env, n->as.member.obj);
            return member_access(ip, obj, n->as.member.name, n->pos);
        }
        case N_STRUCT_LITERAL: {
            const char *tname = n->as.structlit.type_name;
            KcStructTypeDef *def = kc_find_struct_type(ip, tname);
            if (!def) kc_error_at(n->pos, "unknown struct type '%s' (declare it with `struct %s { ... }` first)", tname, tname);
            if (def->is_union) kc_error_at(n->pos, "'%s' is a union: its fields overlap in memory, so it lives in a buffer or alloc block (use `alloc(sizeof(%s)) as *%s`, then p->field)", tname, tname, tname);
            int given = n->as.structlit.field_values.count;
            if (given != def->field_count)
                kc_error_at(n->pos, "struct '%s' has %d field(s), but %d were given", tname, def->field_count, given);
            KcStructInst *inst = kc_struct_alloc();
            inst->type_name = def->name;
            inst->count = given;
            inst->names = kc_malloc(sizeof(char *) * given);
            inst->values = kc_malloc(sizeof(KcValue) * given);
            for (int i = 0; i < given; i++) {
                char *fname = (char *)node_name(n->as.structlit.field_names.items[i]);
                bool is_declared = false;
                for (int j = 0; j < def->field_count; j++) if (strcmp(def->field_names[j], fname) == 0) { is_declared = true; break; }
                if (!is_declared) kc_error_at(n->pos, "struct '%s' has no field '%s'", tname, fname);
                inst->names[i] = fname;
                inst->values[i] = eval_expr(ip, env, n->as.structlit.field_values.items[i]);
            }
            KcValue v; v.type = VAL_STRUCT; v.as.strct = inst;
            return v;
        }
        case N_FN_EXPR: {
            KcClosure *cl = kc_malloc(sizeof(KcClosure));
            memset(cl, 0, sizeof(KcClosure));
            cl->fnnode = n; cl->env = env; cl->name = NULL;
            kc_env_capture(env);
            KcValue v; v.type = VAL_CLOSURE; v.as.closure = cl;
            return v;
        }
        case N_UNSAFE: {
            KcValue ret = kc_null();
            ip->in_unsafe++;
            exec_stmt(ip, env, n->as.unsafe_.body, &ret);
            ip->in_unsafe--;
            return ret;
        }
        case N_RANGE:
            kc_error_at(n->pos, "range expression only valid in a for-loop");
            return kc_null();
        case N_MATCH: {
            KcValue subject = eval_expr(ip, env, n->as.matchexpr.subject);
            for (int i = 0; i < n->as.matchexpr.patterns.count; i++) {
                KcNode *pat = n->as.matchexpr.patterns.items[i];
                bool matched;
                if (!pat) matched = true; /* `_` wildcard */
                else matched = kc_equal(subject, eval_expr(ip, env, pat));
                if (matched) return eval_expr(ip, env, n->as.matchexpr.bodies.items[i]);
            }
            kc_error_at(n->pos, "match: no arm matched (consider adding a '_' wildcard)");
            return kc_null();
        }
        default:
            kc_error_at(n->pos, "cannot evaluate node kind %d as expression", n->kind);
            return kc_null();
    }
}

/* ---------------- statement execution ---------------- */

/* The statement loop proper: no scope or defer logic (callers have decided). */
/* `goto name;` searches the statement lists of the enclosing blocks, innermost first: the block that
 * holds `name:` among its own statements resumes from there. (Jumping INTO a nested block is not
 * supported; that is reported when the goto escapes the function.) */
static int find_label_in(KcNode *block, const char *label) {
    KcNode **it = block->as.block.stmts.items;
    for (int i = 0, n = block->as.block.stmts.count; i < n; i++)
        if (it[i]->kind == N_LABEL && it[i]->label == label) return i;
    return -1;
}

static inline Signal exec_stmts_plain(KcInterp *ip, KcEnv *env, KcNode *block, KcValue *ret_out) {
    KcNode **it = block->as.block.stmts.items;
    for (int i = 0, n = block->as.block.stmts.count; i < n; i++) {
        Signal s = exec_stmt(ip, env, it[i], ret_out);
        if (__builtin_expect(s != SIG_NONE, 0)) {
            if (s == SIG_GOTO) { int j = find_label_in(block, ip->sig_label); if (j >= 0) { ip->sig_label = NULL; i = j - 1; continue; } }
            return s;
        }
    }
    return SIG_NONE;
}

/* Does this block bind names directly in its own scope? Nested blocks and
 * loops have scopes of their own, so only these statements count. If none
 * is present the block can run in its parent's scope with identical
 * semantics and no scope at all. */
/* A block containing `defer` statements: deferred statements run in reverse
 * order when the block exits by any route (end, break, continue, return),
 * after a pending `return` value has already been computed. */
static __attribute__((noinline)) void run_deferred_down_to(KcInterp *ip, int base) {
    while (ip->ndefers > base) {
        KcDeferEntry e = ip->defers[--ip->ndefers];       /* popped first: it may register defers of its own */
        KcValue scratch = kc_null();
        exec_stmt(ip, e.env, e.stmt, &scratch);
    }
}

static Signal exec_stmts_defer(KcInterp *ip, KcEnv *env, KcNode *block, KcValue *ret_out) {
    int base = ip->ndefers;
    Signal s = SIG_NONE;
    KcNode **it = block->as.block.stmts.items;
    for (int i = 0, n = block->as.block.stmts.count; i < n; i++) {
        KcNode *st = it[i];
        if (st->kind == N_DEFER) {
            if (ip->ndefers == ip->defers_cap) {
                ip->defers_cap = ip->defers_cap ? ip->defers_cap * 2 : 64;
                ip->defers = kc_realloc(ip->defers, sizeof(KcDeferEntry) * (size_t)ip->defers_cap);
            }
            ip->defers[ip->ndefers].stmt = st->as.unop.rhs;
            ip->defers[ip->ndefers].env = env;
            ip->ndefers++;
            continue;
        }
        s = exec_stmt(ip, env, st, ret_out);
        if (s == SIG_GOTO) { int j = find_label_in(block, ip->sig_label); if (j >= 0) { ip->sig_label = NULL; s = SIG_NONE; i = j - 1; continue; } }
        if (s != SIG_NONE) break;
    }
    const char *pending_label = ip->sig_label;               /* deferred code may run loops of its own */
    run_deferred_down_to(ip, base);
    ip->sig_label = pending_label;
    return s;
}

static bool block_needs_scope(KcNode *b) {
    if (!(b->flags & NF_SCOPE_KNOWN)) {
        bool need = false, has_defer = false;
        for (int i = 0; i < b->as.block.stmts.count; i++) {
            KcNode *st_ = b->as.block.stmts.items[i];
            while (st_->kind == N_LABEL) st_ = st_->as.unop.rhs;
            switch (st_->kind) {
                case N_LET: case N_FN_DECL: case N_IMPORT: case N_EXTERN_DECL: case N_COMPTIME_BLOCK: case N_DESTRUCT: case N_ENUM_DECL: case N_MULTI: need = true; break;
                case N_DEFER: has_defer = true; break;
                default: break;
            }
        }
        b->flags |= NF_SCOPE_KNOWN | (need ? NF_NEEDS_SCOPE : 0) | (has_defer ? NF_HAS_DEFER : 0);
    }
    return (b->flags & NF_NEEDS_SCOPE) != 0;
}

/* Runs a block's statements in `env` (a frame/loop scope the caller owns). */
static Signal exec_stmts_in(KcInterp *ip, KcEnv *env, KcNode *block, KcValue *ret_out) {
    unsigned char f = block->flags;
    if (__builtin_expect(!(f & NF_SCOPE_KNOWN), 0)) { block_needs_scope(block); f = block->flags; }
    if (__builtin_expect(f & NF_HAS_DEFER, 0)) return exec_stmts_defer(ip, env, block, ret_out);
    return exec_stmts_plain(ip, env, block, ret_out);
}

static Signal exec_block(KcInterp *ip, KcEnv *parent, KcNode *block, KcValue *ret_out) {
    unsigned char f = block->flags;
    if (__builtin_expect(!(f & NF_SCOPE_KNOWN), 0)) { block_needs_scope(block); f = block->flags; }
    if (!(f & NF_NEEDS_SCOPE)) {
        if (__builtin_expect(f & NF_HAS_DEFER, 0)) return exec_stmts_defer(ip, parent, block, ret_out);
        return exec_stmts_plain(ip, parent, block, ret_out);
    }
    KcEnv *env = kc_env_new(parent);
    Signal s = (f & NF_HAS_DEFER) ? exec_stmts_defer(ip, env, block, ret_out) : exec_stmts_plain(ip, env, block, ret_out);
    if (!env->captured) kc_env_release(env);
    return s;
}

/* Keep in step with bind_std_module() below. */
const char *const kc_std_module_names[] = { "system", "memory", "network", "http", "rhex", "music_note", "io", "vector", "regx", "nlp",
    "nn", "gui", "mmio", "acpi", "json", "csv", "fbin", "mmu", "game", "ascii_screen", "tts", NULL };

static void bind_std_module(KcInterp *ip, KcEnv *env, const char *dotted, const char *alias) {
    const char *leaf = strrchr(dotted, '.');
    leaf = leaf ? leaf + 1 : dotted;
    KcModule *m = NULL;
    if (strcmp(leaf, "system") == 0) m = kc_mod_system();
    else if (strcmp(leaf, "memory") == 0) m = kc_mod_memory();
    else if (strcmp(leaf, "network") == 0) m = kc_mod_network();
    else if (strcmp(leaf, "http") == 0) m = kc_mod_http();
    else if (strcmp(leaf, "rhex") == 0) m = kc_mod_rhex();
    else if (strcmp(leaf, "music_note") == 0) m = kc_mod_music_note();
    else if (strcmp(leaf, "io") == 0) m = kc_mod_io();
    else if (strcmp(leaf, "vector") == 0) m = kc_mod_vector();
    else if (strcmp(leaf, "regx") == 0) m = kc_mod_regx();
    else if (strcmp(leaf, "nlp") == 0) m = kc_mod_nlp();
    else if (strcmp(leaf, "nn") == 0) m = kc_mod_nn();
    else if (strcmp(leaf, "gui") == 0) m = kc_mod_gui();
    else if (strcmp(leaf, "mmio") == 0) m = kc_mod_mmio();
    else if (strcmp(leaf, "acpi") == 0) m = kc_mod_acpi();
    else if (strcmp(leaf, "json") == 0) m = kc_mod_json();
    else if (strcmp(leaf, "csv") == 0) m = kc_mod_csv();
    else if (strcmp(leaf, "fbin") == 0) m = kc_mod_fbin();
    else if (strcmp(leaf, "mmu") == 0) m = kc_mod_mmu();
    else if (strcmp(leaf, "game") == 0) m = kc_mod_game();
    else if (strcmp(leaf, "ascii_screen") == 0) m = kc_mod_ascii_screen();
    else if (strcmp(leaf, "tts") == 0) m = kc_mod_tts();
    else kc_error_at((KcPos){0}, "unknown standard library module '%s'", dotted);
    kc_env_define(env, alias ? alias : leaf, mod_value(m), false);
}

/* `import "path/to/file.kc";` (optionally `as alias`) — a real, if
 * deliberately simple, module system: the file's top-level statements
 * run in their own scope (a child of ip->builtins_env, NOT of the
 * importer's globals, so a module can't accidentally see or collide
 * with the importer's own variables), and whatever that scope ends up
 * defining at its own top level (functions, lets) becomes a namespace
 * value the importer accesses as `alias.name` — reusing exactly the
 * struct-field machinery that already backs `obj.field` access, so no
 * new runtime representation was needed. No cycle detection yet (an
 * import cycle will recurse until the C call stack gives up) — see
 * docs/ROADMAP.md. */
/* Resolves an import path the way most languages with file-based
 * modules do: relative to the importing file's own directory first
 * (so `import "mathutils.kc";` works regardless of where the process
 * was launched from), falling back to the path as given (relative to
 * the current working directory, or absolute) if that doesn't exist.
 * Caller frees the result. */
static char *resolve_import_path(const char *importer_file, const char *path) {
    bool is_absolute = path[0] == '/' || path[0] == '\\' || (isalpha((unsigned char)path[0]) && path[1] == ':');
    if (is_absolute || !importer_file) return kc_strdup(path);

    const char *slash = strrchr(importer_file, '/');
    const char *bslash = strrchr(importer_file, '\\');
    const char *sep = slash > bslash ? slash : bslash;
    if (!sep) return kc_strdup(path); /* importer has no directory component */

    size_t dirlen = (size_t)(sep - importer_file) + 1; /* include the separator */
    char *combined = kc_malloc(dirlen + strlen(path) + 1);
    memcpy(combined, importer_file, dirlen);
    strcpy(combined + dirlen, path);

    FILE *probe = fopen(combined, "rb");
    if (probe) { fclose(probe); return combined; }
    free(combined);
    return kc_strdup(path); /* fall back to CWD-relative / absolute as given */
}

/* Derives the default binding name for an import with no explicit
 * `as alias`: the file's base name with its extension stripped, e.g.
 * "modules/mathutils.kc" -> "mathutils". Caller owns the result. */
static char *default_import_name(const char *raw_path) {
    const char *base = strrchr(raw_path, '/');
    if (!base) base = strrchr(raw_path, '\\');
    base = base ? base + 1 : raw_path;
    char *name = kc_strdup(base);
    char *dot = strrchr(name, '.');
    if (dot) *dot = 0;
    return name;
}

static void bind_file_module(KcInterp *ip, KcEnv *env, const char *raw_path, char *alias, KcPos pos) {
    char *path = resolve_import_path(pos.file, raw_path);
    FILE *f = fopen(path, "rb");
    if (!f) kc_error_at(pos, "cannot open imported file '%s' (resolved from '%s')", path, raw_path);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *src = kc_malloc((size_t)size + 1);
    size_t got = fread(src, 1, (size_t)size, f);
    src[got] = 0;
    fclose(f);

    KcNode *prog = kc_parse_program(src, path);
    KcEnv *modenv = kc_env_new(ip->builtins_env);
    KcValue ret = kc_null();
    for (int i = 0; i < prog->as.program.stmts.count; i++)
        exec_stmt(ip, modenv, prog->as.program.stmts.items[i], &ret);

    KcStructInst *inst = kc_struct_alloc();
    inst->type_name = kc_strdup(path);
    inst->count = modenv->count;
    inst->names = kc_malloc(sizeof(char *) * (modenv->count ? modenv->count : 1));
    inst->values = kc_malloc(sizeof(KcValue) * (modenv->count ? modenv->count : 1));
    for (int i = 0; i < modenv->count; i++) {
        inst->names[i] = (char *)modenv->entries[i].name;
        inst->values[i] = modenv->entries[i].value;
    }
    KcValue v; v.type = VAL_STRUCT; v.as.strct = inst;

    kc_env_define(env, alias ? alias : default_import_name(raw_path), v, false);
}

/* `import "lib.so" as alias;` (or `"lib.o"`, linked into a shared
 * library first — see below) — opens a native shared library directly.
 * `alias.symbol(args)` then calls into it dynamically; see
 * kc_ffi_call_dynamic in ffi.c for the calling-convention inference
 * and its documented limits. No declared signatures here at all: this
 * is deliberately the "just call the thing" sibling of the fully
 * declared, more precise `extern` mechanism. */
static void bind_native_module(KcInterp *ip, KcEnv *env, const char *raw_path, char *alias, KcPos pos) {
    kc_forbid_in_comptime(ip, pos, "native import");
    char *resolved = resolve_import_path(pos.file, raw_path);
    const char *ext = strrchr(resolved, '.');
    char *lib_path;

    if (ext && strcmp(ext, ".o") == 0) {
        /* A plain relocatable object file can't be dlopen'd directly on
           most platforms (it's not a shared object) — link it into one
           first, next to the source .o, using whatever C compiler is on
           PATH. Requires the .o to have been compiled with -fPIC. */
        size_t stem_len = (size_t)(ext - resolved);
        lib_path = kc_malloc(stem_len + 32);
        memcpy(lib_path, resolved, stem_len);
#if defined(KC_PLATFORM_WINDOWS)
        strcpy(lib_path + stem_len, ".kcnative.dll");
#else
        strcpy(lib_path + stem_len, ".kcnative.so");
#endif
        char cmd[2048];
#if defined(KC_PLATFORM_WINDOWS)
        snprintf(cmd, sizeof cmd, "gcc -shared -o \"%s\" \"%s\"", lib_path, resolved);
#else
        snprintf(cmd, sizeof cmd, "cc -shared -fPIC -o \"%s\" \"%s\"", lib_path, resolved);
#endif
        if (system(cmd) != 0)
            kc_error_at(pos, "native import: failed to link '%s' into a shared library (ran: %s — is a C compiler on PATH, and was the .o built with -fPIC?)", resolved, cmd);
    } else if (ext && (strcmp(ext, ".so") == 0 || strcmp(ext, ".dll") == 0 || strcmp(ext, ".dylib") == 0)) {
        lib_path = resolved;
    } else {
        kc_error_at(pos, "native import: unsupported extension in '%s' (expected .o, .so, .dll, or .dylib)", raw_path);
        return;
    }

    kc_dl_handle handle = kc_dlopen(lib_path);
    if (!handle) kc_error_at(pos, "native import: could not load '%s': %s", lib_path, kc_dlerror());

    KcNativeLib *lib = kc_malloc(sizeof(KcNativeLib));
    lib->dl_handle = (void *)handle;
    lib->label = lib_path;
    KcValue v; v.type = VAL_NATIVE_LIB; v.as.native_lib = lib;

    kc_env_define(env, alias ? alias : default_import_name(raw_path), v, false);
}

/* try { body } catch (e) { handler }
 * Runs the body under the recoverable-error primitive. If any runtime error or `throw` unwinds
 * out of it: interpreter state is restored to what it was at the `try`, every `defer` registered
 * since then runs (innermost first, so cleanup really happens), and the handler runs with `e`
 * bound to the error: an object {message, kind, file, line, col} for runtime errors, or the
 * thrown value itself for `throw`. */
static __attribute__((noinline)) Signal exec_try(KcInterp *ip, KcEnv *env, KcNode *n, KcValue *ret_out) {
    int saved_depth = ip->call_depth, saved_unsafe = ip->in_unsafe, saved_comptime = ip->in_comptime;
    int saved_defers = ip->ndefers;
    const char *saved_label = ip->sig_label;
    KcErrCatch c;
    c.quiet = true;                                         /* the handler decides what to report */
    ip->has_thrown = false;
    if (KC_TRY(&c) == 0) {
        Signal s = exec_block(ip, env, n->as.trystmt.body, ret_out);
        kc_try_end(&c);
        return s;
    }
    kc_try_end(&c);
    ip->call_depth = saved_depth; ip->in_unsafe = saved_unsafe; ip->in_comptime = saved_comptime;
    ip->sig_label = saved_label;
    KcValue err = ip->has_thrown ? ip->thrown : kc_error_object(c.msg, c.pos, "runtime");
    ip->has_thrown = false;
    run_deferred_down_to(ip, saved_defers);                 /* unwinding: pending cleanup runs now */
    KcEnv *cenv = kc_env_new(env);
    if (n->as.trystmt.catch_var) kc_env_define(cenv, n->as.trystmt.catch_var, err, true);
    Signal s = exec_body(ip, cenv, n->as.trystmt.catch_body, ret_out);
    if (!cenv->captured) kc_env_release(cenv);
    return s;
}

static __attribute__((noinline)) void exec_throw(KcInterp *ip, KcEnv *env, KcNode *n) {
    KcValue v = eval_expr(ip, env, n->as.retstmt.value);
    ip->thrown = v; ip->has_thrown = true; ip->thrown_pos = n->pos;
    char *t = kc_thrown_text(v);
    char msg[420];
    snprintf(msg, sizeof msg, "uncaught exception: %s", t);   /* what is printed if no `catch` is waiting */
    free(t);
    kc_error_at(n->pos, "%s", msg);
}

/* do { } while (cond);  -- `continue` goes to the condition test */
static __attribute__((noinline)) Signal exec_dowhile(KcInterp *ip, KcEnv *env, KcNode *n, KcValue *ret_out) {
    Signal result = SIG_NONE;
    do {
        Signal s = exec_stmt(ip, env, n->as.whilestmt.body, ret_out);
        if (loop_ctl(ip, s, n->label, &result)) break;
    } while (kc_truthy(eval_expr(ip, env, n->as.whilestmt.cond)));
    return result;
}

/* for (init; cond; step) body  -- one scope for the init declarations, shared by every iteration
 * (as in C); `continue` runs the step. */
static __attribute__((noinline)) Signal exec_cfor(KcInterp *ip, KcEnv *env, KcNode *n, KcValue *ret_out) {
    KcEnv *lenv = kc_env_new(env);
    Signal result = SIG_NONE;
    KcNodeList *init = &n->as.cfor.init, *step = &n->as.cfor.step;
    for (int i = 0; i < init->count; i++) exec_stmt(ip, lenv, init->items[i], ret_out);
    for (;;) {
        if (n->as.cfor.cond && !kc_truthy(eval_expr(ip, lenv, n->as.cfor.cond))) break;
        Signal s = exec_stmt(ip, lenv, n->as.cfor.body, ret_out);
        if (loop_ctl(ip, s, n->label, &result)) break;
        for (int i = 0; i < step->count; i++) exec_stmt(ip, lenv, step->items[i], ret_out);
    }
    if (!lenv->captured) kc_env_release(lenv);
    return result;
}

/* switch: the first `case` holding an equal value is entered (else `default`, wherever it stands), and
 * execution FALLS THROUGH later cases until a `break`. All cases share one scope. */
static __attribute__((noinline)) Signal exec_switch(KcInterp *ip, KcEnv *env, KcNode *n, KcValue *ret_out) {
    KcValue subj = eval_expr(ip, env, n->as.switchstmt.subject);
    KcNodeList *cs = &n->as.switchstmt.cases;
    int start = -1, def = -1;
    for (int i = 0; i < cs->count && start < 0; i++) {
        KcNode *c = cs->items[i];
        if (c->as.casenode.is_default) { def = i; continue; }
        for (int j = 0; j < c->as.casenode.values.count; j++)
            if (kc_equal(subj, eval_expr(ip, env, c->as.casenode.values.items[j]))) { start = i; break; }
    }
    if (start < 0) start = def;
    if (start < 0) return SIG_NONE;
    KcEnv *senv = kc_env_new(env);
    Signal result = SIG_NONE;
    for (int i = start; i < cs->count; i++) {
        Signal s = exec_stmts_in(ip, senv, cs->items[i]->as.casenode.body, ret_out);
        if (s == SIG_NONE) continue;
        if (s == SIG_BREAK && !ip->sig_label) break;           /* a plain `break` ends the switch */
        result = s; break;                                     /* continue / return / goto / labeled break travel on */
    }
    if (!senv->captured) kc_env_release(senv);
    return result;
}

/* `static x = init;` -- initialised once, on first execution; lives in the function's statics scope. */
static __attribute__((noinline)) void exec_static_let(KcInterp *ip, KcEnv *env, KcNode *n) {
    if (!(n->flags & NF_INTERNED)) { n->as.let.name = (char *)kc_intern(n->as.let.name); n->flags |= NF_INTERNED; }
    KcEnv *se = NULL;
    for (KcEnv *e = env; e; e = e->parent) if (e->is_statics) { se = e; break; }
    if (!se) {                                                   /* file scope: an ordinary variable */
        KcValue v = n->as.let.init ? eval_expr(ip, env, n->as.let.init) : kc_null();
        kc_env_define_i(env, n->as.let.name, v, true);
        return;
    }
    for (int i = 0; i < se->count; i++) if (se->entries[i].name == n->as.let.name) return;   /* already initialised */
    KcValue v = n->as.let.init ? eval_expr(ip, env, n->as.let.init) : kc_null();
    kc_env_define_i(se, n->as.let.name, v, true);
}

static Signal exec_stmt(KcInterp *ip, KcEnv *env, KcNode *n, KcValue *ret_out) {
    if (__builtin_expect(kc_dbg_on, 0)) kc_dbg_stmt(ip, env, n);
    switch (n->kind) {
        case N_LABEL: return exec_stmt(ip, env, n->as.unop.rhs, ret_out);
        case N_GOTO: ip->sig_label = n->label; return SIG_GOTO;
        case N_MULTI: {
            for (int i = 0; i < n->as.block.stmts.count; i++) {
                Signal s = exec_stmt(ip, env, n->as.block.stmts.items[i], ret_out);
                if (s != SIG_NONE) return s;
            }
            return SIG_NONE;
        }
        case N_EXTRN:
            for (int i = 0; i < n->as.array.items.count; i++) {
                KcNode *id = n->as.array.items.items[i];
                if (!kc_env_lookup_i(env, node_name(id)))
                    kc_error_at(id->pos, "extrn '%s': no such function or variable is defined", id->as.ident.name);
            }
            return SIG_NONE;
        case N_DOWHILE: return exec_dowhile(ip, env, n, ret_out);
        case N_CFOR: return exec_cfor(ip, env, n, ret_out);
        case N_SWITCH: return exec_switch(ip, env, n, ret_out);
        case N_LET: {
            if (__builtin_expect(n->as.let.is_static, 0)) { exec_static_let(ip, env, n); return SIG_NONE; }
            KcValue v = n->as.let.init ? eval_expr(ip, env, n->as.let.init) : kc_null();
            if (!(n->flags & NF_INTERNED)) { n->as.let.name = (char *)kc_intern(n->as.let.name); n->flags |= NF_INTERNED; }
            kc_env_define_i(env, n->as.let.name, v, n->as.let.is_mutable);
            return SIG_NONE;
        }
        case N_EXPRSTMT: {
            KcNode *x = n->as.exprstmt.expr;
            if (x->kind == N_ASSIGN) eval_assign(ip, env, x);
            else if (x->kind == N_CALL) eval_call(ip, env, x);
            else eval_expr(ip, env, x);
            return SIG_NONE;
        }
        case N_BLOCK: return exec_block(ip, env, n, ret_out);
        case N_IF: {
            if (kc_truthy(eval_expr(ip, env, n->as.ifstmt.cond)))
                return exec_stmt(ip, env, n->as.ifstmt.then_branch, ret_out);
            else if (n->as.ifstmt.else_branch)
                return exec_stmt(ip, env, n->as.ifstmt.else_branch, ret_out);
            return SIG_NONE;
        }
        case N_WHILE: return exec_while(ip, env, n, ret_out);
        case N_FOR: return exec_for(ip, env, n, ret_out);
        case N_RETURN:
            *ret_out = n->as.retstmt.value ? eval_expr(ip, env, n->as.retstmt.value) : kc_null();
            return SIG_RETURN;
        case N_BREAK: ip->sig_label = n->label; return SIG_BREAK;
        case N_CONTINUE: ip->sig_label = n->label; return SIG_CONTINUE;
        case N_DESTRUCT: exec_destruct(ip, env, n); return SIG_NONE;
        case N_ENUM_DECL: exec_enum(ip, env, n); return SIG_NONE;
        case N_IMPL: exec_impl(ip, env, n); return SIG_NONE;
        case N_TYPE_ALIAS: return SIG_NONE;
        case N_TRY: return exec_try(ip, env, n, ret_out);
        case N_THROW: exec_throw(ip, env, n); return SIG_NONE;
        case N_FN_DECL: {
            KcClosure *cl = kc_malloc(sizeof(KcClosure));
            memset(cl, 0, sizeof(KcClosure));
            cl->fnnode = n; cl->env = env; cl->name = n->as.fn.name;
            kc_env_capture(env);
            KcValue v; v.type = VAL_CLOSURE; v.as.closure = cl;
            kc_env_define(env, n->as.fn.name, v, false);
            return SIG_NONE;
        }
        case N_DEFER:
            kc_error_at(n->pos, "`defer` is only allowed directly inside a function or block body");
            return SIG_NONE;
        case N_STRUCT_DECL:
            register_struct_type(ip, n->as.structdecl.name, &n->as.structdecl.fields, n->as.structdecl.field_types, n->as.structdecl.packed, n->as.structdecl.is_union, n->pos);
            { KcStructTypeDef *sd_ = kc_find_struct_type(ip, n->as.structdecl.name); if (sd_ && n->as.structdecl.doc) sd_->doc = n->as.structdecl.doc; }
            return SIG_NONE;
        case N_COMPTIME_BLOCK: {
            /* Iterate the body's statements directly against `env`
               (NOT via exec_block, which would create a child scope) so
               `let`/`fn` bindings made inside land in the enclosing
               scope and stay usable afterward — the whole point of a
               comptime block is to define constants you then use. */
            ip->in_comptime++;
            Signal s = SIG_NONE;
            KcNodeList *stmts = &n->as.comptime_block.body->as.block.stmts;
            for (int i = 0; i < stmts->count; i++) {
                s = exec_stmt(ip, env, stmts->items[i], ret_out);
                if (s != SIG_NONE) break;
            }
            ip->in_comptime--;
            return s;
        }
        case N_IMPORT:
            if (n->as.import_.is_file) {
                const char *path = n->as.import_.module;
                const char *ext = strrchr(path, '.');
                if (ext && strcmp(ext, ".kc") == 0)
                    bind_file_module(ip, env, path, n->as.import_.alias, n->pos);
                else
                    bind_native_module(ip, env, path, n->as.import_.alias, n->pos);
            } else {
                bind_std_module(ip, env, n->as.import_.module, n->as.import_.alias);
            }
            return SIG_NONE;
        case N_EXTERN_DECL: {
            KcExternFn *ext = kc_ffi_declare(n, n->pos);
            KcValue v; v.type = VAL_EXTERN; v.as.ext = ext;
            kc_env_define(env, n->as.externdecl.name, v, false);
            return SIG_NONE;
        }
        default:
            eval_expr(ip, env, n);
            return SIG_NONE;
    }
}

/* Calls `fn` with no arguments, containing any error exactly as `try` does: state restored, pending
 * defers run, and the error value (object or thrown value) returned through *err_out. Used by the
 * test runner. Returns true if the call completed normally. */
bool kc_interp_try_call(KcInterp *ip, KcValue fn, KcValue *err_out) {
    int saved_depth = ip->call_depth, saved_unsafe = ip->in_unsafe, saved_comptime = ip->in_comptime;
    int saved_defers = ip->ndefers;
    const char *saved_label = ip->sig_label;
    KcErrCatch c;
    c.quiet = true;
    ip->has_thrown = false;
    if (KC_TRY(&c) == 0) {
        KcPos p = { ip->filename, 0, 0 };
        kc_call_value(ip, fn, NULL, 0, p);
        kc_try_end(&c);
        return true;
    }
    kc_try_end(&c);
    ip->call_depth = saved_depth; ip->in_unsafe = saved_unsafe; ip->in_comptime = saved_comptime;
    ip->sig_label = saved_label;
    *err_out = ip->has_thrown ? ip->thrown : kc_error_object(c.msg, c.pos, "runtime");
    ip->has_thrown = false;
    run_deferred_down_to(ip, saved_defers);
    return false;
}

int kc_script_argc = 0; char **kc_script_argv = NULL;   /* argv[0] is the script; set by main.c */
int kc_main_exit_code = 0;                              /* what main() returned, if an int */

void kc_interp_run(KcInterp *ip, KcNode *program) {
    KcValue ret = kc_null();
    for (int i = 0; i < program->as.program.stmts.count; i++) {
        exec_stmt(ip, ip->globals, program->as.program.stmts.items[i], &ret);
    }
    KcValue mainv;
    if (kc_env_get(ip->globals, "main", &mainv) && mainv.type == VAL_CLOSURE) {
        /* C-style entry point: main(), main(argv) or main(argc, argv); an int result is the exit status. */
        KcValue margs[2]; int nm = 0;
        int np = mainv.as.closure->fnnode->as.fn.params.count;
        if (np >= 1 && np <= 2) {
            KcValue av = kc_array_new();
            for (int i = 0; i < kc_script_argc; i++) kc_array_push(av, kc_string(kc_script_argv[i]));
            if (np == 2) margs[nm++] = kc_int(kc_script_argc);
            margs[nm++] = av;
        }
        KcValue r = kc_call_value(ip, mainv, margs, nm, (KcPos){ ip->filename, 0, 0 });
        if (r.type == VAL_INT) kc_main_exit_code = (int)(r.as.i & 0xFF);
    }
}

/* ---------------- REPL hooks ---------------- */

/* Run one top-level statement against the persistent global scope. Returns
 * true and sets *out when the statement was a value-producing expression
 * (not an assignment), so the REPL can echo it. */
/* debugger: evaluate an expression (or assignment) in a given scope */
KcValue kc_interp_eval_node(KcInterp *ip, KcEnv *env, KcNode *x) {
    if (x->kind == N_ASSIGN) { eval_assign(ip, env, x); return kc_null(); }
    return eval_expr(ip, env, x);
}
bool kc_interp_exec_repl(KcInterp *ip, KcNode *stmt, KcValue *out) {
    if (stmt->kind == N_EXPRSTMT) {
        KcNode *x = stmt->as.exprstmt.expr;
        if (x->kind == N_ASSIGN) { eval_assign(ip, ip->globals, x); return false; }
        *out = eval_expr(ip, ip->globals, x);
        return true;
    }
    KcValue ret = kc_null();
    exec_stmt(ip, ip->globals, stmt, &ret);
    return false;
}

/* Execute a whole program's top-level statements WITHOUT auto-calling main(). */
void kc_interp_run_nomain(KcInterp *ip, KcNode *program) {
    KcValue ret = kc_null();
    for (int i = 0; i < program->as.program.stmts.count; i++)
        exec_stmt(ip, ip->globals, program->as.program.stmts.items[i], &ret);
}

/* After an error unwinds out of the interpreter mid-statement, put the
 * per-run counters back to their idle values. */
void kc_interp_reset_state(KcInterp *ip) {
    ip->call_depth = 0; ip->in_unsafe = 0; ip->in_comptime = 0; ip->sig_label = NULL;
    ip->has_thrown = false;
    /* An unwound REPL line behaves like an error caught by `try`: its pending defers RUN (cleanup such as
       free/close must not be skipped). Each is contained on its own, so one failing defer is reported
       and the rest still run. */
    while (ip->ndefers > 0) {
        KcDeferEntry e = ip->defers[--ip->ndefers];
        KcErrCatch c; c.quiet = false;
        if (KC_TRY(&c) == 0) {
            KcValue scratch = kc_null();
            exec_stmt(ip, e.env, e.stmt, &scratch);
            kc_try_end(&c);
        } else {
            kc_try_end(&c);
            ip->call_depth = 0; ip->in_unsafe = 0; ip->in_comptime = 0; ip->sig_label = NULL; ip->has_thrown = false;
        }
    }
}
