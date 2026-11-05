/* std.nn core: memory, tensors, RNG, initializers, GEMM */
#include "nn_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <time.h>

/* ------------------------------------------------------------------ memory */
void *nn_malloc(size_t n) { void *p = malloc(n ? n : 1); if (!p) nn_fail("out of memory (%zu bytes)", n); return p; }
void *nn_calloc(size_t c, size_t s) { void *p = calloc(c ? c : 1, s ? s : 1); if (!p) nn_fail("out of memory (%zu bytes)", c * s); return p; }
void *nn_realloc(void *p, size_t n) { void *q = realloc(p, n ? n : 1); if (!q) nn_fail("out of memory (%zu bytes)", n); return q; }
char *nn_strdup(const char *s) { size_t l = strlen(s); char *c = nn_malloc(l + 1); memcpy(c, s, l + 1); return c; }

/* ------------------------------------------------------------------ tensor */
size_t nt_numel(int nd, const int *shape) { size_t n = 1; for (int i = 0; i < nd; i++) n *= (size_t)shape[i]; return n; }
void nt_init(NnTensor *t) { memset(t, 0, sizeof *t); }

const char *nt_shape_str(int nd, const int *shape) {
    static char buf[4][96]; static int slot = 0; char *b = buf[slot = (slot + 1) & 3]; size_t o = 0;
    o += (size_t)snprintf(b + o, sizeof buf[0] - o, "[");
    for (int i = 0; i < nd && o < sizeof buf[0] - 12; i++) o += (size_t)snprintf(b + o, sizeof buf[0] - o, "%s%d", i ? "," : "", shape[i]);
    snprintf(b + o, sizeof buf[0] - o, "]"); return b;
}
void nt_resize(NnTensor *t, int nd, const int *shape) {
    if (nd < 0 || nd > NN_MAXDIM) nn_fail("tensor rank %d is out of range (max %d)", nd, NN_MAXDIM);
    size_t n = 1;
    for (int i = 0; i < nd; i++) {
        if (shape[i] < 0) nn_fail("negative dimension in shape %s", nt_shape_str(nd, shape));
        if (shape[i] && n > ((size_t)1 << 40) / (size_t)shape[i]) nn_fail("tensor shape %s is too large", nt_shape_str(nd, shape));
        n *= (size_t)shape[i];
    }
    t->nd = nd; for (int i = 0; i < nd; i++) t->shape[i] = shape[i]; for (int i = nd; i < NN_MAXDIM; i++) t->shape[i] = 0;
    t->n = n;
    size_t need = n ? n : 1;
    if (need > t->cap) { free(t->d); t->d = nn_malloc(sizeof(nn_real) * need); t->cap = need; }
}
void nt_alloc(NnTensor *t, int nd, const int *shape) { nt_resize(t, nd, shape); memset(t->d, 0, sizeof(nn_real) * (t->n ? t->n : 1)); }
void nt_free(NnTensor *t) { free(t->d); memset(t, 0, sizeof *t); }
void nt_zero(NnTensor *t) { if (t->d) memset(t->d, 0, sizeof(nn_real) * t->n); }
void nt_copy(NnTensor *dst, const NnTensor *src) { nt_resize(dst, src->nd, src->shape); if (src->n) memcpy(dst->d, src->d, sizeof(nn_real) * src->n); }
bool nt_same_shape(const NnTensor *a, const NnTensor *b) {
    if (a->nd != b->nd) return false; for (int i = 0; i < a->nd; i++) if (a->shape[i] != b->shape[i]) return false; return true;
}
NnTensor *nt_new(int nd, const int *shape) { NnTensor *t = nn_malloc(sizeof *t); nt_init(t); nt_alloc(t, nd, shape); return t; }
void nt_delete(NnTensor *t) { if (!t) return; nt_free(t); free(t); }

/* ------------------------------------------------------------------ rng: xoshiro256** seeded by splitmix64 */
static NnRngState R; static bool R_ready = false;
static uint64_t splitmix(uint64_t *x) { uint64_t z = (*x += 0x9E3779B97F4A7C15ULL); z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL; z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL; return z ^ (z >> 31); }
static inline uint64_t rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }
void nn_seed(uint64_t seed) { uint64_t x = seed; for (int i = 0; i < 4; i++) R.s[i] = splitmix(&x); R.has_spare = 0; R_ready = true; }
static uint64_t rng_next(void) {
    if (!R_ready) nn_seed((uint64_t)time(NULL) ^ ((uint64_t)clock() << 20));
    uint64_t *s = R.s, r = rotl(s[1] * 5, 7) * 9, t = s[1] << 17;
    s[2] ^= s[0]; s[3] ^= s[1]; s[1] ^= s[2]; s[0] ^= s[3]; s[2] ^= t; s[3] = rotl(s[3], 45); return r;
}
double nn_rand(void) { return (double)(rng_next() >> 11) * (1.0 / 9007199254740992.0); }
double nn_randn(void) {
    if (R.has_spare) { R.has_spare = 0; return R.spare; }
    double u, v, s;
    do { u = 2.0 * nn_rand() - 1.0; v = 2.0 * nn_rand() - 1.0; s = u * u + v * v; } while (s >= 1.0 || s == 0.0);
    double m = sqrt(-2.0 * log(s) / s); R.spare = v * m; R.has_spare = 1; return u * m;
}
int64_t nn_randint(int64_t n) { if (n <= 1) return 0; return (int64_t)(rng_next() % (uint64_t)n); }
void nn_rng_save(NnRngState *st) { if (!R_ready) nn_seed(12345); *st = R; }
void nn_rng_restore(const NnRngState *st) { R = *st; R_ready = true; }

/* ------------------------------------------------------------------ initializers */
static const char *INIT_NAMES[] = { "none", "xavier", "xavier_normal", "he_normal", "he_uniform", "lecun_normal", "lecun_uniform",
                                    "normal", "uniform", "zeros", "ones", "constant", "orthogonal" };
int nn_init_from_name(const char *n) {
    if (!strcmp(n, "glorot") || !strcmp(n, "glorot_uniform") || !strcmp(n, "xavier_uniform")) return NNI_XAVIER_U;
    if (!strcmp(n, "glorot_normal")) return NNI_XAVIER_N;
    if (!strcmp(n, "he") || !strcmp(n, "kaiming") || !strcmp(n, "he_normal") || !strcmp(n, "kaiming_normal")) return NNI_HE_N;
    if (!strcmp(n, "kaiming_uniform")) return NNI_HE_U;
    if (!strcmp(n, "lecun")) return NNI_LECUN_N;
    for (int i = 1; i < NNI_COUNT; i++) if (!strcmp(n, INIT_NAMES[i])) return i;
    return -1;
}
static void fill_normal(NnTensor *w, double std) { for (size_t i = 0; i < w->n; i++) w->d[i] = std * nn_randn(); }
static void fill_uniform(NnTensor *w, double a) { for (size_t i = 0; i < w->n; i++) w->d[i] = (2.0 * nn_rand() - 1.0) * a; }
/* orthonormalise rows (or columns when rows > cols) of an r x c block by modified Gram-Schmidt */
static void orthogonalize(nn_real *m, int r, int c) {
    bool by_rows = r <= c; int vecs = by_rows ? r : c, len = by_rows ? c : r;
#define EL(v, k) m[by_rows ? (size_t)(v) * c + (k) : (size_t)(k) * c + (v)]
    for (int v = 0; v < vecs; v++) {
        for (int u = 0; u < v; u++) { double dot = 0; for (int k = 0; k < len; k++) dot += EL(u, k) * EL(v, k); for (int k = 0; k < len; k++) EL(v, k) -= dot * EL(u, k); }
        double nrm = 0; for (int k = 0; k < len; k++) nrm += EL(v, k) * EL(v, k); nrm = sqrt(nrm);
        if (nrm < 1e-12) { for (int k = 0; k < len; k++) EL(v, k) = (k == v % len) ? 1.0 : 0.0; continue; }
        for (int k = 0; k < len; k++) EL(v, k) /= nrm;
    }
#undef EL
}
void nn_init_tensor(NnTensor *w, int fi, int fo, int init, double arg) {
    if (fi < 1) fi = 1; if (fo < 1) fo = 1;
    switch (init) {
    case NNI_NONE: break;
    case NNI_XAVIER_U: fill_uniform(w, sqrt(6.0 / (fi + fo))); break;
    case NNI_XAVIER_N: fill_normal(w, sqrt(2.0 / (fi + fo))); break;
    case NNI_HE_N: fill_normal(w, sqrt(2.0 / fi)); break;
    case NNI_HE_U: fill_uniform(w, sqrt(6.0 / fi)); break;
    case NNI_LECUN_N: fill_normal(w, sqrt(1.0 / fi)); break;
    case NNI_LECUN_U: fill_uniform(w, sqrt(3.0 / fi)); break;
    case NNI_NORMAL: fill_normal(w, arg > 0 ? arg : 0.01); break;
    case NNI_UNIFORM: fill_uniform(w, arg > 0 ? arg : 0.05); break;
    case NNI_ZEROS: nt_zero(w); break;
    case NNI_ONES: for (size_t i = 0; i < w->n; i++) w->d[i] = 1.0; break;
    case NNI_CONST: for (size_t i = 0; i < w->n; i++) w->d[i] = arg; break;
    case NNI_ORTHOGONAL: {
        int r = w->nd >= 2 ? w->shape[0] : 1; int c = r ? (int)(w->n / (size_t)r) : 0;
        fill_normal(w, 1.0); if (r > 0 && c > 0) orthogonalize(w->d, r, c);
        double gain = arg > 0 ? arg : 1.0; if (gain != 1.0) for (size_t i = 0; i < w->n; i++) w->d[i] *= gain; break; }
    }
}

/* ------------------------------------------------------------------ gemm */
void nn_gemm_nn(int M, int N, int K, const nn_real *A, const nn_real *B, nn_real *C) {
    for (int i = 0; i < M; i++) {
        nn_real *c = C + (size_t)i * N; const nn_real *a = A + (size_t)i * K;
        for (int k = 0; k < K; k++) { nn_real av = a[k]; if (av == 0) continue; const nn_real *b = B + (size_t)k * N; for (int j = 0; j < N; j++) c[j] += av * b[j]; }
    }
}
void nn_gemm_nt(int M, int N, int K, const nn_real *A, const nn_real *B, nn_real *C) {
    for (int i = 0; i < M; i++) {
        const nn_real *a = A + (size_t)i * K; nn_real *c = C + (size_t)i * N;
        for (int j = 0; j < N; j++) {
            const nn_real *b = B + (size_t)j * K; nn_real s0 = 0, s1 = 0, s2 = 0, s3 = 0; int k = 0;
            for (; k + 3 < K; k += 4) { s0 += a[k] * b[k]; s1 += a[k + 1] * b[k + 1]; s2 += a[k + 2] * b[k + 2]; s3 += a[k + 3] * b[k + 3]; }
            for (; k < K; k++) s0 += a[k] * b[k];
            c[j] += (s0 + s1) + (s2 + s3);
        }
    }
}
void nn_gemm_tn(int M, int N, int K, const nn_real *A, const nn_real *B, nn_real *C) {
    for (int k = 0; k < K; k++) {
        const nn_real *a = A + (size_t)k * M, *b = B + (size_t)k * N;
        for (int i = 0; i < M; i++) { nn_real av = a[i]; if (av == 0) continue; nn_real *c = C + (size_t)i * N; for (int j = 0; j < N; j++) c[j] += av * b[j]; }
    }
}
