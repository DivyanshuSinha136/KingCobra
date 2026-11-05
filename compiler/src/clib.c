/* The C and B standard libraries (v0.8).
 *
 * Familiar names with the familiar behaviour, adapted to KingCobra's values:
 *   <stdio.h>   printf sprintf eprintf puts putchar getchar fflush            (printf: full %-spec language)
 *   <string.h>  strlen strcmp strncmp strcasecmp strchr strrchr strstr strcat strdup strtol strtod atoi atof
 *   <ctype.h>   isdigit isalpha isalnum isspace isupper islower ispunct isxdigit isprint iscntrl isgraph toupper tolower
 *   <stdlib.h>  exit abort rand srand qsort bsearch itoa time
 *   <math.h>    floor ceil round trunc fabs pow fmod hypot atan2 sin cos tan asin acos atan exp log log2 log10 cbrt
 *   constants   NULL EOF EXIT_SUCCESS EXIT_FAILURE RAND_MAX INT_MAX INT_MIN INT64_MAX INT64_MIN UINT32_MAX CHAR_BIT M_PI M_E
 *   B           char lchar getvec rlsevec printn putnumb
 *
 * Strings are values here, not char arrays: functions that C would run in place (strcat, strcpy) return
 * a NEW string instead, and "index" results are ints (-1 = not found) instead of pointers. */
#include "interp.h"
#include "kc_platform.h"
#include "ptr.h"
#include "mem.h"
#include "lib.h"
#include <math.h>
#include <ctype.h>
#include <time.h>

/* ---------------------------------------------------------------- helpers */

static int64_t want_int(KcValue v, KcPos pos, const char *fn, int argno) {
    if (v.type == VAL_INT) return v.as.i;
    if (v.type == VAL_BOOL) return v.as.b ? 1 : 0;
    if (v.type == VAL_FLOAT) return kc_f2i(v.as.f);
    kc_error_at(pos, "%s(): argument %d must be a number, got %s", fn, argno, kc_type_name(v));
    return 0;
}
static double want_dbl(KcValue v, KcPos pos, const char *fn, int argno) {
    if (v.type == VAL_FLOAT) return v.as.f;
    if (v.type == VAL_INT) return (double)v.as.i;
    if (v.type == VAL_BOOL) return v.as.b ? 1.0 : 0.0;
    kc_error_at(pos, "%s(): argument %d must be a number, got %s", fn, argno, kc_type_name(v));
    return 0;
}
static KcString *want_str(KcValue v, KcPos pos, const char *fn, int argno) {
    if (v.type != VAL_STRING) kc_error_at(pos, "%s(): argument %d must be a string, got %s", fn, argno, kc_type_name(v));
    if (v.as.s->flags & KC_STR_FREED) kc_error_freed(v, pos);
    return v.as.s;
}
static void arity(int n, int lo, int hi, const char *usage, KcPos pos) {
    if (n < lo || n > hi) kc_error_at(pos, "usage: %s", usage);
}

/* growable byte buffer */
typedef struct { char *p; size_t len, cap; } Sb;
static void sb_put(Sb *b, const char *s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        b->cap = (b->len + n + 1) * 2 + 32;
        b->p = kc_realloc(b->p, b->cap);
    }
    memcpy(b->p + b->len, s, n); b->len += n; b->p[b->len] = 0;
}
static void sb_putc(Sb *b, char c) { sb_put(b, &c, 1); }
static void sb_pad(Sb *b, char c, int n) { while (n-- > 0) sb_putc(b, c); }

static size_t utf8_encode(unsigned long cp, char out[4]) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) { out[0] = (char)(0xC0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) { out[0] = (char)(0xE0 | (cp >> 12)); out[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[2] = (char)(0x80 | (cp & 0x3F)); return 3; }
    out[0] = (char)(0xF0 | (cp >> 18)); out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[3] = (char)(0x80 | (cp & 0x3F)); return 4;
}

/* ---------------------------------------------------------------- printf */

static void put_native(Sb *b, const char *spec, int kind, long long iv, double dv) {
    int need = kind == 0 ? snprintf(NULL, 0, spec, iv) : snprintf(NULL, 0, spec, dv);
    if (need < 0) return;
    char *tmp = kc_malloc((size_t)need + 1);
    if (kind == 0) snprintf(tmp, (size_t)need + 1, spec, iv); else snprintf(tmp, (size_t)need + 1, spec, dv);
    sb_put(b, tmp, (size_t)need);
    free(tmp);
}

static void put_padded(Sb *b, const char *s, size_t n, int width, bool left) {
    int pad = width > (int)n ? width - (int)n : 0;
    if (!left) sb_pad(b, ' ', pad);
    sb_put(b, s, n);
    if (left) sb_pad(b, ' ', pad);
}

/* Formats `fmt` with `args` into a fresh string. Conversions: %d %i %u %x %X %o %b %B %c %s %f %F %e %E %g %G %a %A %p %%
 * with flags - + space # 0, width, .precision and `*` for either; length modifiers (l ll h z j t) are accepted
 * and ignored (every integer is 64-bit). %b/%B print binary (`%#b` adds 0b). */
char *kc_format_values(const char *fmt, size_t flen, KcValue *args, int nargs, KcPos pos, const char *fname, size_t *outlen) {
    Sb b = {0};
    int ai = 0;
    #define NEXT() (ai < nargs ? args[ai++] : (kc_error_at(pos, "%s(): the format asks for more arguments than were given (%d)", fname, nargs), kc_null()))
    #define NEXT_INT() ({ KcValue v_ = NEXT(); want_int(v_, pos, fname, ai); })
    #define NEXT_DBL() ({ KcValue v_ = NEXT(); want_dbl(v_, pos, fname, ai); })
    for (size_t i = 0; i < flen; ) {
        if (fmt[i] != '%') { sb_putc(&b, fmt[i++]); continue; }
        if (++i >= flen) kc_error_at(pos, "%s(): format string ends with a lone '%%'", fname);
        if (fmt[i] == '%') { sb_putc(&b, '%'); i++; continue; }
        char flags[8]; int nf = 0;
        while (i < flen && strchr("-+ #0", fmt[i])) { if (nf < 7 && !memchr(flags, fmt[i], (size_t)nf)) flags[nf++] = fmt[i]; i++; }
        flags[nf] = 0;
        bool left = strchr(flags, '-') != NULL, zero = strchr(flags, '0') != NULL, alt = strchr(flags, '#') != NULL;
        int width = -1, prec = -1;
        if (i < flen && fmt[i] == '*') { width = (int)NEXT_INT(); i++; if (width < 0) { left = true; width = -width; if (nf < 7) flags[nf++] = '-', flags[nf] = 0; } }
        else if (i < flen && isdigit((unsigned char)fmt[i])) { width = 0; while (i < flen && isdigit((unsigned char)fmt[i])) { width = width * 10 + (fmt[i++] - '0'); if (width > 100000) kc_error_at(pos, "%s(): field width too large", fname); } }
        if (i < flen && fmt[i] == '.') {
            i++; prec = 0;
            if (i < flen && fmt[i] == '*') { prec = (int)NEXT_INT(); i++; if (prec < 0) prec = -1; }
            else while (i < flen && isdigit((unsigned char)fmt[i])) { prec = prec * 10 + (fmt[i++] - '0'); if (prec > 100000) kc_error_at(pos, "%s(): precision too large", fname); }
        }
        while (i < flen && strchr("hlLqjzt", fmt[i])) i++;
        if (i >= flen) kc_error_at(pos, "%s(): incomplete format conversion", fname);
        char conv = fmt[i++];
        char spec[96];
        int sl = snprintf(spec, sizeof spec, "%%%s", flags);
        if (width >= 0) sl += snprintf(spec + sl, sizeof spec - (size_t)sl, "%d", width);
        if (prec >= 0) sl += snprintf(spec + sl, sizeof spec - (size_t)sl, ".%d", prec);
        switch (conv) {
            case 'd': case 'i': {
                long long v = (long long)NEXT_INT();
                snprintf(spec + sl, sizeof spec - (size_t)sl, "lld");
                put_native(&b, spec, 0, v, 0);
                break;
            }
            case 'u': case 'x': case 'X': case 'o': {
                long long v = (long long)NEXT_INT();
                snprintf(spec + sl, sizeof spec - (size_t)sl, "ll%c", conv);
                put_native(&b, spec, 0, v, 0);
                break;
            }
            case 'b': case 'B': {
                unsigned long long v = (unsigned long long)NEXT_INT();
                char dig[72]; int nd = 0;
                if (v == 0) dig[nd++] = '0';
                while (v) { dig[nd++] = (char)('0' + (v & 1)); v >>= 1; }
                int zeros = prec > nd ? prec - nd : 0;
                int total = nd + zeros + (alt ? 2 : 0);
                int pad = width > total ? width - total : 0;
                if (!left && !(zero && prec < 0)) sb_pad(&b, ' ', pad);
                if (alt) { sb_putc(&b, '0'); sb_putc(&b, conv); }
                if (!left && zero && prec < 0) sb_pad(&b, '0', pad);
                sb_pad(&b, '0', zeros);
                while (nd) sb_putc(&b, dig[--nd]);
                if (left) sb_pad(&b, ' ', pad);
                break;
            }
            case 'c': {
                long long v = (long long)NEXT_INT();
                if (v < 0 || v > 0x10FFFF) kc_error_at(pos, "%s(): %%c needs a character code between 0 and 1114111, got %lld", fname, v);
                char u[4]; size_t n = utf8_encode((unsigned long)v, u);
                put_padded(&b, u, n, width, left);
                break;
            }
            case 's': {
                KcValue v = NEXT();
                char *owned = NULL; const char *s; size_t n;
                if (v.type == VAL_STRING) { if (v.as.s->flags & KC_STR_FREED) kc_error_freed(v, pos); s = v.as.s->data; n = (size_t)v.as.s->len; }
                else { owned = kc_to_display_string(v); s = owned; n = strlen(owned); }
                if (prec >= 0 && (size_t)prec < n) n = (size_t)prec;
                put_padded(&b, s, n, width, left);
                free(owned);
                break;
            }
            case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
                double v = NEXT_DBL();
                snprintf(spec + sl, sizeof spec - (size_t)sl, "%c", conv);
                put_native(&b, spec, 1, 0, v);
                break;
            }
            case 'p': {
                KcValue v = NEXT();
                char *s = kc_to_display_string(v);
                put_padded(&b, s, strlen(s), width, left);
                free(s);
                break;
            }
            default:
                kc_error_at(pos, "%s(): unknown conversion '%%%c' (supported: d i u x X o b c s f e g a p %%)", fname, conv);
        }
    }
    #undef NEXT
    #undef NEXT_INT
    #undef NEXT_DBL
    if (!b.p) { b.p = kc_malloc(1); b.p[0] = 0; }
    *outlen = b.len;
    return b.p;
}

static KcValue b_sprintf(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    arity(n, 1, 1 << 20, "sprintf(format, ...) -> string", pos);
    KcString *f = want_str(a[0], pos, "sprintf", 1);
    size_t len; char *s = kc_format_values(f->data, (size_t)f->len, a + 1, n - 1, pos, "sprintf", &len);
    return kc_string_adopt(s, (int)len);
}
static KcValue b_printf(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    kc_forbid_in_comptime(ip, pos, "printf");
    arity(n, 1, 1 << 20, "printf(format, ...) -> bytes written", pos);
    KcString *f = want_str(a[0], pos, "printf", 1);
    size_t len; char *s = kc_format_values(f->data, (size_t)f->len, a + 1, n - 1, pos, "printf", &len);
    fwrite(s, 1, len, stdout);
    free(s);
    return kc_int((int64_t)len);
}
static KcValue b_eprintf(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    kc_forbid_in_comptime(ip, pos, "eprintf");
    arity(n, 1, 1 << 20, "eprintf(format, ...) -> bytes written", pos);
    KcString *f = want_str(a[0], pos, "eprintf", 1);
    size_t len; char *s = kc_format_values(f->data, (size_t)f->len, a + 1, n - 1, pos, "eprintf", &len);
    fflush(stdout); fwrite(s, 1, len, stderr);
    free(s);
    return kc_int((int64_t)len);
}
static KcValue b_puts(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    kc_forbid_in_comptime(ip, pos, "puts");
    arity(n, 1, 1, "puts(string)", pos);
    KcString *s = want_str(a[0], pos, "puts", 1);
    fwrite(s->data, 1, (size_t)s->len, stdout); fputc('\n', stdout);
    return kc_int(s->len + 1);
}
static KcValue b_putchar(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    kc_forbid_in_comptime(ip, pos, "putchar");
    arity(n, 1, 1, "putchar(code) -> code", pos);
    int64_t c = want_int(a[0], pos, "putchar", 1);
    if (c < 0 || c > 0x10FFFF) kc_error_at(pos, "putchar(): character code out of range: %lld", (long long)c);
    if (c < 256 && c >= 128) fputc((int)c, stdout);          /* a raw byte, as in C and B (use a code point > 255 for UTF-8) */
    else { char u[4]; size_t k = utf8_encode((unsigned long)c, u); fwrite(u, 1, k, stdout); }
    return kc_int(c);
}
static KcValue b_getchar(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)a; (void)n;
    kc_forbid_in_comptime(ip, pos, "getchar");
    return kc_int(getchar());                                 /* a byte, or -1 (EOF) */
}
static KcValue b_fflush(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; (void)a; (void)n; (void)pos;
    fflush(stdout); return kc_int(0);
}

/* B: printn(n, base) prints n in the given base (2..36); putnumb(n) prints it in decimal. */
static void put_number(int64_t v, int base) {
    char tmp[72]; int k = 0;
    uint64_t u = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    do { int d = (int)(u % (uint64_t)base); tmp[k++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); u /= (uint64_t)base; } while (u);
    if (v < 0) fputc('-', stdout);
    while (k) fputc(tmp[--k], stdout);
}
static KcValue b_printn(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    kc_forbid_in_comptime(ip, pos, "printn");
    arity(n, 1, 2, "printn(number [, base])", pos);
    int64_t base = n == 2 ? want_int(a[1], pos, "printn", 2) : 10;
    if (base < 2 || base > 36) kc_error_at(pos, "printn(): base must be between 2 and 36, got %lld", (long long)base);
    put_number(want_int(a[0], pos, "printn", 1), (int)base);
    return kc_null();
}
static KcValue b_putnumb(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    kc_forbid_in_comptime(ip, pos, "putnumb");
    arity(n, 1, 1, "putnumb(number)", pos);
    put_number(want_int(a[0], pos, "putnumb", 1), 10);
    return kc_null();
}

/* ---------------------------------------------------------------- strings */

static KcValue b_strlen(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; arity(n, 1, 1, "strlen(string) -> int", pos);
    return kc_int(want_str(a[0], pos, "strlen", 1)->len);
}
static int sgn(int c) { return c < 0 ? -1 : c > 0 ? 1 : 0; }
static KcValue b_strcmp(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; arity(n, 2, 2, "strcmp(a, b) -> -1 | 0 | 1", pos);
    KcString *x = want_str(a[0], pos, "strcmp", 1), *y = want_str(a[1], pos, "strcmp", 2);
    int l = x->len < y->len ? x->len : y->len;
    int c = memcmp(x->data, y->data, (size_t)l);
    return kc_int(c ? sgn(c) : (x->len < y->len ? -1 : x->len > y->len ? 1 : 0));
}
static KcValue b_strncmp(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; arity(n, 3, 3, "strncmp(a, b, n) -> -1 | 0 | 1", pos);
    KcString *x = want_str(a[0], pos, "strncmp", 1), *y = want_str(a[1], pos, "strncmp", 2);
    int64_t k = want_int(a[2], pos, "strncmp", 3);
    if (k < 0) k = 0;
    int64_t lx = x->len < k ? x->len : k, ly = y->len < k ? y->len : k;
    int64_t l = lx < ly ? lx : ly;
    int c = memcmp(x->data, y->data, (size_t)l);
    return kc_int(c ? sgn(c) : (lx < ly ? -1 : lx > ly ? 1 : 0));
}
static KcValue b_strcasecmp(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; arity(n, 2, 2, "strcasecmp(a, b) -> -1 | 0 | 1", pos);
    KcString *x = want_str(a[0], pos, "strcasecmp", 1), *y = want_str(a[1], pos, "strcasecmp", 2);
    int i = 0;
    for (; i < x->len && i < y->len; i++) {
        int cx = tolower((unsigned char)x->data[i]), cy = tolower((unsigned char)y->data[i]);
        if (cx != cy) return kc_int(cx < cy ? -1 : 1);
    }
    return kc_int(x->len == y->len ? 0 : x->len < y->len ? -1 : 1);
}
static int char_arg(KcValue v, KcPos pos, const char *fn, int argno) {
    if (v.type == VAL_STRING && v.as.s->len == 1) return (unsigned char)v.as.s->data[0];
    return (int)want_int(v, pos, fn, argno) & 0xFF;
}
static KcValue b_strchr(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; arity(n, 2, 2, "strchr(string, char) -> index or -1", pos);
    KcString *s = want_str(a[0], pos, "strchr", 1); int c = char_arg(a[1], pos, "strchr", 2);
    for (int i = 0; i < s->len; i++) if ((unsigned char)s->data[i] == c) return kc_int(i);
    return kc_int(-1);
}
static KcValue b_strrchr(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; arity(n, 2, 2, "strrchr(string, char) -> index or -1", pos);
    KcString *s = want_str(a[0], pos, "strrchr", 1); int c = char_arg(a[1], pos, "strrchr", 2);
    for (int i = s->len - 1; i >= 0; i--) if ((unsigned char)s->data[i] == c) return kc_int(i);
    return kc_int(-1);
}
static KcValue b_strstr(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; arity(n, 2, 2, "strstr(haystack, needle) -> index or -1", pos);
    KcString *h = want_str(a[0], pos, "strstr", 1), *nd = want_str(a[1], pos, "strstr", 2);
    if (nd->len == 0) return kc_int(0);
    for (int i = 0; i + nd->len <= h->len; i++) if (memcmp(h->data + i, nd->data, (size_t)nd->len) == 0) return kc_int(i);
    return kc_int(-1);
}
static KcValue b_strcat(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; arity(n, 2, 2, "strcat(a, b) -> a new string (strings are values; nothing is modified)", pos);
    KcString *x = want_str(a[0], pos, "strcat", 1), *y = want_str(a[1], pos, "strcat", 2);
    char *d; KcValue r = kc_string_alloc(x->len + y->len, &d);
    memcpy(d, x->data, (size_t)x->len); memcpy(d + x->len, y->data, (size_t)y->len);
    return r;
}
static KcValue b_strdup(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; arity(n, 1, 1, "strdup(string) -> a mutable copy (for lchar)", pos);
    KcString *x = want_str(a[0], pos, "strdup", 1);
    char *d; KcValue r = kc_string_alloc(x->len, &d);
    memcpy(d, x->data, (size_t)x->len);
    return r;
}
static KcValue b_atoi(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; arity(n, 1, 1, "atoi(string) -> int", pos);
    return kc_int(strtoll(want_str(a[0], pos, "atoi", 1)->data, NULL, 10));
}
static KcValue b_atof(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; arity(n, 1, 1, "atof(string) -> float", pos);
    return kc_float(strtod(want_str(a[0], pos, "atof", 1)->data, NULL));
}
static KcValue b_strtol(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; arity(n, 1, 2, "strtol(string [, base]) -> int  (base 0 detects 0x / 0b-less prefixes)", pos);
    int64_t base = n == 2 ? want_int(a[1], pos, "strtol", 2) : 10;
    if (base != 0 && (base < 2 || base > 36)) kc_error_at(pos, "strtol(): base must be 0 or 2..36, got %lld", (long long)base);
    return kc_int(strtoll(want_str(a[0], pos, "strtol", 1)->data, NULL, (int)base));
}
static KcValue b_strtod(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; arity(n, 1, 1, "strtod(string) -> float", pos);
    return kc_float(strtod(want_str(a[0], pos, "strtod", 1)->data, NULL));
}
static KcValue b_itoa(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; arity(n, 1, 2, "itoa(number [, base]) -> string", pos);
    int64_t base = n == 2 ? want_int(a[1], pos, "itoa", 2) : 10;
    if (base < 2 || base > 36) kc_error_at(pos, "itoa(): base must be between 2 and 36, got %lld", (long long)base);
    int64_t v = want_int(a[0], pos, "itoa", 1);
    char tmp[72]; int k = 0;
    uint64_t u = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    do { int d = (int)(u % (uint64_t)base); tmp[k++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); u /= (uint64_t)base; } while (u);
    if (v < 0) tmp[k++] = '-';
    char *d; KcValue r = kc_string_alloc(k, &d);
    for (int i = 0; i < k; i++) d[i] = tmp[k - 1 - i];
    return r;
}

/* ---------------------------------------------------------------- ctype */

#define CTYPE(NAME, TEST) \
    static KcValue b_##NAME(KcInterp *ip, KcValue *a, int n, KcPos pos) { \
        (void)ip; arity(n, 1, 1, #NAME "(char) -> bool   (a code or a one-character string)", pos); \
        int64_t c = a[0].type == VAL_STRING && a[0].as.s->len == 1 ? (unsigned char)a[0].as.s->data[0] : want_int(a[0], pos, #NAME, 1); \
        return kc_bool(c >= 0 && c < 128 && TEST((int)c)); \
    }
CTYPE(isdigit, isdigit) CTYPE(isalpha, isalpha) CTYPE(isalnum, isalnum) CTYPE(isspace, isspace)
CTYPE(isupper, isupper) CTYPE(islower, islower) CTYPE(ispunct, ispunct) CTYPE(isxdigit, isxdigit)
CTYPE(isprint, isprint) CTYPE(iscntrl, iscntrl) CTYPE(isgraph, isgraph)

static KcValue b_toupper(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; arity(n, 1, 1, "toupper(code | string)", pos);
    if (a[0].type == VAL_STRING) {
        KcString *s = want_str(a[0], pos, "toupper", 1); char *d; KcValue r = kc_string_alloc(s->len, &d);
        for (int i = 0; i < s->len; i++) d[i] = (char)toupper((unsigned char)s->data[i]);
        return r;
    }
    int64_t c = want_int(a[0], pos, "toupper", 1);
    return kc_int(c >= 0 && c < 128 ? toupper((int)c) : c);
}
static KcValue b_tolower(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; arity(n, 1, 1, "tolower(code | string)", pos);
    if (a[0].type == VAL_STRING) {
        KcString *s = want_str(a[0], pos, "tolower", 1); char *d; KcValue r = kc_string_alloc(s->len, &d);
        for (int i = 0; i < s->len; i++) d[i] = (char)tolower((unsigned char)s->data[i]);
        return r;
    }
    int64_t c = want_int(a[0], pos, "tolower", 1);
    return kc_int(c >= 0 && c < 128 ? tolower((int)c) : c);
}

/* ---------------------------------------------------------------- stdlib */

static KcValue b_exit(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    kc_forbid_in_comptime(ip, pos, "exit");
    arity(n, 0, 1, "exit([status])", pos);
    fflush(stdout); fflush(stderr);
    exit(n ? (int)want_int(a[0], pos, "exit", 1) : 0);
    return kc_null();
}
static KcValue b_abort(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)a; (void)n;
    kc_forbid_in_comptime(ip, pos, "abort");
    fflush(stdout); fflush(stderr);
    abort();
    return kc_null();
}

static uint64_t g_rand = 0x853c49e6748fea9bULL;
static KcValue b_srand(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    kc_forbid_in_comptime(ip, pos, "srand");
    arity(n, 1, 1, "srand(seed)", pos);
    g_rand = (uint64_t)want_int(a[0], pos, "srand", 1) * 6364136223846793005ULL + 1442695040888963407ULL;
    if (!g_rand) g_rand = 1;
    return kc_null();
}
static KcValue b_rand(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)a; (void)n;
    kc_forbid_in_comptime(ip, pos, "rand");
    g_rand ^= g_rand >> 12; g_rand ^= g_rand << 25; g_rand ^= g_rand >> 27;          /* xorshift64*: deterministic until srand() */
    return kc_int((int64_t)(((g_rand * 2685821657736338717ULL) >> 33) & 0x7FFFFFFF));
}
static KcValue b_time(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)a; (void)n;
    kc_forbid_in_comptime(ip, pos, "time");
    return kc_int((int64_t)time(NULL));
}

/* qsort(array, cmp) sorts IN PLACE, stably; cmp(a, b) returns <0, 0 or >0. bsearch(array, key, cmp) -> index or -1. */
static int cmp_result(KcValue r, KcPos pos, const char *fn) {
    if (r.type == VAL_INT) return r.as.i < 0 ? -1 : r.as.i > 0 ? 1 : 0;
    if (r.type == VAL_FLOAT) return r.as.f < 0 ? -1 : r.as.f > 0 ? 1 : 0;
    if (r.type == VAL_BOOL) return r.as.b ? 1 : 0;
    kc_error_at(pos, "%s(): the comparator must return a number (negative, zero or positive), got %s", fn, kc_type_name(r));
    return 0;
}
static void msort(KcInterp *ip, KcValue cmp, KcValue *v, KcValue *tmp, int lo, int hi, KcPos pos) {
    if (hi - lo < 2) return;
    int mid = lo + (hi - lo) / 2;
    msort(ip, cmp, v, tmp, lo, mid, pos); msort(ip, cmp, v, tmp, mid, hi, pos);
    int i = lo, j = mid, k = lo;
    while (i < mid && j < hi) {
        KcValue args[2] = { v[j], v[i] };                       /* take the right element only if it is strictly smaller: stable */
        if (cmp_result(kc_call_value(ip, cmp, args, 2, pos), pos, "qsort") < 0) tmp[k++] = v[j++]; else tmp[k++] = v[i++];
    }
    while (i < mid) tmp[k++] = v[i++];
    while (j < hi) tmp[k++] = v[j++];
    memcpy(v + lo, tmp + lo, sizeof(KcValue) * (size_t)(hi - lo));
}
static KcValue b_qsort(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    arity(n, 2, 2, "qsort(array, compare(a, b))", pos);
    if (a[0].type != VAL_ARRAY) kc_error_at(pos, "qsort(): argument 1 must be an array, got %s", kc_type_name(a[0]));
    if (a[0].as.arr->flags & KC_OBJ_FREED) kc_error_freed(a[0], pos);
    int cnt = a[0].as.arr->count;
    if (cnt < 2) return kc_null();
    KcValue *copy = kc_malloc(sizeof(KcValue) * (size_t)cnt), *tmp = kc_malloc(sizeof(KcValue) * (size_t)cnt);
    memcpy(copy, a[0].as.arr->items, sizeof(KcValue) * (size_t)cnt);
    msort(ip, a[1], copy, tmp, 0, cnt, pos);                    /* sorts a private copy: a comparator that raises leaves the array intact */
    if (a[0].as.arr->count == cnt) memcpy(a[0].as.arr->items, copy, sizeof(KcValue) * (size_t)cnt);
    free(copy); free(tmp);
    return kc_null();
}
static KcValue b_bsearch(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    arity(n, 3, 3, "bsearch(sorted_array, key, compare(key, element)) -> index or -1", pos);
    if (a[0].type != VAL_ARRAY) kc_error_at(pos, "bsearch(): argument 1 must be an array, got %s", kc_type_name(a[0]));
    int lo = 0, hi = a[0].as.arr->count - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        KcValue args[2] = { a[1], a[0].as.arr->items[mid] };
        int c = cmp_result(kc_call_value(ip, a[2], args, 2, pos), pos, "bsearch");
        if (c == 0) return kc_int(mid);
        if (c < 0) hi = mid - 1; else lo = mid + 1;
    }
    return kc_int(-1);
}

/* ---------------------------------------------------------------- math */

#define MATH1(NAME, EXPR) \
    static KcValue b_##NAME(KcInterp *ip, KcValue *a, int n, KcPos pos) { \
        (void)ip; arity(n, 1, 1, #NAME "(x) -> float", pos); \
        double x = want_dbl(a[0], pos, #NAME, 1); return kc_float(EXPR); \
    }
#define MATH2(NAME, EXPR) \
    static KcValue b_##NAME(KcInterp *ip, KcValue *a, int n, KcPos pos) { \
        (void)ip; arity(n, 2, 2, #NAME "(x, y) -> float", pos); \
        double x = want_dbl(a[0], pos, #NAME, 1), y = want_dbl(a[1], pos, #NAME, 2); return kc_float(EXPR); \
    }
MATH1(floor, floor(x)) MATH1(ceil, ceil(x)) MATH1(round, round(x)) MATH1(trunc, trunc(x)) MATH1(fabs, fabs(x))
MATH1(sin, sin(x)) MATH1(cos, cos(x)) MATH1(tan, tan(x)) MATH1(asin, asin(x)) MATH1(acos, acos(x)) MATH1(atan, atan(x))
MATH1(exp, exp(x)) MATH1(log, log(x)) MATH1(log2, log2(x)) MATH1(log10, log10(x)) MATH1(cbrt, cbrt(x))
MATH2(pow, pow(x, y)) MATH2(fmod, fmod(x, y)) MATH2(hypot, hypot(x, y)) MATH2(atan2, atan2(x, y))

/* ---------------------------------------------------------------- B: words, vectors, chars */

/* char(x, i): the i-th byte of a string or buffer (or the i-th word of a vector).
 * lchar(x, i, c): store byte c there. Strings made by strdup()/concatenation are writable; literals are not. */
static KcValue b_char(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    arity(n, 2, 2, "char(string | buffer | vector, i) -> byte", pos);
    int64_t i = want_int(a[1], pos, "char", 2);
    if (a[0].type == VAL_STRING) {
        KcString *s = want_str(a[0], pos, "char", 1);
        if (i < 0 || i >= s->len) kc_error_at(pos, "char(): index %lld out of bounds (string length %d)", (long long)i, s->len);
        return kc_int((unsigned char)s->data[i]);
    }
    if (a[0].type == VAL_BUFFER) return kc_ptr_load(ip, kc_buffer_elem(ip, a[0], kc_int(i), pos), pos);
    if (a[0].type == VAL_ARRAY) {
        if (a[0].as.arr->flags & KC_OBJ_FREED) kc_error_freed(a[0], pos);
        if (i < 0 || i >= a[0].as.arr->count) kc_error_at(pos, "char(): index %lld out of bounds (length %d)", (long long)i, a[0].as.arr->count);
        return a[0].as.arr->items[i];
    }
    kc_error_at(pos, "char(): argument 1 must be a string, buffer or vector, got %s", kc_type_name(a[0]));
    return kc_null();
}
static KcValue b_lchar(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    arity(n, 3, 3, "lchar(string | buffer | vector, i, byte) -> byte", pos);
    int64_t i = want_int(a[1], pos, "lchar", 2), c = want_int(a[2], pos, "lchar", 3);
    if (a[0].type == VAL_STRING) {
        KcString *s = want_str(a[0], pos, "lchar", 1);
        if (s->flags & KC_STR_IMMORTAL) kc_error_at(pos, "lchar(): a string literal is read-only (make a writable copy with strdup())");
        if (i < 0 || i >= s->len) kc_error_at(pos, "lchar(): index %lld out of bounds (string length %d)", (long long)i, s->len);
        s->data[i] = (char)c;
        return kc_int(c & 0xFF);
    }
    if (a[0].type == VAL_BUFFER) { kc_ptr_store(ip, kc_buffer_elem(ip, a[0], kc_int(i), pos), kc_int(c & 0xFF), pos); return kc_int(c & 0xFF); }
    if (a[0].type == VAL_ARRAY) {
        if (a[0].as.arr->flags & KC_OBJ_FREED) kc_error_freed(a[0], pos);
        if (i < 0 || i >= a[0].as.arr->count) kc_error_at(pos, "lchar(): index %lld out of bounds (length %d)", (long long)i, a[0].as.arr->count);
        a[0].as.arr->items[i] = kc_int(c);
        return kc_int(c);
    }
    kc_error_at(pos, "lchar(): argument 1 must be a string, buffer or vector, got %s", kc_type_name(a[0]));
    return kc_null();
}
/* getvec(n): a vector of n+1 zero words (indices 0..n), as in B. `auto v[n];` makes the same thing. */
static KcValue b_getvec(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; arity(n, 1, 1, "getvec(n) -> a vector of n+1 words (indices 0..n)", pos);
    int64_t cnt = want_int(a[0], pos, "getvec", 1);
    if (cnt < 0 || cnt >= ((int64_t)1 << 28)) kc_error_at(pos, "getvec(): size %lld is out of range", (long long)cnt);
    KcValue arr = kc_array_new();
    for (int64_t i = 0; i <= cnt; i++) kc_array_push(arr, kc_int(0));
    return arr;
}
static KcValue b_rlsevec(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    arity(n, 1, 1, "rlsevec(vector)", pos);
    KcValue freev;
    if (!kc_env_get(ip->builtins_env, "free", &freev)) kc_error_at(pos, "rlsevec(): free() is unavailable");
    kc_call_value(ip, freev, a, 1, pos);
    return kc_null();
}

/* ---------------------------------------------------------------- registration */

void kc_register_clib_builtins(KcInterp *ip) {
    static const struct { const char *name; KcNativeFn fn; } fns[] = {
        {"printf", b_printf}, {"sprintf", b_sprintf}, {"format", b_sprintf}, {"eprintf", b_eprintf}, {"puts", b_puts},
        {"putchar", b_putchar}, {"getchar", b_getchar}, {"fflush", b_fflush}, {"printn", b_printn}, {"putnumb", b_putnumb},
        {"strlen", b_strlen}, {"strcmp", b_strcmp}, {"strncmp", b_strncmp}, {"strcasecmp", b_strcasecmp},
        {"strchr", b_strchr}, {"strrchr", b_strrchr}, {"strstr", b_strstr}, {"strcat", b_strcat}, {"strdup", b_strdup},
        {"atoi", b_atoi}, {"atol", b_atoi}, {"atof", b_atof}, {"strtol", b_strtol}, {"strtod", b_strtod}, {"itoa", b_itoa},
        {"isdigit", b_isdigit}, {"isalpha", b_isalpha}, {"isalnum", b_isalnum}, {"isspace", b_isspace},
        {"isupper", b_isupper}, {"islower", b_islower}, {"ispunct", b_ispunct}, {"isxdigit", b_isxdigit},
        {"isprint", b_isprint}, {"iscntrl", b_iscntrl}, {"isgraph", b_isgraph}, {"toupper", b_toupper}, {"tolower", b_tolower},
        {"exit", b_exit}, {"abort", b_abort}, {"rand", b_rand}, {"srand", b_srand}, {"time", b_time},
        {"qsort", b_qsort}, {"bsearch", b_bsearch},
        {"floor", b_floor}, {"ceil", b_ceil}, {"round", b_round}, {"trunc", b_trunc}, {"fabs", b_fabs},
        {"sin", b_sin}, {"cos", b_cos}, {"tan", b_tan}, {"asin", b_asin}, {"acos", b_acos}, {"atan", b_atan},
        {"exp", b_exp}, {"log", b_log}, {"log2", b_log2}, {"log10", b_log10}, {"cbrt", b_cbrt},
        {"pow", b_pow}, {"fmod", b_fmod}, {"hypot", b_hypot}, {"atan2", b_atan2},
        {"char", b_char}, {"lchar", b_lchar}, {"getvec", b_getvec}, {"__vec", b_getvec}, {"rlsevec", b_rlsevec},
    };
    for (size_t i = 0; i < sizeof fns / sizeof fns[0]; i++) {
        KcValue v; v.type = VAL_NATIVE; v.as.native = fns[i].fn;
        kc_env_define(ip->builtins_env, fns[i].name, v, false);
    }
    static const struct { const char *name; int64_t v; } ints[] = {
        {"EOF", -1}, {"EXIT_SUCCESS", 0}, {"EXIT_FAILURE", 1}, {"RAND_MAX", 2147483647}, {"INT_MAX", 2147483647},
        {"INT_MIN", -2147483648LL}, {"INT64_MAX", INT64_MAX}, {"INT64_MIN", INT64_MIN}, {"LLONG_MAX", INT64_MAX},
        {"LLONG_MIN", INT64_MIN}, {"UINT32_MAX", 4294967295LL}, {"CHAR_BIT", 8},
    };
    for (size_t i = 0; i < sizeof ints / sizeof ints[0]; i++) kc_env_define(ip->builtins_env, ints[i].name, kc_int(ints[i].v), false);
    kc_env_define(ip->builtins_env, "NULL", kc_null(), false);
    kc_env_define(ip->builtins_env, "M_PI", kc_float(3.14159265358979323846), false);
    kc_env_define(ip->builtins_env, "M_E", kc_float(2.71828182845904523536), false);
}
