/* KingCobra std.json — JSONPath-style queries.
 *
 * Supported syntax (a practical subset of RFC 9535):
 *   $                       the document root          (a leading "$" is optional)
 *   .name   ['name']        member selection
 *   [3]  [-1]               array index (negative counts from the end)
 *   .*   [*]                every child
 *   ..name  ..*  ..[..]     recursive descent
 *   [1:4]  [::2]  [::-1]    slices  start:end:step
 *   [0,2,'k']               unions of the selectors above
 *   [?(@.price < 10 && @.stock)]   filters:
 *        operands   @.path   $.path   123  1.5  "str"  'str'  true  false  null
 *        operators  == != < <= > >=   && || !   ( )   plain existence tests
 *        functions  length(x)  count(@.items[*])  match(x, "re")  search(x, "re")
 *
 * Results come back in document order. Missing paths simply match nothing.
 *
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com>
 */
#include "json_internal.h"
#include <ctype.h>
#include <math.h>

/* ============================================================ AST */

typedef struct JqPath JqPath;
typedef struct FExpr FExpr;

typedef struct Opnd {
    int kind;                         /* O_LIT, O_PATH, O_FUNC */
    KcValue lit;
    JqPath *path;
    int fn;                           /* F_LENGTH ... */
    struct Opnd *args[2];
    int nargs;
} Opnd;
enum { O_LIT, O_PATH, O_FUNC };
enum { FN_LENGTH, FN_COUNT, FN_MATCH, FN_SEARCH };

struct FExpr {
    int kind;                         /* E_OR E_AND E_NOT E_CMP E_EXISTS */
    FExpr *l, *r;
    Opnd *a, *b;
    int op;                           /* C_EQ.. */
};
enum { E_OR, E_AND, E_NOT, E_CMP, E_EXISTS };
enum { C_EQ, C_NE, C_LT, C_LE, C_GT, C_GE };

typedef struct {
    int kind;                         /* S_NAME S_INDEX S_WILD S_SLICE S_FILTER */
    char *name; size_t nlen;
    int64_t idx;
    bool has_start, has_end, has_step;
    int64_t start, end, step;
    FExpr *filter;
} JqSel;
enum { S_NAME, S_INDEX, S_WILD, S_SLICE, S_FILTER };

typedef struct { bool descend; JqSel *sels; int nsels; } JqSeg;
struct JqPath { JqSeg *segs; int nsegs; bool from_root; };

/* ============================================================ parser */

typedef struct { const char *s; size_t i, n; char *err; size_t errsz; bool failed; } QP;

static void qfail(QP *q, const char *fmt, ...) {
    if (q->failed) return;
    q->failed = true;
    char msg[140];
    va_list ap; va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    snprintf(q->err, q->errsz, "query error at offset %zu: %s", q->i, msg);
}

static void qws(QP *q) { while (q->i < q->n && isspace((unsigned char)q->s[q->i])) q->i++; }
static bool qpeek(QP *q, char c) { return q->i < q->n && q->s[q->i] == c; }
static bool qlook(QP *q, const char *w) { size_t l = strlen(w); return q->i + l <= q->n && memcmp(q->s + q->i, w, l) == 0; }

static bool is_name_char(unsigned char c) { return isalnum(c) || c == '_' || c == '-' || c >= 0x80; }

static bool parse_quoted(QP *q, char **out, size_t *olen) {
    char quote = q->s[q->i++];
    JBuf b; jb_init(&b);
    while (q->i < q->n) {
        char c = q->s[q->i];
        if (c == '\\' && q->i + 1 < q->n) {
            char e = q->s[q->i + 1];
            jb_putc(&b, e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e);
            q->i += 2; continue;
        }
        if (c == quote) { q->i++; *out = jb_take(&b, olen); return true; }
        jb_putc(&b, c); q->i++;
    }
    free(b.p);
    qfail(q, "unterminated string");
    return false;
}

static bool parse_int(QP *q, int64_t *out) {
    size_t st = q->i;
    if (qpeek(q, '-')) q->i++;
    size_t ds = q->i;
    while (q->i < q->n && isdigit((unsigned char)q->s[q->i])) q->i++;
    if (q->i == ds) { q->i = st; return false; }
    if (q->i - ds > 18) { qfail(q, "integer too large"); return false; }
    *out = strtoll(q->s + st, NULL, 10);
    return true;
}

static FExpr *parse_or(QP *q);
static bool parse_path_tail(QP *q, JqPath *p);

static bool parse_selector(QP *q, JqSel *sel) {
    memset(sel, 0, sizeof *sel);
    qws(q);
    if (q->i >= q->n) { qfail(q, "unexpected end inside [...]"); return false; }
    char c = q->s[q->i];
    if (c == '*') { q->i++; sel->kind = S_WILD; return true; }
    if (c == '\'' || c == '"') { sel->kind = S_NAME; return parse_quoted(q, &sel->name, &sel->nlen); }
    if (c == '?') {
        q->i++;
        sel->kind = S_FILTER;
        sel->filter = parse_or(q);
        return sel->filter != NULL;
    }
    /* index or slice */
    int64_t a = 0, b = 0, st = 0;
    bool ha = parse_int(q, &a);
    if (q->failed) return false;
    qws(q);
    if (qpeek(q, ':')) {
        q->i++;
        sel->kind = S_SLICE;
        sel->has_start = ha; sel->start = a;
        qws(q);
        if (parse_int(q, &b)) { sel->has_end = true; sel->end = b; }
        if (q->failed) return false;
        qws(q);
        if (qpeek(q, ':')) {
            q->i++; qws(q);
            if (parse_int(q, &st)) { sel->has_step = true; sel->step = st; }
            if (q->failed) return false;
        }
        return true;
    }
    if (!ha) { qfail(q, "expected an index, slice, quoted name, '*' or filter"); return false; }
    sel->kind = S_INDEX; sel->idx = a;
    return true;
}

static void seg_add_sel(JqSeg *seg, JqSel sel) {
    seg->sels = kc_realloc(seg->sels, sizeof(JqSel) * (size_t)(seg->nsels + 1));
    seg->sels[seg->nsels++] = sel;
}
static void path_add_seg(JqPath *p, JqSeg seg) {
    p->segs = kc_realloc(p->segs, sizeof(JqSeg) * (size_t)(p->nsegs + 1));
    p->segs[p->nsegs++] = seg;
}

static bool parse_bracket(QP *q, JqSeg *seg) {
    q->i++;                                        /* '[' */
    for (;;) {
        JqSel sel;
        if (!parse_selector(q, &sel)) return false;
        seg_add_sel(seg, sel);
        qws(q);
        if (qpeek(q, ',')) { q->i++; continue; }
        if (qpeek(q, ']')) { q->i++; return true; }
        qfail(q, "expected ',' or ']'");
        return false;
    }
}

static bool parse_name_sel(QP *q, JqSeg *seg) {
    JqSel sel; memset(&sel, 0, sizeof sel);
    if (qpeek(q, '*')) { q->i++; sel.kind = S_WILD; seg_add_sel(seg, sel); return true; }
    size_t st = q->i;
    while (q->i < q->n && is_name_char((unsigned char)q->s[q->i])) q->i++;
    if (q->i == st) { qfail(q, "expected a member name, '*' or '[' here"); return false; }
    sel.kind = S_NAME;
    sel.nlen = q->i - st;
    sel.name = kc_malloc(sel.nlen + 1);
    memcpy(sel.name, q->s + st, sel.nlen); sel.name[sel.nlen] = 0;
    seg_add_sel(seg, sel);
    return true;
}

/* Parses segments after the root marker ('$' or '@'). `implicit_name`: allow a bare leading name. */
static bool parse_segments(QP *q, JqPath *p, bool implicit_name) {
    bool first = true;
    for (;;) {
        JqSeg seg; memset(&seg, 0, sizeof seg);
        if (qlook(q, "..")) {
            q->i += 2;
            seg.descend = true;
            if (qpeek(q, '[')) { if (!parse_bracket(q, &seg)) return false; }
            else if (!parse_name_sel(q, &seg)) return false;
        } else if (qpeek(q, '.')) {
            q->i++;
            if (!parse_name_sel(q, &seg)) return false;
        } else if (qpeek(q, '[')) {
            if (!parse_bracket(q, &seg)) return false;
        } else if (first && implicit_name && q->i < q->n && is_name_char((unsigned char)q->s[q->i])) {
            if (!parse_name_sel(q, &seg)) return false;
        } else return !q->failed;
        path_add_seg(p, seg);
        first = false;
    }
}

static bool parse_path_tail(QP *q, JqPath *p) { return parse_segments(q, p, false); }

/* ---- filter expressions ---- */

static Opnd *new_opnd(int kind) { Opnd *o = kc_malloc(sizeof(Opnd)); memset(o, 0, sizeof *o); o->kind = kind; return o; }

static Opnd *parse_operand(QP *q) {
    qws(q);
    if (q->i >= q->n) { qfail(q, "unexpected end of filter"); return NULL; }
    char c = q->s[q->i];
    if (c == '@' || c == '$') {
        q->i++;
        Opnd *o = new_opnd(O_PATH);
        o->path = kc_malloc(sizeof(JqPath)); memset(o->path, 0, sizeof(JqPath));
        o->path->from_root = (c == '$');
        if (!parse_path_tail(q, o->path)) return NULL;
        return o;
    }
    if (c == '\'' || c == '"') {
        char *s; size_t l;
        if (!parse_quoted(q, &s, &l)) return NULL;
        Opnd *o = new_opnd(O_LIT);
        o->lit = kc_string_take(s, (int)l);
        return o;
    }
    if (isdigit((unsigned char)c) || c == '-' || c == '+' || c == '.') {
        size_t st = q->i;
        if (c == '-' || c == '+') q->i++;
        bool isf = false;
        while (q->i < q->n && isdigit((unsigned char)q->s[q->i])) q->i++;
        if (qpeek(q, '.') && q->i + 1 < q->n && isdigit((unsigned char)q->s[q->i + 1])) {
            isf = true; q->i++;
            while (q->i < q->n && isdigit((unsigned char)q->s[q->i])) q->i++;
        }
        if (qpeek(q, 'e') || qpeek(q, 'E')) {
            size_t save = q->i; q->i++;
            if (qpeek(q, '+') || qpeek(q, '-')) q->i++;
            if (q->i < q->n && isdigit((unsigned char)q->s[q->i])) { isf = true; while (q->i < q->n && isdigit((unsigned char)q->s[q->i])) q->i++; }
            else q->i = save;
        }
        char tmp[64]; size_t l = q->i - st;
        if (l == 0 || l >= sizeof tmp || (l == 1 && !isdigit((unsigned char)q->s[st]))) { qfail(q, "invalid number"); return NULL; }
        memcpy(tmp, q->s + st, l); tmp[l] = 0;
        Opnd *o = new_opnd(O_LIT);
        o->lit = isf ? kc_float(strtod(tmp, NULL)) : kc_int(strtoll(tmp, NULL, 10));
        return o;
    }
    if (isalpha((unsigned char)c)) {
        size_t st = q->i;
        while (q->i < q->n && (isalnum((unsigned char)q->s[q->i]) || q->s[q->i] == '_')) q->i++;
        size_t l = q->i - st;
        const char *w = q->s + st;
        #define IS(x) (l == strlen(x) && memcmp(w, x, l) == 0)
        if (IS("true")) { Opnd *o = new_opnd(O_LIT); o->lit = kc_bool(true); return o; }
        if (IS("false")) { Opnd *o = new_opnd(O_LIT); o->lit = kc_bool(false); return o; }
        if (IS("null")) { Opnd *o = new_opnd(O_LIT); o->lit = kc_null(); return o; }
        int fn = -1, want = 0;
        if (IS("length")) { fn = FN_LENGTH; want = 1; }
        else if (IS("count")) { fn = FN_COUNT; want = 1; }
        else if (IS("match")) { fn = FN_MATCH; want = 2; }
        else if (IS("search")) { fn = FN_SEARCH; want = 2; }
        #undef IS
        if (fn < 0) { q->i = st; qfail(q, "unknown name '%.*s' (functions: length count match search; literals: true false null)", (int)l, w); return NULL; }
        qws(q);
        if (!qpeek(q, '(')) { qfail(q, "expected '(' after function name"); return NULL; }
        q->i++;
        Opnd *o = new_opnd(O_FUNC);
        o->fn = fn; o->nargs = want;
        for (int k = 0; k < want; k++) {
            if (k) { qws(q); if (!qpeek(q, ',')) { qfail(q, "expected ',' between function arguments"); return NULL; } q->i++; }
            o->args[k] = parse_operand(q);
            if (!o->args[k]) return NULL;
        }
        qws(q);
        if (!qpeek(q, ')')) { qfail(q, "expected ')' after function arguments"); return NULL; }
        q->i++;
        if (fn == FN_COUNT && o->args[0]->kind != O_PATH) { qfail(q, "count() needs a path argument such as @.items[*]"); return NULL; }
        return o;
    }
    qfail(q, "unexpected '%c' in filter", c);
    return NULL;
}

static FExpr *new_fexpr(int kind) { FExpr *e = kc_malloc(sizeof(FExpr)); memset(e, 0, sizeof *e); e->kind = kind; return e; }

static FExpr *parse_unary(QP *q) {
    qws(q);
    if (qpeek(q, '!') && !qlook(q, "!=")) {
        q->i++;
        FExpr *inner = parse_unary(q);
        if (!inner) return NULL;
        FExpr *e = new_fexpr(E_NOT); e->l = inner;
        return e;
    }
    if (qpeek(q, '(')) {
        q->i++;
        FExpr *e = parse_or(q);
        if (!e) return NULL;
        qws(q);
        if (!qpeek(q, ')')) { qfail(q, "expected ')'"); return NULL; }
        q->i++;
        return e;
    }
    Opnd *a = parse_operand(q);
    if (!a) return NULL;
    qws(q);
    int op = -1;
    if (qlook(q, "==")) { op = C_EQ; q->i += 2; }
    else if (qlook(q, "!=")) { op = C_NE; q->i += 2; }
    else if (qlook(q, "<=")) { op = C_LE; q->i += 2; }
    else if (qlook(q, ">=")) { op = C_GE; q->i += 2; }
    else if (qlook(q, "<")) { op = C_LT; q->i += 1; }
    else if (qlook(q, ">")) { op = C_GT; q->i += 1; }
    if (op < 0) { FExpr *e = new_fexpr(E_EXISTS); e->a = a; return e; }
    Opnd *b = parse_operand(q);
    if (!b) return NULL;
    FExpr *e = new_fexpr(E_CMP); e->a = a; e->b = b; e->op = op;
    return e;
}

static FExpr *parse_and(QP *q) {
    FExpr *l = parse_unary(q);
    if (!l) return NULL;
    for (;;) {
        qws(q);
        if (!qlook(q, "&&")) return l;
        q->i += 2;
        FExpr *r = parse_unary(q);
        if (!r) return NULL;
        FExpr *e = new_fexpr(E_AND); e->l = l; e->r = r;
        l = e;
    }
}
static FExpr *parse_or(QP *q) {
    FExpr *l = parse_and(q);
    if (!l) return NULL;
    for (;;) {
        qws(q);
        if (!qlook(q, "||")) return l;
        q->i += 2;
        FExpr *r = parse_and(q);
        if (!r) return NULL;
        FExpr *e = new_fexpr(E_OR); e->l = l; e->r = r;
        l = e;
    }
}

/* ============================================================ evaluation */

typedef struct { KcValue v; char *path; } Node;
typedef struct { Node *items; int n, cap; } NodeList;

typedef struct { KcInterp *ip; KcPos pos; KcValue root; bool want_paths; int depth; } QCtx;

static void nl_add(NodeList *l, KcValue v, char *path) {
    if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 8; l->items = kc_realloc(l->items, sizeof(Node) * (size_t)l->cap); }
    l->items[l->n].v = v; l->items[l->n].path = path;
    l->n++;
}

static bool key_plain(const char *k, size_t n) {
    if (!n) return false;
    for (size_t i = 0; i < n; i++) if (!is_name_char((unsigned char)k[i])) return false;
    return true;
}

static char *child_name_path(QCtx *c, const char *parent, const char *key, size_t klen) {
    if (!c->want_paths) return NULL;
    JBuf b; jb_init(&b);
    jb_puts(&b, parent);
    if (key_plain(key, klen)) { jb_putc(&b, '.'); jb_put(&b, key, klen); }
    else {
        jb_puts(&b, "[\"");
        for (size_t i = 0; i < klen; i++) { if (key[i] == '"' || key[i] == '\\') jb_putc(&b, '\\'); jb_putc(&b, key[i]); }
        jb_puts(&b, "\"]");
    }
    size_t l; return jb_take(&b, &l);
}
static char *child_index_path(QCtx *c, const char *parent, int idx) {
    if (!c->want_paths) return NULL;
    JBuf b; jb_init(&b);
    jb_puts(&b, parent); jb_printf(&b, "[%d]", idx);
    size_t l; return jb_take(&b, &l);
}

typedef struct { bool exists; KcValue v; } OVal;

static bool eval_fexpr(QCtx *c, FExpr *e, KcValue cur);
static void eval_path(QCtx *c, const JqPath *p, KcValue start, const char *start_path, NodeList *out);

static int64_t norm_idx(int64_t i, int64_t len) { return i >= 0 ? i : len + i; }

static void apply_selector(QCtx *c, const JqSel *s, Node node, NodeList *out) {
    KcValue v = node.v;
    const char *pp = node.path ? node.path : "$";
    switch (s->kind) {
        case S_NAME:
            if (v.type == VAL_STRUCT) {
                KcValue *ch = kc_object_get(v, s->name, s->nlen);
                if (ch) nl_add(out, *ch, child_name_path(c, pp, s->name, s->nlen));
            }
            break;
        case S_INDEX:
            if (v.type == VAL_ARRAY) {
                int64_t i = norm_idx(s->idx, v.as.arr->count);
                if (i >= 0 && i < v.as.arr->count) nl_add(out, v.as.arr->items[i], child_index_path(c, pp, (int)i));
            }
            break;
        case S_WILD:
            if (v.type == VAL_ARRAY) {
                for (int i = 0; i < v.as.arr->count; i++) nl_add(out, v.as.arr->items[i], child_index_path(c, pp, i));
            } else if (v.type == VAL_STRUCT) {
                for (int i = 0; i < v.as.strct->count; i++)
                    nl_add(out, v.as.strct->values[i], child_name_path(c, pp, v.as.strct->names[i], strlen(v.as.strct->names[i])));
            }
            break;
        case S_SLICE:
            if (v.type == VAL_ARRAY) {
                int64_t len = v.as.arr->count;
                int64_t step = s->has_step ? s->step : 1;
                if (step == 0) break;
                int64_t start = s->has_start ? s->start : (step >= 0 ? 0 : len - 1);
                int64_t end = s->has_end ? s->end : (step >= 0 ? len : -len - 1);
                int64_t ns = norm_idx(start, len), ne = norm_idx(end, len), lower, upper;
                if (step > 0) {
                    lower = ns < 0 ? 0 : (ns > len ? len : ns);
                    upper = ne < 0 ? 0 : (ne > len ? len : ne);
                    for (int64_t i = lower; i < upper; i += step) nl_add(out, v.as.arr->items[i], child_index_path(c, pp, (int)i));
                } else {
                    upper = ns < -1 ? -1 : (ns > len - 1 ? len - 1 : ns);
                    lower = ne < -1 ? -1 : (ne > len - 1 ? len - 1 : ne);
                    for (int64_t i = upper; lower < i; i += step) nl_add(out, v.as.arr->items[i], child_index_path(c, pp, (int)i));
                }
            }
            break;
        case S_FILTER: {
            if (v.type == VAL_ARRAY) {
                for (int i = 0; i < v.as.arr->count; i++)
                    if (eval_fexpr(c, s->filter, v.as.arr->items[i])) nl_add(out, v.as.arr->items[i], child_index_path(c, pp, i));
            } else if (v.type == VAL_STRUCT) {
                for (int i = 0; i < v.as.strct->count; i++)
                    if (eval_fexpr(c, s->filter, v.as.strct->values[i]))
                        nl_add(out, v.as.strct->values[i], child_name_path(c, pp, v.as.strct->names[i], strlen(v.as.strct->names[i])));
            }
            break;
        }
    }
}

static void collect_descendants(QCtx *c, Node node, NodeList *out, int depth) {
    if (depth > 512) kc_error_at(c->pos, "json.query: document nests too deeply (cyclic structure?)");
    nl_add(out, node.v, node.path);
    const char *pp = node.path ? node.path : "$";
    if (node.v.type == VAL_ARRAY) {
        for (int i = 0; i < node.v.as.arr->count; i++) {
            Node ch = { node.v.as.arr->items[i], child_index_path(c, pp, i) };
            collect_descendants(c, ch, out, depth + 1);
        }
    } else if (node.v.type == VAL_STRUCT) {
        for (int i = 0; i < node.v.as.strct->count; i++) {
            Node ch = { node.v.as.strct->values[i], child_name_path(c, pp, node.v.as.strct->names[i], strlen(node.v.as.strct->names[i])) };
            collect_descendants(c, ch, out, depth + 1);
        }
    }
}

static void eval_path(QCtx *c, const JqPath *p, KcValue start, const char *start_path, NodeList *out) {
    NodeList cur = {0};
    nl_add(&cur, start, start_path ? kc_strdup(start_path) : NULL);
    for (int si = 0; si < p->nsegs; si++) {
        const JqSeg *seg = &p->segs[si];
        NodeList next = {0};
        for (int ni = 0; ni < cur.n; ni++) {
            if (seg->descend) {
                NodeList desc = {0};
                collect_descendants(c, cur.items[ni], &desc, 0);
                for (int di = 0; di < desc.n; di++)
                    for (int k = 0; k < seg->nsels; k++) apply_selector(c, &seg->sels[k], desc.items[di], &next);
                free(desc.items);
            } else {
                for (int k = 0; k < seg->nsels; k++) apply_selector(c, &seg->sels[k], cur.items[ni], &next);
            }
        }
        free(cur.items);
        cur = next;
    }
    for (int i = 0; i < cur.n; i++) nl_add(out, cur.items[i].v, cur.items[i].path);
    free(cur.items);
}

static int utf8_count(const char *s, int n) {
    int c = 0;
    for (int i = 0; i < n; i++) if (((unsigned char)s[i] & 0xC0) != 0x80) c++;
    return c;
}

static OVal eval_opnd(QCtx *c, Opnd *o, KcValue cur) {
    OVal r = { false, kc_null() };
    switch (o->kind) {
        case O_LIT: r.exists = true; r.v = o->lit; return r;
        case O_PATH: {
            NodeList nl = {0};
            bool saved = c->want_paths; c->want_paths = false;
            eval_path(c, o->path, o->path->from_root ? c->root : cur, NULL, &nl);
            c->want_paths = saved;
            if (nl.n > 0) { r.exists = true; r.v = nl.items[0].v; }
            free(nl.items);
            return r;
        }
        case O_FUNC: {
            if (o->fn == FN_COUNT) {
                NodeList nl = {0};
                bool saved = c->want_paths; c->want_paths = false;
                eval_path(c, o->args[0]->path, o->args[0]->path->from_root ? c->root : cur, NULL, &nl);
                c->want_paths = saved;
                r.exists = true; r.v = kc_int(nl.n);
                free(nl.items);
                return r;
            }
            OVal a0 = eval_opnd(c, o->args[0], cur);
            if (o->fn == FN_LENGTH) {
                if (!a0.exists) return r;
                if (a0.v.type == VAL_STRING) { r.exists = true; r.v = kc_int(utf8_count(a0.v.as.s->data, a0.v.as.s->len)); }
                else if (a0.v.type == VAL_ARRAY) { r.exists = true; r.v = kc_int(a0.v.as.arr->count); }
                else if (a0.v.type == VAL_STRUCT) { r.exists = true; r.v = kc_int(a0.v.as.strct->count); }
                return r;
            }
            /* match / search */
            OVal a1 = eval_opnd(c, o->args[1], cur);
            r.exists = true; r.v = kc_bool(false);
            if (!a0.exists || !a1.exists || a0.v.type != VAL_STRING || a1.v.type != VAL_STRING) return r;
            KcModule *rm = kc_mod_regx();
            const char *want = o->fn == FN_MATCH ? "full" : "test";
            for (int i = 0; i < rm->count; i++) {
                if (strcmp(rm->entries[i].name, want) == 0) {
                    KcValue args[2] = { a1.v, a0.v };           /* regx.full(pattern, text) */
                    r.v = rm->entries[i].fn(c->ip, args, 2, c->pos);
                    break;
                }
            }
            return r;
        }
    }
    return r;
}

static bool vals_eq(OVal a, OVal b) {
    if (!a.exists || !b.exists) return !a.exists && !b.exists;
    return jv_equal(a.v, b.v);
}
static bool vals_lt(OVal a, OVal b) {
    if (!a.exists || !b.exists) return false;
    if ((a.v.type == VAL_INT || a.v.type == VAL_FLOAT) && (b.v.type == VAL_INT || b.v.type == VAL_FLOAT)) {
        if (a.v.type == VAL_INT && b.v.type == VAL_INT) return a.v.as.i < b.v.as.i;
        double x = a.v.type == VAL_INT ? (double)a.v.as.i : a.v.as.f;
        double y = b.v.type == VAL_INT ? (double)b.v.as.i : b.v.as.f;
        return x < y;
    }
    if (a.v.type == VAL_STRING && b.v.type == VAL_STRING) {
        size_t la = (size_t)a.v.as.s->len, lb = (size_t)b.v.as.s->len;
        int cmp = memcmp(a.v.as.s->data, b.v.as.s->data, la < lb ? la : lb);
        return cmp < 0 || (cmp == 0 && la < lb);
    }
    return false;
}

static bool eval_fexpr(QCtx *c, FExpr *e, KcValue cur) {
    if (++c->depth > 512) kc_error_at(c->pos, "json.query: filter nests too deeply");
    bool r = false;
    switch (e->kind) {
        case E_OR: r = eval_fexpr(c, e->l, cur) || eval_fexpr(c, e->r, cur); break;
        case E_AND: r = eval_fexpr(c, e->l, cur) && eval_fexpr(c, e->r, cur); break;
        case E_NOT: r = !eval_fexpr(c, e->l, cur); break;
        case E_EXISTS: {
            OVal a = eval_opnd(c, e->a, cur);
            if (e->a->kind == O_PATH) r = a.exists;
            else r = a.exists && kc_truthy(a.v);
            break;
        }
        case E_CMP: {
            OVal a = eval_opnd(c, e->a, cur), b = eval_opnd(c, e->b, cur);
            switch (e->op) {
                case C_EQ: r = vals_eq(a, b); break;
                case C_NE: r = !vals_eq(a, b); break;
                case C_LT: r = vals_lt(a, b); break;
                case C_LE: r = vals_lt(a, b) || vals_eq(a, b); break;
                case C_GT: r = vals_lt(b, a); break;
                case C_GE: r = vals_lt(b, a) || vals_eq(a, b); break;
            }
            break;
        }
    }
    c->depth--;
    return r;
}

/* ============================================================ entry point */

bool jq_run(KcInterp *ip, KcPos pos, KcValue doc, const char *expr, KcValue *out_values, KcValue *out_paths, char *err, size_t errsz) {
    QP q = { expr, 0, strlen(expr), err, errsz, false };
    err[0] = 0;
    qws(&q);
    JqPath path; memset(&path, 0, sizeof path);
    bool implicit = true;
    if (qpeek(&q, '$')) { q.i++; implicit = false; }
    if (!parse_segments(&q, &path, implicit) || q.failed) { if (!err[0]) snprintf(err, errsz, "query error at offset %zu", q.i); return false; }
    qws(&q);
    if (q.i < q.n) { qfail(&q, "unexpected '%c'", q.s[q.i]); return false; }

    QCtx c = { ip, pos, doc, out_paths != NULL, 0 };
    NodeList nl = {0};
    eval_path(&c, &path, doc, out_paths ? "$" : NULL, &nl);
    *out_values = kc_array_new();
    if (out_paths) *out_paths = kc_array_new();
    for (int i = 0; i < nl.n; i++) {
        kc_array_push(*out_values, nl.items[i].v);
        if (out_paths) kc_array_push(*out_paths, kc_string(nl.items[i].path ? nl.items[i].path : "$"));
    }
    free(nl.items);
    return true;
}

/* ============================================================ module functions */

static void need(int n, int lo, int hi, KcPos pos, const char *sig) {
    if (n < lo || n > hi) kc_error_at(pos, "usage: json.%s", sig);
}
static const char *expr_arg(KcValue v, KcPos pos, const char *who) {
    if (v.type != VAL_STRING) kc_error_at(pos, "%s: the query must be a string, got %s", who, kc_type_name(v));
    return v.as.s->data;
}

static KcValue q_query(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 2, 2, pos, "query(doc, expr) -> array   (JSONPath: $.a.b[*].c, ..name, [1:3], [?(@.x > 1)])");
    char err[200]; KcValue vals;
    if (!jq_run(ip, pos, a[0], expr_arg(a[1], pos, "json.query"), &vals, NULL, err, sizeof err)) kc_error_at(pos, "json.query: %s", err);
    return vals;
}
static KcValue q_query_paths(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 2, 2, pos, "query_paths(doc, expr) -> string[]   (normalized paths like \"$.a[0].b\"; usable with get_path)");
    char err[200]; KcValue vals, paths;
    if (!jq_run(ip, pos, a[0], expr_arg(a[1], pos, "json.query_paths"), &vals, &paths, err, sizeof err)) kc_error_at(pos, "json.query_paths: %s", err);
    return paths;
}
static KcValue q_query_first(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 2, 3, pos, "query_first(doc, expr[, default]) -> value");
    char err[200]; KcValue vals;
    if (!jq_run(ip, pos, a[0], expr_arg(a[1], pos, "json.query_first"), &vals, NULL, err, sizeof err)) kc_error_at(pos, "json.query_first: %s", err);
    if (vals.as.arr->count > 0) return vals.as.arr->items[0];
    return n > 2 ? a[2] : kc_null();
}

KcModuleEntry json_query_entries[] = {
    {"query", q_query}, {"query_paths", q_query_paths}, {"query_first", q_query_first},
};
const int json_query_count = (int)(sizeof json_query_entries / sizeof json_query_entries[0]);
