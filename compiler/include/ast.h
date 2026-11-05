#ifndef KC_AST_H
#define KC_AST_H
#include "kc_common.h"

typedef enum {
    /* expressions */
    N_INT, N_FLOAT, N_STRING, N_BOOL, N_NULL, N_IDENT,
    N_BINOP, N_UNOP, N_ASSIGN, N_CALL, N_INDEX, N_MEMBER,
    N_ARRAY, N_FN_EXPR, N_UNSAFE, N_RANGE, N_STRUCT_LITERAL, N_MATCH,
    /* pointer / low-level expressions (v0.5) */
    N_ADDR, N_DEREF, N_ARROW, N_CAST, N_SIZEOF,
    N_DEFER,   /* defer <stmt>: runs when the enclosing block exits (v0.6) */
    /* v0.7 */
    N_TERNARY,   /* cond ? a : b */
    N_MAPLIT,    /* { key: value, ... } -> an open object */
    N_SLICE,     /* a[lo..hi] (as.index.obj, as.index.index = an N_RANGE whose ends may be NULL) */
    N_DESTRUCT,  /* let [a, b, ...rest] = e;   let {x, y} = e; */
    N_ENUM_DECL, /* enum Name { A, B = 5, C } */
    N_IMPL,      /* impl Type { fn method(self) ... } */
    N_TYPE_ALIAS,/* type Name = T;  (resolved by the parser; a no-op at run time) */
    N_TRY,       /* try { } catch (e) { } */
    N_THROW,     /* throw expr; */
    /* v0.8: C and B language capabilities */
    N_INCDEC,    /* ++x  --x  x++  x--  (as.assign: target, value = 1, op; NF_POSTFIX marks the postfix forms) */
    N_COMMA,     /* a, b  (C comma operator: evaluates a, yields b) */
    N_REPEATARR,    /* [value; count]  (as.index.obj = value, as.index.index = count) */
    N_DOWHILE,   /* do { } while (cond); */
    N_CFOR,      /* for (init; cond; step) body */
    N_SWITCH,    /* switch (x) { case a, b: ... default: ... }  with C fall-through */
    N_CASE,      /* one `case`/`default` group of a switch */
    N_LABEL,     /* name: stmt   (a goto target; loops also use it for break/continue) */
    N_GOTO,      /* goto name; */
    N_MULTI,     /* several declarations from one statement (`auto a, b[10];`), run in the enclosing scope */
    N_EXTRN,     /* extrn a, b;  (B: declare that these names come from outside the function) */
    /* statements */
    N_LET, N_EXPRSTMT, N_BLOCK, N_IF, N_WHILE, N_FOR, N_RETURN,
    N_BREAK, N_CONTINUE, N_FN_DECL, N_STRUCT_DECL, N_IMPORT, N_EXTERN_DECL, N_COMPTIME_BLOCK,
    N_PROGRAM,
} KcNodeKind;

typedef enum {
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD,
    OP_EQ, OP_NEQ, OP_LT, OP_LE, OP_GT, OP_GE,
    OP_AND, OP_OR, OP_COALESCE,   /* the short-circuit operators: keep these three adjacent (see N_BINOP in interp.c) */
    OP_NOT, OP_NEG,
    OP_BAND, OP_BOR, OP_BXOR, OP_SHL, OP_SHR, OP_USHR, OP_BNOT,
    OP_IN,         /* x in container */
} KcOp;

typedef struct KcNode KcNode;

typedef struct {
    KcNode **items;
    int count, cap;
} KcNodeList;

void kc_list_push(KcNodeList *l, KcNode *n);

/* per-node interpreter state (v0.5.1) */
#define NF_INTERNED     1   /* identifier text replaced by its interned copy */
#define NF_SCOPE_KNOWN  2   /* block: NF_NEEDS_SCOPE has been computed */
#define NF_NEEDS_SCOPE  4   /* block: defines names directly, so needs its own scope */
#define NF_HAS_DEFER    8   /* block: contains a `defer` statement */
#define NF_HAS_STATIC  32   /* fn: its body declares `static` locals */
#define NF_POSTFIX     16   /* N_INCDEC: the value is the one BEFORE the update */

/* A function node's array of default-value expressions (one entry per parameter, NULL where none;
 * the pointer itself is NULL when no parameter has a default). Kept in the scratch slot so the
 * rarely-used field does not widen every AST node. */
#define FN_DEFAULTS(n) ((n)->fn_defaults)

struct KcNode {
    KcNodeKind kind;
    unsigned char flags;
    /* One scratch slot per node; each node kind uses at most one meaning, so they share storage
     * (this keeps KcNode as small as it was before v0.7: hot nodes stay dense in cache). */
    union {
        void *cache;          /* N_STRING: the cached runtime value */
        int64_t ival;         /* N_INT: the exact value; N_MEMBER (call target): cached method id + 1 */
        const char *label;    /* loops: their label; break/continue: the label they target (interned) */
        KcNode **fn_defaults; /* N_FN_DECL / N_FN_EXPR: see FN_DEFAULTS (a real member: no type-punning of `cache`) */
    };
    KcPos pos;
    union {
        double num;
        char *str;
        bool boolean;
        struct { char *name; } ident;
        struct { KcNode *lhs, *rhs; int op; } binop;
        struct { KcNode *rhs; int op; } unop;
        struct { KcNode *target, *value; int op; } assign;
        struct { KcNode *callee; KcNodeList args; } call;
        struct { KcNode *obj, *index; } index;
        struct { KcNode *obj; char *name; } member;
        struct { KcNodeList items; } array;
        struct { char *name; KcNodeList params; char **param_types; KcNode *body; char *ret_type; bool is_closure;
                 bool variadic;       /* the last parameter collects the remaining arguments into an array */
                 const char *doc;     /* `///` comment lines before the declaration (help()) */
                 int min_args;        /* parameters that must be supplied */
                 /* per-parameter default expressions live in the node's scratch slot: FN_DEFAULTS(n) */
               } fn;
        struct { char *name; char *type_name; KcNode *init; bool is_mutable; bool is_static; } let;
        struct { KcNode *expr; } exprstmt;
        struct { KcNodeList stmts; } block;
        struct { KcNode *cond, *then_branch, *else_branch; } ifstmt;
        struct { KcNode *cond, *body; } whilestmt;
        struct { char *var, *var2; KcNode *iterable, *body; } forstmt;   /* for (var in x) / for (var, var2 in x): (index, element) or (key, value) */
        struct { KcNode *value; } retstmt;
        struct { char *name; KcNodeList fields; char **field_types; bool packed; bool is_union; const char *doc; } structdecl;
        struct { char *module; char *alias; bool is_file; } import_;
        struct { char *name; KcNodeList param_types; char *ret_type; char *from_lib; char *real_symbol; } externdecl;
        struct { KcNode *body; } unsafe_;
        struct { KcNode *body; } comptime_block;
        struct { KcNode *lo, *hi; } range;
        struct { char *type_name; KcNodeList field_names; KcNodeList field_values; } structlit;
        struct { KcNode *subject; KcNodeList patterns; KcNodeList bodies; } matchexpr;
        struct { KcNode *expr; char *type_name; } cast;
        struct { char *type_name; char *field; int which; } sizeof_; /* which: 0 sizeof, 1 alignof, 2 offsetof */
        struct { KcNode *cond, *a, *b; } ternary;
        struct { KcNodeList names, fields; KcNode *init; bool is_obj, is_mutable; char *rest; } destruct;
        struct { char *name; KcNodeList names, values; } enumdecl;
        struct { char *type_name; KcNodeList methods; } impl;
        struct { char *name, *target; } typealias;
        struct { KcNode *body; char *catch_var; KcNode *catch_body; } trystmt;
        struct { KcNodeList keys, values; } maplit;
        struct { KcNodeList init; KcNode *cond; KcNodeList step; KcNode *body; } cfor;
        struct { KcNode *subject; KcNodeList cases; } switchstmt;
        struct { KcNodeList values; KcNode *body; bool is_default; } casenode;
        struct { KcNodeList stmts; } program;
    } as;
};

KcNode *kc_node_new(KcNodeKind kind, KcPos pos);

#endif
