/* KingCobra std.nn script API — advanced tensor math (special functions, reductions, scans, sorting, shape ops,
 * indexing, products, statistics, sampling).  The numeric work lives in nn_math.c.
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com> */
#include "nn_glue.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>

#define FN(name) static KcValue name(KcInterp *ip, KcValue *a, int n, KcPos pos)
static NnTensor *newt(void) { NnTensor *t = nn_malloc(sizeof *t); nt_init(t); return t; }
#define E(name, fn) { name, fn }

/* ------------------------------------------------------------------ argument helpers */
static int ax_opt(KcValue *a, int n, int i, const NnTensor *t, const char *fn) {                 /* missing / null -> whole tensor */
    if (i >= n || a[i].type == VAL_NULL) return NT_AXIS_ALL;
    return nt_norm_axis((int)arg_int(a, n, i, fn), t->nd, fn);
}
static int ax_req(KcValue *a, int n, int i, const NnTensor *t, int def, const char *fn) {        /* missing -> def (usually -1) */
    return nt_norm_axis(i < n && a[i].type != VAL_NULL ? (int)arg_int(a, n, i, fn) : def, t->nd, fn);
}
static bool bool_opt(KcValue *a, int n, int i, bool def) { return i < n ? kc_truthy(a[i]) : def; }
static int64_t val_int(KcValue v, const char *fn) {
    if (v.type == VAL_INT) return v.as.i;
    if (v.type == VAL_FLOAT && v.as.f == floor(v.as.f) && fabs(v.as.f) < 9e15) return (int64_t)v.as.f;
    NNERR("nn.%s: expected an integer (got %s)", fn, kc_type_name(v)); return 0;
}
static int int_list(KcValue v, int *out, int max, const char *fn) {                            /* int or array of ints */
    if (v.type != VAL_ARRAY) { out[0] = (int)val_int(v, fn); return 1; }
    int c = v.as.arr->count; if (c > max) NNERR("nn.%s: too many entries (max %d)", fn, max);
    for (int i = 0; i < c; i++) out[i] = (int)val_int(v.as.arr->items[i], fn); return c;
}
static double parse_ord(KcValue *a, int n, int i, const char *fn) {
    if (i >= n || a[i].type == VAL_NULL) return 2.0;
    if (a[i].type == VAL_STRING) {
        const char *s = a[i].as.s->data;
        if (!strcmp(s, "inf") || !strcmp(s, "+inf")) return INFINITY; if (!strcmp(s, "-inf")) return -INFINITY; if (!strcmp(s, "fro")) return 2.0;
        NNERR("nn.%s: unknown norm order '%s' (use a number, \"inf\", \"-inf\" or \"fro\")", fn, s);
    }
    return arg_num(a, n, i, fn);
}
static NnTensor *reshaped_copy(const NnTensor *t, int nd, const int *shape) {
    NnTensor *r = newt(); nt_copy(r, t); r->nd = nd; for (int i = 0; i < NN_MAXDIM; i++) r->shape[i] = i < nd ? shape[i] : 0; return r;
}
static int64_t *index_list(KcValue v, int64_t *cnt, const char *fn) {
    TArg t; NnTensor *x = targ_get(&t, &v, 1, 0, fn); *cnt = (int64_t)x->n; int64_t *idx = nn_malloc(sizeof(int64_t) * (size_t)(x->n ? x->n : 1));
    for (size_t i = 0; i < x->n; i++) { if (x->d[i] != floor(x->d[i])) NNERR("nn.%s: indices must be integers (got %g)", fn, x->d[i]); idx[i] = (int64_t)x->d[i]; }
    targ_free(&t); return idx;
}

/* ================================================================== elementwise */
#define NMU(fname, opname, op) FN(fname) { NNAPI(); need_n(n, 1, 1, opname); TArg x; NnTensor *p = targ_get(&x, a, n, 0, opname); NnTensor *r = newt(); nm_unary(op, p, r); targ_free(&x); return nn_ret_tensor(r); }
NMU(f_erf, "erf", NMU_ERF) NMU(f_erfc, "erfc", NMU_ERFC) NMU(f_erfinv, "erfinv", NMU_ERFINV) NMU(f_lgamma, "lgamma", NMU_LGAMMA) NMU(f_digamma, "digamma", NMU_DIGAMMA)
NMU(f_log1p, "log1p", NMU_LOG1P) NMU(f_expm1, "expm1", NMU_EXPM1) NMU(f_tan, "tan", NMU_TAN) NMU(f_asin, "asin", NMU_ASIN) NMU(f_acos, "acos", NMU_ACOS) NMU(f_atan, "atan", NMU_ATAN)
NMU(f_sinh, "sinh", NMU_SINH) NMU(f_cosh, "cosh", NMU_COSH) NMU(f_asinh, "asinh", NMU_ASINH) NMU(f_acosh, "acosh", NMU_ACOSH) NMU(f_atanh, "atanh", NMU_ATANH)
NMU(f_rsqrt, "rsqrt", NMU_RSQRT) NMU(f_reciprocal, "reciprocal", NMU_RECIP) NMU(f_cbrt, "cbrt", NMU_CBRT) NMU(f_log2, "log2", NMU_LOG2) NMU(f_log10, "log10", NMU_LOG10) NMU(f_exp2, "exp2", NMU_EXP2)
NMU(f_trunc, "trunc", NMU_TRUNC) NMU(f_frac, "frac", NMU_FRAC) NMU(f_log_sigmoid, "log_sigmoid", NMU_LOGSIGMOID) NMU(f_normal_cdf, "normal_cdf", NMU_NDTR) NMU(f_normal_ppf, "normal_ppf", NMU_NDTRI)
NMU(f_logit, "logit", NMU_LOGIT) NMU(f_isnan, "isnan", NMU_ISNAN) NMU(f_isinf, "isinf", NMU_ISINF) NMU(f_isfinite, "isfinite", NMU_ISFINITE) NMU(f_logical_not, "logical_not", NMU_LNOT)

#define NMB(fname, opname, op) FN(fname) { NNAPI(); need_n(n, 2, 2, opname); TArg x, y; NnTensor *p = targ_get(&x, a, n, 0, opname), *q = targ_get(&y, a, n, 1, opname); NnTensor *r = newt(); nt_binop(op, p, q, r); targ_free(&x); targ_free(&y); return nn_ret_tensor(r); }
NMB(f_atan2, "atan2", NTB_ATAN2) NMB(f_hypot, "hypot", NTB_HYPOT) NMB(f_mod, "mod", NTB_MOD) NMB(f_floor_div, "floor_div", NTB_FLOORDIV) NMB(f_logaddexp, "logaddexp", NTB_LOGADDEXP)
NMB(f_eq, "eq", NTB_EQ) NMB(f_ne, "ne", NTB_NE) NMB(f_lt, "lt", NTB_LT) NMB(f_le, "le", NTB_LE) NMB(f_gt, "gt", NTB_GT) NMB(f_ge, "ge", NTB_GE)
NMB(f_logical_and, "logical_and", NTB_AND) NMB(f_logical_or, "logical_or", NTB_OR) NMB(f_logical_xor, "logical_xor", NTB_XOR)

FN(f_where) {
    NNAPI(); need_n(n, 3, 3, "where"); TArg c, x, y; NnTensor *pc = targ_get(&c, a, n, 0, "where"), *px = targ_get(&x, a, n, 1, "where"), *py = targ_get(&y, a, n, 2, "where");
    NnTensor *r = newt(); nm_where(pc, px, py, r); targ_free(&c); targ_free(&x); targ_free(&y); return nn_ret_tensor(r);
}
FN(f_masked_fill) {
    NNAPI(); need_n(n, 3, 3, "masked_fill"); TArg x, m, v; NnTensor *px = targ_get(&x, a, n, 0, "masked_fill"), *pm = targ_get(&m, a, n, 1, "masked_fill"), *pv = targ_get(&v, a, n, 2, "masked_fill");
    NnTensor *r = newt(); nm_where(pm, pv, px, r); targ_free(&x); targ_free(&m); targ_free(&v); return nn_ret_tensor(r);
}
FN(f_nan_to_num) {
    NNAPI(); need_n(n, 1, 4, "nan_to_num"); TArg x; NnTensor *p = targ_get(&x, a, n, 0, "nan_to_num");
    double nv = n > 1 ? arg_num(a, n, 1, "nan_to_num") : 0.0, pv = n > 2 ? arg_num(a, n, 2, "nan_to_num") : DBL_MAX, mv = n > 3 ? arg_num(a, n, 3, "nan_to_num") : -DBL_MAX;
    NnTensor *r = newt(); nt_resize(r, p->nd, p->shape); for (size_t i = 0; i < p->n; i++) { double v = p->d[i]; r->d[i] = isnan(v) ? nv : (v == INFINITY ? pv : (v == -INFINITY ? mv : v)); }
    targ_free(&x); return nn_ret_tensor(r);
}
FN(f_lerp) {                                                                                  /* a + w * (b - a) */
    NNAPI(); need_n(n, 3, 3, "lerp"); TArg x, y, w; NnTensor *pa = targ_get(&x, a, n, 0, "lerp"), *pb = targ_get(&y, a, n, 1, "lerp"), *pw = targ_get(&w, a, n, 2, "lerp");
    NnTensor d, s, *r = newt(); nt_init(&d); nt_init(&s); nt_binop(NTB_SUB, pb, pa, &d); nt_binop(NTB_MUL, &d, pw, &s); nt_binop(NTB_ADD, pa, &s, r);
    nt_free(&d); nt_free(&s); targ_free(&x); targ_free(&y); targ_free(&w); return nn_ret_tensor(r);
}
FN(f_clamp) {                                                                                 /* tensor-valued bounds; null skips a side */
    NNAPI(); need_n(n, 3, 3, "clamp"); TArg x, lo, hi; NnTensor *p = targ_get(&x, a, n, 0, "clamp"); NnTensor t1, *r = newt(); nt_init(&t1); nt_copy(&t1, p);
    if (a[1].type != VAL_NULL) { NnTensor *pl = targ_get(&lo, a, n, 1, "clamp"); NnTensor t2; nt_init(&t2); nt_binop(NTB_MAX, &t1, pl, &t2); nt_free(&t1); t1 = t2; targ_free(&lo); }
    if (a[2].type != VAL_NULL) { NnTensor *ph = targ_get(&hi, a, n, 2, "clamp"); NnTensor t2; nt_init(&t2); nt_binop(NTB_MIN, &t1, ph, &t2); nt_free(&t1); t1 = t2; targ_free(&hi); }
    *r = t1; targ_free(&x); return nn_ret_tensor(r);
}
FN(f_equal) {
    NNAPI(); need_n(n, 2, 2, "equal"); TArg x, y; NnTensor *p = targ_get(&x, a, n, 0, "equal"), *q = targ_get(&y, a, n, 1, "equal"); bool eq = nt_same_shape(p, q);
    for (size_t i = 0; eq && i < p->n; i++) if (p->d[i] != q->d[i]) eq = false; targ_free(&x); targ_free(&y); return kc_bool(eq);
}
FN(f_allclose) {                                                                               /* |a - b| <= atol + rtol * |b|; equal infinities count as close; NaN never does */
    NNAPI(); need_n(n, 2, 4, "allclose"); TArg x, y; NnTensor *p = targ_get(&x, a, n, 0, "allclose"), *q = targ_get(&y, a, n, 1, "allclose");
    double rtol = n > 2 ? arg_num(a, n, 2, "allclose") : 1e-5, atol = n > 3 ? arg_num(a, n, 3, "allclose") : 1e-8;
    if (!nt_same_shape(p, q) && p->n != 1 && q->n != 1) NNERR("nn.allclose: shapes must match, or one argument must be a single value (%s vs %s)", nt_shape_str(p->nd, p->shape), nt_shape_str(q->nd, q->shape));
    size_t N = p->n > q->n ? p->n : q->n; bool ok = true;
    for (size_t i = 0; i < N && ok; i++) { double u = p->d[p->n == 1 ? 0 : i], v = q->d[q->n == 1 ? 0 : i]; if (u == v) continue; if (!(fabs(u - v) <= atol + rtol * fabs(v))) ok = false; }
    targ_free(&x); targ_free(&y); return kc_bool(ok);
}

/* ================================================================== activations as tensor functions */
FN(f_activation_grad) {
    NNAPI(); need_n(n, 2, 3, "activation_grad"); const char *nm = arg_str(a, n, 0, "activation_grad"); int id = nn_act_from_name(nm); if (id < 0) NNERR("nn.activation_grad: unknown activation '%s'", nm);
    TArg x; NnTensor *p = targ_get(&x, a, n, 1, "activation_grad"); double al = n > 2 ? arg_num(a, n, 2, "activation_grad") : (id == NNA_LEAKY ? 0.01 : 1.0);
    NnTensor *r = newt(); nm_act(id, al, true, p, r); targ_free(&x); return nn_ret_tensor(r);
}
FN(f_log_softmax_axis) {
    NNAPI(); need_n(n, 1, 2, "log_softmax_axis"); TArg x; NnTensor *p = targ_get(&x, a, n, 0, "log_softmax_axis"); int ax = ax_req(a, n, 1, p, -1, "log_softmax_axis");
    NnTensor lse; nt_init(&lse); nm_reduce(NMR_LOGSUMEXP, p, ax, true, 0, &lse); NnTensor *r = newt(); nt_binop(NTB_SUB, p, &lse, r); nt_free(&lse); targ_free(&x); return nn_ret_tensor(r);
}
FN(f_softmax_grad) {                                                                           /* given y = softmax(x) and dL/dy: dL/dx = y * (dy - sum(dy*y)) */
    NNAPI(); need_n(n, 2, 3, "softmax_grad"); TArg y, d; NnTensor *py = targ_get(&y, a, n, 0, "softmax_grad"), *pd = targ_get(&d, a, n, 1, "softmax_grad");
    if (!nt_same_shape(py, pd)) NNERR("nn.softmax_grad: y and dy must have the same shape (%s vs %s)", nt_shape_str(py->nd, py->shape), nt_shape_str(pd->nd, pd->shape));
    int ax = ax_req(a, n, 2, py, -1, "softmax_grad"); NnTensor pr, s, df; nt_init(&pr); nt_init(&s); nt_init(&df); nt_binop(NTB_MUL, pd, py, &pr); nt_reduce(NTR_SUM, &pr, ax, true, &s); nt_binop(NTB_SUB, pd, &s, &df);
    NnTensor *r = newt(); nt_binop(NTB_MUL, py, &df, r); nt_free(&pr); nt_free(&s); nt_free(&df); targ_free(&y); targ_free(&d); return nn_ret_tensor(r);
}

/* ================================================================== reductions */
static KcValue red2(KcValue *a, int n, KcPos pos, int op, const char *fn, int ai, double param) {
    NN_POS = &pos; TArg x; NnTensor *p = targ_get(&x, a, n, 0, fn); int axis = ax_opt(a, n, ai, p, fn); bool keep = bool_opt(a, n, ai + 1, false);
    NnTensor *r = newt(); nm_reduce(op, p, axis, keep, param, r); targ_free(&x); return nn_ret_tensor(r);
}
#define RED2(fname, opname, op) FN(fname) { need_n(n, 1, 3, opname); return red2(a, n, pos, op, opname, 1, 0); }
RED2(f_any, "any", NMR_ANY) RED2(f_all, "all", NMR_ALL) RED2(f_count_nonzero, "count_nonzero", NMR_COUNT) RED2(f_logsumexp, "logsumexp", NMR_LOGSUMEXP)
RED2(f_nansum, "nansum", NMR_NANSUM) RED2(f_nanmean, "nanmean", NMR_NANMEAN) RED2(f_ptp, "ptp", NMR_PTP)
FN(f_median) { need_n(n, 1, 3, "median"); return red2(a, n, pos, NMR_QUANTILE, "median", 1, 0.5); }
FN(f_quantile) {
    NN_POS = &pos; need_n(n, 2, 4, "quantile"); double q = arg_num(a, n, 1, "quantile"); if (!(q >= 0 && q <= 1)) NNERR("nn.quantile: q must be between 0 and 1 (got %g)", q);
    return red2(a, n, pos, NMR_QUANTILE, "quantile", 2, q);
}
FN(f_norm) {                                                                                  /* vector p-norm over an axis (or over all elements) */
    NNAPI(); need_n(n, 1, 4, "norm"); double p = parse_ord(a, n, 1, "norm"); if (p < 0 && !isinf(p)) NNERR("nn.norm: order must be >= 0, or -inf");
    TArg x; NnTensor *t = targ_get(&x, a, n, 0, "norm"); int axis = ax_opt(a, n, 2, t, "norm"); bool keep = bool_opt(a, n, 3, false);
    NnTensor *r = newt(); nm_reduce(NMR_NORM, t, axis, keep, p, r); targ_free(&x); return nn_ret_tensor(r);
}

/* ================================================================== scans */
static KcValue cum_api(KcValue *a, int n, KcPos pos, int op, const char *fn) {
    NN_POS = &pos; need_n(n, 1, 3, fn); TArg x; NnTensor *p = targ_get(&x, a, n, 0, fn); int axis = ax_req(a, n, 1, p, -1, fn); bool rev = bool_opt(a, n, 2, false);
    NnTensor *r = newt(); nm_cum(op, p, axis, rev, r); targ_free(&x); return nn_ret_tensor(r);
}
#define CUM(fname, opname, op) FN(fname) { return cum_api(a, n, pos, op, opname); }
CUM(f_cumsum, "cumsum", NMC_SUM) CUM(f_cumprod, "cumprod", NMC_PROD) CUM(f_cummax, "cummax", NMC_MAX) CUM(f_cummin, "cummin", NMC_MIN) CUM(f_logcumsumexp, "logcumsumexp", NMC_LOGSUMEXP)
FN(f_diff) {
    NNAPI(); need_n(n, 1, 3, "diff"); TArg x; NnTensor *p = targ_get(&x, a, n, 0, "diff"); int order = n > 1 ? (int)arg_int(a, n, 1, "diff") : 1; int ax = ax_req(a, n, 2, p, -1, "diff");
    NnTensor *r = newt(); nm_diff(p, order, ax, r); targ_free(&x); return nn_ret_tensor(r);
}

/* ================================================================== sorting */
FN(f_sort) {
    NNAPI(); need_n(n, 1, 3, "sort"); TArg x; NnTensor *p = targ_get(&x, a, n, 0, "sort"); int ax = ax_req(a, n, 1, p, -1, "sort"); NnTensor *r = newt(); nm_sort(p, ax, bool_opt(a, n, 2, false), r, NULL); targ_free(&x); return nn_ret_tensor(r);
}
FN(f_argsort) {
    NNAPI(); need_n(n, 1, 3, "argsort"); TArg x; NnTensor *p = targ_get(&x, a, n, 0, "argsort"); int ax = ax_req(a, n, 1, p, -1, "argsort"); NnTensor *r = newt(); nm_sort(p, ax, bool_opt(a, n, 2, false), NULL, r); targ_free(&x); return nn_ret_tensor(r);
}
FN(f_topk) {
    NNAPI(); need_n(n, 2, 4, "topk"); TArg x; NnTensor *p = targ_get(&x, a, n, 0, "topk"); int k = (int)arg_int(a, n, 1, "topk"); int ax = ax_req(a, n, 2, p, -1, "topk");
    NnTensor *v = newt(), *i = newt(); nm_topk(p, k, ax, bool_opt(a, n, 3, true), v, i); targ_free(&x);
    const char *nm[] = { "values", "indices" }; KcValue vals[] = { nn_ret_tensor(v), nn_ret_tensor(i) }; return mk_struct("TopK", 2, nm, vals);
}

/* ================================================================== shape manipulation */
FN(f_squeeze) {
    NNAPI(); need_n(n, 1, 2, "squeeze"); TArg x; NnTensor *p = targ_get(&x, a, n, 0, "squeeze"); int sh[NN_MAXDIM], nd = 0;
    if (n > 1 && a[1].type != VAL_NULL) {
        int ax = nt_norm_axis((int)arg_int(a, n, 1, "squeeze"), p->nd, "squeeze"); if (p->shape[ax] != 1) NNERR("nn.squeeze: axis %d has size %d, only size-1 axes can be squeezed", ax, p->shape[ax]);
        for (int i = 0; i < p->nd; i++) if (i != ax) sh[nd++] = p->shape[i];
    } else for (int i = 0; i < p->nd; i++) if (p->shape[i] != 1) sh[nd++] = p->shape[i];
    NnTensor *r = reshaped_copy(p, nd, sh); targ_free(&x); return nn_ret_tensor(r);
}
FN(f_unsqueeze) {
    NNAPI(); need_n(n, 2, 2, "unsqueeze"); TArg x; NnTensor *p = targ_get(&x, a, n, 0, "unsqueeze"); if (p->nd >= NN_MAXDIM) NNERR("nn.unsqueeze: tensor already has the maximum of %d dimensions", NN_MAXDIM);
    int ax = (int)arg_int(a, n, 1, "unsqueeze"); if (ax < 0) ax += p->nd + 1; if (ax < 0 || ax > p->nd) NNERR("nn.unsqueeze: axis out of range for a tensor with %d dimensions", p->nd);
    int sh[NN_MAXDIM], k = 0; for (int i = 0; i <= p->nd; i++) { if (i == ax) sh[k++] = 1; if (i < p->nd) sh[k++] = p->shape[i]; }
    NnTensor *r = reshaped_copy(p, p->nd + 1, sh); targ_free(&x); return nn_ret_tensor(r);
}
FN(f_broadcast_to) {
    NNAPI(); need_n(n, 2, 2, "broadcast_to"); TArg x; NnTensor *p = targ_get(&x, a, n, 0, "broadcast_to"); int sh[NN_MAXDIM], nd = arg_shape(a, n, 1, sh, "broadcast_to");
    for (int i = 0; i < nd; i++) if (sh[i] < 0) NNERR("nn.broadcast_to: dimensions must be >= 0");
    NnTensor *r = newt(); nm_broadcast_to(p, nd, sh, r); targ_free(&x); return nn_ret_tensor(r);
}
FN(f_swapaxes) {
    NNAPI(); need_n(n, 3, 3, "swapaxes"); TArg x; NnTensor *p = targ_get(&x, a, n, 0, "swapaxes"); int i = nt_norm_axis((int)arg_int(a, n, 1, "swapaxes"), p->nd, "swapaxes"), j = nt_norm_axis((int)arg_int(a, n, 2, "swapaxes"), p->nd, "swapaxes");
    int ax[NN_MAXDIM]; for (int k = 0; k < p->nd; k++) ax[k] = k; ax[i] = j; ax[j] = i; NnTensor *r = newt(); nt_permute(p, ax, r); targ_free(&x); return nn_ret_tensor(r);
}
FN(f_moveaxis) {
    NNAPI(); need_n(n, 3, 3, "moveaxis"); TArg x; NnTensor *p = targ_get(&x, a, n, 0, "moveaxis"); int s = nt_norm_axis((int)arg_int(a, n, 1, "moveaxis"), p->nd, "moveaxis"), d = nt_norm_axis((int)arg_int(a, n, 2, "moveaxis"), p->nd, "moveaxis");
    int order[NN_MAXDIM], k = 0; for (int i = 0; i < p->nd; i++) if (i != s) order[k++] = i; for (int i = p->nd - 1; i > d; i--) order[i] = order[i - 1]; order[d] = s;
    NnTensor *r = newt(); nt_permute(p, order, r); targ_free(&x); return nn_ret_tensor(r);
}
FN(f_stack) {
    NNAPI(); need_n(n, 1, 2, "stack"); if (a[0].type != VAL_ARRAY || a[0].as.arr->count < 1) NNERR("nn.stack: first argument must be a non-empty array of tensors");
    int k = a[0].as.arr->count; TArg *ta = nn_calloc((size_t)k, sizeof(TArg)); NnTensor *views = nn_calloc((size_t)k, sizeof(NnTensor)); const NnTensor **ps = nn_malloc(sizeof(NnTensor *) * (size_t)k);
    for (int i = 0; i < k; i++) ps[i] = targ_get(&ta[i], a[0].as.arr->items, k, i, "stack");
    int nd = ps[0]->nd; if (nd >= NN_MAXDIM) NNERR("nn.stack: tensors already have the maximum of %d dimensions", NN_MAXDIM);
    for (int i = 1; i < k; i++) if (!nt_same_shape(ps[0], ps[i])) NNERR("nn.stack: all tensors must have the same shape (%s vs %s)", nt_shape_str(ps[0]->nd, ps[0]->shape), nt_shape_str(ps[i]->nd, ps[i]->shape));
    int ax = n > 1 ? (int)arg_int(a, n, 1, "stack") : 0; if (ax < 0) ax += nd + 1; if (ax < 0 || ax > nd) NNERR("nn.stack: axis out of range");
    const NnTensor **vp = nn_malloc(sizeof(NnTensor *) * (size_t)k);
    for (int i = 0; i < k; i++) { views[i] = *ps[i]; views[i].nd = nd + 1; int q = 0; for (int d = 0; d <= nd; d++) { if (d == ax) views[i].shape[d] = 1; else views[i].shape[d] = ps[i]->shape[q++]; } vp[i] = &views[i]; }
    NnTensor *r = newt(); nt_concat(vp, k, ax, r);
    for (int i = 0; i < k; i++) targ_free(&ta[i]); free(ta); free(views); free(ps); free(vp); return nn_ret_tensor(r);
}
FN(f_split) {                                                                                  /* equal sections (int) or explicit sizes (array) */
    NNAPI(); need_n(n, 2, 3, "split"); TArg x; NnTensor *p = targ_get(&x, a, n, 0, "split"); int ax = ax_req(a, n, 2, p, 0, "split"); int N = p->shape[ax]; int sizes[256], ns = 0;
    if (a[1].type == VAL_ARRAY) { ns = int_list(a[1], sizes, 256, "split"); int tot = 0; for (int i = 0; i < ns; i++) { if (sizes[i] < 0) NNERR("nn.split: sizes must be >= 0"); tot += sizes[i]; } if (tot != N) NNERR("nn.split: sizes sum to %d but axis %d has %d entries", tot, ax, N); }
    else { int64_t s = arg_int(a, n, 1, "split"); if (s < 1 || s > 256 || N % s) NNERR("nn.split: cannot split an axis of size %d into %lld equal sections", N, (long long)s); ns = (int)s; for (int i = 0; i < ns; i++) sizes[i] = N / ns; }
    KcValue out = kc_array_new(); int pos0 = 0; for (int i = 0; i < ns; i++) { NnTensor *r = newt(); nm_slice_axis(p, ax, pos0, pos0 + sizes[i], 1, r); pos0 += sizes[i]; kc_array_push(out, nn_ret_tensor(r)); }
    targ_free(&x); return out;
}
FN(f_slice_axis) {
    NNAPI(); need_n(n, 4, 5, "slice_axis"); TArg x; NnTensor *p = targ_get(&x, a, n, 0, "slice_axis"); int ax = nt_norm_axis((int)arg_int(a, n, 1, "slice_axis"), p->nd, "slice_axis");
    int64_t s = a[2].type == VAL_NULL ? 0 : arg_int(a, n, 2, "slice_axis"), e = a[3].type == VAL_NULL ? p->shape[ax] : arg_int(a, n, 3, "slice_axis"), st = n > 4 ? arg_int(a, n, 4, "slice_axis") : 1;
    NnTensor *r = newt(); nm_slice_axis(p, ax, s, e, st, r); targ_free(&x); return nn_ret_tensor(r);
}
FN(f_flip) {
    NNAPI(); need_n(n, 1, 2, "flip"); TArg x; NnTensor *p = targ_get(&x, a, n, 0, "flip"); bool m[NN_MAXDIM] = { false };
    if (n > 1 && a[1].type != VAL_NULL) { int ax[NN_MAXDIM], c = int_list(a[1], ax, NN_MAXDIM, "flip"); for (int i = 0; i < c; i++) m[nt_norm_axis(ax[i], p->nd, "flip")] = true; } else for (int i = 0; i < p->nd; i++) m[i] = true;
    NnTensor *r = newt(); nm_flip(p, m, r); targ_free(&x); return nn_ret_tensor(r);
}
FN(f_roll) {
    NNAPI(); need_n(n, 2, 3, "roll"); TArg x; NnTensor *p = targ_get(&x, a, n, 0, "roll"); int64_t sh = arg_int(a, n, 1, "roll"); NnTensor *r = newt();
    if (n > 2 && a[2].type != VAL_NULL) nm_roll(p, sh, nt_norm_axis((int)arg_int(a, n, 2, "roll"), p->nd, "roll"), r);
    else { int one[1] = { (int)p->n }; NnTensor flat = *p; flat.nd = 1; flat.shape[0] = one[0]; for (int i = 1; i < NN_MAXDIM; i++) flat.shape[i] = 0; NnTensor t; nt_init(&t); nm_roll(&flat, sh, 0, &t); t.nd = p->nd; for (int i = 0; i < NN_MAXDIM; i++) t.shape[i] = p->shape[i]; *r = t; }
    targ_free(&x); return nn_ret_tensor(r);
}
FN(f_tile) {
    NNAPI(); need_n(n, 2, 2, "tile"); TArg x; NnTensor *p = targ_get(&x, a, n, 0, "tile"); int reps[NN_MAXDIM], nr = int_list(a[1], reps, NN_MAXDIM, "tile"); NnTensor *r = newt(); nm_tile(p, reps, nr, r); targ_free(&x); return nn_ret_tensor(r);
}
FN(f_repeat) {
    NNAPI(); need_n(n, 2, 3, "repeat"); TArg x; NnTensor *p = targ_get(&x, a, n, 0, "repeat"); int64_t times = arg_int(a, n, 1, "repeat"); NnTensor *r = newt();
    if (n > 2 && a[2].type != VAL_NULL) nm_repeat(p, times, nt_norm_axis((int)arg_int(a, n, 2, "repeat"), p->nd, "repeat"), r);
    else { NnTensor flat = *p; flat.nd = 1; flat.shape[0] = (int)p->n; for (int i = 1; i < NN_MAXDIM; i++) flat.shape[i] = 0; nm_repeat(&flat, times, 0, r); }
    targ_free(&x); return nn_ret_tensor(r);
}
static void parse_pads(KcValue v, int nd, int *bef, int *aft) {
    if (v.type != VAL_ARRAY) { int p = (int)val_int(v, "pad"); for (int i = 0; i < nd; i++) bef[i] = aft[i] = p; return; }
    KcArray *A = v.as.arr;
    if (A->count > 0 && A->items[0].type == VAL_ARRAY) {
        if (A->count != nd) NNERR("nn.pad: expected one [before, after] pair for each of the %d dimensions (got %d)", nd, A->count);
        for (int i = 0; i < nd; i++) { int t[2]; int c = int_list(A->items[i], t, 2, "pad"); if (c == 1) t[1] = t[0]; else if (c != 2) NNERR("nn.pad: each entry must be [before, after]"); bef[i] = t[0]; aft[i] = t[1]; }
        return;
    }
    if (A->count != 2) NNERR("nn.pad: pads must be an integer, a [before, after] pair, or one pair per dimension");
    int b = (int)val_int(A->items[0], "pad"), f = (int)val_int(A->items[1], "pad"); for (int i = 0; i < nd; i++) { bef[i] = b; aft[i] = f; }
}
FN(f_pad) {
    NNAPI(); need_n(n, 2, 4, "pad"); TArg x; NnTensor *p = targ_get(&x, a, n, 0, "pad"); int bef[NN_MAXDIM], aft[NN_MAXDIM]; parse_pads(a[1], p->nd, bef, aft);
    const char *ms = n > 2 ? arg_str(a, n, 2, "pad") : "constant"; int mode = NMP_CONST; double val = n > 3 ? arg_num(a, n, 3, "pad") : 0.0;
    if (!strcmp(ms, "constant")) mode = NMP_CONST; else if (!strcmp(ms, "edge")) mode = NMP_EDGE; else if (!strcmp(ms, "reflect")) mode = NMP_REFLECT; else if (!strcmp(ms, "wrap")) mode = NMP_WRAP;
    else NNERR("nn.pad: unknown mode '%s' (constant, edge, reflect, wrap)", ms);
    NnTensor *r = newt(); nm_pad(p, bef, aft, mode, val, r); targ_free(&x); return nn_ret_tensor(r);
}

/* ---- indexing ---- */
FN(f_take) {
    NNAPI(); need_n(n, 2, 3, "take"); TArg x; NnTensor *p = targ_get(&x, a, n, 0, "take"); int ax = ax_req(a, n, 2, p, 0, "take"); int64_t c; int64_t *idx = index_list(a[1], &c, "take");
    NnTensor *r = newt(); nm_take(p, idx, c, ax, r); free(idx); targ_free(&x); return nn_ret_tensor(r);
}
FN(f_gather) {
    NNAPI(); need_n(n, 3, 3, "gather"); TArg x, y; NnTensor *p = targ_get(&x, a, n, 0, "gather"), *q = targ_get(&y, a, n, 1, "gather"); int ax = nt_norm_axis((int)arg_int(a, n, 2, "gather"), p->nd, "gather");
    NnTensor *r = newt(); nm_gather(p, q, ax, r); targ_free(&x); targ_free(&y); return nn_ret_tensor(r);
}
static KcValue scatter_api(KcValue *a, int n, KcPos pos, bool add, const char *fn) {
    NN_POS = &pos; need_n(n, 4, 4, fn); TArg x, y, z; NnTensor *b = targ_get(&x, a, n, 0, fn), *q = targ_get(&y, a, n, 1, fn), *s = targ_get(&z, a, n, 2, fn); int ax = nt_norm_axis((int)arg_int(a, n, 3, fn), b->nd, fn);
    NnTensor *r = newt(); nm_scatter(b, q, s, ax, add, r); targ_free(&x); targ_free(&y); targ_free(&z); return nn_ret_tensor(r);
}
FN(f_scatter) { return scatter_api(a, n, pos, false, "scatter"); }
FN(f_scatter_add) { return scatter_api(a, n, pos, true, "scatter_add"); }
FN(f_tril) { NNAPI(); need_n(n, 1, 2, "tril"); TArg x; NnTensor *p = targ_get(&x, a, n, 0, "tril"); NnTensor *r = newt(); nm_tri(p, n > 1 ? arg_int(a, n, 1, "tril") : 0, false, r); targ_free(&x); return nn_ret_tensor(r); }
FN(f_triu) { NNAPI(); need_n(n, 1, 2, "triu"); TArg x; NnTensor *p = targ_get(&x, a, n, 0, "triu"); NnTensor *r = newt(); nm_tri(p, n > 1 ? arg_int(a, n, 1, "triu") : 0, true, r); targ_free(&x); return nn_ret_tensor(r); }
FN(f_diag) { NNAPI(); need_n(n, 1, 2, "diag"); TArg x; NnTensor *p = targ_get(&x, a, n, 0, "diag"); NnTensor *r = newt(); nm_diag(p, n > 1 ? arg_int(a, n, 1, "diag") : 0, r); targ_free(&x); return nn_ret_tensor(r); }
FN(f_trace) {
    NNAPI(); need_n(n, 1, 2, "trace"); TArg x; NnTensor *p = targ_get(&x, a, n, 0, "trace"); if (p->nd != 2) NNERR("nn.trace: needs a 2-D tensor (got %s)", nt_shape_str(p->nd, p->shape));
    NnTensor d; nt_init(&d); nm_diag(p, n > 1 ? arg_int(a, n, 1, "trace") : 0, &d); NnTensor *r = newt(); nt_reduce(NTR_SUM, &d, NT_AXIS_ALL, false, r); nt_free(&d); targ_free(&x); return nn_ret_tensor(r);
}

/* ================================================================== products */
FN(f_outer) { NNAPI(); need_n(n, 2, 2, "outer"); TArg x, y; NnTensor *p = targ_get(&x, a, n, 0, "outer"), *q = targ_get(&y, a, n, 1, "outer"); NnTensor *r = newt(); nm_outer(p, q, r); targ_free(&x); targ_free(&y); return nn_ret_tensor(r); }
FN(f_kron) { NNAPI(); need_n(n, 2, 2, "kron"); TArg x, y; NnTensor *p = targ_get(&x, a, n, 0, "kron"), *q = targ_get(&y, a, n, 1, "kron"); NnTensor *r = newt(); nm_kron(p, q, r); targ_free(&x); targ_free(&y); return nn_ret_tensor(r); }
FN(f_cross) { NNAPI(); need_n(n, 2, 2, "cross"); TArg x, y; NnTensor *p = targ_get(&x, a, n, 0, "cross"), *q = targ_get(&y, a, n, 1, "cross"); NnTensor *r = newt(); nm_cross(p, q, r); targ_free(&x); targ_free(&y); return nn_ret_tensor(r); }
FN(f_bmatmul) { NNAPI(); need_n(n, 2, 2, "bmatmul"); TArg x, y; NnTensor *p = targ_get(&x, a, n, 0, "bmatmul"), *q = targ_get(&y, a, n, 1, "bmatmul"); NnTensor *r = newt(); nm_bmatmul(p, q, r); targ_free(&x); targ_free(&y); return nn_ret_tensor(r); }
FN(f_tensordot) {
    NNAPI(); need_n(n, 2, 3, "tensordot"); TArg x, y; NnTensor *p = targ_get(&x, a, n, 0, "tensordot"), *q = targ_get(&y, a, n, 1, "tensordot"); int aa[NN_MAXDIM], ab[NN_MAXDIM], k;
    if (n < 3) { k = 1; if (p->nd < 1 || q->nd < 1) NNERR("nn.tensordot: needs tensors with at least 1 dimension"); aa[0] = p->nd - 1; ab[0] = 0; }
    else if (a[2].type == VAL_ARRAY) {
        if (a[2].as.arr->count != 2) NNERR("nn.tensordot: axes must be an integer or a pair [axes_of_a, axes_of_b]");
        k = int_list(a[2].as.arr->items[0], aa, NN_MAXDIM, "tensordot"); int kb = int_list(a[2].as.arr->items[1], ab, NN_MAXDIM, "tensordot"); if (k != kb) NNERR("nn.tensordot: both axis lists must have the same length");
        for (int i = 0; i < k; i++) { aa[i] = nt_norm_axis(aa[i], p->nd, "tensordot"); ab[i] = nt_norm_axis(ab[i], q->nd, "tensordot"); }
    } else {
        k = (int)arg_int(a, n, 2, "tensordot"); if (k < 0 || k > p->nd || k > q->nd) NNERR("nn.tensordot: cannot contract %d axes of tensors with %d and %d dimensions", k, p->nd, q->nd);
        for (int i = 0; i < k; i++) { aa[i] = p->nd - k + i; ab[i] = i; }
    }
    NnTensor *r = newt(); nm_tensordot(p, q, aa, ab, k, r); targ_free(&x); targ_free(&y); return nn_ret_tensor(r);
}
FN(f_einsum) {
    NNAPI(); if (n < 2) NNERR("nn.einsum expects a subscript string followed by at least one tensor"); const char *spec = arg_str(a, n, 0, "einsum"); int nt = n - 1; if (nt > 8) NNERR("nn.einsum: at most 8 operands are supported");
    TArg ta[8]; const NnTensor *ps[8]; for (int i = 0; i < nt; i++) ps[i] = targ_get(&ta[i], a, n, i + 1, "einsum");
    NnTensor *r = newt(); nm_einsum(spec, ps, nt, r); for (int i = 0; i < nt; i++) targ_free(&ta[i]); return nn_ret_tensor(r);
}

/* ================================================================== statistics */
FN(f_cov) {
    NNAPI(); need_n(n, 1, 2, "cov"); TArg x; NnTensor *p = targ_get(&x, a, n, 0, "cov"); int ddof = n > 1 ? (int)arg_int(a, n, 1, "cov") : 1; NnTensor *r = newt();
    if (p->nd == 1) { NnTensor v = *p; v.nd = 2; v.shape[0] = p->shape[0]; v.shape[1] = 1; nm_cov(&v, ddof, r); } else nm_cov(p, ddof, r); targ_free(&x); return nn_ret_tensor(r);
}
FN(f_corrcoef) {
    NNAPI(); need_n(n, 1, 1, "corrcoef"); TArg x; NnTensor *p = targ_get(&x, a, n, 0, "corrcoef"); NnTensor *r = newt();
    if (p->nd == 1) { NnTensor v = *p; v.nd = 2; v.shape[0] = p->shape[0]; v.shape[1] = 1; nm_corrcoef(&v, r); } else nm_corrcoef(p, r); targ_free(&x); return nn_ret_tensor(r);
}
FN(f_cdist) {
    NNAPI(); need_n(n, 2, 3, "cdist"); TArg x, y; NnTensor *p = targ_get(&x, a, n, 0, "cdist"), *q = targ_get(&y, a, n, 1, "cdist"); double pp = parse_ord(a, n, 2, "cdist");
    NnTensor *r = newt(); nm_cdist(p, q, pp, r); targ_free(&x); targ_free(&y); return nn_ret_tensor(r);
}
FN(f_cosine_similarity) {
    NNAPI(); need_n(n, 2, 4, "cosine_similarity"); TArg x, y; NnTensor *p = targ_get(&x, a, n, 0, "cosine_similarity"), *q = targ_get(&y, a, n, 1, "cosine_similarity");
    if (!nt_same_shape(p, q)) NNERR("nn.cosine_similarity: shapes must match (%s vs %s)", nt_shape_str(p->nd, p->shape), nt_shape_str(q->nd, q->shape));
    int ax = ax_req(a, n, 2, p, -1, "cosine_similarity"); double eps = n > 3 ? arg_num(a, n, 3, "cosine_similarity") : 1e-12;
    NnTensor pr, dot, na, nb, den, e; nt_init(&pr); nt_init(&dot); nt_init(&na); nt_init(&nb); nt_init(&den); nt_init(&e);
    nt_binop(NTB_MUL, p, q, &pr); nt_reduce(NTR_SUM, &pr, ax, false, &dot); nm_reduce(NMR_NORM, p, ax, false, 2.0, &na); nm_reduce(NMR_NORM, q, ax, false, 2.0, &nb);
    nt_binop(NTB_MUL, &na, &nb, &den); nt_resize(&e, 0, NULL); e.d[0] = eps; NnTensor den2; nt_init(&den2); nt_binop(NTB_MAX, &den, &e, &den2); NnTensor *r = newt(); nt_binop(NTB_DIV, &dot, &den2, r);
    nt_free(&pr); nt_free(&dot); nt_free(&na); nt_free(&nb); nt_free(&den); nt_free(&e); nt_free(&den2); targ_free(&x); targ_free(&y); return nn_ret_tensor(r);
}
FN(f_normalize) {
    NNAPI(); need_n(n, 1, 4, "normalize"); TArg x; NnTensor *p = targ_get(&x, a, n, 0, "normalize"); double pp = parse_ord(a, n, 1, "normalize"); int ax = ax_req(a, n, 2, p, -1, "normalize"); double eps = n > 3 ? arg_num(a, n, 3, "normalize") : 1e-12;
    NnTensor nr, e, nr2; nt_init(&nr); nt_init(&e); nt_init(&nr2); nm_reduce(NMR_NORM, p, ax, true, pp, &nr); nt_resize(&e, 0, NULL); e.d[0] = eps; nt_binop(NTB_MAX, &nr, &e, &nr2);
    NnTensor *r = newt(); nt_binop(NTB_DIV, p, &nr2, r); nt_free(&nr); nt_free(&e); nt_free(&nr2); targ_free(&x); return nn_ret_tensor(r);
}
FN(f_histogram) {
    NNAPI(); need_n(n, 2, 4, "histogram"); TArg x; NnTensor *p = targ_get(&x, a, n, 0, "histogram"); int bins = (int)arg_int(a, n, 1, "histogram"); double lo, hi;
    if (n >= 4) { lo = arg_num(a, n, 2, "histogram"); hi = arg_num(a, n, 3, "histogram"); }
    else { lo = INFINITY; hi = -INFINITY; for (size_t i = 0; i < p->n; i++) if (!isnan(p->d[i])) { if (p->d[i] < lo) lo = p->d[i]; if (p->d[i] > hi) hi = p->d[i]; } if (lo > hi) { lo = 0; hi = 1; } }
    NnTensor *r = newt(); nm_histogram(p, bins, lo, hi, r); targ_free(&x); return nn_ret_tensor(r);
}
FN(f_bincount) { NNAPI(); need_n(n, 1, 2, "bincount"); TArg x; NnTensor *p = targ_get(&x, a, n, 0, "bincount"); NnTensor *r = newt(); nm_bincount(p, n > 1 ? (int)arg_int(a, n, 1, "bincount") : 0, r); targ_free(&x); return nn_ret_tensor(r); }
FN(f_convolve) {
    NNAPI(); need_n(n, 2, 3, "convolve"); TArg x, y; NnTensor *p = targ_get(&x, a, n, 0, "convolve"), *q = targ_get(&y, a, n, 1, "convolve"); const char *ms = n > 2 ? arg_str(a, n, 2, "convolve") : "full"; int mode = NMV_FULL;
    if (!strcmp(ms, "full")) mode = NMV_FULL; else if (!strcmp(ms, "same")) mode = NMV_SAME; else if (!strcmp(ms, "valid")) mode = NMV_VALID; else NNERR("nn.convolve: mode must be \"full\", \"same\" or \"valid\"");
    NnTensor *r = newt(); nm_convolve(p, q, mode, r); targ_free(&x); targ_free(&y); return nn_ret_tensor(r);
}

/* ================================================================== sampling */
static void need_shape(KcValue *a, int n, int i, int *sh, int *nd, const char *fn) { *nd = arg_shape(a, n, i, sh, fn); for (int k = 0; k < *nd; k++) if (sh[k] < 0) NNERR("nn.%s: dimensions must be >= 0", fn); }
FN(f_uniform) {
    NNAPI(); need_n(n, 1, 3, "uniform"); int sh[NN_MAXDIM], nd; need_shape(a, n, 0, sh, &nd, "uniform"); double lo = n > 1 ? arg_num(a, n, 1, "uniform") : 0.0, hi = n > 2 ? arg_num(a, n, 2, "uniform") : 1.0;
    if (!(hi >= lo)) NNERR("nn.uniform: high (%g) must be >= low (%g)", hi, lo); NnTensor *t = nt_new(nd, sh); for (size_t i = 0; i < t->n; i++) t->d[i] = lo + (hi - lo) * nn_rand(); return nn_ret_tensor(t);
}
FN(f_normal) {
    NNAPI(); need_n(n, 1, 3, "normal"); int sh[NN_MAXDIM], nd; need_shape(a, n, 0, sh, &nd, "normal"); double mu = n > 1 ? arg_num(a, n, 1, "normal") : 0.0, sd = n > 2 ? arg_num(a, n, 2, "normal") : 1.0;
    if (sd < 0) NNERR("nn.normal: std must be >= 0"); NnTensor *t = nt_new(nd, sh); for (size_t i = 0; i < t->n; i++) t->d[i] = mu + sd * nn_randn(); return nn_ret_tensor(t);
}
FN(f_trunc_normal) {                                                                           /* inverse-CDF sampling: exact, no rejection loop */
    NNAPI(); need_n(n, 5, 5, "trunc_normal"); int sh[NN_MAXDIM], nd; need_shape(a, n, 0, sh, &nd, "trunc_normal");
    double mu = arg_num(a, n, 1, "trunc_normal"), sd = arg_num(a, n, 2, "trunc_normal"), lo = arg_num(a, n, 3, "trunc_normal"), hi = arg_num(a, n, 4, "trunc_normal");
    if (!(sd > 0)) NNERR("nn.trunc_normal: std must be > 0"); if (!(hi > lo)) NNERR("nn.trunc_normal: high must be greater than low");
    double pa = 0.5 * erfc(-((lo - mu) / sd) / 1.4142135623730951), pb = 0.5 * erfc(-((hi - mu) / sd) / 1.4142135623730951);
    NnTensor *t = nt_new(nd, sh);
    for (size_t i = 0; i < t->n; i++) {
        double v; if (!(pb > pa)) v = (lo - mu) > 0 ? lo : hi; else { double u = pa + (pb - pa) * nn_rand(); v = mu + sd * nm_ndtri(u); }
        t->d[i] = v < lo ? lo : (v > hi ? hi : v);
    }
    return nn_ret_tensor(t);
}
FN(f_bernoulli) {
    NNAPI(); need_n(n, 2, 2, "bernoulli"); int sh[NN_MAXDIM], nd; need_shape(a, n, 0, sh, &nd, "bernoulli"); double p = arg_num(a, n, 1, "bernoulli"); if (!(p >= 0 && p <= 1)) NNERR("nn.bernoulli: p must be between 0 and 1 (got %g)", p);
    NnTensor *t = nt_new(nd, sh); for (size_t i = 0; i < t->n; i++) t->d[i] = nn_rand() < p ? 1.0 : 0.0; return nn_ret_tensor(t);
}
FN(f_randperm) {
    NNAPI(); need_n(n, 1, 1, "randperm"); int64_t N = arg_int(a, n, 0, "randperm"); if (N < 0 || N > 2000000000) NNERR("nn.randperm: bad length"); int sh[1] = { (int)N }; NnTensor *t = nt_new(1, sh);
    for (int64_t i = 0; i < N; i++) t->d[i] = (double)i; for (int64_t i = N - 1; i > 0; i--) { int64_t j = nn_randint(i + 1); double s = t->d[i]; t->d[i] = t->d[j]; t->d[j] = s; } return nn_ret_tensor(t);
}
FN(f_multinomial) {                                                                            /* indices drawn proportionally to non-negative weights */
    NNAPI(); need_n(n, 2, 3, "multinomial"); TArg x; NnTensor *p = targ_get(&x, a, n, 0, "multinomial"); int64_t cnt = arg_int(a, n, 1, "multinomial"); bool repl = bool_opt(a, n, 2, true);
    if (p->nd != 1 || p->n == 0) NNERR("nn.multinomial: weights must be a non-empty 1-D tensor"); if (cnt < 0 || cnt > 100000000) NNERR("nn.multinomial: bad sample count");
    size_t N = p->n; double *w = nn_malloc(sizeof(double) * N), total = 0; size_t pos_cnt = 0;
    for (size_t i = 0; i < N; i++) { w[i] = p->d[i]; if (!(w[i] >= 0) || isinf(w[i])) NNERR("nn.multinomial: weights must be finite and non-negative (got %g at position %zu)", w[i], i); total += w[i]; pos_cnt += w[i] > 0; }
    if (!(total > 0)) NNERR("nn.multinomial: weights sum to zero"); if (!repl && (size_t)cnt > pos_cnt) NNERR("nn.multinomial: cannot draw %lld samples without replacement from %zu positive weights", (long long)cnt, pos_cnt);
    int sh[1] = { (int)cnt }; NnTensor *r = nt_new(1, sh);
    for (int64_t k = 0; k < cnt; k++) {
        double u = nn_rand() * total, acc = 0; size_t pick = N - 1; for (size_t i = 0; i < N; i++) { acc += w[i]; if (u < acc && w[i] > 0) { pick = i; break; } }
        while (w[pick] <= 0 && pick > 0) pick--;                                              /* guards against round-off at the upper edge */
        r->d[k] = (double)pick; if (!repl) { total -= w[pick]; w[pick] = 0; }
    }
    free(w); targ_free(&x); return nn_ret_tensor(r);
}

const NnEntry nn_math_entries[] = {
    E("erf", f_erf), E("erfc", f_erfc), E("erfinv", f_erfinv), E("lgamma", f_lgamma), E("digamma", f_digamma), E("log1p", f_log1p), E("expm1", f_expm1),
    E("tan", f_tan), E("asin", f_asin), E("acos", f_acos), E("atan", f_atan), E("sinh", f_sinh), E("cosh", f_cosh), E("asinh", f_asinh), E("acosh", f_acosh), E("atanh", f_atanh),
    E("rsqrt", f_rsqrt), E("reciprocal", f_reciprocal), E("cbrt", f_cbrt), E("log2", f_log2), E("log10", f_log10), E("exp2", f_exp2), E("trunc", f_trunc), E("frac", f_frac),
    E("log_sigmoid", f_log_sigmoid), E("normal_cdf", f_normal_cdf), E("normal_ppf", f_normal_ppf), E("logit", f_logit),
    E("isnan", f_isnan), E("isinf", f_isinf), E("isfinite", f_isfinite), E("logical_not", f_logical_not),
    E("atan2", f_atan2), E("hypot", f_hypot), E("mod", f_mod), E("floor_div", f_floor_div), E("logaddexp", f_logaddexp),
    E("eq", f_eq), E("ne", f_ne), E("lt", f_lt), E("le", f_le), E("gt", f_gt), E("ge", f_ge), E("logical_and", f_logical_and), E("logical_or", f_logical_or), E("logical_xor", f_logical_xor),
    E("where", f_where), E("masked_fill", f_masked_fill), E("nan_to_num", f_nan_to_num), E("lerp", f_lerp), E("clamp", f_clamp), E("equal", f_equal), E("allclose", f_allclose),
    E("activation_grad", f_activation_grad), E("log_softmax_axis", f_log_softmax_axis), E("softmax_grad", f_softmax_grad),
    E("any", f_any), E("all", f_all), E("count_nonzero", f_count_nonzero), E("logsumexp", f_logsumexp), E("nansum", f_nansum), E("nanmean", f_nanmean), E("ptp", f_ptp),
    E("median", f_median), E("quantile", f_quantile), E("norm", f_norm),
    E("cumsum", f_cumsum), E("cumprod", f_cumprod), E("cummax", f_cummax), E("cummin", f_cummin), E("logcumsumexp", f_logcumsumexp), E("diff", f_diff),
    E("sort", f_sort), E("argsort", f_argsort), E("topk", f_topk),
    E("squeeze", f_squeeze), E("unsqueeze", f_unsqueeze), E("broadcast_to", f_broadcast_to), E("swapaxes", f_swapaxes), E("moveaxis", f_moveaxis), E("stack", f_stack), E("split", f_split),
    E("slice_axis", f_slice_axis), E("flip", f_flip), E("roll", f_roll), E("tile", f_tile), E("repeat", f_repeat), E("pad", f_pad),
    E("take", f_take), E("gather", f_gather), E("scatter", f_scatter), E("scatter_add", f_scatter_add), E("tril", f_tril), E("triu", f_triu), E("diag", f_diag), E("trace", f_trace),
    E("outer", f_outer), E("kron", f_kron), E("cross", f_cross), E("bmatmul", f_bmatmul), E("tensordot", f_tensordot), E("einsum", f_einsum),
    E("cov", f_cov), E("corrcoef", f_corrcoef), E("cdist", f_cdist), E("cosine_similarity", f_cosine_similarity), E("normalize", f_normalize), E("histogram", f_histogram), E("bincount", f_bincount), E("convolve", f_convolve),
    E("uniform", f_uniform), E("normal", f_normal), E("trunc_normal", f_trunc_normal), E("bernoulli", f_bernoulli), E("randperm", f_randperm), E("multinomial", f_multinomial),
};
const int nn_math_count = (int)(sizeof nn_math_entries / sizeof nn_math_entries[0]);
