/* std.game — argument parsing, colours, noise, RNG, file helpers, matrices. */
#include "game_internal.h"
#include <stdarg.h>
#include <ctype.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>
#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <direct.h>
#else
#  include <unistd.h>
#endif

Game G;

/* ---------------------------------------------------------------- errors */
void gm_err(const char *fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    kc_error_at(G.pos, "game: %s", buf);
    abort();
}
void gm_warn(const char *fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    fprintf(stderr, "game: warning: %s\n", buf);
}

/* ------------------------------------------------------------- arguments */
void A_argc(int n, int lo, int hi, const char *sig) {
    if (n < lo || n > hi) gm_err("usage: game.%s", sig);
}
bool V_isnum(KcValue v) { return v.type == VAL_INT || v.type == VAL_FLOAT || v.type == VAL_BOOL; }
double V_num(KcValue v, const char *who) {
    if (v.type == VAL_FLOAT) return v.as.f;
    if (v.type == VAL_INT) return (double)v.as.i;
    if (v.type == VAL_BOOL) return v.as.b ? 1.0 : 0.0;
    gm_err("%s: expected a number, got %s", who, kc_type_name(v));
}
double A_num(KcValue *a, int n, int i, const char *who) {
    if (i >= n) gm_err("%s: missing argument %d", who, i + 1);
    return V_num(a[i], who);
}
int A_int(KcValue *a, int n, int i, const char *who) { return (int)floor(A_num(a, n, i, who) + 0.0); }
const char *A_str(KcValue *a, int n, int i, const char *who) {
    if (i >= n) gm_err("%s: missing argument %d", who, i + 1);
    if (a[i].type != VAL_STRING) gm_err("%s: argument %d must be a string, got %s", who, i + 1, kc_type_name(a[i]));
    return a[i].as.s->data;
}
bool V_truthy_arg(KcValue v) { return kc_truthy(v); }
bool A_bool(KcValue *a, int n, int i, const char *who) {
    if (i >= n) gm_err("%s: missing argument %d", who, i + 1);
    return kc_truthy(a[i]);
}
V3 A_v3(KcValue *a, int n, int i, const char *who) {
    if (i >= n) gm_err("%s: missing argument %d", who, i + 1);
    if (a[i].type != VAL_ARRAY || a[i].as.arr->count < 3) gm_err("%s: argument %d must be an array [x, y, z]", who, i + 1);
    KcValue *it = a[i].as.arr->items;
    return v3((float)V_num(it[0], who), (float)V_num(it[1], who), (float)V_num(it[2], who));
}

static int hexv(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

static const struct { const char *n; uint32_t c; } NAMED[] = {
    {"black", 0xFF000000}, {"white", 0xFFFFFFFF}, {"red", 0xFFE53935}, {"green", 0xFF43A047}, {"blue", 0xFF1E88E5},
    {"yellow", 0xFFFDD835}, {"cyan", 0xFF00ACC1}, {"magenta", 0xFFD81B60}, {"orange", 0xFFFB8C00}, {"purple", 0xFF8E24AA},
    {"gray", 0xFF9E9E9E}, {"grey", 0xFF9E9E9E}, {"brown", 0xFF6D4C41}, {"pink", 0xFFF48FB1}, {"gold", 0xFFFFC107},
    {"sky", 0xFF87CEEB}, {"transparent", 0x00000000}, {"none", 0x00000000}, {"lime", 0xFF9CCC65}, {"teal", 0xFF00897B},
    {"navy", 0xFF1A237E}, {"dark", 0xFF14161C}, {"cloud", 0xFFE8EEF4},
};

uint32_t col_from_value(KcValue v, const char *who) {
    if (v.type == VAL_INT || v.type == VAL_FLOAT) {
        int64_t i = v.type == VAL_INT ? v.as.i : (int64_t)v.as.f;
        if (i < 0) return 0x00000000u;                       /* any negative number = fully transparent (game.rgb(.., 0) returns -1) */
        uint32_t u = (uint32_t)i;
        if (i >= 0 && i <= 0xFFFFFF) return 0xFF000000u | u;
        return u;
    }
    if (v.type == VAL_STRING) {
        const char *s = v.as.s->data;
        if (*s == '#' || (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))) {
            s += (*s == '#') ? 1 : 2;
            size_t l = strlen(s); int d[8];
            for (size_t i = 0; i < l && i < 8; i++) { d[i] = hexv(s[i]); if (d[i] < 0) gm_err("%s: bad colour \"%s\"", who, v.as.s->data); }
            if (l == 3 || l == 4) return col_pack(d[0] * 17, d[1] * 17, d[2] * 17, l == 4 ? d[3] * 17 : 255);
            if (l == 6 || l == 8) return col_pack(d[0] * 16 + d[1], d[2] * 16 + d[3], d[4] * 16 + d[5], l == 8 ? d[6] * 16 + d[7] : 255);
            gm_err("%s: bad colour \"%s\" (use #rgb, #rrggbb or #rrggbbaa)", who, v.as.s->data);
        }
        for (size_t i = 0; i < sizeof NAMED / sizeof NAMED[0]; i++) if (!strcmp(NAMED[i].n, s)) return NAMED[i].c;
        gm_err("%s: unknown colour name \"%s\"", who, s);
    }
    if (v.type == VAL_ARRAY && v.as.arr->count >= 3) {
        KcValue *it = v.as.arr->items; int c = v.as.arr->count > 4 ? 4 : v.as.arr->count;
        double q[4] = { 0, 0, 0, 255 }; bool frac = false, any_float = false;
        for (int i = 0; i < c; i++) { q[i] = V_num(it[i], who); if (it[i].type == VAL_FLOAT) any_float = true; if (q[i] > 1.0001) frac = false; }
        bool all_small = true; for (int i = 0; i < c; i++) if (q[i] > 1.0001) all_small = false;
        frac = any_float && all_small;
        if (frac) { for (int i = 0; i < c; i++) q[i] *= 255.0; if (c == 3) q[3] = 255; }
        return col_pack((int)q[0], (int)q[1], (int)q[2], (int)q[3]);
    }
    gm_err("%s: expected a colour (0xRRGGBB, \"#rrggbb\", a name, or [r,g,b,a]), got %s", who, kc_type_name(v));
}
uint32_t A_col(KcValue *a, int n, int i, const char *who) {
    if (i >= n) gm_err("%s: missing colour argument %d", who, i + 1);
    return col_from_value(a[i], who);
}

uint32_t col_mix(uint32_t a, uint32_t b, float t) {
    t = fclampf(t, 0, 1);
    return col_pack((int)(CR(a) + (CR(b) - CR(a)) * t + 0.5f), (int)(CG(a) + (CG(b) - CG(a)) * t + 0.5f),
                    (int)(CB(a) + (CB(b) - CB(a)) * t + 0.5f), (int)(CA(a) + (CA(b) - CA(a)) * t + 0.5f));
}
uint32_t col_mul(uint32_t a, uint32_t b) {
    return col_pack(CR(a) * CR(b) / 255, CG(a) * CG(b) / 255, CB(a) * CB(b) / 255, CA(a) * CA(b) / 255);
}
uint32_t col_scale(uint32_t a, float k) { return col_pack((int)(CR(a) * k), (int)(CG(a) * k), (int)(CB(a) * k), CA(a)); }
uint32_t col_hsv(float h, float s, float v, int a) {
    h = h - floorf(h); float r, g, b; int i = (int)(h * 6); float f = h * 6 - i, p = v * (1 - s), q = v * (1 - f * s), t = v * (1 - (1 - f) * s);
    switch (i % 6) { case 0: r = v; g = t; b = p; break; case 1: r = q; g = v; b = p; break; case 2: r = p; g = v; b = t; break;
                     case 3: r = p; g = q; b = v; break; case 4: r = t; g = p; b = v; break; default: r = v; g = p; b = q; }
    return col_pack((int)(r * 255), (int)(g * 255), (int)(b * 255), a);
}

/* ---------------------------------------------------------- value helpers */
KcValue kc_str(const char *s) { return kc_string(s); }
KcValue arr2(double a, double b) { KcValue r = kc_array_new(); kc_array_push(r, kc_float(a)); kc_array_push(r, kc_float(b)); return r; }
KcValue arr3(double a, double b, double c) { KcValue r = arr2(a, b); kc_array_push(r, kc_float(c)); return r; }
KcValue arr4(double a, double b, double c, double d) { KcValue r = arr3(a, b, c); kc_array_push(r, kc_float(d)); return r; }
KcValue obj_new(void) { return kc_object_new(); }
void obj_val(KcValue o, const char *k, KcValue v) { kc_object_set(o, k, strlen(k), v); }
void obj_num(KcValue o, const char *k, double v) { obj_val(o, k, kc_float(v)); }
void obj_int(KcValue o, const char *k, int64_t v) { obj_val(o, k, kc_int(v)); }
void obj_bool(KcValue o, const char *k, bool v) { obj_val(o, k, kc_bool(v)); }
void obj_str(KcValue o, const char *k, const char *v) { obj_val(o, k, kc_string(v)); }

/* ---------------------------------------------------------------- options */
Opts opts_make(KcValue *a, int n, int start) {
    Opts o; o.a = a; o.n = n; o.start = start; o.has_obj = false; o.obj = kc_null();
    if (start >= n) return o;
    if (n - start == 1 && kc_is_object(a[start])) { o.has_obj = true; o.obj = a[start]; return o; }
    if ((n - start) % 2 != 0) gm_err("options must be \"name\", value pairs (or one options object)");
    for (int i = start; i < n; i += 2) if (a[i].type != VAL_STRING) gm_err("option names must be strings, got %s", kc_type_name(a[i]));
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
uint32_t opt_col(const Opts *o, const char *key, uint32_t def) { const KcValue *v = opt_find(o, key); return v ? col_from_value(*v, key) : def; }
const char *opt_str(const Opts *o, const char *key, const char *def) {
    const KcValue *v = opt_find(o, key);
    if (!v) return def;
    if (v->type != VAL_STRING) gm_err("option \"%s\" must be a string", key);
    return v->as.s->data;
}
V3 opt_v3(const Opts *o, const char *key, V3 def) {
    const KcValue *v = opt_find(o, key);
    if (!v) return def;
    if (v->type == VAL_ARRAY && v->as.arr->count >= 3) return v3((float)V_num(v->as.arr->items[0], key), (float)V_num(v->as.arr->items[1], key), (float)V_num(v->as.arr->items[2], key));
    if (V_isnum(*v)) { float s = (float)V_num(*v, key); return v3(s, s, s); }
    gm_err("option \"%s\" must be a number or [x, y, z]", key);
}

/* ------------------------------------------------------------------ noise */
uint32_t hash32(uint32_t x) {
    x = (x ^ 61u) ^ (x >> 16); x *= 9u; x ^= x >> 4; x *= 0x27d4eb2du; x ^= x >> 15; return x;
}
uint32_t hash3(int x, int y, int z, uint32_t seed) {
    uint32_t h = seed * 0x9e3779b1u + (uint32_t)x * 0x85ebca6bu;
    h = hash32(h ^ ((uint32_t)y * 0xc2b2ae35u));
    h = hash32(h ^ ((uint32_t)z * 0x27d4eb2fu));
    return hash32(h);
}
static inline float fade(float t) { return t * t * (3.0f - 2.0f * t); }
static inline float h01(int x, int y, int z, uint32_t s) { return (float)(hash3(x, y, z, s) & 0xFFFFFF) / 16777215.0f; }
float vnoise2(float x, float y, uint32_t seed) {
    int xi = (int)floorf(x), yi = (int)floorf(y); float fx = fade(x - xi), fy = fade(y - yi);
    float a = h01(xi, yi, 0, seed), b = h01(xi + 1, yi, 0, seed), c = h01(xi, yi + 1, 0, seed), d = h01(xi + 1, yi + 1, 0, seed);
    return lerpf(lerpf(a, b, fx), lerpf(c, d, fx), fy);
}
float vnoise3(float x, float y, float z, uint32_t seed) {
    int xi = (int)floorf(x), yi = (int)floorf(y), zi = (int)floorf(z);
    float fx = fade(x - xi), fy = fade(y - yi), fz = fade(z - zi);
    float c000 = h01(xi, yi, zi, seed), c100 = h01(xi + 1, yi, zi, seed), c010 = h01(xi, yi + 1, zi, seed), c110 = h01(xi + 1, yi + 1, zi, seed);
    float c001 = h01(xi, yi, zi + 1, seed), c101 = h01(xi + 1, yi, zi + 1, seed), c011 = h01(xi, yi + 1, zi + 1, seed), c111 = h01(xi + 1, yi + 1, zi + 1, seed);
    return lerpf(lerpf(lerpf(c000, c100, fx), lerpf(c010, c110, fx), fy), lerpf(lerpf(c001, c101, fx), lerpf(c011, c111, fx), fy), fz);
}
float fbm2(float x, float y, int oct, float lac, float gain, uint32_t seed) {
    float sum = 0, amp = 1, norm = 0, f = 1;
    for (int i = 0; i < oct; i++) { sum += amp * vnoise2(x * f, y * f, seed + (uint32_t)i * 131u); norm += amp; amp *= gain; f *= lac; }
    return norm > 0 ? sum / norm : 0;
}
float fbm3(float x, float y, float z, int oct, float lac, float gain, uint32_t seed) {
    float sum = 0, amp = 1, norm = 0, f = 1;
    for (int i = 0; i < oct; i++) { sum += amp * vnoise3(x * f, y * f, z * f, seed + (uint32_t)i * 131u); norm += amp; amp *= gain; f *= lac; }
    return norm > 0 ? sum / norm : 0;
}

/* -------------------------------------------------------------------- RNG */
static uint64_t rs = 0x9E3779B97F4A7C15ull;
void rnd_seed(uint32_t s) { rs = 0x9E3779B97F4A7C15ull ^ ((uint64_t)s * 0xBF58476D1CE4E5B9ull); if (!rs) rs = 1; }
uint32_t rnd_u32(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (uint32_t)(rs >> 16); }
float rnd_f(void) { return (float)(rnd_u32() & 0xFFFFFF) / 16777216.0f; }
float rnd_range(float lo, float hi) { return lo + (hi - lo) * rnd_f(); }

/* --------------------------------------------------------------- time/files */
double clock_s(void) {
#if defined(_WIN32)
    static LARGE_INTEGER freq; LARGE_INTEGER c;                       /* high-resolution timer: GetTickCount64 only ticks every ~15 ms */
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c); return (double)c.QuadPart / (double)freq.QuadPart;
#else
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#endif
}
void sleep_ms_(int ms) {
    if (ms <= 0) return;
#if defined(_WIN32)
    Sleep((DWORD)ms);
#else
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L }; nanosleep(&ts, NULL);
#endif
}
char *read_file_all(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return NULL; }
    char *b = kc_malloc((size_t)sz + 1);
    size_t got = fread(b, 1, (size_t)sz, f); fclose(f);
    b[got] = 0; if (len) *len = got;
    return b;
}
bool write_file_all(const char *path, const void *d, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(d, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}
char *path_dir(const char *path) {
    const char *s1 = strrchr(path, '/'), *s2 = strrchr(path, '\\'); const char *s = s1 > s2 ? s1 : s2;
    if (!s) return kc_strdup("");
    size_t n = (size_t)(s - path); char *r = kc_malloc(n + 1); memcpy(r, path, n); r[n] = 0; return r;
}
char *path_join(const char *dir, const char *rel) {
    bool abs = rel[0] == '/' || rel[0] == '\\' || (isalpha((unsigned char)rel[0]) && rel[1] == ':');
    if (abs || !*dir) return kc_strdup(rel);
    size_t a = strlen(dir), b = strlen(rel); char *r = kc_malloc(a + b + 2);
    memcpy(r, dir, a); r[a] = '/'; memcpy(r + a + 1, rel, b + 1); return r;
}
bool file_exists(const char *path) { struct stat st; return stat(path, &st) == 0; }
bool dir_make(const char *path) {
    if (!path || !*path) return true;
    char *tmp = kc_strdup(path); bool ok = true;
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char c = *p; *p = 0;
#if defined(_WIN32)
            _mkdir(tmp);
#else
            mkdir(tmp, 0777);
#endif
            *p = c;
        }
    }
#if defined(_WIN32)
    ok = _mkdir(tmp) == 0 || errno == EEXIST;
#else
    ok = mkdir(tmp, 0777) == 0 || errno == EEXIST;
#endif
    free(tmp); return ok;
}

/* --------------------------------------------------------------- matrices */
M4 m4_identity(void) { M4 r; memset(&r, 0, sizeof r); r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1; return r; }
M4 m4_mul(const M4 *a, const M4 *b) {
    M4 r;
    for (int c = 0; c < 4; c++) for (int rr = 0; rr < 4; rr++) {
        float s = 0; for (int k = 0; k < 4; k++) s += a->m[k * 4 + rr] * b->m[c * 4 + k];
        r.m[c * 4 + rr] = s;
    }
    return r;
}
M4 m4_perspective(float fovy, float aspect, float n, float f) {
    M4 r; memset(&r, 0, sizeof r); float t = 1.0f / tanf(fovy * 0.5f);
    r.m[0] = t / aspect; r.m[5] = t; r.m[10] = (f + n) / (n - f); r.m[11] = -1.0f; r.m[14] = 2.0f * f * n / (n - f);
    return r;
}
M4 m4_trs(V3 p, V3 rot, V3 s) {
    float cx = cosf(DEG2RAD(rot.x)), sx = sinf(DEG2RAD(rot.x)), cy = cosf(DEG2RAD(rot.y)), sy = sinf(DEG2RAD(rot.y)), cz = cosf(DEG2RAD(rot.z)), sz = sinf(DEG2RAD(rot.z));
    float Rx[9] = { 1, 0, 0, 0, cx, sx, 0, -sx, cx };        /* column-major 3x3 */
    float Ry[9] = { cy, 0, -sy, 0, 1, 0, sy, 0, cy };
    float Rz[9] = { cz, sz, 0, -sz, cz, 0, 0, 0, 1 };
    float T1[9], R[9];
    for (int c = 0; c < 3; c++) for (int r = 0; r < 3; r++) { float q = 0; for (int k = 0; k < 3; k++) q += Rx[k * 3 + r] * Rz[c * 3 + k]; T1[c * 3 + r] = q; }
    for (int c = 0; c < 3; c++) for (int r = 0; r < 3; r++) { float q = 0; for (int k = 0; k < 3; k++) q += Ry[k * 3 + r] * T1[c * 3 + k]; R[c * 3 + r] = q; }
    M4 m = m4_identity(); float sc[3] = { s.x, s.y, s.z };
    for (int c = 0; c < 3; c++) for (int r = 0; r < 3; r++) m.m[c * 4 + r] = R[c * 3 + r] * sc[c];
    m.m[12] = p.x; m.m[13] = p.y; m.m[14] = p.z;
    return m;
}
V3 m4_point(const M4 *m, V3 p) {
    return v3(m->m[0] * p.x + m->m[4] * p.y + m->m[8] * p.z + m->m[12], m->m[1] * p.x + m->m[5] * p.y + m->m[9] * p.z + m->m[13],
              m->m[2] * p.x + m->m[6] * p.y + m->m[10] * p.z + m->m[14]);
}
V3 m4_dir(const M4 *m, V3 d) {
    return v3(m->m[0] * d.x + m->m[4] * d.y + m->m[8] * d.z, m->m[1] * d.x + m->m[5] * d.y + m->m[9] * d.z, m->m[2] * d.x + m->m[6] * d.y + m->m[10] * d.z);
}
