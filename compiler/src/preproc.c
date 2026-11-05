/* The KingCobra preprocessor (v0.8): a C-style text pass that runs before the lexer.
 *
 *   #define NAME value            #define SQ(x) ((x) * (x))      #define LOG(f, ...) printf(f, __VA_ARGS__)
 *   #undef NAME                   #x stringizes a parameter, a ## b pastes tokens
 *   #if expr / #ifdef / #ifndef / #elif / #else / #endif       (expr: integers, defined(X), && || ! == != < <= > >= + - * / % & | ^ << >> ?:)
 *   #include "file.kc"  /  #include <name>      #pragma once      #error msg      #warning msg
 *   predefined: __FILE__ __LINE__ __COUNTER__ __KC__ __KC_VERSION__ __LINUX__ | __WINDOWS__ | __APPLE__
 *
 * Line numbers are preserved (a directive becomes a blank line), so diagnostics still point at the right
 * line. An #included file is spliced in flattened onto the directive's own line (its comments removed),
 * so errors inside it report the including line. Strings, character literals and comments are never
 * macro-expanded. Arguments of a macro call may span lines; a `,` inside (), [] or {} does not split them.
 */
#include "kc_common.h"
#include "preproc.h"
#include <ctype.h>
#include <stdarg.h>
#include <limits.h>
#include <stdlib.h>
#ifndef _WIN32
#include <unistd.h>
#endif

typedef struct { char *p; size_t len, cap; } Sb;
static void sb_put(Sb *b, const char *s, size_t n) {
    if (b->len + n + 1 > b->cap) { b->cap = (b->len + n + 1) * 2 + 64; b->p = kc_realloc(b->p, b->cap); }
    memcpy(b->p + b->len, s, n); b->len += n; b->p[b->len] = 0;
}
static void sb_putc(Sb *b, char c) { sb_put(b, &c, 1); }
static void sb_puts(Sb *b, const char *s) { sb_put(b, s, strlen(s)); }

typedef struct { char *name; char **params; int nparams; bool func, variadic; char *body; } Macro;

struct KcPP {
    Macro *m; int n, cap;
    int counter;
    char **once; int nonce;
};

#define MAX_HIDE 48
#define MAX_COND 64
typedef struct {
    KcPP *pp;
    const char *file;
    KcPos pos;                     /* position of the directive / invocation being processed (for errors) */
    int line;
    bool strip;                    /* inside an #include: drop comments */
    int depth;                     /* #include depth */
    const char *hide[MAX_HIDE]; int nhide;
    bool cact[MAX_COND], ctaken[MAX_COND], cparent[MAX_COND]; int csp;
} Ex;

static char *g_cmdline_defs[64]; static int g_ncmd = 0;
void kc_pp_add_cmdline_define(const char *s) { if (g_ncmd < 64) g_cmdline_defs[g_ncmd++] = kc_strdup(s); }

static Macro *find_macro(KcPP *pp, const char *name, size_t len) {
    for (int i = 0; i < pp->n; i++)
        if (strlen(pp->m[i].name) == len && memcmp(pp->m[i].name, name, len) == 0) return &pp->m[i];
    return NULL;
}
static void add_macro(KcPP *pp, Macro mac) {
    Macro *old = find_macro(pp, mac.name, strlen(mac.name));
    if (old) { *old = mac; return; }
    if (pp->n == pp->cap) { pp->cap = pp->cap ? pp->cap * 2 : 32; pp->m = kc_realloc(pp->m, sizeof(Macro) * (size_t)pp->cap); }
    pp->m[pp->n++] = mac;
}
static void del_macro(KcPP *pp, const char *name, size_t len) {
    for (int i = 0; i < pp->n; i++)
        if (strlen(pp->m[i].name) == len && memcmp(pp->m[i].name, name, len) == 0) { pp->m[i] = pp->m[--pp->n]; return; }
}
void kc_pp_define(KcPP *pp, const char *name, const char *body) {
    Macro m; memset(&m, 0, sizeof m);
    m.name = kc_strdup(name); m.body = kc_strdup(body ? body : "1");
    add_macro(pp, m);
}
bool kc_pp_is_defined(KcPP *pp, const char *name) { return find_macro(pp, name, strlen(name)) != NULL; }
int kc_pp_macro_count(KcPP *pp) { return pp->n; }
const char *kc_pp_macro_name(KcPP *pp, int i) { return pp->m[i].name; }
const char *kc_pp_macro_body(KcPP *pp, int i) { return pp->m[i].body; }
bool kc_pp_macro_is_func(KcPP *pp, int i) { return pp->m[i].func; }
void kc_pp_macro_params(KcPP *pp, int i, char *out, size_t cap) {
    Macro *m = &pp->m[i]; out[0] = 0; size_t l = 0;
    for (int k = 0; k < m->nparams && l + 2 < cap; k++) l += (size_t)snprintf(out + l, cap - l, "%s%s", k ? ", " : "", m->params[k]);
    if (m->variadic && l + 6 < cap) snprintf(out + l, cap - l, "%s...", m->nparams ? ", " : "");
}
bool kc_pp_undefine(KcPP *pp, const char *name) {
    if (!find_macro(pp, name, strlen(name))) return false;
    del_macro(pp, name, strlen(name)); return true;
}

KcPP *kc_pp_new(void) {
    KcPP *pp = kc_malloc(sizeof(KcPP));
    memset(pp, 0, sizeof *pp);
    kc_pp_define(pp, "__KC__", "1");
    char ver[64]; snprintf(ver, sizeof ver, "\"%s\"", KC_VERSION);
    kc_pp_define(pp, "__KC_VERSION__", ver);
#if defined(_WIN32)
    kc_pp_define(pp, "__WINDOWS__", "1");
#elif defined(__APPLE__)
    kc_pp_define(pp, "__APPLE__", "1");
#else
    kc_pp_define(pp, "__LINUX__", "1");
#endif
    for (int i = 0; i < g_ncmd; i++) {                       /* -DNAME  /  -DNAME=value */
        char *eq = strchr(g_cmdline_defs[i], '=');
        if (eq) { *eq = 0; kc_pp_define(pp, g_cmdline_defs[i], eq + 1); *eq = '='; }
        else kc_pp_define(pp, g_cmdline_defs[i], "1");
    }
    return pp;
}

/* ---------------------------------------------------------------- helpers */

static bool is_idstart(int c) { return isalpha(c) || c == '_'; }
static bool is_idchar(int c) { return isalnum(c) || c == '_'; }
static void pp_err(Ex *x, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char msg[500]; vsnprintf(msg, sizeof msg, fmt, ap); va_end(ap);
    kc_error_at(x->pos, "%s", msg);
}

static bool active(Ex *x) { return x->csp == 0 || x->cact[x->csp - 1]; }

/* copies a string or character literal starting at text[*i] (the opening quote) */
static void copy_quoted(const char *text, size_t len, size_t *i, Sb *out) {
    char q = text[*i];
    sb_putc(out, text[(*i)++]);
    while (*i < len && text[*i] != q) {
        if (text[*i] == '\\' && *i + 1 < len) sb_putc(out, text[(*i)++]);
        sb_putc(out, text[(*i)++]);
    }
    if (*i < len) sb_putc(out, text[(*i)++]);
}

static void pp_scan(Ex *x, const char *text, size_t len, Sb *out, bool top);
static void do_directive(Ex *x, const char *dir, size_t dlen, Sb *out);

static bool hidden(Ex *x, const char *name, size_t n) {
    for (int i = 0; i < x->nhide; i++) if (strlen(x->hide[i]) == n && memcmp(x->hide[i], name, n) == 0) return true;
    return false;
}

static void stringize(Sb *out, const char *s, size_t n) {
    sb_putc(out, '"');
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '"' || s[i] == '\\') sb_putc(out, '\\');
        if (s[i] == '\n') { sb_putc(out, ' '); continue; }
        sb_putc(out, s[i]);
    }
    sb_putc(out, '"');
}

/* ---------------------------------------------------------------- macro expansion */

typedef struct { const char *p; size_t n; } Span;

/* Collects the arguments of a call whose '(' is at text[*pos]; leaves *pos after the ')'. */
static int collect_args(Ex *x, const char *text, size_t len, size_t *pos, Span *args, int maxargs, const char *mname) {
    size_t i = *pos + 1;
    int nargs = 0, depth = 0;
    size_t start = i;
    for (;;) {
        if (i >= len) pp_err(x, "unterminated argument list in a call of macro '%s'", mname);
        char c = text[i];
        if (c == '"' || c == '\'') { Sb tmp = {0}; copy_quoted(text, len, &i, &tmp); free(tmp.p); continue; }
        if (c == '(' || c == '[' || c == '{') depth++;
        else if (c == ')' || c == ']' || c == '}') {
            if (depth == 0) {
                if (c != ')') pp_err(x, "mismatched '%c' in a call of macro '%s'", c, mname);
                if (nargs >= maxargs) pp_err(x, "too many arguments in a call of macro '%s'", mname);
                args[nargs].p = text + start; args[nargs].n = i - start; nargs++;
                i++; break;
            }
            depth--;
        } else if (c == ',' && depth == 0) {
            if (nargs >= maxargs) pp_err(x, "too many arguments in a call of macro '%s'", mname);
            args[nargs].p = text + start; args[nargs].n = i - start; nargs++;
            start = i + 1;
        }
        i++;
    }
    for (int k = 0; k < nargs; k++) {                         /* trim */
        while (args[k].n && isspace((unsigned char)args[k].p[0])) { args[k].p++; args[k].n--; }
        while (args[k].n && isspace((unsigned char)args[k].p[args[k].n - 1])) args[k].n--;
    }
    *pos = i;
    return nargs;
}

static void expand_text(Ex *x, const char *text, size_t len, Sb *out) { pp_scan(x, text, len, out, false); }

static char *expanded_copy(Ex *x, Span s) {
    Sb o = {0};
    expand_text(x, s.p, s.n, &o);
    if (!o.p) { o.p = kc_malloc(1); o.p[0] = 0; }
    return o.p;
}

/* Tries to expand the identifier text[i..j). On success appends the expansion, sets *next to the first
 * unconsumed byte and returns true. */
static bool try_expand(Ex *x, const char *text, size_t len, size_t i, size_t j, Sb *out, size_t *next) {
    const char *name = text + i; size_t nl = j - i;
    char tmp[64];
    if (nl == 8 && !memcmp(name, "__LINE__", 8)) { snprintf(tmp, sizeof tmp, "%d", x->line); sb_puts(out, tmp); *next = j; return true; }
    if (nl == 8 && !memcmp(name, "__FILE__", 8)) { stringize(out, x->file ? x->file : "<input>", strlen(x->file ? x->file : "<input>")); *next = j; return true; }
    if (nl == 11 && !memcmp(name, "__COUNTER__", 11)) { snprintf(tmp, sizeof tmp, "%d", x->pp->counter++); sb_puts(out, tmp); *next = j; return true; }
    Macro *m = find_macro(x->pp, name, nl);
    if (!m || hidden(x, name, nl)) return false;

    Span args[65]; int nargs = 0;
    size_t end = j;
    if (m->func) {
        size_t k = j;
        while (k < len && isspace((unsigned char)text[k])) k++;
        if (k >= len || text[k] != '(') return false;         /* a function-like macro named without a call is left alone */
        nargs = collect_args(x, text, len, &k, args, 65, m->name);
        end = k;
        int want = m->nparams;
        if (want == 0 && !m->variadic && nargs == 1 && args[0].n == 0) nargs = 0;
        if (m->variadic ? nargs < want : nargs != want)
            pp_err(x, "macro '%s' expects %s%d argument(s), got %d", m->name, m->variadic ? "at least " : "", want, nargs);
    }

    /* substitute the body */
    Sb sub = {0};
    const char *b = m->body; size_t bl = strlen(b);
    bool paste_next = false;
    for (size_t q = 0; q < bl; ) {
        char c = b[q];
        if (c == '"' || c == '\'') { copy_quoted(b, bl, &q, &sub); paste_next = false; continue; }
        if (m->func && c == '#' && q + 1 < bl && b[q + 1] == '#') {         /* a ## b */
            while (sub.len && isspace((unsigned char)sub.p[sub.len - 1])) sub.p[--sub.len] = 0;
            q += 2; while (q < bl && isspace((unsigned char)b[q])) q++;
            paste_next = true; continue;
        }
        if (m->func && c == '#') {                                          /* #param */
            size_t r = q + 1; while (r < bl && isspace((unsigned char)b[r])) r++;
            size_t s = r; while (r < bl && is_idchar((unsigned char)b[r])) r++;
            bool done = false;
            for (int pi = 0; pi < m->nparams && !done; pi++)
                if (strlen(m->params[pi]) == r - s && !memcmp(m->params[pi], b + s, r - s)) { stringize(&sub, args[pi].p, args[pi].n); done = true; }
            if (!done && m->variadic && r - s == 11 && !memcmp(b + s, "__VA_ARGS__", 11)) {
                Sb va = {0};
                for (int k = m->nparams; k < nargs; k++) { if (k > m->nparams) sb_puts(&va, ", "); sb_put(&va, args[k].p, args[k].n); }
                stringize(&sub, va.p ? va.p : "", va.len); free(va.p); done = true;
            }
            if (done) { q = r; continue; }
            sb_putc(&sub, c); q++; continue;
        }
        if (is_idstart((unsigned char)c)) {
            size_t r = q; while (r < bl && is_idchar((unsigned char)b[r])) r++;
            bool done = false;
            if (m->func) {
                size_t look = r; while (look < bl && isspace((unsigned char)b[look])) look++;
                bool raw = paste_next || (look + 1 < bl && b[look] == '#' && b[look + 1] == '#');
                for (int pi = 0; pi < m->nparams && !done; pi++)
                    if (strlen(m->params[pi]) == r - q && !memcmp(m->params[pi], b + q, r - q)) {
                        if (raw) sb_put(&sub, args[pi].p, args[pi].n);
                        else { char *e = expanded_copy(x, args[pi]); sb_puts(&sub, e); free(e); }
                        done = true;
                    }
                if (!done && m->variadic && r - q == 11 && !memcmp(b + q, "__VA_ARGS__", 11)) {
                    for (int k = m->nparams; k < nargs; k++) {
                        if (k > m->nparams) sb_puts(&sub, ", ");
                        if (raw) sb_put(&sub, args[k].p, args[k].n);
                        else { char *e = expanded_copy(x, args[k]); sb_puts(&sub, e); free(e); }
                    }
                    done = true;
                }
            }
            if (!done) sb_put(&sub, b + q, r - q);
            paste_next = false; q = r; continue;
        }
        sb_putc(&sub, c); if (!isspace((unsigned char)c)) paste_next = false; q++;
    }

    /* rescan with this macro hidden (no infinite recursion) */
    if (x->nhide >= MAX_HIDE) pp_err(x, "macro expansion nested too deeply (at '%s')", m->name);
    x->hide[x->nhide++] = m->name;
    if (sub.p) expand_text(x, sub.p, sub.len, out);
    x->nhide--;
    free(sub.p);
    *next = end;
    return true;
}

/* ---------------------------------------------------------------- #if expressions */

typedef struct { Ex *x; const char *s; size_t i, n; } CondP;
static void cp_ws(CondP *c) { while (c->i < c->n && isspace((unsigned char)c->s[c->i])) c->i++; }
static bool cp_eat(CondP *c, const char *op) {
    cp_ws(c); size_t l = strlen(op);
    if (c->i + l <= c->n && !memcmp(c->s + c->i, op, l)) {
        char nx = c->i + l < c->n ? c->s[c->i + l] : 0;        /* don't split `&&` into `&`, `<=` into `<`, ... */
        if ((l == 1 && (op[0] == '&' || op[0] == '|') && nx == op[0]) ||
            (l == 1 && (op[0] == '<' || op[0] == '>') && (nx == op[0] || nx == '=')) ||
            (l == 1 && (op[0] == '!' || op[0] == '=') && nx == '=')) return false;
        c->i += l; return true;
    }
    return false;
}
static int64_t cp_ternary(CondP *c);
static int64_t cp_primary(CondP *c) {
    cp_ws(c);
    if (c->i >= c->n) pp_err(c->x, "#if: the expression is incomplete");
    char ch = c->s[c->i];
    if (ch == '(') { c->i++; int64_t v = cp_ternary(c); if (!cp_eat(c, ")")) pp_err(c->x, "#if: missing ')'"); return v; }
    if (ch == '!') { c->i++; return !cp_primary(c); }
    if (ch == '~') { c->i++; return ~cp_primary(c); }
    if (ch == '-') { c->i++; return -cp_primary(c); }
    if (ch == '+') { c->i++; return cp_primary(c); }
    if (isdigit((unsigned char)ch)) {
        char *end; int64_t v = (int64_t)strtoull(c->s + c->i, &end, 0);
        c->i = (size_t)(end - c->s);
        while (c->i < c->n && (c->s[c->i] == 'u' || c->s[c->i] == 'U' || c->s[c->i] == 'l' || c->s[c->i] == 'L')) c->i++;
        return v;
    }
    if (ch == '\'') {
        if (c->i + 2 < c->n && c->s[c->i + 1] != '\\' && c->s[c->i + 2] == '\'') { int64_t v = (unsigned char)c->s[c->i + 1]; c->i += 3; return v; }
        pp_err(c->x, "#if: unsupported character literal");
    }
    if (is_idstart((unsigned char)ch)) {                      /* an identifier left after expansion is 0 (true/false are 1/0) */
        size_t s = c->i; while (c->i < c->n && is_idchar((unsigned char)c->s[c->i])) c->i++;
        if (c->i - s == 4 && !memcmp(c->s + s, "true", 4)) return 1;
        return 0;
    }
    pp_err(c->x, "#if: unexpected '%c' in the expression", ch);
    return 0;
}
static int64_t cp_mul(CondP *c) {
    int64_t v = cp_primary(c);
    for (;;) {
        if (cp_eat(c, "*")) v *= cp_primary(c);
        else if (cp_eat(c, "/")) { int64_t r = cp_primary(c); if (!r) pp_err(c->x, "#if: division by zero"); v /= r; }
        else if (cp_eat(c, "%")) { int64_t r = cp_primary(c); if (!r) pp_err(c->x, "#if: division by zero"); v %= r; }
        else return v;
    }
}
static int64_t cp_add(CondP *c) { int64_t v = cp_mul(c); for (;;) { if (cp_eat(c, "+")) v += cp_mul(c); else if (cp_eat(c, "-")) v -= cp_mul(c); else return v; } }
static int64_t cp_shift(CondP *c) { int64_t v = cp_add(c); for (;;) { if (cp_eat(c, "<<")) v <<= (cp_add(c) & 63); else if (cp_eat(c, ">>")) v >>= (cp_add(c) & 63); else return v; } }
static int64_t cp_rel(CondP *c) {
    int64_t v = cp_shift(c);
    for (;;) {
        if (cp_eat(c, "<=")) v = v <= cp_shift(c); else if (cp_eat(c, ">=")) v = v >= cp_shift(c);
        else if (cp_eat(c, "<")) v = v < cp_shift(c); else if (cp_eat(c, ">")) v = v > cp_shift(c); else return v;
    }
}
static int64_t cp_eq(CondP *c) { int64_t v = cp_rel(c); for (;;) { if (cp_eat(c, "==")) v = v == cp_rel(c); else if (cp_eat(c, "!=")) v = v != cp_rel(c); else return v; } }
static int64_t cp_band(CondP *c) { int64_t v = cp_eq(c); while (cp_eat(c, "&")) v &= cp_eq(c); return v; }
static int64_t cp_bxor(CondP *c) { int64_t v = cp_band(c); while (cp_eat(c, "^")) v ^= cp_band(c); return v; }
static int64_t cp_bor(CondP *c) { int64_t v = cp_bxor(c); while (cp_eat(c, "|")) v |= cp_bxor(c); return v; }
static int64_t cp_land(CondP *c) { int64_t v = cp_bor(c); while (cp_eat(c, "&&")) { int64_t r = cp_bor(c); v = v && r; } return v; }
static int64_t cp_lor(CondP *c) { int64_t v = cp_land(c); while (cp_eat(c, "||")) { int64_t r = cp_land(c); v = v || r; } return v; }
static int64_t cp_ternary(CondP *c) {
    int64_t v = cp_lor(c);
    if (cp_eat(c, "?")) { int64_t a = cp_ternary(c); if (!cp_eat(c, ":")) pp_err(c->x, "#if: expected ':' in ?:"); int64_t b = cp_ternary(c); return v ? a : b; }
    return v;
}

static bool eval_condition(Ex *x, const char *expr, size_t len) {
    Sb pre = {0};                                             /* defined X / defined(X) first, before macro expansion */
    for (size_t i = 0; i < len; ) {
        if (is_idstart((unsigned char)expr[i])) {
            size_t j = i; while (j < len && is_idchar((unsigned char)expr[j])) j++;
            if (j - i == 7 && !memcmp(expr + i, "defined", 7)) {
                size_t k = j; while (k < len && isspace((unsigned char)expr[k])) k++;
                bool paren = k < len && expr[k] == '(';
                if (paren) { k++; while (k < len && isspace((unsigned char)expr[k])) k++; }
                size_t s = k; while (k < len && is_idchar((unsigned char)expr[k])) k++;
                if (k == s) pp_err(x, "#if: `defined` needs a name");
                bool d = find_macro(x->pp, expr + s, k - s) != NULL;
                if (!d && k - s == 8 && (!memcmp(expr + s, "__LINE__", 8) || !memcmp(expr + s, "__FILE__", 8))) d = true;
                if (paren) { while (k < len && isspace((unsigned char)expr[k])) k++; if (k < len && expr[k] == ')') k++; else pp_err(x, "#if: missing ')' after defined(...)"); }
                sb_puts(&pre, d ? "1" : "0");
                i = k; continue;
            }
            sb_put(&pre, expr + i, j - i); i = j; continue;
        }
        sb_putc(&pre, expr[i++]);
    }
    Sb ex = {0};
    if (pre.p) expand_text(x, pre.p, pre.len, &ex);
    CondP c = { x, ex.p ? ex.p : "", 0, ex.len };
    int64_t v = cp_ternary(&c);
    cp_ws(&c);
    if (c.i < c.n) pp_err(x, "#if: unexpected text '%.*s' after the expression", (int)(c.n - c.i > 20 ? 20 : c.n - c.i), c.s + c.i);
    free(pre.p); free(ex.p);
    return v != 0;
}

/* ---------------------------------------------------------------- directives */

static char *read_whole(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long size = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = kc_malloc((size_t)size + 1);
    size_t got = fread(buf, 1, (size_t)size, f); buf[got] = 0; fclose(f);
    return buf;
}

static char *dir_of(const char *file) {
    if (!file) return kc_strdup(".");
    const char *slash = strrchr(file, '/');
#ifdef _WIN32
    const char *bs = strrchr(file, '\\'); if (bs && (!slash || bs > slash)) slash = bs;
#endif
    if (!slash) return kc_strdup(".");
    size_t n = (size_t)(slash - file);
    char *d = kc_malloc(n + 2); memcpy(d, file, n); d[n] = 0;
    return d;
}

static char *find_include(Ex *x, const char *name, bool angle) {
    char buf[4096];
    if (!angle) {
        char *d = dir_of(x->file);
        snprintf(buf, sizeof buf, "%s/%s", d, name); free(d);
        FILE *f = fopen(buf, "rb"); if (f) { fclose(f); return kc_strdup(buf); }
        f = fopen(name, "rb"); if (f) { fclose(f); return kc_strdup(name); }
    }
    const char *env = getenv("KC_INCLUDE");
    if (env) {
        const char *s = env;
        while (*s) {
            const char *e = strchr(s, ':'); size_t n = e ? (size_t)(e - s) : strlen(s);
            snprintf(buf, sizeof buf, "%.*s/%s", (int)n, s, name);
            FILE *f = fopen(buf, "rb"); if (f) { fclose(f); return kc_strdup(buf); }
            if (!e) break;
            s = e + 1;
        }
    }
    snprintf(buf, sizeof buf, "include/%s", name);
    FILE *f = fopen(buf, "rb"); if (f) { fclose(f); return kc_strdup(buf); }
    return NULL;
}

static void do_include(Ex *x, const char *arg, size_t alen, Sb *out) {
    Sb tmp = {0};
    Sb argx = {0};
    expand_text(x, arg, alen, &argx);                          /* #include MACRO */
    const char *a = argx.p ? argx.p : ""; size_t al = argx.len;
    while (al && isspace((unsigned char)*a)) { a++; al--; }
    if (al < 2 || (a[0] != '"' && a[0] != '<')) pp_err(x, "#include expects \"file\" or <file>");
    char close = a[0] == '"' ? '"' : '>';
    const char *e = memchr(a + 1, close, al - 1);
    if (!e) pp_err(x, "#include: missing closing '%c'", close);
    char name[2048]; size_t nl = (size_t)(e - (a + 1));
    if (nl >= sizeof name) pp_err(x, "#include: path too long");
    memcpy(name, a + 1, nl); name[nl] = 0;
    char *path = find_include(x, name, a[0] == '<');
    if (!path) pp_err(x, "#include: cannot find '%s'", name);
    for (int i = 0; i < x->pp->nonce; i++) if (!strcmp(x->pp->once[i], path)) { free(tmp.p); free(argx.p); return; }   /* #pragma once */
    if (x->depth >= 32) pp_err(x, "#include nested too deeply (is '%s' including itself?)", path);
    char *src = read_whole(path);
    if (!src) pp_err(x, "#include: cannot read '%s'", path);

    Ex sub = *x;                                              /* the included file: own conds, flattened, comments stripped */
    sub.file = path; sub.csp = 0; sub.strip = true; sub.depth = x->depth + 1; sub.nhide = 0; sub.line = 1;
    pp_scan(&sub, src, strlen(src), &tmp, true);
    if (sub.csp != 0) { sub.pos = (KcPos){ path, 1, 1 }; pp_err(&sub, "unterminated #if in '%s'", path); }
    for (size_t i = 0; i < tmp.len; i++) { char c = tmp.p ? tmp.p[i] : ' '; sb_putc(out, (c == '\n' || c == '\r') ? ' ' : c); }
    sb_putc(out, ' ');
    free(src); free(tmp.p); free(argx.p);
}

static void do_directive(Ex *x, const char *dir, size_t dlen, Sb *out) {
    size_t i = 0;
    while (i < dlen && isspace((unsigned char)dir[i])) i++;
    size_t s = i; while (i < dlen && is_idchar((unsigned char)dir[i])) i++;
    const char *kw = dir + s; size_t kl = i - s;
    while (i < dlen && isspace((unsigned char)dir[i])) i++;
    const char *rest = dir + i; size_t rl = dlen - i;
    while (rl && isspace((unsigned char)rest[rl - 1])) rl--;
    #define IS(K) (kl == strlen(K) && !memcmp(kw, K, kl))

    if (kl == 0) return;                                       /* a lone `#` is a null directive */
    if (IS("if") || IS("ifdef") || IS("ifndef")) {
        if (x->csp >= MAX_COND) pp_err(x, "#if nested too deeply");
        bool parent = active(x), val = false;
        if (parent) {
            if (IS("if")) val = eval_condition(x, rest, rl);
            else {
                size_t k = 0; while (k < rl && is_idchar((unsigned char)rest[k])) k++;
                if (k == 0) pp_err(x, "#%.*s needs a name", (int)kl, kw);
                val = find_macro(x->pp, rest, k) != NULL;
                if (IS("ifndef")) val = !val;
            }
        }
        x->cparent[x->csp] = parent; x->cact[x->csp] = parent && val; x->ctaken[x->csp] = parent && val; x->csp++;
        return;
    }
    if (IS("elif") || IS("else")) {
        if (x->csp == 0) pp_err(x, "#%.*s without #if", (int)kl, kw);
        int t = x->csp - 1;
        if (!x->cparent[t]) return;
        if (x->ctaken[t]) { x->cact[t] = false; return; }
        bool val = IS("else") ? true : eval_condition(x, rest, rl);
        x->cact[t] = val; if (val) x->ctaken[t] = true;
        return;
    }
    if (IS("endif")) { if (x->csp == 0) pp_err(x, "#endif without #if"); x->csp--; return; }
    if (!active(x)) return;

    if (IS("define")) {
        size_t k = 0; if (!rl || !is_idstart((unsigned char)rest[0])) pp_err(x, "#define needs a macro name");
        while (k < rl && is_idchar((unsigned char)rest[k])) k++;
        Macro m; memset(&m, 0, sizeof m);
        m.name = kc_malloc(k + 1); memcpy(m.name, rest, k); m.name[k] = 0;
        if (k < rl && rest[k] == '(') {
            m.func = true; k++;
            char *params[64]; int np = 0;
            for (;;) {
                while (k < rl && isspace((unsigned char)rest[k])) k++;
                if (k < rl && rest[k] == ')') { k++; break; }
                if (k + 2 < rl && !memcmp(rest + k, "...", 3)) { m.variadic = true; k += 3; while (k < rl && isspace((unsigned char)rest[k])) k++; if (k < rl && rest[k] == ')') { k++; break; } pp_err(x, "#define %s: '...' must be the last parameter", m.name); }
                size_t ps = k; while (k < rl && is_idchar((unsigned char)rest[k])) k++;
                if (k == ps) pp_err(x, "#define %s: bad parameter list", m.name);
                if (np >= 64) pp_err(x, "#define %s: too many parameters", m.name);
                params[np] = kc_malloc(k - ps + 1); memcpy(params[np], rest + ps, k - ps); params[np][k - ps] = 0; np++;
                while (k < rl && isspace((unsigned char)rest[k])) k++;
                if (k < rl && rest[k] == ',') { k++; continue; }
                if (k < rl && rest[k] == ')') { k++; break; }
                pp_err(x, "#define %s: expected ',' or ')' in the parameter list", m.name);
            }
            m.nparams = np; m.params = kc_malloc(sizeof(char *) * (size_t)(np ? np : 1)); memcpy(m.params, params, sizeof(char *) * (size_t)np);
        }
        while (k < rl && isspace((unsigned char)rest[k])) k++;
        m.body = kc_malloc(rl - k + 1); memcpy(m.body, rest + k, rl - k); m.body[rl - k] = 0;
        add_macro(x->pp, m);
        return;
    }
    if (IS("undef")) {
        size_t k = 0; while (k < rl && is_idchar((unsigned char)rest[k])) k++;
        if (k == 0) pp_err(x, "#undef needs a name");
        del_macro(x->pp, rest, k);
        return;
    }
    if (IS("include")) { do_include(x, rest, rl, out); return; }
    if (IS("error")) pp_err(x, "#error %.*s", (int)rl, rest);
    if (IS("warning")) { kc_warn_at(x->pos, "#warning %.*s", (int)rl, rest); return; }
    if (IS("pragma")) {
        if (rl >= 4 && !memcmp(rest, "once", 4) && x->depth > 0) {
            x->pp->once = kc_realloc(x->pp->once, sizeof(char *) * (size_t)(x->pp->nonce + 1));
            x->pp->once[x->pp->nonce++] = kc_strdup(x->file);
        }
        return;
    }
    if (IS("line")) return;
    pp_err(x, "unknown preprocessor directive '#%.*s'", (int)kl, kw);
    #undef IS
}

/* ---------------------------------------------------------------- the scanner */

static void pp_scan(Ex *x, const char *text, size_t len, Sb *out, bool top) {
    size_t i = 0;
    bool bol = true;
    while (i < len) {
        char c = text[i];
        if (top && bol) {
            size_t k = i; while (k < len && (text[k] == ' ' || text[k] == '\t')) k++;
            if (k < len && text[k] == '#') {
                if (x->line == 1 && k + 1 < len && text[k + 1] == '!') {          /* #!shebang: a blank line */
                    while (i < len && text[i] != '\n') i++;
                    continue;
                }
                x->pos = (KcPos){ x->file, x->line, (int)(k - i) + 1 };
                size_t ds = k + 1; Sb dir = {0}; int nl = 0;
                size_t q = ds;
                while (q < len) {                                                 /* the logical line: joins `\` continuations, drops comments */
                    if (text[q] == '\n') break;
                    if (text[q] == '\\' && q + 1 < len && (text[q + 1] == '\n' || (text[q + 1] == '\r' && q + 2 < len && text[q + 2] == '\n'))) {
                        q += text[q + 1] == '\r' ? 3 : 2; nl++; sb_putc(&dir, ' '); continue;
                    }
                    if (text[q] == '"' || text[q] == '\'') { copy_quoted(text, len, &q, &dir); continue; }
                    if (text[q] == '/' && q + 1 < len && text[q + 1] == '/') { while (q < len && text[q] != '\n') q++; break; }
                    if (text[q] == '/' && q + 1 < len && text[q + 1] == '*') {
                        q += 2; while (q + 1 < len && !(text[q] == '*' && text[q + 1] == '/')) { if (text[q] == '\n') nl++; q++; }
                        q = q + 2 <= len ? q + 2 : len; sb_putc(&dir, ' '); continue;
                    }
                    sb_putc(&dir, text[q++]);
                }
                Sb inc = {0};
                do_directive(x, dir.p ? dir.p : "", dir.len, &inc);
                if (inc.p) sb_put(out, inc.p, inc.len);
                free(dir.p); free(inc.p);
                for (int n = 0; n < nl; n++) sb_putc(out, '\n');
                x->line += nl;
                i = q;                                                            /* the terminating '\n' is handled below */
                bol = false;
                continue;
            }
        }
        if (c == '\n') { if (!top || active(x)) sb_putc(out, c); else sb_putc(out, c); if (top) x->line++; bol = true; i++; continue; }
        if (top && !active(x)) { i++; bol = false; continue; }                    /* a skipped #if branch */
        bol = false;
        if (c == '/' && i + 1 < len && text[i + 1] == '/') {
            size_t e = i; while (e < len && text[e] != '\n') e++;
            if (!x->strip) sb_put(out, text + i, e - i);
            i = e; continue;
        }
        if (c == '/' && i + 1 < len && text[i + 1] == '*') {
            size_t e = i + 2; int nls = 0;
            while (e + 1 < len && !(text[e] == '*' && text[e + 1] == '/')) { if (text[e] == '\n') nls++; e++; }
            e = e + 2 <= len ? e + 2 : len;
            if (x->strip) sb_putc(out, ' '); else sb_put(out, text + i, e - i);
            if (x->strip) for (int n = 0; n < nls; n++) sb_putc(out, '\n');
            if (top) x->line += nls;
            i = e; continue;
        }
        if (c == '"' || c == '\'') {
            size_t s = i; copy_quoted(text, len, &i, out);
            if (top) for (size_t k = s; k < i; k++) if (text[k] == '\n') x->line++;
            continue;
        }
        if (isdigit((unsigned char)c)) {
            size_t j = i; while (j < len && (isalnum((unsigned char)text[j]) || text[j] == '_' || text[j] == '.')) j++;
            sb_put(out, text + i, j - i); i = j; continue;
        }
        if (is_idstart((unsigned char)c)) {
            size_t j = i; while (j < len && is_idchar((unsigned char)text[j])) j++;
            if (j < len && text[j] == '"' && j - i == 1 && c == 'f') { sb_putc(out, 'f'); i = j; continue; }   /* f"..." stays a string */
            size_t next;
            if (try_expand(x, text, len, i, j, out, &next)) {
                if (top) { int nls = 0; for (size_t k = i; k < next; k++) if (text[k] == '\n') nls++; x->line += nls; for (int n = 0; n < nls; n++) sb_putc(out, '\n'); }
                i = next; continue;
            }
            sb_put(out, text + i, j - i); i = j; continue;
        }
        sb_putc(out, c); i++;
    }
}

/* ---------------------------------------------------------------- entry points */

static bool needs_pp(const char *src) {
    if (g_ncmd > 0) return true;                               /* -D macros may be used anywhere */
    const char *p = src;
    for (bool bol = true; *p; p++) {
        if (bol && (*p == ' ' || *p == '\t')) continue;
        if (bol && *p == '#') return true;
        bol = (*p == '\n');
    }
    return strstr(src, "__FILE__") || strstr(src, "__LINE__") || strstr(src, "__COUNTER__") || strstr(src, "__KC") ||
           strstr(src, "__LINUX__") || strstr(src, "__WINDOWS__") || strstr(src, "__APPLE__");
}

/* Runs the preprocessor over `src`. Returns `src` itself when nothing needs doing (no directives, no macros
 * defined, none of the predefined names used), else a new string. `pp` may be NULL (a fresh table is used)
 * or a persistent table (the REPL). */
const char *kc_preprocess(const char *src, const char *filename, KcPP *pp) {
    if (!pp && !needs_pp(src)) return src;
    if (pp && pp->n == 0 && !needs_pp(src)) return src;
    if (!pp) pp = kc_pp_new();
    else if (!needs_pp(src) && pp->n <= 3 + g_ncmd) return src;
    Ex x; memset(&x, 0, sizeof x);
    x.pp = pp; x.file = filename; x.line = 1;
    x.pos = (KcPos){ filename, 1, 1 };
    Sb out = {0};
    pp_scan(&x, src, strlen(src), &out, true);
    if (x.csp != 0) { x.pos = (KcPos){ filename, x.line, 1 }; pp_err(&x, "unterminated #if (missing #endif)"); }
    if (!out.p) { out.p = kc_malloc(1); out.p[0] = 0; }
    return out.p;
}
