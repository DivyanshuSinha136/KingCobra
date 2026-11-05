/* std.ascii_screen — argument/option parsing, colours, UTF-8, width table, RNG, noise, clock. */
#include "as_internal.h"
#include <time.h>
#include <errno.h>
#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <unistd.h>
#  include <sys/stat.h>
#endif

AsCtx AC;

/* ---------------------------------------------------------------- errors */
void as_err(const char *fmt, ...) {
    char buf[600];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    kc_error_at(AC.pos, "ascii_screen: %s", buf);
    abort();
}
void as_warn(const char *fmt, ...) {
    char buf[600];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    fprintf(stderr, "ascii_screen: warning: %s\n", buf);
}

/* ------------------------------------------------------------- arguments */
void A_argc(int n, int lo, int hi, const char *sig) {
    if (n < lo || n > hi) as_err("usage: ascii_screen.%s", sig);
}
bool V_isnum(KcValue v) { return v.type == VAL_INT || v.type == VAL_FLOAT || v.type == VAL_BOOL; }
double V_num(KcValue v, const char *who) {
    if (v.type == VAL_FLOAT) return v.as.f;
    if (v.type == VAL_INT) return (double)v.as.i;
    if (v.type == VAL_BOOL) return v.as.b ? 1.0 : 0.0;
    as_err("%s: expected a number, got %s", who, kc_type_name(v));
}
double A_num(KcValue *a, int n, int i, const char *who) {
    if (i >= n) as_err("%s: missing argument %d", who, i + 1);
    return V_num(a[i], who);
}
int A_int(KcValue *a, int n, int i, const char *who) { return (int)floor(A_num(a, n, i, who)); }
const char *A_str(KcValue *a, int n, int i, const char *who) {
    if (i >= n) as_err("%s: missing argument %d", who, i + 1);
    if (a[i].type != VAL_STRING) as_err("%s: argument %d must be a string, got %s", who, i + 1, kc_type_name(a[i]));
    return a[i].as.s->data;
}
bool A_bool(KcValue *a, int n, int i, const char *who) {
    if (i >= n) as_err("%s: missing argument %d", who, i + 1);
    return kc_truthy(a[i]);
}
bool as_is_fn(KcValue v) { return v.type == VAL_CLOSURE || v.type == VAL_NATIVE; }

/* ---------------------------------------------------------------- options */
Opts opts_make(KcValue *a, int n, int start) {
    Opts o; o.a = a; o.n = n; o.start = start; o.has_obj = false; o.obj = kc_null();
    if (start >= n) return o;
    if (n - start == 1 && kc_is_object(a[start])) { o.has_obj = true; o.obj = a[start]; return o; }
    if ((n - start) % 2 != 0) as_err("options must be \"name\", value pairs (or one options object)");
    for (int i = start; i < n; i += 2) if (a[i].type != VAL_STRING) as_err("option names must be strings, got %s", kc_type_name(a[i]));
    return o;
}
const KcValue *opt_find(const Opts *o, const char *key) {
    if (!o) return NULL;
    if (o->has_obj) return kc_object_get(o->obj, key, strlen(key));
    for (int i = o->start; i + 1 < o->n; i += 2) if (!strcmp(o->a[i].as.s->data, key)) return &o->a[i + 1];
    return NULL;
}
bool opt_has(const Opts *o, const char *key) { return opt_find(o, key) != NULL; }
double opt_num(const Opts *o, const char *key, double def) { const KcValue *v = opt_find(o, key); return v ? V_num(*v, key) : def; }
int opt_int(const Opts *o, const char *key, int def) { return (int)floor(opt_num(o, key, def)); }
bool opt_bool(const Opts *o, const char *key, bool def) { const KcValue *v = opt_find(o, key); return v ? kc_truthy(*v) : def; }
Col opt_col(const Opts *o, const char *key, Col def) { const KcValue *v = opt_find(o, key); return v ? col_from_value(*v, key) : def; }
const char *opt_str(const Opts *o, const char *key, const char *def) {
    const KcValue *v = opt_find(o, key);
    if (!v) return def;
    if (v->type != VAL_STRING) as_err("option \"%s\" must be a string", key);
    return v->as.s->data;
}

KcValue ret_str(const char *s, size_t len) {
    char *d; KcValue v = kc_string_alloc((int)len, &d);
    if (len) memcpy(d, s, len);
    d[len] = 0;
    return v;
}
KcValue ret_cstr(const char *s) { return ret_str(s, strlen(s)); }

/* ----------------------------------------------------------------- files */
bool as_file_exists(const char *p) {
    FILE *f = fopen(p, "rb");
    if (!f) return false;
    fclose(f); return true;
}
uint8_t *as_read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return NULL; }
    uint8_t *b = kc_malloc((size_t)sz + 1);
    size_t got = fread(b, 1, (size_t)sz, f); fclose(f);
    b[got] = 0; if (len) *len = got;
    return b;
}
bool as_write_file(const char *path, const void *d, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    size_t w = n ? fwrite(d, 1, n, f) : 0;
    return fclose(f) == 0 && w == n;
}

/* ----------------------------------------------------------------- colour */
static const struct { const char *n; uint32_t c; } NAMED[] = {
    {"black", 0x000000}, {"red", 0xE53935}, {"green", 0x43A047}, {"yellow", 0xFDD835}, {"blue", 0x1E88E5}, {"magenta", 0xD81B60},
    {"cyan", 0x00ACC1}, {"white", 0xF5F5F5}, {"gray", 0x9E9E9E}, {"grey", 0x9E9E9E}, {"darkgray", 0x555555}, {"darkgrey", 0x555555},
    {"lightgray", 0xCCCCCC}, {"lightgrey", 0xCCCCCC}, {"orange", 0xFB8C00}, {"purple", 0x8E24AA}, {"brown", 0x6D4C41}, {"pink", 0xF48FB1},
    {"gold", 0xFFC107}, {"sky", 0x87CEEB}, {"lime", 0x9CCC65}, {"teal", 0x00897B}, {"navy", 0x1A237E}, {"maroon", 0x800000},
    {"olive", 0x808000}, {"silver", 0xC0C0C0}, {"violet", 0xEE82EE}, {"indigo", 0x4B0082}, {"coral", 0xFF7F50}, {"salmon", 0xFA8072},
    {"khaki", 0xF0E68C}, {"crimson", 0xDC143C}, {"turquoise", 0x40E0D0}, {"tan", 0xD2B48C}, {"beige", 0xF5F5DC}, {"ivory", 0xFFFFF0},
    {"bright_black", 0x555555}, {"bright_red", 0xFF5252}, {"bright_green", 0x69F0AE}, {"bright_yellow", 0xFFFF00}, {"bright_blue", 0x448AFF},
    {"bright_magenta", 0xFF4081}, {"bright_cyan", 0x18FFFF}, {"bright_white", 0xFFFFFF}, {"dark", 0x14161C}, {"cloud", 0xE8EEF4},
};
static int hexv(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

bool col_parse(const char *s, Col *out) {
    while (*s == ' ') s++;
    if (!strcmp(s, "default") || !strcmp(s, "none") || !strcmp(s, "transparent") || !strcmp(s, "reset") || !*s) { *out = 0; return true; }
    if (s[0] == '#') {
        size_t l = strlen(s + 1); int v[8];
        if (l != 3 && l != 6 && l != 8) return false;
        for (size_t i = 0; i < l; i++) { v[i] = hexv(s[1 + i]); if (v[i] < 0) return false; }
        if (l == 3) *out = col_rgb(v[0] * 17, v[1] * 17, v[2] * 17);
        else *out = col_rgb(v[0] * 16 + v[1], v[2] * 16 + v[3], v[4] * 16 + v[5]);
        return true;
    }
    char low[32]; size_t i = 0;
    for (; s[i] && i < 31; i++) low[i] = (char)tolower((unsigned char)s[i]);
    low[i] = 0;
    for (size_t k = 0; k < sizeof NAMED / sizeof NAMED[0]; k++) if (!strcmp(NAMED[k].n, low)) { *out = COL_SET | NAMED[k].c; return true; }
    return false;
}
Col col_from_value(KcValue v, const char *who) {
    if (v.type == VAL_INT || v.type == VAL_FLOAT) {
        int64_t i = v.type == VAL_INT ? v.as.i : (int64_t)v.as.f;
        if (i < 0) return 0;                                       /* any negative number = unset/transparent */
        return COL_SET | ((uint32_t)i & 0xFFFFFFu);
    }
    if (v.type == VAL_STRING) {
        Col c;
        if (col_parse(v.as.s->data, &c)) return c;
        as_err("%s: unknown colour \"%s\" (use 0xRRGGBB, \"#rrggbb\", \"#rgb\", or a name such as \"red\", \"bright_cyan\", \"gold\")", who, v.as.s->data);
    }
    if (v.type == VAL_ARRAY && v.as.arr->count >= 3) {
        KcValue *it = v.as.arr->items;
        return col_rgb((int)V_num(it[0], who), (int)V_num(it[1], who), (int)V_num(it[2], who));
    }
    if (v.type == VAL_NULL) return 0;
    as_err("%s: expected a colour (number, \"#rrggbb\", name or [r,g,b]), got %s", who, kc_type_name(v));
}
Col A_col(KcValue *a, int n, int i, const char *who) {
    if (i >= n) as_err("%s: missing argument %d", who, i + 1);
    return col_from_value(a[i], who);
}
int as_lerpi(int a, int b, float t) { return (int)lroundf((float)a + (float)(b - a) * t); }
Col col_mix(Col a, Col b, float t) {
    if (!(a & COL_SET)) a = COL_SET; if (!(b & COL_SET)) b = COL_SET;
    return col_rgb(as_lerpi(COL_R(a), COL_R(b), t), as_lerpi(COL_G(a), COL_G(b), t), as_lerpi(COL_B(a), COL_B(b), t));
}
Col col_scale(Col a, float k) {
    if (!(a & COL_SET)) return a;
    return col_rgb((int)(COL_R(a) * k), (int)(COL_G(a) * k), (int)(COL_B(a) * k));
}
Col col_hsv(float h, float s, float v) {
    h = h - floorf(h); float r, g, b; int i = (int)(h * 6); float f = h * 6 - (float)i, p = v * (1 - s), q = v * (1 - f * s), t = v * (1 - (1 - f) * s);
    switch (i % 6) { case 0: r = v; g = t; b = p; break; case 1: r = q; g = v; b = p; break; case 2: r = p; g = v; b = t; break;
                     case 3: r = p; g = q; b = v; break; case 4: r = t; g = p; b = v; break; default: r = v; g = p; b = q; }
    return col_rgb((int)(r * 255 + 0.5f), (int)(g * 255 + 0.5f), (int)(b * 255 + 0.5f));
}
float col_luma(Col c) { return (0.2126f * COL_R(c) + 0.7152f * COL_G(c) + 0.0722f * COL_B(c)) / 255.0f; }

static const uint8_t PAL16[16][3] = {
    {0, 0, 0}, {205, 0, 0}, {0, 205, 0}, {205, 205, 0}, {0, 0, 238}, {205, 0, 205}, {0, 205, 205}, {229, 229, 229},
    {127, 127, 127}, {255, 0, 0}, {0, 255, 0}, {255, 255, 0}, {92, 92, 255}, {255, 0, 255}, {0, 255, 255}, {255, 255, 255},
};
int col_to_16(int r, int g, int b) {
    int best = 0, bd = 1 << 30;
    for (int i = 0; i < 16; i++) {
        int dr = r - PAL16[i][0], dg = g - PAL16[i][1], db = b - PAL16[i][2];
        int d = 2 * dr * dr + 4 * dg * dg + 3 * db * db;           /* weighted: the eye is most sensitive to green */
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}
int col_to_256(int r, int g, int b) {
    static const int lv[6] = { 0, 95, 135, 175, 215, 255 };
    int ri = r < 48 ? 0 : r < 115 ? 1 : (r - 35) / 40, gi = g < 48 ? 0 : g < 115 ? 1 : (g - 35) / 40, bi = b < 48 ? 0 : b < 115 ? 1 : (b - 35) / 40;
    if (ri > 5) ri = 5; if (gi > 5) gi = 5; if (bi > 5) bi = 5;
    int cr = lv[ri], cg = lv[gi], cb = lv[bi];
    int cube = 16 + 36 * ri + 6 * gi + bi;
    int avg = (r + g + b) / 3, gi2 = avg < 8 ? 0 : avg > 238 ? 23 : (avg - 8) / 10;
    int gv = 8 + gi2 * 10;
    int dc = (r - cr) * (r - cr) + (g - cg) * (g - cg) + (b - cb) * (b - cb), dg = (r - gv) * (r - gv) + (g - gv) * (g - gv) + (b - gv) * (b - gv);
    return dg < dc ? 232 + gi2 : cube;
}

/* ------------------------------------------------------------------ utf-8 */
uint32_t utf8_next(const char **pp) {
    const unsigned char *p = (const unsigned char *)*pp;
    uint32_t c = *p;
    if (c < 0x80) { *pp += 1; return c; }
    int len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 0;
    if (!len || c >= 0xF8) { *pp += 1; return 0xFFFD; }
    uint32_t cp = c & (0xFFu >> (len + 1));
    for (int i = 1; i < len; i++) { if ((p[i] & 0xC0) != 0x80) { *pp += (size_t)i; return 0xFFFD; } cp = (cp << 6) | (p[i] & 0x3F); }
    *pp += len;
    if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000) || cp > 0x10FFFF || (cp >= 0xD800 && cp < 0xE000)) return 0xFFFD;
    return cp;
}
int utf8_put(char *o, uint32_t cp) {
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) { o[0] = (char)(0xE0 | (cp >> 12)); o[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[2] = (char)(0x80 | (cp & 0x3F)); return 3; }
    o[0] = (char)(0xF0 | (cp >> 18)); o[1] = (char)(0x80 | ((cp >> 12) & 0x3F)); o[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[3] = (char)(0x80 | (cp & 0x3F)); return 4;
}
int cp_width(uint32_t c) {
    if (c == 0) return 0;
    if (c < 32 || (c >= 0x7F && c < 0xA0)) return 0;
    if (c < 0x300) return 1;
    if ((c >= 0x300 && c <= 0x36F) || (c >= 0x200B && c <= 0x200F) || (c >= 0x20D0 && c <= 0x20FF) || (c >= 0xFE00 && c <= 0xFE0F) || (c >= 0xFE20 && c <= 0xFE2F)) return 0;
    if ((c >= 0x1100 && c <= 0x115F) || (c >= 0x2E80 && c <= 0x303E) || (c >= 0x3041 && c <= 0x33FF) || (c >= 0x3400 && c <= 0x4DBF) || (c >= 0x4E00 && c <= 0x9FFF) ||
        (c >= 0xA000 && c <= 0xA4CF) || (c >= 0xAC00 && c <= 0xD7A3) || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFE30 && c <= 0xFE6F) || (c >= 0xFF00 && c <= 0xFF60) ||
        (c >= 0xFFE0 && c <= 0xFFE6) || (c >= 0x1F300 && c <= 0x1F64F) || (c >= 0x1F680 && c <= 0x1F6FF) || (c >= 0x1F900 && c <= 0x1F9FF) || (c >= 0x20000 && c <= 0x3FFFD)) return 2;
    return 1;
}
int str_width(const char *s) { int w = 0; while (*s) w += cp_width(utf8_next(&s)); return w; }

/* -------------------------------------------------------------- rng / noise */
static uint64_t rng_state = 0x9E3779B97F4A7C15ull;
void as_seed(uint32_t s) { rng_state = 0x9E3779B97F4A7C15ull ^ ((uint64_t)s * 0xBF58476D1CE4E5B9ull); if (!rng_state) rng_state = 1; }
uint32_t as_rand_u32(void) {
    rng_state ^= rng_state >> 12; rng_state ^= rng_state << 25; rng_state ^= rng_state >> 27;
    return (uint32_t)((rng_state * 0x2545F4914F6CDD1Dull) >> 32);
}
float as_rand(void) { return (float)(as_rand_u32() >> 8) / 16777216.0f; }
uint32_t as_hash2(int x, int y, uint32_t seed) {
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u; return h ^ (h >> 16);
}
float as_noise2(float x, float y, uint32_t seed) {
    int xi = (int)floorf(x), yi = (int)floorf(y); float xf = x - (float)xi, yf = y - (float)yi;
    float u = xf * xf * (3 - 2 * xf), v = yf * yf * (3 - 2 * yf);
    float a = (float)(as_hash2(xi, yi, seed) & 0xFFFF) / 65535.0f, b = (float)(as_hash2(xi + 1, yi, seed) & 0xFFFF) / 65535.0f;
    float c = (float)(as_hash2(xi, yi + 1, seed) & 0xFFFF) / 65535.0f, d = (float)(as_hash2(xi + 1, yi + 1, seed) & 0xFFFF) / 65535.0f;
    return a + (b - a) * u + (c - a) * v + (a - b - c + d) * u * v;
}
float as_fbm(float x, float y, uint32_t seed, int oct) {
    float sum = 0, amp = 0.5f, norm = 0; if (oct < 1) oct = 1; if (oct > 8) oct = 8;
    for (int i = 0; i < oct; i++) { sum += amp * as_noise2(x, y, seed + (uint32_t)i * 131u); norm += amp; x *= 2; y *= 2; amp *= 0.5f; }
    return sum / norm;
}

/* ------------------------------------------------------------------- clock */
double as_clock_s(void) {
#if defined(_WIN32)
    static LARGE_INTEGER f; LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c); return (double)c.QuadPart / (double)f.QuadPart;
#else
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#endif
}
void as_sleep_ms(int ms) {
    if (ms <= 0) return;
#if defined(_WIN32)
    Sleep((DWORD)ms);
#else
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L }; nanosleep(&ts, NULL);
#endif
}

/* ------------------------------------------------------------ byte buffer */
void buf_init(Buf *b) { b->d = NULL; b->n = b->cap = 0; }
void buf_add(Buf *b, const void *p, size_t n) {
    if (b->n + n + 1 > b->cap) { size_t nc = b->cap ? b->cap : 256; while (nc < b->n + n + 1) nc *= 2; b->d = kc_realloc(b->d, nc); b->cap = nc; }
    if (n) memcpy(b->d + b->n, p, n);
    b->n += n; b->d[b->n] = 0;
}
void buf_adds(Buf *b, const char *s) { buf_add(b, s, strlen(s)); }
void buf_addc(Buf *b, char c) { buf_add(b, &c, 1); }
void buf_printf(Buf *b, const char *fmt, ...) {
    char tmp[256]; va_list ap; va_start(ap, fmt);
    int l = vsnprintf(tmp, sizeof tmp, fmt, ap); va_end(ap);
    if (l < (int)sizeof tmp) { buf_add(b, tmp, (size_t)(l > 0 ? l : 0)); return; }
    char *big = kc_malloc((size_t)l + 1); va_start(ap, fmt); vsnprintf(big, (size_t)l + 1, fmt, ap); va_end(ap);
    buf_add(b, big, (size_t)l); free(big);
}
void buf_free(Buf *b) { free(b->d); b->d = NULL; b->n = b->cap = 0; }

/* -------------------------------------------------------------------- SGR */
static void sgr_color(Buf *b, int cmode, bool fg, Col c) {
    if (!(c & COL_SET)) { buf_adds(b, fg ? ";39" : ";49"); return; }
    int r = COL_R(c), g = COL_G(c), bl = COL_B(c);
    if (cmode >= 3) buf_printf(b, ";%d;2;%d;%d;%d", fg ? 38 : 48, r, g, bl);
    else if (cmode == 2) buf_printf(b, ";%d;5;%d", fg ? 38 : 48, col_to_256(r, g, bl));
    else { int i = col_to_16(r, g, bl); buf_printf(b, ";%d", (fg ? (i < 8 ? 30 + i : 90 + i - 8) : (i < 8 ? 40 + i : 100 + i - 8))); }
}
static bool sgr_same(int cmode, Col a, Col b) {
    if (a == b) return true;
    if (!(a & COL_SET) || !(b & COL_SET)) return false;
    if (cmode >= 3) return false;
    if (cmode == 2) return col_to_256(COL_R(a), COL_G(a), COL_B(a)) == col_to_256(COL_R(b), COL_G(b), COL_B(b));
    return col_to_16(COL_R(a), COL_G(a), COL_B(a)) == col_to_16(COL_R(b), COL_G(b), COL_B(b));
}
void sgr_apply(Buf *b, int cmode, Col fg, Col bg, uint16_t attr, Col *cf, Col *cb, uint16_t *ca) {
    bool fgc = !sgr_same(cmode, fg, *cf), bgc = !sgr_same(cmode, bg, *cb), atc = attr != *ca;
    if (!fgc && !bgc && !atc) return;
    buf_adds(b, "\x1b[");
    if (atc) {
        buf_addc(b, '0');
        if (attr & AT_BOLD) buf_adds(b, ";1");   if (attr & AT_DIM) buf_adds(b, ";2");   if (attr & AT_ITALIC) buf_adds(b, ";3");
        if (attr & AT_UNDER) buf_adds(b, ";4");  if (attr & AT_BLINK) buf_adds(b, ";5"); if (attr & AT_REVERSE) buf_adds(b, ";7");
        if (attr & AT_STRIKE) buf_adds(b, ";9");
        if (fg & COL_SET) sgr_color(b, cmode, true, fg);
        if (bg & COL_SET) sgr_color(b, cmode, false, bg);
    } else {
        bool first = true;
        if (fgc) { Buf t; buf_init(&t); sgr_color(&t, cmode, true, fg); buf_adds(b, t.d + 1); buf_free(&t); first = false; }
        if (bgc) { Buf t; buf_init(&t); sgr_color(&t, cmode, false, bg); buf_adds(b, first ? t.d + 1 : t.d); buf_free(&t); }
    }
    buf_addc(b, 'm');
    *cf = fg; *cb = bg; *ca = attr;
}

Col col_from_ansi(int i) {
    if (i < 0) i = 0; if (i > 255) i = 255;
    if (i < 16) return col_rgb(PAL16[i][0], PAL16[i][1], PAL16[i][2]);
    if (i >= 232) { int g = 8 + (i - 232) * 10; return col_rgb(g, g, g); }
    static const int lv[6] = { 0, 95, 135, 175, 215, 255 }; i -= 16;
    return col_rgb(lv[i / 36], lv[(i / 6) % 6], lv[i % 6]);
}
