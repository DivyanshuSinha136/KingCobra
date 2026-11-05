/* KingCobra std.json — strict, fast, native JSON.
 *
 *  - RFC 8259 parser: full UTF-8 validation, \uXXXX + surrogate pairs, exact
 *    int64 integers (anything else becomes a float), precise line/column errors,
 *    nesting limit, optional comments / trailing commas / NaN / Infinity.
 *  - Objects map to KingCobra open objects (see kc_object_* in value.h): insertion
 *    order is preserved, duplicate keys keep the last value in the first slot.
 *  - Serializer: compact or pretty, sorted keys, ASCII-only output, shortest
 *    round-trip float formatting (a float always prints with '.' or 'e').
 *  - Token-level formatter (minify / format) that never changes numbers or strings.
 *  - JSON Lines (ndjson) helpers and whole-file helpers.
 *
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com>
 */
#include "json_internal.h"
#include <errno.h>
#include <math.h>
#include <ctype.h>

/* ============================================================ byte buffer */

void jb_init(JBuf *b) { b->p = NULL; b->len = 0; b->cap = 0; }

static void jb_reserve(JBuf *b, size_t extra) {
    if (b->len + extra + 1 <= b->cap) return;
    size_t nc = b->cap ? b->cap : 64;
    while (nc < b->len + extra + 1) nc *= 2;
    b->p = kc_realloc(b->p, nc);
    b->cap = nc;
}
void jb_putc(JBuf *b, char c) { jb_reserve(b, 1); b->p[b->len++] = c; }
void jb_put(JBuf *b, const char *s, size_t n) {
    if (!n) return;
    jb_reserve(b, n);
    memcpy(b->p + b->len, s, n);
    b->len += n;
}
void jb_puts(JBuf *b, const char *s) { jb_put(b, s, strlen(s)); }
void jb_printf(JBuf *b, const char *fmt, ...) {
    char tmp[512];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n < sizeof tmp) { jb_put(b, tmp, (size_t)n); return; }
    char *big = kc_malloc((size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    jb_put(b, big, (size_t)n);
    free(big);
}
char *jb_take(JBuf *b, size_t *len) {
    jb_reserve(b, 0);
    if (!b->p) { b->p = kc_malloc(1); }
    b->p[b->len] = 0;
    if (len) *len = b->len;
    char *r = b->p;
    b->p = NULL; b->len = b->cap = 0;
    return r;
}
KcValue jb_to_string(JBuf *b) {
    size_t n;
    char *s = jb_take(b, &n);
    return kc_string_adopt(s, (int)n);
}

/* ============================================================ UTF-8 */

/* Length of the valid UTF-8 sequence starting at s (rem bytes available), or 0 if invalid. */
static int utf8_len(const unsigned char *s, size_t rem) {
    unsigned char c = s[0];
    if (c < 0x80) return 1;
    if (c >= 0xC2 && c <= 0xDF) return (rem >= 2 && (s[1] & 0xC0) == 0x80) ? 2 : 0;
    if (c >= 0xE0 && c <= 0xEF) {
        if (rem < 3 || (s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80) return 0;
        if (c == 0xE0 && s[1] < 0xA0) return 0;      /* overlong */
        if (c == 0xED && s[1] >= 0xA0) return 0;     /* UTF-16 surrogates */
        return 3;
    }
    if (c >= 0xF0 && c <= 0xF4) {
        if (rem < 4 || (s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80 || (s[3] & 0xC0) != 0x80) return 0;
        if (c == 0xF0 && s[1] < 0x90) return 0;      /* overlong */
        if (c == 0xF4 && s[1] >= 0x90) return 0;     /* > U+10FFFF */
        return 4;
    }
    return 0;
}

static void put_utf8(JBuf *b, unsigned cp) {
    if (cp < 0x80) jb_putc(b, (char)cp);
    else if (cp < 0x800) { jb_putc(b, (char)(0xC0 | (cp >> 6))); jb_putc(b, (char)(0x80 | (cp & 0x3F))); }
    else if (cp < 0x10000) {
        jb_putc(b, (char)(0xE0 | (cp >> 12)));
        jb_putc(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
        jb_putc(b, (char)(0x80 | (cp & 0x3F)));
    } else {
        jb_putc(b, (char)(0xF0 | (cp >> 18)));
        jb_putc(b, (char)(0x80 | ((cp >> 12) & 0x3F)));
        jb_putc(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
        jb_putc(b, (char)(0x80 | (cp & 0x3F)));
    }
}

/* ============================================================ parser */

typedef struct {
    const char *s; size_t n, i;
    const JParseOpts *o;
    JError *err;
    bool failed;
} P;

void json_default_parse_opts(JParseOpts *o) {
    o->comments = false; o->trailing_commas = false; o->nan = false; o->dup_error = false; o->max_depth = 512;
}

static void p_fail(P *p, size_t at, const char *fmt, ...) {
    if (p->failed) return;
    p->failed = true;
    va_list ap; va_start(ap, fmt);
    vsnprintf(p->err->msg, sizeof p->err->msg, fmt, ap);
    va_end(ap);
    int line = 1, col = 1;
    for (size_t k = 0; k < at && k < p->n; k++) {
        if (p->s[k] == '\n') { line++; col = 1; }
        else if (((unsigned char)p->s[k] & 0xC0) != 0x80) col++;
    }
    p->err->line = line; p->err->col = col;
}

static bool skip_ws(P *p) {
    for (;;) {
        while (p->i < p->n) {
            char c = p->s[p->i];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') p->i++; else break;
        }
        if (p->o->comments && p->i + 1 < p->n && p->s[p->i] == '/') {
            if (p->s[p->i + 1] == '/') {
                p->i += 2;
                while (p->i < p->n && p->s[p->i] != '\n') p->i++;
                continue;
            }
            if (p->s[p->i + 1] == '*') {
                size_t st = p->i; p->i += 2;
                for (;;) {
                    if (p->i + 1 >= p->n) { p_fail(p, st, "unterminated /* comment"); return false; }
                    if (p->s[p->i] == '*' && p->s[p->i + 1] == '/') { p->i += 2; break; }
                    p->i++;
                }
                continue;
            }
        }
        return true;
    }
}

static bool p_hex4(P *p, size_t at, unsigned *out) {
    if (at + 4 > p->n) { p_fail(p, at, "incomplete \\u escape"); return false; }
    unsigned v = 0;
    for (int k = 0; k < 4; k++) {
        char c = p->s[at + (size_t)k];
        unsigned d;
        if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (unsigned)(c - 'A' + 10);
        else { p_fail(p, at + (size_t)k, "invalid hex digit in \\u escape"); return false; }
        v = v * 16 + d;
    }
    *out = v;
    return true;
}

/* Parses a string token starting at the opening quote. With out == NULL it only validates. */
static bool p_string(P *p, JBuf *out) {
    size_t start = p->i;
    p->i++;
    for (;;) {
        size_t seg = p->i;
        while (p->i < p->n) {
            unsigned char c = (unsigned char)p->s[p->i];
            if (c == '"' || c == '\\' || c < 0x20 || c >= 0x80) break;
            p->i++;
        }
        if (out && p->i > seg) jb_put(out, p->s + seg, p->i - seg);
        if (p->i >= p->n) { p_fail(p, start, "unterminated string"); return false; }
        unsigned char c = (unsigned char)p->s[p->i];
        if (c == '"') { p->i++; return true; }
        if (c < 0x20) { p_fail(p, p->i, "control character in string (must be escaped)"); return false; }
        if (c >= 0x80) {
            int l = utf8_len((const unsigned char *)p->s + p->i, p->n - p->i);
            if (!l) { p_fail(p, p->i, "invalid UTF-8 in string"); return false; }
            if (out) jb_put(out, p->s + p->i, (size_t)l);
            p->i += (size_t)l;
            continue;
        }
        /* backslash escape */
        if (p->i + 1 >= p->n) { p_fail(p, start, "unterminated string"); return false; }
        char e = p->s[p->i + 1];
        switch (e) {
            case '"': case '\\': case '/': if (out) jb_putc(out, e); p->i += 2; break;
            case 'b': if (out) jb_putc(out, '\b'); p->i += 2; break;
            case 'f': if (out) jb_putc(out, '\f'); p->i += 2; break;
            case 'n': if (out) jb_putc(out, '\n'); p->i += 2; break;
            case 'r': if (out) jb_putc(out, '\r'); p->i += 2; break;
            case 't': if (out) jb_putc(out, '\t'); p->i += 2; break;
            case 'u': {
                unsigned cp;
                size_t esc_at = p->i;
                if (!p_hex4(p, p->i + 2, &cp)) return false;
                p->i += 6;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    unsigned lo;
                    if (p->i + 1 < p->n && p->s[p->i] == '\\' && p->s[p->i + 1] == 'u') {
                        if (!p_hex4(p, p->i + 2, &lo)) return false;
                        if (lo < 0xDC00 || lo > 0xDFFF) { p_fail(p, esc_at, "high surrogate not followed by a low surrogate"); return false; }
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        p->i += 6;
                    } else { p_fail(p, esc_at, "high surrogate not followed by a low surrogate"); return false; }
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    p_fail(p, esc_at, "unpaired low surrogate in \\u escape"); return false;
                }
                if (cp == 0) { p_fail(p, esc_at, "\\u0000 is not supported (KingCobra strings cannot contain NUL)"); return false; }
                if (out) put_utf8(out, cp);
                break;
            }
            default:
                p_fail(p, p->i, "invalid escape '\\%c'", (e >= 0x20 && e < 0x7f) ? e : '?');
                return false;
        }
    }
}

/* Scans a number lexeme per RFC 8259. Sets [*st,*en) and *isint. */
static bool p_number_scan(P *p, size_t *st, size_t *en, bool *isint) {
    size_t i = p->i;
    *st = i;
    if (i < p->n && p->s[i] == '-') i++;
    if (i >= p->n) { p_fail(p, p->i, "invalid number"); return false; }
    if (p->s[i] == '0') {
        i++;
        if (i < p->n && isdigit((unsigned char)p->s[i])) { p_fail(p, p->i, "leading zeros are not allowed in numbers"); return false; }
    } else if (p->s[i] >= '1' && p->s[i] <= '9') {
        while (i < p->n && isdigit((unsigned char)p->s[i])) i++;
    } else { p_fail(p, p->i, "invalid number"); return false; }
    *isint = true;
    if (i < p->n && p->s[i] == '.') {
        *isint = false; i++;
        if (i >= p->n || !isdigit((unsigned char)p->s[i])) { p_fail(p, i, "digit expected after the decimal point"); return false; }
        while (i < p->n && isdigit((unsigned char)p->s[i])) i++;
    }
    if (i < p->n && (p->s[i] == 'e' || p->s[i] == 'E')) {
        *isint = false; i++;
        if (i < p->n && (p->s[i] == '+' || p->s[i] == '-')) i++;
        if (i >= p->n || !isdigit((unsigned char)p->s[i])) { p_fail(p, i, "digit expected in the exponent"); return false; }
        while (i < p->n && isdigit((unsigned char)p->s[i])) i++;
    }
    *en = i;
    p->i = i;
    return true;
}

static bool p_number(P *p, KcValue *out) {
    size_t st, en, at = p->i; bool isint;
    if (!p_number_scan(p, &st, &en, &isint)) return false;
    size_t len = en - st;
    char small[64];
    char *tmp = len < sizeof small ? small : kc_malloc(len + 1);
    memcpy(tmp, p->s + st, len); tmp[len] = 0;
    bool ok = true;
    if (isint) {
        errno = 0;
        char *end;
        long long v = strtoll(tmp, &end, 10);
        if (errno == ERANGE) { *out = kc_float(strtod(tmp, NULL)); }   /* beyond int64: keep as float */
        else *out = kc_int((int64_t)v);
    } else {
        errno = 0;
        double d = strtod(tmp, NULL);
        if (isinf(d)) { p_fail(p, at, "number out of range"); ok = false; }
        else *out = kc_float(d);
    }
    if (tmp != small) free(tmp);
    return ok;
}

static bool p_literal(P *p, const char *word, KcValue v, KcValue *out) {
    size_t l = strlen(word);
    if (p->i + l <= p->n && memcmp(p->s + p->i, word, l) == 0) { p->i += l; *out = v; return true; }
    p_fail(p, p->i, "invalid literal (expected '%s')", word);
    return false;
}

/* ---- object builder: insertion-ordered, with a hash index once large ---- */
typedef struct {
    char **names; size_t *klens; KcValue *vals; int count, cap;
    int *tab; int tabcap;
} OB;

static uint32_t fnv32(const char *s, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= (unsigned char)s[i]; h *= 16777619u; }
    return h;
}
static void ob_rehash(OB *ob) {
    free(ob->tab);
    int cap = 32;
    while (cap < ob->count * 2 + 2) cap *= 2;
    ob->tab = kc_malloc(sizeof(int) * (size_t)cap);
    for (int i = 0; i < cap; i++) ob->tab[i] = -1;
    ob->tabcap = cap;
    for (int i = 0; i < ob->count; i++) {
        uint32_t h = fnv32(ob->names[i], ob->klens[i]) & (uint32_t)(cap - 1);
        while (ob->tab[h] >= 0) h = (h + 1) & (uint32_t)(cap - 1);
        ob->tab[h] = i;
    }
}
static int ob_find(OB *ob, const char *k, size_t kl) {
    if (ob->tab) {
        uint32_t h = fnv32(k, kl) & (uint32_t)(ob->tabcap - 1);
        while (ob->tab[h] >= 0) {
            int i = ob->tab[h];
            if (ob->klens[i] == kl && memcmp(ob->names[i], k, kl) == 0) return i;
            h = (h + 1) & (uint32_t)(ob->tabcap - 1);
        }
        return -1;
    }
    for (int i = 0; i < ob->count; i++)
        if (ob->klens[i] == kl && memcmp(ob->names[i], k, kl) == 0) return i;
    return -1;
}
/* Takes ownership of key (NUL-terminated, length kl). Returns the index it landed at, -1 if it was a duplicate. */
static int ob_put(OB *ob, char *key, size_t kl, KcValue v, bool *was_dup) {
    int at = ob_find(ob, key, kl);
    if (at >= 0) { ob->vals[at] = v; free(key); *was_dup = true; return -1; }
    *was_dup = false;
    if (ob->count == ob->cap) {
        ob->cap = ob->cap ? ob->cap * 2 : 4;
        ob->names = kc_realloc(ob->names, sizeof(char *) * (size_t)ob->cap);
        ob->klens = kc_realloc(ob->klens, sizeof(size_t) * (size_t)ob->cap);
        ob->vals = kc_realloc(ob->vals, sizeof(KcValue) * (size_t)ob->cap);
    }
    ob->names[ob->count] = key; ob->klens[ob->count] = kl; ob->vals[ob->count] = v;
    ob->count++;
    if (ob->tab && ob->count * 2 > ob->tabcap) ob_rehash(ob);
    else if (ob->tab) {
        uint32_t h = fnv32(key, kl) & (uint32_t)(ob->tabcap - 1);
        while (ob->tab[h] >= 0) h = (h + 1) & (uint32_t)(ob->tabcap - 1);
        ob->tab[h] = ob->count - 1;
    } else if (ob->count > 8) ob_rehash(ob);
    return ob->count - 1;
}
static void ob_abandon(OB *ob) {
    for (int i = 0; i < ob->count; i++) free(ob->names[i]);
    free(ob->names); free(ob->klens); free(ob->vals); free(ob->tab);
}

static bool p_value(P *p, int depth, KcValue *out);

static bool p_object(P *p, int depth, KcValue *out) {
    if (depth >= p->o->max_depth) { p_fail(p, p->i, "nesting too deep (limit %d)", p->o->max_depth); return false; }
    size_t open_at = p->i;
    p->i++;
    OB ob; memset(&ob, 0, sizeof ob);
    for (;;) {
        if (!skip_ws(p)) goto fail;
        if (p->i >= p->n) { p_fail(p, open_at, "unterminated object (missing '}')"); goto fail; }
        if (p->s[p->i] == '}') {
            if (ob.count == 0 || p->o->trailing_commas) { p->i++; break; }
            p_fail(p, p->i, "trailing comma before '}'"); goto fail;
        }
        if (p->s[p->i] != '"') { p_fail(p, p->i, "expected a string key in object"); goto fail; }
        size_t key_at = p->i;
        JBuf kb; jb_init(&kb);
        if (!p_string(p, &kb)) { free(kb.p); goto fail; }
        size_t kl; char *key = jb_take(&kb, &kl);
        if (!skip_ws(p)) { free(key); goto fail; }
        if (p->i >= p->n || p->s[p->i] != ':') { free(key); p_fail(p, p->i, "expected ':' after object key"); goto fail; }
        p->i++;
        KcValue v;
        if (!p_value(p, depth + 1, &v)) { free(key); goto fail; }
        bool dup;
        char key_preview[48]; snprintf(key_preview, sizeof key_preview, "%.40s", key);   /* ob_put frees key on a duplicate */
        ob_put(&ob, key, kl, v, &dup);
        if (dup && p->o->dup_error) { p_fail(p, key_at, "duplicate object key \"%s\"", key_preview); goto fail; }
        if (!skip_ws(p)) goto fail;
        if (p->i >= p->n) { p_fail(p, open_at, "unterminated object (missing '}')"); goto fail; }
        if (p->s[p->i] == ',') { p->i++; continue; }
        if (p->s[p->i] == '}') { p->i++; break; }
        p_fail(p, p->i, "expected ',' or '}' in object"); goto fail;
    }
    {
        KcStructInst *si = kc_struct_alloc();
        si->type_name = (char *)kc_object_type_name;
        si->count = ob.count;
        si->names = ob.names ? ob.names : kc_malloc(1);
        si->values = ob.vals ? ob.vals : kc_malloc(1);
        free(ob.klens); free(ob.tab);
        KcValue v; v.type = VAL_STRUCT; v.as.strct = si;
        *out = v;
        return true;
    }
fail:
    ob_abandon(&ob);
    return false;
}

static bool p_array(P *p, int depth, KcValue *out) {
    if (depth >= p->o->max_depth) { p_fail(p, p->i, "nesting too deep (limit %d)", p->o->max_depth); return false; }
    size_t open_at = p->i;
    p->i++;
    KcValue arr = kc_array_new();
    for (;;) {
        if (!skip_ws(p)) return false;
        if (p->i >= p->n) { p_fail(p, open_at, "unterminated array (missing ']')"); return false; }
        if (p->s[p->i] == ']') {
            if (arr.as.arr->count == 0 || p->o->trailing_commas) { p->i++; break; }
            p_fail(p, p->i, "trailing comma before ']'"); return false;
        }
        KcValue v;
        if (!p_value(p, depth + 1, &v)) return false;
        kc_array_push(arr, v);
        if (!skip_ws(p)) return false;
        if (p->i >= p->n) { p_fail(p, open_at, "unterminated array (missing ']')"); return false; }
        if (p->s[p->i] == ',') { p->i++; continue; }
        if (p->s[p->i] == ']') { p->i++; break; }
        p_fail(p, p->i, "expected ',' or ']' in array"); return false;
    }
    *out = arr;
    return true;
}

static bool p_nonfinite(P *p, KcValue *out) {
    static const struct { const char *w; double v; } k[] = {
        {"NaN", NAN}, {"Infinity", INFINITY}, {"-Infinity", -INFINITY},
    };
    for (int j = 0; j < 3; j++) {
        size_t l = strlen(k[j].w);
        if (p->i + l <= p->n && memcmp(p->s + p->i, k[j].w, l) == 0) { p->i += l; *out = kc_float(k[j].v); return true; }
    }
    return false;
}

static void describe_char(char *buf, size_t cap, unsigned char c) {
    if (c >= 0x20 && c < 0x7f) snprintf(buf, cap, "'%c'", c);
    else snprintf(buf, cap, "byte 0x%02X", c);
}

static bool p_value(P *p, int depth, KcValue *out) {
    if (!skip_ws(p)) return false;
    if (p->i >= p->n) { p_fail(p, p->i, "unexpected end of input"); return false; }
    char c = p->s[p->i];
    switch (c) {
        case '{': return p_object(p, depth, out);
        case '[': return p_array(p, depth, out);
        case '"': {
            JBuf b; jb_init(&b);
            if (!p_string(p, &b)) { free(b.p); return false; }
            *out = jb_to_string(&b);
            return true;
        }
        case 't': return p_literal(p, "true", kc_bool(true), out);
        case 'f': return p_literal(p, "false", kc_bool(false), out);
        case 'n': return p_literal(p, "null", kc_null(), out);
        default: break;
    }
    if (p->o->nan && (c == 'N' || c == 'I' || (c == '-' && p->i + 1 < p->n && p->s[p->i + 1] == 'I'))) {
        if (p_nonfinite(p, out)) return true;
    }
    if (c == '-' || (c >= '0' && c <= '9')) return p_number(p, out);
    char d[24]; describe_char(d, sizeof d, (unsigned char)c);
    p_fail(p, p->i, "unexpected %s", d);
    return false;
}

bool json_parse_text(const char *s, size_t n, const JParseOpts *o, KcValue *out, JError *err) {
    P p = { s, n, 0, o, err, false };
    err->msg[0] = 0; err->line = 0; err->col = 0;
    if (n >= 3 && memcmp(s, "\xEF\xBB\xBF", 3) == 0) p.i = 3;       /* UTF-8 byte-order mark */
    KcValue v;
    if (!p_value(&p, 0, &v)) return false;
    if (!skip_ws(&p)) return false;
    if (p.i < n) { p_fail(&p, p.i, "unexpected data after the JSON value"); return false; }
    *out = v;
    return true;
}

/* ============================================================ token-level formatter */

static void f_nl(JBuf *out, const char *ind, size_t indlen, int level) {
    if (!indlen) return;
    jb_putc(out, '\n');
    for (int k = 0; k < level; k++) jb_put(out, ind, indlen);
}

static bool f_value(P *p, int depth, JBuf *out, const char *ind, size_t indlen, int level) {
    if (!skip_ws(p)) return false;
    if (p->i >= p->n) { p_fail(p, p->i, "unexpected end of input"); return false; }
    char c = p->s[p->i];
    if (c == '{' || c == '[') {
        char close = c == '{' ? '}' : ']';
        if (depth >= p->o->max_depth) { p_fail(p, p->i, "nesting too deep (limit %d)", p->o->max_depth); return false; }
        size_t open_at = p->i;
        p->i++;
        jb_putc(out, c);
        bool first = true;
        for (;;) {
            if (!skip_ws(p)) return false;
            if (p->i >= p->n) { p_fail(p, open_at, "unterminated %s", c == '{' ? "object" : "array"); return false; }
            if (p->s[p->i] == close) {          /* end of container (a trailing comma was vetted below) */
                p->i++;
                if (!first) f_nl(out, ind, indlen, level);
                jb_putc(out, close);
                return true;
            }
            f_nl(out, ind, indlen, level + 1);
            if (c == '{') {
                if (p->s[p->i] != '"') { p_fail(p, p->i, "expected a string key in object"); return false; }
                size_t ks = p->i;
                if (!p_string(p, NULL)) return false;
                jb_put(out, p->s + ks, p->i - ks);
                if (!skip_ws(p)) return false;
                if (p->i >= p->n || p->s[p->i] != ':') { p_fail(p, p->i, "expected ':' after object key"); return false; }
                p->i++;
                jb_putc(out, ':');
                if (indlen) jb_putc(out, ' ');
            }
            if (!f_value(p, depth + 1, out, ind, indlen, level + 1)) return false;
            first = false;
            if (!skip_ws(p)) return false;
            if (p->i >= p->n) { p_fail(p, open_at, "unterminated %s", c == '{' ? "object" : "array"); return false; }
            if (p->s[p->i] == ',') {
                p->i++;
                /* defer the comma until we know another element follows (trailing-comma mode) */
                size_t save = p->i;
                if (!skip_ws(p)) return false;
                bool more = !(p->i < p->n && p->s[p->i] == close);
                p->i = save;
                if (more) jb_putc(out, ',');
                else if (!p->o->trailing_commas) { p_fail(p, save, "trailing comma before '%c'", close); return false; }
                continue;
            }
            if (p->s[p->i] == close) continue;       /* handled at loop top */
            p_fail(p, p->i, "expected ',' or '%c'", close);
            return false;
        }
    }
    if (c == '"') {
        size_t st = p->i;
        if (!p_string(p, NULL)) return false;
        jb_put(out, p->s + st, p->i - st);
        return true;
    }
    if (c == 't' || c == 'f' || c == 'n') {
        const char *w = c == 't' ? "true" : c == 'f' ? "false" : "null";
        KcValue dummy;
        size_t st = p->i;
        if (!p_literal(p, w, kc_null(), &dummy)) return false;
        jb_put(out, p->s + st, p->i - st);
        return true;
    }
    if (p->o->nan && (c == 'N' || c == 'I' || (c == '-' && p->i + 1 < p->n && p->s[p->i + 1] == 'I'))) {
        size_t st = p->i; KcValue dummy;
        if (p_nonfinite(p, &dummy)) { jb_put(out, p->s + st, p->i - st); return true; }
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        size_t st, en; bool isint;
        if (!p_number_scan(p, &st, &en, &isint)) return false;
        jb_put(out, p->s + st, en - st);
        return true;
    }
    char d[24]; describe_char(d, sizeof d, (unsigned char)c);
    p_fail(p, p->i, "unexpected %s", d);
    return false;
}

bool json_reformat_text(const char *s, size_t n, const JParseOpts *o, const char *indent, size_t indlen, JBuf *out, JError *err) {
    P p = { s, n, 0, o, err, false };
    err->msg[0] = 0; err->line = 0; err->col = 0;
    if (n >= 3 && memcmp(s, "\xEF\xBB\xBF", 3) == 0) p.i = 3;
    if (!f_value(&p, 0, out, indent, indlen, 0)) return false;
    if (!skip_ws(&p)) return false;
    if (p.i < n) { p_fail(&p, p.i, "unexpected data after the JSON value"); return false; }
    return true;
}

/* ============================================================ serializer */

void json_format_double(char *out, double d) {
    /* shortest representation that round-trips: try 15, 16, then 17 significant digits */
    for (int prec = 15; prec <= 17; prec++) {
        snprintf(out, 40, "%.*g", prec, d);
        if (strtod(out, NULL) == d) break;
    }
    if (!strpbrk(out, ".eEnN")) strcat(out, ".0");   /* keep float-ness visible: 3.0, not 3 */
}

bool json_write_string(JBuf *b, const char *s, size_t n, bool ascii) {
    static const char HEX[] = "0123456789abcdef";
    bool ok = true;
    jb_putc(b, '"');
    size_t i = 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 0x20 && c != '"' && c != '\\' && c < 0x80) {      /* run of plain ASCII */
            size_t j = i + 1;
            while (j < n) {
                unsigned char d = (unsigned char)s[j];
                if (d < 0x20 || d == '"' || d == '\\' || d >= 0x80) break;
                j++;
            }
            jb_put(b, s + i, j - i);
            i = j;
            continue;
        }
        if (c < 0x80) {
            switch (c) {
                case '"': jb_puts(b, "\\\""); break;
                case '\\': jb_puts(b, "\\\\"); break;
                case '\b': jb_puts(b, "\\b"); break;
                case '\f': jb_puts(b, "\\f"); break;
                case '\n': jb_puts(b, "\\n"); break;
                case '\r': jb_puts(b, "\\r"); break;
                case '\t': jb_puts(b, "\\t"); break;
                default:
                    jb_puts(b, "\\u00"); jb_putc(b, HEX[c >> 4]); jb_putc(b, HEX[c & 15]);
            }
            i++;
            continue;
        }
        int l = utf8_len((const unsigned char *)s + i, n - i);
        if (!l) { ok = false; jb_puts(b, "\\ufffd"); i++; continue; }
        if (!ascii) { jb_put(b, s + i, (size_t)l); i += (size_t)l; continue; }
        unsigned cp;
        const unsigned char *u = (const unsigned char *)s + i;
        if (l == 2) cp = ((unsigned)(u[0] & 0x1F) << 6) | (u[1] & 0x3F);
        else if (l == 3) cp = ((unsigned)(u[0] & 0x0F) << 12) | ((unsigned)(u[1] & 0x3F) << 6) | (u[2] & 0x3F);
        else cp = ((unsigned)(u[0] & 0x07) << 18) | ((unsigned)(u[1] & 0x3F) << 12) | ((unsigned)(u[2] & 0x3F) << 6) | (u[3] & 0x3F);
        if (cp >= 0x10000) {
            unsigned v = cp - 0x10000;
            jb_printf(b, "\\u%04x\\u%04x", 0xD800 + (v >> 10), 0xDC00 + (v & 0x3FF));
        } else jb_printf(b, "\\u%04x", cp);
        i += (size_t)l;
    }
    jb_putc(b, '"');
    return ok;
}

typedef struct { JBuf *b; const JWriteOpts *o; char *err; size_t errsz; bool failed; } W;

static void w_fail(W *w, const char *fmt, ...) {
    if (w->failed) return;
    w->failed = true;
    va_list ap; va_start(ap, fmt);
    vsnprintf(w->err, w->errsz, fmt, ap);
    va_end(ap);
}
static void w_nl(W *w, int level) {
    if (!w->o->indlen) return;
    jb_putc(w->b, '\n');
    for (int k = 0; k < level; k++) jb_put(w->b, w->o->indent, w->o->indlen);
}

typedef struct { const char *k; int idx; } KeyIdx;
static int keyidx_cmp(const void *a, const void *b) { return strcmp(((const KeyIdx *)a)->k, ((const KeyIdx *)b)->k); }

static bool w_value(W *w, KcValue v, int level) {
    if (level > 512) { w_fail(w, "nesting too deep (cyclic structure?)"); return false; }
    switch (v.type) {
        case VAL_NULL: jb_puts(w->b, "null"); return true;
        case VAL_BOOL: jb_puts(w->b, v.as.b ? "true" : "false"); return true;
        case VAL_INT: jb_printf(w->b, "%lld", (long long)v.as.i); return true;
        case VAL_FLOAT: {
            if (isnan(v.as.f) || isinf(v.as.f)) {
                if (w->o->nan_null) { jb_puts(w->b, "null"); return true; }
                w_fail(w, "cannot serialize %s (JSON has no NaN/Infinity; use the \"nan_null\" flag)", isnan(v.as.f) ? "NaN" : "Infinity");
                return false;
            }
            char buf[48]; json_format_double(buf, v.as.f);
            jb_puts(w->b, buf);
            return true;
        }
        case VAL_STRING:
            if (!json_write_string(w->b, v.as.s->data, (size_t)v.as.s->len, w->o->ascii)) {
                w_fail(w, "string is not valid UTF-8");
                return false;
            }
            return true;
        case VAL_ARRAY: {
            KcArray *a = v.as.arr;
            if (a->count == 0) { jb_puts(w->b, "[]"); return true; }
            jb_putc(w->b, '[');
            for (int i = 0; i < a->count; i++) {
                if (i) jb_putc(w->b, ',');
                w_nl(w, level + 1);
                if (!w_value(w, a->items[i], level + 1)) return false;
            }
            w_nl(w, level);
            jb_putc(w->b, ']');
            return true;
        }
        case VAL_STRUCT: {
            KcStructInst *s = v.as.strct;
            if (s->count == 0) { jb_puts(w->b, "{}"); return true; }
            KeyIdx *order = NULL;
            if (w->o->sort_keys) {
                order = kc_malloc(sizeof(KeyIdx) * (size_t)s->count);
                for (int i = 0; i < s->count; i++) { order[i].k = s->names[i]; order[i].idx = i; }
                qsort(order, (size_t)s->count, sizeof(KeyIdx), keyidx_cmp);
            }
            jb_putc(w->b, '{');
            for (int j = 0; j < s->count; j++) {
                int i = order ? order[j].idx : j;
                if (j) jb_putc(w->b, ',');
                w_nl(w, level + 1);
                if (!json_write_string(w->b, s->names[i], strlen(s->names[i]), w->o->ascii)) {
                    w_fail(w, "object key is not valid UTF-8"); free(order); return false;
                }
                jb_putc(w->b, ':');
                if (w->o->indlen) jb_putc(w->b, ' ');
                if (!w_value(w, s->values[i], level + 1)) { free(order); return false; }
            }
            free(order);
            w_nl(w, level);
            jb_putc(w->b, '}');
            return true;
        }
        default:
            w_fail(w, "cannot serialize a value of type '%s'", kc_type_name(v));
            return false;
    }
}

bool json_write_value(JBuf *b, KcValue v, const JWriteOpts *o, char *err, size_t errsz) {
    W w = { b, o, err, errsz, false };
    err[0] = 0;
    return w_value(&w, v, 0);
}

/* ============================================================ files */

bool json_read_whole_file(const char *path, char **buf, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t cap = 4096, n = 0;
    char *b = kc_malloc(cap);
    for (;;) {
        if (n + 4096 > cap) { cap *= 2; b = kc_realloc(b, cap); }
        size_t got = fread(b + n, 1, cap - n - 1, f);
        n += got;
        if (got == 0) break;
    }
    int bad = ferror(f);
    fclose(f);
    if (bad) { free(b); errno = EIO; return false; }
    b[n] = 0;
    *buf = b; *len = n;
    return true;
}

static bool write_whole_file(const char *path, const char *data, size_t n, const char *mode) {
    FILE *f = fopen(path, mode);
    if (!f) return false;
    bool ok = fwrite(data, 1, n, f) == n;
    if (fclose(f) != 0) ok = false;
    return ok;
}

/* ============================================================ argument helpers */

static void need(int n, int lo, int hi, KcPos pos, const char *sig) {
    if (n < lo || n > hi) kc_error_at(pos, "usage: json.%s", sig);
}
static KcString *get_str(KcValue v, KcPos pos, const char *who, const char *what) {
    if (v.type != VAL_STRING) kc_error_at(pos, "%s: %s must be a string, got %s", who, what, kc_type_name(v));
    return v.as.s;
}

/* flag words are separated by spaces and/or commas */
static bool flag_is(const char *w, size_t l, const char *name) { return strlen(name) == l && memcmp(w, name, l) == 0; }

typedef struct { bool sort_keys, ascii, nan_null; } WFlags;

/* Parses a flags string into parse flags and/or write flags (either pointer may be NULL
   to reject that group). Unknown words are a fatal, descriptive error. */
static void parse_flag_words(KcValue flags, KcPos pos, const char *who, JParseOpts *po, WFlags *wf) {
    if (flags.type == VAL_NULL) return;
    if (flags.type != VAL_STRING) kc_error_at(pos, "%s: flags must be a string such as \"comments sort_keys\"", who);
    const char *s = flags.as.s->data;
    while (*s) {
        while (*s == ' ' || *s == ',' || *s == '\t') s++;
        if (!*s) break;
        const char *w = s;
        while (*s && *s != ' ' && *s != ',' && *s != '\t') s++;
        size_t l = (size_t)(s - w);
        bool ok = false;
        if (po) {
            if (flag_is(w, l, "comments")) { po->comments = true; ok = true; }
            else if (flag_is(w, l, "trailing_commas")) { po->trailing_commas = true; ok = true; }
            else if (flag_is(w, l, "nan")) { po->nan = true; ok = true; }
            else if (flag_is(w, l, "lenient")) { po->comments = po->trailing_commas = po->nan = true; ok = true; }
            else if (flag_is(w, l, "dup_error")) { po->dup_error = true; ok = true; }
            else if (l > 6 && memcmp(w, "depth=", 6) == 0) {
                char tmp[16]; size_t m = l - 6 < 15 ? l - 6 : 15; memcpy(tmp, w + 6, m); tmp[m] = 0;
                int d = atoi(tmp);
                if (d < 1 || d > 100000) kc_error_at(pos, "%s: depth must be between 1 and 100000", who);
                po->max_depth = d; ok = true;
            }
        }
        if (!ok && wf) {
            if (flag_is(w, l, "sort_keys")) { wf->sort_keys = true; ok = true; }
            else if (flag_is(w, l, "ascii")) { wf->ascii = true; ok = true; }
            else if (flag_is(w, l, "nan_null")) { wf->nan_null = true; ok = true; }
        }
        if (!ok) {
            kc_error_at(pos, "%s: unknown flag '%.*s' (parse: comments trailing_commas nan lenient dup_error depth=N; write: sort_keys ascii nan_null)",
                        who, (int)l, w);
        }
    }
}

void json_parse_opts_from(KcValue flags, KcPos pos, const char *who, JParseOpts *o) {
    json_default_parse_opts(o);
    parse_flag_words(flags, pos, who, o, NULL);
}

/* indent argument: int spaces (0 = compact) or a whitespace-only string */
static void get_indent(KcValue v, KcPos pos, const char *who, char *buf, size_t *len) {
    if (v.type == VAL_INT) {
        if (v.as.i < 0 || v.as.i > 32) kc_error_at(pos, "%s: indent must be between 0 and 32 spaces", who);
        memset(buf, ' ', (size_t)v.as.i);
        *len = (size_t)v.as.i;
        return;
    }
    if (v.type == VAL_STRING) {
        if (v.as.s->len > 32) kc_error_at(pos, "%s: indent string too long (max 32)", who);
        for (int i = 0; i < v.as.s->len; i++)
            if (v.as.s->data[i] != ' ' && v.as.s->data[i] != '\t') kc_error_at(pos, "%s: indent string may contain only spaces and tabs", who);
        memcpy(buf, v.as.s->data, (size_t)v.as.s->len);
        *len = (size_t)v.as.s->len;
        return;
    }
    kc_error_at(pos, "%s: indent must be an int (spaces) or a string of spaces/tabs", who);
}

static void report_parse_error(KcPos pos, const char *who, const char *where, const JError *e) {
    if (where) kc_error_at(pos, "%s: %s:%d:%d: %s", who, where, e->line, e->col, e->msg);
    kc_error_at(pos, "%s: line %d, column %d: %s", who, e->line, e->col, e->msg);
}

static void io_fail(KcPos pos, const char *who, const char *what, const char *path) {
    kc_error_at(pos, "%s: cannot %s '%s': %s", who, what, path, strerror(errno));
}

/* ============================================================ module functions */

static KcValue j_parse(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 2, pos, "parse(text[, flags]) -> value");
    KcString *s = get_str(a[0], pos, "json.parse", "text");
    JParseOpts o; json_parse_opts_from(n > 1 ? a[1] : kc_null(), pos, "json.parse", &o);
    KcValue out; JError e;
    if (!json_parse_text(s->data, (size_t)s->len, &o, &out, &e)) report_parse_error(pos, "json.parse", NULL, &e);
    return out;
}

static KcValue j_try_parse(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 3, pos, "try_parse(text[, default[, flags]]) -> value or default");
    KcString *s = get_str(a[0], pos, "json.try_parse", "text");
    JParseOpts o; json_parse_opts_from(n > 2 ? a[2] : kc_null(), pos, "json.try_parse", &o);
    KcValue out; JError e;
    if (!json_parse_text(s->data, (size_t)s->len, &o, &out, &e)) return n > 1 ? a[1] : kc_null();
    return out;
}

static KcValue j_is_valid(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 2, pos, "is_valid(text[, flags]) -> bool");
    KcString *s = get_str(a[0], pos, "json.is_valid", "text");
    JParseOpts o; json_parse_opts_from(n > 1 ? a[1] : kc_null(), pos, "json.is_valid", &o);
    JBuf sink; jb_init(&sink); JError e;
    bool ok = json_reformat_text(s->data, (size_t)s->len, &o, "", 0, &sink, &e);   /* validates without building values */
    free(sink.p);
    return kc_bool(ok);
}

static KcValue j_error(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 2, pos, "error(text[, flags]) -> string or null");
    KcString *s = get_str(a[0], pos, "json.error", "text");
    JParseOpts o; json_parse_opts_from(n > 1 ? a[1] : kc_null(), pos, "json.error", &o);
    JBuf sink; jb_init(&sink); JError e;
    bool ok = json_reformat_text(s->data, (size_t)s->len, &o, "", 0, &sink, &e);
    free(sink.p);
    if (ok) return kc_null();
    char buf[240];
    snprintf(buf, sizeof buf, "line %d, column %d: %s", e.line, e.col, e.msg);
    return kc_string(buf);
}

static KcValue j_parse_file(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 2, pos, "parse_file(path[, flags]) -> value");
    KcString *path = get_str(a[0], pos, "json.parse_file", "path");
    JParseOpts o; json_parse_opts_from(n > 1 ? a[1] : kc_null(), pos, "json.parse_file", &o);
    char *buf; size_t len;
    if (!json_read_whole_file(path->data, &buf, &len)) io_fail(pos, "json.parse_file", "read", path->data);
    KcValue out; JError e;
    bool ok = json_parse_text(buf, len, &o, &out, &e);
    free(buf);
    if (!ok) report_parse_error(pos, "json.parse_file", path->data, &e);
    return out;
}

static void init_wopts(JWriteOpts *o, const WFlags *f, const char *indent, size_t indlen) {
    o->indent = indent; o->indlen = indlen;
    o->sort_keys = f->sort_keys; o->ascii = f->ascii; o->nan_null = f->nan_null;
}

static KcValue stringify_impl(KcValue v, KcValue flags, const char *indent, size_t indlen, KcPos pos, const char *who, bool newline) {
    WFlags wf = {0};
    parse_flag_words(flags, pos, who, NULL, &wf);
    JWriteOpts o; init_wopts(&o, &wf, indent, indlen);
    JBuf b; jb_init(&b);
    char err[160];
    if (!json_write_value(&b, v, &o, err, sizeof err)) { free(b.p); kc_error_at(pos, "%s: %s", who, err); }
    if (newline) jb_putc(&b, '\n');
    return jb_to_string(&b);
}

static KcValue j_stringify(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 2, pos, "stringify(value[, flags]) -> string   (compact; flags: sort_keys ascii nan_null)");
    return stringify_impl(a[0], n > 1 ? a[1] : kc_null(), "", 0, pos, "json.stringify", false);
}

static KcValue j_pretty(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 3, pos, "pretty(value[, indent[, flags]]) -> string   (indent: int spaces or tab/space string, default 2)");
    char ind[40]; size_t il = 2; memcpy(ind, "  ", 2);
    if (n > 1) get_indent(a[1], pos, "json.pretty", ind, &il);
    return stringify_impl(a[0], n > 2 ? a[2] : kc_null(), ind, il, pos, "json.pretty", false);
}

static KcValue reformat_impl(KcValue *a, int n, KcPos pos, const char *who, bool pretty) {
    KcString *s = get_str(a[0], pos, who, "text");
    char ind[40]; size_t il = 0;
    int flagidx;
    if (pretty) {
        il = 2; memcpy(ind, "  ", 2);
        if (n > 1 && a[1].type != VAL_NULL) get_indent(a[1], pos, who, ind, &il);
        flagidx = 2;
    } else flagidx = 1;
    JParseOpts o; json_parse_opts_from(n > flagidx ? a[flagidx] : kc_null(), pos, who, &o);
    JBuf out; jb_init(&out); JError e;
    if (!json_reformat_text(s->data, (size_t)s->len, &o, ind, il, &out, &e)) { free(out.p); report_parse_error(pos, who, NULL, &e); }
    return jb_to_string(&out);
}
static KcValue j_minify(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 2, pos, "minify(text[, flags]) -> string");
    return reformat_impl(a, n, pos, "json.minify", false);
}
static KcValue j_format(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 3, pos, "format(text[, indent[, flags]]) -> string   (re-indents text without changing numbers or strings)");
    return reformat_impl(a, n, pos, "json.format", true);
}

static KcValue j_write_file(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    kc_forbid_in_comptime(ip, pos, "json.write_file");
    need(n, 2, 4, pos, "write_file(path, value[, indent[, flags]]) -> int   (default indent 2; 0 = compact)");
    KcString *path = get_str(a[0], pos, "json.write_file", "path");
    char ind[40]; size_t il = 2; memcpy(ind, "  ", 2);
    if (n > 2 && a[2].type != VAL_NULL) get_indent(a[2], pos, "json.write_file", ind, &il);
    KcValue text = stringify_impl(a[1], n > 3 ? a[3] : kc_null(), ind, il, pos, "json.write_file", true);
    if (!write_whole_file(path->data, text.as.s->data, (size_t)text.as.s->len, "wb")) io_fail(pos, "json.write_file", "write", path->data);
    return kc_int(text.as.s->len);
}

/* ---- JSON Lines ---- */

static KcValue parse_lines_impl(const char *s, size_t n, const JParseOpts *o, KcPos pos, const char *who, const char *where) {
    KcValue arr = kc_array_new();
    size_t i = 0; int lineno = 0;
    while (i <= n) {
        size_t j = i;
        while (j < n && s[j] != '\n') j++;
        lineno++;
        size_t end = j;
        if (end > i && s[end - 1] == '\r') end--;
        size_t b = i;
        while (b < end && (s[b] == ' ' || s[b] == '\t')) b++;
        if (b < end) {
            KcValue v; JError e;
            if (!json_parse_text(s + i, end - i, o, &v, &e))
                kc_error_at(pos, "%s: %s%sline %d, column %d: %s", who, where ? where : "", where ? ": " : "", lineno, e.col, e.msg);
            kc_array_push(arr, v);
        }
        if (j >= n) break;
        i = j + 1;
    }
    return arr;
}

static KcValue j_parse_lines(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 2, pos, "parse_lines(text[, flags]) -> array");
    KcString *s = get_str(a[0], pos, "json.parse_lines", "text");
    JParseOpts o; json_parse_opts_from(n > 1 ? a[1] : kc_null(), pos, "json.parse_lines", &o);
    return parse_lines_impl(s->data, (size_t)s->len, &o, pos, "json.parse_lines", NULL);
}

static KcValue j_read_lines(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 2, pos, "read_lines(path[, flags]) -> array   (JSON Lines / ndjson)");
    KcString *path = get_str(a[0], pos, "json.read_lines", "path");
    JParseOpts o; json_parse_opts_from(n > 1 ? a[1] : kc_null(), pos, "json.read_lines", &o);
    char *buf; size_t len;
    if (!json_read_whole_file(path->data, &buf, &len)) io_fail(pos, "json.read_lines", "read", path->data);
    KcValue r = parse_lines_impl(buf, len, &o, pos, "json.read_lines", path->data);
    free(buf);
    return r;
}

static KcValue lines_text(KcValue arr, KcValue flags, KcPos pos, const char *who) {
    if (arr.type != VAL_ARRAY) kc_error_at(pos, "%s: expected an array of values, got %s", who, kc_type_name(arr));
    WFlags wf = {0};
    parse_flag_words(flags, pos, who, NULL, &wf);
    JWriteOpts o; init_wopts(&o, &wf, "", 0);
    JBuf b; jb_init(&b);
    char err[160];
    for (int i = 0; i < arr.as.arr->count; i++) {
        if (!json_write_value(&b, arr.as.arr->items[i], &o, err, sizeof err)) { free(b.p); kc_error_at(pos, "%s: item %d: %s", who, i, err); }
        jb_putc(&b, '\n');
    }
    return jb_to_string(&b);
}

static KcValue j_stringify_lines(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 2, pos, "stringify_lines(array[, flags]) -> string");
    return lines_text(a[0], n > 1 ? a[1] : kc_null(), pos, "json.stringify_lines");
}

static KcValue j_write_lines(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    kc_forbid_in_comptime(ip, pos, "json.write_lines");
    need(n, 2, 3, pos, "write_lines(path, array[, flags]) -> int");
    KcString *path = get_str(a[0], pos, "json.write_lines", "path");
    KcValue t = lines_text(a[1], n > 2 ? a[2] : kc_null(), pos, "json.write_lines");
    if (!write_whole_file(path->data, t.as.s->data, (size_t)t.as.s->len, "wb")) io_fail(pos, "json.write_lines", "write", path->data);
    return kc_int(a[1].as.arr->count);
}

static KcValue j_append_line(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    kc_forbid_in_comptime(ip, pos, "json.append_line");
    need(n, 2, 3, pos, "append_line(path, value[, flags]) -> int   (appends one compact line)");
    KcString *path = get_str(a[0], pos, "json.append_line", "path");
    KcValue one = kc_array_new(); kc_array_push(one, a[1]);
    KcValue t = lines_text(one, n > 2 ? a[2] : kc_null(), pos, "json.append_line");
    if (!write_whole_file(path->data, t.as.s->data, (size_t)t.as.s->len, "ab")) io_fail(pos, "json.append_line", "append to", path->data);
    return kc_int(t.as.s->len);
}

/* ---- string literals ---- */

static KcValue j_escape(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 2, pos, "escape(text[, ascii: bool]) -> string   (a quoted JSON string literal)");
    KcString *s = get_str(a[0], pos, "json.escape", "text");
    bool ascii = n > 1 && kc_truthy(a[1]);
    JBuf b; jb_init(&b);
    if (!json_write_string(&b, s->data, (size_t)s->len, ascii)) { free(b.p); kc_error_at(pos, "json.escape: string is not valid UTF-8"); }
    return jb_to_string(&b);
}

static KcValue j_unescape(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 1, pos, "unescape(literal) -> string   (decodes a quoted JSON string literal)");
    KcString *s = get_str(a[0], pos, "json.unescape", "literal");
    JParseOpts o; json_default_parse_opts(&o);
    KcValue out; JError e;
    if (!json_parse_text(s->data, (size_t)s->len, &o, &out, &e)) report_parse_error(pos, "json.unescape", NULL, &e);
    if (out.type != VAL_STRING) kc_error_at(pos, "json.unescape: expected a JSON string literal such as \"abc\", got %s", kc_type_name(out));
    return out;
}

/* ============================================================ registration */

static KcModuleEntry BASE_ENTRIES[] = {
    {"parse", j_parse}, {"try_parse", j_try_parse}, {"is_valid", j_is_valid}, {"error", j_error},
    {"parse_file", j_parse_file},
    {"stringify", j_stringify}, {"pretty", j_pretty}, {"minify", j_minify}, {"format", j_format},
    {"write_file", j_write_file},
    {"parse_lines", j_parse_lines}, {"read_lines", j_read_lines},
    {"stringify_lines", j_stringify_lines}, {"write_lines", j_write_lines}, {"append_line", j_append_line},
    {"escape", j_escape}, {"unescape", j_unescape},
};

KcModule *kc_mod_json(void) {
    static KcModule m;
    static KcModuleEntry *all = NULL;
    if (!all) {
        int nb = (int)(sizeof BASE_ENTRIES / sizeof BASE_ENTRIES[0]);
        int total = nb + json_ops_count + json_query_count + json_schema_count;
        all = kc_malloc(sizeof(KcModuleEntry) * (size_t)total);
        int k = 0;
        for (int i = 0; i < nb; i++) all[k++] = BASE_ENTRIES[i];
        for (int i = 0; i < json_ops_count; i++) all[k++] = json_ops_entries[i];
        for (int i = 0; i < json_query_count; i++) all[k++] = json_query_entries[i];
        for (int i = 0; i < json_schema_count; i++) all[k++] = json_schema_entries[i];
        m.name = "json"; m.entries = all; m.count = total;
    }
    return &m;
}
