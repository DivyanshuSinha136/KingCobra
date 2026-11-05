/* KingCobra static type checker — see typecheck.h for the documented
 * scope and the deliberate permissiveness policy ("any" = skip rather
 * than guess).
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com>
 */
#include "typecheck.h"
#include "kc_common.h"
#include "interp.h"
#include "lang.h"

/* ---------------------------- type-env scope chain ---------------------------- */

typedef struct TcEnv TcEnv;
struct TcEnv {
    TcEnv *parent;
    char **names;
    const char **types; /* borrowed strings: AST-owned, registry-owned, or static literals */
    int count, cap;
};

static TcEnv *tc_env_new(TcEnv *parent) {
    TcEnv *e = kc_malloc(sizeof(TcEnv));
    e->parent = parent; e->names = NULL; e->types = NULL; e->count = 0; e->cap = 0;
    return e;
}
static void tc_env_define(TcEnv *e, char *name, const char *type) {
    if (e->count + 1 > e->cap) {
        e->cap = e->cap ? e->cap * 2 : 8;
        e->names = kc_realloc(e->names, sizeof(char *) * e->cap);
        e->types = kc_realloc(e->types, sizeof(char *) * e->cap);
    }
    e->names[e->count] = name;
    e->types[e->count] = type;
    e->count++;
}
static const char *tc_env_get(TcEnv *e, const char *name) {
    for (TcEnv *cur = e; cur; cur = cur->parent)
        for (int i = cur->count - 1; i >= 0; i--)
            if (strcmp(cur->names[i], name) == 0) return cur->types[i];
    return "any"; /* unknown identifier at check time: permissive, not an error here
                     (an actually-undefined variable is still caught at runtime) */
}

static bool tc_env_has(TcEnv *e, const char *name) {
    for (TcEnv *cur = e; cur; cur = cur->parent)
        for (int i = cur->count - 1; i >= 0; i--)
            if (strcmp(cur->names[i], name) == 0) return true;
    return false;
}

/* ---------------------------- known global names ---------------------------- */
/* Names that are always resolvable: the interpreter's builtins plus every
 * top-level declaration of the program (collected in the pre-pass, so a
 * function body may refer to a global declared further down the file, the
 * way it can at run time). An identifier found in none of the scope chain,
 * this set, or the builtins is reported as undefined. */
static bool g_repl;                                   /* defined with the registries below */
static char **g_known = NULL;
static int g_known_count = 0, g_known_cap = 0;
static KcEnv *g_builtin_env = NULL;

static void known_add(const char *name) {
    if (!name) return;
    for (int i = 0; i < g_known_count; i++) if (strcmp(g_known[i], name) == 0) return;
    if (g_known_count + 1 > g_known_cap) { g_known_cap = g_known_cap ? g_known_cap * 2 : 32; g_known = kc_realloc(g_known, sizeof(char *) * g_known_cap); }
    g_known[g_known_count++] = kc_strdup(name);
}
static bool known_has(const char *name) {
    if (g_repl && strcmp(name, "_") == 0) return true;      /* the REPL's last-value variable */
    for (int i = 0; i < g_known_count; i++) if (strcmp(g_known[i], name) == 0) return true;
    if (!g_builtin_env) {
        /* a private, never-run interpreter just to learn the builtin names (not kc_interp_new: that
           also installs the process-wide stack guard, which must be anchored by the real one) */
        KcInterp *tmp = kc_malloc(sizeof(KcInterp));
        memset(tmp, 0, sizeof *tmp);
        tmp->builtins_env = kc_env_new(NULL);
        tmp->globals = kc_env_new(tmp->builtins_env);
        kc_register_builtins(tmp);
        g_builtin_env = tmp->builtins_env;
    }
    KcValue v;
    return kc_env_get(g_builtin_env, name, &v);
}
static char *import_default_name(const char *raw_path) {   /* mirrors default_import_name() in interp.c */
    const char *base = strrchr(raw_path, '/');
    if (!base) base = strrchr(raw_path, '\\');
    base = base ? base + 1 : raw_path;
    char *name = kc_strdup(base);
    char *dot = strrchr(name, '.');
    if (dot) *dot = 0;
    return name;
}

/* Reports an undefined name with the same "did you mean" hint the run-time error carries:
 * candidates are offered innermost scope first, then globals, then builtins. */
static void undefined_name(TcEnv *env, KcPos pos, const char *name, const char *what) {
    int cap = 64, n = 0;
    const char **cands = kc_malloc(sizeof(char *) * cap);
    #define ADD(c) do { if (n == cap) { cap *= 2; cands = kc_realloc(cands, sizeof(char *) * cap); } cands[n++] = (c); } while (0)
    for (TcEnv *e = env; e; e = e->parent) for (int i = 0; i < e->count; i++) ADD(e->names[i]);
    for (int i = 0; i < g_known_count; i++) ADD(g_known[i]);
    if (g_builtin_env) for (int i = 0; i < g_builtin_env->count; i++) ADD(g_builtin_env->entries[i].name);
    #undef ADD
    const char *hint = kc_hint_for_module(cands, n, name);
    free(cands);
    kc_error_at(pos, "undefined %s '%s'%s", what, name, hint);
}

/* ---------------------------- function/struct registries ---------------------------- */

typedef struct { char *name; const char **param_types; int param_count; char *ret_type; int min_args; bool variadic; bool self_first; } TcFnSig;
typedef struct { char *name; char **field_names; char **field_types; int count; } TcStructDef;

static TcFnSig *g_fns = NULL;
static int g_fns_count = 0, g_fns_cap = 0;
static TcStructDef *g_structs = NULL;
static int g_structs_count = 0, g_structs_cap = 0;

/* REPL mode: registries persist across inputs and a redeclaration replaces
 * the earlier signature instead of being shadowed by it. */
static bool g_repl = false;

static void register_fn(char *name, KcNodeList *params, char **param_types, char *ret_type, int min_args, bool variadic) {
    TcFnSig *sig = NULL;
    if (g_repl) for (int i = 0; i < g_fns_count; i++) if (strcmp(g_fns[i].name, name) == 0) { sig = &g_fns[i]; break; }
    if (!sig) {
        if (g_fns_count + 1 > g_fns_cap) { g_fns_cap = g_fns_cap ? g_fns_cap * 2 : 8; g_fns = kc_realloc(g_fns, sizeof(TcFnSig) * g_fns_cap); }
        sig = &g_fns[g_fns_count++];
    }
    sig->name = name;
    sig->param_count = params->count;
    sig->param_types = (const char **)param_types;
    sig->ret_type = ret_type;
    sig->min_args = min_args; sig->variadic = variadic; sig->self_first = false;
}
static TcFnSig *find_fn(const char *name) {
    for (int i = 0; i < g_fns_count; i++) if (strcmp(g_fns[i].name, name) == 0) return &g_fns[i];
    return NULL;
}
static void register_struct(char *name, KcNodeList *fields, char **field_types) {
    TcStructDef *def = NULL;
    if (g_repl) for (int i = 0; i < g_structs_count; i++) if (strcmp(g_structs[i].name, name) == 0) { def = &g_structs[i]; break; }
    if (!def) {
        if (g_structs_count + 1 > g_structs_cap) { g_structs_cap = g_structs_cap ? g_structs_cap * 2 : 8; g_structs = kc_realloc(g_structs, sizeof(TcStructDef) * g_structs_cap); }
        def = &g_structs[g_structs_count++];
    }
    def->name = name;
    def->count = fields->count;
    def->field_names = kc_malloc(sizeof(char *) * fields->count);
    for (int i = 0; i < fields->count; i++) def->field_names[i] = fields->items[i]->as.ident.name;
    def->field_types = field_types;
}
static TcStructDef *find_struct(const char *name) {
    for (int i = 0; i < g_structs_count; i++) if (strcmp(g_structs[i].name, name) == 0) return &g_structs[i];
    return NULL;
}
static const char *struct_field_type(TcStructDef *def, const char *field) {
    for (int i = 0; i < def->count; i++) if (strcmp(def->field_names[i], field) == 0) return def->field_types[i];
    return NULL;
}

/* ---------------------------- compatibility rule ---------------------------- */

/* Sized machine types (u8..i64, usize/isize, f32/f64) are just int/float to
 * the checker: ranges are a run-time (truncate-on-store) matter, as in C. */
static const char *tc_norm(const char *t) {
    static const char *INTS[] = { "u8", "i8", "u16", "i16", "u32", "i32", "u64", "i64", "usize", "isize" };
    for (size_t i = 0; i < sizeof INTS / sizeof INTS[0]; i++) if (strcmp(t, INTS[i]) == 0) return "int";
    if (strcmp(t, "f32") == 0 || strcmp(t, "f64") == 0) return "float";
    return t;
}
static bool tc_is_ptr(const char *t) { return t && t[0] == '*'; }
static bool tc_is_fixed_array(const char *t) { size_t n = t ? strlen(t) : 0; return n > 2 && t[n - 1] == ']'; }

static bool tc_compatible(const char *declared, const char *actual) {
    if (!declared || !actual) return true;
    if (strcmp(declared, "any") == 0 || strcmp(actual, "any") == 0) return true;
    if (strcmp(declared, actual) == 0) return true;
    if (tc_is_ptr(declared) || tc_is_ptr(actual)) {
        if (!tc_is_ptr(declared) || !tc_is_ptr(actual)) return false;
        /* both pointers: *void and *any convert freely (C's void*); otherwise pointee types must match */
        return strcmp(declared, "*void") == 0 || strcmp(actual, "*void") == 0 || strcmp(declared, "*any") == 0 || strcmp(actual, "*any") == 0;
    }
    declared = tc_norm(declared); actual = tc_norm(actual);
    if (strcmp(declared, actual) == 0) return true;
    if (strcmp(declared, "float") == 0 && strcmp(actual, "int") == 0) return true; /* int widens to float */
    if (strcmp(actual, "array") == 0 && tc_is_fixed_array(declared)) return true;
    if (strcmp(actual, "array") == 0 && strlen(declared) > 2 && strcmp(declared + strlen(declared) - 2, "[]") == 0) return true;
    return false;
}

/* ---------------------------- expression inference ---------------------------- */

static const char *tc_infer_expr(TcEnv *env, KcNode *n, const char *filename);
static void tc_check_block(TcEnv *env, KcNode *block, const char *ret_type, const char *filename);

static void tc_check_call_args(TcEnv *env, TcFnSig *sig, KcNode *call_node, const char *filename) {
    int given = call_node->as.call.args.count;
    int fixed = sig->variadic ? sig->param_count - 1 : sig->param_count;
    if (given < sig->min_args || (!sig->variadic && given > sig->param_count)) {
        if (sig->variadic) kc_error_at(call_node->pos, "'%s' expects at least %d argument(s), got %d", sig->name, sig->min_args, given);
        if (sig->min_args != sig->param_count) kc_error_at(call_node->pos, "'%s' expects %d to %d argument(s), got %d", sig->name, sig->min_args, sig->param_count, given);
        kc_error_at(call_node->pos, "'%s' expects %d argument(s), got %d", sig->name, sig->param_count, given);
    }
    for (int i = 0; i < given; i++) {
        const char *actual = tc_infer_expr(env, call_node->as.call.args.items[i], filename);
        char elem[96];
        const char *declared;
        if (i < fixed) declared = sig->param_types[i];
        else {                                                 /* an argument absorbed by `...rest: T`: check against T */
            const char *arr = sig->param_types[sig->param_count - 1];
            size_t l = strlen(arr);
            if (l > 2 && strcmp(arr + l - 2, "[]") == 0 && l - 2 < sizeof elem) { memcpy(elem, arr, l - 2); elem[l - 2] = 0; declared = elem; }
            else declared = "any";
        }
        if (!tc_compatible(declared, actual))
            kc_error_at(call_node->as.call.args.items[i]->pos,
                        "'%s' argument %d: expected %s, got %s", sig->name, i + 1, declared, actual);
    }
}

static const char *tc_infer_expr(TcEnv *env, KcNode *n, const char *filename) {
    (void)filename;
    switch (n->kind) {
        case N_INT: return "int";
        case N_FLOAT: return "float";
        case N_STRING: return "string";
        case N_BOOL: return "bool";
        case N_NULL: return "any";
        case N_IDENT: {
            const char *nm = n->as.ident.name;
            if (!tc_env_has(env, nm) && !known_has(nm))
                undefined_name(env, n->pos, nm, "variable");
            return tc_env_get(env, nm);
        }
        case N_INCDEC: {
            KcNode *t = n->as.assign.target;
            if (t->kind == N_IDENT) {
                if (!tc_env_has(env, t->as.ident.name) && !known_has(t->as.ident.name)) undefined_name(env, t->pos, t->as.ident.name, "variable");
                return tc_env_get(env, t->as.ident.name);
            }
            tc_infer_expr(env, t, filename);
            return "any";
        }
        case N_COMMA: tc_infer_expr(env, n->as.binop.lhs, filename); return tc_infer_expr(env, n->as.binop.rhs, filename);
        case N_REPEATARR: tc_infer_expr(env, n->as.index.obj, filename); tc_infer_expr(env, n->as.index.index, filename); return "array";
        case N_ARRAY:
            for (int i = 0; i < n->as.array.items.count; i++) tc_infer_expr(env, n->as.array.items.items[i], filename);
            return "array";
        case N_UNOP: {
            const char *t = tc_norm(tc_infer_expr(env, n->as.unop.rhs, filename));
            if (n->as.unop.op == OP_NOT) return "bool";
            if (n->as.unop.op == OP_BNOT) return "int";
            if (strcmp(t, "int") == 0 || strcmp(t, "float") == 0) return t;
            return "any";
        }
        case N_TERNARY: {
            tc_infer_expr(env, n->as.ternary.cond, filename);
            const char *ta = tc_infer_expr(env, n->as.ternary.a, filename), *tb = tc_infer_expr(env, n->as.ternary.b, filename);
            return strcmp(ta, tb) == 0 ? ta : "any";
        }
        case N_MAPLIT:
            for (int i = 0; i < n->as.maplit.values.count; i++) tc_infer_expr(env, n->as.maplit.values.items[i], filename);
            return "any";
        case N_SLICE: {
            const char *ot = tc_infer_expr(env, n->as.index.obj, filename);
            if (n->as.index.index->as.range.lo) tc_infer_expr(env, n->as.index.index->as.range.lo, filename);
            if (n->as.index.index->as.range.hi) tc_infer_expr(env, n->as.index.index->as.range.hi, filename);
            return (strcmp(ot, "string") == 0 || strcmp(ot, "array") == 0) ? ot : "any";
        }
        case N_ADDR:
            tc_infer_expr(env, n->as.unop.rhs, filename);
            return "any";
        case N_DEREF: {
            const char *t = tc_infer_expr(env, n->as.unop.rhs, filename);
            if (tc_is_ptr(t) && strcmp(t, "*void") != 0 && strcmp(t, "*any") != 0) return t + 1; /* *T -> T */
            return "any";
        }
        case N_ARROW:
            tc_infer_expr(env, n->as.member.obj, filename);
            return "any";
        case N_CAST:
            tc_infer_expr(env, n->as.cast.expr, filename);
            return n->as.cast.type_name;
        case N_SIZEOF: return "int";
        case N_BINOP: {
            const char *lt = tc_infer_expr(env, n->as.binop.lhs, filename);
            const char *rt = tc_infer_expr(env, n->as.binop.rhs, filename);
            if (tc_is_ptr(lt) || tc_is_ptr(rt)) {
                /* pointer arithmetic / comparison: `p + n`, `p - q`, `p < q` */
                switch (n->as.binop.op) {
                    case OP_ADD: return tc_is_ptr(lt) ? lt : rt;
                    case OP_SUB: return (tc_is_ptr(lt) && tc_is_ptr(rt)) ? "int" : lt;
                    case OP_EQ: case OP_NEQ: case OP_LT: case OP_LE: case OP_GT: case OP_GE: return "bool";
                    default: return "any";
                }
            }
            if (n->as.binop.op == OP_IN) return "bool";
            if (n->as.binop.op == OP_COALESCE) return strcmp(lt, rt) == 0 ? lt : "any";
            lt = tc_norm(lt); rt = tc_norm(rt);
            switch (n->as.binop.op) {
                case OP_ADD:
                    if (strcmp(lt, "string") == 0 || strcmp(rt, "string") == 0) return "string";
                    if (strcmp(lt, "any") == 0 || strcmp(rt, "any") == 0) return "any";
                    if (strcmp(lt, "int") == 0 && strcmp(rt, "int") == 0) return "int";
                    if ((strcmp(lt, "int") == 0 || strcmp(lt, "float") == 0) && (strcmp(rt, "int") == 0 || strcmp(rt, "float") == 0)) return "float";
                    kc_error_at(n->pos, "cannot apply '+' to %s and %s", lt, rt);
                    return "any";
                case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD:
                    if (strcmp(lt, "any") == 0 || strcmp(rt, "any") == 0) return "any";
                    if (strcmp(lt, "int") == 0 && strcmp(rt, "int") == 0) return "int";
                    if ((strcmp(lt, "int") == 0 || strcmp(lt, "float") == 0) && (strcmp(rt, "int") == 0 || strcmp(rt, "float") == 0)) return "float";
                    kc_error_at(n->pos, "cannot apply arithmetic operator to %s and %s", lt, rt);
                    return "any";
                case OP_EQ: case OP_NEQ: case OP_LT: case OP_LE: case OP_GT: case OP_GE:
                case OP_AND: case OP_OR:
                    return "bool";
                case OP_BAND: case OP_BOR: case OP_BXOR: case OP_SHL: case OP_SHR: case OP_USHR:
                    return (strcmp(lt, "int") == 0 && strcmp(rt, "int") == 0) ? "int" : "any";
                default: return "any";
            }
        }
        case N_ASSIGN: {
            const char *vt = tc_infer_expr(env, n->as.assign.value, filename);
            KcNode *t = n->as.assign.target;
            if (t->kind == N_IDENT) {
                if (!tc_env_has(env, t->as.ident.name) && !known_has(t->as.ident.name))
                    undefined_name(env, t->pos, t->as.ident.name, "variable");
                const char *declared = tc_env_get(env, t->as.ident.name);
                if (!tc_compatible(declared, vt))
                    kc_error_at(n->pos, "cannot assign %s to '%s' (declared %s)", vt, t->as.ident.name, declared);
            } else if (t->kind == N_MEMBER) {
                const char *obj_t = tc_infer_expr(env, t->as.member.obj, filename);
                TcStructDef *def = find_struct(obj_t);
                if (def) {
                    const char *ft = struct_field_type(def, t->as.member.name);
                    if (ft && !tc_compatible(ft, vt))
                        kc_error_at(n->pos, "cannot assign %s to field '%s.%s' (declared %s)", vt, obj_t, t->as.member.name, ft);
                }
            } else if (t->kind == N_INDEX) {
                tc_infer_expr(env, t->as.index.obj, filename);
                tc_infer_expr(env, t->as.index.index, filename);
            }
            return vt;
        }
        case N_CALL: {
            KcNode *callee = n->as.call.callee;
            if (callee->kind == N_IDENT) {
                TcFnSig *sig = find_fn(callee->as.ident.name);
                if (sig) { tc_check_call_args(env, sig, n, filename); return sig->ret_type; }
            }
            /* module/native/method/closure-in-variable calls: no static
               signature available — still recurse for nested errors. */
            if (callee->kind == N_MEMBER) {
                /* impl methods: `Type.make(args)` and `value.method(args)` get the same arity/type
                   checking as a direct call to a named function, when the receiver's struct type is known */
                KcNode *ro = callee->as.member.obj;
                bool is_type = ro->kind == N_IDENT && !tc_env_has(env, ro->as.ident.name) && find_struct(ro->as.ident.name);
                const char *rt = is_type ? ro->as.ident.name : tc_infer_expr(env, ro, filename);
                TcStructDef *sd = find_struct(rt);
                if (sd && !(!is_type && struct_field_type(sd, callee->as.member.name))) {
                    char key[256];
                    snprintf(key, sizeof key, "%s.%s", rt, callee->as.member.name);
                    TcFnSig *ms = find_fn(key);
                    if (ms) {
                        bool inst = ms->param_count > 0 && ms->self_first;
                        if (inst != is_type) {                 /* instance via value, static via Type */
                            TcFnSig shifted = *ms;
                            if (inst) { shifted.param_count--; shifted.param_types++; shifted.min_args = shifted.min_args > 0 ? shifted.min_args - 1 : 0; }
                            tc_check_call_args(env, &shifted, n, filename);
                            return ms->ret_type;
                        }
                    }
                }
                for (int i = 0; i < n->as.call.args.count; i++) tc_infer_expr(env, n->as.call.args.items[i], filename);
                return "any";
            }
            if (callee->kind == N_IDENT && !tc_env_has(env, callee->as.ident.name) && !known_has(callee->as.ident.name))
                undefined_name(env, callee->pos, callee->as.ident.name, "function");
            tc_infer_expr(env, callee->kind == N_MEMBER ? callee->as.member.obj : callee, filename);
            for (int i = 0; i < n->as.call.args.count; i++) tc_infer_expr(env, n->as.call.args.items[i], filename);
            return "any";
        }
        case N_INDEX:
            tc_infer_expr(env, n->as.index.obj, filename);
            tc_infer_expr(env, n->as.index.index, filename);
            return "any";
        case N_MEMBER: {
            const char *obj_t = tc_infer_expr(env, n->as.member.obj, filename);
            TcStructDef *def = find_struct(obj_t);
            if (def) {
                const char *ft = struct_field_type(def, n->as.member.name);
                if (ft) return ft;
            }
            return "any";
        }
        case N_FN_EXPR: {
            TcEnv *fenv = tc_env_new(env);
            for (int i = 0; i < n->as.fn.params.count; i++)
                tc_env_define(fenv, n->as.fn.params.items[i]->as.ident.name, n->as.fn.param_types[i]);
            /* body checked for internal consistency; a closure value's
               own type is just the opaque `fn` from here */
            tc_check_block(fenv, n->as.fn.body, n->as.fn.ret_type, filename);
            return "fn";
        }
        case N_UNSAFE: {
            tc_check_block(env, n->as.unsafe_.body, "any", filename);
            return "any";
        }
        case N_RANGE:
            tc_infer_expr(env, n->as.range.lo, filename);
            tc_infer_expr(env, n->as.range.hi, filename);
            return "any";
        case N_STRUCT_LITERAL: {
            TcStructDef *def = find_struct(n->as.structlit.type_name);
            for (int i = 0; i < n->as.structlit.field_values.count; i++) {
                const char *vt = tc_infer_expr(env, n->as.structlit.field_values.items[i], filename);
                if (def) {
                    const char *fname = n->as.structlit.field_names.items[i]->as.ident.name;
                    const char *ft = struct_field_type(def, fname);
                    if (ft && !tc_compatible(ft, vt))
                        kc_error_at(n->pos, "struct '%s' field '%s': expected %s, got %s", n->as.structlit.type_name, fname, ft, vt);
                }
            }
            /* unknown/mismatched-arity struct literals are already caught
               at runtime with a clearer message; the checker only adds
               the field-type layer on top when the struct IS known */
            return def ? n->as.structlit.type_name : "any";
        }
        case N_MATCH: {
            tc_infer_expr(env, n->as.matchexpr.subject, filename);
            const char *result = NULL;
            for (int i = 0; i < n->as.matchexpr.patterns.count; i++) {
                if (n->as.matchexpr.patterns.items[i]) tc_infer_expr(env, n->as.matchexpr.patterns.items[i], filename);
                const char *bt = tc_infer_expr(env, n->as.matchexpr.bodies.items[i], filename);
                if (!result) result = bt;
                else if (!tc_compatible(result, bt) && !tc_compatible(bt, result)) result = "any"; /* arms disagree: don't guess */
            }
            return result ? result : "any";
        }
        default:
            return "any";
    }
}

/* ---------------------------- statement checking ---------------------------- */

static void tc_check_stmt(TcEnv *env, KcNode *n, const char *ret_type, const char *filename);

static void tc_check_block(TcEnv *env, KcNode *block, const char *ret_type, const char *filename) {
    TcEnv *inner = tc_env_new(env);
    for (int i = 0; i < block->as.block.stmts.count; i++)
        tc_check_stmt(inner, block->as.block.stmts.items[i], ret_type, filename);
}

static void tc_check_stmt(TcEnv *env, KcNode *n, const char *ret_type, const char *filename) {
    switch (n->kind) {
        case N_LET: {
            const char *declared = n->as.let.type_name;
            const char *actual = n->as.let.init ? tc_infer_expr(env, n->as.let.init, filename) : "any";
            if (declared && n->as.let.init && !tc_compatible(declared, actual))
                kc_error_at(n->pos, "'%s': declared %s, but initializer is %s", n->as.let.name, declared, actual);
            tc_env_define(env, n->as.let.name, declared ? declared : actual);
            return;
        }
        case N_EXPRSTMT: tc_infer_expr(env, n->as.exprstmt.expr, filename); return;
        case N_DEFER: tc_check_stmt(env, n->as.unop.rhs, ret_type, filename); return;
        case N_BLOCK: tc_check_block(env, n, ret_type, filename); return;
        case N_IF:
            tc_infer_expr(env, n->as.ifstmt.cond, filename);
            tc_check_stmt(env, n->as.ifstmt.then_branch, ret_type, filename);
            if (n->as.ifstmt.else_branch) tc_check_stmt(env, n->as.ifstmt.else_branch, ret_type, filename);
            return;
        case N_WHILE:
            tc_infer_expr(env, n->as.whilestmt.cond, filename);
            tc_check_stmt(env, n->as.whilestmt.body, ret_type, filename);
            return;
        case N_FOR: {
            TcEnv *loop_env = tc_env_new(env);
            const char *var_type = "any";
            if (n->as.forstmt.iterable->kind == N_RANGE) {
                tc_infer_expr(env, n->as.forstmt.iterable, filename);
                var_type = "int";
            } else {
                tc_infer_expr(env, n->as.forstmt.iterable, filename);
            }
            tc_env_define(loop_env, n->as.forstmt.var, n->as.forstmt.var2 ? "any" : var_type);
            if (n->as.forstmt.var2) tc_env_define(loop_env, n->as.forstmt.var2, "any");
            tc_check_stmt(loop_env, n->as.forstmt.body, ret_type, filename);
            return;
        }
        case N_RETURN: {
            const char *actual = n->as.retstmt.value ? tc_infer_expr(env, n->as.retstmt.value, filename) : "void";
            if (ret_type && !tc_compatible(ret_type, actual))
                kc_error_at(n->pos, "return type mismatch: function declared %s, returning %s", ret_type, actual);
            return;
        }
        case N_BREAK: case N_CONTINUE: case N_GOTO: return;
        case N_LABEL: tc_check_stmt(env, n->as.unop.rhs, ret_type, filename); return;
        case N_MULTI:
            for (int i = 0; i < n->as.block.stmts.count; i++) tc_check_stmt(env, n->as.block.stmts.items[i], ret_type, filename);
            return;
        case N_EXTRN:
            for (int i = 0; i < n->as.array.items.count; i++) tc_env_define(env, n->as.array.items.items[i]->as.ident.name, "any");
            return;
        case N_DOWHILE:
            tc_check_stmt(env, n->as.whilestmt.body, ret_type, filename);
            tc_infer_expr(env, n->as.whilestmt.cond, filename);
            return;
        case N_CFOR: {
            TcEnv *le = tc_env_new(env);
            for (int i = 0; i < n->as.cfor.init.count; i++) tc_check_stmt(le, n->as.cfor.init.items[i], ret_type, filename);
            if (n->as.cfor.cond) tc_infer_expr(le, n->as.cfor.cond, filename);
            for (int i = 0; i < n->as.cfor.step.count; i++) tc_check_stmt(le, n->as.cfor.step.items[i], ret_type, filename);
            tc_check_stmt(le, n->as.cfor.body, ret_type, filename);
            return;
        }
        case N_SWITCH: {
            tc_infer_expr(env, n->as.switchstmt.subject, filename);
            TcEnv *se = tc_env_new(env);
            for (int c = 0; c < n->as.switchstmt.cases.count; c++) {
                KcNode *cn = n->as.switchstmt.cases.items[c];
                for (int v = 0; v < cn->as.casenode.values.count; v++) tc_infer_expr(se, cn->as.casenode.values.items[v], filename);
                KcNodeList *st = &cn->as.casenode.body->as.block.stmts;
                for (int i = 0; i < st->count; i++) tc_check_stmt(se, st->items[i], ret_type, filename);
            }
            return;
        }
        case N_FN_DECL: {
            /* signature already registered in the pre-pass; check the body */
            tc_env_define(env, n->as.fn.name, "any");           /* a local fn is a name from here on (and inside itself, for recursion) */
            TcEnv *fenv = tc_env_new(env);
            for (int i = 0; i < n->as.fn.params.count; i++) {
                tc_env_define(fenv, n->as.fn.params.items[i]->as.ident.name, n->as.fn.param_types[i]);
                if (FN_DEFAULTS(n) && FN_DEFAULTS(n)[i]) {          /* a default must fit its parameter's type */
                    const char *dt = tc_infer_expr(fenv, FN_DEFAULTS(n)[i], filename);
                    if (!tc_compatible(n->as.fn.param_types[i], dt))
                        kc_error_at(FN_DEFAULTS(n)[i]->pos, "default for '%s': expected %s, got %s",
                                    n->as.fn.params.items[i]->as.ident.name, n->as.fn.param_types[i], dt);
                }
            }
            tc_check_block(fenv, n->as.fn.body, n->as.fn.ret_type, filename);
            return;
        }
        case N_DESTRUCT:
            tc_infer_expr(env, n->as.destruct.init, filename);
            for (int i = 0; i < n->as.destruct.names.count; i++) tc_env_define(env, n->as.destruct.names.items[i]->as.ident.name, "any");
            if (n->as.destruct.rest) tc_env_define(env, n->as.destruct.rest, "array");
            return;
        case N_ENUM_DECL:
            for (int i = 0; i < n->as.enumdecl.values.count; i++)
                if (n->as.enumdecl.values.items[i]) tc_infer_expr(env, n->as.enumdecl.values.items[i], filename);
            tc_env_define(env, n->as.enumdecl.name, "any");
            return;
        case N_IMPL: {
            TcEnv *ienv = tc_env_new(env);               /* method names are not bare names in the enclosing scope */
            for (int m = 0; m < n->as.impl.methods.count; m++) tc_check_stmt(ienv, n->as.impl.methods.items[m], NULL, filename);
            return;
        }
        case N_TYPE_ALIAS: return;               /* resolved by the parser */
        case N_TRY: {
            tc_check_stmt(env, n->as.trystmt.body, ret_type, filename);
            TcEnv *cenv = tc_env_new(env);
            if (n->as.trystmt.catch_var) tc_env_define(cenv, n->as.trystmt.catch_var, "any");
            tc_check_stmt(cenv, n->as.trystmt.catch_body, ret_type, filename);
            return;
        }
        case N_THROW: tc_infer_expr(env, n->as.retstmt.value, filename); return;
        case N_STRUCT_DECL: return; /* already registered in the pre-pass */
        case N_IMPORT: return;      /* imported file contents aren't checked here — see typecheck.h */
        case N_EXTERN_DECL: return; /* already registered in the pre-pass */
        case N_UNSAFE: tc_check_block(env, n->as.unsafe_.body, ret_type, filename); return;
        case N_COMPTIME_BLOCK: {
            /* Iterate directly against `env`, matching the interpreter's
               same-scope semantics for comptime blocks (bindings made
               inside must stay visible afterward). */
            KcNodeList *stmts = &n->as.comptime_block.body->as.block.stmts;
            for (int i = 0; i < stmts->count; i++) tc_check_stmt(env, stmts->items[i], ret_type, filename);
            return;
        }
        default: tc_infer_expr(env, n, filename); return; /* expression used as a statement via other paths */
    }
}

/* ---------------------------- pre-pass + entry point ---------------------------- */

static void collect_names(KcNode *n) {
    switch (n->kind) {
        case N_FN_DECL: known_add(n->as.fn.name); break;
        case N_LET: known_add(n->as.let.name); break;
        case N_DESTRUCT:
            for (int j = 0; j < n->as.destruct.names.count; j++) known_add(n->as.destruct.names.items[j]->as.ident.name);
            known_add(n->as.destruct.rest);
            break;
        case N_ENUM_DECL: known_add(n->as.enumdecl.name); break;
        case N_MULTI: for (int k = 0; k < n->as.block.stmts.count; k++) if (n->as.block.stmts.items[k]->kind == N_LET) known_add(n->as.block.stmts.items[k]->as.let.name); break;
        case N_STRUCT_DECL: known_add(n->as.structdecl.name); break;      /* `Type.make(...)` names the type */
        case N_IMPL: known_add(n->as.impl.type_name); break;
        case N_EXTERN_DECL: known_add(n->as.externdecl.name); break;
        case N_IMPORT: {
            if (n->as.import_.alias) { known_add(n->as.import_.alias); break; }
            if (n->as.import_.is_file) { char *d = import_default_name(n->as.import_.module); known_add(d); free(d); }
            else { const char *dot = strrchr(n->as.import_.module, '.'); known_add(dot ? dot + 1 : n->as.import_.module); }
            break;
        }
        case N_COMPTIME_BLOCK: {                       /* bindings made inside stay visible afterwards */
            KcNodeList *st = &n->as.comptime_block.body->as.block.stmts;
            for (int j = 0; j < st->count; j++) collect_names(st->items[j]);
            break;
        }
        default: break;
    }
}

static void prepass(KcNode *program) {
    for (int i = 0; i < program->as.program.stmts.count; i++) {
        KcNode *n = program->as.program.stmts.items[i];
        collect_names(n);
        if (n->kind == N_FN_DECL) {
            register_fn(n->as.fn.name, &n->as.fn.params, n->as.fn.param_types, n->as.fn.ret_type, n->as.fn.min_args, n->as.fn.variadic);
        } else if (n->kind == N_IMPL) {
            for (int m = 0; m < n->as.impl.methods.count; m++) {
                KcNode *f = n->as.impl.methods.items[m];
                if (f->kind != N_FN_DECL) continue;
                char *key = kc_malloc(strlen(n->as.impl.type_name) + strlen(f->as.fn.name) + 2);
                sprintf(key, "%s.%s", n->as.impl.type_name, f->as.fn.name);
                register_fn(key, &f->as.fn.params, f->as.fn.param_types, f->as.fn.ret_type, f->as.fn.min_args, f->as.fn.variadic);
                find_fn(key)->self_first = f->as.fn.params.count > 0 && strcmp(f->as.fn.params.items[0]->as.ident.name, "self") == 0;
            }
        } else if (n->kind == N_STRUCT_DECL) {
            register_struct(n->as.structdecl.name, &n->as.structdecl.fields, n->as.structdecl.field_types);
        } else if (n->kind == N_EXTERN_DECL) {
            char **ptypes = kc_malloc(sizeof(char *) * (n->as.externdecl.param_types.count ? n->as.externdecl.param_types.count : 1));
            for (int j = 0; j < n->as.externdecl.param_types.count; j++) ptypes[j] = n->as.externdecl.param_types.items[j]->as.ident.name;
            register_fn(n->as.externdecl.name, &n->as.externdecl.param_types, ptypes, n->as.externdecl.ret_type, n->as.externdecl.param_types.count, false);
        }
    }
}

void kc_typecheck_program(KcNode *program, const char *filename) {
    g_fns = NULL; g_fns_count = 0; g_fns_cap = 0;
    g_structs = NULL; g_structs_count = 0; g_structs_cap = 0;
    g_known_count = 0;
    prepass(program);
    TcEnv *top = tc_env_new(NULL);
    for (int i = 0; i < program->as.program.stmts.count; i++)
        tc_check_stmt(top, program->as.program.stmts.items[i], NULL, filename);
}

/* ---------------------------- REPL entry points ---------------------------- */

typedef struct { TcFnSig *fns; int nfns; TcStructDef *structs; int nstructs; int nknown; } TcSnapshot;

void *kc_typecheck_snapshot(void) {
    TcSnapshot *s = kc_malloc(sizeof *s);
    s->nfns = g_fns_count; s->nstructs = g_structs_count; s->nknown = g_known_count;
    s->fns = kc_malloc(sizeof(TcFnSig) * (g_fns_count ? g_fns_count : 1));
    s->structs = kc_malloc(sizeof(TcStructDef) * (g_structs_count ? g_structs_count : 1));
    if (g_fns_count) memcpy(s->fns, g_fns, sizeof(TcFnSig) * g_fns_count);
    if (g_structs_count) memcpy(s->structs, g_structs, sizeof(TcStructDef) * g_structs_count);
    return s;
}
void kc_typecheck_restore(void *snap) {
    TcSnapshot *s = snap;
    if (s->nfns > g_fns_cap) { g_fns_cap = s->nfns; g_fns = kc_realloc(g_fns, sizeof(TcFnSig) * g_fns_cap); }
    if (s->nstructs > g_structs_cap) { g_structs_cap = s->nstructs; g_structs = kc_realloc(g_structs, sizeof(TcStructDef) * g_structs_cap); }
    g_fns_count = s->nfns; g_structs_count = s->nstructs; g_known_count = s->nknown;
    if (s->nfns) memcpy(g_fns, s->fns, sizeof(TcFnSig) * s->nfns);
    if (s->nstructs) memcpy(g_structs, s->structs, sizeof(TcStructDef) * s->nstructs);
    free(s->fns); free(s->structs); free(s);
}
void kc_typecheck_discard(void *snap) { TcSnapshot *s = snap; free(s->fns); free(s->structs); free(s); }
void kc_typecheck_reset(void) { g_fns_count = 0; g_structs_count = 0; g_known_count = 0; }

/* Check one REPL input against everything declared so far in the session. */
void kc_typecheck_repl(KcNode *program, const char *filename) {
    g_repl = true;
    prepass(program);
    TcEnv *top = tc_env_new(NULL);
    for (int i = 0; i < program->as.program.stmts.count; i++)
        tc_check_stmt(top, program->as.program.stmts.items[i], NULL, filename);
}
