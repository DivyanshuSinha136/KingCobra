/* std.nn tensor math: broadcasting, reductions, matmul, permute, concat, gather, softmax */
#include "nn_internal.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>

int nt_norm_axis(int axis, int nd, const char *fn) {
    int a = axis < 0 ? axis + nd : axis;
    if (a < 0 || a >= nd) nn_fail("%s: axis %d is out of range for a tensor with %d dimension%s", fn, axis, nd, nd == 1 ? "" : "s");
    return a;
}
static void take(NnTensor *out, NnTensor *r) { nt_free(out); *out = *r; }

/* ---- broadcasting binary ops ---- */
static int bshape(const NnTensor *a, const NnTensor *b, int *os) {
    int nd = a->nd > b->nd ? a->nd : b->nd;
    for (int i = 0; i < nd; i++) {
        int ia = i - (nd - a->nd), ib = i - (nd - b->nd);
        int da = ia >= 0 ? a->shape[ia] : 1, db = ib >= 0 ? b->shape[ib] : 1;
        if (da == db) os[i] = da; else if (da == 1) os[i] = db; else if (db == 1) os[i] = da; else return -1;
    }
    return nd;
}
static inline nn_real apply_bin(int op, nn_real x, nn_real y) {
    switch (op) {
    case NTB_ADD: return x + y; case NTB_SUB: return x - y; case NTB_MUL: return x * y; case NTB_DIV: return x / y;
    case NTB_POW: return pow(x, y); case NTB_MAX: return x > y ? x : y; case NTB_MIN: return x < y ? x : y;
    case NTB_ATAN2: return atan2(x, y); case NTB_HYPOT: return hypot(x, y);
    case NTB_MOD: { nn_real m = fmod(x, y); if (m != 0 && ((m < 0) != (y < 0))) m += y; return m; }   /* result takes the sign of the divisor */
    case NTB_FLOORDIV: return floor(x / y);
    case NTB_EQ: return x == y; case NTB_NE: return x != y; case NTB_LT: return x < y; case NTB_LE: return x <= y; case NTB_GT: return x > y; case NTB_GE: return x >= y;
    case NTB_AND: return (x != 0) && (y != 0); case NTB_OR: return (x != 0) || (y != 0); case NTB_XOR: return (x != 0) != (y != 0);
    case NTB_LOGADDEXP: { if (x == y) return x + 0.6931471805599453; nn_real m = x > y ? x : y, d = fabs(x - y); return isnan(d) ? NAN : m + log1p(exp(-d)); }
    }
    return 0;
}
void nt_binop(int op, const NnTensor *a, const NnTensor *b, NnTensor *out) {
    int os[NN_MAXDIM]; int nd = bshape(a, b, os);
    if (nd < 0) nn_fail("cannot broadcast shapes %s and %s", nt_shape_str(a->nd, a->shape), nt_shape_str(b->nd, b->shape));
    NnTensor r; nt_init(&r); nt_resize(&r, nd, os);
    if (nt_same_shape(a, b)) { for (size_t i = 0; i < r.n; i++) r.d[i] = apply_bin(op, a->d[i], b->d[i]); take(out, &r); return; }
    if (b->n == 1 && nd == a->nd) { nn_real y = b->d[0]; for (size_t i = 0; i < r.n; i++) r.d[i] = apply_bin(op, a->d[i], y); take(out, &r); return; }
    size_t sa[NN_MAXDIM], sb[NN_MAXDIM], acc_a = 1, acc_b = 1;
    for (int i = nd - 1; i >= 0; i--) {
        int ia = i - (nd - a->nd), ib = i - (nd - b->nd);
        sa[i] = (ia >= 0 && a->shape[ia] != 1) ? acc_a : 0; sb[i] = (ib >= 0 && b->shape[ib] != 1) ? acc_b : 0;
        if (ia >= 0) acc_a *= (size_t)a->shape[ia]; if (ib >= 0) acc_b *= (size_t)b->shape[ib];
    }
    int idx[NN_MAXDIM] = { 0 }; size_t oa = 0, ob = 0;
    for (size_t k = 0; k < r.n; k++) {
        r.d[k] = apply_bin(op, a->d[oa], b->d[ob]);
        for (int i = nd - 1; i >= 0; i--) {
            idx[i]++; oa += sa[i]; ob += sb[i];
            if (idx[i] < os[i]) break;
            oa -= sa[i] * (size_t)os[i]; ob -= sb[i] * (size_t)os[i]; idx[i] = 0;
        }
    }
    take(out, &r);
}

void nt_unary(int op, const NnTensor *a, NnTensor *out) {
    NnTensor r; nt_init(&r); nt_resize(&r, a->nd, a->shape);
    for (size_t i = 0; i < a->n; i++) {
        nn_real x = a->d[i], y = 0;
        switch (op) {
        case NTU_NEG: y = -x; break; case NTU_ABS: y = fabs(x); break; case NTU_EXP: y = exp(x); break;
        case NTU_LOG: y = log(x); break; case NTU_SQRT: y = sqrt(x); break; case NTU_SQUARE: y = x * x; break;
        case NTU_TANH: y = tanh(x); break; case NTU_SIGMOID: y = x >= 0 ? 1.0 / (1.0 + exp(-x)) : exp(x) / (1.0 + exp(x)); break;
        case NTU_RELU: y = x > 0 ? x : 0; break; case NTU_SIGN: y = (x > 0) - (x < 0); break;
        case NTU_FLOOR: y = floor(x); break; case NTU_CEIL: y = ceil(x); break; case NTU_ROUND: y = round(x); break;
        case NTU_SIN: y = sin(x); break; case NTU_COS: y = cos(x); break;
        }
        r.d[i] = y;
    }
    take(out, &r);
}

/* ---- reductions ---- */
void nt_reduce(int op, const NnTensor *a, int axis, bool keepdims, NnTensor *out) {
    size_t outer, len, inner; int os[NN_MAXDIM], ond = 0;
    if (axis == NT_AXIS_ALL) {
        outer = 1; len = a->n; inner = 1;
        if (keepdims) { ond = a->nd; for (int i = 0; i < ond; i++) os[i] = 1; }
    } else {
        outer = 1; inner = 1; for (int i = 0; i < axis; i++) outer *= (size_t)a->shape[i]; for (int i = axis + 1; i < a->nd; i++) inner *= (size_t)a->shape[i];
        len = (size_t)a->shape[axis];
        for (int i = 0; i < a->nd; i++) { if (i == axis) { if (keepdims) os[ond++] = 1; } else os[ond++] = a->shape[i]; }
    }
    if (len == 0 && op != NTR_SUM && op != NTR_PROD) nn_fail("cannot reduce over an empty axis");
    NnTensor r; nt_init(&r); nt_resize(&r, ond, os);
    for (size_t o = 0; o < outer; o++) for (size_t in = 0; in < inner; in++) {
        const nn_real *p = a->d + o * len * inner + in; size_t st = inner; nn_real acc = 0; size_t arg = 0;
        switch (op) {
        case NTR_SUM: case NTR_MEAN: for (size_t k = 0; k < len; k++) acc += p[k * st]; if (op == NTR_MEAN) acc /= (nn_real)len; break;
        case NTR_PROD: acc = 1; for (size_t k = 0; k < len; k++) acc *= p[k * st]; break;
        case NTR_MAX: case NTR_ARGMAX: acc = p[0]; for (size_t k = 1; k < len; k++) if (p[k * st] > acc) { acc = p[k * st]; arg = k; } if (op == NTR_ARGMAX) acc = (nn_real)arg; break;
        case NTR_MIN: case NTR_ARGMIN: acc = p[0]; for (size_t k = 1; k < len; k++) if (p[k * st] < acc) { acc = p[k * st]; arg = k; } if (op == NTR_ARGMIN) acc = (nn_real)arg; break;
        case NTR_VAR: case NTR_STD: {
            nn_real m = 0; for (size_t k = 0; k < len; k++) m += p[k * st]; m /= (nn_real)len;
            for (size_t k = 0; k < len; k++) { nn_real d = p[k * st] - m; acc += d * d; } acc /= (nn_real)len; if (op == NTR_STD) acc = sqrt(acc); break; }
        }
        r.d[o * inner + in] = acc;
    }
    take(out, &r);
}

/* ---- matmul (2-D, vector forms, [..,M,K]x[K,N], batched [B,M,K]x[B,K,N]) ---- */
void nt_matmul(const NnTensor *a, const NnTensor *b, NnTensor *out) {
    NnTensor r; nt_init(&r);
    if (a->nd == 0 || b->nd == 0) nn_fail("matmul needs tensors with at least 1 dimension");
    if (a->nd == 1 && b->nd == 1) {
        if (a->shape[0] != b->shape[0]) nn_fail("matmul: dot product length mismatch (%d vs %d)", a->shape[0], b->shape[0]);
        nt_resize(&r, 0, NULL); nn_real s = 0; for (int i = 0; i < a->shape[0]; i++) s += a->d[i] * b->d[i]; r.d[0] = s; take(out, &r); return;
    }
    if (b->nd == 1) {                                            /* [...,K] x [K] -> [...] */
        int K = a->shape[a->nd - 1]; if (K != b->shape[0]) nn_fail("matmul: shapes %s and %s are not aligned", nt_shape_str(a->nd, a->shape), nt_shape_str(b->nd, b->shape));
        nt_resize(&r, a->nd - 1, a->shape); size_t rows = r.n; for (size_t i = 0; i < rows; i++) { nn_real s = 0; for (int k = 0; k < K; k++) s += a->d[i * (size_t)K + k] * b->d[k]; r.d[i] = s; }
        take(out, &r); return;
    }
    if (a->nd == 1) {                                            /* [K] x [K,N] -> [N] */
        if (a->shape[0] != b->shape[0]) nn_fail("matmul: shapes %s and %s are not aligned", nt_shape_str(a->nd, a->shape), nt_shape_str(b->nd, b->shape));
        int sh[1] = { b->shape[1] }; nt_alloc(&r, 1, sh); nn_gemm_nn(1, b->shape[1], a->shape[0], a->d, b->d, r.d); take(out, &r); return;
    }
    if (b->nd == 2) {
        int K = a->shape[a->nd - 1], N = b->shape[1]; if (K != b->shape[0]) nn_fail("matmul: shapes %s and %s are not aligned", nt_shape_str(a->nd, a->shape), nt_shape_str(b->nd, b->shape));
        int sh[NN_MAXDIM]; for (int i = 0; i < a->nd - 1; i++) sh[i] = a->shape[i]; sh[a->nd - 1] = N;
        nt_alloc(&r, a->nd, sh); int M = K ? (int)(a->n / (size_t)K) : (int)nt_numel(a->nd - 1, a->shape);
        nn_gemm_nn(M, N, K, a->d, b->d, r.d); take(out, &r); return;
    }
    if (a->nd == 3 && b->nd == 3) {
        int B = a->shape[0], M = a->shape[1], K = a->shape[2], N = b->shape[2];
        if (b->shape[0] != B || b->shape[1] != K) nn_fail("matmul: batched shapes %s and %s are not aligned", nt_shape_str(a->nd, a->shape), nt_shape_str(b->nd, b->shape));
        int sh[3] = { B, M, N }; nt_alloc(&r, 3, sh);
        for (int i = 0; i < B; i++) nn_gemm_nn(M, N, K, a->d + (size_t)i * M * K, b->d + (size_t)i * K * N, r.d + (size_t)i * M * N);
        take(out, &r); return;
    }
    nn_fail("matmul: unsupported shapes %s and %s", nt_shape_str(a->nd, a->shape), nt_shape_str(b->nd, b->shape));
}

/* ---- permute ---- */
void nt_permute(const NnTensor *a, const int *axes, NnTensor *out) {
    int nd = a->nd, os[NN_MAXDIM]; bool seen[NN_MAXDIM] = { false };
    for (int i = 0; i < nd; i++) { int ax = axes[i]; if (ax < 0 || ax >= nd || seen[ax]) nn_fail("permute: axes must be a permutation of 0..%d", nd - 1); seen[ax] = true; os[i] = a->shape[ax]; }
    size_t ist[NN_MAXDIM], acc = 1; for (int i = nd - 1; i >= 0; i--) { ist[i] = acc; acc *= (size_t)a->shape[i]; }
    NnTensor r; nt_init(&r); nt_resize(&r, nd, os); size_t st[NN_MAXDIM]; for (int i = 0; i < nd; i++) st[i] = ist[axes[i]];
    int idx[NN_MAXDIM] = { 0 }; size_t off = 0;
    for (size_t k = 0; k < r.n; k++) {
        r.d[k] = a->d[off];
        for (int i = nd - 1; i >= 0; i--) { idx[i]++; off += st[i]; if (idx[i] < os[i]) break; off -= st[i] * (size_t)os[i]; idx[i] = 0; }
    }
    take(out, &r);
}

/* ---- concat / gather ---- */
void nt_concat(const NnTensor *const *ts, int n, int axis, NnTensor *out) {
    if (n < 1) nn_fail("concat needs at least one tensor");
    int nd = ts[0]->nd; if (nd == 0) nn_fail("concat needs tensors with at least 1 dimension");
    int os[NN_MAXDIM]; memcpy(os, ts[0]->shape, sizeof(int) * (size_t)nd); os[axis] = 0;
    for (int t = 0; t < n; t++) {
        if (ts[t]->nd != nd) nn_fail("concat: tensors must have the same number of dimensions");
        for (int i = 0; i < nd; i++) if (i != axis && ts[t]->shape[i] != ts[0]->shape[i]) nn_fail("concat: shape %s does not match %s outside axis %d", nt_shape_str(ts[t]->nd, ts[t]->shape), nt_shape_str(ts[0]->nd, ts[0]->shape), axis);
        os[axis] += ts[t]->shape[axis];
    }
    NnTensor r; nt_init(&r); nt_resize(&r, nd, os);
    size_t outer = 1, inner = 1; for (int i = 0; i < axis; i++) outer *= (size_t)os[i]; for (int i = axis + 1; i < nd; i++) inner *= (size_t)os[i];
    size_t pos = 0, total = (size_t)os[axis] * inner;
    for (int t = 0; t < n; t++) {
        size_t chunk = (size_t)ts[t]->shape[axis] * inner;
        for (size_t o = 0; o < outer; o++) if (chunk) memcpy(r.d + o * total + pos, ts[t]->d + o * chunk, sizeof(nn_real) * chunk);
        pos += chunk;
    }
    take(out, &r);
}
void nt_rows(const NnTensor *a, const int64_t *idx, int64_t n, NnTensor *out) {
    if (a->nd < 1) nn_fail("rows: tensor must have at least 1 dimension");
    int os[NN_MAXDIM]; memcpy(os, a->shape, sizeof(int) * (size_t)a->nd); os[0] = (int)n;
    size_t rowlen = a->shape[0] ? a->n / (size_t)a->shape[0] : 0;
    NnTensor r; nt_init(&r); nt_resize(&r, a->nd, os);
    for (int64_t i = 0; i < n; i++) {
        if (idx[i] < 0 || idx[i] >= a->shape[0]) nn_fail("rows: index %lld is out of range (0..%d)", (long long)idx[i], a->shape[0] - 1);
        if (rowlen) memcpy(r.d + (size_t)i * rowlen, a->d + (size_t)idx[i] * rowlen, sizeof(nn_real) * rowlen);
    }
    take(out, &r);
}

/* ---- softmax along any axis ---- */
void nt_softmax_axis(const NnTensor *a, int axis, NnTensor *out) {
    size_t outer = 1, inner = 1, len = (size_t)a->shape[axis];
    for (int i = 0; i < axis; i++) outer *= (size_t)a->shape[i]; for (int i = axis + 1; i < a->nd; i++) inner *= (size_t)a->shape[i];
    NnTensor r; nt_init(&r); nt_resize(&r, a->nd, a->shape);
    for (size_t o = 0; o < outer; o++) for (size_t in = 0; in < inner; in++) {
        const nn_real *p = a->d + o * len * inner + in; nn_real *q = r.d + o * len * inner + in; nn_real m = -DBL_MAX, s = 0;
        for (size_t k = 0; k < len; k++) if (p[k * inner] > m) m = p[k * inner];
        for (size_t k = 0; k < len; k++) { q[k * inner] = exp(p[k * inner] - m); s += q[k * inner]; }
        for (size_t k = 0; k < len; k++) q[k * inner] /= s;
    }
    take(out, &r);
}
