/* std.nn script API: tensors, math, data preparation, metrics */
#include "nn_glue.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define FN(name) static KcValue name(KcInterp *ip, KcValue *a, int n, KcPos pos)
static NnTensor *newt(void) { NnTensor *t = nn_malloc(sizeof *t); nt_init(t); return t; }

/* ------------------------------------------------------------------ creation */
static void resolve_shape(int *shape, int nd, size_t total, const char *fn) {          /* resolves a single -1 */
    int unk = -1; size_t known = 1; for (int i = 0; i < nd; i++) { if (shape[i] == -1) { if (unk >= 0) NNERR("nn.%s: only one dimension can be -1", fn); unk = i; } else known *= (size_t)shape[i]; }
    if (unk >= 0) { if (known == 0 || total % known) NNERR("nn.%s: cannot reshape %zu values into %s", fn, total, nt_shape_str(nd, shape)); shape[unk] = (int)(total / known); }
    else if (known != total) NNERR("nn.%s: cannot reshape %zu values into %s", fn, total, nt_shape_str(nd, shape));
}
FN(f_tensor) {
    NNAPI(); need_n(n, 1, 2, "tensor"); NnTensor *t = newt();
    if (!nn_value_to_tensor(a[0], t, "tensor")) { nt_delete(t); NNERR("nn.tensor: expected a number, a nested array of numbers or a tensor (got %s)", kc_type_name(a[0])); }
    if (n == 2) { int sh[NN_MAXDIM]; int nd = arg_shape(a, n, 1, sh, "tensor"); resolve_shape(sh, nd, t->n, "tensor"); t->nd = nd; for (int i = 0; i < NN_MAXDIM; i++) t->shape[i] = i < nd ? sh[i] : 0; }
    return nn_ret_tensor(t);
}
FN(f_zeros) { NNAPI(); need_n(n, 1, 1, "zeros"); int sh[NN_MAXDIM]; int nd = arg_shape(a, n, 0, sh, "zeros"); for (int i = 0; i < nd; i++) if (sh[i] < 0) NNERR("nn.zeros: dimensions must be >= 0"); return nn_ret_tensor(nt_new(nd, sh)); }
FN(f_ones) { NNAPI(); need_n(n, 1, 1, "ones"); int sh[NN_MAXDIM]; int nd = arg_shape(a, n, 0, sh, "ones"); for (int i = 0; i < nd; i++) if (sh[i] < 0) NNERR("nn.ones: dimensions must be >= 0"); NnTensor *t = nt_new(nd, sh); for (size_t i = 0; i < t->n; i++) t->d[i] = 1; return nn_ret_tensor(t); }
FN(f_full) { NNAPI(); need_n(n, 2, 2, "full"); int sh[NN_MAXDIM]; int nd = arg_shape(a, n, 0, sh, "full"); double v = arg_num(a, n, 1, "full"); for (int i = 0; i < nd; i++) if (sh[i] < 0) NNERR("nn.full: dimensions must be >= 0"); NnTensor *t = nt_new(nd, sh); for (size_t i = 0; i < t->n; i++) t->d[i] = v; return nn_ret_tensor(t); }
FN(f_rand) { NNAPI(); need_n(n, 1, 1, "rand"); int sh[NN_MAXDIM]; int nd = arg_shape(a, n, 0, sh, "rand"); NnTensor *t = nt_new(nd, sh); for (size_t i = 0; i < t->n; i++) t->d[i] = nn_rand(); return nn_ret_tensor(t); }
FN(f_randn) { NNAPI(); need_n(n, 1, 2, "randn"); int sh[NN_MAXDIM]; int nd = arg_shape(a, n, 0, sh, "randn"); double sd = n > 1 ? arg_num(a, n, 1, "randn") : 1.0; NnTensor *t = nt_new(nd, sh); for (size_t i = 0; i < t->n; i++) t->d[i] = sd * nn_randn(); return nn_ret_tensor(t); }
FN(f_arange) {
    NNAPI(); need_n(n, 1, 3, "arange"); double s = 0, e, st = 1; if (n == 1) e = arg_num(a, n, 0, "arange"); else { s = arg_num(a, n, 0, "arange"); e = arg_num(a, n, 1, "arange"); if (n == 3) st = arg_num(a, n, 2, "arange"); }
    if (st == 0) NNERR("nn.arange: step must not be 0"); double cnt = ceil((e - s) / st); if (cnt < 0) cnt = 0; if (cnt > 2e9) NNERR("nn.arange: too many elements");
    int sh[1] = { (int)cnt }; NnTensor *t = nt_new(1, sh); for (int i = 0; i < sh[0]; i++) t->d[i] = s + st * i; return nn_ret_tensor(t);
}
FN(f_linspace) {
    NNAPI(); need_n(n, 3, 3, "linspace"); double s = arg_num(a, n, 0, "linspace"), e = arg_num(a, n, 1, "linspace"); int64_t c = arg_int(a, n, 2, "linspace"); if (c < 0 || c > 2000000000) NNERR("nn.linspace: bad count");
    int sh[1] = { (int)c }; NnTensor *t = nt_new(1, sh); for (int64_t i = 0; i < c; i++) t->d[i] = c == 1 ? s : s + (e - s) * (double)i / (double)(c - 1); return nn_ret_tensor(t);
}
FN(f_eye) { NNAPI(); need_n(n, 1, 1, "eye"); int64_t k = arg_int(a, n, 0, "eye"); if (k < 0 || k > 46000) NNERR("nn.eye: bad size"); int sh[2] = { (int)k, (int)k }; NnTensor *t = nt_new(2, sh); for (int i = 0; i < k; i++) t->d[(size_t)i * (size_t)k + (size_t)i] = 1; return nn_ret_tensor(t); }

FN(f_randint) {
    NNAPI(); need_n(n, 3, 3, "randint"); int64_t lo = arg_int(a, n, 0, "randint"), hi = arg_int(a, n, 1, "randint"); if (hi <= lo) NNERR("nn.randint: high (%lld) must be greater than low (%lld)", (long long)hi, (long long)lo);
    int sh[NN_MAXDIM]; int nd = arg_shape(a, n, 2, sh, "randint"); NnTensor *t = nt_new(nd, sh); for (size_t i = 0; i < t->n; i++) t->d[i] = (double)(lo + nn_randint(hi - lo)); return nn_ret_tensor(t);
}
/* nn.sample(logits [, temperature [, top_k]]) -> index drawn from softmax(logits / temperature), optionally restricted to the top_k logits */
FN(f_sample) {
    NNAPI(); need_n(n, 1, 3, "sample"); TArg x; NnTensor *t = targ_get(&x, a, n, 0, "sample"); double temp = n > 1 ? arg_num(a, n, 1, "sample") : 1.0; int64_t topk = n > 2 ? arg_int(a, n, 2, "sample") : 0;
    if (t->n < 1) { targ_free(&x); NNERR("nn.sample: needs at least one logit"); } if (!(temp > 0)) { targ_free(&x); NNERR("nn.sample: temperature must be > 0"); } if (topk < 0) { targ_free(&x); NNERR("nn.sample: top_k must be >= 0"); }
    size_t N = t->n; double *p = nn_malloc(sizeof(double) * N); double thr = -INFINITY;
    if (topk > 0 && (size_t)topk < N) { double *tmp = nn_malloc(sizeof(double) * N); memcpy(tmp, t->d, sizeof(double) * N); for (int64_t k = 0; k < topk; k++) { size_t b = (size_t)k; for (size_t i = (size_t)k + 1; i < N; i++) if (tmp[i] > tmp[b]) b = i; double sw = tmp[k]; tmp[k] = tmp[b]; tmp[b] = sw; } thr = tmp[topk - 1]; free(tmp); }
    double m = -INFINITY; for (size_t i = 0; i < N; i++) if (t->d[i] >= thr && t->d[i] > m) m = t->d[i];
    double sum = 0; for (size_t i = 0; i < N; i++) { p[i] = t->d[i] >= thr ? exp((t->d[i] - m) / temp) : 0.0; sum += p[i]; }
    double u = nn_rand() * sum, acc = 0; size_t pick = N - 1; for (size_t i = 0; i < N; i++) { acc += p[i]; if (u < acc && p[i] > 0) { pick = i; break; } }
    free(p); targ_free(&x); return kc_int((int64_t)pick);
}
/* ------------------------------------------------------------------ inspection */
FN(f_shape) { NNAPI(); need_n(n, 1, 1, "shape"); TArg ta; NnTensor *t = targ_get(&ta, a, n, 0, "shape"); KcValue r = kc_array_new(); for (int i = 0; i < t->nd; i++) kc_array_push(r, kc_int(t->shape[i])); targ_free(&ta); return r; }
FN(f_size) { NNAPI(); need_n(n, 1, 1, "size"); TArg ta; NnTensor *t = targ_get(&ta, a, n, 0, "size"); int64_t s = (int64_t)t->n; targ_free(&ta); return kc_int(s); }
FN(f_ndim) { NNAPI(); need_n(n, 1, 1, "ndim"); TArg ta; NnTensor *t = targ_get(&ta, a, n, 0, "ndim"); int64_t s = t->nd; targ_free(&ta); return kc_int(s); }
FN(f_to_array) { NNAPI(); need_n(n, 1, 1, "to_array"); TArg ta; NnTensor *t = targ_get(&ta, a, n, 0, "to_array"); KcValue r = nn_to_nested(t); targ_free(&ta); return r; }
FN(f_item) { NNAPI(); need_n(n, 1, 1, "item"); TArg ta; NnTensor *t = targ_get(&ta, a, n, 0, "item"); if (t->n != 1) { targ_free(&ta); NNERR("nn.item: tensor has %zu values; item() needs exactly one", t->n); } double v = t->d[0]; targ_free(&ta); return kc_float(v); }
static size_t offset_of(NnTensor *t, KcValue *a, int n, int first, const char *fn) {
    if (n - first != t->nd) NNERR("nn.%s: the tensor has %d dimension%s but %d index%s given", fn, t->nd, t->nd == 1 ? "" : "s", n - first, n - first == 1 ? " was" : "es were");
    size_t off = 0; for (int i = 0; i < t->nd; i++) { int64_t ix = arg_int(a, n, first + i, fn); if (ix < 0) ix += t->shape[i]; if (ix < 0 || ix >= t->shape[i]) NNERR("nn.%s: index %lld is out of range for dimension %d (size %d)", fn, (long long)arg_int(a, n, first + i, fn), i, t->shape[i]); off = off * (size_t)t->shape[i] + (size_t)ix; }
    return off;
}
FN(f_get) { NNAPI(); NnTensor *t = nnh_need(a, n, 0, NNH_TENSOR, "get"); return kc_float(t->d[offset_of(t, a, n, 1, "get")]); }
FN(f_set) { NNAPI(); NnTensor *t = nnh_need(a, n, 0, NNH_TENSOR, "set"); double v = arg_num(a, n, 1, "set"); t->d[offset_of(t, a, n, 2, "set")] = v; return kc_null(); }
FN(f_copy) { NNAPI(); need_n(n, 1, 1, "copy"); TArg ta; NnTensor *t = targ_get(&ta, a, n, 0, "copy"); NnTensor *r = newt(); nt_copy(r, t); targ_free(&ta); return nn_ret_tensor(r); }
FN(f_free) {
    NNAPI(); need_n(n, 1, 1, "free"); if (a[0].type != VAL_HANDLE || a[0].as.handle->kind != H_NN) NNERR("nn.free: argument must be an nn tensor");
    KcHandle *h = a[0].as.handle; if (h->extra != NNH_TENSOR) NNERR("nn.free: only tensors can be freed (models, losses and optimizers live until the program ends)");
    if (h->ptr) { nt_delete(h->ptr); h->ptr = NULL; } return kc_null();
}
FN(f_reshape) {
    NNAPI(); need_n(n, 2, 2, "reshape"); TArg ta; NnTensor *t = targ_get(&ta, a, n, 0, "reshape"); int sh[NN_MAXDIM]; int nd = arg_shape(a, n, 1, sh, "reshape"); resolve_shape(sh, nd, t->n, "reshape");
    NnTensor *r = newt(); nt_copy(r, t); r->nd = nd; for (int i = 0; i < NN_MAXDIM; i++) r->shape[i] = i < nd ? sh[i] : 0; targ_free(&ta); return nn_ret_tensor(r);
}
FN(f_transpose) {
    NNAPI(); need_n(n, 1, 2, "transpose"); TArg ta; NnTensor *t = targ_get(&ta, a, n, 0, "transpose"); int ax[NN_MAXDIM]; NnTensor *r = newt();
    if (n == 2) { int nd = arg_shape(a, n, 1, ax, "transpose"); if (nd != t->nd) { targ_free(&ta); nt_delete(r); NNERR("nn.transpose: axes must list all %d dimensions", t->nd); } }
    else { if (t->nd < 2) { targ_free(&ta); nt_delete(r); NNERR("nn.transpose: needs at least 2 dimensions"); } for (int i = 0; i < t->nd; i++) ax[i] = i; ax[t->nd - 1] = t->nd - 2; ax[t->nd - 2] = t->nd - 1; }
    nt_permute(t, ax, r); targ_free(&ta); return nn_ret_tensor(r);
}
FN(f_matmul) { NNAPI(); need_n(n, 2, 2, "matmul"); TArg x, y; NnTensor *p = targ_get(&x, a, n, 0, "matmul"), *q = targ_get(&y, a, n, 1, "matmul"); NnTensor *r = newt(); nt_init(r); nt_matmul(p, q, r); targ_free(&x); targ_free(&y); return nn_ret_tensor(r); }

/* ------------------------------------------------------------------ elementwise */
#define BIN(fname, opname, op) FN(fname) { NNAPI(); need_n(n, 2, 2, opname); TArg x, y; NnTensor *p = targ_get(&x, a, n, 0, opname), *q = targ_get(&y, a, n, 1, opname); NnTensor *r = newt(); nt_binop(op, p, q, r); targ_free(&x); targ_free(&y); return nn_ret_tensor(r); }
BIN(f_add, "add", NTB_ADD) BIN(f_sub, "sub", NTB_SUB) BIN(f_mul, "mul", NTB_MUL) BIN(f_div, "div", NTB_DIV) BIN(f_pow, "pow", NTB_POW) BIN(f_maximum, "maximum", NTB_MAX) BIN(f_minimum, "minimum", NTB_MIN)
#define UN(fname, opname, op) FN(fname) { NNAPI(); need_n(n, 1, 1, opname); TArg x; NnTensor *p = targ_get(&x, a, n, 0, opname); NnTensor *r = newt(); nt_unary(op, p, r); targ_free(&x); return nn_ret_tensor(r); }
UN(f_neg, "neg", NTU_NEG) UN(f_abs, "abs", NTU_ABS) UN(f_exp, "exp", NTU_EXP) UN(f_log, "log", NTU_LOG) UN(f_sqrt, "sqrt", NTU_SQRT) UN(f_square, "square", NTU_SQUARE)
UN(f_sign, "sign", NTU_SIGN) UN(f_floor, "floor", NTU_FLOOR) UN(f_sin, "sin", NTU_SIN) UN(f_cos, "cos", NTU_COS) UN(f_ceil, "ceil", NTU_CEIL) UN(f_round, "round", NTU_ROUND)
FN(f_clip) {
    NNAPI(); need_n(n, 3, 3, "clip"); TArg x; NnTensor *p = targ_get(&x, a, n, 0, "clip"); double lo = arg_num(a, n, 1, "clip"), hi = arg_num(a, n, 2, "clip"); if (lo > hi) { targ_free(&x); NNERR("nn.clip: min must be <= max"); }
    NnTensor *r = newt(); nt_resize(r, p->nd, p->shape); for (size_t i = 0; i < p->n; i++) r->d[i] = p->d[i] < lo ? lo : (p->d[i] > hi ? hi : p->d[i]); targ_free(&x); return nn_ret_tensor(r);
}
static KcValue reduce_api(KcValue *a, int n, KcPos pos, int op, const char *fn) {
    NN_POS = &pos; need_n(n, 1, 3, fn); TArg x; NnTensor *p = targ_get(&x, a, n, 0, fn); int axis = NT_AXIS_ALL; bool keep = false;
    if (n > 1 && a[1].type != VAL_NULL) axis = nt_norm_axis((int)arg_int(a, n, 1, fn), p->nd, fn);
    if (n > 2) keep = kc_truthy(a[2]); if (axis == NT_AXIS_ALL && p->n == 0 && op != NTR_SUM && op != NTR_PROD) { targ_free(&x); NNERR("nn.%s: cannot reduce an empty tensor", fn); }
    NnTensor *r = newt(); nt_reduce(op, p, axis, keep, r); targ_free(&x); return nn_ret_tensor(r);
}
#define RED(fname, opname, op) FN(fname) { return reduce_api(a, n, pos, op, opname); }
RED(f_sum, "sum", NTR_SUM) RED(f_mean, "mean", NTR_MEAN) RED(f_tmax, "max", NTR_MAX) RED(f_tmin, "min", NTR_MIN) RED(f_var, "var", NTR_VAR) RED(f_std, "std", NTR_STD) RED(f_argmax, "argmax", NTR_ARGMAX) RED(f_argmin, "argmin", NTR_ARGMIN) RED(f_prod, "prod", NTR_PROD)
/* ------------------------------------------------------------------ joining / slicing */
FN(f_concat) {
    NNAPI(); need_n(n, 1, 2, "concat"); if (a[0].type != VAL_ARRAY || a[0].as.arr->count < 1) NNERR("nn.concat: first argument must be a non-empty array of tensors");
    int k = a[0].as.arr->count; TArg *ta = nn_calloc((size_t)k, sizeof(TArg)); const NnTensor **ts = nn_malloc(sizeof(NnTensor *) * (size_t)k);
    for (int i = 0; i < k; i++) ts[i] = targ_get(&ta[i], a[0].as.arr->items, k, i, "concat");
    int axis = nt_norm_axis(n > 1 ? (int)arg_int(a, n, 1, "concat") : 0, ts[0]->nd ? ts[0]->nd : 1, "concat"); NnTensor *r = newt(); nt_concat(ts, k, axis, r);
    for (int i = 0; i < k; i++) targ_free(&ta[i]); free(ta); free(ts); return nn_ret_tensor(r);
}
FN(f_slice) {
    NNAPI(); need_n(n, 3, 3, "slice"); TArg x; NnTensor *p = targ_get(&x, a, n, 0, "slice"); if (p->nd < 1) { targ_free(&x); NNERR("nn.slice: needs at least 1 dimension"); }
    int64_t s = arg_int(a, n, 1, "slice"), e = arg_int(a, n, 2, "slice"), N = p->shape[0]; if (s < 0) s += N; if (e < 0) e += N; if (s < 0) s = 0; if (e > N) e = N; if (e < s) e = s;
    int64_t *idx = nn_malloc(sizeof(int64_t) * (size_t)(e - s + 1)); for (int64_t i = s; i < e; i++) idx[i - s] = i; NnTensor *r = newt(); nt_rows(p, idx, e - s, r); free(idx); targ_free(&x); return nn_ret_tensor(r);
}
static int64_t *index_list(KcValue v, int64_t *cnt, const char *fn) {
    TArg t; NnTensor *x = targ_get(&t, &v, 1, 0, fn); *cnt = (int64_t)x->n; int64_t *idx = nn_malloc(sizeof(int64_t) * (size_t)(x->n ? x->n : 1));
    for (size_t i = 0; i < x->n; i++) { if (x->d[i] != floor(x->d[i])) { targ_free(&t); free(idx); NNERR("nn.%s: indices must be integers", fn); } idx[i] = (int64_t)x->d[i]; } targ_free(&t); return idx;
}
FN(f_rows) {
    NNAPI(); need_n(n, 2, 2, "rows"); TArg x; NnTensor *p = targ_get(&x, a, n, 0, "rows"); int64_t c; int64_t *idx = index_list(a[1], &c, "rows");
    for (int64_t i = 0; i < c; i++) if (idx[i] < 0) idx[i] += p->shape[0]; NnTensor *r = newt(); nt_rows(p, idx, c, r); free(idx); targ_free(&x); return nn_ret_tensor(r);
}

/* ------------------------------------------------------------------ data preparation */
FN(f_one_hot) {
    NNAPI(); need_n(n, 2, 2, "one_hot"); TArg x; NnTensor *p = targ_get(&x, a, n, 0, "one_hot"); int64_t C = arg_int(a, n, 1, "one_hot"); if (C < 1 || C > 10000000) { targ_free(&x); NNERR("nn.one_hot: classes must be >= 1"); }
    int sh[NN_MAXDIM]; if (p->nd >= NN_MAXDIM) { targ_free(&x); NNERR("nn.one_hot: input has too many dimensions"); } memcpy(sh, p->shape, sizeof(int) * (size_t)p->nd); sh[p->nd] = (int)C; NnTensor *r = nt_new(p->nd + 1, sh);
    for (size_t i = 0; i < p->n; i++) { long c = (long)p->d[i]; if (p->d[i] != (double)c || c < 0 || c >= C) { targ_free(&x); nt_delete(r); NNERR("nn.one_hot: label %g at position %zu is outside 0..%lld", p->d[i], i, (long long)C - 1); } r->d[i * (size_t)C + (size_t)c] = 1; }
    targ_free(&x); return nn_ret_tensor(r);
}
static KcValue scale_api(KcValue *a, int n, KcPos pos, bool minmax) {
    NN_POS = &pos; const char *fn = minmax ? "minmax" : "standardize"; need_n(n, 1, 2, fn); TArg x; NnTensor *p = targ_get(&x, a, n, 0, fn); if (p->nd < 1) { targ_free(&x); NNERR("nn.%s: needs at least 1 dimension", fn); }
    int axis = nt_norm_axis(n > 1 ? (int)arg_int(a, n, 1, fn) : 0, p->nd, fn); NnTensor m1, m2, r1; nt_init(&m1); nt_init(&m2); nt_init(&r1); NnTensor *r = newt();
    if (minmax) { nt_reduce(NTR_MIN, p, axis, true, &m1); nt_reduce(NTR_MAX, p, axis, true, &m2); nt_binop(NTB_SUB, p, &m1, &r1); nt_binop(NTB_SUB, &m2, &m1, &m2); for (size_t i = 0; i < m2.n; i++) if (m2.d[i] == 0) m2.d[i] = 1; nt_binop(NTB_DIV, &r1, &m2, r); }
    else { nt_reduce(NTR_MEAN, p, axis, true, &m1); nt_reduce(NTR_STD, p, axis, true, &m2); nt_binop(NTB_SUB, p, &m1, &r1); for (size_t i = 0; i < m2.n; i++) if (m2.d[i] < 1e-12) m2.d[i] = 1; nt_binop(NTB_DIV, &r1, &m2, r); }
    nt_free(&m1); nt_free(&m2); nt_free(&r1); targ_free(&x); return nn_ret_tensor(r);
}
FN(f_standardize) { return scale_api(a, n, pos, false); }
FN(f_minmax) { return scale_api(a, n, pos, true); }
static void shuffle_idx(int64_t *p, int64_t N) { for (int64_t i = 0; i < N; i++) p[i] = i; for (int64_t i = N - 1; i > 0; i--) { int64_t j = nn_randint(i + 1); int64_t t = p[i]; p[i] = p[j]; p[j] = t; } }
FN(f_shuffle) {
    NNAPI(); need_n(n, 1, 2, "shuffle"); TArg x, y; NnTensor *p = targ_get(&x, a, n, 0, "shuffle"); NnTensor *q = n > 1 ? targ_get(&y, a, n, 1, "shuffle") : NULL; if (p->nd < 1) NNERR("nn.shuffle: needs at least 1 dimension");
    if (q && q->shape[0] != p->shape[0]) NNERR("nn.shuffle: both tensors need the same number of samples (%d vs %d)", p->shape[0], q->shape[0]);
    int64_t N = p->shape[0], *perm = nn_malloc(sizeof(int64_t) * (size_t)(N ? N : 1)); shuffle_idx(perm, N); NnTensor *r1 = newt(); nt_rows(p, perm, N, r1); KcValue out = nn_ret_tensor(r1);
    if (q) { NnTensor *r2 = newt(); nt_rows(q, perm, N, r2); KcValue pr = kc_array_new(); kc_array_push(pr, out); kc_array_push(pr, nn_ret_tensor(r2)); out = pr; targ_free(&y); } free(perm); targ_free(&x); return out;
}
FN(f_train_test_split) {
    NNAPI(); need_n(n, 3, 3, "train_test_split"); TArg x, y; NnTensor *p = targ_get(&x, a, n, 0, "train_test_split"), *q = targ_get(&y, a, n, 1, "train_test_split"); double frac = arg_num(a, n, 2, "train_test_split");
    if (p->nd < 1 || q->nd < 1 || p->shape[0] != q->shape[0]) NNERR("nn.train_test_split: X and Y must have the same number of samples"); if (!(frac > 0 && frac < 1)) NNERR("nn.train_test_split: test fraction must be between 0 and 1");
    int64_t N = p->shape[0]; int64_t nt = (int64_t)floor(frac * (double)N + 0.5); if (nt < 1) nt = 1; if (nt >= N) nt = N - 1; if (N < 2) NNERR("nn.train_test_split: need at least 2 samples");
    int64_t *perm = nn_malloc(sizeof(int64_t) * (size_t)N); shuffle_idx(perm, N); int64_t ntr = N - nt; KcValue out = kc_array_new(); NnTensor *r;
    r = newt(); nt_rows(p, perm, ntr, r); kc_array_push(out, nn_ret_tensor(r)); r = newt(); nt_rows(q, perm, ntr, r); kc_array_push(out, nn_ret_tensor(r));
    r = newt(); nt_rows(p, perm + ntr, nt, r); kc_array_push(out, nn_ret_tensor(r)); r = newt(); nt_rows(q, perm + ntr, nt, r); kc_array_push(out, nn_ret_tensor(r));
    free(perm); targ_free(&x); targ_free(&y); return out;
}
FN(f_pad_sequences) {
    NNAPI(); need_n(n, 1, 4, "pad_sequences"); if (a[0].type != VAL_ARRAY) NNERR("nn.pad_sequences: first argument must be an array of integer arrays");
    KcArray *S = a[0].as.arr; int64_t ml = n > 1 && a[1].type != VAL_NULL ? arg_int(a, n, 1, "pad_sequences") : 0; double val = n > 2 ? arg_num(a, n, 2, "pad_sequences") : 0; const char *side = n > 3 ? arg_str(a, n, 3, "pad_sequences") : "post";
    bool post = !strcmp(side, "post"); if (!post && strcmp(side, "pre")) NNERR("nn.pad_sequences: side must be \"post\" or \"pre\"");
    int64_t longest = 0; for (int i = 0; i < S->count; i++) { if (S->items[i].type != VAL_ARRAY) NNERR("nn.pad_sequences: item %d is not an array", i + 1); if (S->items[i].as.arr->count > longest) longest = S->items[i].as.arr->count; }
    if (ml <= 0) ml = longest; if (ml < 1) ml = 1; int sh[2] = { S->count, (int)ml }; NnTensor *r = nt_new(2, sh); for (size_t i = 0; i < r->n; i++) r->d[i] = val;
    for (int i = 0; i < S->count; i++) { KcArray *s = S->items[i].as.arr; int64_t len = s->count < ml ? s->count : ml; int64_t src0 = post ? 0 : s->count - len, dst0 = post ? 0 : ml - len;
        for (int64_t k = 0; k < len; k++) { KcValue v = s->items[src0 + k]; if (v.type != VAL_INT && v.type != VAL_FLOAT) { nt_delete(r); NNERR("nn.pad_sequences: sequence %d contains a non-number", i + 1); } r->d[(size_t)i * (size_t)ml + (size_t)(dst0 + k)] = v.type == VAL_INT ? (double)v.as.i : v.as.f; } }
    return nn_ret_tensor(r);
}
FN(f_window_series) {
    NNAPI(); need_n(n, 2, 3, "window_series"); TArg x; NnTensor *p = targ_get(&x, a, n, 0, "window_series"); int64_t W = arg_int(a, n, 1, "window_series"), Hz = n > 2 ? arg_int(a, n, 2, "window_series") : 1;
    if (p->nd < 1 || p->nd > 2) { targ_free(&x); NNERR("nn.window_series: data must be a 1-D series or a [time, features] table"); } if (W < 1 || Hz < 1) { targ_free(&x); NNERR("nn.window_series: window and horizon must be >= 1"); }
    int T = p->shape[0], F = p->nd == 2 ? p->shape[1] : 1; int64_t S = (int64_t)T - W - Hz + 1; if (S < 1) { targ_free(&x); NNERR("nn.window_series: series of %d steps is too short for window %lld + horizon %lld", T, (long long)W, (long long)Hz); }
    int xs[3] = { (int)S, (int)W, F }, ys[2] = { (int)S, (int)(Hz * F) }; NnTensor *X = nt_new(3, xs), *Y = nt_new(2, ys);
    for (int64_t s = 0; s < S; s++) { memcpy(X->d + (size_t)s * (size_t)W * (size_t)F, p->d + (size_t)s * (size_t)F, sizeof(nn_real) * (size_t)W * (size_t)F); memcpy(Y->d + (size_t)s * (size_t)Hz * (size_t)F, p->d + (size_t)(s + W) * (size_t)F, sizeof(nn_real) * (size_t)Hz * (size_t)F); }
    KcValue out = kc_array_new(); kc_array_push(out, nn_ret_tensor(X)); kc_array_push(out, nn_ret_tensor(Y)); targ_free(&x); return out;
}

/* ------------------------------------------------------------------ metrics */
FN(f_metric) {
    NNAPI(); need_n(n, 3, 3, "metric"); const char *name = arg_str(a, n, 0, "metric"); int kind, k; if (!nn_metric_parse(name, &kind, &k) || kind == NNM_NONE) NNERR("nn.metric: unknown metric '%s' (accuracy, top_k:N, mae, mse, rmse, r2)", name);
    TArg x, y; NnTensor *p = targ_get(&x, a, n, 1, "metric"), *q = targ_get(&y, a, n, 2, "metric"); NnMetric m; memset(&m, 0, sizeof m); m.kind = kind; m.k = k; nn_metric_update(&m, p, q); double v = nn_metric_value(&m); targ_free(&x); targ_free(&y); return kc_float(v);
}
static void to_classes(const NnTensor *p, int64_t *out, int64_t R, int64_t C_hint, const char *fn) {                          /* scores / one-hot / indices -> class ids */
    size_t last = p->nd ? (size_t)p->shape[p->nd - 1] : 1;
    if (p->nd >= 2 && last > 1 && p->n == (size_t)R * last) { for (int64_t r = 0; r < R; r++) { size_t am = 0; for (size_t c = 1; c < last; c++) if (p->d[(size_t)r * last + c] > p->d[(size_t)r * last + am]) am = c; out[r] = (int64_t)am; } return; }
    if ((int64_t)p->n != R) NNERR("nn.%s: prediction and target sizes disagree", fn); (void)C_hint;
    for (int64_t r = 0; r < R; r++) out[r] = (p->nd >= 2 && last == 1) ? (p->d[r] >= 0.5) : (int64_t)llround(p->d[r]);
}
static int64_t sample_count(const NnTensor *t) { size_t last = t->nd ? (size_t)t->shape[t->nd - 1] : 1; return (t->nd >= 2 && last > 1) ? (int64_t)(t->n / last) : (int64_t)t->n; }
static NnTensor *confusion(NnTensor *p, NnTensor *q, int64_t C, const char *fn) {
    int64_t R = sample_count(q); if (sample_count(p) != R) NNERR("nn.%s: prediction has %lld samples but target has %lld", fn, (long long)sample_count(p), (long long)R);
    int64_t *pc = nn_malloc(sizeof(int64_t) * (size_t)(R ? R : 1)), *tc = nn_malloc(sizeof(int64_t) * (size_t)(R ? R : 1)); to_classes(p, pc, R, C, fn); to_classes(q, tc, R, C, fn);
    int sh[2] = { (int)C, (int)C }; NnTensor *m = nt_new(2, sh);
    for (int64_t i = 0; i < R; i++) { if (pc[i] < 0 || pc[i] >= C || tc[i] < 0 || tc[i] >= C) { free(pc); free(tc); nt_delete(m); NNERR("nn.%s: class id out of range 0..%lld", fn, (long long)C - 1); } m->d[(size_t)tc[i] * (size_t)C + (size_t)pc[i]] += 1; }
    free(pc); free(tc); return m;
}
FN(f_confusion_matrix) {
    NNAPI(); need_n(n, 3, 3, "confusion_matrix"); TArg x, y; NnTensor *p = targ_get(&x, a, n, 0, "confusion_matrix"), *q = targ_get(&y, a, n, 1, "confusion_matrix"); int64_t C = arg_int(a, n, 2, "confusion_matrix");
    if (C < 2 || C > 20000) NNERR("nn.confusion_matrix: classes must be 2..20000"); NnTensor *m = confusion(p, q, C, "confusion_matrix"); targ_free(&x); targ_free(&y); return nn_ret_tensor(m);
}
FN(f_prf) {
    NNAPI(); need_n(n, 3, 3, "prf"); TArg x, y; NnTensor *p = targ_get(&x, a, n, 0, "prf"), *q = targ_get(&y, a, n, 1, "prf"); int64_t C = arg_int(a, n, 2, "prf"); if (C < 2 || C > 20000) NNERR("nn.prf: classes must be 2..20000");
    NnTensor *m = confusion(p, q, C, "prf"); double *pr = nn_calloc((size_t)C, sizeof(double)), *rc = nn_calloc((size_t)C, sizeof(double)), *f1 = nn_calloc((size_t)C, sizeof(double)); double correct = 0, total = 0, mf = 0; int64_t *sup = nn_calloc((size_t)C, sizeof(int64_t));
    for (int64_t c = 0; c < C; c++) { double tp = m->d[(size_t)c * (size_t)C + (size_t)c], fp = 0, fn_ = 0; for (int64_t k = 0; k < C; k++) { if (k != c) { fp += m->d[(size_t)k * (size_t)C + (size_t)c]; fn_ += m->d[(size_t)c * (size_t)C + (size_t)k]; } }
        pr[c] = tp + fp > 0 ? tp / (tp + fp) : 0; rc[c] = tp + fn_ > 0 ? tp / (tp + fn_) : 0; f1[c] = pr[c] + rc[c] > 0 ? 2 * pr[c] * rc[c] / (pr[c] + rc[c]) : 0; correct += tp; total += tp + fn_; sup[c] = (int64_t)(tp + fn_); mf += f1[c]; }
    const char *nm[] = { "precision", "recall", "f1", "support", "macro_f1", "accuracy" }; KcValue v[] = { arr_of_doubles(pr, (int)C), arr_of_doubles(rc, (int)C), arr_of_doubles(f1, (int)C), arr_of_ints(sup, (int)C), kc_float(mf / (double)C), kc_float(total > 0 ? correct / total : 0) };
    free(pr); free(rc); free(f1); free(sup); nt_delete(m); targ_free(&x); targ_free(&y); return mk_struct("PRF", 6, nm, v);
}

#define E(name, fn) { name, fn }
const NnEntry nn_tensor_entries[] = {
    E("tensor", f_tensor), E("zeros", f_zeros), E("ones", f_ones), E("full", f_full), E("rand", f_rand), E("randn", f_randn), E("randint", f_randint), E("sample", f_sample), E("arange", f_arange), E("linspace", f_linspace), E("eye", f_eye),
    E("shape", f_shape), E("size", f_size), E("ndim", f_ndim), E("to_array", f_to_array), E("item", f_item), E("get", f_get), E("set", f_set), E("copy", f_copy), E("free", f_free),
    E("reshape", f_reshape), E("transpose", f_transpose), E("matmul", f_matmul),
    E("add", f_add), E("sub", f_sub), E("mul", f_mul), E("div", f_div), E("pow", f_pow), E("maximum", f_maximum), E("minimum", f_minimum),
    E("neg", f_neg), E("abs", f_abs), E("exp", f_exp), E("log", f_log), E("sqrt", f_sqrt), E("square", f_square), E("sin", f_sin), E("cos", f_cos), E("sign", f_sign), E("floor", f_floor), E("ceil", f_ceil), E("round", f_round), E("clip", f_clip),
    E("sum", f_sum), E("mean", f_mean), E("max", f_tmax), E("min", f_tmin), E("var", f_var), E("std", f_std), E("prod", f_prod), E("argmax", f_argmax), E("argmin", f_argmin),
    E("concat", f_concat), E("slice", f_slice), E("rows", f_rows),
    E("one_hot", f_one_hot), E("standardize", f_standardize), E("minmax", f_minmax), E("shuffle", f_shuffle), E("train_test_split", f_train_test_split), E("pad_sequences", f_pad_sequences), E("window_series", f_window_series),
    E("metric", f_metric), E("confusion_matrix", f_confusion_matrix), E("prf", f_prf),
};
const int nn_tensor_count = (int)(sizeof nn_tensor_entries / sizeof nn_tensor_entries[0]);
