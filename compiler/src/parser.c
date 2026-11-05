#include "parser.h"
#include "env.h"
#include <stdarg.h>
#include <ctype.h>

typedef struct {
    KcLexer lx;
    KcToken cur, prev;
    const char *filename;
    const char *labels[32];   /* enclosing loop labels, for validating `break label;` */
    int nlabels;
    const char *alias_names[64]; char *alias_targets[64];   /* `type Name = T;` (resolved while parsing) */
    int naliases;
    KcNode *cur_fn;           /* the function whose body is being parsed (to flag `static` locals) */
} Parser;

static void advance(Parser *p) {
    p->prev = p->cur;
    p->cur = kc_lexer_next(&p->lx);
}

static bool check(Parser *p, KcTokKind k) { return p->cur.kind == k; }

static bool match(Parser *p, KcTokKind k) {
    if (!check(p, k)) return false;
    advance(p);
    return true;
}

static void expect(Parser *p, KcTokKind k, const char *what) {
    if (!check(p, k)) {
        kc_error_at(p->cur.pos, "expected %s but found '%.*s'", what, p->cur.len, p->cur.start);
    }
    advance(p);
}

static char *tok_str(KcToken t) {
    char *s = kc_malloc(t.len + 1);
    memcpy(s, t.start, t.len);
    s[t.len] = 0;
    return s;
}

/* forward decls */
static KcNode *parse_expr(Parser *p);
static void parse_decl_list(Parser *p, bool mutable, KcNodeList *out);
static KcNode *parse_body(Parser *p);
static KcNode *parse_block(Parser *p);
static KcNode *parse_stmt(Parser *p);

static char *parse_type_str(Parser *p) {
    /* types: ident, ident[], ident[N], *T (pointers, nestable), or the bare
       keyword 'fn' (any closure/function value) -- kept as text; the
       static checker and ptr.c interpret it. */
    int stars = 0;
    while (match(p, TK_STAR)) stars++;
    char *base;
    if (check(p, TK_FN)) { base = kc_strdup("fn"); advance(p); }
    else if (check(p, TK_IDENT)) { base = tok_str(p->cur); advance(p); }
    else { kc_error_at(p->cur.pos, "expected type name"); return NULL; }
    for (int i = p->naliases - 1; i >= 0; i--)          /* `type Num = int;` */
        if (strcmp(base, p->alias_names[i]) == 0) { free(base); base = kc_strdup(p->alias_targets[i]); break; }
    while (match(p, TK_LBRACKET)) {
        char suffix[40];
        if (check(p, TK_INT)) { snprintf(suffix, sizeof suffix, "[%lld]", (long long)p->cur.num); advance(p); }
        else strcpy(suffix, "[]");
        expect(p, TK_RBRACKET, "]");
        char *n = kc_malloc(strlen(base) + strlen(suffix) + 1);
        sprintf(n, "%s%s", base, suffix);
        base = n;
    }
    if (stars) {
        char *n = kc_malloc(strlen(base) + (size_t)stars + 1);
        memset(n, '*', (size_t)stars);
        strcpy(n + stars, base);
        base = n;
    }
    return base;
}

/* ---------------- primary / postfix ---------------- */

/* `let add = f(x, y) { ... }` / `func(a: int) -> int { ... }` : a call whose ")" is followed by a block or "->" is almost
 * certainly a function expression with a mistyped keyword. Say so instead of a baffling "expected ;". */
static void hint_misspelled_fn(Parser *p, KcNode *callee) {
    if (callee->kind != N_IDENT) return;
    static const char *const names[] = { "f", "fun", "func", "function", "def", "lambda", "fnc", "fb", NULL };
    const char *nm = callee->as.ident.name; bool hit = false;
    for (int i = 0; names[i]; i++) if (!strcmp(nm, names[i])) hit = true;
    if (!hit) return;
    KcLexer lx = p->lx; KcToken t = p->cur; int depth = 1;
    for (;;) {
        if (t.kind == TK_EOF) return;
        if (t.kind == TK_LPAREN) depth++;
        else if (t.kind == TK_RPAREN && --depth == 0) break;
        t = kc_lexer_next(&lx);
    }
    t = kc_lexer_next(&lx);
    if (t.kind == TK_LBRACE || t.kind == TK_ARROW)
        kc_error_at(callee->pos, "'%s(...)' followed by a body looks like a function written with the wrong keyword: use `fn` (for example `fn(x, y) { return x + y; }`)", nm);
}

static KcNode *finish_call(Parser *p, KcNode *callee) {
    hint_misspelled_fn(p, callee);
    KcNode *n = kc_node_new(N_CALL, callee->pos);
    n->as.call.callee = callee;
    if (!check(p, TK_RPAREN)) {
        do {
            kc_list_push(&n->as.call.args, parse_expr(p));
        } while (match(p, TK_COMMA));
    }
    expect(p, TK_RPAREN, ")");
    return n;
}

/* A match arm's pattern: a literal (int/float/string/bool/null, with an
   optional leading '-' for negative numbers) or the wildcard `_`. Full
   destructuring patterns are a later roadmap item — this covers the
   common "switch over a value" use case. Returns NULL for `_`. */
static KcNode *parse_match_pattern(Parser *p) {
    KcPos pos = p->cur.pos;
    if (check(p, TK_IDENT) && p->cur.len == 1 && p->cur.start[0] == '_') { advance(p); return NULL; }
    if (match(p, TK_INT))    { if (p->prev.imin) kc_error_at(pos, "integer literal is too large"); KcNode *n = kc_node_new(N_INT, pos); n->as.num = p->prev.num; n->ival = p->prev.ival; return n; }
    if (match(p, TK_FLOAT))  { KcNode *n = kc_node_new(N_FLOAT, pos);  n->as.num = p->prev.num; return n; }
    if (match(p, TK_STRING)) { KcNode *n = kc_node_new(N_STRING, pos); n->as.str = p->prev.str; return n; }
    if (match(p, TK_TRUE))   { KcNode *n = kc_node_new(N_BOOL, pos);   n->as.boolean = true;  return n; }
    if (match(p, TK_FALSE))  { KcNode *n = kc_node_new(N_BOOL, pos);   n->as.boolean = false; return n; }
    if (match(p, TK_NULL))   { return kc_node_new(N_NULL, pos); }
    if (match(p, TK_MINUS)) {
        if (match(p, TK_INT))   { KcNode *n = kc_node_new(N_INT, pos);   n->ival = p->prev.imin ? INT64_MIN : -p->prev.ival; n->as.num = (double)n->ival; return n; }
        if (match(p, TK_FLOAT)) { KcNode *n = kc_node_new(N_FLOAT, pos); n->as.num = -p->prev.num; return n; }
        kc_error_at(pos, "expected a number after '-' in match pattern");
    }
    if (check(p, TK_IDENT)) {              /* a constant or Enum.Member path: compared with == at run time */
        KcNode *e = kc_node_new(N_IDENT, pos);
        e->as.ident.name = tok_str(p->cur);
        advance(p);
        while (check(p, TK_DOT)) {
            KcPos mp = p->cur.pos;
            advance(p);
            KcNode *m = kc_node_new(N_MEMBER, mp);
            m->as.member.obj = e;
            m->as.member.name = tok_str(p->cur);
            expect(p, TK_IDENT, "member name");
            e = m;
        }
        return e;
    }
    kc_error_at(pos, "invalid match pattern: expected a literal, '_', a constant, or an Enum.Member path");
    return NULL;
}

static KcNode *parse_match(Parser *p) {
    KcPos pos = p->cur.pos;
    expect(p, TK_MATCH, "match");
    KcNode *n = kc_node_new(N_MATCH, pos);
    expect(p, TK_LPAREN, "(");
    n->as.matchexpr.subject = parse_expr(p);
    expect(p, TK_RPAREN, ")");
    expect(p, TK_LBRACE, "{");
    bool seen_wildcard = false;
    while (!check(p, TK_RBRACE) && !check(p, TK_EOF)) {
        KcNode *alts[16]; int nalts = 0;
        do {                                           /* 1 | 2 | 3 => ... shares one body */
            if (nalts == 16) kc_error_at(p->cur.pos, "match: too many alternatives in one arm (max 16)");
            alts[nalts] = parse_match_pattern(p);
            if (!alts[nalts]) {
                if (seen_wildcard) kc_error_at(p->cur.pos, "match: only one '_' wildcard arm is allowed");
                if (nalts > 0 || check(p, TK_PIPE)) kc_error_at(p->cur.pos, "match: '_' cannot be combined with other patterns using '|'");
                seen_wildcard = true;
            }
            nalts++;
        } while (match(p, TK_PIPE));
        expect(p, TK_FATARROW, "=>");
        KcNode *body = parse_expr(p);
        for (int i = 0; i < nalts; i++) {
            kc_list_push(&n->as.matchexpr.patterns, alts[i]);
            kc_list_push(&n->as.matchexpr.bodies, body);
        }
        if (!match(p, TK_COMMA)) break;
    }
    expect(p, TK_RBRACE, "}");
    if (n->as.matchexpr.patterns.count == 0) kc_error_at(pos, "match: at least one arm is required");
    return n;
}

/* ---- f-strings: f"x = {x}, next = {x + 1}" desugars to  "x = " + x + ", next = " + (x + 1) ---- */

static KcNode *mkbin(KcNode *l, KcOp op, KcNode *r, KcPos pos);

static void fs_put_utf8(char **buf, size_t *len, size_t *cap, unsigned long cp) {
    char tmp[4]; int n;
    if (cp < 0x80) { tmp[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { tmp[0] = (char)(0xC0 | (cp >> 6)); tmp[1] = (char)(0x80 | (cp & 0x3F)); n = 2; }
    else if (cp < 0x10000) { tmp[0] = (char)(0xE0 | (cp >> 12)); tmp[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); tmp[2] = (char)(0x80 | (cp & 0x3F)); n = 3; }
    else { tmp[0] = (char)(0xF0 | (cp >> 18)); tmp[1] = (char)(0x80 | ((cp >> 12) & 0x3F)); tmp[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); tmp[3] = (char)(0x80 | (cp & 0x3F)); n = 4; }
    if (*len + 5 >= *cap) { *cap *= 2; *buf = kc_realloc(*buf, *cap); }
    memcpy(*buf + *len, tmp, (size_t)n); *len += (size_t)n;
}

static KcNode *parse_embedded(const char *file, KcPos pos, const char *text, int len) {
    Parser sub;
    memset(&sub, 0, sizeof sub);
    char *copy = kc_malloc((size_t)len + 1);       /* kept alive: tokens point into it */
    memcpy(copy, text, (size_t)len); copy[len] = 0;
    kc_lexer_init(&sub.lx, copy, file);
    sub.lx.line = pos.line; sub.lx.col = pos.col;
    sub.filename = file;
    advance(&sub);
    if (check(&sub, TK_EOF)) kc_error_at(pos, "empty expression in f-string ({})");
    KcNode *e = parse_expr(&sub);
    if (!check(&sub, TK_EOF)) kc_error_at(sub.cur.pos, "unexpected '%.*s' in f-string expression", sub.cur.len, sub.cur.start);
    return e;
}

static KcNode *fs_literal(KcPos pos, char *buf, size_t len) {
    buf[len] = 0;
    KcNode *n = kc_node_new(N_STRING, pos);
    n->as.str = buf;
    return n;
}

static KcNode *parse_fstring(Parser *p) {
    KcPos pos = p->prev.pos;
    const char *raw = p->prev.str;
    size_t n = strlen(raw), i = 0;
    KcNode *acc = NULL;
    size_t cap = 32, len = 0;
    char *buf = kc_malloc(cap);
    while (i < n) {
        char c = raw[i];
        if (c == '{' && i + 1 < n && raw[i + 1] == '{') { if (len + 2 >= cap) { cap *= 2; buf = kc_realloc(buf, cap); } buf[len++] = '{'; i += 2; continue; }
        if (c == '}' && i + 1 < n && raw[i + 1] == '}') { if (len + 2 >= cap) { cap *= 2; buf = kc_realloc(buf, cap); } buf[len++] = '}'; i += 2; continue; }
        if (c == '}') kc_error_at(pos, "single '}' in f-string (write '}}' for a literal brace)");
        if (c == '\\' && i + 1 < n) {
            char e = raw[i + 1]; i += 2;
            if (len + 5 >= cap) { cap *= 2; buf = kc_realloc(buf, cap); }
            switch (e) {
                case 'n': buf[len++] = '\n'; break;
                case 't': buf[len++] = '\t'; break;
                case 'r': buf[len++] = '\r'; break;
                case '0': buf[len++] = '\0'; break;
                case 'x':
                    if (i + 1 < n && isxdigit((unsigned char)raw[i]) && isxdigit((unsigned char)raw[i + 1])) {
                        char h[3] = { raw[i], raw[i + 1], 0 }; buf[len++] = (char)strtol(h, NULL, 16); i += 2;
                    } else kc_error_at(pos, "\\x needs two hex digits");
                    break;
                case 'u':
                    if (i < n && raw[i] == '{') {
                        char *end; unsigned long cp = strtoul(raw + i + 1, &end, 16);
                        if (*end != '}') kc_error_at(pos, "malformed \\u{...} escape");
                        fs_put_utf8(&buf, &len, &cap, cp); i = (size_t)(end - raw) + 1;
                    } else kc_error_at(pos, "\\u needs braces, e.g. \\u{1F600}");
                    break;
                default: buf[len++] = e; break;      /* \\ \" \' \{ \} and unknown: the character itself */
            }
            continue;
        }
        if (c == '{') {
            /* literal text so far (the first piece is always a string, so `+` means concatenation) */
            if (len > 0 || !acc) {
                KcNode *lit = fs_literal(pos, buf, len);
                acc = acc ? mkbin(acc, OP_ADD, lit, pos) : lit;
                cap = 32; len = 0; buf = kc_malloc(cap);
            }
            size_t start = i + 1, j = start; int depth = 1;
            while (j < n && depth > 0) {
                if (raw[j] == '"') { j++; while (j < n && raw[j] != '"') { if (raw[j] == '\\') j++; j++; } }
                else if (raw[j] == '{') depth++;
                else if (raw[j] == '}') depth--;
                j++;
            }
            if (depth != 0) kc_error_at(pos, "unclosed '{' in f-string");
            KcPos epos = pos; epos.col += 2 + (int)start;
            KcNode *e = parse_embedded(p->filename, epos, raw + start, (int)(j - 1 - start));
            acc = mkbin(acc, OP_ADD, e, pos);
            i = j;
            continue;
        }
        if (len + 2 >= cap) { cap *= 2; buf = kc_realloc(buf, cap); }
        buf[len++] = c; i++;
    }
    if (len > 0 || !acc) {
        KcNode *lit = fs_literal(pos, buf, len);
        acc = acc ? mkbin(acc, OP_ADD, lit, pos) : lit;
    } else free(buf);
    return acc;
}

/* Parameters after the opening '(' up to and including ')':
 *     name [: type] [= default]  ,  ...rest [: element-type]
 * Defaults are evaluated at call time in the callee's scope (so a default may use earlier
 * parameters). A variadic parameter must be last and arrives as an array. */
static void parse_params(Parser *p, KcNode *n) {
    int cap = 4, cnt = 0, required = 0;
    char **ptypes = kc_malloc(sizeof(char *) * cap);
    KcNode **defaults = NULL;
    bool seen_default = false;
    if (!check(p, TK_RPAREN)) {
        do {
            if (n->as.fn.variadic) kc_error_at(p->cur.pos, "the '...' parameter must be the last one");
            bool variadic = match(p, TK_ELLIPSIS);
            KcNode *param = kc_node_new(N_IDENT, p->cur.pos);
            param->as.ident.name = tok_str(p->cur);
            expect(p, TK_IDENT, "parameter name");
            char *ty = kc_strdup("any");
            if (match(p, TK_COLON)) { free(ty); ty = parse_type_str(p); }
            if (variadic) {                                  /* element type T  ->  the parameter is a T[] */
                char *arr = kc_malloc(strlen(ty) + 3); sprintf(arr, "%s[]", ty); free(ty); ty = arr;
                n->as.fn.variadic = true;
            }
            KcNode *def = NULL;
            if (!variadic && match(p, TK_ASSIGN)) { def = parse_expr(p); seen_default = true; }
            else if (!variadic) {
                if (seen_default) kc_error_at(param->pos, "parameter '%s' has no default but follows a parameter that has one", param->as.ident.name);
                required++;
            }
            if (cnt >= 64) kc_error_at(param->pos, "too many parameters (max 64)");
            if (cnt + 1 > cap) { cap *= 2; ptypes = kc_realloc(ptypes, sizeof(char *) * (size_t)cap); }
            if (def && !defaults) { defaults = kc_malloc(sizeof(KcNode *) * 64); memset(defaults, 0, sizeof(KcNode *) * 64); }
            if (defaults) defaults[cnt] = def;
            ptypes[cnt++] = ty;
            kc_list_push(&n->as.fn.params, param);
        } while (match(p, TK_COMMA));
    }
    expect(p, TK_RPAREN, ")");
    n->as.fn.param_types = ptypes;
    FN_DEFAULTS(n) = defaults;
    n->as.fn.min_args = required;
}

/* generics are erased: inside the function a type parameter T checks as `any` (T, T[], *T, ...) */
static char *erase_tparam(char *ty, char **tp, int ntp) {
    char *base = ty; int stars = 0;
    while (*base == '*') { base++; stars++; }
    size_t blen = strcspn(base, "[");
    for (int i = 0; i < ntp; i++) {
        if (strlen(tp[i]) == blen && strncmp(base, tp[i], blen) == 0) {
            char *out = kc_malloc(strlen(ty) + 8);
            memset(out, '*', (size_t)stars);
            strcpy(out + stars, "any");
            strcat(out, base + blen);
            free(ty);
            return out;
        }
    }
    return ty;
}

static KcNode *parse_primary(Parser *p) {
    if (check(p, TK_MATCH)) return parse_match(p);
    KcPos pos = p->cur.pos;
    if (match(p, TK_INT)) {
        if (p->prev.imin) kc_error_at(pos, "integer literal 9223372036854775808 is too large (only -9223372036854775808 is valid)");
        KcNode *n = kc_node_new(N_INT, pos); n->as.num = p->prev.num; n->ival = p->prev.ival; return n;
    }
    if (match(p, TK_FSTRING)) return parse_fstring(p);
    if (check(p, TK_LBRACE)) {
        /* object literal: { name: expr, "other key": expr }  (a block can never start here:
           statements are recognised by parse_stmt before expression parsing begins) */
        advance(p);
        KcNode *n = kc_node_new(N_MAPLIT, pos);
        while (!check(p, TK_RBRACE) && !check(p, TK_EOF)) {
            KcNode *key = NULL;
            if (check(p, TK_STRING)) { key = kc_node_new(N_STRING, p->cur.pos); key->as.str = p->cur.str; advance(p); }
            else if (check(p, TK_IDENT)) { key = kc_node_new(N_STRING, p->cur.pos); key->as.str = tok_str(p->cur); advance(p); }
            else kc_error_at(p->cur.pos, "expected a key (a name or a string) in an object literal, found '%.*s'", p->cur.len, p->cur.start);
            expect(p, TK_COLON, ":");
            kc_list_push(&n->as.maplit.keys, key);
            kc_list_push(&n->as.maplit.values, parse_expr(p));
            if (!match(p, TK_COMMA)) break;
        }
        expect(p, TK_RBRACE, "}");
        return n;
    }
    if (match(p, TK_FLOAT)) {
        KcNode *n = kc_node_new(N_FLOAT, pos); n->as.num = p->prev.num; return n;
    }
    if (match(p, TK_STRING)) {
        KcNode *n = kc_node_new(N_STRING, pos); n->as.str = p->prev.str;
        while (check(p, TK_STRING)) {                           /* C: "a" "b" is "ab" (adjacent literals join) */
            size_t a = strlen(n->as.str), b = strlen(p->cur.str);
            char *j = kc_malloc(a + b + 1);
            memcpy(j, n->as.str, a); memcpy(j + a, p->cur.str, b + 1);
            n->as.str = j;
            advance(p);
        }
        return n;
    }
    if (match(p, TK_TRUE)) { KcNode *n = kc_node_new(N_BOOL, pos); n->as.boolean = true; return n; }
    if (match(p, TK_FALSE)) { KcNode *n = kc_node_new(N_BOOL, pos); n->as.boolean = false; return n; }
    if (match(p, TK_NULL)) { return kc_node_new(N_NULL, pos); }
    if (match(p, TK_LBRACKET)) {
        KcNode *n = kc_node_new(N_ARRAY, pos);
        if (!check(p, TK_RBRACKET)) {
            KcNode *first = parse_expr(p);
            if (check(p, TK_SEMI)) {                               /* [value; count] */
                advance(p);
                KcNode *r = kc_node_new(N_REPEATARR, pos);
                r->as.index.obj = first;
                r->as.index.index = parse_expr(p);
                expect(p, TK_RBRACKET, "]");
                return r;
            }
            kc_list_push(&n->as.array.items, first);
            while (match(p, TK_COMMA)) {
                if (check(p, TK_RBRACKET)) break;
                kc_list_push(&n->as.array.items, parse_expr(p));
            }
        }
        expect(p, TK_RBRACKET, "]");
        return n;
    }
    if (match(p, TK_FN)) {
        KcNode *n = kc_node_new(N_FN_EXPR, pos);
        n->as.fn.is_closure = true;
        expect(p, TK_LPAREN, "(");
    parse_params(p, n);
        if (match(p, TK_ARROW)) n->as.fn.ret_type = parse_type_str(p);
        else n->as.fn.ret_type = kc_strdup("void");
        { KcNode *saved = p->cur_fn; p->cur_fn = n; n->as.fn.body = parse_block(p); p->cur_fn = saved; }
        return n;
    }
    if (match(p, TK_UNSAFE)) {
        KcNode *n = kc_node_new(N_UNSAFE, pos);
        n->as.unsafe_.body = parse_block(p);
        return n;
    }
    if (match(p, TK_IDENT)) {
        char *name = tok_str(p->prev);
        if (check(p, TK_LPAREN) && (strcmp(name, "sizeof") == 0 || strcmp(name, "alignof") == 0 || strcmp(name, "offsetof") == 0)) {
            /* sizeof(T) / alignof(T) / offsetof(Struct, field) — contextual,
               so a user variable or function named `sizeof` still works
               whenever it isn't called like this */
            KcNode *n = kc_node_new(N_SIZEOF, pos);
            n->as.sizeof_.which = name[0] == 's' ? 0 : name[0] == 'a' ? 1 : 2;
            advance(p); /* '(' */
            if (n->as.sizeof_.which == 2) {
                n->as.sizeof_.type_name = tok_str(p->cur);
                expect(p, TK_IDENT, "struct name");
                expect(p, TK_COMMA, ",");
                n->as.sizeof_.field = tok_str(p->cur);
                expect(p, TK_IDENT, "field name");
            } else {
                n->as.sizeof_.type_name = parse_type_str(p);
            }
            expect(p, TK_RPAREN, ")");
            return n;
        }
        if (check(p, TK_LBRACE)) {
            /* struct literal: TypeName { field: expr, ... } — only an
               identifier directly followed by '{' triggers this, so it
               never collides with `if (cond) { }` / `while (cond) { }`
               (those always have a ')' between the condition and the
               brace) or with a fresh block statement (which parse_stmt
               recognizes before ever calling into expression parsing). */
            KcNode *n = kc_node_new(N_STRUCT_LITERAL, pos);
            n->as.structlit.type_name = name;
            advance(p); /* consume '{' */
            if (!check(p, TK_RBRACE)) {
                do {
                    KcNode *fname = kc_node_new(N_IDENT, p->cur.pos);
                    fname->as.ident.name = tok_str(p->cur);
                    expect(p, TK_IDENT, "field name");
                    expect(p, TK_COLON, ":");
                    KcNode *fval = parse_expr(p);
                    kc_list_push(&n->as.structlit.field_names, fname);
                    kc_list_push(&n->as.structlit.field_values, fval);
                } while (match(p, TK_COMMA));
            }
            expect(p, TK_RBRACE, "}");
            return n;
        }
        KcNode *n = kc_node_new(N_IDENT, pos);
        n->as.ident.name = name;
        return n;
    }
    if (match(p, TK_LPAREN)) {
        KcNode *e = parse_expr(p);
        while (check(p, TK_COMMA)) {                              /* C comma operator: (a, b) yields b */
            KcPos cp = p->cur.pos;
            advance(p);
            KcNode *c = kc_node_new(N_COMMA, cp);
            c->as.binop.lhs = e; c->as.binop.rhs = parse_expr(p);
            e = c;
        }
        expect(p, TK_RPAREN, ")");
        return e;
    }
    if (p->cur.kind == TK_EOF) kc_error_at(pos, "unexpected end of input: an expression is incomplete");
    kc_error_at(pos, "unexpected token '%.*s' in expression", p->cur.len, p->cur.start);
    return NULL;
}

static KcNode *parse_postfix(Parser *p) {
    KcNode *e = parse_primary(p);
    for (;;) {
        if (match(p, TK_LPAREN)) {
            e = finish_call(p, e);
        } else if (match(p, TK_DOT)) {
            KcNode *n = kc_node_new(N_MEMBER, p->prev.pos);
            n->as.member.obj = e;
            n->as.member.name = tok_str(p->cur);
            expect(p, TK_IDENT, "member name");
            e = n;
        } else if (match(p, TK_ARROW)) {
            /* p->field : field of the struct a pointer points at */
            KcNode *n = kc_node_new(N_ARROW, p->prev.pos);
            n->as.member.obj = e;
            n->as.member.name = tok_str(p->cur);
            expect(p, TK_IDENT, "member name");
            e = n;
        } else if (match(p, TK_LBRACKET)) {
            KcNode *n = kc_node_new(N_INDEX, p->prev.pos);
            n->as.index.obj = e;
            KcNode *lo = NULL;
            if (!check(p, TK_DOTDOT)) lo = parse_expr(p);
            if (check(p, TK_DOTDOT)) {              /* a[lo..hi], a[..hi], a[lo..], a[..] : a slice */
                KcPos rp = p->cur.pos;
                advance(p);
                KcNode *hi = NULL;
                if (!check(p, TK_RBRACKET)) hi = parse_expr(p);
                KcNode *r = kc_node_new(N_RANGE, rp);
                r->as.range.lo = lo; r->as.range.hi = hi;
                n->as.index.index = r;
                n->kind = N_SLICE;
            } else n->as.index.index = lo;
            expect(p, TK_RBRACKET, "]");
            e = n;
        } else if (check(p, TK_INCR) || check(p, TK_DECR)) {      /* x++  x-- */
            bool inc = check(p, TK_INCR);
            KcNode *n = kc_node_new(N_INCDEC, p->cur.pos);
            advance(p);
            n->flags |= NF_POSTFIX;
            n->as.assign.target = e;
            n->as.assign.op = inc ? OP_ADD : OP_SUB;
            n->as.assign.value = kc_node_new(N_INT, n->pos); n->as.assign.value->ival = 1; n->as.assign.value->as.num = 1;
            e = n;
        } else break;
    }
    return e;
}

static KcNode *parse_unary(Parser *p) {
    KcPos pos = p->cur.pos;
    if (check(p, TK_INCR) || check(p, TK_DECR)) {                 /* ++x  --x */
        bool inc = check(p, TK_INCR);
        advance(p);
        KcNode *n = kc_node_new(N_INCDEC, pos);
        n->as.assign.target = parse_unary(p);
        n->as.assign.op = inc ? OP_ADD : OP_SUB;
        n->as.assign.value = kc_node_new(N_INT, pos); n->as.assign.value->ival = 1; n->as.assign.value->as.num = 1;
        return n;
    }
    if (match(p, TK_MINUS)) {
        if (check(p, TK_INT) && p->cur.imin) {            /* -9223372036854775808 */
            KcNode *n = kc_node_new(N_INT, pos); n->ival = INT64_MIN; n->as.num = (double)INT64_MIN; advance(p); return n;
        }
        KcNode *n = kc_node_new(N_UNOP, pos); n->as.unop.op = OP_NEG; n->as.unop.rhs = parse_unary(p); return n;
    }
    if (match(p, TK_NOT)) {
        KcNode *n = kc_node_new(N_UNOP, pos); n->as.unop.op = OP_NOT; n->as.unop.rhs = parse_unary(p); return n;
    }
    if (match(p, TK_TILDE)) {
        KcNode *n = kc_node_new(N_UNOP, pos); n->as.unop.op = OP_BNOT; n->as.unop.rhs = parse_unary(p); return n;
    }
    if (match(p, TK_AMP)) {   /* &place : address-of */
        KcNode *n = kc_node_new(N_ADDR, pos); n->as.unop.rhs = parse_unary(p); return n;
    }
    if (match(p, TK_STAR)) {  /* *ptr : dereference */
        KcNode *n = kc_node_new(N_DEREF, pos); n->as.unop.rhs = parse_unary(p); return n;
    }
    return parse_postfix(p);
}

/* `expr as Type` — binds tighter than * / % and looser than unary ops,
   so `-x as u8` is `(-x) as u8` and `*p as u32` is `(*p) as u32`. */
static KcNode *parse_cast(Parser *p) {
    KcNode *e = parse_unary(p);
    while (check(p, TK_AS)) {
        KcPos pos = p->cur.pos;
        advance(p);
        KcNode *n = kc_node_new(N_CAST, pos);
        n->as.cast.expr = e;
        n->as.cast.type_name = parse_type_str(p);
        e = n;
    }
    return e;
}

static KcNode *mkbin(KcNode *l, KcOp op, KcNode *r, KcPos pos) {
    KcNode *n = kc_node_new(N_BINOP, pos);
    n->as.binop.lhs = l; n->as.binop.op = op; n->as.binop.rhs = r;
    return n;
}

static KcNode *parse_mul(Parser *p) {
    KcNode *e = parse_cast(p);
    for (;;) {
        KcPos pos = p->cur.pos;
        if (match(p, TK_STAR)) e = mkbin(e, OP_MUL, parse_cast(p), pos);
        else if (match(p, TK_SLASH)) e = mkbin(e, OP_DIV, parse_cast(p), pos);
        else if (match(p, TK_PERCENT)) e = mkbin(e, OP_MOD, parse_cast(p), pos);
        else break;
    }
    return e;
}

static KcNode *parse_add(Parser *p) {
    KcNode *e = parse_mul(p);
    for (;;) {
        KcPos pos = p->cur.pos;
        if (match(p, TK_PLUS)) e = mkbin(e, OP_ADD, parse_mul(p), pos);
        else if (match(p, TK_MINUS)) e = mkbin(e, OP_SUB, parse_mul(p), pos);
        else break;
    }
    return e;
}

/* Bitwise precedence follows Rust, not C: shifts bind tighter than `&`,
   then `^`, then `|`, and all of them bind TIGHTER than comparisons, so
   `flags & MASK == 0` means `(flags & MASK) == 0` (C's famous trap). */
static KcNode *parse_shift(Parser *p) {
    KcNode *e = parse_add(p);
    for (;;) {
        KcPos pos = p->cur.pos;
        if (match(p, TK_SHL)) e = mkbin(e, OP_SHL, parse_add(p), pos);
        else if (match(p, TK_SHR)) e = mkbin(e, OP_SHR, parse_add(p), pos);
        else if (match(p, TK_USHR)) e = mkbin(e, OP_USHR, parse_add(p), pos);
        else break;
    }
    return e;
}
static KcNode *parse_bitand(Parser *p) {
    KcNode *e = parse_shift(p);
    while (check(p, TK_AMP)) { KcPos pos = p->cur.pos; advance(p); e = mkbin(e, OP_BAND, parse_shift(p), pos); }
    return e;
}
static KcNode *parse_bitxor(Parser *p) {
    KcNode *e = parse_bitand(p);
    while (check(p, TK_CARET)) { KcPos pos = p->cur.pos; advance(p); e = mkbin(e, OP_BXOR, parse_bitand(p), pos); }
    return e;
}
static KcNode *parse_bitor(Parser *p) {
    KcNode *e = parse_bitxor(p);
    while (check(p, TK_PIPE)) { KcPos pos = p->cur.pos; advance(p); e = mkbin(e, OP_BOR, parse_bitxor(p), pos); }
    return e;
}

static KcNode *parse_cmp(Parser *p) {
    KcNode *e = parse_bitor(p);
    for (;;) {
        KcPos pos = p->cur.pos;
        if (match(p, TK_LT)) e = mkbin(e, OP_LT, parse_bitor(p), pos);
        else if (match(p, TK_LE)) e = mkbin(e, OP_LE, parse_bitor(p), pos);
        else if (match(p, TK_GT)) e = mkbin(e, OP_GT, parse_bitor(p), pos);
        else if (match(p, TK_GE)) e = mkbin(e, OP_GE, parse_bitor(p), pos);
        else if (match(p, TK_IN)) e = mkbin(e, OP_IN, parse_bitor(p), pos);
        else break;
    }
    return e;
}

static KcNode *parse_eq(Parser *p) {
    KcNode *e = parse_cmp(p);
    for (;;) {
        KcPos pos = p->cur.pos;
        if (match(p, TK_EQ)) e = mkbin(e, OP_EQ, parse_cmp(p), pos);
        else if (match(p, TK_NEQ)) e = mkbin(e, OP_NEQ, parse_cmp(p), pos);
        else break;
    }
    return e;
}

static KcNode *parse_and(Parser *p) {
    KcNode *e = parse_eq(p);
    while (check(p, TK_AND)) { KcPos pos = p->cur.pos; advance(p); e = mkbin(e, OP_AND, parse_eq(p), pos); }
    return e;
}

static KcNode *parse_or(Parser *p) {
    KcNode *e = parse_and(p);
    while (check(p, TK_OR)) { KcPos pos = p->cur.pos; advance(p); e = mkbin(e, OP_OR, parse_and(p), pos); }
    return e;
}

/* a ?? b  (null-coalescing, looser than ||) and  c ? a : b  (right-associative, loosest of the
 * non-assignment operators) */
static KcNode *parse_coalesce(Parser *p) {
    KcNode *e = parse_or(p);
    while (check(p, TK_QQ)) { KcPos pos = p->cur.pos; advance(p); e = mkbin(e, OP_COALESCE, parse_or(p), pos); }
    return e;
}

static KcNode *parse_ternary(Parser *p) {
    KcNode *c = parse_coalesce(p);
    if (check(p, TK_QUESTION)) {
        KcPos pos = p->cur.pos;
        advance(p);
        KcNode *n = kc_node_new(N_TERNARY, pos);
        n->as.ternary.cond = c;
        n->as.ternary.a = parse_ternary(p);
        expect(p, TK_COLON, ": (the else-part of ?:)");
        n->as.ternary.b = parse_ternary(p);
        return n;
    }
    return c;
}

static KcNode *parse_assign(Parser *p) {
    KcNode *e = parse_ternary(p);
    KcPos pos = p->cur.pos;
    if (match(p, TK_ASSIGN)) {
        KcNode *n = kc_node_new(N_ASSIGN, pos);
        n->as.assign.target = e; n->as.assign.op = -1; n->as.assign.value = parse_assign(p);
        return n;
    }
    if (match(p, TK_PLUSEQ)) {
        KcNode *n = kc_node_new(N_ASSIGN, pos);
        n->as.assign.target = e; n->as.assign.op = OP_ADD; n->as.assign.value = parse_assign(p);
        return n;
    }
    if (match(p, TK_MINUSEQ)) {
        KcNode *n = kc_node_new(N_ASSIGN, pos);
        n->as.assign.target = e; n->as.assign.op = OP_SUB; n->as.assign.value = parse_assign(p);
        return n;
    }
    {
        static const struct { KcTokKind t; KcOp op; } CA[] = {
            {TK_AMPEQ, OP_BAND}, {TK_PIPEEQ, OP_BOR}, {TK_CARETEQ, OP_BXOR}, {TK_SHLEQ, OP_SHL}, {TK_SHREQ, OP_SHR},
            {TK_STAREQ, OP_MUL}, {TK_SLASHEQ, OP_DIV}, {TK_PERCENTEQ, OP_MOD},
        };
        for (size_t i = 0; i < sizeof CA / sizeof CA[0]; i++) {
            if (match(p, CA[i].t)) {
                KcNode *n = kc_node_new(N_ASSIGN, pos);
                n->as.assign.target = e; n->as.assign.op = CA[i].op; n->as.assign.value = parse_assign(p);
                return n;
            }
        }
    }
    return e;
}

static KcNode *parse_expr(Parser *p) { return parse_assign(p); }

/* ---------------- statements ---------------- */

static KcNode *parse_block(Parser *p) {
    KcPos pos = p->cur.pos;
    expect(p, TK_LBRACE, "{");
    KcNode *n = kc_node_new(N_BLOCK, pos);
    while (!check(p, TK_RBRACE) && !check(p, TK_EOF)) {
        kc_list_push(&n->as.block.stmts, parse_stmt(p));
    }
    expect(p, TK_RBRACE, "}");
    return n;
}

/* A loop / if body: a `{ block }`, or (C) a single statement. */
static KcNode *parse_body(Parser *p) {
    if (check(p, TK_LBRACE)) return parse_block(p);
    KcNode *b = kc_node_new(N_BLOCK, p->cur.pos);
    kc_list_push(&b->as.block.stmts, parse_stmt(p));
    return b;
}

static KcNode *parse_destructure(Parser *p, bool mutable, KcPos pos) {
    /* let [a, _, ...rest] = e;      let {x, y: renamed} = e; */
    KcNode *n = kc_node_new(N_DESTRUCT, pos);
    n->as.destruct.is_mutable = mutable;
    bool arr = check(p, TK_LBRACKET);
    advance(p);
    KcTokKind close = arr ? TK_RBRACKET : TK_RBRACE;
    while (!check(p, close) && !check(p, TK_EOF)) {
        if (arr && match(p, TK_ELLIPSIS)) {
            n->as.destruct.rest = tok_str(p->cur);
            expect(p, TK_IDENT, "name for the rest of the array");
            break;
        }
        KcNode *var = kc_node_new(N_IDENT, p->cur.pos);
        char *first = tok_str(p->cur);
        expect(p, TK_IDENT, arr ? "name" : "field name");
        var->as.ident.name = first;
        if (!arr) {
            KcNode *field = kc_node_new(N_STRING, var->pos); field->as.str = kc_strdup(first);
            if (match(p, TK_COLON)) { var->as.ident.name = tok_str(p->cur); expect(p, TK_IDENT, "variable name"); }
            kc_list_push(&n->as.destruct.fields, field);
        }
        kc_list_push(&n->as.destruct.names, var);
        if (!match(p, TK_COMMA)) break;
    }
    expect(p, close, arr ? "]" : "}");
    n->as.destruct.is_obj = !arr;
    expect(p, TK_ASSIGN, "= (a destructuring let needs a value)");
    n->as.destruct.init = parse_expr(p);
    expect(p, TK_SEMI, ";");
    return n;
}

static KcNode *parse_let_ex(Parser *p, bool mutable, bool is_const) {
    KcPos pos = p->cur.pos;
    if (is_const) advance(p); else expect(p, mutable ? TK_VAR : TK_LET, "let/var");
    if (check(p, TK_LBRACKET) || check(p, TK_LBRACE)) return parse_destructure(p, mutable, pos);
    KcNodeList decls = {0};
    parse_decl_list(p, mutable, &decls);                 /* let a = 1, b = 2;  (C: several declarators) */
    expect(p, TK_SEMI, ";");
    if (decls.count == 1) return decls.items[0];
    KcNode *multi = kc_node_new(N_MULTI, pos);
    multi->as.block.stmts = decls;
    return multi;
}

static KcNode *parse_let(Parser *p, bool mutable) { return parse_let_ex(p, mutable, false); }

static KcNode *parse_fn_decl(Parser *p) {
    KcPos pos = p->cur.pos;
    const char *doc = p->cur.doc;
    expect(p, TK_FN, "fn");
    KcNode *n = kc_node_new(N_FN_DECL, pos);
    n->as.fn.doc = doc;
    n->as.fn.name = tok_str(p->cur);
    expect(p, TK_IDENT, "function name");
    char *tparams[8]; int ntp = 0;
    if (match(p, TK_LT)) {                       /* fn id<T, U>(x: T) -> T : type parameters */
        do {
            if (ntp == 8) kc_error_at(p->cur.pos, "too many type parameters (max 8)");
            tparams[ntp++] = tok_str(p->cur);
            expect(p, TK_IDENT, "type parameter name");
        } while (match(p, TK_COMMA));
        expect(p, TK_GT, ">");
    }
    expect(p, TK_LPAREN, "(");
    parse_params(p, n);
    if (match(p, TK_ARROW)) n->as.fn.ret_type = parse_type_str(p);
    else n->as.fn.ret_type = kc_strdup("void");
    if (ntp) {
        for (int i = 0; i < n->as.fn.params.count; i++) n->as.fn.param_types[i] = erase_tparam(n->as.fn.param_types[i], tparams, ntp);
        n->as.fn.ret_type = erase_tparam(n->as.fn.ret_type, tparams, ntp);
    }
    { KcNode *saved = p->cur_fn; p->cur_fn = n; n->as.fn.body = parse_block(p); p->cur_fn = saved; }
    return n;
}

static KcNode *parse_struct_decl(Parser *p) {
    KcPos pos = p->cur.pos;
    bool is_union = false;
    const char *sdoc = p->cur.doc;
    if (check(p, TK_IDENT) && p->cur.len == 5 && memcmp(p->cur.start, "union", 5) == 0) { advance(p); is_union = true; }
    else expect(p, TK_STRUCT, "struct");
    KcNode *n = kc_node_new(N_STRUCT_DECL, pos);
    n->as.structdecl.is_union = is_union;
    n->as.structdecl.doc = sdoc;
    n->as.structdecl.name = tok_str(p->cur);
    expect(p, TK_IDENT, "struct name");
    expect(p, TK_LBRACE, "{");
    int cap = 4, cnt = 0;
    char **ftypes = kc_malloc(sizeof(char *) * cap);
    while (!check(p, TK_RBRACE) && !check(p, TK_EOF)) {
        KcNode *field = kc_node_new(N_IDENT, p->cur.pos);
        field->as.ident.name = tok_str(p->cur);
        expect(p, TK_IDENT, "field name");
        expect(p, TK_COLON, ":");
        char *ty = parse_type_str(p); /* retained: used by the static type checker */
        if (cnt + 1 > cap) { cap *= 2; ftypes = kc_realloc(ftypes, sizeof(char *) * cap); }
        ftypes[cnt++] = ty;
        match(p, TK_COMMA);
        kc_list_push(&n->as.structdecl.fields, field);
    }
    expect(p, TK_RBRACE, "}");
    n->as.structdecl.field_types = ftypes;
    return n;
}

static KcNode *parse_import(Parser *p) {
    KcPos pos = p->cur.pos;
    expect(p, TK_IMPORT, "import");
    KcNode *n = kc_node_new(N_IMPORT, pos);
    if (check(p, TK_STRING)) {
        /* import "path/to/file.kc";  — a user source file, not a std module */
        n->as.import_.is_file = true;
        n->as.import_.module = kc_strdup(p->cur.str);
        advance(p);
    } else {
        /* import std.system;  or  import std.system as sys; */
        n->as.import_.is_file = false;
        char buf[256]; buf[0] = 0;
        strcat(buf, tok_str(p->cur));
        expect(p, TK_IDENT, "module path");
        while (match(p, TK_DOT)) {
            strcat(buf, ".");
            strcat(buf, tok_str(p->cur));
            expect(p, TK_IDENT, "module path");
        }
        n->as.import_.module = kc_strdup(buf);
    }
    if (match(p, TK_AS)) {
        n->as.import_.alias = tok_str(p->cur);
        expect(p, TK_IDENT, "alias");
    }
    expect(p, TK_SEMI, ";");
    return n;
}

static KcNode *parse_extern_decl(Parser *p) {
    KcPos pos = p->cur.pos;
    expect(p, TK_EXTERN, "extern");
    KcNode *n = kc_node_new(N_EXTERN_DECL, pos);
    expect(p, TK_FN, "fn");
    n->as.externdecl.name = tok_str(p->cur);
    n->as.externdecl.real_symbol = kc_strdup(n->as.externdecl.name);
    expect(p, TK_IDENT, "function name");
    expect(p, TK_LPAREN, "(");
    if (!check(p, TK_RPAREN)) {
        do {
            if (check(p, TK_IDENT)) advance(p); /* optional param name */
            if (match(p, TK_COLON)) {
                KcNode *ty = kc_node_new(N_IDENT, p->cur.pos);
                ty->as.ident.name = parse_type_str(p);
                kc_list_push(&n->as.externdecl.param_types, ty);
            }
        } while (match(p, TK_COMMA));
    }
    expect(p, TK_RPAREN, ")");
    if (match(p, TK_ARROW)) n->as.externdecl.ret_type = parse_type_str(p);
    else n->as.externdecl.ret_type = kc_strdup("void");
    if (match(p, TK_FROM)) {
        n->as.externdecl.from_lib = p->cur.str ? kc_strdup(p->cur.str) : tok_str(p->cur);
        expect(p, TK_STRING, "library path string");
    }
    expect(p, TK_SEMI, ";");
    return n;
}

/* ---- contextual keywords (type, enum, impl, const, try, throw) ----
 * They are ordinary identifiers everywhere except where one of these exact shapes follows, so
 * existing code that names a variable `type` or `try` keeps working. */
static bool is_word(Parser *p, const char *w) {
    size_t n = strlen(w);
    return check(p, TK_IDENT) && (size_t)p->cur.len == n && memcmp(p->cur.start, w, n) == 0;
}
static KcTokKind peek_kind(Parser *p, int ahead) {
    KcLexer lx = p->lx;
    KcTokKind k = TK_EOF;
    for (int i = 0; i < ahead; i++) k = kc_lexer_next(&lx).kind;
    return k;
}

/* enum Name { A, B = 5, C }  ->  Name.A == 0, Name.B == 5, Name.C == 6 */
static KcNode *parse_enum(Parser *p) {
    KcPos pos = p->cur.pos;
    advance(p);                                         /* enum */
    KcNode *n = kc_node_new(N_ENUM_DECL, pos);
    n->as.enumdecl.name = tok_str(p->cur);
    expect(p, TK_IDENT, "enum name");
    expect(p, TK_LBRACE, "{");
    while (!check(p, TK_RBRACE) && !check(p, TK_EOF)) {
        KcNode *id = kc_node_new(N_IDENT, p->cur.pos);
        id->as.ident.name = tok_str(p->cur);
        expect(p, TK_IDENT, "enum member name");
        kc_list_push(&n->as.enumdecl.names, id);
        kc_list_push(&n->as.enumdecl.values, match(p, TK_ASSIGN) ? parse_expr(p) : NULL);
        if (!match(p, TK_COMMA)) break;
    }
    expect(p, TK_RBRACE, "}");
    if (n->as.enumdecl.names.count == 0) kc_error_at(pos, "an enum needs at least one member");
    match(p, TK_SEMI);
    return n;
}

/* impl Type { fn describe(self) -> string { ... }  fn make() -> Type { ... } }
 * A method whose first parameter is named `self` is called as value.describe(); any other
 * function is called on the type itself: Type.make(). Works for structs and for built-in types
 * (impl string { fn shout(self) ... }). */
static KcNode *parse_impl(Parser *p) {
    KcPos pos = p->cur.pos;
    advance(p);                                         /* impl */
    KcNode *n = kc_node_new(N_IMPL, pos);
    n->as.impl.type_name = tok_str(p->cur);
    expect(p, TK_IDENT, "type name");
    expect(p, TK_LBRACE, "{");
    while (!check(p, TK_RBRACE) && !check(p, TK_EOF)) {
        if (!check(p, TK_FN)) kc_error_at(p->cur.pos, "only functions may appear inside an impl block");
        kc_list_push(&n->as.impl.methods, parse_fn_decl(p));
    }
    expect(p, TK_RBRACE, "}");
    match(p, TK_SEMI);
    return n;
}

/* try { ... } catch (e) { ... }     (the (e) is optional) */
static KcNode *parse_try(Parser *p) {
    KcPos pos = p->cur.pos;
    advance(p);                                         /* try */
    KcNode *n = kc_node_new(N_TRY, pos);
    n->as.trystmt.body = parse_block(p);
    if (!is_word(p, "catch")) kc_error_at(p->cur.pos, "'try' needs a 'catch' block");
    advance(p);
    if (match(p, TK_LPAREN)) {
        n->as.trystmt.catch_var = tok_str(p->cur);
        expect(p, TK_IDENT, "error variable name");
        expect(p, TK_RPAREN, ")");
    }
    n->as.trystmt.catch_body = parse_block(p);
    match(p, TK_SEMI);
    return n;
}

/* ---- v0.8 statement helpers ---- */
static bool at_case_label(Parser *p) {
    return is_word(p, "case") || (is_word(p, "default") && peek_kind(p, 1) == TK_COLON);
}

/* name [: type] [= init] {, name ...}   -- declarations without the terminating ';' (for headers, auto) */
static void parse_decl_list(Parser *p, bool mutable, KcNodeList *out) {
    do {
        KcNode *n = kc_node_new(N_LET, p->cur.pos);
        n->as.let.is_mutable = mutable;
        n->as.let.name = tok_str(p->cur);
        expect(p, TK_IDENT, "identifier");
        if (match(p, TK_COLON)) n->as.let.type_name = parse_type_str(p);
        if (match(p, TK_ASSIGN)) n->as.let.init = parse_expr(p);
        kc_list_push(out, n);
    } while (match(p, TK_COMMA));
}

static KcNode *exprstmt_of(KcNode *e) {
    KcNode *es = kc_node_new(N_EXPRSTMT, e->pos);
    es->as.exprstmt.expr = e;
    return es;
}

/* B:  auto a, b, v[10];   -- v is a vector of 11 words (indices 0..10), as in B */
static KcNode *parse_auto(Parser *p) {
    KcPos pos = p->cur.pos;
    advance(p);                                         /* auto */
    KcNode *multi = kc_node_new(N_MULTI, pos);
    do {
        KcNode *n = kc_node_new(N_LET, p->cur.pos);
        n->as.let.is_mutable = true;
        n->as.let.name = tok_str(p->cur);
        expect(p, TK_IDENT, "identifier");
        if (match(p, TK_LBRACKET)) {
            KcNode *callee = kc_node_new(N_IDENT, n->pos); callee->as.ident.name = kc_strdup("__vec");
            KcNode *call = kc_node_new(N_CALL, n->pos);
            call->as.call.callee = callee;
            kc_list_push(&call->as.call.args, parse_expr(p));
            expect(p, TK_RBRACKET, "]");
            n->as.let.init = call;
        } else if (match(p, TK_COLON)) {
            n->as.let.type_name = parse_type_str(p);
            if (match(p, TK_ASSIGN)) n->as.let.init = parse_expr(p);
        } else if (match(p, TK_ASSIGN)) n->as.let.init = parse_expr(p);
        kc_list_push(&multi->as.block.stmts, n);
    } while (match(p, TK_COMMA));
    expect(p, TK_SEMI, ";");
    if (multi->as.block.stmts.count == 1) return multi->as.block.stmts.items[0];
    return multi;
}

/* B:  extrn a, b;   (names that come from outside the function: checked when the statement runs) */
static KcNode *parse_extrn(Parser *p) {
    KcPos pos = p->cur.pos;
    advance(p);                                         /* extrn */
    KcNode *n = kc_node_new(N_EXTRN, pos);
    do {
        KcNode *id = kc_node_new(N_IDENT, p->cur.pos);
        id->as.ident.name = tok_str(p->cur);
        expect(p, TK_IDENT, "identifier");
        kc_list_push(&n->as.array.items, id);
    } while (match(p, TK_COMMA));
    expect(p, TK_SEMI, ";");
    return n;
}

/* switch (x) { case 1, 2: ... default: ... }   -- C semantics: execution falls through to the next
 * case until a `break`. */
static KcNode *parse_switch(Parser *p) {
    KcPos pos = p->cur.pos;
    advance(p);                                         /* switch */
    KcNode *n = kc_node_new(N_SWITCH, pos);
    expect(p, TK_LPAREN, "(");
    n->as.switchstmt.subject = parse_expr(p);
    expect(p, TK_RPAREN, ")");
    expect(p, TK_LBRACE, "{");
    bool seen_default = false;
    while (!check(p, TK_RBRACE) && !check(p, TK_EOF)) {
        KcNode *c = kc_node_new(N_CASE, p->cur.pos);
        if (is_word(p, "case")) {
            advance(p);
            do { kc_list_push(&c->as.casenode.values, parse_ternary(p)); } while (match(p, TK_COMMA));
            expect(p, TK_COLON, ":");
        } else if (is_word(p, "default") && peek_kind(p, 1) == TK_COLON) {
            if (seen_default) kc_error_at(p->cur.pos, "a switch can have only one `default`");
            seen_default = true;
            advance(p); advance(p);
            c->as.casenode.is_default = true;
        } else {
            kc_error_at(p->cur.pos, "expected `case` or `default` inside a switch, found '%.*s'", p->cur.len, p->cur.start);
        }
        KcNode *body = kc_node_new(N_BLOCK, c->pos);
        while (!check(p, TK_RBRACE) && !check(p, TK_EOF) && !at_case_label(p))
            kc_list_push(&body->as.block.stmts, parse_stmt(p));
        c->as.casenode.body = body;
        kc_list_push(&n->as.switchstmt.cases, c);
    }
    expect(p, TK_RBRACE, "}");
    match(p, TK_SEMI);
    return n;
}

/* B-style function definition:  name(a, b) { ... }   (no `fn`, untyped parameters) */
static bool looks_like_b_fn(Parser *p) {
    if (!check(p, TK_IDENT) || peek_kind(p, 1) != TK_LPAREN) return false;
    KcLexer lx = p->lx;
    KcToken t = kc_lexer_next(&lx);                     /* ( */
    t = kc_lexer_next(&lx);
    if (t.kind != TK_RPAREN) {
        for (;;) {
            if (t.kind != TK_IDENT) return false;
            t = kc_lexer_next(&lx);
            if (t.kind == TK_COMMA) { t = kc_lexer_next(&lx); continue; }
            if (t.kind == TK_RPAREN) break;
            return false;
        }
    }
    return kc_lexer_next(&lx).kind == TK_LBRACE;
}

static KcNode *parse_b_fn(Parser *p) {
    KcPos pos = p->cur.pos;
    KcNode *n = kc_node_new(N_FN_DECL, pos);
    n->as.fn.doc = p->cur.doc;
    n->as.fn.name = tok_str(p->cur);
    advance(p);
    expect(p, TK_LPAREN, "(");
    parse_params(p, n);
    n->as.fn.ret_type = kc_strdup("any");             /* B is typeless: a function may return anything (or nothing) */
    { KcNode *saved = p->cur_fn; p->cur_fn = n; n->as.fn.body = parse_block(p); p->cur_fn = saved; }
    return n;
}

static KcNode *parse_stmt_core(Parser *p, const char *label);

/* A statement, possibly preceded by a label:  name: stmt.  A label before a loop is also the
 * target of `break name;` / `continue name;`; every label is a `goto` target. */
static KcNode *parse_stmt(Parser *p) {
    if (check(p, TK_IDENT) && peek_kind(p, 1) == TK_COLON && !at_case_label(p)) {
        KcPos pos = p->cur.pos;
        const char *label = kc_intern(tok_str(p->cur));
        advance(p); advance(p);
        KcNode *ln = kc_node_new(N_LABEL, pos);
        ln->label = label;
        if (check(p, TK_RBRACE) || check(p, TK_EOF) || at_case_label(p)) ln->as.unop.rhs = kc_node_new(N_BLOCK, pos);
        else ln->as.unop.rhs = parse_stmt_core(p, label);
        return ln;
    }
    return parse_stmt_core(p, NULL);
}

static KcNode *parse_stmt_core(Parser *p, const char *label) {
    KcPos pos = p->cur.pos;
    if (is_word(p, "static")) {                         /* static n = 0;  static let n = 0;  static fn f() {} */
        KcTokKind nk = peek_kind(p, 1);
        if (nk == TK_IDENT && (peek_kind(p, 2) == TK_ASSIGN || peek_kind(p, 2) == TK_SEMI || peek_kind(p, 2) == TK_COLON)) {
            advance(p);
            KcNode *n = kc_node_new(N_LET, pos);
            n->as.let.is_mutable = true; n->as.let.is_static = true;
            if (p->cur_fn) p->cur_fn->flags |= NF_HAS_STATIC;
            n->as.let.name = tok_str(p->cur); advance(p);
            if (match(p, TK_COLON)) n->as.let.type_name = parse_type_str(p);
            if (match(p, TK_ASSIGN)) n->as.let.init = parse_expr(p);
            expect(p, TK_SEMI, ";");
            return n;
        }
        if (nk == TK_LET || nk == TK_VAR || nk == TK_FN || nk == TK_STRUCT || nk == TK_IDENT) {
            advance(p);
            KcNode *d = parse_stmt_core(p, NULL);
            if (d->kind == N_LET) { d->as.let.is_static = true; if (p->cur_fn) p->cur_fn->flags |= NF_HAS_STATIC; }
            return d;
        }
    }
    if (is_word(p, "auto") && peek_kind(p, 1) == TK_IDENT) return parse_auto(p);
    if (is_word(p, "extrn") && peek_kind(p, 1) == TK_IDENT) return parse_extrn(p);
    if (is_word(p, "switch") && peek_kind(p, 1) == TK_LPAREN) return parse_switch(p);
    if (is_word(p, "goto") && peek_kind(p, 1) == TK_IDENT) {
        advance(p);
        KcNode *n = kc_node_new(N_GOTO, pos);
        n->label = kc_intern(tok_str(p->cur));
        advance(p);
        expect(p, TK_SEMI, ";");
        return n;
    }
    if (is_word(p, "typedef") && (peek_kind(p, 1) == TK_IDENT || peek_kind(p, 1) == TK_STAR || peek_kind(p, 1) == TK_FN)) {
        advance(p);
        KcNode *n = kc_node_new(N_TYPE_ALIAS, pos);
        n->as.typealias.target = parse_type_str(p);
        n->as.typealias.name = tok_str(p->cur);
        expect(p, TK_IDENT, "new type name");
        expect(p, TK_SEMI, ";");
        if (p->naliases == 64) kc_error_at(pos, "too many type aliases (max 64)");
        p->alias_names[p->naliases] = n->as.typealias.name;
        p->alias_targets[p->naliases++] = n->as.typealias.target;
        return n;
    }
    if (is_word(p, "union") && peek_kind(p, 1) == TK_IDENT && peek_kind(p, 2) == TK_LBRACE) return parse_struct_decl(p);
    if (is_word(p, "do") && peek_kind(p, 1) == TK_LBRACE) {
        advance(p);
        KcNode *n = kc_node_new(N_DOWHILE, pos);
        n->label = label;
        if (label) { if (p->nlabels == 32) kc_error_at(pos, "loops are nested too deeply for labels (max 32)"); p->labels[p->nlabels++] = label; }
        n->as.whilestmt.body = parse_block(p);
        if (label) p->nlabels--;
        expect(p, TK_WHILE, "`while` after the body of a do-loop");
        expect(p, TK_LPAREN, "(");
        n->as.whilestmt.cond = parse_expr(p);
        expect(p, TK_RPAREN, ")");
        match(p, TK_SEMI);
        return n;
    }
    if (looks_like_b_fn(p)) return parse_b_fn(p);
    if (check(p, TK_SEMI)) { advance(p); return kc_node_new(N_BLOCK, pos); }      /* C: the empty statement */
    if (is_word(p, "const") && (peek_kind(p, 1) == TK_IDENT || peek_kind(p, 1) == TK_LBRACKET || peek_kind(p, 1) == TK_LBRACE))
        return parse_let_ex(p, false, true);
    if (is_word(p, "enum") && peek_kind(p, 1) == TK_IDENT) return parse_enum(p);
    if (is_word(p, "impl") && peek_kind(p, 1) == TK_IDENT) return parse_impl(p);
    if (is_word(p, "try") && peek_kind(p, 1) == TK_LBRACE) return parse_try(p);
    if (is_word(p, "type") && peek_kind(p, 1) == TK_IDENT && peek_kind(p, 2) == TK_ASSIGN) {
        advance(p);
        KcNode *n = kc_node_new(N_TYPE_ALIAS, pos);
        n->as.typealias.name = tok_str(p->cur);
        advance(p); advance(p);                         /* name, = */
        n->as.typealias.target = parse_type_str(p);
        expect(p, TK_SEMI, ";");
        if (p->naliases == 64) kc_error_at(pos, "too many type aliases (max 64)");
        p->alias_names[p->naliases] = n->as.typealias.name;
        p->alias_targets[p->naliases++] = n->as.typealias.target;
        return n;
    }
    if (is_word(p, "throw")) {
        KcTokKind nk = peek_kind(p, 1);
        if (nk == TK_IDENT || nk == TK_STRING || nk == TK_FSTRING || nk == TK_INT || nk == TK_FLOAT || nk == TK_LBRACE ||
            nk == TK_LBRACKET || nk == TK_LPAREN || nk == TK_MINUS || nk == TK_NOT || nk == TK_TRUE || nk == TK_FALSE ||
            nk == TK_NULL || nk == TK_FN || nk == TK_MATCH) {
            advance(p);
            KcNode *n = kc_node_new(N_THROW, pos);
            n->as.retstmt.value = parse_expr(p);
            expect(p, TK_SEMI, ";");
            return n;
        }
    }
    if (check(p, TK_LET)) return parse_let(p, false);
    if (check(p, TK_VAR)) return parse_let(p, true);
    if (check(p, TK_FN)) return parse_fn_decl(p);
    if (check(p, TK_STRUCT)) return parse_struct_decl(p);
    if (check(p, TK_IDENT) && p->cur.len == 6 && memcmp(p->cur.start, "packed", 6) == 0) {
        /* `packed struct Name { ... }` — contextual keyword: only when `struct` follows */
        KcLexer ahead = p->lx;
        if (kc_lexer_next(&ahead).kind == TK_STRUCT) {
            const char *pdoc = p->cur.doc;
            advance(p);
            KcNode *sd = parse_struct_decl(p);
            sd->as.structdecl.packed = true;
            if (pdoc) sd->as.structdecl.doc = pdoc;
            return sd;
        }
    }
    if (check(p, TK_IMPORT)) return parse_import(p);
    if (check(p, TK_EXTERN)) return parse_extern_decl(p);
    if (check(p, TK_LBRACE)) return parse_block(p);
    if (match(p, TK_IF)) {
        KcNode *n = kc_node_new(N_IF, pos);
        expect(p, TK_LPAREN, "(");
        n->as.ifstmt.cond = parse_expr(p);
        expect(p, TK_RPAREN, ")");
        n->as.ifstmt.then_branch = parse_body(p);
        if (match(p, TK_ELSE)) {
            if (check(p, TK_IF)) n->as.ifstmt.else_branch = parse_stmt(p);
            else n->as.ifstmt.else_branch = parse_body(p);
        }
        return n;
    }
    if (match(p, TK_WHILE)) {
        KcNode *n = kc_node_new(N_WHILE, pos);
        n->label = label;
        expect(p, TK_LPAREN, "(");
        n->as.whilestmt.cond = parse_expr(p);
        expect(p, TK_RPAREN, ")");
        if (label) { if (p->nlabels == 32) kc_error_at(pos, "loops are nested too deeply for labels (max 32)"); p->labels[p->nlabels++] = label; }
        n->as.whilestmt.body = parse_body(p);
        if (label) p->nlabels--;
        return n;
    }
    if (match(p, TK_FOR)) {
        expect(p, TK_LPAREN, "(");
        bool forin = false;
        if (check(p, TK_IDENT)) {
            KcTokKind k1 = peek_kind(p, 1);
            if (k1 == TK_IN) forin = true;
            else if (k1 == TK_COMMA && peek_kind(p, 2) == TK_IDENT && peek_kind(p, 3) == TK_IN) forin = true;
        }
        if (!forin) {                                    /* for (init; cond; step) body */
            KcNode *cf = kc_node_new(N_CFOR, pos);
            cf->label = label;
            if (!check(p, TK_SEMI)) {
                if (check(p, TK_LET) || check(p, TK_VAR) || (is_word(p, "auto") && peek_kind(p, 1) == TK_IDENT)) {
                    advance(p);
                    parse_decl_list(p, true, &cf->as.cfor.init);
                } else {
                    do { kc_list_push(&cf->as.cfor.init, exprstmt_of(parse_expr(p))); } while (match(p, TK_COMMA));
                }
            }
            expect(p, TK_SEMI, ";");
            if (!check(p, TK_SEMI)) cf->as.cfor.cond = parse_expr(p);
            expect(p, TK_SEMI, ";");
            if (!check(p, TK_RPAREN)) {
                do { kc_list_push(&cf->as.cfor.step, exprstmt_of(parse_expr(p))); } while (match(p, TK_COMMA));
            }
            expect(p, TK_RPAREN, ")");
            if (label) { if (p->nlabels == 32) kc_error_at(pos, "loops are nested too deeply for labels (max 32)"); p->labels[p->nlabels++] = label; }
            cf->as.cfor.body = parse_body(p);
            if (label) p->nlabels--;
            return cf;
        }
        KcNode *n = kc_node_new(N_FOR, pos);
        n->as.forstmt.var = tok_str(p->cur);
        expect(p, TK_IDENT, "loop variable");
        if (match(p, TK_COMMA)) {                   /* for (i, x in array)  /  for (key, value in object) */
            n->as.forstmt.var2 = tok_str(p->cur);
            expect(p, TK_IDENT, "second loop variable");
        }
        /* support "for i in 0..10" and "for x in arr" */
        expect(p, TK_IN, "'in'");
        KcNode *iter = parse_expr(p);
        if (match(p, TK_DOTDOT)) {
            KcNode *rng = kc_node_new(N_RANGE, iter->pos);
            rng->as.range.lo = iter;
            rng->as.range.hi = parse_expr(p);
            iter = rng;
        }
        n->as.forstmt.iterable = iter;
        n->label = label;
        expect(p, TK_RPAREN, ")");
        if (label) { if (p->nlabels == 32) kc_error_at(pos, "loops are nested too deeply for labels (max 32)"); p->labels[p->nlabels++] = label; }
        n->as.forstmt.body = parse_body(p);
        if (label) p->nlabels--;
        return n;
    }
    if (match(p, TK_RETURN)) {
        KcNode *n = kc_node_new(N_RETURN, pos);
        if (!check(p, TK_SEMI)) n->as.retstmt.value = parse_expr(p);
        expect(p, TK_SEMI, ";");
        return n;
    }
    if (check(p, TK_UNSAFE)) {
        /* `unsafe { ... }` used as a statement: no trailing ';' required,
           same as if/while/for/block statements. */
        advance(p);
        KcNode *n = kc_node_new(N_UNSAFE, pos);
        n->as.unsafe_.body = parse_block(p);
        match(p, TK_SEMI); /* allow but don't require one */
        KcNode *es = kc_node_new(N_EXPRSTMT, pos);
        es->as.exprstmt.expr = n;
        return es;
    }
    if (check(p, TK_COMPTIME)) {
        /* `comptime { ... }`: runs once, when execution reaches it, with
           side-effecting natives disallowed inside (see interp.c's
           kc_forbid_in_comptime). A real statement, not an expression —
           no trailing ';' required, same as unsafe/if/while/for. */
        advance(p);
        KcNode *n = kc_node_new(N_COMPTIME_BLOCK, pos);
        n->as.comptime_block.body = parse_block(p);
        match(p, TK_SEMI);
        return n;
    }
    if (check(p, TK_IDENT) && p->cur.len == 5 && memcmp(p->cur.start, "defer", 5) == 0) {
        /* `defer stmt;` / `defer { ... }` — contextual keyword. It is a defer statement only
           when what follows can start a statement (a name, `{`, a string or an int), which
           no expression continuation of a variable called `defer` could. */
        KcLexer ahead = p->lx;
        KcTokKind nk = kc_lexer_next(&ahead).kind;
        if (nk == TK_IDENT || nk == TK_LBRACE || nk == TK_STRING || nk == TK_INT) {
            advance(p);
            KcNode *n = kc_node_new(N_DEFER, pos);
            if (check(p, TK_LBRACE)) { n->as.unop.rhs = parse_block(p); match(p, TK_SEMI); }
            else {
                KcNode *es = kc_node_new(N_EXPRSTMT, pos);
                es->as.exprstmt.expr = parse_expr(p);
                expect(p, TK_SEMI, ";");
                n->as.unop.rhs = es;
            }
            return n;
        }
    }
    if (check(p, TK_BREAK) || check(p, TK_CONTINUE)) {
        bool is_break = check(p, TK_BREAK);
        advance(p);
        KcNode *n = kc_node_new(is_break ? N_BREAK : N_CONTINUE, pos);
        if (check(p, TK_IDENT)) {                           /* break outer;  continue outer; */
            const char *lab = kc_intern(tok_str(p->cur));
            bool known = false;
            for (int i = 0; i < p->nlabels; i++) if (p->labels[i] == lab) known = true;
            if (!known) kc_error_at(p->cur.pos, "unknown loop label '%s' (a label must name an enclosing loop)", lab);
            n->label = lab;
            advance(p);
        }
        expect(p, TK_SEMI, ";");
        return n;
    }

    KcNode *e = parse_expr(p);
    if (e->kind == N_MATCH) match(p, TK_SEMI);       /* a match statement ends with its '}' like if/while */
    else expect(p, TK_SEMI, ";");
    KcNode *n = kc_node_new(N_EXPRSTMT, pos);
    n->as.exprstmt.expr = e;
    return n;
}

/* REPL: `type`/`typedef` aliases outlive the input that declared them. They are copied into each fresh
 * Parser and back out after a successful parse, so a rejected input leaves no alias behind. */
static bool g_keep_aliases;
static const char *g_alias_names[64]; static char *g_alias_targets[64]; static int g_naliases;
void kc_parse_keep_aliases(bool on) { g_keep_aliases = on; }
void kc_parse_reset_aliases(void) { g_naliases = 0; }

KcNode *kc_parse_program(const char *src, const char *filename) { return kc_parse_program_pp(src, filename, NULL); }

KcNode *kc_parse_program_pp(const char *src, const char *filename, KcPP *pp) {
    src = kc_preprocess(src, filename, pp);     /* #define / #include / #if ... (a no-op for source without directives) */
    Parser p;
    memset(&p, 0, sizeof p);
    kc_lexer_init(&p.lx, src, filename);
    p.filename = filename;
    if (g_keep_aliases) {
        p.naliases = g_naliases;
        for (int i = 0; i < g_naliases; i++) { p.alias_names[i] = g_alias_names[i]; p.alias_targets[i] = g_alias_targets[i]; }
    }
    advance(&p); /* prime cur; prev garbage */
    KcNode *prog = kc_node_new(N_PROGRAM, p.cur.pos);
    while (!check(&p, TK_EOF)) {
        kc_list_push(&prog->as.program.stmts, parse_stmt(&p));
    }
    if (g_keep_aliases) {
        g_naliases = p.naliases;
        for (int i = 0; i < p.naliases; i++) { g_alias_names[i] = p.alias_names[i]; g_alias_targets[i] = p.alias_targets[i]; }
    }
    return prog;
}
