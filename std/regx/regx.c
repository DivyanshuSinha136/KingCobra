/* KingCobra std.regx — a native regular-expression engine.
 *
 * Written from scratch (no PCRE/RE2 dependency): the pattern is parsed to
 * an AST, compiled to a small bytecode program and run by an iterative
 * backtracking VM with an explicit stack (no C recursion per character,
 * so long subjects can't overflow the C stack) and a step budget that
 * turns catastrophic backtracking into a clean error instead of a hang.
 *
 * SIMPLE API  — pass a pattern string straight to any function:
 *     regx.test("^\\d+$", s)   regx.find("(\\w+)@(\\w+)", s)   regx.replace("\\s+", s, " ")
 * ADVANCED    — regx.compile(pattern, flags) returns a reusable handle that
 *               every function also accepts; string patterns are cached.
 *
 * Syntax: literals . ^ $ \A \z  [...] [^...] [a-z] [[:alpha:]]  \d \D \w \W \s \S
 *   \b \B  \n \t \r \f \v \0 \xHH \u{H..} \uHHHH   groups (...) (?:...) (?<name>...)
 *   (?P<name>...)  alternation |  quantifiers * + ? {n} {n,} {n,m} (+ lazy `?`)
 *   lookahead (?=...) (?!...)  backreferences \1..\9 \k<name>
 *   inline flags (?i) (?m) (?s) (?-i) (?i:...)   flags: i = ignore ASCII case,
 *   m = ^/$ match at line breaks, s = . matches \n.
 * Text is treated as UTF-8: `.`, negated classes and \W\D\S consume a whole
 * code point; character classes themselves are ASCII-only (non-ASCII in a
 * class is a clear error). `$` without m matches only at the very end.
 * Not supported (reported as errors, never silently mis-matched):
 * lookbehind, possessive quantifiers, atomic groups.
 *
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com>
 */
#include "interp.h"
#include <ctype.h>

#define RX_MAX_INS    400000
#define RX_MAX_GROUPS 99
#define RX_STEP_LIMIT 30000000L
#define RX_STACK_MAX  (8L * 1024 * 1024)

/* ---------------------------------------------------------------- program */

enum { I_CHAR, I_CHARI, I_ANY, I_CLASS, I_SPLIT, I_JMP, I_SAVE, I_BOL, I_EOL, I_WB, I_NWB,
       I_BACKREF, I_LOOK, I_LOOKEND, I_MARK, I_CHK, I_MATCH };

typedef struct { int op, x, y; } Ins;
typedef struct { uint8_t bits[16]; bool high; } RxClass;   /* bits cover 0..127 */

typedef struct Prog {
    Ins *ins; int n, cap;
    RxClass *cls; int ncls, clscap;
    int ngroups;            /* capture groups, not counting group 0 */
    char *names[RX_MAX_GROUPS + 1];   /* names[g] or NULL */
    int nloop;
    char *source;
} Prog;

/* ------------------------------------------------------------------- AST */

enum { N_EMPTY, N_CHAR, N_ANY, N_CLASS, N_CAT, N_ALT, N_REPEAT, N_GROUP, N_BOL, N_EOL, N_WB, N_NWB,
       N_BACKREF, N_LOOK };

typedef struct Node {
    int type;
    int c;                 /* char / class index / group number / flags */
    int min, max;          /* repeat; max == -1 -> unbounded */
    bool greedy, icase, multiline, dotall, neg;
    struct Node *kid;
    struct Node **list; int n, cap;
} Node;

typedef struct {
    const char *src, *p, *end;
    KcPos pos;
    Prog *prog;
    int ngroups;
    int fl_i, fl_m, fl_s;
    char *pending_backref_names[RX_MAX_GROUPS + 1]; /* unused placeholder to keep struct simple */
} Parser;

#include <setjmp.h>
#include <stdarg.h>
/* Compile-time diagnostics go through rx_err so that regx.is_valid can probe a
 * pattern and recover (longjmp) instead of terminating the program. */
static jmp_buf *g_probe_jmp = NULL;
static void rx_err(KcPos pos, const char *fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    if (g_probe_jmp) longjmp(*g_probe_jmp, 1);
    kc_error_at(pos, "%s", buf);
}
static void rx_fail(Parser *P, const char *msg) {
    rx_err(P->pos, "regx: %s at offset %d in pattern /%s/", msg, (int)(P->p - P->src), P->src);
}

static Node *node_new(int type) { Node *n = kc_malloc(sizeof(Node)); memset(n, 0, sizeof *n); n->type = type; n->greedy = true; return n; }
static void node_add(Node *parent, Node *kid) {
    if (parent->n == parent->cap) { parent->cap = parent->cap ? parent->cap * 2 : 4; parent->list = kc_realloc(parent->list, sizeof(Node *) * (size_t)parent->cap); }
    parent->list[parent->n++] = kid;
}
static void node_free(Node *n) {
    if (!n) return;
    node_free(n->kid);
    for (int i = 0; i < n->n; i++) node_free(n->list[i]);
    free(n->list); free(n);
}

static int add_class(Prog *pr, const RxClass *c) {
    if (pr->ncls == pr->clscap) { pr->clscap = pr->clscap ? pr->clscap * 2 : 8; pr->cls = kc_realloc(pr->cls, sizeof(RxClass) * (size_t)pr->clscap); }
    pr->cls[pr->ncls] = *c; return pr->ncls++;
}
static void cls_set(RxClass *c, int ch) { c->bits[ch >> 3] |= (uint8_t)(1u << (ch & 7)); }
static bool cls_has(const RxClass *c, int ch) { return (c->bits[ch >> 3] >> (ch & 7)) & 1; }
static void cls_range(RxClass *c, int lo, int hi, bool icase) {
    for (int ch = lo; ch <= hi; ch++) {
        cls_set(c, ch);
        if (icase && isalpha(ch)) { cls_set(c, tolower(ch)); cls_set(c, toupper(ch)); }
    }
}
static bool is_word_byte(int ch) { return isalnum(ch) || ch == '_'; }

/* add a \d \w \s style set (optionally complemented) into c */
static void cls_add_escape(RxClass *c, char e) {
    bool neg = isupper((unsigned char)e); char lo = (char)tolower((unsigned char)e);
    for (int ch = 0; ch < 128; ch++) {
        bool in = lo == 'd' ? isdigit(ch) : lo == 'w' ? is_word_byte(ch) : (ch == ' ' || (ch >= 9 && ch <= 13));
        if (in != neg) cls_set(c, ch);
    }
    if (neg) c->high = true;     /* \D \W \S also match any non-ASCII code point */
}

/* ---------------------------------------------------------------- parser */

static Node *parse_alt(Parser *P);

static int hexval(int ch) { return isdigit(ch) ? ch - '0' : (ch >= 'a' && ch <= 'f') ? ch - 'a' + 10 : (ch >= 'A' && ch <= 'F') ? ch - 'A' + 10 : -1; }

static int utf8_encode(unsigned cp, unsigned char *out) {
    if (cp < 0x80) { out[0] = (unsigned char)cp; return 1; }
    if (cp < 0x800) { out[0] = (unsigned char)(0xC0 | (cp >> 6)); out[1] = (unsigned char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) { out[0] = (unsigned char)(0xE0 | (cp >> 12)); out[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F)); out[2] = (unsigned char)(0x80 | (cp & 0x3F)); return 3; }
    out[0] = (unsigned char)(0xF0 | (cp >> 18)); out[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F)); out[3] = (unsigned char)(0x80 | (cp & 0x3F)); return 4;
}

static Node *char_node(Parser *P, int ch) {
    Node *n = node_new(N_CHAR); n->c = ch; n->icase = P->fl_i && isalpha(ch); return n;
}
static Node *bytes_node(Parser *P, const unsigned char *b, int len) {
    if (len == 1) return char_node(P, b[0]);
    Node *cat = node_new(N_CAT);
    for (int i = 0; i < len; i++) node_add(cat, char_node(P, b[i]));
    return cat;
}

/* parses \uHHHH or \u{H...}; p is just after the 'u' */
static unsigned parse_u_escape(Parser *P) {
    unsigned cp = 0; int digits = 0;
    if (P->p < P->end && *P->p == '{') {
        P->p++;
        while (P->p < P->end && *P->p != '}') { int h = hexval((unsigned char)*P->p); if (h < 0) rx_fail(P, "bad \\u{...} escape"); cp = cp * 16 + (unsigned)h; P->p++; if (++digits > 6) rx_fail(P, "\\u{...} too long"); }
        if (P->p >= P->end) rx_fail(P, "unterminated \\u{...}");
        P->p++;
    } else {
        for (int i = 0; i < 4; i++) { if (P->p >= P->end) rx_fail(P, "bad \\u escape"); int h = hexval((unsigned char)*P->p); if (h < 0) rx_fail(P, "bad \\u escape"); cp = cp * 16 + (unsigned)h; P->p++; }
    }
    if (cp > 0x10FFFF) rx_fail(P, "code point out of range");
    return cp;
}

static int find_name(Parser *P, const char *name, int len) {
    for (int g = 1; g <= P->ngroups; g++)
        if (P->prog->names[g] && (int)strlen(P->prog->names[g]) == len && memcmp(P->prog->names[g], name, (size_t)len) == 0) return g;
    return -1;
}

/* escape appearing OUTSIDE a class. p is just after the backslash. */
static Node *parse_escape(Parser *P) {
    if (P->p >= P->end) rx_fail(P, "pattern ends with a lone backslash");
    unsigned char e = (unsigned char)*P->p++;
    switch (e) {
        case 'd': case 'D': case 'w': case 'W': case 's': case 'S': {
            RxClass c; memset(&c, 0, sizeof c);
            cls_add_escape(&c, (char)e);
            Node *n = node_new(N_CLASS); n->c = add_class(P->prog, &c); return n;
        }
        case 'b': return node_new(N_WB);
        case 'B': return node_new(N_NWB);
        case 'A': { Node *n = node_new(N_BOL); n->multiline = false; return n; }
        case 'z': { Node *n = node_new(N_EOL); n->multiline = false; return n; }
        case 'n': return char_node(P, '\n'); case 't': return char_node(P, '\t'); case 'r': return char_node(P, '\r');
        case 'f': return char_node(P, '\f'); case 'v': return char_node(P, '\v'); case '0': return char_node(P, 0);
        case 'x': {
            if (P->end - P->p < 2 || hexval((unsigned char)P->p[0]) < 0 || hexval((unsigned char)P->p[1]) < 0) rx_fail(P, "bad \\x escape");
            int v = hexval((unsigned char)P->p[0]) * 16 + hexval((unsigned char)P->p[1]); P->p += 2;
            return char_node(P, v);
        }
        case 'u': { unsigned char b[4]; int l = utf8_encode(parse_u_escape(P), b); return bytes_node(P, b, l); }
        case 'k': {
            if (P->p >= P->end || *P->p != '<') rx_fail(P, "expected \\k<name>");
            const char *s = ++P->p; while (P->p < P->end && *P->p != '>') P->p++;
            if (P->p >= P->end) rx_fail(P, "unterminated \\k<name>");
            int g = find_name(P, s, (int)(P->p - s)); P->p++;
            if (g < 0) rx_fail(P, "backreference to unknown or later group name");
            Node *n = node_new(N_BACKREF); n->c = g; n->icase = P->fl_i; return n;
        }
        default:
            if (e >= '1' && e <= '9') {
                int g = e - '0';
                if (g > P->ngroups) rx_fail(P, "backreference to a group that does not exist (yet)");
                Node *n = node_new(N_BACKREF); n->c = g; n->icase = P->fl_i; return n;
            }
            if (isalnum(e)) rx_fail(P, "unknown escape sequence");
            return char_node(P, e);
    }
}

static const char *POSIX[] = { "alpha", "digit", "alnum", "upper", "lower", "space", "punct", "xdigit", "word", "blank", "cntrl", "print", "graph" };

static bool posix_has(int idx, int ch) {
    switch (idx) {
        case 0: return isalpha(ch); case 1: return isdigit(ch); case 2: return isalnum(ch); case 3: return isupper(ch);
        case 4: return islower(ch); case 5: return ch == ' ' || (ch >= 9 && ch <= 13); case 6: return ispunct(ch);
        case 7: return isxdigit(ch); case 8: return is_word_byte(ch); case 9: return ch == ' ' || ch == '\t';
        case 10: return iscntrl(ch); case 11: return isprint(ch); default: return isgraph(ch);
    }
}

/* reads one class "atom" as a single ASCII byte value, or -1 if it was a set (already merged) */
static int class_atom(Parser *P, RxClass *c) {
    unsigned char ch = (unsigned char)*P->p++;
    if (ch >= 0x80) rx_fail(P, "non-ASCII characters are not supported inside [...] (use alternation, e.g. (?:é|è))");
    if (ch == '[' && P->p < P->end && *P->p == ':') {
        const char *q = P->p + 1; const char *s = q;
        while (q < P->end && isalpha((unsigned char)*q)) q++;
        if (q + 1 < P->end && *q == ':' && q[1] == ']') {
            for (size_t i = 0; i < sizeof POSIX / sizeof *POSIX; i++)
                if ((size_t)(q - s) == strlen(POSIX[i]) && memcmp(s, POSIX[i], (size_t)(q - s)) == 0) {
                    for (int k = 0; k < 128; k++) if (posix_has((int)i, k)) cls_set(c, k);
                    P->p = q + 2; return -1;
                }
            rx_fail(P, "unknown POSIX class");
        }
    }
    if (ch != '\\') return ch;
    if (P->p >= P->end) rx_fail(P, "pattern ends with a lone backslash");
    unsigned char e = (unsigned char)*P->p++;
    switch (e) {
        case 'd': case 'D': case 'w': case 'W': case 's': case 'S': cls_add_escape(c, (char)e); return -1;
        case 'n': return '\n'; case 't': return '\t'; case 'r': return '\r'; case 'f': return '\f'; case 'v': return '\v';
        case 'b': return '\b'; case '0': return 0;
        case 'x': {
            if (P->end - P->p < 2 || hexval((unsigned char)P->p[0]) < 0 || hexval((unsigned char)P->p[1]) < 0) rx_fail(P, "bad \\x escape");
            int v = hexval((unsigned char)P->p[0]) * 16 + hexval((unsigned char)P->p[1]); P->p += 2;
            if (v >= 0x80) rx_fail(P, "non-ASCII \\x escape inside [...]");
            return v;
        }
        default: if (isalnum(e)) rx_fail(P, "unknown escape sequence in class"); return e;
    }
}

static Node *parse_class(Parser *P) {            /* p just after '[' */
    RxClass c; memset(&c, 0, sizeof c);
    bool neg = false, first = true;
    if (P->p < P->end && *P->p == '^') { neg = true; P->p++; }
    for (;;) {
        if (P->p >= P->end) rx_fail(P, "unterminated character class");
        if (*P->p == ']' && !first) { P->p++; break; }
        first = false;
        int lo = class_atom(P, &c);
        if (lo < 0) continue;
        if (P->end - P->p >= 2 && *P->p == '-' && P->p[1] != ']') {
            P->p++;
            int hi = class_atom(P, &c);
            if (hi < 0) rx_fail(P, "a class shorthand cannot be a range endpoint");
            if (hi < lo) rx_fail(P, "range out of order in character class");
            cls_range(&c, lo, hi, P->fl_i);
        } else cls_range(&c, lo, lo, P->fl_i);
    }
    if (neg) { for (int i = 0; i < 128; i++) { if (cls_has(&c, i)) c.bits[i >> 3] &= (uint8_t)~(1u << (i & 7)); else cls_set(&c, i); } c.high = !c.high; }
    Node *n = node_new(N_CLASS); n->c = add_class(P->prog, &c); return n;
}

static bool parse_int(Parser *P, int *out) {
    if (P->p >= P->end || !isdigit((unsigned char)*P->p)) return false;
    long v = 0; while (P->p < P->end && isdigit((unsigned char)*P->p)) { v = v * 10 + (*P->p++ - '0'); if (v > 100000) rx_fail(P, "repeat count too large"); }
    *out = (int)v; return true;
}

static bool nullable(const Node *n) {
    switch (n->type) {
        case N_EMPTY: case N_BOL: case N_EOL: case N_WB: case N_NWB: case N_LOOK: return true;
        case N_BACKREF: return true;
        case N_CHAR: case N_ANY: case N_CLASS: return false;
        case N_CAT: for (int i = 0; i < n->n; i++) if (!nullable(n->list[i])) return false; return true;
        case N_ALT: for (int i = 0; i < n->n; i++) if (nullable(n->list[i])) return true; return false;
        case N_REPEAT: return n->min == 0 || nullable(n->kid);
        case N_GROUP: return nullable(n->kid);
    }
    return true;
}

/* returns an atom node, or NULL when the construct produced no node (inline flag group) */
static Node *parse_atom(Parser *P) {
    unsigned char ch = (unsigned char)*P->p;
    if (ch == '(') {
        P->p++;
        int cap = -1; bool look = false, neg = false;
        int sv_i = P->fl_i, sv_m = P->fl_m, sv_s = P->fl_s;
        if (P->p < P->end && *P->p == '?') {
            P->p++;
            if (P->p >= P->end) rx_fail(P, "unterminated group");
            char k = *P->p;
            if (k == ':') P->p++;
            else if (k == '=' || k == '!') { look = true; neg = (k == '!'); P->p++; }
            else if (k == '<' && P->p + 1 < P->end && (P->p[1] == '=' || P->p[1] == '!')) rx_fail(P, "lookbehind is not supported");
            else if (k == '<' || k == 'P') {
                if (k == 'P') { P->p++; if (P->p >= P->end || *P->p != '<') rx_fail(P, "expected (?P<name>...)"); }
                const char *s = ++P->p; while (P->p < P->end && *P->p != '>') { if (!is_word_byte((unsigned char)*P->p)) rx_fail(P, "bad group name"); P->p++; }
                if (P->p >= P->end || P->p == s) rx_fail(P, "bad group name");
                if (P->ngroups >= RX_MAX_GROUPS) rx_fail(P, "too many groups");
                int len = (int)(P->p - s);
                if (find_name(P, s, len) > 0) rx_fail(P, "duplicate group name");
                cap = ++P->ngroups;
                P->prog->names[cap] = kc_malloc((size_t)len + 1); memcpy(P->prog->names[cap], s, (size_t)len); P->prog->names[cap][len] = 0;
                P->p++;
            } else {                                /* inline flags: (?i) (?-s) (?im:...) */
                bool on = true; int ni = P->fl_i, nm = P->fl_m, ns = P->fl_s;
                while (P->p < P->end && *P->p != ')' && *P->p != ':') {
                    char f = *P->p++;
                    if (f == '-') on = false;
                    else if (f == 'i') ni = on; else if (f == 'm') nm = on; else if (f == 's') ns = on;
                    else rx_fail(P, "unknown inline flag (supported: i m s)");
                }
                if (P->p >= P->end) rx_fail(P, "unterminated group");
                if (*P->p == ')') { P->p++; P->fl_i = ni; P->fl_m = nm; P->fl_s = ns; return NULL; }   /* applies to the rest of the enclosing group */
                P->p++; P->fl_i = ni; P->fl_m = nm; P->fl_s = ns;                                      /* (?i:...) scoped */
            }
        } else {
            if (P->ngroups >= RX_MAX_GROUPS) rx_fail(P, "too many groups");
            cap = ++P->ngroups;
        }
        Node *inner = parse_alt(P);
        if (P->p >= P->end || *P->p != ')') rx_fail(P, "missing closing parenthesis");
        P->p++;
        P->fl_i = sv_i; P->fl_m = sv_m; P->fl_s = sv_s;
        if (look) { Node *n = node_new(N_LOOK); n->kid = inner; n->neg = neg; return n; }
        Node *n = node_new(N_GROUP); n->kid = inner; n->c = cap; return n;
    }
    P->p++;
    switch (ch) {
        case '.': { Node *n = node_new(N_ANY); n->dotall = P->fl_s; return n; }
        case '^': { Node *n = node_new(N_BOL); n->multiline = P->fl_m; return n; }
        case '$': { Node *n = node_new(N_EOL); n->multiline = P->fl_m; return n; }
        case '[': return parse_class(P);
        case '\\': return parse_escape(P);
        case '*': case '+': case '?': rx_fail(P, "nothing to repeat");
        default: break;
    }
    if (ch >= 0xC0) {                                  /* multi-byte UTF-8 literal stays one atom */
        unsigned char b[4]; int l = 0; b[l++] = ch;
        while (l < 4 && P->p < P->end && ((unsigned char)*P->p & 0xC0) == 0x80) b[l++] = (unsigned char)*P->p++;
        return bytes_node(P, b, l);
    }
    return char_node(P, ch);
}

static Node *parse_quantified(Parser *P) {
    Node *atom = parse_atom(P);
    if (!atom) return NULL;
    for (;;) {
        if (P->p >= P->end) break;
        int min, max; char q = *P->p;
        if (q == '*') { min = 0; max = -1; P->p++; }
        else if (q == '+') { min = 1; max = -1; P->p++; }
        else if (q == '?') { min = 0; max = 1; P->p++; }
        else if (q == '{') {
            const char *save = P->p; P->p++;
            int a, b;
            if (!parse_int(P, &a)) { P->p = save; break; }          /* literal '{' */
            if (P->p < P->end && *P->p == '}') { min = max = a; P->p++; }
            else if (P->p < P->end && *P->p == ',') {
                P->p++;
                if (P->p < P->end && *P->p == '}') { min = a; max = -1; P->p++; }
                else if (parse_int(P, &b) && P->p < P->end && *P->p == '}') { min = a; max = b; P->p++; }
                else { P->p = save; break; }
            } else { P->p = save; break; }
            if (max >= 0 && max < min) rx_fail(P, "bad repeat range {n,m} with m < n");
        } else break;
        if (atom->type == N_BOL || atom->type == N_EOL || atom->type == N_WB || atom->type == N_NWB || atom->type == N_LOOK)
            rx_fail(P, "nothing to repeat (assertions cannot be quantified)");
        Node *r = node_new(N_REPEAT); r->kid = atom; r->min = min; r->max = max;
        if (P->p < P->end && *P->p == '?') { r->greedy = false; P->p++; }
        else if (P->p < P->end && *P->p == '+') rx_fail(P, "possessive quantifiers are not supported");
        atom = r;
        /* a second quantifier directly after (e.g. a**) is an error, like most engines */
        if (P->p < P->end && (*P->p == '*' || *P->p == '+' || *P->p == '?')) rx_fail(P, "nothing to repeat");
    }
    return atom;
}

static Node *parse_cat(Parser *P) {
    Node *cat = node_new(N_CAT);
    while (P->p < P->end && *P->p != '|' && *P->p != ')') {
        Node *n = parse_quantified(P);
        if (n) node_add(cat, n);
    }
    return cat;
}

static Node *parse_alt(Parser *P) {
    Node *first = parse_cat(P);
    if (P->p >= P->end || *P->p != '|') return first;
    Node *alt = node_new(N_ALT); node_add(alt, first);
    while (P->p < P->end && *P->p == '|') { P->p++; node_add(alt, parse_cat(P)); }
    return alt;
}

/* --------------------------------------------------------------- compiler */

typedef struct { Prog *pr; KcPos pos; const char *src; } Comp;

static int emit(Comp *C, int op, int x, int y) {
    Prog *pr = C->pr;
    if (pr->n >= RX_MAX_INS) rx_err(C->pos, "regx: pattern /%s/ is too large once repeats are expanded", C->src);
    if (pr->n == pr->cap) { pr->cap = pr->cap ? pr->cap * 2 : 64; pr->ins = kc_realloc(pr->ins, sizeof(Ins) * (size_t)pr->cap); }
    pr->ins[pr->n] = (Ins){ op, x, y }; return pr->n++;
}

static void compile_node(Comp *C, const Node *n) {
    switch (n->type) {
        case N_EMPTY: break;
        case N_CHAR: if (n->icase) emit(C, I_CHARI, tolower(n->c), 0); else emit(C, I_CHAR, n->c, 0); break;
        case N_ANY: emit(C, I_ANY, n->dotall, 0); break;
        case N_CLASS: emit(C, I_CLASS, n->c, 0); break;
        case N_BOL: emit(C, I_BOL, n->multiline, 0); break;
        case N_EOL: emit(C, I_EOL, n->multiline, 0); break;
        case N_WB: emit(C, I_WB, 0, 0); break;
        case N_NWB: emit(C, I_NWB, 0, 0); break;
        case N_BACKREF: emit(C, I_BACKREF, n->c, n->icase); break;
        case N_CAT: for (int i = 0; i < n->n; i++) compile_node(C, n->list[i]); break;
        case N_ALT: {
            int *jmps = kc_malloc(sizeof(int) * (size_t)n->n);
            for (int i = 0; i < n->n; i++) {
                if (i < n->n - 1) {
                    int sp = emit(C, I_SPLIT, 0, 0);
                    C->pr->ins[sp].x = C->pr->n;
                    compile_node(C, n->list[i]);
                    jmps[i] = emit(C, I_JMP, 0, 0);
                    C->pr->ins[sp].y = C->pr->n;
                } else compile_node(C, n->list[i]);
            }
            for (int i = 0; i < n->n - 1; i++) C->pr->ins[jmps[i]].x = C->pr->n;
            free(jmps); break;
        }
        case N_GROUP:
            if (n->c > 0) emit(C, I_SAVE, 2 * n->c, 0);
            compile_node(C, n->kid);
            if (n->c > 0) emit(C, I_SAVE, 2 * n->c + 1, 0);
            break;
        case N_LOOK: {
            int lk = emit(C, I_LOOK, n->neg, 0);
            compile_node(C, n->kid);
            emit(C, I_LOOKEND, 0, 0);
            C->pr->ins[lk].y = C->pr->n; break;
        }
        case N_REPEAT: {
            for (int i = 0; i < n->min; i++) compile_node(C, n->kid);
            if (n->max < 0) {
                bool guard = nullable(n->kid);
                int k = guard ? C->pr->nloop++ : 0;
                int sp = emit(C, I_SPLIT, 0, 0);
                int body = C->pr->n;
                if (guard) emit(C, I_MARK, k, 0);
                compile_node(C, n->kid);
                if (guard) emit(C, I_CHK, k, 0);
                emit(C, I_JMP, sp, 0);
                int out = C->pr->n;
                if (n->greedy) { C->pr->ins[sp].x = body; C->pr->ins[sp].y = out; }
                else           { C->pr->ins[sp].x = out;  C->pr->ins[sp].y = body; }
            } else {
                int opt = n->max - n->min;
                int *sps = kc_malloc(sizeof(int) * (size_t)(opt ? opt : 1));
                for (int i = 0; i < opt; i++) {
                    sps[i] = emit(C, I_SPLIT, 0, 0);
                    int body = C->pr->n;
                    if (n->greedy) C->pr->ins[sps[i]].x = body; else C->pr->ins[sps[i]].y = body;
                    compile_node(C, n->kid);
                }
                int out = C->pr->n;
                for (int i = 0; i < opt; i++) { if (n->greedy) C->pr->ins[sps[i]].y = out; else C->pr->ins[sps[i]].x = out; }
                free(sps);
            }
            break;
        }
    }
}

static Prog *rx_compile(const char *pattern, int plen, const char *flags, KcPos pos) {
    Prog *pr = kc_malloc(sizeof(Prog)); memset(pr, 0, sizeof *pr);
    pr->source = kc_malloc((size_t)plen + 1); memcpy(pr->source, pattern, (size_t)plen); pr->source[plen] = 0;
    Parser P; memset(&P, 0, sizeof P);
    P.src = pr->source; P.p = pr->source; P.end = pr->source + plen; P.pos = pos; P.prog = pr;
    if (flags) for (const char *f = flags; *f; f++) {
        if (*f == 'i') P.fl_i = 1; else if (*f == 'm') P.fl_m = 1; else if (*f == 's') P.fl_s = 1;
        else rx_err(pos, "regx: unknown flag '%c' (supported: i m s)", *f);
    }
    if (memchr(pr->source, 0, (size_t)plen)) rx_err(pos, "regx: pattern contains a NUL byte; use \\0");
    Node *ast = parse_alt(&P);
    if (P.p < P.end) { if (*P.p == ')') rx_fail(&P, "unmatched closing parenthesis"); rx_fail(&P, "unexpected character"); }
    pr->ngroups = P.ngroups;
    Comp C = { pr, pos, pr->source };
    emit(&C, I_SAVE, 0, 0);
    compile_node(&C, ast);
    emit(&C, I_SAVE, 1, 0);
    emit(&C, I_MATCH, 0, 0);
    node_free(ast);
    return pr;
}

/* ------------------------------------------------------------------- VM */

typedef struct { int kind; int a; long b; } Bt;          /* kind: 0 branch(pc,pos) 1 cap-restore 2 loop-restore */
typedef struct { Bt *v; long n, cap; } BtStack;

static bool bt_push(BtStack *s, int kind, int a, long b) {
    if (s->n == s->cap) {
        if (s->cap >= RX_STACK_MAX) return false;
        s->cap = s->cap ? s->cap * 2 : 1024; if (s->cap > RX_STACK_MAX) s->cap = RX_STACK_MAX;
        s->v = kc_realloc(s->v, sizeof(Bt) * (size_t)s->cap);
    }
    s->v[s->n++] = (Bt){ kind, a, b }; return true;
}

static int u8len(const char *s, long pos, long len) {
    unsigned char c = (unsigned char)s[pos]; int l = c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    if (pos + l > len) l = (int)(len - pos);
    return l;
}
static bool is_word_at(const char *s, long pos, long len) { return pos >= 0 && pos < len && is_word_byte((unsigned char)s[pos]); }

#define RX_FAIL_LIMIT (-2)

static long vm_run(const Prog *P, const char *s, long len, int pc, long pos, long *caps, long *lr, BtStack *st, long *steps) {
    long base = st->n;
    for (;;) {
        if (++*steps > RX_STEP_LIMIT) return RX_FAIL_LIMIT;
        const Ins *I = &P->ins[pc];
        switch (I->op) {
            case I_CHAR:  if (pos < len && (unsigned char)s[pos] == I->x) { pos++; pc++; continue; } goto fail;
            case I_CHARI: if (pos < len && tolower((unsigned char)s[pos]) == I->x) { pos++; pc++; continue; } goto fail;
            case I_ANY:
                if (pos < len && (I->x || s[pos] != '\n')) { pos += u8len(s, pos, len); pc++; continue; } goto fail;
            case I_CLASS: {
                if (pos >= len) goto fail;
                unsigned char c = (unsigned char)s[pos]; const RxClass *k = &P->cls[I->x];
                if (c < 0x80) { if (cls_has(k, c)) { pos++; pc++; continue; } goto fail; }
                if (k->high) { pos += u8len(s, pos, len); pc++; continue; }
                goto fail;
            }
            case I_SPLIT: if (!bt_push(st, 0, I->y, pos)) return RX_FAIL_LIMIT; pc = I->x; continue;
            case I_JMP: pc = I->x; continue;
            case I_SAVE: if (!bt_push(st, 1, I->x, caps[I->x])) return RX_FAIL_LIMIT; caps[I->x] = pos; pc++; continue;
            case I_MARK: if (!bt_push(st, 2, I->x, lr[I->x])) return RX_FAIL_LIMIT; lr[I->x] = pos; pc++; continue;
            case I_CHK: if (lr[I->x] == pos) goto fail; pc++; continue;
            case I_BOL: if (pos == 0 || (I->x && s[pos - 1] == '\n')) { pc++; continue; } goto fail;
            case I_EOL: if (pos == len || (I->x && s[pos] == '\n')) { pc++; continue; } goto fail;
            case I_WB:  if (is_word_at(s, pos - 1, len) != is_word_at(s, pos, len)) { pc++; continue; } goto fail;
            case I_NWB: if (is_word_at(s, pos - 1, len) == is_word_at(s, pos, len)) { pc++; continue; } goto fail;
            case I_BACKREF: {
                long a = caps[2 * I->x], b = caps[2 * I->x + 1];
                if (a < 0 || b < 0) goto fail;                       /* group did not participate */
                long l = b - a;
                if (pos + l > len) goto fail;
                for (long i = 0; i < l; i++) {
                    unsigned char x = (unsigned char)s[a + i], y = (unsigned char)s[pos + i];
                    if (I->y ? tolower(x) != tolower(y) : x != y) goto fail;
                }
                pos += l; pc++; continue;
            }
            case I_LOOK: {
                long inner = st->n;
                long r = vm_run(P, s, len, pc + 1, pos, caps, lr, st, steps);
                if (r == RX_FAIL_LIMIT) return RX_FAIL_LIMIT;
                if (r >= 0) {
                    if (I->x) {                                       /* negative look: inner matched -> fail, undo captures */
                        while (st->n > inner) { Bt e = st->v[--st->n]; if (e.kind == 1) caps[e.a] = e.b; else if (e.kind == 2) lr[e.a] = e.b; }
                        goto fail;
                    }
                    long w = inner;                                    /* positive look: keep capture undo records, drop branches */
                    for (long i = inner; i < st->n; i++) if (st->v[i].kind != 0) st->v[w++] = st->v[i];
                    st->n = w; pc = I->y; continue;
                }
                if (I->x) { pc = I->y; continue; }
                goto fail;
            }
            case I_LOOKEND: case I_MATCH: return pos;
        }
    fail:
        for (;;) {
            if (st->n == base) return -1;
            Bt e = st->v[--st->n];
            if (e.kind == 0) { pc = e.a; pos = e.b; break; }
            else if (e.kind == 1) caps[e.a] = e.b;
            else lr[e.a] = e.b;
        }
    }
}

typedef struct { long *caps; long *lr; BtStack st; int ncap; } Matcher;

static void matcher_init(Matcher *M, const Prog *P) {
    M->ncap = 2 * (P->ngroups + 1);
    M->caps = kc_malloc(sizeof(long) * (size_t)M->ncap);
    M->lr = kc_malloc(sizeof(long) * (size_t)(P->nloop ? P->nloop : 1));
    memset(&M->st, 0, sizeof M->st);
}
static void matcher_free(Matcher *M) { free(M->caps); free(M->lr); free(M->st.v); }

/* Finds the leftmost match starting at or after `from`. caps[0..1] = overall span. */
static bool rx_search(const Prog *P, Matcher *M, const char *s, long len, long from, KcPos pos) {
    long steps = 0;
    int first = -1;                                   /* literal first byte shortcut */
    if (P->ins[1].op == I_CHAR) first = P->ins[1].x;
    for (long start = from; start <= len; start++) {
        if (first >= 0) {
            const char *f = start < len ? memchr(s + start, first, (size_t)(len - start)) : NULL;
            if (!f) return false;
            start = f - s;
        }
        for (int i = 0; i < M->ncap; i++) M->caps[i] = -1;
        for (int i = 0; i < P->nloop; i++) M->lr[i] = -1;
        M->st.n = 0;
        long r = vm_run(P, s, len, 0, start, M->caps, M->lr, &M->st, &steps);
        if (r == RX_FAIL_LIMIT) kc_error_at(pos, "regx: matching /%s/ exceeded the backtracking limit (pattern too complex for this input)", P->source);
        if (r >= 0) return true;
        if (start < len && (unsigned char)s[start] >= 0xC0) while (start + 1 < len && ((unsigned char)s[start + 1] & 0xC0) == 0x80) start++;
    }
    return false;
}

/* --------------------------------------------------------- cache + glue */

#define CACHE_N 24
static struct { char *key; int klen; Prog *prog; } cache[CACHE_N];
static int cache_next = 0;

static Prog *compile_cached(const char *pat, int plen, const char *flags, KcPos pos) {
    int fl = flags ? (int)strlen(flags) : 0;
    int klen = plen + 1 + fl;
    char *key = kc_malloc((size_t)klen + 1);
    memcpy(key, pat, (size_t)plen); key[plen] = 1; if (fl) memcpy(key + plen + 1, flags, (size_t)fl); key[klen] = 0;
    for (int i = 0; i < CACHE_N; i++)
        if (cache[i].key && cache[i].klen == klen && memcmp(cache[i].key, key, (size_t)klen) == 0) { free(key); return cache[i].prog; }
    Prog *p = rx_compile(pat, plen, flags, pos);
    free(cache[cache_next].key);                      /* evicted Prog is left allocated: a handle may still own it */
    cache[cache_next].key = key; cache[cache_next].klen = klen; cache[cache_next].prog = p;
    cache_next = (cache_next + 1) % CACHE_N;
    return p;
}

static Prog *get_prog(KcValue v, KcPos pos, const char *who) {
    if (v.type == VAL_HANDLE && v.as.handle->kind == H_REGEX) return (Prog *)v.as.handle->ptr;
    if (v.type == VAL_STRING) return compile_cached(v.as.s->data, v.as.s->len, NULL, pos);
    kc_error_at(pos, "%s: first argument must be a pattern string or a regx.compile() handle", who);
    return NULL;
}
static KcString *get_str(KcValue v, KcPos pos, const char *who) {
    if (v.type != VAL_STRING) kc_error_at(pos, "%s: expected a string, got %s", who, kc_type_name(v));
    return v.as.s;
}
static long get_start(KcValue *a, int n, int idx, long len, KcPos pos, const char *who) {
    if (n <= idx) return 0;
    if (a[idx].type != VAL_INT) kc_error_at(pos, "%s: start must be an int", who);
    long s = (long)a[idx].as.i;
    if (s < 0) s += len;
    if (s < 0) s = 0;
    if (s > len) s = len + 1;                          /* past the end: no match */
    return s;
}
static void need(int n, int lo, int hi, KcPos pos, const char *sig) {
    if (n < lo || n > hi) kc_error_at(pos, "usage: regx.%s", sig);
}

static KcValue substr(const char *s, long a, long b) {
    if (a < 0 || b < 0) return kc_null();
    char *buf = kc_malloc((size_t)(b - a) + 1); memcpy(buf, s + a, (size_t)(b - a)); buf[b - a] = 0;
    return kc_string_take(buf, (int)(b - a));
}
static KcValue groups_array(const Prog *P, const Matcher *M, const char *s) {
    KcValue arr = kc_array_new();
    for (int g = 0; g <= P->ngroups; g++) kc_array_push(arr, substr(s, M->caps[2 * g], M->caps[2 * g + 1]));
    return arr;
}
static long next_start(const char *s, long len, long end, long start_of_match) {
    if (end > start_of_match) return end;               /* empty match: step one code point forward */
    if (end >= len) return len + 1;
    return end + u8len(s, end, len);
}

/* ------------------------------------------------------------- functions */

static KcValue rx_f_compile(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 2, pos, "compile(pattern[, flags])");
    KcString *p = get_str(a[0], pos, "regx.compile");
    const char *fl = n == 2 ? get_str(a[1], pos, "regx.compile")->data : NULL;
    Prog *pr = rx_compile(p->data, p->len, fl, pos);
    KcHandle *h = kc_malloc(sizeof(KcHandle));
    h->kind = H_REGEX; h->ptr = pr; h->size = 0; h->extra = 0;
    KcValue v; v.type = VAL_HANDLE; v.as.handle = h; return v;
}

static KcValue rx_f_is_valid(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 2, pos, "is_valid(pattern[, flags])");
    KcString *p = get_str(a[0], pos, "regx.is_valid");
    const char *fl = n == 2 ? get_str(a[1], pos, "regx.is_valid")->data : NULL;
    jmp_buf jb; jmp_buf *prev = g_probe_jmp;
    g_probe_jmp = &jb;
    if (setjmp(jb) != 0) { g_probe_jmp = prev; return kc_bool(false); }
    rx_compile(p->data, p->len, fl, pos);
    g_probe_jmp = prev;
    return kc_bool(true);
}

static KcValue rx_f_escape(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 1, pos, "escape(text)");
    KcString *s = get_str(a[0], pos, "regx.escape");
    char *out = kc_malloc((size_t)s->len * 2 + 1); int o = 0;
    for (int i = 0; i < s->len; i++) {
        unsigned char c = (unsigned char)s->data[i];
        if (c < 0x80 && strchr("\\.^$|?*+()[]{}-/", c) && c) out[o++] = '\\';
        out[o++] = (char)c;
    }
    out[o] = 0; return kc_string_take(out, o);
}

static KcValue rx_f_test(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 2, 2, pos, "test(re, text)");
    Prog *P = get_prog(a[0], pos, "regx.test"); KcString *s = get_str(a[1], pos, "regx.test");
    Matcher M; matcher_init(&M, P);
    bool ok = rx_search(P, &M, s->data, s->len, 0, pos); matcher_free(&M);
    return kc_bool(ok);
}
static KcValue rx_f_full(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 2, 2, pos, "full(re, text)  (the whole text must match)");
    Prog *P = get_prog(a[0], pos, "regx.full"); KcString *s = get_str(a[1], pos, "regx.full");
    /* leftmost-first matching can stop short of the end, so try each start with an end anchor appended logically:
       search for any match whose span is [0,len] by retrying from every alternative via a wrapper program. */
    char *wrapped = kc_malloc(strlen(P->source) + 12);
    sprintf(wrapped, "\\A(?:%s)\\z", P->source);
    Prog *W = compile_cached(wrapped, (int)strlen(wrapped), NULL, pos); free(wrapped);
    Matcher M; matcher_init(&M, W);
    bool ok = rx_search(W, &M, s->data, s->len, 0, pos); matcher_free(&M);
    return kc_bool(ok);
}
static KcValue rx_f_find(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 2, 3, pos, "find(re, text[, start]) -> string or null");
    Prog *P = get_prog(a[0], pos, "regx.find"); KcString *s = get_str(a[1], pos, "regx.find");
    long st = get_start(a, n, 2, s->len, pos, "regx.find");
    if (st > s->len) return kc_null();
    Matcher M; matcher_init(&M, P);
    KcValue r = rx_search(P, &M, s->data, s->len, st, pos) ? substr(s->data, M.caps[0], M.caps[1]) : kc_null();
    matcher_free(&M); return r;
}
static KcValue rx_f_index(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 2, 3, pos, "index(re, text[, start]) -> byte offset or -1");
    Prog *P = get_prog(a[0], pos, "regx.index"); KcString *s = get_str(a[1], pos, "regx.index");
    long st = get_start(a, n, 2, s->len, pos, "regx.index");
    if (st > s->len) return kc_int(-1);
    Matcher M; matcher_init(&M, P);
    long r = rx_search(P, &M, s->data, s->len, st, pos) ? M.caps[0] : -1;
    matcher_free(&M); return kc_int(r);
}
static KcValue rx_f_span(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 2, 3, pos, "span(re, text[, start]) -> [start, end] or null");
    Prog *P = get_prog(a[0], pos, "regx.span"); KcString *s = get_str(a[1], pos, "regx.span");
    long st = get_start(a, n, 2, s->len, pos, "regx.span");
    if (st > s->len) return kc_null();
    Matcher M; matcher_init(&M, P);
    KcValue r = kc_null();
    if (rx_search(P, &M, s->data, s->len, st, pos)) { r = kc_array_new(); kc_array_push(r, kc_int(M.caps[0])); kc_array_push(r, kc_int(M.caps[1])); }
    matcher_free(&M); return r;
}
static KcValue rx_f_groups(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 2, 3, pos, "groups(re, text[, start]) -> [whole, g1, g2, ...] or null");
    Prog *P = get_prog(a[0], pos, "regx.groups"); KcString *s = get_str(a[1], pos, "regx.groups");
    long st = get_start(a, n, 2, s->len, pos, "regx.groups");
    if (st > s->len) return kc_null();
    Matcher M; matcher_init(&M, P);
    KcValue r = rx_search(P, &M, s->data, s->len, st, pos) ? groups_array(P, &M, s->data) : kc_null();
    matcher_free(&M); return r;
}
static KcValue rx_f_group(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 3, 3, pos, "group(re, text, name_or_index) -> string or null");
    Prog *P = get_prog(a[0], pos, "regx.group"); KcString *s = get_str(a[1], pos, "regx.group");
    int g = -1;
    if (a[2].type == VAL_INT) { g = (int)a[2].as.i; if (g < 0 || g > P->ngroups) kc_error_at(pos, "regx.group: pattern has %d group(s), asked for %d", P->ngroups, g); }
    else if (a[2].type == VAL_STRING) {
        for (int i = 1; i <= P->ngroups; i++) if (P->names[i] && strcmp(P->names[i], a[2].as.s->data) == 0) g = i;
        if (g < 0) kc_error_at(pos, "regx.group: no group named '%s'", a[2].as.s->data);
    } else kc_error_at(pos, "regx.group: third argument must be a group number or name");
    Matcher M; matcher_init(&M, P);
    KcValue r = kc_null();
    if (rx_search(P, &M, s->data, s->len, 0, pos)) r = substr(s->data, M.caps[2 * g], M.caps[2 * g + 1]);
    matcher_free(&M); return r;
}
static KcValue rx_f_names(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 1, pos, "names(re) -> array of group names (\"\" for unnamed groups)");
    Prog *P = get_prog(a[0], pos, "regx.names");
    KcValue arr = kc_array_new();
    for (int g = 1; g <= P->ngroups; g++) kc_array_push(arr, kc_string(P->names[g] ? P->names[g] : ""));
    return arr;
}
static KcValue rx_f_group_count(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 1, pos, "group_count(re)");
    return kc_int(get_prog(a[0], pos, "regx.group_count")->ngroups);
}
static KcValue rx_f_pattern(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 1, pos, "pattern(re)");
    return kc_string(get_prog(a[0], pos, "regx.pattern")->source);
}

static KcValue find_all_impl(KcValue *a, int n, KcPos pos, bool with_groups, const char *who, const char *sig) {
    need(n, 2, 2, pos, sig);
    Prog *P = get_prog(a[0], pos, who); KcString *s = get_str(a[1], pos, who);
    Matcher M; matcher_init(&M, P);
    KcValue out = kc_array_new(); long at = 0;
    while (at <= s->len && rx_search(P, &M, s->data, s->len, at, pos)) {
        kc_array_push(out, with_groups ? groups_array(P, &M, s->data) : substr(s->data, M.caps[0], M.caps[1]));
        at = next_start(s->data, s->len, M.caps[1], M.caps[0]);
    }
    matcher_free(&M); return out;
}
static KcValue rx_f_find_all(KcInterp *ip, KcValue *a, int n, KcPos pos) { return find_all_impl(a, n, pos, false, "regx.find_all", "find_all(re, text) -> string[]"); }
static KcValue rx_f_find_all_groups(KcInterp *ip, KcValue *a, int n, KcPos pos) { return find_all_impl(a, n, pos, true, "regx.find_all_groups", "find_all_groups(re, text) -> string[][]"); }
static KcValue rx_f_count(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    KcValue r = find_all_impl(a, n, pos, false, "regx.count", "count(re, text) -> int");
    return kc_int(r.as.arr->count);
}

/* growable output buffer */
typedef struct { char *d; long n, cap; } Buf;
static void buf_put(Buf *b, const char *s, long len) {
    if (b->n + len + 1 > b->cap) { b->cap = (b->n + len + 1) * 2; b->d = kc_realloc(b->d, (size_t)b->cap); }
    memcpy(b->d + b->n, s, (size_t)len); b->n += len; b->d[b->n] = 0;
}

static void expand_template(Buf *out, const Prog *P, const Matcher *M, const char *s, const char *t, long tl, KcPos pos) {
    for (long i = 0; i < tl; i++) {
        if (t[i] != '$' || i + 1 >= tl) { buf_put(out, t + i, 1); continue; }
        char c = t[i + 1];
        if (c == '$') { buf_put(out, "$", 1); i++; continue; }
        if (c == '&') { buf_put(out, s + M->caps[0], M->caps[1] - M->caps[0]); i++; continue; }
        int g = -1;
        if (isdigit((unsigned char)c)) {
            g = c - '0'; long j = i + 2;
            if (j < tl && isdigit((unsigned char)t[j]) && g * 10 + (t[j] - '0') <= P->ngroups) { g = g * 10 + (t[j] - '0'); j++; }
            i = j - 1;
        } else if (c == '{') {
            long j = i + 2; while (j < tl && t[j] != '}') j++;
            if (j >= tl) { buf_put(out, t + i, 1); continue; }
            long nl = j - (i + 2); const char *nm = t + i + 2;
            bool num = nl > 0; for (long k = 0; k < nl; k++) if (!isdigit((unsigned char)nm[k])) num = false;
            if (num) g = atoi(nm);
            else { for (int q = 1; q <= P->ngroups; q++) if (P->names[q] && (long)strlen(P->names[q]) == nl && memcmp(P->names[q], nm, (size_t)nl) == 0) g = q;
                   if (g < 0) kc_error_at(pos, "regx.replace: replacement refers to unknown group name '%.*s'", (int)nl, nm); }
            i = j;
        } else { buf_put(out, t + i, 1); continue; }
        if (g > P->ngroups) kc_error_at(pos, "regx.replace: replacement refers to group %d but the pattern has %d group(s)", g, P->ngroups);
        if (M->caps[2 * g] >= 0) buf_put(out, s + M->caps[2 * g], M->caps[2 * g + 1] - M->caps[2 * g]);
    }
}

static KcValue replace_impl(KcInterp *ip, KcValue *a, int n, KcPos pos, bool all, const char *who, const char *sig) {
    need(n, 3, 3, pos, sig);
    Prog *P = get_prog(a[0], pos, who); KcString *s = get_str(a[1], pos, who);
    bool fn = a[2].type == VAL_CLOSURE || a[2].type == VAL_NATIVE;
    KcString *t = fn ? NULL : get_str(a[2], pos, who);
    Matcher M; matcher_init(&M, P);
    Buf out = {0}; long at = 0, copied = 0;
    while (at <= s->len && rx_search(P, &M, s->data, s->len, at, pos)) {
        long ms = M.caps[0], me = M.caps[1];
        buf_put(&out, s->data + copied, ms - copied);
        if (fn) {
            KcValue g = groups_array(P, &M, s->data);
            KcValue r = kc_call_value(ip, a[2], &g, 1, pos);
            if (r.type != VAL_STRING) kc_error_at(pos, "%s: callback must return a string, got %s", who, kc_type_name(r));
            buf_put(&out, r.as.s->data, r.as.s->len);
        } else expand_template(&out, P, &M, s->data, t->data, t->len, pos);
        copied = me;
        at = next_start(s->data, s->len, me, ms);
        if (!all) break;
    }
    buf_put(&out, s->data + copied, s->len - copied);
    matcher_free(&M);
    return kc_string_take(out.d, (int)out.n);
}
static KcValue rx_f_replace(KcInterp *ip, KcValue *a, int n, KcPos pos) { return replace_impl(ip, a, n, pos, true, "regx.replace", "replace(re, text, replacement)  ($1 ${name} $& $$)"); }
static KcValue rx_f_replace_first(KcInterp *ip, KcValue *a, int n, KcPos pos) { return replace_impl(ip, a, n, pos, false, "regx.replace_first", "replace_first(re, text, replacement)"); }
static KcValue rx_f_replace_fn(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    if (n == 3 && a[2].type != VAL_CLOSURE && a[2].type != VAL_NATIVE) kc_error_at(pos, "regx.replace_fn: third argument must be a function(groups) -> string");
    return replace_impl(ip, a, n, pos, true, "regx.replace_fn", "replace_fn(re, text, fn(groups) -> string)");
}

static KcValue rx_f_split(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 2, 3, pos, "split(re, text[, limit]) -> string[]  (captured groups are included, like Perl/JS)");
    Prog *P = get_prog(a[0], pos, "regx.split"); KcString *s = get_str(a[1], pos, "regx.split");
    long limit = -1;
    if (n == 3) { if (a[2].type != VAL_INT) kc_error_at(pos, "regx.split: limit must be an int"); limit = (long)a[2].as.i; }
    KcValue out = kc_array_new();
    if (limit == 0) return out;
    Matcher M; matcher_init(&M, P);
    long at = 0, piece = 0;
    while (at <= s->len && (limit < 0 || out.as.arr->count < limit - 1) && rx_search(P, &M, s->data, s->len, at, pos)) {
        long ms = M.caps[0], me = M.caps[1];
        if (me == ms && ms >= s->len) break;                 /* empty match at the very end adds nothing */
        if (me == ms && ms == 0) { at = next_start(s->data, s->len, me, ms); continue; }   /* no leading empty piece for empty matches */
        kc_array_push(out, substr(s->data, piece, ms));
        for (int g = 1; g <= P->ngroups; g++) kc_array_push(out, substr(s->data, M.caps[2 * g], M.caps[2 * g + 1]));
        piece = me; at = next_start(s->data, s->len, me, ms);
    }
    kc_array_push(out, substr(s->data, piece, s->len));
    matcher_free(&M); return out;
}

static KcModuleEntry ENTRIES[] = {
    {"compile", rx_f_compile}, {"is_valid", rx_f_is_valid}, {"escape", rx_f_escape},
    {"test", rx_f_test}, {"full", rx_f_full}, {"find", rx_f_find}, {"index", rx_f_index}, {"span", rx_f_span},
    {"groups", rx_f_groups}, {"group", rx_f_group}, {"names", rx_f_names}, {"group_count", rx_f_group_count}, {"pattern", rx_f_pattern},
    {"find_all", rx_f_find_all}, {"find_all_groups", rx_f_find_all_groups}, {"count", rx_f_count},
    {"replace", rx_f_replace}, {"replace_first", rx_f_replace_first}, {"replace_fn", rx_f_replace_fn}, {"split", rx_f_split},
};

KcModule *kc_mod_regx(void) {
    static KcModule m = { "regx", ENTRIES, sizeof(ENTRIES)/sizeof(ENTRIES[0]) };
    return &m;
}
