/* KingCobra std.nlp — native natural-language-processing toolkit.
 *
 * Everything is plain C with no third-party dependencies and no model files:
 *   text cleanup   lower/upper/title, strip_accents, strip_punct, strip_html, strip_emoji,
 *                  normalize, slugify, expand_contractions, collapse_ws
 *   segmentation   tokenize (flags), sentences, ngrams, char_ngrams, chunk
 *   linguistics    Porter stemmer, stopwords, syllables, readability, soundex
 *   similarity     levenshtein, damerau, hamming, jaro, jaro_winkler, lcs, dice,
 *                  jaccard, cosine, ratio, fuzzy/closest, simhash
 *   statistics     freq, vocab, bow, collocations (PMI), stats
 *   models         tfidf_*, bm25_* (ranked search), nb_* (naive Bayes classifier),
 *                  spell_* (frequency-based spelling correction)
 *   analysis       sentiment (lexicon, negation/booster aware), detect_lang,
 *                  detect_script, keywords (RAKE), summarize (extractive)
 *   extraction     emails, urls, hashtags, mentions, numbers
 *
 * Text is UTF-8 throughout. Word segmentation, case mapping and accent
 * stripping understand Latin (incl. Latin-1 / Extended-A / Extended Additional),
 * Greek and Cyrillic; Han/Kana text is segmented one character per token.
 * Stemming, stopwords, sentiment and readability are English.
 *
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com>
 */
/* MinGW's default C runtime lacks %zu/%lld; request the C99-conforming printf family (no effect elsewhere) */
#if defined(__MINGW32__) && !defined(__USE_MINGW_ANSI_STDIO)
#define __USE_MINGW_ANSI_STDIO 1
#endif
#include "interp.h"
#include <ctype.h>
#include <math.h>

/* compact one-line `if (..) a; if (..) b;` chains are used deliberately in the data tables */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#endif

/* ============================================================ helpers */

static KcPos G_POS_V;  /* copy of the position of the call being served (the parameter itself dies on return) */
static KcPos *G_POS = &G_POS_V;
#define FAIL(...) kc_error_at(*G_POS, __VA_ARGS__)

typedef struct { char *p; int len, cap; } Buf;
static void buf_init(Buf *b) { b->cap = 64; b->len = 0; b->p = kc_malloc(b->cap); b->p[0] = 0; }
static void buf_grow(Buf *b, int need) {
    if (b->len + need + 1 <= b->cap) return;
    while (b->len + need + 1 > b->cap) b->cap *= 2;
    b->p = kc_realloc(b->p, b->cap);
}
static void buf_add(Buf *b, const char *s, int n) { buf_grow(b, n); memcpy(b->p + b->len, s, n); b->len += n; b->p[b->len] = 0; }
static void buf_ch(Buf *b, char c) { buf_grow(b, 1); b->p[b->len++] = c; b->p[b->len] = 0; }
static void buf_cp(Buf *b, uint32_t cp) {
    char t[4]; int n;
    if (cp >= 0x110000u) { buf_ch(b, (char)(cp - 0x110000u)); return; }
    if (cp < 0x80) { t[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { t[0] = (char)(0xC0 | (cp >> 6)); t[1] = (char)(0x80 | (cp & 63)); n = 2; }
    else if (cp < 0x10000) { t[0] = (char)(0xE0 | (cp >> 12)); t[1] = (char)(0x80 | ((cp >> 6) & 63)); t[2] = (char)(0x80 | (cp & 63)); n = 3; }
    else { t[0] = (char)(0xF0 | (cp >> 18)); t[1] = (char)(0x80 | ((cp >> 12) & 63)); t[2] = (char)(0x80 | ((cp >> 6) & 63)); t[3] = (char)(0x80 | (cp & 63)); n = 4; }
    buf_add(b, t, n);
}
static KcValue buf_value(Buf *b) { return kc_string_take(b->p, b->len); }
static KcValue vstr(const char *s, int n) {
    char *c = kc_malloc(n + 1); memcpy(c, s, n); c[n] = 0; return kc_string_take(c, n);
}

/* Decode one UTF-8 code point at s[*i]. Strict: overlong forms, surrogates and values above
 * U+10FFFF are rejected. Every undecodable byte becomes the private value KC_RAW + byte, which
 * buf_cp() writes back out verbatim — so invalid input passes through untouched instead of being
 * silently reinterpreted — and which the character-class tests treat as an ordinary word byte. */
#define KC_RAW 0x110000u
static uint32_t u8next(const char *s, int n, int *i) {
    unsigned char c = (unsigned char)s[*i];
    if (c < 0x80) { (*i)++; return c; }
    int need = (c >= 0xC2 && c <= 0xDF) ? 1 : (c >= 0xE0 && c <= 0xEF) ? 2 : (c >= 0xF0 && c <= 0xF4) ? 3 : 0;
    if (!need || *i + need >= n) { (*i)++; return KC_RAW + c; }
    uint32_t cp = c & (0x3F >> need);
    for (int k = 1; k <= need; k++) {
        unsigned char d = (unsigned char)s[*i + k];
        if ((d & 0xC0) != 0x80) { (*i)++; return KC_RAW + c; }
        cp = (cp << 6) | (d & 63);
    }
    if ((need == 2 && cp < 0x800) || (need == 3 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp <= 0xDFFF)) { (*i)++; return KC_RAW + c; }
    *i += need + 1; return cp;
}
static uint32_t *u8decode(const char *s, int n, int *outn) {
    uint32_t *a = kc_malloc(sizeof(uint32_t) * (n + 1)); int k = 0;
    for (int i = 0; i < n;) a[k++] = u8next(s, n, &i);
    *outn = k; return a;
}

/* ---- argument access ---- */
static const char *A_str(KcValue *a, int nargs, int i, const char *fn, int *len) {
    if (i >= nargs || a[i].type != VAL_STRING) FAIL("nlp.%s: argument %d must be a string", fn, i + 1);
    if (len) *len = a[i].as.s->len;
    return a[i].as.s->data;
}
static int64_t A_int(KcValue *a, int nargs, int i, const char *fn, int64_t def, bool opt) {
    if (i >= nargs) { if (opt) return def; FAIL("nlp.%s: missing argument %d", fn, i + 1); }
    if (a[i].type == VAL_INT) return a[i].as.i;
    if (a[i].type == VAL_FLOAT) return (int64_t)a[i].as.f;
    FAIL("nlp.%s: argument %d must be a number", fn, i + 1); return 0;
}
static double A_num(KcValue *a, int nargs, int i, const char *fn, double def, bool opt) {
    if (i >= nargs) { if (opt) return def; FAIL("nlp.%s: missing argument %d", fn, i + 1); }
    if (a[i].type == VAL_INT) return (double)a[i].as.i;
    if (a[i].type == VAL_FLOAT) return a[i].as.f;
    FAIL("nlp.%s: argument %d must be a number", fn, i + 1); return 0;
}
static const char *A_flags(KcValue *a, int nargs, int i, const char *fn) {
    if (i >= nargs) return "";
    return A_str(a, nargs, i, fn, NULL);
}
static void need_args(int nargs, int lo, int hi, const char *fn) {
    if (nargs >= lo && nargs <= hi) return;
    if (lo == hi) FAIL("nlp.%s expects %d argument%s, got %d", fn, lo, lo == 1 ? "" : "s", nargs);
    FAIL("nlp.%s expects %d to %d arguments, got %d", fn, lo, hi, nargs);
}

/* ---- small value builders ---- */
static KcValue mk_struct(const char *type, int n, const char **names, KcValue *vals) {
    KcStructInst *s = kc_struct_alloc();
    s->type_name = kc_strdup(type); s->count = n;
    s->names = kc_malloc(sizeof(char *) * n); s->values = kc_malloc(sizeof(KcValue) * n);
    for (int i = 0; i < n; i++) { s->names[i] = kc_strdup(names[i]); s->values[i] = vals[i]; }
    KcValue v; v.type = VAL_STRUCT; v.as.strct = s; return v;
}
static KcValue pair(KcValue a, KcValue b) { KcValue r = kc_array_new(); kc_array_push(r, a); kc_array_push(r, b); return r; }
static KcValue triple(KcValue a, KcValue b, KcValue c) { KcValue r = pair(a, b); kc_array_push(r, c); return r; }

/* ---- token lists: borrowed pointers into script strings, or owned copies ---- */
typedef struct { int n; char **s; int *len; char *owned; } Toks;
static void toks_free(Toks *t) {
    if (t->owned) for (int i = 0; i < t->n; i++) free(t->s[i]);
    free(t->s); free(t->len);
}
static KcValue tokenize_value(const char *s, int n, const char *flags);
static void toks_from_array(Toks *t, KcValue v, const char *fn) {
    KcArray *ar = v.as.arr; t->n = ar->count; t->owned = NULL;
    t->s = kc_malloc(sizeof(char *) * (t->n + 1)); t->len = kc_malloc(sizeof(int) * (t->n + 1));
    for (int i = 0; i < t->n; i++) {
        if (ar->items[i].type != VAL_STRING) FAIL("nlp.%s: token list item %d is not a string", fn, i + 1);
        t->s[i] = ar->items[i].as.s->data; t->len[i] = ar->items[i].as.s->len;
    }
}
/* token list from an array of strings, or (convenience) a string auto-tokenized lowercase */
static void get_toks(Toks *t, KcValue *a, int nargs, int i, const char *fn) {
    if (i >= nargs) FAIL("nlp.%s: missing token list (argument %d)", fn, i + 1);
    if (a[i].type == VAL_ARRAY) { toks_from_array(t, a[i], fn); return; }
    if (a[i].type != VAL_STRING) FAIL("nlp.%s: argument %d must be an array of tokens or a string", fn, i + 1);
    KcValue arr = tokenize_value(a[i].as.s->data, a[i].as.s->len, "l");
    toks_from_array(t, arr, fn);
    /* the array we just built is only referenced here: take its strings (leak-free enough: freed with t) */
    t->owned = (char *)1; /* marker: token strings are ours to free */
}

/* ============================================================ Unicode character data */

static uint32_t cp_lower(uint32_t c) {
    if (c < 0x80) return (c >= 'A' && c <= 'Z') ? c + 32 : c;
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 32;
    if (c >= 0x100 && c <= 0x17F) {
        if (c == 0x130) return 'i';
        if (c == 0x178) return 0xFF;
        if ((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177)) return (c & 1) ? c : c + 1;
        if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return (c & 1) ? c + 1 : c;
        return c;
    }
    if (c >= 0x386 && c <= 0x3AB) {
        if (c == 0x386) return 0x3AC;
        if (c >= 0x388 && c <= 0x38A) return c + 37;
        if (c == 0x38C) return 0x3CC;
        if (c == 0x38E || c == 0x38F) return c + 63;
        if (c >= 0x391 && c <= 0x3AB && c != 0x3A2) return c + 32;
        return c;
    }
    if (c >= 0x400 && c <= 0x40F) return c + 80;
    if (c >= 0x410 && c <= 0x42F) return c + 32;
    if ((c >= 0x460 && c <= 0x481) || (c >= 0x48A && c <= 0x4BF) || (c >= 0x4D0 && c <= 0x52F)) return (c & 1) ? c : c + 1;
    if ((c >= 0x1E00 && c <= 0x1E95) || (c >= 0x1EA0 && c <= 0x1EFF)) return (c & 1) ? c : c + 1;
    if (c >= 0x531 && c <= 0x556) return c + 48;
    return c;
}
static uint32_t cp_upper(uint32_t c) {
    if (c < 0x80) return (c >= 'a' && c <= 'z') ? c - 32 : c;
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 32;
    if (c == 0xFF) return 0x178;
    if (c == 0xB5) return 0x39C;
    if (c >= 0x100 && c <= 0x17F) {
        if (c == 0x131) return 'I';
        if (c == 0x17F) return 'S';
        if ((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177)) return (c & 1) ? c - 1 : c;
        if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return (c & 1) ? c : c - 1;
        return c;
    }
    if (c >= 0x3AC && c <= 0x3CE) {
        if (c == 0x3AC) return 0x386;
        if (c >= 0x3AD && c <= 0x3AF) return c - 37;
        if (c == 0x3C2) return 0x3A3;
        if (c == 0x3CC) return 0x38C;
        if (c == 0x3CD || c == 0x3CE) return c - 63;
        if (c >= 0x3B1 && c <= 0x3CB) return c - 32;
        return c;
    }
    if (c >= 0x450 && c <= 0x45F) return c - 80;
    if (c >= 0x430 && c <= 0x44F) return c - 32;
    if ((c >= 0x460 && c <= 0x481) || (c >= 0x48A && c <= 0x4BF) || (c >= 0x4D0 && c <= 0x52F)) return (c & 1) ? c - 1 : c;
    if ((c >= 0x1E00 && c <= 0x1E95) || (c >= 0x1EA0 && c <= 0x1EFF)) return (c & 1) ? c - 1 : c;
    if (c >= 0x561 && c <= 0x586) return c - 48;
    return c;
}
static bool cp_is_upper(uint32_t c) { return cp_lower(c) != c; }
static bool cp_is_lower(uint32_t c) { return cp_upper(c) != c; }

static bool cp_is_han(uint32_t c) { return (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3400 && c <= 0x4DBF) || (c >= 0x20000 && c <= 0x2A6DF) || (c >= 0xF900 && c <= 0xFAFF); }
static bool cp_is_kana(uint32_t c) { return (c >= 0x3040 && c <= 0x30FF && c != 0x30FB) || (c >= 0x31F0 && c <= 0x31FF) || (c >= 0xFF66 && c <= 0xFF9F); }
static bool cp_is_hangul(uint32_t c) { return (c >= 0xAC00 && c <= 0xD7AF) || (c >= 0x1100 && c <= 0x11FF) || (c >= 0x3130 && c <= 0x318F); }
static bool cp_is_space(uint32_t c) {
    return c == ' ' || (c >= 9 && c <= 13) || c == 0x85 || c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) ||
           c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000 || c == 0x200B || c == 0xFEFF;
}
static bool cp_is_emoji(uint32_t c) {
    return (c >= 0x1F300 && c <= 0x1FAFF) || (c >= 0x2600 && c <= 0x27BF) || (c >= 0x1F000 && c <= 0x1F2FF) ||
           c == 0x200D || (c >= 0xFE00 && c <= 0xFE0F) || (c >= 0x1F1E6 && c <= 0x1F1FF) || c == 0x2B50 || c == 0x2B55 ||
           c == 0x20E3 || (c >= 0xE0020 && c <= 0xE007F);
}
/* punctuation/symbol, ASCII and the common Unicode blocks */
static bool cp_is_punct(uint32_t c) {
    if (c < 0x80) return ispunct((int)c) != 0;
    if (c >= 0xA1 && c <= 0xBF) return !(c == 0xAA || c == 0xB5 || c == 0xBA);
    if (c == 0xD7 || c == 0xF7) return true;
    if (c >= 0x2000 && c <= 0x206F) return !cp_is_space(c) && c != 0x200D;
    if (c >= 0x20A0 && c <= 0x20CF) return true;      /* currency */
    if (c >= 0x2190 && c <= 0x2BFF) return true;      /* arrows, math, box, symbols */
    if (c >= 0x3000 && c <= 0x303F) return c != 0x3000;
    if (c == 0x30FB) return true;
    if (c >= 0xFF01 && c <= 0xFF0F) return true;
    if (c >= 0xFF1A && c <= 0xFF20) return true;
    if (c >= 0xFF3B && c <= 0xFF40) return true;
    if (c >= 0xFF5B && c <= 0xFF65) return true;
    if (c >= 0x2E00 && c <= 0x2E7F) return true;
    return cp_is_emoji(c);
}
static bool cp_is_combining(uint32_t c) { return (c >= 0x300 && c <= 0x36F) || (c >= 0x1AB0 && c <= 0x1AFF) || (c >= 0x20D0 && c <= 0x20FF) || (c >= 0xFE20 && c <= 0xFE2F); }
/* a character that can be part of a word */
static bool cp_is_word(uint32_t c) {
    if (c < 0x80) return isalnum((int)c) != 0;
    if (cp_is_space(c) || cp_is_punct(c)) return false;
    return true;
}
static bool cp_is_digit(uint32_t c) { return c >= '0' && c <= '9'; }
static bool cp_is_apos(uint32_t c) { return c == '\'' || c == 0x2019 || c == 0x2018 || c == 0x02BC; }
static bool cp_is_hyphen(uint32_t c) { return c == '-' || c == 0x2010 || c == 0x2011; }

static const char *LAT1_BASE = "AAAAAAACEEEEIIIIDNOOOOO?OUUUUYTsaaaaaaaceeeeiiiidnooooo?ouuuuyty";
static const char *LATA_BASE =
    "AaAaAaCcCcCcCcDd" "DdEeEeEeEeEeGgGg" "GgGgHhHhIiIiIiIi" "IiIiJjKkkLlLlLlL"
    "lLlNnNnNnnNnOoOo" "OoOoRrRrRrSsSsSs" "SsTtTtTtUuUuUuUu" "UuUuWwYyYZzZzZzs";
/* base-letter expansion of a code point, or NULL if it has none */
static const char *cp_base(uint32_t c, char *tmp) {
    if (c >= 0xC0 && c <= 0xFF) {
        char ch = LAT1_BASE[c - 0xC0];
        if (ch == '?') return NULL;
        if (c == 0xC6) return "AE"; if (c == 0xE6) return "ae";
        if (c == 0xDE) return "Th"; if (c == 0xFE) return "th";
        if (c == 0xDF) return "ss";
        tmp[0] = ch; tmp[1] = 0; return tmp;
    }
    if (c >= 0x100 && c <= 0x17F) {
        if (c == 0x132) return "IJ"; if (c == 0x133) return "ij";
        if (c == 0x152) return "OE"; if (c == 0x153) return "oe";
        tmp[0] = LATA_BASE[c - 0x100]; tmp[1] = 0; return tmp;
    }
    if (c == 0x1E9E) return "SS";
    return NULL;
}

/* ============================================================ case & cleanup */

static KcValue f_lower(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    G_POS_V = pos; need_args(n, 1, 1, "lower"); int len; const char *s = A_str(a, n, 0, "lower", &len);
    Buf b; buf_init(&b);
    for (int i = 0; i < len;) buf_cp(&b, cp_lower(u8next(s, len, &i)));
    return buf_value(&b);
}
static KcValue f_upper(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    G_POS_V = pos; need_args(n, 1, 1, "upper"); int len; const char *s = A_str(a, n, 0, "upper", &len);
    Buf b; buf_init(&b);
    for (int i = 0; i < len;) { uint32_t c = u8next(s, len, &i); if (c == 0xDF) buf_add(&b, "SS", 2); else buf_cp(&b, cp_upper(c)); }
    return buf_value(&b);
}
static KcValue f_title(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    G_POS_V = pos; need_args(n, 1, 1, "title"); int len; const char *s = A_str(a, n, 0, "title", &len);
    Buf b; buf_init(&b); bool start = true;
    for (int i = 0; i < len;) {
        uint32_t c = u8next(s, len, &i);
        if (cp_is_word(c)) { buf_cp(&b, start ? cp_upper(c) : cp_lower(c)); start = false; }
        else { buf_cp(&b, c); start = !cp_is_apos(c); if (cp_is_apos(c)) start = false; }
    }
    return buf_value(&b);
}
static KcValue f_capitalize(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    G_POS_V = pos; need_args(n, 1, 1, "capitalize"); int len; const char *s = A_str(a, n, 0, "capitalize", &len);
    Buf b; buf_init(&b); bool first = true;
    for (int i = 0; i < len;) { uint32_t c = u8next(s, len, &i); buf_cp(&b, first ? cp_upper(c) : c); first = false; }
    return buf_value(&b);
}
static void do_strip_accents(const char *s, int len, Buf *b) {
    char tmp[2];
    for (int i = 0; i < len;) {
        uint32_t c = u8next(s, len, &i);
        if (cp_is_combining(c)) continue;
        const char *r = c >= 0xC0 ? cp_base(c, tmp) : NULL;
        if (r) buf_add(b, r, (int)strlen(r)); else buf_cp(b, c);
    }
}
static KcValue f_strip_accents(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    G_POS_V = pos; need_args(n, 1, 1, "strip_accents"); int len; const char *s = A_str(a, n, 0, "strip_accents", &len);
    Buf b; buf_init(&b); do_strip_accents(s, len, &b); return buf_value(&b);
}
/* punctuation -> space; apostrophes/hyphens inside words are kept */
static KcValue f_strip_punct(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    G_POS_V = pos; need_args(n, 1, 2, "strip_punct"); int len; const char *s = A_str(a, n, 0, "strip_punct", &len);
    const char *fl = A_flags(a, n, 1, "strip_punct"); bool keep_inner = strchr(fl, 'k') != NULL;
    int cn; uint32_t *c = u8decode(s, len, &cn); Buf b; buf_init(&b);
    for (int i = 0; i < cn; i++) {
        if (cp_is_punct(c[i])) {
            bool inner = keep_inner && (cp_is_apos(c[i]) || cp_is_hyphen(c[i])) && i > 0 && i + 1 < cn && cp_is_word(c[i - 1]) && cp_is_word(c[i + 1]);
            if (inner) buf_cp(&b, c[i]); else buf_ch(&b, ' ');
        } else buf_cp(&b, c[i]);
    }
    free(c); return buf_value(&b);
}
static KcValue f_strip_emoji(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    G_POS_V = pos; need_args(n, 1, 1, "strip_emoji"); int len; const char *s = A_str(a, n, 0, "strip_emoji", &len);
    Buf b; buf_init(&b);
    for (int i = 0; i < len;) { uint32_t c = u8next(s, len, &i); if (!cp_is_emoji(c)) buf_cp(&b, c); }
    return buf_value(&b);
}
static void do_collapse(const char *s, int len, Buf *b) {
    bool pend = false;
    for (int i = 0; i < len;) {
        uint32_t c = u8next(s, len, &i);
        if (cp_is_space(c)) { pend = b->len > 0; continue; }
        if (pend) { buf_ch(b, ' '); pend = false; }
        buf_cp(b, c);
    }
}
static KcValue f_collapse_ws(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    G_POS_V = pos; need_args(n, 1, 1, "collapse_ws"); int len; const char *s = A_str(a, n, 0, "collapse_ws", &len);
    Buf b; buf_init(&b); do_collapse(s, len, &b); return buf_value(&b);
}
static bool ci_at(const char *s, int len, int i, const char *w) {
    int k = (int)strlen(w); if (i + k > len) return false;
    for (int x = 0; x < k; x++) if (tolower((unsigned char)s[i + x]) != w[x]) return false;
    return true;
}
static const struct { const char *ent; const char *rep; } HTML_ENT[] = {
    {"amp","&"},{"lt","<"},{"gt",">"},{"quot","\""},{"apos","'"},{"nbsp"," "},{"copy","\xC2\xA9"},{"reg","\xC2\xAE"},
    {"hellip","\xE2\x80\xA6"},{"mdash","\xE2\x80\x94"},{"ndash","\xE2\x80\x93"},{"lsquo","\xE2\x80\x98"},{"rsquo","\xE2\x80\x99"},
    {"ldquo","\xE2\x80\x9C"},{"rdquo","\xE2\x80\x9D"},{"euro","\xE2\x82\xAC"},{"pound","\xC2\xA3"},{"yen","\xC2\xA5"},
};
static KcValue f_strip_html(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    G_POS_V = pos; need_args(n, 1, 1, "strip_html"); int len; const char *s = A_str(a, n, 0, "strip_html", &len);
    Buf b; buf_init(&b);
    for (int i = 0; i < len;) {
        if (s[i] == '<' && i + 1 < len && (isalpha((unsigned char)s[i + 1]) || s[i + 1] == '/' || s[i + 1] == '!' || s[i + 1] == '?')) {
            int j = i + 1;
            if (len - i >= 4 && !strncmp(s + i, "<!--", 4)) { const char *e = strstr(s + i + 4, "-->"); j = e ? (int)(e - s) + 3 : len; i = j; buf_ch(&b, ' '); continue; }
            /* drop <script>/<style> bodies entirely */
            const char *blk = ci_at(s, len, i + 1, "script") ? "script" : ci_at(s, len, i + 1, "style") ? "style" : NULL;
            if (blk) {
                int k = i + 1;
                while (k < len && !(s[k] == '<' && s[k + 1] == '/' && ci_at(s, len, k + 2, blk))) k++;
                while (k < len && s[k] != '>') k++;
                i = k < len ? k + 1 : len; buf_ch(&b, ' '); continue;
            }
            while (j < len && s[j] != '>') j++;
            i = j < len ? j + 1 : len; buf_ch(&b, ' '); continue;
        }
        if (s[i] == '&') {
            int j = i + 1; while (j < len && j - i < 10 && s[j] != ';' && s[j] != ' ' && s[j] != '&') j++;
            if (j < len && s[j] == ';' && j > i + 1) {
                if (s[i + 1] == '#') {
                    uint32_t cp = (s[i + 2] == 'x' || s[i + 2] == 'X') ? (uint32_t)strtoul(s + i + 3, NULL, 16) : (uint32_t)strtoul(s + i + 2, NULL, 10);
                    if (cp) { buf_cp(&b, cp); i = j + 1; continue; }
                } else {
                    bool done = false;
                    for (size_t k = 0; k < sizeof HTML_ENT / sizeof HTML_ENT[0]; k++)
                        if ((int)strlen(HTML_ENT[k].ent) == j - i - 1 && !strncmp(s + i + 1, HTML_ENT[k].ent, j - i - 1)) { buf_add(&b, HTML_ENT[k].rep, (int)strlen(HTML_ENT[k].rep)); done = true; break; }
                    if (done) { i = j + 1; continue; }
                }
            }
        }
        buf_ch(&b, s[i++]);
    }
    Buf c; buf_init(&c); do_collapse(b.p, b.len, &c); free(b.p); return buf_value(&c);
}

/* ---- normalize / slugify / contractions ---- */
static KcValue f_normalize(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    G_POS_V = pos; need_args(n, 1, 1, "normalize"); int len; const char *s = A_str(a, n, 0, "normalize", &len);
    Buf t; buf_init(&t); char tmp[2];
    for (int i = 0; i < len;) {
        uint32_t c = u8next(s, len, &i);
        if (cp_is_combining(c)) continue;
        if (c == 0x2018 || c == 0x2019 || c == 0x201A || c == 0x2032) { buf_ch(&t, '\''); continue; }
        if (c == 0x201C || c == 0x201D || c == 0x201E || c == 0x2033 || c == 0xAB || c == 0xBB) { buf_ch(&t, '"'); continue; }
        if (c >= 0x2010 && c <= 0x2015) { buf_ch(&t, '-'); continue; }
        if (c == 0x2026) { buf_add(&t, "...", 3); continue; }
        if (c >= 0xFF01 && c <= 0xFF5E) c = c - 0xFF01 + 0x21;
        c = cp_lower(c);
        const char *r = c >= 0xC0 ? cp_base(c, tmp) : NULL;
        if (r) { for (const char *q = r; *q; q++) buf_ch(&t, (char)tolower((unsigned char)*q)); } else buf_cp(&t, c);
    }
    Buf o; buf_init(&o); do_collapse(t.p, t.len, &o); free(t.p); return buf_value(&o);
}
static KcValue f_slugify(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    G_POS_V = pos; need_args(n, 1, 2, "slugify"); int len; const char *s = A_str(a, n, 0, "slugify", &len);
    const char *sep = n > 1 ? A_str(a, n, 1, "slugify", NULL) : "-"; int sl = (int)strlen(sep);
    Buf t; buf_init(&t); do_strip_accents(s, len, &t);
    Buf o; buf_init(&o); bool pend = false;
    for (int i = 0; i < t.len;) {
        uint32_t c = u8next(t.p, t.len, &i);
        if (cp_is_word(c)) { if (pend && o.len) buf_add(&o, sep, sl); pend = false; buf_cp(&o, cp_lower(c)); }
        else if (!cp_is_apos(c)) pend = true;
    }
    free(t.p); return buf_value(&o);
}
static bool is_pron_s(const char *w, int l) {
    static const char *ok[] = { "it", "he", "she", "that", "there", "what", "who", "here", "where", "how", "when", "why" };
    for (size_t k = 0; k < sizeof ok / sizeof ok[0]; k++) if ((int)strlen(ok[k]) == l && !strncmp(ok[k], w, l)) return true;
    return false;
}
static KcValue f_expand_contractions(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    G_POS_V = pos; need_args(n, 1, 1, "expand_contractions"); int len; const char *s = A_str(a, n, 0, "expand_contractions", &len);
    Buf o; buf_init(&o);
    for (int i = 0; i < len;) {
        int st = i; uint32_t c = u8next(s, len, &i);
        if (!cp_is_word(c)) { buf_cp(&o, c); continue; }
        int e = i;
        while (e < len) {
            int t = e; uint32_t d = u8next(s, len, &t);
            if (cp_is_word(d)) e = t;
            else if (cp_is_apos(d) && t < len) { int t2 = t; uint32_t d2 = u8next(s, len, &t2); if (cp_is_word(d2)) e = t2; else break; }
            else break;
        }
        i = e;
        char w[96]; int wl = 0; bool has = false;
        for (int k = st; k < e && wl < 90;) { int t = k; uint32_t d = u8next(s, len, &t); if (cp_is_apos(d)) { w[wl++] = '\''; has = true; } else for (int q = k; q < t; q++) w[wl++] = (char)(d < 0x80 ? tolower((unsigned char)s[q]) : s[q]); k = t; }
        w[wl] = 0;
        if (!has) { buf_add(&o, s + st, e - st); continue; }
        const char *rep = NULL; char out[128]; out[0] = 0;
        if (!strcmp(w, "won't")) rep = "will not"; else if (!strcmp(w, "can't")) rep = "cannot";
        else if (!strcmp(w, "shan't")) rep = "shall not"; else if (!strcmp(w, "ain't")) rep = "is not";
        else if (!strcmp(w, "let's")) rep = "let us";
        else {
            const char *suf[] = { "n't", "'re", "'ve", "'ll", "'d", "'m", "'s" };
            const char *exp[] = { " not", " are", " have", " will", " would", " am", " is" };
            for (int k = 0; k < 7 && !rep; k++) {
                int sl = (int)strlen(suf[k]);
                if (wl > sl && !strcmp(w + wl - sl, suf[k])) {
                    if (k == 6 && !is_pron_s(w, wl - 2)) continue;
                    snprintf(out, sizeof out, "%.*s%s", wl - sl, w, exp[k]); rep = out;
                }
            }
        }
        if (!rep) { buf_add(&o, s + st, e - st); continue; }
        bool cap = cp_is_upper((uint32_t)(unsigned char)s[st]) && (unsigned char)s[st] < 0x80;
        bool allcap = cap && e - st > 2; for (int k = st; k < e; k++) if (islower((unsigned char)s[k])) allcap = false;
        for (const char *q = rep; *q; q++) buf_ch(&o, (allcap || (cap && q == rep)) ? (char)toupper((unsigned char)*q) : *q);
    }
    return buf_value(&o);
}

/* ============================================================ tokenizer */

static bool tok_numeric(const char *p, int l) {
    bool dig = false;
    for (int i = 0; i < l; i++) { if (isdigit((unsigned char)p[i])) dig = true; else if (p[i] != '.' && p[i] != ',') return false; }
    return dig;
}
static void emit_tok(KcValue arr, const char *s, int a, int b, bool lower) {
    if (!lower) { kc_array_push(arr, vstr(s + a, b - a)); return; }
    Buf o; buf_init(&o);
    for (int i = a; i < b;) buf_cp(&o, cp_lower(u8next(s, b, &i)));
    kc_array_push(arr, buf_value(&o));
}
static bool ends_with_i(const char *s, int a, int b, const char *suf) {
    int l = (int)strlen(suf); if (b - a < l) return false;
    for (int i = 0; i < l; i++) { char c = s[b - l + i]; if ((unsigned char)c >= 0x80) return false;   /* ASCII only; written so it means the same where plain char is unsigned (ARM, RISC-V) */ if (tolower((unsigned char)c) != suf[i]) return false; }
    return true;
}
static int strncasecmp_(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) { int x = tolower((unsigned char)a[i]), y = tolower((unsigned char)b[i]); if (x != y) return x - y; }
    return 0;
}
static KcValue tokenize_value(const char *s, int n, const char *flags) {
    bool L = strchr(flags, 'l') != NULL, P = strchr(flags, 'p') != NULL, D = strchr(flags, 'd') != NULL;
    bool H = strchr(flags, 'h') != NULL, C = strchr(flags, 'c') != NULL;
    KcValue arr = kc_array_new();
    int i = 0;
    while (i < n) {
        int st = i; uint32_t c = u8next(s, n, &i);
        if (cp_is_space(c) || cp_is_combining(c)) continue;
        if (cp_is_han(c) || cp_is_kana(c)) { emit_tok(arr, s, st, i, L); continue; }
        if (!cp_is_word(c)) { if (P && !cp_is_emoji(c)) emit_tok(arr, s, st, i, false); else if (P) emit_tok(arr, s, st, i, false); continue; }
        int e = i; int prev_digit = cp_is_digit(c); uint32_t prev = c;
        for (;;) {
            if (e >= n) break;
            int t = e; uint32_t d = u8next(s, n, &t);
            if ((cp_is_word(d) || cp_is_combining(d)) && !cp_is_han(d) && !cp_is_kana(d)) { e = t; prev = d; prev_digit = cp_is_digit(d); continue; }
            if (t < n && ((cp_is_apos(d) && !(C && 0)) || (cp_is_hyphen(d) && !H) || ((d == '.' || d == ',') && prev_digit))) {
                int t2 = t; uint32_t d2 = u8next(s, n, &t2);
                bool ok = cp_is_word(d2) && !cp_is_han(d2) && !cp_is_kana(d2);
                if (d == '.' || d == ',') ok = cp_is_digit(d2);
                if (ok && (cp_is_word(prev))) { e = t2; prev = d2; prev_digit = cp_is_digit(d2); continue; }
            }
            break;
        }
        if (D && tok_numeric(s + st, e - st)) { i = e; continue; }
        if (C && e - st > 2) {
            /* Penn-style clitic split: don't -> do + n't, it's -> it + 's */
            int cut = -1;
            const char *lw = s + st;
            if (ends_with_i(s, st, e, "n't") || ends_with_i(s, st, e, "n\xE2\x80\x99t")) {
                bool curly = !ends_with_i(s, st, e, "n't");
                cut = e - (curly ? 5 : 3);
                const char *irr[][2] = { {"ca", "can"}, {"wo", "will"}, {"sha", "shall"} };
                for (int k = 0; k < 3; k++) {
                    int il = (int)strlen(irr[k][0]);
                    if (cut - st == il && !strncasecmp_(lw, irr[k][0], il)) {
                        Buf o; buf_init(&o); bool up = isupper((unsigned char)lw[0]) && !L;
                        for (const char *q = irr[k][1]; *q; q++) buf_ch(&o, (char)(q == irr[k][1] && up ? toupper((unsigned char)*q) : *q));
                        kc_array_push(arr, buf_value(&o)); kc_array_push(arr, kc_string("n't")); cut = -2; break;
                    }
                }
                if (cut == -2) { i = e; continue; }
            } else {
                static const char *sf[] = { "'s", "'m", "'d", "'re", "'ve", "'ll" };
                for (int k = 0; k < 6 && cut < 0; k++) {
                    int sl = (int)strlen(sf[k]);
                    if (ends_with_i(s, st, e, sf[k]) && e - st > sl) cut = e - sl;
                }
                if (cut < 0) for (int k = 0; k < 6 && cut < 0; k++) {
                    /* curly apostrophe form */
                    int sl = (int)strlen(sf[k]) + 2;
                    if (e - st > sl && !memcmp(s + e - sl, "\xE2\x80\x99", 3) && ends_with_i(s, st, e, sf[k] + 1)) cut = e - sl;
                }
            }
            if (cut > st) {
                emit_tok(arr, s, st, cut, L);
                Buf o; buf_init(&o);
                for (int q = cut; q < e;) { uint32_t d = u8next(s, e, &q); buf_cp(&o, cp_is_apos(d) ? '\'' : (L ? cp_lower(d) : d)); }
                kc_array_push(arr, buf_value(&o)); i = e; continue;
            }
        }
        emit_tok(arr, s, st, e, L); i = e;
    }
    return arr;
}

static KcValue f_tokenize(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    G_POS_V = pos; need_args(n, 1, 2, "tokenize"); int len; const char *s = A_str(a, n, 0, "tokenize", &len);
    const char *fl = A_flags(a, n, 1, "tokenize");
    for (const char *q = fl; *q; q++) if (!strchr("lpdhc", *q)) FAIL("nlp.tokenize: unknown flag '%c' (valid: l p d h c)", *q);
    return tokenize_value(s, len, fl);
}

/* ============================================================ sentences */

typedef struct { int a, b; } Span;
static bool word_in(const char *w, const char *list) {
    int l = (int)strlen(w); const char *p = list;
    while (*p) { const char *e = strchr(p, ' '); int pl = e ? (int)(e - p) : (int)strlen(p); if (pl == l && !strncmp(p, w, l)) return true; if (!e) break; p = e + 1; }
    return false;
}
static const char HARD_ABBR[] = "mr mrs ms dr prof sr jr st mt vs gen col lt sgt capt gov sen rep rev hon fig vol pp cf al e.g i.e no "
                                "jan feb mar apr jun jul aug sep sept oct nov dec ave blvd rd approx";
static const char SOFT_ABBR[] = "etc inc ltd co corp est dept";
static void span_push(Span **r, int *cnt, int *cap, const char *s, int a, int b) {
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\n' || s[a] == '\r')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\n' || s[b - 1] == '\r')) b--;
    if (b <= a) return;
    if (*cnt == *cap) { *cap *= 2; *r = kc_realloc(*r, sizeof(Span) * *cap); }
    (*r)[*cnt].a = a; (*r)[*cnt].b = b; (*cnt)++;
}
static int split_sents(const char *s, int n, Span **out) {
    int cap = 16, cnt = 0; Span *r = kc_malloc(sizeof(Span) * cap); int st = 0, i = 0;
    while (i < n) {
        int pi = i; uint32_t c = u8next(s, n, &i);
        bool cjk = c == 0x3002 || c == 0xFF01 || c == 0xFF1F || c == 0xFF61;
        bool term = c == '.' || c == '!' || c == '?' || c == 0x2026 || cjk;
        if (!term) {
            if (c == '\n') {
                int j = i, nl = 1;
                while (j < n && (s[j] == ' ' || s[j] == '\t' || s[j] == '\r' || s[j] == '\n')) { if (s[j] == '\n') nl++; j++; }
                if (nl >= 2) { span_push(&r, &cnt, &cap, s, st, pi); st = j; i = j; }
            }
            continue;
        }
        int e = i, run = 1; bool dot_only = (c == '.');
        while (e < n) { int t = e; uint32_t d = u8next(s, n, &t); if (d == '.' || d == '!' || d == '?' || d == 0x2026) { if (d != '.') dot_only = false; run++; e = t; } else break; }
        while (e < n) { int t = e; uint32_t d = u8next(s, n, &t); if (d == '"' || d == '\'' || d == ')' || d == ']' || d == '}' || d == 0x201D || d == 0x2019 || d == 0xBB) e = t; else break; }
        bool bound = false;
        if (e >= n || cjk) bound = true;
        else {
            int t = e; uint32_t d = u8next(s, n, &t);
            if (cp_is_space(d)) {
                int j = e;
                while (j < n) { int t2 = j; uint32_t d2 = u8next(s, n, &t2); if (cp_is_space(d2)) j = t2; else break; }
                if (j >= n) bound = true;
                else {
                    int t3 = j; uint32_t nx = u8next(s, n, &t3);
                    if (!cp_is_lower(nx)) {
                        bound = true;
                        if (dot_only && run == 1) {
                            int ws = pi; while (ws > 0 && (isalpha((unsigned char)s[ws - 1]) || s[ws - 1] == '.')) ws--;
                            char w[24]; int wl = pi - ws; if (wl > 22) wl = 22;
                            for (int k = 0; k < wl; k++) w[k] = (char)tolower((unsigned char)s[ws + k]);
                            w[wl] = 0;
                            if (wl == 1 && isalpha((unsigned char)s[ws]) && isupper((unsigned char)s[ws])) bound = false;
                            else if (wl > 0 && word_in(w, HARD_ABBR)) bound = false;
                            else if (wl > 0 && word_in(w, SOFT_ABBR) && !cp_is_upper(nx)) bound = false;
                        }
                    }
                }
            }
        }
        if (bound) { span_push(&r, &cnt, &cap, s, st, e); st = e; i = e; }
        else i = e;
    }
    if (st < n) span_push(&r, &cnt, &cap, s, st, n);
    *out = r; return cnt;
}
static KcValue f_sentences(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    G_POS_V = pos; need_args(n, 1, 1, "sentences"); int len; const char *s = A_str(a, n, 0, "sentences", &len);
    Span *sp; int c = split_sents(s, len, &sp); KcValue arr = kc_array_new();
    for (int i = 0; i < c; i++) kc_array_push(arr, vstr(s + sp[i].a, sp[i].b - sp[i].a));
    free(sp); return arr;
}

/* ============================================================ n-grams, chunking */

static KcValue f_ngrams(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    G_POS_V = pos; need_args(n, 2, 3, "ngrams"); Toks t; get_toks(&t, a, n, 0, "ngrams");
    int64_t k = A_int(a, n, 1, "ngrams", 0, false); if (k < 1 || k > 64) FAIL("nlp.ngrams: n must be 1..64");
    const char *sep = n > 2 ? A_str(a, n, 2, "ngrams", NULL) : " "; int sl = (int)strlen(sep);
    KcValue arr = kc_array_new();
    for (int i = 0; i + k <= t.n; i++) {
        Buf o; buf_init(&o);
        for (int j = 0; j < k; j++) { if (j) buf_add(&o, sep, sl); buf_add(&o, t.s[i + j], t.len[i + j]); }
        kc_array_push(arr, buf_value(&o));
    }
    toks_free(&t); return arr;
}
static KcValue f_char_ngrams(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    G_POS_V = pos; need_args(n, 2, 3, "char_ngrams"); int len; const char *s = A_str(a, n, 0, "char_ngrams", &len);
    int64_t k = A_int(a, n, 1, "char_ngrams", 0, false); if (k < 1 || k > 64) FAIL("nlp.char_ngrams: n must be 1..64");
    bool pad = n > 2 && strchr(A_str(a, n, 2, "char_ngrams", NULL), 'p');
    int cn; uint32_t *c0 = u8decode(s, len, &cn); uint32_t *c = c0; int tot = cn;
    if (pad) { c = kc_malloc(sizeof(uint32_t) * (cn + 2 * (k - 1) + 1)); tot = 0; for (int i = 0; i < k - 1; i++) c[tot++] = '_'; for (int i = 0; i < cn; i++) c[tot++] = c0[i]; for (int i = 0; i < k - 1; i++) c[tot++] = '_'; }
    KcValue arr = kc_array_new();
    for (int i = 0; i + k <= tot; i++) { Buf o; buf_init(&o); for (int j = 0; j < k; j++) buf_cp(&o, c[i + j]); kc_array_push(arr, buf_value(&o)); }
    if (pad) free(c); free(c0); return arr;
}
/* split an over-long sentence on whitespace (or code-point boundaries as a last resort) */
static void chunk_push(KcValue arr, const char *s, int a, int b) { if (b > a) kc_array_push(arr, vstr(s + a, b - a)); }
static KcValue f_chunk(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    G_POS_V = pos; need_args(n, 2, 3, "chunk"); int len; const char *s = A_str(a, n, 0, "chunk", &len);
    int64_t mx = A_int(a, n, 1, "chunk", 0, false), ov = A_int(a, n, 2, "chunk", 0, true);
    if (mx < 8) FAIL("nlp.chunk: max_chars must be >= 8"); if (ov < 0 || ov >= mx) FAIL("nlp.chunk: overlap must be 0 <= overlap < max_chars");
    Span *sp; int sc = split_sents(s, len, &sp);
    /* break oversize sentences into pieces first */
    Span *pc = kc_malloc(sizeof(Span) * (sc + len / (mx / 2 + 1) + 4)); int pn = 0;
    for (int i = 0; i < sc; i++) {
        int a0 = sp[i].a, b0 = sp[i].b;
        while (b0 - a0 > mx) {
            int cut = a0 + (int)mx;
            while (cut > a0 && (s[cut] & 0xC0) == 0x80) cut--;
            int w = cut; while (w > a0 + mx / 2 && !isspace((unsigned char)s[w])) w--;
            if (isspace((unsigned char)s[w]) && w > a0 + mx / 2) cut = w;
            int ce = cut; while (ce > a0 && isspace((unsigned char)s[ce - 1])) ce--;
            pc[pn].a = a0; pc[pn].b = ce; pn++;
            while (cut < b0 && isspace((unsigned char)s[cut])) cut++;
            a0 = cut;
        }
        if (b0 > a0) { pc[pn].a = a0; pc[pn].b = b0; pn++; }
    }
    KcValue arr = kc_array_new(); int i = 0;
    while (i < pn) {
        int j = i, start = pc[i].a, end = pc[i].b;
        while (j + 1 < pn && pc[j + 1].b - start <= mx) { j++; end = pc[j].b; }
        chunk_push(arr, s, start, end);
        if (j + 1 >= pn) break;
        int nx = j + 1;                                   /* earliest piece whose tail fits in the overlap */
        for (int k = j; k > i; k--) { if (end - pc[k].a <= ov) nx = k; else break; }
        i = nx;
    }
    free(sp); free(pc); return arr;
}

/* ============================================================ stopwords */

static const char SW_EN[] =
 "a about above after again against all am an and any are aren't as at be because been before being below between both but by "
 "can can't cannot could couldn't did didn't do does doesn't doing don't down during each few for from further had hadn't has "
 "hasn't have haven't having he he'd he'll he's her here here's hers herself him himself his how how's i i'd i'll i'm i've if "
 "in into is isn't it it's its itself let's me more most mustn't my myself no nor not of off on once only or other ought our "
 "ours ourselves out over own same shan't she she'd she'll she's should shouldn't so some such than that that's the their "
 "theirs them themselves then there there's these they they'd they'll they're they've this those through to too under until up "
 "very was wasn't we we'd we'll we're we've were weren't what what's when when's where where's which while who who's whom why "
 "why's will with won't would wouldn't you you'd you'll you're you've your yours yourself yourselves also just than however "
 "yet still much many may might must shall";
static const char SW_ES[] = "el la de que y en los las un una es por con para no se su al lo como mas pero sus le ya o este si porque esta entre cuando muy sin sobre tambien me hasta hay donde del son fue ser son han tiene";
static const char SW_FR[] = "le la les de des du et en un une est que qui dans pour pas sur au avec ce il elle ne se plus par mais ou nous vous sont ete cette ces ils elles je tu on mon ton son leur aux";
static const char SW_DE[] = "der die das und ist nicht ein eine zu den mit von sich auf fur im dem auch es dass wir sie ich er als bei aus nach wie noch oder zum zur einen einem einer sind war hat haben wird werden aber nur uber";
static const char SW_IT[] = "il lo la le di che e un una e per con non sono del della dei nel gli piu come ma anche si questo questa delle degli alla alle nella sono era ha hanno";
static const char SW_PT[] = "o a os as de que e do da em um uma para com nao na no se por mais mas como foi ao dos das nos nas ser tem sua seu ele ela eles elas";
static const char SW_NL[] = "de het een en van is dat op te in niet ze zijn voor met als aan er maar om ook dan nog bij wat deze die dit hij zij wij ik je u wordt worden was waren heeft hebben";
static const struct { const char *code, *words; } LANGS[] = {
    {"en", SW_EN}, {"es", SW_ES}, {"fr", SW_FR}, {"de", SW_DE}, {"it", SW_IT}, {"pt", SW_PT}, {"nl", SW_NL},
};
static const char *lang_words(const char *code) {
    for (size_t i = 0; i < sizeof LANGS / sizeof LANGS[0]; i++) if (!strcmp(LANGS[i].code, code)) return LANGS[i].words;
    return NULL;
}
typedef struct SMap SMap;
static bool is_stop_en(const char *w, int l) {
    char t[40]; if (l <= 0 || l > 38) return false;
    for (int i = 0; i < l; i++) { t[i] = (char)tolower((unsigned char)w[i]); if (t[i] == (char)0xE2) return false; }
    t[l] = 0; return word_in(t, SW_EN);
}
static KcValue f_stopwords(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    G_POS_V = pos; need_args(n, 0, 1, "stopwords");
    const char *code = n ? A_str(a, n, 0, "stopwords", NULL) : "en";
    const char *w = lang_words(code); if (!w) FAIL("nlp.stopwords: unsupported language '%s' (en es fr de it pt nl)", code);
    KcValue arr = kc_array_new(); const char *p = w;
    while (*p) { const char *e = strchr(p, ' '); int l = e ? (int)(e - p) : (int)strlen(p); kc_array_push(arr, vstr(p, l)); if (!e) break; p = e + 1; }
    return arr;
}
static KcValue f_is_stopword(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    G_POS_V = pos; need_args(n, 1, 2, "is_stopword"); int len; const char *s = A_str(a, n, 0, "is_stopword", &len);
    const char *code = n > 1 ? A_str(a, n, 1, "is_stopword", NULL) : "en";
    const char *w = lang_words(code); if (!w) FAIL("nlp.is_stopword: unsupported language '%s'", code);
    Buf b; buf_init(&b); for (int i = 0; i < len;) { uint32_t c = u8next(s, len, &i); buf_cp(&b, cp_is_apos(c) ? '\'' : cp_lower(c)); }
    bool r = word_in(b.p, w); free(b.p); return kc_bool(r);
}
static KcValue f_remove_stopwords(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    G_POS_V = pos; need_args(n, 1, 2, "remove_stopwords"); Toks t; get_toks(&t, a, n, 0, "remove_stopwords");
    const char *code = n > 1 ? A_str(a, n, 1, "remove_stopwords", NULL) : "en";
    const char *w = lang_words(code); if (!w) FAIL("nlp.remove_stopwords: unsupported language '%s'", code);
    KcValue arr = kc_array_new();
    for (int i = 0; i < t.n; i++) {
        Buf b; buf_init(&b); for (int k = 0; k < t.len[i];) { uint32_t c = u8next(t.s[i], t.len[i], &k); buf_cp(&b, cp_is_apos(c) ? '\'' : cp_lower(c)); }
        bool st = word_in(b.p, w); free(b.p);
        if (!st) kc_array_push(arr, vstr(t.s[i], t.len[i]));
    }
    toks_free(&t); return arr;
}

/* ============================================================ Porter stemmer (M.F. Porter, 1980) */

static char *pb; static int pk, pk0, pj;
static int p_cons(int i) {
    switch (pb[i]) { case 'a': case 'e': case 'i': case 'o': case 'u': return 0; case 'y': return (i == pk0) ? 1 : !p_cons(i - 1); default: return 1; }
}
static int p_m(void) {
    int n = 0, i = pk0;
    for (;;) { if (i > pj) return n; if (!p_cons(i)) break; i++; }
    i++;
    for (;;) {
        for (;;) { if (i > pj) return n; if (p_cons(i)) break; i++; }
        i++; n++;
        for (;;) { if (i > pj) return n; if (!p_cons(i)) break; i++; }
        i++;
    }
}
static int p_vowelinstem(void) { for (int i = pk0; i <= pj; i++) if (!p_cons(i)) return 1; return 0; }
static int p_doublec(int j) { if (j < pk0 + 1) return 0; if (pb[j] != pb[j - 1]) return 0; return p_cons(j); }
static int p_cvc(int i) {
    if (i < pk0 + 2 || !p_cons(i) || p_cons(i - 1) || !p_cons(i - 2)) return 0;
    int ch = pb[i]; return !(ch == 'w' || ch == 'x' || ch == 'y');
}
static int p_ends(const char *s) {
    int l = (int)strlen(s);
    if (s[l - 1] != pb[pk]) return 0;
    if (l > pk - pk0 + 1) return 0;
    if (memcmp(pb + pk - l + 1, s, l) != 0) return 0;
    pj = pk - l; return 1;
}
static void p_setto(const char *s) { int l = (int)strlen(s); memmove(pb + pj + 1, s, l); pk = pj + l; }
static void p_r(const char *s) { if (p_m() > 0) p_setto(s); }
static void p_step1ab(void) {
    if (pb[pk] == 's') {
        if (p_ends("sses")) pk -= 2; else if (p_ends("ies")) p_setto("i"); else if (pb[pk - 1] != 's') pk--;
    }
    if (p_ends("eed")) { if (p_m() > 0) pk--; }
    else if ((p_ends("ed") || p_ends("ing")) && p_vowelinstem()) {
        pk = pj;
        if (p_ends("at")) p_setto("ate"); else if (p_ends("bl")) p_setto("ble"); else if (p_ends("iz")) p_setto("ize");
        else if (p_doublec(pk)) { pk--; int ch = pb[pk]; if (ch == 'l' || ch == 's' || ch == 'z') pk++; }
        else if (p_m() == 1 && p_cvc(pk)) p_setto("e");
    }
}
static void p_step1c(void) { if (p_ends("y") && p_vowelinstem()) pb[pk] = 'i'; }
static void p_step2(void) {
    switch (pb[pk - 1]) {
    case 'a': if (p_ends("ational")) { p_r("ate"); break; } if (p_ends("tional")) { p_r("tion"); break; } break;
    case 'c': if (p_ends("enci")) { p_r("ence"); break; } if (p_ends("anci")) { p_r("ance"); break; } break;
    case 'e': if (p_ends("izer")) { p_r("ize"); break; } break;
    case 'l': if (p_ends("bli")) { p_r("ble"); break; } if (p_ends("alli")) { p_r("al"); break; } if (p_ends("entli")) { p_r("ent"); break; }
              if (p_ends("eli")) { p_r("e"); break; } if (p_ends("ousli")) { p_r("ous"); break; } break;
    case 'o': if (p_ends("ization")) { p_r("ize"); break; } if (p_ends("ation")) { p_r("ate"); break; } if (p_ends("ator")) { p_r("ate"); break; } break;
    case 's': if (p_ends("alism")) { p_r("al"); break; } if (p_ends("iveness")) { p_r("ive"); break; } if (p_ends("fulness")) { p_r("ful"); break; }
              if (p_ends("ousness")) { p_r("ous"); break; } break;
    case 't': if (p_ends("aliti")) { p_r("al"); break; } if (p_ends("iviti")) { p_r("ive"); break; } if (p_ends("biliti")) { p_r("ble"); break; } break;
    case 'g': if (p_ends("logi")) { p_r("log"); break; }
    }
}
static void p_step3(void) {
    switch (pb[pk]) {
    case 'e': if (p_ends("icate")) { p_r("ic"); break; } if (p_ends("ative")) { p_r(""); break; } if (p_ends("alize")) { p_r("al"); break; } break;
    case 'i': if (p_ends("iciti")) { p_r("ic"); break; } break;
    case 'l': if (p_ends("ical")) { p_r("ic"); break; } if (p_ends("ful")) { p_r(""); break; } break;
    case 's': if (p_ends("ness")) { p_r(""); break; } break;
    }
}
static void p_step4(void) {
    switch (pb[pk - 1]) {
    case 'a': if (p_ends("al")) break; return;
    case 'c': if (p_ends("ance")) break; if (p_ends("ence")) break; return;
    case 'e': if (p_ends("er")) break; return;
    case 'i': if (p_ends("ic")) break; return;
    case 'l': if (p_ends("able")) break; if (p_ends("ible")) break; return;
    case 'n': if (p_ends("ant")) break; if (p_ends("ement")) break; if (p_ends("ment")) break; if (p_ends("ent")) break; return;
    case 'o': if (p_ends("ion") && pj >= pk0 && (pb[pj] == 's' || pb[pj] == 't')) break; if (p_ends("ou")) break; return;
    case 's': if (p_ends("ism")) break; return;
    case 't': if (p_ends("ate")) break; if (p_ends("iti")) break; return;
    case 'u': if (p_ends("ous")) break; return;
    case 'v': if (p_ends("ive")) break; return;
    case 'z': if (p_ends("ize")) break; return;
    default: return;
    }
    if (p_m() > 1) pk = pj;
}
static void p_step5(void) {
    pj = pk;
    if (pb[pk] == 'e') { int m = p_m(); if (m > 1 || (m == 1 && !p_cvc(pk - 1))) pk--; }
    if (pb[pk] == 'l' && p_doublec(pk) && p_m() > 1) pk--;
}
/* stem into out[64]; unstemmable words (non a-z, too short/long) are copied unchanged. Returns length. */
static int stem_into(const char *w, int l, char *out) {
    if (l < 3 || l > 60) { int c = l < 63 ? l : 62; memcpy(out, w, c); out[c] = 0; return c; }
    char buf[64];
    for (int i = 0; i < l; i++) {
        unsigned char c = (unsigned char)w[i];
        if (c >= 'A' && c <= 'Z') c += 32;
        if (c < 'a' || c > 'z') { memcpy(out, w, l); out[l] = 0; return l; }
        buf[i] = (char)c;
    }
    buf[l] = 0; pb = buf; pk = l - 1; pk0 = 0; pj = 0;
    p_step1ab();
    if (pk > pk0) { p_step1c(); p_step2(); p_step3(); p_step4(); p_step5(); }
    memcpy(out, buf, pk + 1); out[pk + 1] = 0; return pk + 1;
}
static KcValue stem_word(const char *w, int l) {
    if (l > 60) return vstr(w, l);
    char out[64]; int k = stem_into(w, l, out); return vstr(out, k);
}
static KcValue f_stem(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    G_POS_V = pos; need_args(n, 1, 1, "stem"); int len; const char *s = A_str(a, n, 0, "stem", &len); return stem_word(s, len);
}
static KcValue f_stem_all(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    G_POS_V = pos; need_args(n, 1, 1, "stem_all"); Toks t; get_toks(&t, a, n, 0, "stem_all"); KcValue arr = kc_array_new();
    for (int i = 0; i < t.n; i++) kc_array_push(arr, stem_word(t.s[i], t.len[i]));
    toks_free(&t); return arr;
}

/* ============================================================ syllables / readability / soundex */

static int syllables_of(const char *w, int l) {
    char t[64]; int k = 0;
    for (int i = 0; i < l && k < 62; i++) { unsigned char c = (unsigned char)w[i]; if (isalpha(c)) t[k++] = (char)tolower(c); else if (c >= 0x80) t[k++] = 'e'; }
    t[k] = 0; if (k == 0) return 0; if (k <= 3) return 1;
    int cnt = 0; bool pv = false;
    for (int i = 0; i < k; i++) { bool v = strchr("aeiouy", t[i]) != NULL; if (v && !pv) cnt++; pv = v; }
    if (k > 2 && t[k - 1] == 'e' && !strchr("aeiouy", t[k - 2]) && !(t[k - 2] == 'l' && k > 3 && !strchr("aeiouy", t[k - 3]))) cnt--;
    else if (k > 2 && t[k - 2] == 'e' && t[k - 1] == 'd' && !strchr("aeiouydt", t[k - 3])) cnt--;
    else if (k > 2 && t[k - 2] == 'e' && t[k - 1] == 's' && !strchr("aeiouyszxc", t[k - 3]) && !(t[k - 3] == 'h' && k > 3 && strchr("sc", t[k - 4]))) cnt--;
    return cnt < 1 ? 1 : cnt;
}
static KcValue f_syllables(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    G_POS_V = pos; need_args(n, 1, 1, "syllables"); int len; const char *s = A_str(a, n, 0, "syllables", &len); return kc_int(syllables_of(s, len));
}
static KcValue f_readability(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    G_POS_V = pos; need_args(n, 1, 1, "readability"); int len; const char *s = A_str(a, n, 0, "readability", &len);
    KcValue toks = tokenize_value(s, len, "d"); Span *sp; int sc = split_sents(s, len, &sp); free(sp);
    int words = toks.as.arr->count, syl = 0;
    for (int i = 0; i < words; i++) syl += syllables_of(toks.as.arr->items[i].as.s->data, toks.as.arr->items[i].as.s->len);
    if (sc < 1) sc = 1;
    double fre = 0, fk = 0, fog = 0; int complex = 0;
    if (words > 0) {
        double wps = (double)words / sc, spw = (double)syl / words;
        fre = 206.835 - 1.015 * wps - 84.6 * spw; fk = 0.39 * wps + 11.8 * spw - 15.59;
        for (int i = 0; i < words; i++) if (syllables_of(toks.as.arr->items[i].as.s->data, toks.as.arr->items[i].as.s->len) >= 3) complex++;
        fog = 0.4 * (wps + 100.0 * complex / words);
    }
    const char *nm[] = { "flesch", "grade", "fog", "words", "sentences", "syllables" };
    KcValue v[] = { kc_float(fre), kc_float(fk), kc_float(fog), kc_int(words), kc_int(sc), kc_int(syl) };
    return mk_struct("Readability", 6, nm, v);
}
static KcValue f_soundex(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    G_POS_V = pos; need_args(n, 1, 1, "soundex"); int len; const char *s = A_str(a, n, 0, "soundex", &len);
    static const char *code = "01230120022455012623010202"; /* a..z */
    char out[5] = "0000"; int k = 0; char last = 0;
    for (int i = 0; i < len && k < 4; i++) {
        unsigned char c = (unsigned char)s[i]; if (!isalpha(c)) continue; c = (unsigned char)tolower(c);
        char d = code[c - 'a'];
        if (k == 0) { out[k++] = (char)toupper(c); last = d; continue; }
        if (c == 'h' || c == 'w') continue;
        if (d != '0' && d != last) out[k++] = d;
        last = d;
    }
    if (k == 0) return kc_string("");
    out[4] = 0; return kc_string(out);
}

/* ============================================================ similarity & distance */

static int dist_lev(const uint32_t *a, int n, const uint32_t *b, int m, bool osa) {
    if (!n) return m; if (!m) return n;
    int *p2 = kc_malloc(sizeof(int) * (m + 1)), *p1 = kc_malloc(sizeof(int) * (m + 1)), *cu = kc_malloc(sizeof(int) * (m + 1));
    for (int j = 0; j <= m; j++) p1[j] = j;
    for (int i = 1; i <= n; i++) {
        cu[0] = i;
        for (int j = 1; j <= m; j++) {
            int cost = a[i - 1] == b[j - 1] ? 0 : 1;
            int v = p1[j] + 1; if (cu[j - 1] + 1 < v) v = cu[j - 1] + 1; if (p1[j - 1] + cost < v) v = p1[j - 1] + cost;
            if (osa && i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1] && p2[j - 2] + 1 < v) v = p2[j - 2] + 1;
            cu[j] = v;
        }
        int *t = p2; p2 = p1; p1 = cu; cu = t;
    }
    int r = p1[m]; free(p2); free(p1); free(cu); return r;
}
static double dist_jaro(const uint32_t *a, int n, const uint32_t *b, int m) {
    if (!n && !m) return 1.0; if (!n || !m) return 0.0;
    int win = (n > m ? n : m) / 2 - 1; if (win < 0) win = 0;
    char *am = calloc(n, 1), *bm = calloc(m, 1); int mt = 0;
    for (int i = 0; i < n; i++) {
        int lo = i - win > 0 ? i - win : 0, hi = i + win + 1 < m ? i + win + 1 : m;
        for (int j = lo; j < hi; j++) if (!bm[j] && a[i] == b[j]) { am[i] = bm[j] = 1; mt++; break; }
    }
    if (!mt) { free(am); free(bm); return 0.0; }
    int t = 0, k = 0;
    for (int i = 0; i < n; i++) if (am[i]) { while (!bm[k]) k++; if (a[i] != b[k]) t++; k++; }
    free(am); free(bm);
    return ((double)mt / n + (double)mt / m + (double)(mt - t / 2) / mt) / 3.0;
}
#define TWO_STR(fn) G_POS_V = pos; int l1, l2; const char *s1 = A_str(a, n, 0, fn, &l1), *s2 = A_str(a, n, 1, fn, &l2); \
    int n1, n2; uint32_t *c1 = u8decode(s1, l1, &n1), *c2 = u8decode(s2, l2, &n2)
#define DONE_TWO free(c1); free(c2)
static KcValue f_levenshtein(KcInterp *ip, KcValue *a, int n, KcPos pos) { G_POS_V = pos; need_args(n, 2, 2, "levenshtein"); TWO_STR("levenshtein"); int r = dist_lev(c1, n1, c2, n2, false); DONE_TWO; return kc_int(r); }
static KcValue f_damerau(KcInterp *ip, KcValue *a, int n, KcPos pos) { G_POS_V = pos; need_args(n, 2, 2, "damerau"); TWO_STR("damerau"); int r = dist_lev(c1, n1, c2, n2, true); DONE_TWO; return kc_int(r); }
static KcValue f_hamming(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    G_POS_V = pos; need_args(n, 2, 2, "hamming"); TWO_STR("hamming");
    if (n1 != n2) { DONE_TWO; FAIL("nlp.hamming: strings must have equal length (%d vs %d characters)", n1, n2); }
    int d = 0; for (int i = 0; i < n1; i++) if (c1[i] != c2[i]) d++; DONE_TWO; return kc_int(d);
}
static KcValue f_jaro(KcInterp *ip, KcValue *a, int n, KcPos pos) { G_POS_V = pos; need_args(n, 2, 2, "jaro"); TWO_STR("jaro"); double r = dist_jaro(c1, n1, c2, n2); DONE_TWO; return kc_float(r); }
static KcValue f_jaro_winkler(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    G_POS_V = pos; need_args(n, 2, 3, "jaro_winkler"); TWO_STR("jaro_winkler"); double sc = A_num(a, n, 2, "jaro_winkler", 0.1, true);
    if (sc < 0 || sc > 0.25) { DONE_TWO; FAIL("nlp.jaro_winkler: scale must be 0..0.25"); }
    double j = dist_jaro(c1, n1, c2, n2); int pre = 0; while (pre < 4 && pre < n1 && pre < n2 && c1[pre] == c2[pre]) pre++;
    DONE_TWO; return kc_float(j + pre * sc * (1.0 - j));
}
static KcValue f_lcs(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    G_POS_V = pos; need_args(n, 2, 2, "lcs"); TWO_STR("lcs");
    int *p = calloc(n2 + 1, sizeof(int)), *q = calloc(n2 + 1, sizeof(int));
    for (int i = 1; i <= n1; i++) { for (int j = 1; j <= n2; j++) q[j] = c1[i - 1] == c2[j - 1] ? p[j - 1] + 1 : (p[j] > q[j - 1] ? p[j] : q[j - 1]); int *t = p; p = q; q = t; }
    int r = p[n2]; free(p); free(q); DONE_TWO; return kc_int(r);
}
static KcValue f_ratio(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    G_POS_V = pos; need_args(n, 2, 2, "ratio"); TWO_STR("ratio"); int mx = n1 > n2 ? n1 : n2; double r = 1.0;
    if (mx) r = 1.0 - (double)dist_lev(c1, n1, c2, n2, true) / mx;
    DONE_TWO; return kc_float(r);
}
static KcValue f_dice(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    G_POS_V = pos; need_args(n, 2, 2, "dice"); TWO_STR("dice");
    if (n1 < 2 || n2 < 2) { bool eq = n1 == n2 && !memcmp(c1, c2, sizeof(uint32_t) * n1); DONE_TWO; return kc_float(eq ? 1.0 : 0.0); }
    char *used = calloc(n2, 1); int hit = 0;
    for (int i = 0; i + 1 < n1; i++) for (int j = 0; j + 1 < n2; j++) if (!used[j] && c1[i] == c2[j] && c1[i + 1] == c2[j + 1]) { used[j] = 1; hit++; break; }
    free(used); double r = 2.0 * hit / ((n1 - 1) + (n2 - 1)); DONE_TWO; return kc_float(r);
}

/* ---- string-keyed hash map: dense ids in insertion order ---- */
struct SMap { char **key; int *klen; int *slot; int cnt, cap, nslots; };
static uint64_t fnv64(const char *p, int l) { uint64_t h = 1469598103934665603ULL; for (int i = 0; i < l; i++) { h ^= (unsigned char)p[i]; h *= 1099511628211ULL; } return h; }
static void smap_init(SMap *m) { m->cnt = 0; m->cap = 16; m->nslots = 64; m->key = kc_malloc(sizeof(char *) * m->cap); m->klen = kc_malloc(sizeof(int) * m->cap); m->slot = kc_malloc(sizeof(int) * m->nslots); for (int i = 0; i < m->nslots; i++) m->slot[i] = -1; }
static int smap_find(SMap *m, const char *k, int l) {
    uint64_t h = fnv64(k, l); int s = (int)(h & (uint64_t)(m->nslots - 1));
    while (m->slot[s] >= 0) { int id = m->slot[s]; if (m->klen[id] == l && !memcmp(m->key[id], k, l)) return id; s = (s + 1) & (m->nslots - 1); }
    return -1;
}
static int smap_add(SMap *m, const char *k, int l, bool *isnew) {
    int f = smap_find(m, k, l); if (f >= 0) { if (isnew) *isnew = false; return f; }
    if ((m->cnt + 1) * 2 > m->nslots) {
        m->nslots *= 2; free(m->slot); m->slot = kc_malloc(sizeof(int) * m->nslots); for (int i = 0; i < m->nslots; i++) m->slot[i] = -1;
        for (int id = 0; id < m->cnt; id++) { int s = (int)(fnv64(m->key[id], m->klen[id]) & (uint64_t)(m->nslots - 1)); while (m->slot[s] >= 0) s = (s + 1) & (m->nslots - 1); m->slot[s] = id; }
    }
    if (m->cnt == m->cap) { m->cap *= 2; m->key = kc_realloc(m->key, sizeof(char *) * m->cap); m->klen = kc_realloc(m->klen, sizeof(int) * m->cap); }
    char *c = kc_malloc(l + 1); memcpy(c, k, l); c[l] = 0; int id = m->cnt++; m->key[id] = c; m->klen[id] = l;
    int s = (int)(fnv64(k, l) & (uint64_t)(m->nslots - 1)); while (m->slot[s] >= 0) s = (s + 1) & (m->nslots - 1); m->slot[s] = id;
    if (isnew) *isnew = true; return id;
}
static void smap_free(SMap *m) { for (int i = 0; i < m->cnt; i++) free(m->key[i]); free(m->key); free(m->klen); free(m->slot); }

static KcValue f_jaccard(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    G_POS_V = pos; need_args(n, 2, 2, "jaccard"); Toks x, y; get_toks(&x, a, n, 0, "jaccard"); get_toks(&y, a, n, 1, "jaccard");
    SMap A; smap_init(&A); SMap B; smap_init(&B);
    for (int i = 0; i < x.n; i++) smap_add(&A, x.s[i], x.len[i], NULL);
    for (int i = 0; i < y.n; i++) smap_add(&B, y.s[i], y.len[i], NULL);
    int inter = 0; for (int i = 0; i < A.cnt; i++) if (smap_find(&B, A.key[i], A.klen[i]) >= 0) inter++;
    int uni = A.cnt + B.cnt - inter; double r = uni ? (double)inter / uni : 1.0;
    smap_free(&A); smap_free(&B); toks_free(&x); toks_free(&y); return kc_float(r);
}
static bool all_numbers(KcValue v) {
    if (v.type != VAL_ARRAY || v.as.arr->count == 0) return false;
    for (int i = 0; i < v.as.arr->count; i++) if (v.as.arr->items[i].type != VAL_INT && v.as.arr->items[i].type != VAL_FLOAT) return false;
    return true;
}
static double numv(KcValue v) { return v.type == VAL_INT ? (double)v.as.i : v.as.f; }
static KcValue f_cosine(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    G_POS_V = pos; need_args(n, 2, 2, "cosine");
    if (n == 2 && all_numbers(a[0]) && all_numbers(a[1])) {
        int k = a[0].as.arr->count; if (a[1].as.arr->count != k) FAIL("nlp.cosine: vectors must have equal length (%d vs %d)", k, a[1].as.arr->count);
        double d = 0, x = 0, y = 0;
        for (int i = 0; i < k; i++) { double p = numv(a[0].as.arr->items[i]), q = numv(a[1].as.arr->items[i]); d += p * q; x += p * p; y += q * q; }
        return kc_float(x > 0 && y > 0 ? d / (sqrt(x) * sqrt(y)) : 0.0);
    }
    Toks x, y; get_toks(&x, a, n, 0, "cosine"); get_toks(&y, a, n, 1, "cosine");
    SMap M; smap_init(&M);
    for (int i = 0; i < x.n; i++) smap_add(&M, x.s[i], x.len[i], NULL);
    for (int i = 0; i < y.n; i++) smap_add(&M, y.s[i], y.len[i], NULL);
    int *cx = calloc(M.cnt + 1, sizeof(int)), *cy = calloc(M.cnt + 1, sizeof(int));
    for (int i = 0; i < x.n; i++) cx[smap_find(&M, x.s[i], x.len[i])]++;
    for (int i = 0; i < y.n; i++) cy[smap_find(&M, y.s[i], y.len[i])]++;
    double d = 0, px = 0, py = 0;
    for (int i = 0; i < M.cnt; i++) { d += (double)cx[i] * cy[i]; px += (double)cx[i] * cx[i]; py += (double)cy[i] * cy[i]; }
    free(cx); free(cy); smap_free(&M); toks_free(&x); toks_free(&y);
    return kc_float(px > 0 && py > 0 ? d / (sqrt(px) * sqrt(py)) : 0.0);
}
typedef struct { double score; int idx; } Scored;
static int cmp_scored_desc(const void *x, const void *y) {
    const Scored *a = x, *b = y; if (a->score < b->score) return 1; if (a->score > b->score) return -1; return a->idx - b->idx;
}
static KcValue f_fuzzy(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    G_POS_V = pos; need_args(n, 2, 4, "fuzzy"); int ql; const char *q = A_str(a, n, 0, "fuzzy", &ql);
    if (a[1].type != VAL_ARRAY) FAIL("nlp.fuzzy: candidates must be an array of strings");
    int64_t k = A_int(a, n, 2, "fuzzy", 1, true); double minsc = A_num(a, n, 3, "fuzzy", 0.0, true);
    KcArray *cand = a[1].as.arr; Scored *sc = kc_malloc(sizeof(Scored) * (cand->count + 1)); int m = 0;
    Buf qb; buf_init(&qb); for (int i = 0; i < ql;) buf_cp(&qb, cp_lower(u8next(q, ql, &i)));
    int qn; uint32_t *qc = u8decode(qb.p, qb.len, &qn);
    for (int i = 0; i < cand->count; i++) {
        if (cand->items[i].type != VAL_STRING) FAIL("nlp.fuzzy: candidate %d is not a string", i + 1);
        KcString *cs = cand->items[i].as.s; Buf cb; buf_init(&cb); for (int j = 0; j < cs->len;) buf_cp(&cb, cp_lower(u8next(cs->data, cs->len, &j)));
        int cn; uint32_t *cc = u8decode(cb.p, cb.len, &cn); int mx = qn > cn ? qn : cn;
        double r = mx ? 1.0 - (double)dist_lev(qc, qn, cc, cn, true) / mx : 1.0;
        free(cc); free(cb.p);
        if (r >= minsc) { sc[m].score = r; sc[m].idx = i; m++; }
    }
    qsort(sc, m, sizeof(Scored), cmp_scored_desc);
    KcValue arr = kc_array_new(); for (int i = 0; i < m && i < k; i++) kc_array_push(arr, pair(cand->items[sc[i].idx], kc_float(sc[i].score)));
    free(sc); free(qc); free(qb.p); return arr;
}
static KcValue f_closest(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    G_POS_V = pos; need_args(n, 2, 2, "closest");
    KcValue args[2] = { a[0], a[1] }; KcValue r = f_fuzzy(ip, args, 2, pos);
    return r.as.arr->count ? r.as.arr->items[0].as.arr->items[0] : kc_null();
}
static KcValue f_simhash(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    G_POS_V = pos; need_args(n, 1, 1, "simhash"); Toks t; get_toks(&t, a, n, 0, "simhash"); int w[64]; memset(w, 0, sizeof w);
    for (int i = 0; i < t.n; i++) { uint64_t h = fnv64(t.s[i], t.len[i]); h ^= h >> 33; h *= 0xff51afd7ed558ccdULL; h ^= h >> 33; for (int b = 0; b < 64; b++) w[b] += (h >> b & 1) ? 1 : -1; }
    uint64_t r = 0; for (int b = 0; b < 64; b++) if (w[b] > 0) r |= 1ULL << b;
    toks_free(&t); return kc_int((int64_t)r);
}
static KcValue f_hamming_bits(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    G_POS_V = pos; need_args(n, 2, 2, "hamming_bits"); uint64_t x = (uint64_t)A_int(a, n, 0, "hamming_bits", 0, false) ^ (uint64_t)A_int(a, n, 1, "hamming_bits", 0, false);
    int c = 0; while (x) { c += (int)(x & 1); x >>= 1; } return kc_int(c);
}

/* ============================================================ frequency / vocabulary / collocations */

static int cmp_int_desc_stable(const void *x, const void *y) { return cmp_scored_desc(x, y); }
static KcValue f_freq(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    G_POS_V = pos; need_args(n, 1, 2, "freq"); Toks t; get_toks(&t, a, n, 0, "freq"); int64_t top = A_int(a, n, 1, "freq", 0, true);
    SMap M; smap_init(&M); for (int i = 0; i < t.n; i++) smap_add(&M, t.s[i], t.len[i], NULL);
    int *cnt = calloc(M.cnt + 1, sizeof(int)); for (int i = 0; i < t.n; i++) cnt[smap_find(&M, t.s[i], t.len[i])]++;
    Scored *sc = kc_malloc(sizeof(Scored) * (M.cnt + 1)); for (int i = 0; i < M.cnt; i++) { sc[i].score = cnt[i]; sc[i].idx = i; }
    qsort(sc, M.cnt, sizeof(Scored), cmp_int_desc_stable);
    KcValue arr = kc_array_new(); for (int i = 0; i < M.cnt && (top <= 0 || i < top); i++) kc_array_push(arr, pair(vstr(M.key[sc[i].idx], M.klen[sc[i].idx]), kc_int(cnt[sc[i].idx])));
    free(sc); free(cnt); smap_free(&M); toks_free(&t); return arr;
}
/* docs argument: array whose items are token arrays or raw strings */
static void docs_arg(KcValue *a, int n, int i, const char *fn, Toks **docs, int *nd) {
    if (i >= n || a[i].type != VAL_ARRAY) FAIL("nlp.%s: argument %d must be an array of documents (token arrays or strings)", fn, i + 1);
    KcArray *ar = a[i].as.arr; *nd = ar->count; *docs = kc_malloc(sizeof(Toks) * (ar->count + 1));
    for (int d = 0; d < ar->count; d++) get_toks(&(*docs)[d], ar->items, ar->count, d, fn);
}
static void docs_free(Toks *docs, int nd) { for (int d = 0; d < nd; d++) toks_free(&docs[d]); free(docs); }
static int cmp_str_ptr(const void *x, const void *y) { return strcmp(*(char *const *)x, *(char *const *)y); }

static KcValue f_vocab(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    G_POS_V = pos; need_args(n, 1, 3, "vocab"); Toks *docs; int nd; docs_arg(a, n, 0, "vocab", &docs, &nd);
    int64_t minc = A_int(a, n, 1, "vocab", 1, true), maxs = A_int(a, n, 2, "vocab", 0, true);
    SMap M; smap_init(&M); for (int d = 0; d < nd; d++) for (int i = 0; i < docs[d].n; i++) smap_add(&M, docs[d].s[i], docs[d].len[i], NULL);
    int *cnt = calloc(M.cnt + 1, sizeof(int)); for (int d = 0; d < nd; d++) for (int i = 0; i < docs[d].n; i++) cnt[smap_find(&M, docs[d].s[i], docs[d].len[i])]++;
    Scored *sc = kc_malloc(sizeof(Scored) * (M.cnt + 1)); int m = 0;
    for (int i = 0; i < M.cnt; i++) if (cnt[i] >= minc) { sc[m].score = cnt[i]; sc[m].idx = i; m++; }
    qsort(sc, m, sizeof(Scored), cmp_int_desc_stable);
    KcValue arr = kc_array_new(); for (int i = 0; i < m && (maxs <= 0 || i < maxs); i++) kc_array_push(arr, vstr(M.key[sc[i].idx], M.klen[sc[i].idx]));
    free(sc); free(cnt); smap_free(&M); docs_free(docs, nd); return arr;
}
static KcValue f_bow(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    G_POS_V = pos; need_args(n, 2, 2, "bow"); Toks t, v; get_toks(&t, a, n, 0, "bow"); get_toks(&v, a, n, 1, "bow");
    SMap M; smap_init(&M); for (int i = 0; i < v.n; i++) smap_add(&M, v.s[i], v.len[i], NULL);
    int *c = calloc(M.cnt + 1, sizeof(int)); for (int i = 0; i < t.n; i++) { int id = smap_find(&M, t.s[i], t.len[i]); if (id >= 0) c[id]++; }
    KcValue arr = kc_array_new(); for (int i = 0; i < M.cnt; i++) kc_array_push(arr, kc_int(c[i]));
    free(c); smap_free(&M); toks_free(&t); toks_free(&v); return arr;
}
static KcValue f_collocations(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    G_POS_V = pos; need_args(n, 1, 3, "collocations"); Toks t; get_toks(&t, a, n, 0, "collocations");
    int64_t top = A_int(a, n, 1, "collocations", 10, true), minc = A_int(a, n, 2, "collocations", 2, true);
    SMap U, B; smap_init(&U); smap_init(&B); int N = t.n;
    for (int i = 0; i < N; i++) smap_add(&U, t.s[i], t.len[i], NULL);
    int *uc = calloc(U.cnt + 1, sizeof(int)); for (int i = 0; i < N; i++) uc[smap_find(&U, t.s[i], t.len[i])]++;
    for (int i = 0; i + 1 < N; i++) { Buf k; buf_init(&k); buf_add(&k, t.s[i], t.len[i]); buf_ch(&k, 0x1F); buf_add(&k, t.s[i + 1], t.len[i + 1]); smap_add(&B, k.p, k.len, NULL); free(k.p); }
    int *bc = calloc(B.cnt + 1, sizeof(int));
    for (int i = 0; i + 1 < N; i++) { Buf k; buf_init(&k); buf_add(&k, t.s[i], t.len[i]); buf_ch(&k, 0x1F); buf_add(&k, t.s[i + 1], t.len[i + 1]); bc[smap_find(&B, k.p, k.len)]++; free(k.p); }
    Scored *sc = kc_malloc(sizeof(Scored) * (B.cnt + 1)); int m = 0;
    for (int b = 0; b < B.cnt; b++) {
        if (bc[b] < minc) continue;
        char *sep = memchr(B.key[b], 0x1F, B.klen[b]); int l1 = (int)(sep - B.key[b]); int l2 = B.klen[b] - l1 - 1;
        int x = smap_find(&U, B.key[b], l1), y = smap_find(&U, sep + 1, l2);
        double pmi = log2(((double)bc[b] / (N - 1)) / (((double)uc[x] / N) * ((double)uc[y] / N)));
        sc[m].score = pmi; sc[m].idx = b; m++;
    }
    qsort(sc, m, sizeof(Scored), cmp_scored_desc);
    KcValue arr = kc_array_new();
    for (int i = 0; i < m && i < top; i++) {
        int b = sc[i].idx; Buf o; buf_init(&o); for (int q = 0; q < B.klen[b]; q++) buf_ch(&o, B.key[b][q] == 0x1F ? ' ' : B.key[b][q]);
        kc_array_push(arr, triple(buf_value(&o), kc_float(sc[i].score), kc_int(bc[b])));
    }
    free(sc); free(uc); free(bc); smap_free(&U); smap_free(&B); toks_free(&t); return arr;
}
static KcValue f_stats(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    G_POS_V = pos; need_args(n, 1, 1, "stats"); int len; const char *s = A_str(a, n, 0, "stats", &len);
    int chars = 0; for (int i = 0; i < len;) { u8next(s, len, &i); chars++; }
    KcValue toks = tokenize_value(s, len, "ld"); int words = toks.as.arr->count; SMap M; smap_init(&M); long wl = 0;
    for (int i = 0; i < words; i++) { KcString *k = toks.as.arr->items[i].as.s; smap_add(&M, k->data, k->len, NULL); int c = 0; for (int q = 0; q < k->len;) { u8next(k->data, k->len, &q); c++; } wl += c; }
    int uniq = M.cnt; smap_free(&M);
    Span *sp; int sc = split_sents(s, len, &sp); free(sp);
    int paras = 0; bool in = false, blank = true;
    for (int i = 0; i <= len; i++) { if (i == len || s[i] == '\n') { if (!blank) { if (!in) { paras++; in = true; } } else in = false; blank = true; } else if (!isspace((unsigned char)s[i])) blank = false; }
    const char *nm[] = { "chars", "words", "unique_words", "sentences", "paragraphs", "avg_word_len", "avg_sentence_len", "lexical_diversity" };
    KcValue v[] = { kc_int(chars), kc_int(words), kc_int(uniq), kc_int(sc), kc_int(paras), kc_float(words ? (double)wl / words : 0), kc_float(sc ? (double)words / sc : 0), kc_float(words ? (double)uniq / words : 0) };
    return mk_struct("TextStats", 8, nm, v);
}

/* ============================================================ model handles */

enum { M_TFIDF = 0, M_BM25 = 1, M_NB = 2, M_SPELL = 3 };
static KcValue mk_model(int kind, void *p) {
    KcHandle *h = kc_malloc(sizeof *h); h->kind = H_NLP; h->ptr = p; h->size = 0; h->extra = kind;
    KcValue v; v.type = VAL_HANDLE; v.as.handle = h; return v;
}
static void *get_model(KcValue *a, int n, int i, int kind, const char *fn, const char *what) {
    if (i >= n || a[i].type != VAL_HANDLE || a[i].as.handle->kind != H_NLP || a[i].as.handle->extra != kind)
        FAIL("nlp.%s: argument %d must be a %s model (from nlp.%s_*)", fn, i + 1, what, what);
    return a[i].as.handle->ptr;
}

/* ---- TF-IDF ---- */
typedef struct { SMap vocab; double *idf; int ndocs; bool sublinear; } Tfidf;
static KcValue f_tfidf_fit(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    G_POS_V = pos; need_args(n, 1, 4, "tfidf_fit"); Toks *docs; int nd; docs_arg(a, n, 0, "tfidf_fit", &docs, &nd);
    int64_t mindf = A_int(a, n, 1, "tfidf_fit", 1, true), maxf = A_int(a, n, 2, "tfidf_fit", 0, true);
    const char *fl = A_flags(a, n, 3, "tfidf_fit");
    if (nd == 0) FAIL("nlp.tfidf_fit: need at least one document");
    SMap all; smap_init(&all); int *df = NULL;
    for (int d = 0; d < nd; d++) for (int i = 0; i < docs[d].n; i++) smap_add(&all, docs[d].s[i], docs[d].len[i], NULL);
    df = calloc(all.cnt + 1, sizeof(int)); int *seen = malloc(sizeof(int) * (all.cnt + 1)); for (int i = 0; i <= all.cnt; i++) seen[i] = -1;
    for (int d = 0; d < nd; d++) for (int i = 0; i < docs[d].n; i++) { int id = smap_find(&all, docs[d].s[i], docs[d].len[i]); if (seen[id] != d) { seen[id] = d; df[id]++; } }
    free(seen);
    Scored *sc = kc_malloc(sizeof(Scored) * (all.cnt + 1)); int m = 0;
    for (int i = 0; i < all.cnt; i++) if (df[i] >= mindf) { sc[m].score = df[i]; sc[m].idx = i; m++; }
    if (maxf > 0 && m > maxf) { qsort(sc, m, sizeof(Scored), cmp_scored_desc); m = (int)maxf; }
    char **keys = kc_malloc(sizeof(char *) * (m + 1)); int *back = kc_malloc(sizeof(int) * (m + 1));
    for (int i = 0; i < m; i++) keys[i] = all.key[sc[i].idx];
    qsort(keys, m, sizeof(char *), cmp_str_ptr);                      /* alphabetical column order */
    Tfidf *M = kc_malloc(sizeof *M); smap_init(&M->vocab); M->idf = kc_malloc(sizeof(double) * (m + 1)); M->ndocs = nd; M->sublinear = strchr(fl, 's') != NULL;
    for (int i = 0; i < m; i++) {
        int id = smap_add(&M->vocab, keys[i], (int)strlen(keys[i]), NULL); int g = smap_find(&all, keys[i], (int)strlen(keys[i]));
        M->idf[id] = log((1.0 + nd) / (1.0 + df[g])) + 1.0; (void)back;
    }
    free(keys); free(back); free(sc); free(df); smap_free(&all); docs_free(docs, nd);
    return mk_model(M_TFIDF, M);
}
/* sparse tf-idf row, l2-normalised: parallel arrays idx[] / w[]; returns count */
static int tfidf_row(Tfidf *M, Toks *t, int **idx, double **w) {
    int V = M->vocab.cnt; int *cnt = calloc(V + 1, sizeof(int)); int nz = 0;
    for (int i = 0; i < t->n; i++) { int id = smap_find(&M->vocab, t->s[i], t->len[i]); if (id >= 0) { if (!cnt[id]++) nz++; } }
    *idx = kc_malloc(sizeof(int) * (nz + 1)); *w = kc_malloc(sizeof(double) * (nz + 1)); int k = 0; double norm = 0;
    for (int id = 0; id < V; id++) if (cnt[id]) { double tf = M->sublinear ? 1.0 + log((double)cnt[id]) : (double)cnt[id]; (*idx)[k] = id; (*w)[k] = tf * M->idf[id]; norm += (*w)[k] * (*w)[k]; k++; }
    norm = sqrt(norm); if (norm > 0) for (int i = 0; i < k; i++) (*w)[i] /= norm;
    free(cnt); return k;
}
static KcValue f_tfidf_vector(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    G_POS_V = pos; need_args(n, 2, 2, "tfidf_vector"); Tfidf *M = get_model(a, n, 0, M_TFIDF, "tfidf_vector", "tfidf");
    Toks t; get_toks(&t, a, n, 1, "tfidf_vector"); int *idx; double *w; int k = tfidf_row(M, &t, &idx, &w);
    int V = M->vocab.cnt; double *d = calloc(V + 1, sizeof(double)); for (int i = 0; i < k; i++) d[idx[i]] = w[i];
    KcValue arr = kc_array_new(); for (int i = 0; i < V; i++) kc_array_push(arr, kc_float(d[i]));
    free(d); free(idx); free(w); toks_free(&t); return arr;
}
static KcValue f_tfidf_sparse(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    G_POS_V = pos; need_args(n, 2, 2, "tfidf_sparse"); Tfidf *M = get_model(a, n, 0, M_TFIDF, "tfidf_sparse", "tfidf");
    Toks t; get_toks(&t, a, n, 1, "tfidf_sparse"); int *idx; double *w; int k = tfidf_row(M, &t, &idx, &w);
    KcValue arr = kc_array_new(); for (int i = 0; i < k; i++) kc_array_push(arr, pair(kc_int(idx[i]), kc_float(w[i])));
    free(idx); free(w); toks_free(&t); return arr;
}
static KcValue f_tfidf_top(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    G_POS_V = pos; need_args(n, 2, 3, "tfidf_top"); Tfidf *M = get_model(a, n, 0, M_TFIDF, "tfidf_top", "tfidf");
    Toks t; get_toks(&t, a, n, 1, "tfidf_top"); int64_t top = A_int(a, n, 2, "tfidf_top", 10, true);
    int *idx; double *w; int k = tfidf_row(M, &t, &idx, &w); Scored *sc = kc_malloc(sizeof(Scored) * (k + 1));
    for (int i = 0; i < k; i++) { sc[i].score = w[i]; sc[i].idx = idx[i]; }
    qsort(sc, k, sizeof(Scored), cmp_scored_desc);
    KcValue arr = kc_array_new(); for (int i = 0; i < k && i < top; i++) kc_array_push(arr, pair(vstr(M->vocab.key[sc[i].idx], M->vocab.klen[sc[i].idx]), kc_float(sc[i].score)));
    free(sc); free(idx); free(w); toks_free(&t); return arr;
}
static KcValue f_tfidf_vocab(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    G_POS_V = pos; need_args(n, 1, 1, "tfidf_vocab"); Tfidf *M = get_model(a, n, 0, M_TFIDF, "tfidf_vocab", "tfidf");
    KcValue arr = kc_array_new(); for (int i = 0; i < M->vocab.cnt; i++) kc_array_push(arr, vstr(M->vocab.key[i], M->vocab.klen[i])); return arr;
}
static KcValue f_tfidf_size(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    G_POS_V = pos; need_args(n, 1, 1, "tfidf_size"); Tfidf *M = get_model(a, n, 0, M_TFIDF, "tfidf_size", "tfidf"); return kc_int(M->vocab.cnt);
}

/* ---- BM25 ---- */
typedef struct { int *d, *tf; int n, cap; } Post;
typedef struct { SMap vocab; Post *post; int ndocs; double *dl; double avgdl, k1, b; } Bm25;
static KcValue f_bm25_new(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    G_POS_V = pos; need_args(n, 1, 3, "bm25_new"); Toks *docs; int nd; docs_arg(a, n, 0, "bm25_new", &docs, &nd);
    Bm25 *M = kc_malloc(sizeof *M); M->k1 = A_num(a, n, 1, "bm25_new", 1.5, true); M->b = A_num(a, n, 2, "bm25_new", 0.75, true);
    if (M->k1 < 0 || M->b < 0 || M->b > 1) FAIL("nlp.bm25_new: need k1 >= 0 and 0 <= b <= 1");
    smap_init(&M->vocab); M->ndocs = nd; M->dl = kc_malloc(sizeof(double) * (nd + 1)); int pc = 64; M->post = kc_malloc(sizeof(Post) * pc); double tot = 0;
    for (int d = 0; d < nd; d++) {
        M->dl[d] = docs[d].n; tot += docs[d].n;
        for (int i = 0; i < docs[d].n; i++) {
            bool nw; int id = smap_add(&M->vocab, docs[d].s[i], docs[d].len[i], &nw);
            if (nw) { if (id >= pc) { pc *= 2; M->post = kc_realloc(M->post, sizeof(Post) * pc); } M->post[id].n = 0; M->post[id].cap = 4; M->post[id].d = kc_malloc(sizeof(int) * 4); M->post[id].tf = kc_malloc(sizeof(int) * 4); }
            Post *p = &M->post[id];
            if (p->n && p->d[p->n - 1] == d) p->tf[p->n - 1]++;
            else { if (p->n == p->cap) { p->cap *= 2; p->d = kc_realloc(p->d, sizeof(int) * p->cap); p->tf = kc_realloc(p->tf, sizeof(int) * p->cap); } p->d[p->n] = d; p->tf[p->n] = 1; p->n++; }
        }
    }
    M->avgdl = nd ? tot / nd : 0; docs_free(docs, nd); return mk_model(M_BM25, M);
}
static void bm25_scores(Bm25 *M, Toks *q, double *acc) {
    SMap seen; smap_init(&seen);
    for (int i = 0; i < q->n; i++) {
        bool nw; smap_add(&seen, q->s[i], q->len[i], &nw); if (!nw) continue;
        int id = smap_find(&M->vocab, q->s[i], q->len[i]); if (id < 0) continue;
        Post *p = &M->post[id]; double idf = log(1.0 + (M->ndocs - p->n + 0.5) / (p->n + 0.5));
        for (int k = 0; k < p->n; k++) {
            double tf = p->tf[k], norm = 1.0 - M->b + (M->avgdl > 0 ? M->b * M->dl[p->d[k]] / M->avgdl : 0);
            acc[p->d[k]] += idf * tf * (M->k1 + 1.0) / (tf + M->k1 * norm);
        }
    }
    smap_free(&seen);
}
static KcValue f_bm25_search(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    G_POS_V = pos; need_args(n, 2, 3, "bm25_search"); Bm25 *M = get_model(a, n, 0, M_BM25, "bm25_search", "bm25");
    Toks q; get_toks(&q, a, n, 1, "bm25_search"); int64_t k = A_int(a, n, 2, "bm25_search", 10, true);
    double *acc = calloc(M->ndocs + 1, sizeof(double)); bm25_scores(M, &q, acc);
    Scored *sc = kc_malloc(sizeof(Scored) * (M->ndocs + 1)); int m = 0;
    for (int d = 0; d < M->ndocs; d++) if (acc[d] > 0) { sc[m].score = acc[d]; sc[m].idx = d; m++; }
    qsort(sc, m, sizeof(Scored), cmp_scored_desc);
    KcValue arr = kc_array_new(); for (int i = 0; i < m && i < k; i++) kc_array_push(arr, pair(kc_int(sc[i].idx), kc_float(sc[i].score)));
    free(sc); free(acc); toks_free(&q); return arr;
}
static KcValue f_bm25_score(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    G_POS_V = pos; need_args(n, 3, 3, "bm25_score"); Bm25 *M = get_model(a, n, 0, M_BM25, "bm25_score", "bm25");
    Toks q; get_toks(&q, a, n, 1, "bm25_score"); int64_t d = A_int(a, n, 2, "bm25_score", 0, false);
    if (d < 0 || d >= M->ndocs) FAIL("nlp.bm25_score: document index %lld out of range (0..%d)", (long long)d, M->ndocs - 1);
    double *acc = calloc(M->ndocs + 1, sizeof(double)); bm25_scores(M, &q, acc); double r = acc[d]; free(acc); toks_free(&q); return kc_float(r);
}
static KcValue f_bm25_size(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    G_POS_V = pos; need_args(n, 1, 1, "bm25_size"); Bm25 *M = get_model(a, n, 0, M_BM25, "bm25_size", "bm25"); return kc_int(M->ndocs);
}

/* ---- multinomial naive Bayes ---- */
typedef struct { SMap cls, vocab; int **wc; int *wcap; double *total; int *docs; int ccap, ndocs; double alpha; } Nb;
static KcValue f_nb_new(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    G_POS_V = pos; need_args(n, 0, 1, "nb_new"); Nb *M = kc_malloc(sizeof *M); memset(M, 0, sizeof *M);
    M->alpha = A_num(a, n, 0, "nb_new", 1.0, true); if (M->alpha <= 0) FAIL("nlp.nb_new: alpha must be > 0");
    smap_init(&M->cls); smap_init(&M->vocab); M->ccap = 8; M->wc = kc_malloc(sizeof(int *) * 8); M->wcap = kc_malloc(sizeof(int) * 8); M->total = kc_malloc(sizeof(double) * 8); M->docs = kc_malloc(sizeof(int) * 8);
    return mk_model(M_NB, M);
}
static void nb_train_one(Nb *M, Toks *t, const char *lab, int ll) {
    bool nw; int c = smap_add(&M->cls, lab, ll, &nw);
    if (nw) { if (c >= M->ccap) { M->ccap *= 2; M->wc = kc_realloc(M->wc, sizeof(int *) * M->ccap); M->wcap = kc_realloc(M->wcap, sizeof(int) * M->ccap); M->total = kc_realloc(M->total, sizeof(double) * M->ccap); M->docs = kc_realloc(M->docs, sizeof(int) * M->ccap); }
              M->wcap[c] = 64; M->wc[c] = calloc(64, sizeof(int)); M->total[c] = 0; M->docs[c] = 0; }
    M->docs[c]++; M->ndocs++;
    for (int i = 0; i < t->n; i++) {
        int w = smap_add(&M->vocab, t->s[i], t->len[i], NULL);
        if (w >= M->wcap[c]) { int nc = M->wcap[c]; while (nc <= w) nc *= 2; M->wc[c] = kc_realloc(M->wc[c], sizeof(int) * nc); memset(M->wc[c] + M->wcap[c], 0, sizeof(int) * (nc - M->wcap[c])); M->wcap[c] = nc; }
        M->wc[c][w]++; M->total[c] += 1;
    }
}
static KcValue f_nb_train(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    G_POS_V = pos; need_args(n, 3, 3, "nb_train"); Nb *M = get_model(a, n, 0, M_NB, "nb_train", "nb");
    Toks t; get_toks(&t, a, n, 1, "nb_train"); int ll; const char *lab = A_str(a, n, 2, "nb_train", &ll);
    nb_train_one(M, &t, lab, ll); toks_free(&t); return kc_int(M->ndocs);
}
static KcValue f_nb_train_many(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    G_POS_V = pos; need_args(n, 3, 3, "nb_train_many"); Nb *M = get_model(a, n, 0, M_NB, "nb_train_many", "nb");
    Toks *docs; int nd; docs_arg(a, n, 1, "nb_train_many", &docs, &nd);
    if (a[2].type != VAL_ARRAY || a[2].as.arr->count != nd) { docs_free(docs, nd); FAIL("nlp.nb_train_many: labels must be an array with one label per document"); }
    for (int d = 0; d < nd; d++) { KcValue l = a[2].as.arr->items[d]; if (l.type != VAL_STRING) { docs_free(docs, nd); FAIL("nlp.nb_train_many: label %d is not a string", d + 1); } nb_train_one(M, &docs[d], l.as.s->data, l.as.s->len); }
    docs_free(docs, nd); return kc_int(M->ndocs);
}
static double *nb_probs(Nb *M, Toks *t) {
    int C = M->cls.cnt, V = M->vocab.cnt; double *lp = kc_malloc(sizeof(double) * (C + 1)), mx = -1e300;
    int *ids = kc_malloc(sizeof(int) * (t->n + 1)); int ni = 0;
    for (int i = 0; i < t->n; i++) { int id = smap_find(&M->vocab, t->s[i], t->len[i]); if (id >= 0) ids[ni++] = id; }
    for (int c = 0; c < C; c++) {
        double s = log((double)M->docs[c] / M->ndocs), den = M->total[c] + M->alpha * V;
        for (int i = 0; i < ni; i++) { int w = ids[i]; double cnt = w < M->wcap[c] ? M->wc[c][w] : 0; s += log((cnt + M->alpha) / den); }
        lp[c] = s; if (s > mx) mx = s;
    }
    double z = 0; for (int c = 0; c < C; c++) { lp[c] = exp(lp[c] - mx); z += lp[c]; } for (int c = 0; c < C; c++) lp[c] /= z;
    free(ids); return lp;
}
static KcValue f_nb_scores(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    G_POS_V = pos; need_args(n, 2, 2, "nb_scores"); Nb *M = get_model(a, n, 0, M_NB, "nb_scores", "nb");
    if (M->cls.cnt == 0) FAIL("nlp.nb_scores: model has no training data"); Toks t; get_toks(&t, a, n, 1, "nb_scores");
    double *p = nb_probs(M, &t); int C = M->cls.cnt; Scored *sc = kc_malloc(sizeof(Scored) * C);
    for (int c = 0; c < C; c++) { sc[c].score = p[c]; sc[c].idx = c; } qsort(sc, C, sizeof(Scored), cmp_scored_desc);
    KcValue arr = kc_array_new(); for (int c = 0; c < C; c++) kc_array_push(arr, pair(vstr(M->cls.key[sc[c].idx], M->cls.klen[sc[c].idx]), kc_float(sc[c].score)));
    free(sc); free(p); toks_free(&t); return arr;
}
static KcValue f_nb_predict(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    G_POS_V = pos; need_args(n, 2, 2, "nb_predict"); Nb *M = get_model(a, n, 0, M_NB, "nb_predict", "nb");
    if (M->cls.cnt == 0) return kc_null(); Toks t; get_toks(&t, a, n, 1, "nb_predict");
    double *p = nb_probs(M, &t); int best = 0; for (int c = 1; c < M->cls.cnt; c++) if (p[c] > p[best]) best = c;
    free(p); toks_free(&t); return vstr(M->cls.key[best], M->cls.klen[best]);
}
static KcValue f_nb_labels(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    G_POS_V = pos; need_args(n, 1, 1, "nb_labels"); Nb *M = get_model(a, n, 0, M_NB, "nb_labels", "nb");
    KcValue arr = kc_array_new(); for (int c = 0; c < M->cls.cnt; c++) kc_array_push(arr, vstr(M->cls.key[c], M->cls.klen[c])); return arr;
}

/* ---- spelling correction ---- */
typedef struct { SMap w; int *f; int fcap; long total; } Spell;
static int cp_count(const char *s, int l) { int c = 0; for (int i = 0; i < l; i++) if (((unsigned char)s[i] & 0xC0) != 0x80) c++; return c; }
static void spell_feed(Spell *S, Toks *t) {
    for (int i = 0; i < t->n; i++) {
        Buf b; buf_init(&b); for (int k = 0; k < t->len[i];) buf_cp(&b, cp_lower(u8next(t->s[i], t->len[i], &k)));
        bool has_digit = false, has_letter = false; for (int k = 0; k < b.len; k++) { if (isdigit((unsigned char)b.p[k])) has_digit = true; else has_letter = true; }
        if (!has_letter || has_digit) { free(b.p); continue; }
        int id = smap_add(&S->w, b.p, b.len, NULL);
        if (id >= S->fcap) { int nc = S->fcap ? S->fcap : 64; while (nc <= id) nc *= 2; S->f = kc_realloc(S->f, sizeof(int) * nc); memset(S->f + S->fcap, 0, sizeof(int) * (nc - S->fcap)); S->fcap = nc; }
        S->f[id]++; S->total++; free(b.p);
    }
}
static KcValue f_spell_new(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    G_POS_V = pos; need_args(n, 0, 1, "spell_new"); Spell *S = kc_malloc(sizeof *S); memset(S, 0, sizeof *S); smap_init(&S->w);
    if (n) { Toks t; get_toks(&t, a, n, 0, "spell_new"); spell_feed(S, &t); toks_free(&t); }
    return mk_model(M_SPELL, S);
}
static KcValue f_spell_add(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    G_POS_V = pos; need_args(n, 2, 2, "spell_add"); Spell *S = get_model(a, n, 0, M_SPELL, "spell_add", "spell");
    Toks t; get_toks(&t, a, n, 1, "spell_add"); spell_feed(S, &t); toks_free(&t); return kc_int(S->w.cnt);
}
typedef struct { int id, dist, freq; } Cand;
static int cmp_cand(const void *x, const void *y) {
    const Cand *a = x, *b = y; if (a->dist != b->dist) return a->dist - b->dist; if (a->freq != b->freq) return b->freq - a->freq; return a->id - b->id;
}
static int spell_candidates(Spell *S, const char *word, int wl, int maxd, Cand **out) {
    Buf lw; buf_init(&lw); for (int k = 0; k < wl;) buf_cp(&lw, cp_lower(u8next(word, wl, &k)));
    int qn; uint32_t *qc = u8decode(lw.p, lw.len, &qn); Cand *r = kc_malloc(sizeof(Cand) * (S->w.cnt + 1)); int m = 0;
    int ex = smap_find(&S->w, lw.p, lw.len);
    if (ex >= 0) { r[m].id = ex; r[m].dist = 0; r[m].freq = S->f[ex]; m++; }
    else for (int id = 0; id < S->w.cnt; id++) {
        int cl = cp_count(S->w.key[id], S->w.klen[id]); if (abs(cl - qn) > maxd) continue;
        int cn; uint32_t *cc = u8decode(S->w.key[id], S->w.klen[id], &cn); int d = dist_lev(qc, qn, cc, cn, true); free(cc);
        if (d <= maxd) { r[m].id = id; r[m].dist = d; r[m].freq = S->f[id]; m++; }
    }
    qsort(r, m, sizeof(Cand), cmp_cand); free(qc); free(lw.p); *out = r; return m;
}
static KcValue match_case(const char *orig, int ol, const char *w, int wl) {
    bool first = false, all = ol > 0, any = false;
    { int i = 0; bool f = true; while (i < ol) { uint32_t c = u8next(orig, ol, &i); if (f) first = cp_is_upper(c); f = false; if (cp_is_lower(c)) all = false; if (cp_is_upper(c)) any = true; } }
    if (!any) return vstr(w, wl); Buf o; buf_init(&o); bool f = true;
    for (int i = 0; i < wl;) { uint32_t c = u8next(w, wl, &i); buf_cp(&o, (all && ol > 1) || (f && first) ? cp_upper(c) : c); f = false; }
    return buf_value(&o);
}
static KcValue f_spell_known(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    G_POS_V = pos; need_args(n, 2, 2, "spell_known"); Spell *S = get_model(a, n, 0, M_SPELL, "spell_known", "spell"); int l; const char *w = A_str(a, n, 1, "spell_known", &l);
    Buf b; buf_init(&b); for (int k = 0; k < l;) buf_cp(&b, cp_lower(u8next(w, l, &k))); bool r = smap_find(&S->w, b.p, b.len) >= 0; free(b.p); return kc_bool(r);
}
static KcValue f_spell_correct(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    G_POS_V = pos; need_args(n, 2, 3, "spell_correct"); Spell *S = get_model(a, n, 0, M_SPELL, "spell_correct", "spell");
    int l; const char *w = A_str(a, n, 1, "spell_correct", &l); int64_t md = A_int(a, n, 2, "spell_correct", 2, true); if (md < 1 || md > 3) FAIL("nlp.spell_correct: max_dist must be 1..3");
    Cand *c; int m = spell_candidates(S, w, l, (int)md, &c); KcValue r = m ? match_case(w, l, S->w.key[c[0].id], S->w.klen[c[0].id]) : vstr(w, l); free(c); return r;
}
static KcValue f_spell_suggest(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    G_POS_V = pos; need_args(n, 2, 4, "spell_suggest"); Spell *S = get_model(a, n, 0, M_SPELL, "spell_suggest", "spell");
    int l; const char *w = A_str(a, n, 1, "spell_suggest", &l); int64_t k = A_int(a, n, 2, "spell_suggest", 5, true), md = A_int(a, n, 3, "spell_suggest", 2, true);
    if (md < 1 || md > 3) FAIL("nlp.spell_suggest: max_dist must be 1..3");
    Cand *c; int m = spell_candidates(S, w, l, (int)md, &c); KcValue arr = kc_array_new();
    for (int i = 0; i < m && i < k; i++) kc_array_push(arr, triple(match_case(w, l, S->w.key[c[i].id], S->w.klen[c[i].id]), kc_int(c[i].dist), kc_int(c[i].freq)));
    free(c); return arr;
}
static KcValue f_spell_size(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    G_POS_V = pos; need_args(n, 1, 1, "spell_size"); Spell *S = get_model(a, n, 0, M_SPELL, "spell_size", "spell"); return kc_int(S->w.cnt);
}

/* ============================================================ sentiment (lexicon, VADER-style rules) */

static const struct { const char *w; double v; } LEX[] = {
 {"good",1.9},{"great",3.1},{"excellent",3.2},{"amazing",2.8},{"awesome",3.1},{"wonderful",2.7},{"fantastic",2.6},{"love",3.2},{"loved",2.9},
 {"loves",2.7},{"loving",2.9},{"lovely",2.8},{"like",1.5},{"liked",1.8},{"likes",1.5},{"nice",1.8},{"happy",2.7},{"glad",2.0},{"pleased",1.9},
 {"delighted",2.9},{"enjoy",2.2},{"enjoyed",2.3},{"enjoying",2.4},{"enjoyable",1.9},{"fun",2.3},{"best",3.2},{"better",1.9},{"perfect",2.7},
 {"beautiful",2.9},{"brilliant",2.8},{"superb",3.0},{"outstanding",3.0},{"impressive",2.3},{"impressed",2.0},{"recommend",1.5},
 {"recommended",1.7},{"easy",1.9},{"helpful",1.8},{"friendly",2.2},{"fast",1.2},{"smooth",1.5},{"reliable",1.8},{"solid",1.4},{"strong",1.3},
 {"win",2.8},{"winner",2.8},{"won",2.7},{"success",2.7},{"successful",2.8},{"thank",1.5},{"thanks",1.9},{"thankful",2.1},{"grateful",2.3},
 {"congratulations",2.9},{"congrats",2.4},{"cool",1.3},{"neat",2.0},{"super",2.9},{"terrific",2.1},{"marvelous",2.9},{"magnificent",2.9},
 {"gorgeous",3.0},{"incredible",2.5},{"exciting",2.2},{"excited",1.4},{"interesting",1.7},{"valuable",2.1},{"useful",1.9},{"worth",0.9},
 {"worthy",1.5},{"smart",1.7},{"clever",2.0},{"genius",2.4},{"comfortable",1.5},{"safe",1.9},{"secure",1.4},{"clean",1.7},{"fresh",1.3},
 {"tasty",2.2},{"delicious",3.0},{"yummy",2.4},{"pleasant",2.3},{"pleasure",2.7},{"joy",2.8},{"joyful",2.9},{"hope",1.9},{"hopeful",2.3},
 {"proud",2.1},{"honest",2.0},{"kind",2.4},{"generous",2.3},{"calm",1.3},{"peaceful",2.2},{"positive",2.6},{"improved",1.8},{"improve",1.9},
 {"improvement",1.9},{"effective",2.1},{"efficient",1.8},{"exceptional",2.5},{"flawless",2.7},{"favorite",2.0},{"favourite",2.0},{"adore",2.6},
 {"appreciate",1.7},{"appreciated",2.3},{"praise",2.5},{"welcome",2.0},{"wow",2.8},{"yay",2.4},{"yes",1.7},{"sweet",2.0},{"luck",2.0},
 {"lucky",1.9},{"fine",0.8},{"okay",0.9},{"ok",1.2},{"decent",1.4},{"satisfied",1.8},{"satisfying",2.0},{"stunning",2.7},{"elegant",2.1},
 {"charming",2.4},{"happiness",2.9},{"smile",2.0},{"laugh",2.2},{"beloved",2.3},{"fabulous",2.4},{"splendid",2.6},{"remarkable",2.0},
 {"bad",-2.5},{"terrible",-2.1},{"awful",-2.0},{"horrible",-2.5},{"worst",-3.1},{"worse",-2.1},{"hate",-2.7},{"hated",-3.2},{"hates",-1.9},
 {"hating",-2.3},{"dislike",-1.6},{"poor",-2.1},{"sad",-2.1},{"angry",-2.3},{"mad",-2.2},{"upset",-1.6},{"annoying",-1.7},{"annoyed",-1.6},
 {"boring",-1.3},{"bored",-1.5},{"disappointed",-1.7},{"disappointing",-2.2},{"disappointment",-2.3},{"useless",-1.8},{"waste",-1.8},
 {"wasted",-2.2},{"broken",-1.9},{"fail",-2.5},{"failed",-2.3},{"failure",-2.3},{"fails",-1.8},{"ugly",-2.3},{"stupid",-2.4},{"dumb",-2.3},
 {"idiot",-2.3},{"nasty",-2.6},{"disgusting",-2.4},{"disgusted",-2.6},{"pathetic",-2.7},{"ridiculous",-1.4},{"slow",-1.0},{"difficult",-1.2},
 {"hurt",-2.4},{"pain",-2.3},{"painful",-2.5},{"sick",-2.1},{"tired",-1.2},{"scared",-1.9},{"afraid",-1.5},{"fear",-2.2},{"worried",-1.2},
 {"worry",-1.9},{"trouble",-1.7},{"problem",-1.7},{"problems",-1.7},{"cry",-2.1},{"crying",-2.1},{"lonely",-1.9},{"miserable",-2.4},
 {"depressed",-2.7},{"depressing",-2.2},{"tragic",-2.5},{"tragedy",-3.4},{"death",-2.9},{"dead",-3.3},{"die",-2.9},{"kill",-3.7},
 {"killed",-3.5},{"murder",-3.8},{"war",-2.9},{"violence",-3.1},{"violent",-2.8},{"abuse",-3.2},{"attack",-2.1},{"fraud",-2.8},{"scam",-2.8},
 {"lie",-2.3},{"lies",-1.8},{"liar",-2.5},{"cheat",-2.0},{"cheated",-2.3},{"danger",-2.4},{"dangerous",-2.1},{"nightmare",-2.7},{"mess",-1.5},
 {"rude",-2.1},{"unfair",-2.1},{"unhappy",-2.7},{"unfortunate",-1.9},{"unfortunately",-1.5},{"worthless",-2.8},{"weak",-1.9},{"negative",-2.7},
 {"wrong",-2.1},{"regret",-2.3},{"trash",-1.9},{"garbage",-1.9},{"junk",-1.5},{"horrific",-3.1},{"hell",-3.6},{"damn",-2.5},{"crap",-2.1},
 {"sucks",-1.5},{"suck",-1.5},{"lame",-2.0},{"nonsense",-1.6},{"overpriced",-1.5},{"complaint",-1.5},{"complain",-1.5},{"denied",-2.0},
 {"reject",-1.8},{"rejected",-2.1},{"ignore",-1.5},{"ignored",-1.5},{"stress",-1.9},{"stressed",-1.9},{"anxious",-1.9},{"anxiety",-1.7},
 {"confused",-1.3},{"confusing",-1.5},{"uncomfortable",-1.6},{"unsafe",-2.0},{"dirty",-2.0},{"terrified",-3.0},{"frustrating",-2.3},
 {"frustrated",-2.1},{"frustration",-2.1},{"disaster",-3.1},{"hopeless",-2.3},{"helpless",-2.0},{"mistake",-1.7},{"mistakes",-1.7},
 {"damaged",-2.3},{"lost",-1.3},{"lose",-1.8},{"loser",-2.4},{"cruel",-2.8},{"evil",-3.4},{"guilty",-1.8},{"shame",-2.1},{"jealous",-2.0},
 {"bitter",-1.8},{"toxic",-2.7},{"awfully",-1.5},{"horribly",-2.3},{"badly",-2.1},{"sadly",-1.9},{"painfully",-2.2},
};
static const char NEGATORS[] = "not no never none nobody nothing neither nor nowhere hardly barely scarcely rarely seldom without cannot aint arent cant couldnt darent didnt doesnt dont hadnt hasnt havent isnt mightnt mustnt neednt oughtnt shant shouldnt wasnt werent wont wouldnt";
static const char BOOST_UP[] = "very really extremely absolutely incredibly totally completely utterly highly super so especially particularly truly remarkably exceptionally hugely deeply entirely thoroughly most amazingly";
static const char BOOST_DN[] = "slightly somewhat kinda marginally fairly partly little barely hardly almost mildly";
static SMap LEXMAP; static bool lex_ready = false;
static double lex_get(const char *w, int l) {
    if (!lex_ready) { smap_init(&LEXMAP); for (size_t i = 0; i < sizeof LEX / sizeof LEX[0]; i++) smap_add(&LEXMAP, LEX[i].w, (int)strlen(LEX[i].w), NULL); lex_ready = true; }
    int id = smap_find(&LEXMAP, w, l); return id < 0 ? 0.0 : LEX[id].v;
}
static bool neg_word(const char *w) {
    int l = (int)strlen(w); if (l > 3 && !strcmp(w + l - 3, "n't")) return true;
    return word_in(w, NEGATORS);
}
typedef struct { double compound, pos, neu, neg; } Sent;
static Sent do_sentiment(const char *s, int len) {
    KcValue toks = tokenize_value(s, len, ""); KcArray *T = toks.as.arr; int n = T->count;
    char **lw = kc_malloc(sizeof(char *) * (n + 1)); double *val = calloc(n + 1, sizeof(double)); bool *caps = calloc(n + 1, 1);
    int ncaps = 0, nalpha = 0;
    for (int i = 0; i < n; i++) {
        KcString *k = T->items[i].as.s; Buf b; buf_init(&b); bool up = true, has = false;
        for (int q = 0; q < k->len;) { uint32_t c = u8next(k->data, k->len, &q); if (cp_is_apos(c)) c = '\''; if (cp_is_lower(c)) up = false; if (cp_is_upper(c)) has = true; buf_cp(&b, cp_lower(c)); }
        lw[i] = b.p; caps[i] = up && has && k->len > 1; if (has) nalpha++; if (caps[i]) ncaps++;
    }
    bool diff = ncaps > 0 && ncaps < nalpha;
    int butpos = -1;
    for (int i = 0; i < n; i++) {
        if (!strcmp(lw[i], "but") && butpos < 0) butpos = i;
        double v = lex_get(lw[i], (int)strlen(lw[i])); if (v == 0.0) continue;
        if (diff && caps[i]) v += v > 0 ? 0.733 : -0.733;
        for (int j = 1; j <= 3 && i - j >= 0; j++) {
            double sc = 0; if (word_in(lw[i - j], BOOST_UP)) sc = 0.293; else if (word_in(lw[i - j], BOOST_DN)) sc = -0.293;
            if (sc != 0) { if (v < 0) sc = -sc; if (j == 2) sc *= 0.95; else if (j == 3) sc *= 0.9; if (diff && caps[i - j]) sc += sc > 0 ? 0.733 : -0.733; v += sc; }
        }
        for (int j = 1; j <= 3 && i - j >= 0; j++) if (neg_word(lw[i - j])) { v *= -0.74; break; }
        val[i] = v;
    }
    if (butpos >= 0) for (int i = 0; i < n; i++) { if (i < butpos) val[i] *= 0.5; else if (i > butpos) val[i] *= 1.5; }
    int ex = 0, qm = 0; for (int i = 0; i < len; i++) { if (s[i] == '!') ex++; else if (s[i] == '?') qm++; }
    double emp = (ex > 4 ? 4 : ex) * 0.292; if (qm > 1) emp += qm <= 3 ? qm * 0.18 : 0.96;
    double sum = 0, ps = 0, ns = 0; int neu = 0;
    for (int i = 0; i < n; i++) { sum += val[i]; if (val[i] > 0) ps += val[i] + 1; else if (val[i] < 0) ns += val[i] - 1; else neu++; }
    if (sum > 0) sum += emp; else if (sum < 0) sum -= emp;
    Sent r; r.compound = sum / sqrt(sum * sum + 15.0); if (r.compound > 1) r.compound = 1; if (r.compound < -1) r.compound = -1;
    if (ps > fabs(ns)) ps += emp; else if (ps < fabs(ns)) ns -= emp;
    double tot = ps + fabs(ns) + neu;
    if (tot > 0) { r.pos = ps / tot; r.neg = fabs(ns) / tot; r.neu = neu / tot; } else { r.pos = r.neg = 0; r.neu = 1; }
    for (int i = 0; i < n; i++) free(lw[i]); free(lw); free(val); free(caps); return r;
}
static const char *sent_label(double c, double th) { return c >= th ? "positive" : c <= -th ? "negative" : "neutral"; }
static KcValue f_sentiment(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    G_POS_V = pos; need_args(n, 1, 1, "sentiment"); int len; const char *s = A_str(a, n, 0, "sentiment", &len); Sent r = do_sentiment(s, len);
    const char *nm[] = { "compound", "pos", "neu", "neg", "label" };
    KcValue v[] = { kc_float(r.compound), kc_float(r.pos), kc_float(r.neu), kc_float(r.neg), kc_string(sent_label(r.compound, 0.05)) };
    return mk_struct("Sentiment", 5, nm, v);
}
static KcValue f_sentiment_label(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    G_POS_V = pos; need_args(n, 1, 2, "sentiment_label"); int len; const char *s = A_str(a, n, 0, "sentiment_label", &len);
    double th = A_num(a, n, 1, "sentiment_label", 0.05, true); return kc_string(sent_label(do_sentiment(s, len).compound, th));
}

/* ============================================================ language & script detection */

enum { SC_LATIN, SC_CYR, SC_GREEK, SC_ARABIC, SC_HEBREW, SC_DEVA, SC_THAI, SC_HANGUL, SC_HAN, SC_KANA, SC_OTHER, SC_N };
static const char *SC_NAME[] = { "Latin", "Cyrillic", "Greek", "Arabic", "Hebrew", "Devanagari", "Thai", "Hangul", "Han", "Kana", "Other" };
static int cp_script(uint32_t c) {
    if (c >= KC_RAW) return -1;
    if (c < 0x80) return isalpha((int)c) ? SC_LATIN : -1;
    if ((c >= 0xC0 && c <= 0x24F) || (c >= 0x1E00 && c <= 0x1EFF)) return c == 0xD7 || c == 0xF7 ? -1 : SC_LATIN;
    if (c >= 0x370 && c <= 0x3FF) return SC_GREEK; if (c >= 0x1F00 && c <= 0x1FFF) return SC_GREEK;
    if ((c >= 0x400 && c <= 0x52F)) return SC_CYR; if (c >= 0x590 && c <= 0x5FF) return SC_HEBREW;
    if ((c >= 0x600 && c <= 0x6FF) || (c >= 0x750 && c <= 0x77F) || (c >= 0xFB50 && c <= 0xFDFF) || (c >= 0xFE70 && c <= 0xFEFF)) return SC_ARABIC;
    if (c >= 0x900 && c <= 0x97F) return SC_DEVA; if (c >= 0xE00 && c <= 0xE7F) return SC_THAI;
    if (cp_is_hangul(c)) return SC_HANGUL; if (cp_is_han(c)) return SC_HAN; if (cp_is_kana(c)) return SC_KANA;
    if (cp_is_word(c) && c > 0x24F && !cp_is_combining(c)) return SC_OTHER;
    return -1;
}
static int dominant_script(const char *s, int len, int *counts) {
    int cnt[SC_N] = { 0 }; for (int i = 0; i < len;) { int sc = cp_script(u8next(s, len, &i)); if (sc >= 0) cnt[sc]++; }
    int best = -1, tot = 0; for (int k = 0; k < SC_N; k++) { tot += cnt[k]; if (best < 0 || cnt[k] > cnt[best]) best = k; }
    if (counts) memcpy(counts, cnt, sizeof cnt); return tot ? best : -1;
}
static KcValue f_detect_script(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    G_POS_V = pos; need_args(n, 1, 1, "detect_script"); int len; const char *s = A_str(a, n, 0, "detect_script", &len);
    int b = dominant_script(s, len, NULL); return b < 0 ? kc_null() : kc_string(SC_NAME[b]);
}
static KcValue f_detect_lang(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    G_POS_V = pos; need_args(n, 1, 1, "detect_lang"); int len; const char *s = A_str(a, n, 0, "detect_lang", &len);
    int cnt[SC_N]; int sc = dominant_script(s, len, cnt); const char *code = "und"; double conf = 0.0;
    if (sc == SC_CYR) {
        bool uk = false; for (int i = 0; i < len;) { uint32_t c = u8next(s, len, &i); if (c == 0x457 || c == 0x454 || c == 0x491 || c == 0x407 || c == 0x404 || c == 0x490) uk = true; }
        code = uk ? "uk" : "ru"; conf = uk ? 0.9 : 0.6;
    } else if (sc == SC_GREEK) { code = "el"; conf = 0.95; } else if (sc == SC_HEBREW) { code = "he"; conf = 0.95; }
    else if (sc == SC_ARABIC) {
        bool fa = false; for (int i = 0; i < len;) { uint32_t c = u8next(s, len, &i); if (c == 0x67E || c == 0x686 || c == 0x698 || c == 0x6AF || c == 0x6CC) fa = true; }
        code = fa ? "fa" : "ar"; conf = fa ? 0.8 : 0.6;
    } else if (sc == SC_DEVA) { code = "hi"; conf = 0.6; } else if (sc == SC_THAI) { code = "th"; conf = 0.95; }
    else if (sc == SC_HANGUL) { code = "ko"; conf = 0.95; }
    else if (sc == SC_KANA || (sc == SC_HAN && cnt[SC_KANA] > 0)) { code = "ja"; conf = 0.95; }
    else if (sc == SC_HAN) { code = "zh"; conf = 0.8; }
    else if (sc == SC_LATIN) {
        KcValue toks = tokenize_value(s, len, "ld"); KcArray *T = toks.as.arr; int nl = (int)(sizeof LANGS / sizeof LANGS[0]);
        int hits[16] = { 0 }; int total = T->count;
        for (int i = 0; i < T->count; i++) {
            KcString *k = T->items[i].as.s; Buf b; buf_init(&b); do_strip_accents(k->data, k->len, &b);
            for (int l = 0; l < nl; l++) if (word_in(b.p, LANGS[l].words)) hits[l]++;
            free(b.p);
        }
        int best = 0, sum = 0; for (int l = 0; l < nl; l++) { sum += hits[l]; if (hits[l] > hits[best]) best = l; }
        int second = 0; for (int l = 0; l < nl; l++) if (l != best && hits[l] > second) second = hits[l];
        if (hits[best] >= 2 || (total <= 3 && hits[best] >= 1 && hits[best] > second)) {
            if (hits[best] > second) { code = LANGS[best].code; double share = (double)hits[best] / sum, cov = (double)hits[best] / (total ? total : 1) * 3.5; if (cov > 1) cov = 1; conf = share * (0.4 + 0.6 * cov) * (1.0 - (double)second / (hits[best] * 2.0)); }
        }
    }
    const char *nm[] = { "lang", "confidence", "script" };
    KcValue v[] = { kc_string(code), kc_float(conf), sc < 0 ? kc_null() : kc_string(SC_NAME[sc]) };
    return mk_struct("LangGuess", 3, nm, v);
}

/* ============================================================ extraction */

static bool ascii_alnum(unsigned char c) { return isalnum(c) != 0; }
static KcValue f_emails(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    G_POS_V = pos; need_args(n, 1, 1, "emails"); int len; const char *s = A_str(a, n, 0, "emails", &len); KcValue arr = kc_array_new();
    for (int i = 1; i < len - 1; i++) {
        if (s[i] != '@') continue;
        int l = i; while (l > 0 && (ascii_alnum((unsigned char)s[l - 1]) || strchr("._%+-", s[l - 1]))) l--;
        while (l < i && s[l] == '.') l++;
        int r = i + 1; while (r < len && (ascii_alnum((unsigned char)s[r]) || s[r] == '.' || s[r] == '-')) r++;
        while (r > i + 1 && (s[r - 1] == '.' || s[r - 1] == '-')) r--;
        if (l == i || r <= i + 1) continue;
        int dot = -1; for (int k = r - 1; k > i; k--) if (s[k] == '.') { dot = k; break; }
        if (dot < 0 || dot == i + 1 || r - dot - 1 < 2) continue;
        bool alpha = true; for (int k = dot + 1; k < r; k++) if (!isalpha((unsigned char)s[k])) alpha = false;
        if (!alpha) continue;
        kc_array_push(arr, vstr(s + l, r - l)); i = r;
    }
    return arr;
}
static KcValue f_urls(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    G_POS_V = pos; need_args(n, 1, 1, "urls"); int len; const char *s = A_str(a, n, 0, "urls", &len); KcValue arr = kc_array_new();
    for (int i = 0; i < len; i++) {
        if (i > 0 && ascii_alnum((unsigned char)s[i - 1])) continue;
        int pre = 0; if (ci_at(s, len, i, "https://")) pre = 8; else if (ci_at(s, len, i, "http://")) pre = 7; else if (ci_at(s, len, i, "ftp://")) pre = 6; else if (ci_at(s, len, i, "www.")) pre = 4;
        if (!pre) continue;
        int e = i + pre; while (e < len && !isspace((unsigned char)s[e]) && !strchr("<>\"`", s[e]) && (unsigned char)s[e] >= 0x20) e++;
        for (;;) {
            if (e <= i + pre) break; char c = s[e - 1];
            if (strchr(".,;:!?'", c)) { e--; continue; }
            if (c == ')' || c == ']' || c == '}') { char o = c == ')' ? '(' : c == ']' ? '[' : '{'; int bal = 0; for (int k = i; k < e; k++) { if (s[k] == o) bal++; else if (s[k] == c) bal--; } if (bal < 0) { e--; continue; } }
            break;
        }
        if (e - i > pre) { kc_array_push(arr, vstr(s + i, e - i)); i = e - 1; }
    }
    return arr;
}
static KcValue extract_marked(KcValue *a, int n, KcPos pos, char mark, const char *fn) {
    G_POS_V = pos; need_args(n, 1, 1, fn); int len; const char *s = A_str(a, n, 0, fn, &len); KcValue arr = kc_array_new();
    for (int i = 0; i < len; i++) {
        if (s[i] != mark) continue;
        if (i > 0) { int p = i - 1; while (p > 0 && ((unsigned char)s[p] & 0xC0) == 0x80) p--; int q = p; uint32_t pc = u8next(s, len, &q); if (cp_is_word(pc) || pc == '_' || pc == (uint32_t)(unsigned char)mark) continue; }
        int e = i + 1;
        while (e < len) { int t = e; uint32_t c = u8next(s, len, &t); if (cp_is_word(c) || c == '_') e = t; else break; }
        if (e > i + 1) { kc_array_push(arr, vstr(s + i, e - i)); i = e - 1; }
    }
    return arr;
}
static KcValue f_hashtags(KcInterp *ip, KcValue *a, int n, KcPos pos) { return extract_marked(a, n, pos, '#', "hashtags"); }
static KcValue f_mentions(KcInterp *ip, KcValue *a, int n, KcPos pos) { return extract_marked(a, n, pos, '@', "mentions"); }
static KcValue f_numbers(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    G_POS_V = pos; need_args(n, 1, 1, "numbers"); int len; const char *s = A_str(a, n, 0, "numbers", &len); KcValue arr = kc_array_new();
    for (int i = 0; i < len;) {
        bool sign = false, start = false;
        if (isdigit((unsigned char)s[i])) start = true;
        else if (s[i] == '-' && i + 1 < len && isdigit((unsigned char)s[i + 1]) && (i == 0 || !(ascii_alnum((unsigned char)s[i - 1]) || s[i - 1] == '.' || s[i - 1] == ')'))) { sign = true; start = true; }
        if (!start) { i++; continue; }
        if (i > 0 && !sign && (ascii_alnum((unsigned char)s[i - 1]) && !isdigit((unsigned char)s[i - 1]))) { while (i < len && ascii_alnum((unsigned char)s[i])) i++; continue; }
        int j = i + (sign ? 1 : 0); char tmp[64]; int tk = 0; bool frac = false;
        if (sign) tmp[tk++] = '-';
        while (j < len && isdigit((unsigned char)s[j])) { if (tk < 60) tmp[tk++] = s[j]; j++; }
        while (j + 3 < len + 0 && s[j] == ',' && isdigit((unsigned char)s[j + 1]) && isdigit((unsigned char)s[j + 2]) && isdigit((unsigned char)s[j + 3]) && !(j + 4 < len && isdigit((unsigned char)s[j + 4]))) { for (int k = 1; k <= 3; k++) if (tk < 60) tmp[tk++] = s[j + k]; j += 4; }
        if (j + 1 < len && s[j] == '.' && isdigit((unsigned char)s[j + 1])) { frac = true; if (tk < 60) tmp[tk++] = '.'; j++; while (j < len && isdigit((unsigned char)s[j])) { if (tk < 60) tmp[tk++] = s[j]; j++; } }
        tmp[tk] = 0;
        if (!frac && tk < 18) kc_array_push(arr, kc_int(strtoll(tmp, NULL, 10))); else kc_array_push(arr, kc_float(strtod(tmp, NULL)));
        i = j;
    }
    return arr;
}

/* ============================================================ keywords (RAKE) & extractive summary */

static KcValue f_keywords(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    G_POS_V = pos; need_args(n, 1, 3, "keywords"); int len; const char *s = A_str(a, n, 0, "keywords", &len);
    int64_t top = A_int(a, n, 1, "keywords", 10, true), maxw = A_int(a, n, 2, "keywords", 4, true); if (maxw < 1) FAIL("nlp.keywords: max_words must be >= 1");
    KcValue toks = tokenize_value(s, len, "lp"); KcArray *T = toks.as.arr; int nt = T->count;
    int *ps = kc_malloc(sizeof(int) * (nt + 2)), *pe = kc_malloc(sizeof(int) * (nt + 2)); int np = 0, st = -1;
    for (int i = 0; i <= nt; i++) {
        bool brk = i == nt; if (!brk) { KcString *k = T->items[i].as.s; int q = 0; uint32_t c = u8next(k->data, k->len, &q); brk = !cp_is_word(c) || tok_numeric(k->data, k->len) || is_stop_en(k->data, k->len); }
        if (brk) { if (st >= 0 && i - st <= maxw) { ps[np] = st; pe[np] = i; np++; } st = -1; } else if (st < 0) st = i;
    }
    SMap W; smap_init(&W); for (int p = 0; p < np; p++) for (int i = ps[p]; i < pe[p]; i++) smap_add(&W, T->items[i].as.s->data, T->items[i].as.s->len, NULL);
    double *fr = calloc(W.cnt + 1, sizeof(double)), *dg = calloc(W.cnt + 1, sizeof(double));
    for (int p = 0; p < np; p++) for (int i = ps[p]; i < pe[p]; i++) { int id = smap_find(&W, T->items[i].as.s->data, T->items[i].as.s->len); fr[id] += 1; dg[id] += pe[p] - ps[p]; }
    SMap P; smap_init(&P); double *psc = kc_malloc(sizeof(double) * (np + 1));
    for (int p = 0; p < np; p++) {
        Buf b; buf_init(&b); double sc = 0;
        for (int i = ps[p]; i < pe[p]; i++) { KcString *k = T->items[i].as.s; if (i > ps[p]) buf_ch(&b, ' '); buf_add(&b, k->data, k->len); int id = smap_find(&W, k->data, k->len); sc += dg[id] / fr[id]; }
        bool nw; int id = smap_add(&P, b.p, b.len, &nw); if (nw) psc[id] = sc; free(b.p);
    }
    Scored *sc = kc_malloc(sizeof(Scored) * (P.cnt + 1)); for (int i = 0; i < P.cnt; i++) { sc[i].score = psc[i]; sc[i].idx = i; }
    qsort(sc, P.cnt, sizeof(Scored), cmp_scored_desc);
    KcValue arr = kc_array_new(); for (int i = 0; i < P.cnt && i < top; i++) kc_array_push(arr, pair(vstr(P.key[sc[i].idx], P.klen[sc[i].idx]), kc_float(sc[i].score)));
    free(sc); free(psc); free(fr); free(dg); free(ps); free(pe); smap_free(&W); smap_free(&P); return arr;
}
/* score each sentence by the document frequency of its (stemmed, non-stopword) terms */
static int rank_sents(const char *s, int len, Span **sp, double **score) {
    int c = split_sents(s, len, sp); *score = calloc(c + 1, sizeof(double)); if (!c) return 0;
    SMap M; smap_init(&M); int **ids = kc_malloc(sizeof(int *) * c); int *ni = calloc(c, sizeof(int));
    for (int k = 0; k < c; k++) {
        KcValue toks = tokenize_value(s + (*sp)[k].a, (*sp)[k].b - (*sp)[k].a, "ld"); KcArray *T = toks.as.arr; ids[k] = kc_malloc(sizeof(int) * (T->count + 1));
        for (int i = 0; i < T->count; i++) {
            KcString *w = T->items[i].as.s; if (is_stop_en(w->data, w->len) || w->len < 2) continue;
            char st[64]; int sl = stem_into(w->data, w->len, st); ids[k][ni[k]++] = smap_add(&M, st, sl, NULL);
        }
    }
    double *tf = calloc(M.cnt + 1, sizeof(double)), mx = 0;
    for (int k = 0; k < c; k++) for (int i = 0; i < ni[k]; i++) tf[ids[k][i]] += 1;
    for (int i = 0; i < M.cnt; i++) if (tf[i] > mx) mx = tf[i];
    for (int k = 0; k < c; k++) {
        double sum = 0; for (int i = 0; i < ni[k]; i++) sum += tf[ids[k][i]] / (mx > 0 ? mx : 1);
        double sc = ni[k] ? sum / pow((double)ni[k], 0.6) : 0; if (ni[k] < 4) sc *= 0.5; if (k == 0) sc *= 1.15; (*score)[k] = sc; free(ids[k]);
    }
    free(ids); free(ni); free(tf); smap_free(&M); return c;
}
static int cmp_idx_asc(const void *x, const void *y) { return *(const int *)x - *(const int *)y; }
static KcValue f_summarize(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    G_POS_V = pos; need_args(n, 1, 2, "summarize"); int len; const char *s = A_str(a, n, 0, "summarize", &len); int64_t k = A_int(a, n, 1, "summarize", 3, true);
    Span *sp; double *sc; int c = rank_sents(s, len, &sp, &sc); Buf o; buf_init(&o);
    if (c) {
        Scored *r = kc_malloc(sizeof(Scored) * c); for (int i = 0; i < c; i++) { r[i].score = sc[i]; r[i].idx = i; } qsort(r, c, sizeof(Scored), cmp_scored_desc);
        int m = k < c ? (int)k : c; int *pick = kc_malloc(sizeof(int) * (m + 1)); for (int i = 0; i < m; i++) pick[i] = r[i].idx; qsort(pick, m, sizeof(int), cmp_idx_asc);
        for (int i = 0; i < m; i++) { if (i) buf_ch(&o, ' '); buf_add(&o, s + sp[pick[i]].a, sp[pick[i]].b - sp[pick[i]].a); }
        free(r); free(pick);
    }
    free(sp); free(sc); return buf_value(&o);
}
static KcValue f_top_sentences(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    G_POS_V = pos; need_args(n, 1, 2, "top_sentences"); int len; const char *s = A_str(a, n, 0, "top_sentences", &len); int64_t k = A_int(a, n, 1, "top_sentences", 3, true);
    Span *sp; double *sc; int c = rank_sents(s, len, &sp, &sc); KcValue arr = kc_array_new();
    if (c) { Scored *r = kc_malloc(sizeof(Scored) * c); for (int i = 0; i < c; i++) { r[i].score = sc[i]; r[i].idx = i; } qsort(r, c, sizeof(Scored), cmp_scored_desc);
             for (int i = 0; i < c && i < k; i++) kc_array_push(arr, pair(vstr(s + sp[r[i].idx].a, sp[r[i].idx].b - sp[r[i].idx].a), kc_float(r[i].score))); free(r); }
    free(sp); free(sc); return arr;
}
static KcValue f_count_words(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    G_POS_V = pos; need_args(n, 1, 1, "count_words"); int len; const char *s = A_str(a, n, 0, "count_words", &len); return kc_int(tokenize_value(s, len, "").as.arr->count);
}

/* ============================================================ module table */

static KcModuleEntry ENTRIES[] = {
    /* cleanup */
    {"lower", f_lower}, {"upper", f_upper}, {"title", f_title}, {"capitalize", f_capitalize}, {"strip_accents", f_strip_accents},
    {"strip_punct", f_strip_punct}, {"strip_html", f_strip_html}, {"strip_emoji", f_strip_emoji}, {"collapse_ws", f_collapse_ws},
    {"normalize", f_normalize}, {"slugify", f_slugify}, {"expand_contractions", f_expand_contractions},
    /* segmentation */
    {"tokenize", f_tokenize}, {"count_words", f_count_words}, {"sentences", f_sentences}, {"ngrams", f_ngrams}, {"char_ngrams", f_char_ngrams}, {"chunk", f_chunk},
    /* linguistics */
    {"stopwords", f_stopwords}, {"is_stopword", f_is_stopword}, {"remove_stopwords", f_remove_stopwords}, {"stem", f_stem}, {"stem_all", f_stem_all},
    {"syllables", f_syllables}, {"readability", f_readability}, {"soundex", f_soundex},
    /* similarity */
    {"levenshtein", f_levenshtein}, {"damerau", f_damerau}, {"hamming", f_hamming}, {"jaro", f_jaro}, {"jaro_winkler", f_jaro_winkler},
    {"lcs", f_lcs}, {"ratio", f_ratio}, {"dice", f_dice}, {"jaccard", f_jaccard}, {"cosine", f_cosine}, {"fuzzy", f_fuzzy}, {"closest", f_closest},
    {"simhash", f_simhash}, {"hamming_bits", f_hamming_bits},
    /* statistics */
    {"freq", f_freq}, {"vocab", f_vocab}, {"bow", f_bow}, {"collocations", f_collocations}, {"stats", f_stats},
    /* models */
    {"tfidf_fit", f_tfidf_fit}, {"tfidf_vector", f_tfidf_vector}, {"tfidf_sparse", f_tfidf_sparse}, {"tfidf_top", f_tfidf_top}, {"tfidf_vocab", f_tfidf_vocab}, {"tfidf_size", f_tfidf_size},
    {"bm25_new", f_bm25_new}, {"bm25_search", f_bm25_search}, {"bm25_score", f_bm25_score}, {"bm25_size", f_bm25_size},
    {"nb_new", f_nb_new}, {"nb_train", f_nb_train}, {"nb_train_many", f_nb_train_many}, {"nb_predict", f_nb_predict}, {"nb_scores", f_nb_scores}, {"nb_labels", f_nb_labels},
    {"spell_new", f_spell_new}, {"spell_add", f_spell_add}, {"spell_known", f_spell_known}, {"spell_correct", f_spell_correct}, {"spell_suggest", f_spell_suggest}, {"spell_size", f_spell_size},
    /* analysis */
    {"sentiment", f_sentiment}, {"sentiment_label", f_sentiment_label}, {"detect_lang", f_detect_lang}, {"detect_script", f_detect_script},
    {"keywords", f_keywords}, {"summarize", f_summarize}, {"top_sentences", f_top_sentences},
    /* extraction */
    {"emails", f_emails}, {"urls", f_urls}, {"hashtags", f_hashtags}, {"mentions", f_mentions}, {"numbers", f_numbers},
};

KcModule *kc_mod_nlp(void) {
    static KcModule m = { "nlp", ENTRIES, sizeof(ENTRIES) / sizeof(ENTRIES[0]) };
    return &m;
}
