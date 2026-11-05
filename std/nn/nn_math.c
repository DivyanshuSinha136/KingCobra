/* KingCobra std.nn — advanced tensor math kernels.
 *
 *   special functions (erf/erfinv/lgamma/digamma/normal cdf & quantile ...), activation derivatives,
 *   ternary broadcasting (where), lane reductions (logsumexp, norms, quantiles, nan-aware ...),
 *   scans (cumsum/cumprod/cummax/cummin/logcumsumexp, diff), stable sorting & top-k,
 *   shape manipulation (broadcast_to, flip, slice, tile, repeat, roll, pad, take/gather/scatter, tril/triu, diag),
 *   products (outer, kron, tensordot, batched matmul, einsum, cross), and statistics (cov, corrcoef, cdist,
 *   histogram, bincount, 1-D convolution).
 *
 * Every kernel validates its input and reports problems through nn_fail().  Inputs are never modified;
 * `out` must be a tensor distinct from the inputs (the API layer always passes a fresh one).
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com>
 */
#include "nn_internal.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>

#define NM_PI 3.14159265358979323846
#define NM_SQRT2 1.41421356237309504880

static void take(NnTensor *out, NnTensor *r) { nt_free(out); *out = *r; }

/* ================================================================== special functions */
double nm_digamma(double x) {
    if (isnan(x)) return x;
    if (x == -INFINITY) return NAN;
    if (x <= 0 && x == floor(x)) return NAN;                              /* poles at 0, -1, -2, ... */
    if (x < 0) return nm_digamma(1.0 - x) - NM_PI / tan(NM_PI * x);       /* reflection */
    double r = 0;
    while (x < 6) { r -= 1.0 / x; x += 1.0; }                             /* recurrence up to the asymptotic region */
    double f = 1.0 / (x * x);
    r += log(x) - 0.5 / x - f * (1.0 / 12 - f * (1.0 / 120 - f * (1.0 / 252 - f * (1.0 / 240 - f * (1.0 / 132)))));
    return r;
}

/* inverse error function: Giles' approximation refined with Newton steps on erf/erfc (full double precision) */
double nm_erfinv(double y) {
    if (isnan(y) || y < -1 || y > 1) return NAN;
    if (y == 0) return 0;
    if (y == 1) return INFINITY;
    if (y == -1) return -INFINITY;
    double sgn = y < 0 ? -1.0 : 1.0, ya = fabs(y);
    double w = -log((1.0 - ya) * (1.0 + ya)), x;
    if (w < 5.0) {
        w -= 2.5; x = 2.81022636e-08;
        x = 3.43273939e-07 + x * w; x = -3.5233877e-06 + x * w; x = -4.39150654e-06 + x * w; x = 0.00021858087 + x * w;
        x = -0.00125372503 + x * w; x = -0.00417768164 + x * w; x = 0.246640727 + x * w; x = 1.50140941 + x * w;
    } else {
        w = sqrt(w) - 3.0; x = -0.000200214257;
        x = 0.000100950558 + x * w; x = 0.00134934322 + x * w; x = -0.00367342844 + x * w; x = 0.00573950773 + x * w;
        x = -0.0076224613 + x * w; x = 0.00943887047 + x * w; x = 1.00167406 + x * w; x = 2.83297682 + x * w;
    }
    x *= ya;
    for (int i = 0; i < 4; i++) {
        double e = ya > 0.5 ? (1.0 - ya) - erfc(x) : erf(x) - ya;          /* erf(x) - ya, computed without cancellation near 1 */
        double d = 1.1283791670955126 * exp(-x * x); if (d == 0) break;
        x -= e / d;
    }
    return sgn * x;
}

/* standard normal quantile: Acklam's rational approximation plus one Halley step */
double nm_ndtri(double p) {
    if (isnan(p) || p < 0 || p > 1) return NAN;
    if (p == 0) return -INFINITY; if (p == 1) return INFINITY;
    static const double a[] = { -3.969683028665376e+01, 2.209460984245205e+02, -2.759285104469687e+02, 1.383577518672690e+02, -3.066479806614716e+01, 2.506628277459239e+00 };
    static const double b[] = { -5.447609879822406e+01, 1.615858368580409e+02, -1.556989798598866e+02, 6.680131188771972e+01, -1.328068155288572e+01 };
    static const double c[] = { -7.784894002430293e-03, -3.223964580411365e-01, -2.400758277161838e+00, -2.549732539343734e+00, 4.374664141464968e+00, 2.938163982698783e+00 };
    static const double d[] = { 7.784695709041462e-03, 3.224671290700398e-01, 2.445134137142996e+00, 3.754408661907416e+00 };
    double x, q, r;
    if (p < 0.02425) { q = sqrt(-2 * log(p)); x = (((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) / ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1); }
    else if (p > 1 - 0.02425) { q = sqrt(-2 * log1p(-p)); x = -(((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) / ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1); }
    else { q = p - 0.5; r = q * q; x = (((((a[0] * r + a[1]) * r + a[2]) * r + a[3]) * r + a[4]) * r + a[5]) * q / (((((b[0] * r + b[1]) * r + b[2]) * r + b[3]) * r + b[4]) * r + 1); }
    double e = 0.5 * erfc(-x / NM_SQRT2) - p, u = e * 2.5066282746310002 * exp(x * x / 2);
    return x - u / (1 + x * u / 2);
}

void nm_unary(int op, const NnTensor *a, NnTensor *out) {
    if (op < 0 || op >= NMU_COUNT) nn_fail("internal: bad unary op %d", op);
    NnTensor r; nt_init(&r); nt_resize(&r, a->nd, a->shape);
    for (size_t i = 0; i < a->n; i++) {
        double x = a->d[i], y = 0;
        switch (op) {
        case NMU_ERF: y = erf(x); break; case NMU_ERFC: y = erfc(x); break; case NMU_ERFINV: y = nm_erfinv(x); break;
        case NMU_LGAMMA: y = lgamma(x); break; case NMU_DIGAMMA: y = nm_digamma(x); break;
        case NMU_LOG1P: y = log1p(x); break; case NMU_EXPM1: y = expm1(x); break;
        case NMU_TAN: y = tan(x); break; case NMU_ASIN: y = asin(x); break; case NMU_ACOS: y = acos(x); break; case NMU_ATAN: y = atan(x); break;
        case NMU_SINH: y = sinh(x); break; case NMU_COSH: y = cosh(x); break; case NMU_ASINH: y = asinh(x); break;
        case NMU_ACOSH: y = acosh(x); break; case NMU_ATANH: y = atanh(x); break;
        case NMU_RSQRT: y = 1.0 / sqrt(x); break; case NMU_RECIP: y = 1.0 / x; break; case NMU_CBRT: y = cbrt(x); break;
        case NMU_LOG2: y = log2(x); break; case NMU_LOG10: y = log10(x); break; case NMU_EXP2: y = exp2(x); break;
        case NMU_TRUNC: y = trunc(x); break; case NMU_FRAC: y = x - trunc(x); break;
        case NMU_LOGSIGMOID: y = x >= 0 ? -log1p(exp(-x)) : x - log1p(exp(x)); break;
        case NMU_NDTR: y = 0.5 * erfc(-x / NM_SQRT2); break; case NMU_NDTRI: y = nm_ndtri(x); break;
        case NMU_LOGIT: y = log(x / (1.0 - x)); break;
        case NMU_ISNAN: y = isnan(x) ? 1 : 0; break; case NMU_ISINF: y = isinf(x) ? 1 : 0; break; case NMU_ISFINITE: y = isfinite(x) ? 1 : 0; break;
        case NMU_LNOT: y = x == 0 ? 1 : 0; break;
        }
        r.d[i] = y;
    }
    take(out, &r);
}

/* ================================================================== activations and their derivatives */
#define SELU_S 1.0507009873554805
#define SELU_A 1.6732632423543772
static inline double sg(double x) { return x >= 0 ? 1.0 / (1.0 + exp(-x)) : exp(x) / (1.0 + exp(x)); }
void nm_act(int id, double alpha, bool deriv, const NnTensor *x, NnTensor *out) {
    if (id == NNA_SOFTMAX || id == NNA_LOGSOFTMAX) nn_fail("%s is not an elementwise activation; use softmax(t, axis) / log_softmax_axis(t, axis)", nn_act_name(id));
    NnTensor r; nt_init(&r); nt_resize(&r, x->nd, x->shape);
    if (!deriv) { nn_act_forward_elems(id, alpha, x->n, x->d, r.d); take(out, &r); return; }
    for (size_t i = 0; i < x->n; i++) {
        double v = x->d[i], g = 1;
        switch (id) {
        case NNA_RELU: g = v > 0; break;
        case NNA_RELU6: g = v > 0 && v < 6; break;
        case NNA_LEAKY: g = v > 0 ? 1 : alpha; break;
        case NNA_ELU: g = v > 0 ? 1 : alpha * exp(v); break;
        case NNA_SELU: g = v > 0 ? SELU_S : SELU_S * SELU_A * exp(v); break;
        case NNA_GELU: { double u = 0.7978845608028654 * (v + 0.044715 * v * v * v), t = tanh(u);
            g = 0.5 * (1.0 + t) + 0.5 * v * (1.0 - t * t) * 0.7978845608028654 * (1.0 + 3.0 * 0.044715 * v * v); break; }
        case NNA_SIGMOID: { double s = sg(v); g = s * (1.0 - s); break; }
        case NNA_TANH: { double t = tanh(v); g = 1.0 - t * t; break; }
        case NNA_SOFTPLUS: g = sg(v); break;
        case NNA_SILU: { double s = sg(v); g = s + v * s * (1.0 - s); break; }
        case NNA_MISH: { double sp = v > 30 ? v : log1p(exp(v)), t = tanh(sp), s = sg(v); g = t + v * (1.0 - t * t) * s; break; }
        case NNA_SOFTSIGN: { double q = 1.0 + fabs(v); g = 1.0 / (q * q); break; }
        case NNA_HARDSIG: { double q = 0.2 * v + 0.5; g = (q > 0 && q < 1) ? 0.2 : 0; break; }
        default: g = 1; break;                                                  /* identity */
        }
        r.d[i] = g;
    }
    take(out, &r);
}

/* ================================================================== ternary broadcasting */
static int bshape_n(const NnTensor *const *ts, int n, int *os) {
    int nd = 0; for (int k = 0; k < n; k++) if (ts[k]->nd > nd) nd = ts[k]->nd;
    for (int i = 0; i < nd; i++) {
        int dim = 1;
        for (int k = 0; k < n; k++) {
            int ik = i - (nd - ts[k]->nd), dk = ik >= 0 ? ts[k]->shape[ik] : 1;
            if (dk == 1) continue; if (dim == 1) dim = dk; else if (dim != dk) return -1;
        }
        os[i] = dim;
    }
    return nd;
}
static void bstrides(const NnTensor *t, int nd, size_t *st) {
    size_t acc = 1;
    for (int i = nd - 1; i >= 0; i--) {
        int it = i - (nd - t->nd);
        if (it >= 0) { st[i] = t->shape[it] != 1 ? acc : 0; acc *= (size_t)t->shape[it]; } else st[i] = 0;
    }
}
void nm_where(const NnTensor *c, const NnTensor *a, const NnTensor *b, NnTensor *out) {
    const NnTensor *ts[3] = { c, a, b }; int os[NN_MAXDIM]; int nd = bshape_n(ts, 3, os);
    if (nd < 0) nn_fail("where: cannot broadcast shapes %s, %s and %s", nt_shape_str(c->nd, c->shape), nt_shape_str(a->nd, a->shape), nt_shape_str(b->nd, b->shape));
    size_t st[3][NN_MAXDIM]; for (int k = 0; k < 3; k++) bstrides(ts[k], nd, st[k]);
    NnTensor r; nt_init(&r); nt_resize(&r, nd, os);
    int idx[NN_MAXDIM] = { 0 }; size_t off[3] = { 0, 0, 0 };
    for (size_t k = 0; k < r.n; k++) {
        r.d[k] = c->d[off[0]] != 0 ? a->d[off[1]] : b->d[off[2]];
        for (int i = nd - 1; i >= 0; i--) {
            idx[i]++; for (int q = 0; q < 3; q++) off[q] += st[q][i];
            if (idx[i] < os[i]) break;
            for (int q = 0; q < 3; q++) off[q] -= st[q][i] * (size_t)os[i]; idx[i] = 0;
        }
    }
    take(out, &r);
}

/* ================================================================== lane helpers */
typedef struct { size_t outer, len, inner; } Lanes;
static Lanes lanes_of(const NnTensor *a, int axis) {
    Lanes L = { 1, (size_t)a->shape[axis], 1 };
    for (int i = 0; i < axis; i++) L.outer *= (size_t)a->shape[i];
    for (int i = axis + 1; i < a->nd; i++) L.inner *= (size_t)a->shape[i];
    return L;
}
static int cmp_dbl(const void *x, const void *y) { double a = *(const double *)x, b = *(const double *)y; return (a > b) - (a < b); }

/* ================================================================== lane reductions */
static double quantile_sorted(const double *s, size_t len, double q) {
    double pos = q * (double)(len - 1); size_t lo = (size_t)floor(pos), hi = (size_t)ceil(pos); if (hi >= len) hi = len - 1;
    return s[lo] + (s[hi] - s[lo]) * (pos - (double)lo);
}
static double lane_value(int op, double *buf, size_t len, double param) {
    switch (op) {
    case NMR_ANY: for (size_t k = 0; k < len; k++) if (buf[k] != 0) return 1; return 0;
    case NMR_ALL: for (size_t k = 0; k < len; k++) if (buf[k] == 0) return 0; return 1;
    case NMR_COUNT: { size_t c = 0; for (size_t k = 0; k < len; k++) c += buf[k] != 0; return (double)c; }
    case NMR_LOGSUMEXP: {
        double m = -INFINITY; for (size_t k = 0; k < len; k++) { if (isnan(buf[k])) return NAN; if (buf[k] > m) m = buf[k]; }
        if (isinf(m)) return m;                                               /* all -inf -> -inf, any +inf -> +inf */
        double s = 0; for (size_t k = 0; k < len; k++) s += exp(buf[k] - m); return m + log(s); }
    case NMR_NORM: {
        double p = param;
        if (p == INFINITY) { double m = 0; for (size_t k = 0; k < len; k++) { double v = fabs(buf[k]); if (isnan(v)) return NAN; if (v > m) m = v; } return m; }
        if (p == -INFINITY) { double m = INFINITY; for (size_t k = 0; k < len; k++) { double v = fabs(buf[k]); if (isnan(v)) return NAN; if (v < m) m = v; } return m; }
        if (p == 0) { size_t c = 0; for (size_t k = 0; k < len; k++) c += buf[k] != 0; return (double)c; }
        if (p == 1) { double s = 0; for (size_t k = 0; k < len; k++) s += fabs(buf[k]); return s; }
        if (p == 2) {                                                          /* scaled to avoid overflow/underflow */
            double sc = 0; for (size_t k = 0; k < len; k++) { double v = fabs(buf[k]); if (isnan(v)) return NAN; if (v > sc) sc = v; }
            if (sc == 0 || isinf(sc)) return sc;
            double s = 0; for (size_t k = 0; k < len; k++) { double v = buf[k] / sc; s += v * v; } return sc * sqrt(s); }
        double s = 0; for (size_t k = 0; k < len; k++) s += pow(fabs(buf[k]), p); return pow(s, 1.0 / p); }
    case NMR_QUANTILE: {
        for (size_t k = 0; k < len; k++) if (isnan(buf[k])) return NAN;
        qsort(buf, len, sizeof(double), cmp_dbl); return quantile_sorted(buf, len, param); }
    case NMR_NANSUM: { double s = 0; for (size_t k = 0; k < len; k++) if (!isnan(buf[k])) s += buf[k]; return s; }
    case NMR_NANMEAN: { double s = 0; size_t c = 0; for (size_t k = 0; k < len; k++) if (!isnan(buf[k])) { s += buf[k]; c++; } return c ? s / (double)c : NAN; }
    case NMR_PTP: { double lo = buf[0], hi = buf[0]; for (size_t k = 0; k < len; k++) { if (isnan(buf[k])) return NAN; if (buf[k] < lo) lo = buf[k]; if (buf[k] > hi) hi = buf[k]; } return hi - lo; }
    }
    return 0;
}
void nm_reduce(int op, const NnTensor *a, int axis, bool keepdims, double param, NnTensor *out) {
    size_t outer, len, inner; int os[NN_MAXDIM], ond = 0;
    if (axis == NT_AXIS_ALL) { outer = 1; len = a->n; inner = 1; if (keepdims) { ond = a->nd; for (int i = 0; i < ond; i++) os[i] = 1; } }
    else {
        Lanes L = lanes_of(a, axis); outer = L.outer; len = L.len; inner = L.inner;
        for (int i = 0; i < a->nd; i++) { if (i == axis) { if (keepdims) os[ond++] = 1; } else os[ond++] = a->shape[i]; }
    }
    if (len == 0 && op != NMR_ANY && op != NMR_ALL && op != NMR_COUNT && op != NMR_NANSUM && !(op == NMR_NORM) && !(op == NMR_LOGSUMEXP))
        nn_fail("cannot reduce over an empty axis");
    NnTensor r; nt_init(&r); nt_resize(&r, ond, os);
    double *buf = nn_malloc(sizeof(double) * (len ? len : 1));
    for (size_t o = 0; o < outer; o++) for (size_t in = 0; in < inner; in++) {
        const double *p = a->d + o * len * inner + in;
        for (size_t k = 0; k < len; k++) buf[k] = p[k * inner];
        double v;
        if (len == 0) v = op == NMR_ALL ? 1 : (op == NMR_LOGSUMEXP ? -INFINITY : 0);
        else v = lane_value(op, buf, len, param);
        r.d[o * inner + in] = v;
    }
    free(buf); take(out, &r);
}

/* ================================================================== scans */
void nm_cum(int op, const NnTensor *a, int axis, bool reverse, NnTensor *out) {
    Lanes L = lanes_of(a, axis); NnTensor r; nt_init(&r); nt_resize(&r, a->nd, a->shape);
    for (size_t o = 0; o < L.outer; o++) for (size_t in = 0; in < L.inner; in++) {
        const double *p = a->d + o * L.len * L.inner + in; double *q = r.d + o * L.len * L.inner + in; double acc = 0;
        for (size_t t = 0; t < L.len; t++) {
            size_t k = reverse ? L.len - 1 - t : t; double v = p[k * L.inner];
            if (t == 0) acc = v;
            else switch (op) {
            case NMC_SUM: acc += v; break; case NMC_PROD: acc *= v; break;
            case NMC_MAX: acc = (isnan(acc) || isnan(v)) ? NAN : (v > acc ? v : acc); break;
            case NMC_MIN: acc = (isnan(acc) || isnan(v)) ? NAN : (v < acc ? v : acc); break;
            case NMC_LOGSUMEXP: { if (acc == v) acc = acc + 0.6931471805599453; else { double m = acc > v ? acc : v, d = fabs(acc - v); acc = isnan(d) ? NAN : m + log1p(exp(-d)); } break; }
            }
            q[k * L.inner] = acc;
        }
    }
    take(out, &r);
}
static void diff_once(const NnTensor *a, int axis, NnTensor *out) {
    Lanes L = lanes_of(a, axis); int os[NN_MAXDIM]; memcpy(os, a->shape, sizeof(int) * (size_t)a->nd); os[axis] = L.len ? (int)L.len - 1 : 0;
    NnTensor r; nt_init(&r); nt_resize(&r, a->nd, os); size_t nl = r.n ? (size_t)os[axis] : 0;
    for (size_t o = 0; o < L.outer; o++) for (size_t k = 0; k < nl; k++) for (size_t in = 0; in < L.inner; in++)
        r.d[(o * nl + k) * L.inner + in] = a->d[(o * L.len + k + 1) * L.inner + in] - a->d[(o * L.len + k) * L.inner + in];
    take(out, &r);
}
void nm_diff(const NnTensor *a, int order, int axis, NnTensor *out) {
    if (order < 0) nn_fail("diff: order must be >= 0");
    if ((size_t)order >= (size_t)a->shape[axis] && order > 0) nn_fail("diff: order %d needs more than %d values along axis %d", order, a->shape[axis], axis);
    NnTensor cur; nt_init(&cur); nt_copy(&cur, a);
    for (int i = 0; i < order; i++) { NnTensor nx; nt_init(&nx); diff_once(&cur, axis, &nx); nt_free(&cur); cur = nx; }
    take(out, &cur);
}

/* ================================================================== stable sort / top-k */
static inline bool before(double a, double b, bool desc) {              /* strict "a sorts before b"; NaN always last */
    if (isnan(a)) return false; if (isnan(b)) return true;
    return desc ? a > b : a < b;
}
static void sort_lane(const double *key, size_t n, bool desc, size_t *idx, size_t *tmp) {
    for (size_t i = 0; i < n; i++) idx[i] = i;
    for (size_t w = 1; w < n; w *= 2) {                                    /* bottom-up stable merge sort */
        for (size_t lo = 0; lo < n; lo += 2 * w) {
            size_t mid = lo + w < n ? lo + w : n, hi = lo + 2 * w < n ? lo + 2 * w : n, i = lo, j = mid, k = lo;
            while (i < mid && j < hi) tmp[k++] = before(key[idx[j]], key[idx[i]], desc) ? idx[j++] : idx[i++];
            while (i < mid) tmp[k++] = idx[i++]; while (j < hi) tmp[k++] = idx[j++];
        }
        memcpy(idx, tmp, sizeof(size_t) * n);
    }
}
static void sort_impl(const NnTensor *a, int axis, bool desc, int64_t keep, NnTensor *vals, NnTensor *idxo) {
    Lanes L = lanes_of(a, axis); size_t kk = keep < 0 ? L.len : (size_t)keep; int os[NN_MAXDIM]; memcpy(os, a->shape, sizeof(int) * (size_t)a->nd); os[axis] = (int)kk;
    NnTensor rv, ri; nt_init(&rv); nt_init(&ri); nt_resize(&rv, a->nd, os); nt_resize(&ri, a->nd, os);
    double *key = nn_malloc(sizeof(double) * (L.len + 1)); size_t *ix = nn_malloc(sizeof(size_t) * (L.len + 1)), *tm = nn_malloc(sizeof(size_t) * (L.len + 1));
    for (size_t o = 0; o < L.outer; o++) for (size_t in = 0; in < L.inner; in++) {
        const double *p = a->d + o * L.len * L.inner + in;
        for (size_t k = 0; k < L.len; k++) key[k] = p[k * L.inner];
        sort_lane(key, L.len, desc, ix, tm);
        for (size_t k = 0; k < kk; k++) { size_t dst = (o * kk + k) * L.inner + in; rv.d[dst] = key[ix[k]]; ri.d[dst] = (double)ix[k]; }
    }
    free(key); free(ix); free(tm);
    if (vals) take(vals, &rv); else nt_free(&rv);
    if (idxo) take(idxo, &ri); else nt_free(&ri);
}
void nm_sort(const NnTensor *a, int axis, bool descending, NnTensor *vals, NnTensor *idx) { sort_impl(a, axis, descending, -1, vals, idx); }
void nm_topk(const NnTensor *a, int k, int axis, bool largest, NnTensor *vals, NnTensor *idx) {
    if (k < 0 || k > a->shape[axis]) nn_fail("topk: k=%d is out of range for an axis of size %d", k, a->shape[axis]);
    sort_impl(a, axis, largest, k, vals, idx);
}

/* ================================================================== shape manipulation */
static NnTensor view_lead(const NnTensor *a, int nd) {                 /* same buffer, extra leading 1-dims (never freed) */
    NnTensor v = *a; int off = nd - a->nd; v.nd = nd;
    for (int i = 0; i < NN_MAXDIM; i++) v.shape[i] = (i < nd) ? (i < off ? 1 : a->shape[i - off]) : 0;
    return v;
}
static void materialize(const NnTensor *a, ptrdiff_t base, const ptrdiff_t *st, int nd, const int *os, NnTensor *out) {
    NnTensor r; nt_init(&r); nt_resize(&r, nd, os);
    if (r.n) {
        int idx[NN_MAXDIM] = { 0 }; ptrdiff_t off = base;
        for (size_t k = 0; k < r.n; k++) {
            r.d[k] = a->d[off];
            for (int i = nd - 1; i >= 0; i--) { idx[i]++; off += st[i]; if (idx[i] < os[i]) break; off -= st[i] * (ptrdiff_t)os[i]; idx[i] = 0; }
        }
    }
    take(out, &r);
}
static void istrides(const NnTensor *a, ptrdiff_t *st) { ptrdiff_t acc = 1; for (int i = a->nd - 1; i >= 0; i--) { st[i] = acc; acc *= a->shape[i]; } }

void nm_broadcast_to(const NnTensor *a, int nd, const int *shape, NnTensor *out) {
    if (nd < a->nd) nn_fail("broadcast_to: cannot broadcast %s to the lower-rank shape %s", nt_shape_str(a->nd, a->shape), nt_shape_str(nd, shape));
    ptrdiff_t ist[NN_MAXDIM], st[NN_MAXDIM]; istrides(a, ist);
    for (int i = 0; i < nd; i++) {
        int ia = i - (nd - a->nd);
        if (ia < 0) { st[i] = 0; continue; }
        if (a->shape[ia] == shape[i]) st[i] = a->shape[ia] == 1 ? 0 : ist[ia];
        else if (a->shape[ia] == 1) st[i] = 0;
        else nn_fail("broadcast_to: cannot broadcast %s to %s", nt_shape_str(a->nd, a->shape), nt_shape_str(nd, shape));
    }
    materialize(a, 0, st, nd, shape, out);
}
void nm_flip(const NnTensor *a, const bool *axes, NnTensor *out) {
    ptrdiff_t ist[NN_MAXDIM], st[NN_MAXDIM], base = 0; istrides(a, ist);
    for (int i = 0; i < a->nd; i++) { if (axes[i] && a->shape[i] > 0) { base += (ptrdiff_t)(a->shape[i] - 1) * ist[i]; st[i] = -ist[i]; } else st[i] = ist[i]; }
    materialize(a, base, st, a->nd, a->shape, out);
}
void nm_slice_axis(const NnTensor *a, int axis, int64_t start, int64_t stop, int64_t step, NnTensor *out) {
    int64_t N = a->shape[axis]; if (step < 1) nn_fail("slice_axis: step must be >= 1");
    if (start < 0) start += N; if (stop < 0) stop += N;
    if (start < 0) start = 0; if (start > N) start = N; if (stop < 0) stop = 0; if (stop > N) stop = N;
    int64_t cnt = stop > start ? (stop - start + step - 1) / step : 0;
    ptrdiff_t ist[NN_MAXDIM], st[NN_MAXDIM]; istrides(a, ist); int os[NN_MAXDIM]; memcpy(os, a->shape, sizeof(int) * (size_t)a->nd);
    for (int i = 0; i < a->nd; i++) st[i] = ist[i];
    st[axis] = ist[axis] * (ptrdiff_t)step; os[axis] = (int)cnt;
    materialize(a, cnt ? (ptrdiff_t)start * ist[axis] : 0, st, a->nd, os, out);
}

/* index remapping: each output coordinate maps to a source coordinate per dimension */
typedef struct { int mode; int64_t p; } DimMap;
enum { DM_ID, DM_MOD, DM_DIV, DM_ROLL, DM_PAD };
static void remap(const NnTensor *a, const DimMap *dm, int padmode, const int *os, int nd, double fillv, NnTensor *out) {
    NnTensor r; nt_init(&r); nt_resize(&r, nd, os);
    size_t ast[NN_MAXDIM], acc = 1; for (int i = nd - 1; i >= 0; i--) { ast[i] = acc; acc *= (size_t)a->shape[i]; }
    for (size_t k = 0; k < r.n; k++) {
        size_t rem = k, off = 0; bool fill = false;
        for (int i = nd - 1; i >= 0; i--) {
            int64_t c = (int64_t)(rem % (size_t)os[i]); rem /= (size_t)os[i];
            int64_t N = a->shape[i], s = 0;
            switch (dm[i].mode) {
            case DM_ID: s = c; break;
            case DM_MOD: s = c % N; break;
            case DM_DIV: s = c / dm[i].p; break;
            case DM_ROLL: s = ((c - dm[i].p) % N + N) % N; break;
            default: {
                int64_t t = c - dm[i].p;
                if (t >= 0 && t < N) s = t;
                else if (padmode == NMP_CONST) s = -1;
                else if (padmode == NMP_EDGE) s = t < 0 ? 0 : N - 1;
                else if (padmode == NMP_WRAP) s = ((t % N) + N) % N;
                else if (N == 1) s = 0;
                else { int64_t per = 2 * (N - 1); t = ((t % per) + per) % per; s = t < N ? t : per - t; }
            }}
            if (s < 0) { fill = true; break; }
            off += (size_t)s * ast[i];
        }
        r.d[k] = fill ? fillv : a->d[off];
    }
    take(out, &r);
}
void nm_tile(const NnTensor *a, const int *reps, int nr, NnTensor *out) {
    int nd = a->nd > nr ? a->nd : nr; if (nd > NN_MAXDIM) nn_fail("tile: result would have %d dimensions (max %d)", nd, NN_MAXDIM);
    NnTensor v = view_lead(a, nd); int os[NN_MAXDIM]; DimMap dm[NN_MAXDIM];
    for (int i = 0; i < nd; i++) { int ri = i - (nd - nr); int rp = ri >= 0 ? reps[ri] : 1; if (rp < 0) nn_fail("tile: repetitions must be >= 0"); os[i] = v.shape[i] * rp; dm[i].mode = DM_MOD; dm[i].p = 0; }
    remap(&v, dm, 0, os, nd, 0, out);
}
void nm_repeat(const NnTensor *a, int64_t times, int axis, NnTensor *out) {
    if (times < 0) nn_fail("repeat: count must be >= 0");
    int os[NN_MAXDIM]; DimMap dm[NN_MAXDIM]; memcpy(os, a->shape, sizeof(int) * (size_t)a->nd);
    for (int i = 0; i < a->nd; i++) { dm[i].mode = DM_ID; dm[i].p = 0; }
    os[axis] = (int)(a->shape[axis] * times); dm[axis].mode = DM_DIV; dm[axis].p = times ? times : 1;
    remap(a, dm, 0, os, a->nd, 0, out);
}
void nm_roll(const NnTensor *a, int64_t shift, int axis, NnTensor *out) {
    DimMap dm[NN_MAXDIM]; for (int i = 0; i < a->nd; i++) { dm[i].mode = DM_ID; dm[i].p = 0; }
    dm[axis].mode = DM_ROLL; dm[axis].p = shift;
    remap(a, dm, 0, a->shape, a->nd, 0, out);
}
void nm_pad(const NnTensor *a, const int *before, const int *after, int mode, double value, NnTensor *out) {
    int os[NN_MAXDIM]; DimMap dm[NN_MAXDIM];
    for (int i = 0; i < a->nd; i++) {
        if (before[i] < 0 || after[i] < 0) nn_fail("pad: padding amounts must be >= 0");
        if (mode != NMP_CONST && a->shape[i] == 0 && (before[i] || after[i])) nn_fail("pad: cannot pad an empty axis with this mode");
        os[i] = a->shape[i] + before[i] + after[i]; dm[i].mode = DM_PAD; dm[i].p = before[i];
    }
    remap(a, dm, mode, os, a->nd, value, out);
}

/* ---- indexing ---- */
void nm_take(const NnTensor *a, const int64_t *idx, int64_t n, int axis, NnTensor *out) {
    Lanes L = lanes_of(a, axis); int os[NN_MAXDIM]; memcpy(os, a->shape, sizeof(int) * (size_t)a->nd); os[axis] = (int)n;
    NnTensor r; nt_init(&r); nt_resize(&r, a->nd, os);
    for (int64_t i = 0; i < n; i++) {
        int64_t ix = idx[i]; if (ix < 0) ix += (int64_t)L.len;
        if (ix < 0 || ix >= (int64_t)L.len) nn_fail("take: index %lld is out of range for axis %d of size %zu", (long long)idx[i], axis, L.len);
        for (size_t o = 0; o < L.outer; o++) if (L.inner) memcpy(r.d + (o * (size_t)n + (size_t)i) * L.inner, a->d + (o * L.len + (size_t)ix) * L.inner, sizeof(nn_real) * L.inner);
    }
    take(out, &r);
}
static void check_index_shape(const NnTensor *a, const NnTensor *idx, int axis, const char *fn) {
    if (idx->nd != a->nd) nn_fail("%s: index must have the same number of dimensions as the tensor (%d vs %d)", fn, idx->nd, a->nd);
    for (int d = 0; d < a->nd; d++) if (d != axis && idx->shape[d] > a->shape[d]) nn_fail("%s: index shape %s does not fit tensor shape %s outside axis %d", fn, nt_shape_str(idx->nd, idx->shape), nt_shape_str(a->nd, a->shape), axis);
}
static size_t gather_offset(const NnTensor *a, const NnTensor *idx, size_t k, int axis, const size_t *ast, const char *fn) {
    size_t rem = k, off = 0;
    for (int d = idx->nd - 1; d >= 0; d--) {
        size_t c = rem % (size_t)idx->shape[d]; rem /= (size_t)idx->shape[d];
        if (d == axis) {
            double v = idx->d[k]; int64_t gi = (int64_t)v;
            if (v != (double)gi) nn_fail("%s: indices must be integers (got %g)", fn, v);
            if (gi < 0) gi += a->shape[axis];
            if (gi < 0 || gi >= a->shape[axis]) nn_fail("%s: index %g is out of range for axis %d of size %d", fn, v, axis, a->shape[axis]);
            off += (size_t)gi * ast[d];
        } else off += c * ast[d];
    }
    return off;
}
void nm_gather(const NnTensor *a, const NnTensor *idx, int axis, NnTensor *out) {
    check_index_shape(a, idx, axis, "gather");
    size_t ast[NN_MAXDIM], acc = 1; for (int i = a->nd - 1; i >= 0; i--) { ast[i] = acc; acc *= (size_t)a->shape[i]; }
    NnTensor r; nt_init(&r); nt_resize(&r, idx->nd, idx->shape);
    for (size_t k = 0; k < idx->n; k++) r.d[k] = a->d[gather_offset(a, idx, k, axis, ast, "gather")];
    take(out, &r);
}
void nm_scatter(const NnTensor *base, const NnTensor *idx, const NnTensor *src, int axis, bool add, NnTensor *out) {
    const char *fn = add ? "scatter_add" : "scatter";
    check_index_shape(base, idx, axis, fn);
    if (!nt_same_shape(idx, src)) nn_fail("%s: source shape %s must equal index shape %s", fn, nt_shape_str(src->nd, src->shape), nt_shape_str(idx->nd, idx->shape));
    size_t ast[NN_MAXDIM], acc = 1; for (int i = base->nd - 1; i >= 0; i--) { ast[i] = acc; acc *= (size_t)base->shape[i]; }
    NnTensor r; nt_init(&r); nt_copy(&r, base);
    for (size_t k = 0; k < idx->n; k++) { size_t off = gather_offset(base, idx, k, axis, ast, fn); if (add) r.d[off] += src->d[k]; else r.d[off] = src->d[k]; }
    take(out, &r);
}
void nm_tri(const NnTensor *a, int64_t k, bool upper, NnTensor *out) {
    if (a->nd < 2) nn_fail("%s: needs at least 2 dimensions", upper ? "triu" : "tril");
    int R = a->shape[a->nd - 2], C = a->shape[a->nd - 1]; size_t mats = (R && C) ? a->n / ((size_t)R * (size_t)C) : 0;
    NnTensor r; nt_init(&r); nt_resize(&r, a->nd, a->shape);
    for (size_t m = 0; m < mats; m++) for (int i = 0; i < R; i++) for (int j = 0; j < C; j++) {
        size_t o = (m * (size_t)R + (size_t)i) * (size_t)C + (size_t)j; int64_t d = (int64_t)j - i;
        r.d[o] = (upper ? d >= k : d <= k) ? a->d[o] : 0;
    }
    take(out, &r);
}
void nm_diag(const NnTensor *a, int64_t k, NnTensor *out) {
    NnTensor r; nt_init(&r);
    if (a->nd == 1) {
        int64_t n = a->shape[0] + (k < 0 ? -k : k); int sh[2] = { (int)n, (int)n }; nt_alloc(&r, 2, sh);
        for (int i = 0; i < a->shape[0]; i++) { int64_t row = k >= 0 ? i : i - k, col = k >= 0 ? i + k : i; r.d[(size_t)row * (size_t)n + (size_t)col] = a->d[i]; }
    } else if (a->nd == 2) {
        int R = a->shape[0], C = a->shape[1]; int64_t len = k >= 0 ? (C - k < R ? C - k : R) : (R + k < C ? R + k : C); if (len < 0) len = 0;
        int sh[1] = { (int)len }; nt_alloc(&r, 1, sh);
        for (int64_t i = 0; i < len; i++) { int64_t row = k >= 0 ? i : i - k, col = k >= 0 ? i + k : i; r.d[i] = a->d[(size_t)row * (size_t)C + (size_t)col]; }
    } else nn_fail("diag: needs a 1-D or 2-D tensor (got %d dimensions)", a->nd);
    take(out, &r);
}

/* ================================================================== products */
void nm_outer(const NnTensor *a, const NnTensor *b, NnTensor *out) {
    int sh[2] = { (int)a->n, (int)b->n }; NnTensor r; nt_init(&r); nt_resize(&r, 2, sh);
    for (size_t i = 0; i < a->n; i++) for (size_t j = 0; j < b->n; j++) r.d[i * b->n + j] = a->d[i] * b->d[j];
    take(out, &r);
}
void nm_kron(const NnTensor *a, const NnTensor *b, NnTensor *out) {
    int nd = a->nd > b->nd ? a->nd : b->nd; if (nd == 0) nn_fail("kron: needs tensors with at least 1 dimension");
    NnTensor va = view_lead(a, nd), vb = view_lead(b, nd); int os[NN_MAXDIM];
    for (int i = 0; i < nd; i++) os[i] = va.shape[i] * vb.shape[i];
    size_t sa[NN_MAXDIM], sb[NN_MAXDIM], ac = 1, bc = 1; for (int i = nd - 1; i >= 0; i--) { sa[i] = ac; sb[i] = bc; ac *= (size_t)va.shape[i]; bc *= (size_t)vb.shape[i]; }
    NnTensor r; nt_init(&r); nt_resize(&r, nd, os);
    for (size_t k = 0; k < r.n; k++) {
        size_t rem = k, oa = 0, ob = 0;
        for (int i = nd - 1; i >= 0; i--) { size_t c = rem % (size_t)os[i]; rem /= (size_t)os[i]; oa += (c / (size_t)vb.shape[i]) * sa[i]; ob += (c % (size_t)vb.shape[i]) * sb[i]; }
        r.d[k] = va.d[oa] * vb.d[ob];
    }
    take(out, &r);
}
void nm_tensordot(const NnTensor *a, const NnTensor *b, const int *ax_a, const int *ax_b, int nax, NnTensor *out) {
    bool ua[NN_MAXDIM] = { false }, ub[NN_MAXDIM] = { false }; size_t K = 1;
    for (int i = 0; i < nax; i++) {
        if (ax_a[i] < 0 || ax_a[i] >= a->nd || ax_b[i] < 0 || ax_b[i] >= b->nd) nn_fail("tensordot: axis out of range");
        if (ua[ax_a[i]] || ub[ax_b[i]]) nn_fail("tensordot: an axis was listed twice");
        if (a->shape[ax_a[i]] != b->shape[ax_b[i]]) nn_fail("tensordot: contracted dimensions differ (%d vs %d)", a->shape[ax_a[i]], b->shape[ax_b[i]]);
        ua[ax_a[i]] = ub[ax_b[i]] = true; K *= (size_t)a->shape[ax_a[i]];
    }
    int pa[NN_MAXDIM], pb[NN_MAXDIM], os[NN_MAXDIM], ond = 0, na = 0, nb = 0; size_t M = 1, N = 1;
    for (int i = 0; i < a->nd; i++) if (!ua[i]) { pa[na++] = i; M *= (size_t)a->shape[i]; if (ond >= NN_MAXDIM) nn_fail("tensordot: result has too many dimensions"); os[ond++] = a->shape[i]; }
    for (int i = 0; i < nax; i++) pa[na++] = ax_a[i];
    for (int i = 0; i < nax; i++) pb[nb++] = ax_b[i];
    for (int i = 0; i < b->nd; i++) if (!ub[i]) { pb[nb++] = i; N *= (size_t)b->shape[i]; if (ond >= NN_MAXDIM) nn_fail("tensordot: result has too many dimensions"); os[ond++] = b->shape[i]; }
    NnTensor ta, tb, r; nt_init(&ta); nt_init(&tb); nt_init(&r);
    nt_permute(a, pa, &ta); nt_permute(b, pb, &tb); nt_alloc(&r, ond, os);
    if (M && N && K) nn_gemm_nn((int)M, (int)N, (int)K, ta.d, tb.d, r.d);
    nt_free(&ta); nt_free(&tb); take(out, &r);
}
void nm_bmatmul(const NnTensor *a, const NnTensor *b, NnTensor *out) {
    if (a->nd < 2 || b->nd < 2) nn_fail("bmatmul: both tensors need at least 2 dimensions (got %s and %s)", nt_shape_str(a->nd, a->shape), nt_shape_str(b->nd, b->shape));
    int M = a->shape[a->nd - 2], K = a->shape[a->nd - 1], K2 = b->shape[b->nd - 2], N = b->shape[b->nd - 1];
    if (K != K2) nn_fail("bmatmul: shapes %s and %s are not aligned", nt_shape_str(a->nd, a->shape), nt_shape_str(b->nd, b->shape));
    int ba = a->nd - 2, bb = b->nd - 2, nbd = ba > bb ? ba : bb; if (nbd + 2 > NN_MAXDIM) nn_fail("bmatmul: too many batch dimensions");
    int os[NN_MAXDIM]; size_t sa[NN_MAXDIM], sb[NN_MAXDIM], acA = 1, acB = 1, nbatch = 1;
    for (int i = nbd - 1; i >= 0; i--) {
        int ia = i - (nbd - ba), ib = i - (nbd - bb), da = ia >= 0 ? a->shape[ia] : 1, db = ib >= 0 ? b->shape[ib] : 1;
        if (da == db) os[i] = da; else if (da == 1) os[i] = db; else if (db == 1) os[i] = da;
        else nn_fail("bmatmul: batch dimensions of %s and %s cannot be broadcast", nt_shape_str(a->nd, a->shape), nt_shape_str(b->nd, b->shape));
        sa[i] = (ia >= 0 && da != 1) ? acA : 0; sb[i] = (ib >= 0 && db != 1) ? acB : 0;
        if (ia >= 0) acA *= (size_t)da; if (ib >= 0) acB *= (size_t)db; nbatch *= (size_t)os[i];
    }
    os[nbd] = M; os[nbd + 1] = N; NnTensor r; nt_init(&r); nt_alloc(&r, nbd + 2, os);
    int idx[NN_MAXDIM] = { 0 }; size_t oa = 0, ob = 0;
    for (size_t bi = 0; bi < nbatch; bi++) {
        if (M && N && K) nn_gemm_nn(M, N, K, a->d + oa * (size_t)M * (size_t)K, b->d + ob * (size_t)K * (size_t)N, r.d + bi * (size_t)M * (size_t)N);
        for (int i = nbd - 1; i >= 0; i--) { idx[i]++; oa += sa[i]; ob += sb[i]; if (idx[i] < os[i]) break; oa -= sa[i] * (size_t)os[i]; ob -= sb[i] * (size_t)os[i]; idx[i] = 0; }
    }
    take(out, &r);
}
void nm_cross(const NnTensor *a, const NnTensor *b, NnTensor *out) {
    if (a->nd < 1 || b->nd < 1 || a->shape[a->nd - 1] != 3 || b->shape[b->nd - 1] != 3) nn_fail("cross: the last dimension of both tensors must be 3");
    const NnTensor *ts[2] = { a, b }; int os[NN_MAXDIM]; int nd = bshape_n(ts, 2, os);
    if (nd < 0) nn_fail("cross: cannot broadcast shapes %s and %s", nt_shape_str(a->nd, a->shape), nt_shape_str(b->nd, b->shape));
    NnTensor x, y; nt_init(&x); nt_init(&y); nm_broadcast_to(a, nd, os, &x); nm_broadcast_to(b, nd, os, &y);
    NnTensor r; nt_init(&r); nt_resize(&r, nd, os);
    for (size_t i = 0; i < r.n; i += 3) {
        const double *p = x.d + i, *q = y.d + i; double *o = r.d + i;
        o[0] = p[1] * q[2] - p[2] * q[1]; o[1] = p[2] * q[0] - p[0] * q[2]; o[2] = p[0] * q[1] - p[1] * q[0];
    }
    nt_free(&x); nt_free(&y); take(out, &r);
}

/* ---- einsum: up to 8 operands; two-operand contractions without repeated labels run on GEMM, everything else on a generic loop nest ---- */
#define EIN_MAXOP 8
void nm_einsum(const char *spec, const NnTensor *const *ts, int nt, NnTensor *out) {
    char buf[256]; size_t bl = 0;
    for (const char *c = spec; *c; c++) if (*c != ' ' && *c != '\t') { if (bl >= sizeof buf - 1) nn_fail("einsum: subscript string is too long"); buf[bl++] = *c; }
    buf[bl] = 0; char *arrow = strstr(buf, "->"); if (arrow) { *arrow = 0; arrow += 2; }
    char inl[EIN_MAXOP][NN_MAXDIM + 2]; int nin = 0; char *tok = buf;
    for (;;) {
        char *comma = strchr(tok, ','); if (comma) *comma = 0;
        if (nin >= EIN_MAXOP) nn_fail("einsum: at most %d operands are supported", EIN_MAXOP);
        if (strlen(tok) > NN_MAXDIM) nn_fail("einsum: operand %d has more than %d subscripts", nin + 1, NN_MAXDIM);
        strcpy(inl[nin++], tok); if (!comma) break; tok = comma + 1;
    }
    if (nin != nt) nn_fail("einsum: the subscripts describe %d operand%s but %d tensor%s given", nin, nin == 1 ? "" : "s", nt, nt == 1 ? " was" : "s were");
    int lsz[128], cnt[128]; for (int i = 0; i < 128; i++) { lsz[i] = -1; cnt[i] = 0; }
    char first[128]; int nfirst = 0;
    for (int k = 0; k < nin; k++) {
        if ((int)strlen(inl[k]) != ts[k]->nd) nn_fail("einsum: operand %d has %d dimension%s but its subscripts '%s' name %d", k + 1, ts[k]->nd, ts[k]->nd == 1 ? "" : "s", inl[k], (int)strlen(inl[k]));
        for (int d = 0; d < ts[k]->nd; d++) {
            unsigned char c = (unsigned char)inl[k][d];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) nn_fail("einsum: invalid subscript '%c' (use letters; '...' is not supported)", c);
            int dim = ts[k]->shape[d];
            if (lsz[c] < 0) { lsz[c] = dim; first[nfirst++] = (char)c; }
            else if (lsz[c] != dim) { if (lsz[c] == 1) lsz[c] = dim; else if (dim != 1) nn_fail("einsum: subscript '%c' has inconsistent sizes (%d vs %d)", c, lsz[c], dim); }
            cnt[c]++;
        }
    }
    char outl[64]; int no = 0;
    if (arrow) {
        for (const char *c = arrow; *c; c++) {
            unsigned char u = (unsigned char)*c;
            if (!((u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z'))) nn_fail("einsum: invalid output subscript '%c'", u);
            if (lsz[u] < 0) nn_fail("einsum: output subscript '%c' does not appear in any input", u);
            for (int i = 0; i < no; i++) if (outl[i] == *c) nn_fail("einsum: output subscript '%c' is repeated", u);
            if (no >= NN_MAXDIM) nn_fail("einsum: output would have more than %d dimensions", NN_MAXDIM);
            outl[no++] = *c;
        }
    } else {
        for (int u = 'A'; u <= 'z'; u++) if (lsz[u] >= 0 && cnt[u] == 1) { if (no >= NN_MAXDIM) nn_fail("einsum: output would have more than %d dimensions", NN_MAXDIM); outl[no++] = (char)u; }
    }
    int osz[NN_MAXDIM]; for (int i = 0; i < no; i++) osz[i] = lsz[(unsigned char)outl[i]];
    bool inout[128] = { false }; for (int i = 0; i < no; i++) inout[(unsigned char)outl[i]] = true;

    /* ---- fast path: two operands, no repeated labels inside an operand, no summed-out single-operand labels, no size-1 broadcasting ---- */
    if (nt == 2) {
        bool ok = true; bool ina[128] = { false }, inb[128] = { false };
        for (int d = 0; d < ts[0]->nd && ok; d++) { unsigned char c = (unsigned char)inl[0][d]; if (ina[c]) ok = false; ina[c] = true; if (ts[0]->shape[d] != lsz[c]) ok = false; }
        for (int d = 0; d < ts[1]->nd && ok; d++) { unsigned char c = (unsigned char)inl[1][d]; if (inb[c]) ok = false; inb[c] = true; if (ts[1]->shape[d] != lsz[c]) ok = false; }
        for (int i = 0; i < nfirst && ok; i++) { unsigned char c = (unsigned char)first[i]; if (!inout[c] && !(ina[c] && inb[c])) ok = false; }
        if (ok) {
            char bl_[NN_MAXDIM], af[NN_MAXDIM], bf[NN_MAXDIM], cl[NN_MAXDIM]; int nb_ = 0, naf = 0, nbf = 0, nc = 0;
            for (int i = 0; i < no; i++) { unsigned char c = (unsigned char)outl[i]; if (ina[c] && inb[c]) bl_[nb_++] = (char)c; else if (ina[c]) af[naf++] = (char)c; else bf[nbf++] = (char)c; }
            for (int d = 0; d < ts[0]->nd; d++) { unsigned char c = (unsigned char)inl[0][d]; if (inb[c] && !inout[c]) cl[nc++] = (char)c; }
            int pa[NN_MAXDIM], pb[NN_MAXDIM], na = 0, nbp = 0;
            #define POS(k, c) ({ int _p = -1; for (int _d = 0; _d < ts[k]->nd; _d++) if (inl[k][_d] == (c)) _p = _d; _p; })
            for (int i = 0; i < nb_; i++) pa[na++] = POS(0, bl_[i]); for (int i = 0; i < naf; i++) pa[na++] = POS(0, af[i]); for (int i = 0; i < nc; i++) pa[na++] = POS(0, cl[i]);
            for (int i = 0; i < nb_; i++) pb[nbp++] = POS(1, bl_[i]); for (int i = 0; i < nc; i++) pb[nbp++] = POS(1, cl[i]); for (int i = 0; i < nbf; i++) pb[nbp++] = POS(1, bf[i]);
            #undef POS
            size_t B = 1, M = 1, K = 1, N = 1;
            for (int i = 0; i < nb_; i++) B *= (size_t)lsz[(unsigned char)bl_[i]]; for (int i = 0; i < naf; i++) M *= (size_t)lsz[(unsigned char)af[i]];
            for (int i = 0; i < nc; i++) K *= (size_t)lsz[(unsigned char)cl[i]]; for (int i = 0; i < nbf; i++) N *= (size_t)lsz[(unsigned char)bf[i]];
            NnTensor ta, tb, res; nt_init(&ta); nt_init(&tb); nt_init(&res); nt_permute(ts[0], pa, &ta); nt_permute(ts[1], pb, &tb);
            char rl[NN_MAXDIM]; int rn = 0, rs[NN_MAXDIM];
            for (int i = 0; i < nb_; i++) { rl[rn] = bl_[i]; rs[rn++] = lsz[(unsigned char)bl_[i]]; }
            for (int i = 0; i < naf; i++) { rl[rn] = af[i]; rs[rn++] = lsz[(unsigned char)af[i]]; }
            for (int i = 0; i < nbf; i++) { rl[rn] = bf[i]; rs[rn++] = lsz[(unsigned char)bf[i]]; }
            nt_alloc(&res, rn, rs);
            if (B && M && N && K) for (size_t bi = 0; bi < B; bi++) nn_gemm_nn((int)M, (int)N, (int)K, ta.d + bi * M * K, tb.d + bi * K * N, res.d + bi * M * N);
            nt_free(&ta); nt_free(&tb);
            int perm[NN_MAXDIM]; for (int j = 0; j < no; j++) for (int q = 0; q < rn; q++) if (rl[q] == outl[j]) perm[j] = q;
            if (no <= 1) take(out, &res); else { NnTensor pr; nt_init(&pr); nt_permute(&res, perm, &pr); nt_free(&res); take(out, &pr); }
            return;
        }
    }

    /* ---- generic path: one loop nest over [output labels..., contracted labels...] ---- */
    char loop[64]; int nl = 0; for (int i = 0; i < no; i++) loop[nl++] = outl[i];
    for (int i = 0; i < nfirst; i++) if (!inout[(unsigned char)first[i]]) loop[nl++] = first[i];
    size_t st[EIN_MAXOP][64], ost[64]; int lsize[64];
    for (int l = 0; l < nl; l++) lsize[l] = lsz[(unsigned char)loop[l]];
    for (int k = 0; k < nt; k++) {
        size_t ist[NN_MAXDIM], acc = 1; for (int d = ts[k]->nd - 1; d >= 0; d--) { ist[d] = acc; acc *= (size_t)ts[k]->shape[d]; }
        for (int l = 0; l < nl; l++) { st[k][l] = 0; for (int d = 0; d < ts[k]->nd; d++) if (inl[k][d] == loop[l] && !(ts[k]->shape[d] == 1 && lsize[l] > 1)) st[k][l] += ist[d]; }
    }
    { size_t acc = 1; for (int l = 0; l < nl; l++) ost[l] = 0; for (int l = no - 1; l >= 0; l--) { ost[l] = acc; acc *= (size_t)lsize[l]; } }
    NnTensor r; nt_init(&r); nt_alloc(&r, no, osz);
    size_t total = 1; for (int l = 0; l < nl; l++) total *= (size_t)lsize[l];
    int idx[64] = { 0 }; size_t off[EIN_MAXOP] = { 0 }, oo = 0;
    for (size_t t = 0; t < total; t++) {
        double p = 1; for (int k = 0; k < nt; k++) p *= ts[k]->d[off[k]];
        r.d[oo] += p;
        for (int l = nl - 1; l >= 0; l--) {
            idx[l]++; for (int k = 0; k < nt; k++) off[k] += st[k][l]; oo += ost[l];
            if (idx[l] < lsize[l]) break;
            for (int k = 0; k < nt; k++) off[k] -= st[k][l] * (size_t)lsize[l]; oo -= ost[l] * (size_t)lsize[l]; idx[l] = 0;
        }
    }
    take(out, &r);
}

/* ================================================================== statistics */
void nm_cov(const NnTensor *x, int ddof, NnTensor *out) {
    if (x->nd != 2) nn_fail("cov: expects a 2-D tensor [observations, variables] (got %s)", nt_shape_str(x->nd, x->shape));
    int N = x->shape[0], D = x->shape[1]; if (N - ddof <= 0) nn_fail("cov: needs more than %d observation%s (got %d)", ddof, ddof == 1 ? "" : "s", N);
    NnTensor c; nt_init(&c); nt_copy(&c, x);
    for (int j = 0; j < D; j++) { double m = 0; for (int i = 0; i < N; i++) m += c.d[(size_t)i * D + j]; m /= N; for (int i = 0; i < N; i++) c.d[(size_t)i * D + j] -= m; }
    int sh[2] = { D, D }; NnTensor r; nt_init(&r); nt_alloc(&r, 2, sh); nn_gemm_tn(D, D, N, c.d, c.d, r.d);
    for (size_t i = 0; i < r.n; i++) r.d[i] /= (double)(N - ddof);
    nt_free(&c); take(out, &r);
}
void nm_corrcoef(const NnTensor *x, NnTensor *out) {
    NnTensor c; nt_init(&c); nm_cov(x, 1, &c); int D = c.shape[0];
    for (int i = 0; i < D; i++) for (int j = 0; j < D; j++) if (i != j) {
        double v = sqrt(c.d[(size_t)i * D + i] * c.d[(size_t)j * D + j]); c.d[(size_t)i * D + j] = v > 0 ? c.d[(size_t)i * D + j] / v : NAN;
    }
    for (int i = 0; i < D; i++) c.d[(size_t)i * D + i] = c.d[(size_t)i * D + i] > 0 ? 1.0 : NAN;
    take(out, &c);
}
void nm_cdist(const NnTensor *a, const NnTensor *b, double p, NnTensor *out) {
    if (a->nd != 2 || b->nd != 2 || a->shape[1] != b->shape[1]) nn_fail("cdist: needs [N,D] and [M,D] tensors (got %s and %s)", nt_shape_str(a->nd, a->shape), nt_shape_str(b->nd, b->shape));
    if (p < 0) nn_fail("cdist: p must be >= 0");
    int N = a->shape[0], M = b->shape[0], D = a->shape[1], sh[2] = { N, M }; NnTensor r; nt_init(&r); nt_resize(&r, 2, sh);
    double *diff = nn_malloc(sizeof(double) * (size_t)(D ? D : 1));
    for (int i = 0; i < N; i++) for (int j = 0; j < M; j++) {
        for (int k = 0; k < D; k++) diff[k] = a->d[(size_t)i * D + k] - b->d[(size_t)j * D + k];
        r.d[(size_t)i * M + j] = lane_value(NMR_NORM, diff, (size_t)D, p);
    }
    free(diff); take(out, &r);
}
void nm_histogram(const NnTensor *x, int bins, double lo, double hi, NnTensor *out) {
    if (bins < 1) nn_fail("histogram: bins must be >= 1");
    if (!(hi >= lo) || !isfinite(lo) || !isfinite(hi)) nn_fail("histogram: needs a finite range with lo <= hi (got %g .. %g)", lo, hi);
    if (lo == hi) { lo -= 0.5; hi += 0.5; }
    int sh[1] = { bins }; NnTensor r; nt_init(&r); nt_alloc(&r, 1, sh); double w = (hi - lo) / bins;
    for (size_t i = 0; i < x->n; i++) {
        double v = x->d[i]; if (isnan(v) || v < lo || v > hi) continue;
        int b = (int)floor((v - lo) / w); if (b >= bins) b = bins - 1; if (b < 0) b = 0; r.d[b] += 1;
    }
    take(out, &r);
}
void nm_bincount(const NnTensor *x, int minlength, NnTensor *out) {
    long mx = -1;
    for (size_t i = 0; i < x->n; i++) {
        double v = x->d[i]; long c = (long)v;
        if (v != (double)c || c < 0) nn_fail("bincount: values must be non-negative integers (got %g at position %zu)", v, i);
        if (c > 100000000L) nn_fail("bincount: value %ld is too large", c);
        if (c > mx) mx = c;
    }
    long len = mx + 1 > minlength ? mx + 1 : minlength; int sh[1] = { (int)len }; NnTensor r; nt_init(&r); nt_alloc(&r, 1, sh);
    for (size_t i = 0; i < x->n; i++) r.d[(long)x->d[i]] += 1;
    take(out, &r);
}
void nm_convolve(const NnTensor *a, const NnTensor *v, int mode, NnTensor *out) {
    if (a->nd != 1 || v->nd != 1) nn_fail("convolve: needs two 1-D tensors");
    long na = a->shape[0], nv = v->shape[0]; if (na == 0 || nv == 0) nn_fail("convolve: inputs must not be empty");
    long full = na + nv - 1, lo = na < nv ? na : nv, hi = na < nv ? nv : na, start = 0, len = full;
    if (mode == NMV_SAME) { start = (lo - 1) / 2; len = hi; } else if (mode == NMV_VALID) { start = lo - 1; len = hi - lo + 1; }
    int sh[1] = { (int)len }; NnTensor r; nt_init(&r); nt_alloc(&r, 1, sh);
    for (long k = 0; k < len; k++) {
        long t = k + start; double s = 0; long i0 = t - (nv - 1) > 0 ? t - (nv - 1) : 0, i1 = t < na - 1 ? t : na - 1;
        for (long i = i0; i <= i1; i++) s += a->d[i] * v->d[t - i];
        r.d[k] = s;
    }
    take(out, &r);
}
