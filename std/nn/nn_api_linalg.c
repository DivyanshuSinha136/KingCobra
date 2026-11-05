/* KingCobra std.nn script API — linear algebra (det, inv, solve, LU, Cholesky, QR, eigh, SVD, pinv, lstsq, norms, expm ...).
 * Numeric kernels live in nn_linalg.c.  Square-matrix routines accept batches ([..., n, n]); decompositions with
 * several outputs (qr, eigh, svd, lu) take a single 2-D matrix.
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com> */
#include "nn_glue.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>

#define FN(name) static KcValue name(KcInterp *ip, KcValue *a, int n, KcPos pos)
#define E(name, fn) { name, fn }
#define EPS 2.220446049250313e-16
static NnTensor *newt(void) { NnTensor *t = nn_malloc(sizeof *t); nt_init(t); return t; }

static void sq_dims(const NnTensor *t, const char *fn, int *dim, size_t *batch) {
    if (t->nd < 2 || t->shape[t->nd - 1] != t->shape[t->nd - 2]) NNERR("nn.%s: needs a square matrix or a batch of square matrices (got %s)", fn, nt_shape_str(t->nd, t->shape));
    *dim = t->shape[t->nd - 1]; size_t b = 1; for (int i = 0; i < t->nd - 2; i++) b *= (size_t)t->shape[i]; *batch = b;
}
static void need_2d(const NnTensor *t, const char *fn) { if (t->nd != 2) NNERR("nn.%s: needs a 2-D matrix (got %s)", fn, nt_shape_str(t->nd, t->shape)); }
static NnTensor *mat2(int r, int c) { int sh[2] = { r, c }; NnTensor *t = newt(); nt_alloc(t, 2, sh); return t; }
static NnTensor *vec1(int n) { int sh[1] = { n }; NnTensor *t = newt(); nt_alloc(t, 1, sh); return t; }

/* ================================================================== determinants, inverse, solve */
FN(f_det) {
    NNAPI(); need_n(n, 1, 1, "det"); TArg x; NnTensor *p = targ_get(&x, a, n, 0, "det"); int d; size_t B; sq_dims(p, "det", &d, &B);
    NnTensor *r = newt(); nt_alloc(r, p->nd - 2, p->shape); size_t mm = (size_t)d * (size_t)d; for (size_t b = 0; b < B; b++) r->d[b] = nl_det(p->d + b * mm, d);
    targ_free(&x); return nn_ret_tensor(r);
}
FN(f_slogdet) {
    NNAPI(); need_n(n, 1, 1, "slogdet"); TArg x; NnTensor *p = targ_get(&x, a, n, 0, "slogdet"); int d; size_t B; sq_dims(p, "slogdet", &d, &B);
    NnTensor *s = newt(), *l = newt(); nt_alloc(s, p->nd - 2, p->shape); nt_alloc(l, p->nd - 2, p->shape); size_t mm = (size_t)d * (size_t)d;
    for (size_t b = 0; b < B; b++) nl_slogdet(p->d + b * mm, d, &s->d[b], &l->d[b]);
    targ_free(&x); const char *nm[] = { "sign", "logabsdet" }; KcValue v[] = { nn_ret_tensor(s), nn_ret_tensor(l) }; return mk_struct("SlogDet", 2, nm, v);
}
FN(f_inv) {
    NNAPI(); need_n(n, 1, 1, "inv"); TArg x; NnTensor *p = targ_get(&x, a, n, 0, "inv"); int d; size_t B; sq_dims(p, "inv", &d, &B);
    NnTensor *r = newt(); nt_alloc(r, p->nd, p->shape); size_t mm = (size_t)d * (size_t)d;
    for (size_t b = 0; b < B; b++) if (!nl_inv(p->d + b * mm, d, r->d + b * mm)) NNERR("nn.inv: matrix is singular%s", B > 1 ? " (in batch entry)" : "");
    targ_free(&x); return nn_ret_tensor(r);
}
FN(f_solve) {                                                                                  /* A [..,n,n]; B [n] | [n,k] (shared) | [..,n,k] (same batch) */
    NNAPI(); need_n(n, 2, 2, "solve"); TArg x, y; NnTensor *A = targ_get(&x, a, n, 0, "solve"), *Bt = targ_get(&y, a, n, 1, "solve"); int d; size_t B; sq_dims(A, "solve", &d, &B);
    bool vec = Bt->nd == 1, shared = vec || (Bt->nd == 2 && A->nd > 2); int k;
    if (vec) { if (Bt->shape[0] != d) NNERR("nn.solve: right-hand side has %d entries but the matrix is %dx%d", Bt->shape[0], d, d); k = 1; }
    else {
        if (Bt->nd < 2 || Bt->shape[Bt->nd - 2] != d) NNERR("nn.solve: right-hand side %s does not match the %dx%d matrix", nt_shape_str(Bt->nd, Bt->shape), d, d);
        if (!shared) { if (Bt->nd != A->nd) NNERR("nn.solve: right-hand side %s must have the same batch dimensions as %s", nt_shape_str(Bt->nd, Bt->shape), nt_shape_str(A->nd, A->shape)); for (int i = 0; i < A->nd - 2; i++) if (A->shape[i] != Bt->shape[i]) NNERR("nn.solve: batch dimensions of the matrix and right-hand side differ"); }
        k = Bt->shape[Bt->nd - 1];
    }
    int sh[NN_MAXDIM], nd = 0; for (int i = 0; i < A->nd - 2; i++) sh[nd++] = A->shape[i]; sh[nd++] = d; if (!vec) sh[nd++] = k;
    NnTensor *r = newt(); nt_alloc(r, nd, sh); size_t mm = (size_t)d * (size_t)d, bm = (size_t)d * (size_t)k;
    for (size_t b = 0; b < B; b++) if (!nl_solve(A->d + b * mm, d, Bt->d + (shared ? 0 : b * bm), k, r->d + b * bm)) NNERR("nn.solve: matrix is singular");
    targ_free(&x); targ_free(&y); return nn_ret_tensor(r);
}

/* ================================================================== factorisations */
FN(f_lu) {                                                                                     /* A = P L U */
    NNAPI(); need_n(n, 1, 1, "lu"); TArg x; NnTensor *p = targ_get(&x, a, n, 0, "lu"); need_2d(p, "lu"); int d = p->shape[0]; if (p->shape[1] != d) NNERR("nn.lu: needs a square matrix (got %s)", nt_shape_str(2, p->shape));
    double *w = nn_malloc(sizeof(double) * (size_t)(d ? d : 1) * (size_t)(d ? d : 1)); int *piv = nn_malloc(sizeof(int) * (size_t)(d ? d : 1)), *perm = nn_malloc(sizeof(int) * (size_t)(d ? d : 1));
    memcpy(w, p->d, sizeof(double) * (size_t)d * (size_t)d); nl_lu(w, d, piv); for (int i = 0; i < d; i++) perm[i] = i; for (int i = 0; i < d; i++) { int t = perm[i]; perm[i] = perm[piv[i]]; perm[piv[i]] = t; }
    NnTensor *P = mat2(d, d), *L = mat2(d, d), *U = mat2(d, d);
    for (int i = 0; i < d; i++) { P->d[(size_t)perm[i] * d + i] = 1.0; for (int j = 0; j < d; j++) { double v = w[(size_t)i * d + j]; if (j < i) L->d[(size_t)i * d + j] = v; else U->d[(size_t)i * d + j] = v; } L->d[(size_t)i * d + i] = 1.0; }
    free(w); free(piv); free(perm); targ_free(&x); const char *nm[] = { "p", "l", "u" }; KcValue v[] = { nn_ret_tensor(P), nn_ret_tensor(L), nn_ret_tensor(U) }; return mk_struct("LU", 3, nm, v);
}
FN(f_cholesky) {
    NNAPI(); need_n(n, 1, 1, "cholesky"); TArg x; NnTensor *p = targ_get(&x, a, n, 0, "cholesky"); int d; size_t B; sq_dims(p, "cholesky", &d, &B);
    NnTensor *r = newt(); nt_alloc(r, p->nd, p->shape); size_t mm = (size_t)d * (size_t)d;
    for (size_t b = 0; b < B; b++) if (!nl_cholesky(p->d + b * mm, d, r->d + b * mm)) NNERR("nn.cholesky: matrix is not positive definite");
    targ_free(&x); return nn_ret_tensor(r);
}
FN(f_chol_solve) {                                                                             /* solve A X = B given the Cholesky factor L of A */
    NNAPI(); need_n(n, 2, 2, "chol_solve"); TArg x, y; NnTensor *L = targ_get(&x, a, n, 0, "chol_solve"), *Bt = targ_get(&y, a, n, 1, "chol_solve"); need_2d(L, "chol_solve"); int d = L->shape[0];
    if (L->shape[1] != d) NNERR("nn.chol_solve: L must be square"); bool vec = Bt->nd == 1; if (Bt->nd < 1 || Bt->nd > 2 || Bt->shape[0] != d) NNERR("nn.chol_solve: right-hand side %s does not match the %dx%d factor", nt_shape_str(Bt->nd, Bt->shape), d, d);
    NnTensor *r = newt(); nt_copy(r, Bt); nl_chol_solve(L->d, d, r->d, vec ? 1 : Bt->shape[1]); targ_free(&x); targ_free(&y); return nn_ret_tensor(r);
}
FN(f_qr) {
    NNAPI(); need_n(n, 1, 1, "qr"); TArg x; NnTensor *p = targ_get(&x, a, n, 0, "qr"); need_2d(p, "qr"); int m = p->shape[0], c = p->shape[1], k = m < c ? m : c;
    NnTensor *Q = mat2(m, k), *R = mat2(k, c); if (m && c) nl_qr(p->d, m, c, Q->d, R->d); targ_free(&x); const char *nm[] = { "q", "r" }; KcValue v[] = { nn_ret_tensor(Q), nn_ret_tensor(R) }; return mk_struct("QR", 2, nm, v);
}
FN(f_eigh) {
    NNAPI(); need_n(n, 1, 1, "eigh"); TArg x; NnTensor *p = targ_get(&x, a, n, 0, "eigh"); need_2d(p, "eigh"); int d = p->shape[0]; if (p->shape[1] != d) NNERR("nn.eigh: needs a square matrix (got %s)", nt_shape_str(2, p->shape));
    NnTensor *w = vec1(d), *V = mat2(d, d); nl_eigh(p->d, d, w->d, V->d); targ_free(&x); const char *nm[] = { "values", "vectors" }; KcValue v[] = { nn_ret_tensor(w), nn_ret_tensor(V) }; return mk_struct("Eigh", 2, nm, v);
}
FN(f_eigvalsh) {
    NNAPI(); need_n(n, 1, 1, "eigvalsh"); TArg x; NnTensor *p = targ_get(&x, a, n, 0, "eigvalsh"); need_2d(p, "eigvalsh"); int d = p->shape[0]; if (p->shape[1] != d) NNERR("nn.eigvalsh: needs a square matrix (got %s)", nt_shape_str(2, p->shape));
    NnTensor *w = vec1(d); double *V = nn_malloc(sizeof(double) * (size_t)(d ? d : 1) * (size_t)(d ? d : 1)); nl_eigh(p->d, d, w->d, V); free(V); targ_free(&x); return nn_ret_tensor(w);
}

/* ---- SVD family ---- */
typedef struct { int m, n, k; double *U, *S, *Vt; } Svd;
static Svd svd_of(const NnTensor *p, const char *fn) {
    need_2d(p, fn); Svd s; s.m = p->shape[0]; s.n = p->shape[1]; s.k = s.m < s.n ? s.m : s.n;
    for (size_t i = 0; i < p->n; i++) if (!isfinite(p->d[i])) NNERR("nn.%s: the matrix contains NaN or infinity", fn);
    s.U = nn_calloc((size_t)s.m * (size_t)(s.k ? s.k : 1), sizeof(double)); s.S = nn_calloc((size_t)(s.k ? s.k : 1), sizeof(double)); s.Vt = nn_calloc((size_t)(s.k ? s.k : 1) * (size_t)s.n, sizeof(double));
    nl_svd(p->d, s.m, s.n, s.U, s.S, s.Vt); return s;
}
static void svd_free(Svd *s) { free(s->U); free(s->S); free(s->Vt); }
FN(f_svd) {
    NNAPI(); need_n(n, 1, 1, "svd"); TArg x; NnTensor *p = targ_get(&x, a, n, 0, "svd"); Svd s = svd_of(p, "svd"); NnTensor *U = mat2(s.m, s.k), *S = vec1(s.k), *Vt = mat2(s.k, s.n);
    memcpy(U->d, s.U, sizeof(double) * U->n); memcpy(S->d, s.S, sizeof(double) * S->n); memcpy(Vt->d, s.Vt, sizeof(double) * Vt->n); svd_free(&s); targ_free(&x);
    const char *nm[] = { "u", "s", "vt" }; KcValue v[] = { nn_ret_tensor(U), nn_ret_tensor(S), nn_ret_tensor(Vt) }; return mk_struct("SVD", 3, nm, v);
}
FN(f_svdvals) {
    NNAPI(); need_n(n, 1, 1, "svdvals"); TArg x; NnTensor *p = targ_get(&x, a, n, 0, "svdvals"); Svd s = svd_of(p, "svdvals"); NnTensor *S = vec1(s.k); memcpy(S->d, s.S, sizeof(double) * S->n); svd_free(&s); targ_free(&x); return nn_ret_tensor(S);
}
static double default_rcond(const Svd *s) { return (double)(s->m > s->n ? s->m : s->n) * EPS; }
static NnTensor *pinv_from(const Svd *s, double rcond, int *rank) {                              /* V diag(1/sigma) U^T over sigma > rcond * sigma_max */
    NnTensor *r = mat2(s->n, s->m); double tol = rcond * (s->k ? s->S[0] : 0); int rk = 0;
    for (int t = 0; t < s->k; t++) {
        if (!(s->S[t] > tol)) continue; rk++; double inv = 1.0 / s->S[t];
        for (int j = 0; j < s->n; j++) { double vj = s->Vt[(size_t)t * s->n + j] * inv; if (vj != 0) for (int i = 0; i < s->m; i++) r->d[(size_t)j * s->m + i] += vj * s->U[(size_t)i * s->k + t]; }
    }
    if (rank) *rank = rk; return r;
}
FN(f_pinv) {
    NNAPI(); need_n(n, 1, 2, "pinv"); TArg x; NnTensor *p = targ_get(&x, a, n, 0, "pinv"); Svd s = svd_of(p, "pinv"); double rc = n > 1 ? arg_num(a, n, 1, "pinv") : default_rcond(&s); if (rc < 0) NNERR("nn.pinv: rcond must be >= 0");
    NnTensor *r = pinv_from(&s, rc, NULL); svd_free(&s); targ_free(&x); return nn_ret_tensor(r);
}
FN(f_lstsq) {                                                                                  /* minimum-norm least squares: argmin |A x - b| */
    NNAPI(); need_n(n, 2, 3, "lstsq"); TArg x, y; NnTensor *A = targ_get(&x, a, n, 0, "lstsq"), *Bt = targ_get(&y, a, n, 1, "lstsq"); Svd s = svd_of(A, "lstsq");
    bool vec = Bt->nd == 1; if (Bt->nd < 1 || Bt->nd > 2 || Bt->shape[0] != s.m) NNERR("nn.lstsq: right-hand side %s does not match the %dx%d matrix", nt_shape_str(Bt->nd, Bt->shape), s.m, s.n);
    double rc = n > 2 ? arg_num(a, n, 2, "lstsq") : default_rcond(&s); int rank = 0; NnTensor *P = pinv_from(&s, rc, &rank); int k = vec ? 1 : Bt->shape[1];
    NnTensor *X = vec ? vec1(s.n) : mat2(s.n, k); nl_matmul(s.n, k, s.m, P->d, Bt->d, X->d); NnTensor *S = vec1(s.k); memcpy(S->d, s.S, sizeof(double) * S->n);
    nt_free(P); free(P); svd_free(&s); targ_free(&x); targ_free(&y); const char *nm[] = { "x", "rank", "s" }; KcValue v[] = { nn_ret_tensor(X), kc_int(rank), nn_ret_tensor(S) }; return mk_struct("LstSq", 3, nm, v);
}
FN(f_matrix_rank) {
    NNAPI(); need_n(n, 1, 2, "matrix_rank"); TArg x; NnTensor *p = targ_get(&x, a, n, 0, "matrix_rank"); Svd s = svd_of(p, "matrix_rank"); double smax = s.k ? s.S[0] : 0, tol = n > 1 ? arg_num(a, n, 1, "matrix_rank") : smax * default_rcond(&s);
    int rk = 0; for (int i = 0; i < s.k; i++) if (s.S[i] > tol) rk++; svd_free(&s); targ_free(&x); return kc_int(rk);
}

/* ---- norms and condition number ---- */
static double matrix_norm_of(const NnTensor *p, KcValue *a, int n, int i, const char *fn) {
    need_2d(p, fn); int R = p->shape[0], C = p->shape[1];
    if (i >= n || a[i].type == VAL_NULL) { NnTensor r; nt_init(&r); nm_reduce(NMR_NORM, p, NT_AXIS_ALL, false, 2.0, &r); double v = r.d[0]; nt_free(&r); return v; }
    if (a[i].type == VAL_STRING) {
        const char *s = a[i].as.s->data;
        if (!strcmp(s, "fro")) { NnTensor r; nt_init(&r); nm_reduce(NMR_NORM, p, NT_AXIS_ALL, false, 2.0, &r); double v = r.d[0]; nt_free(&r); return v; }
        if (!strcmp(s, "nuc")) { Svd sv = svd_of(p, fn); double t = 0; for (int q = 0; q < sv.k; q++) t += sv.S[q]; svd_free(&sv); return t; }
        if (!strcmp(s, "inf") || !strcmp(s, "-inf")) {
            bool neg = s[0] == '-'; double best = neg ? INFINITY : 0; for (int r = 0; r < R; r++) { double t = 0; for (int c = 0; c < C; c++) t += fabs(p->d[(size_t)r * C + c]); if (neg ? t < best : t > best) best = t; } return R ? best : 0;
        }
        NNERR("nn.%s: unknown matrix norm '%s' (\"fro\", \"nuc\", \"inf\", \"-inf\", 1, -1, 2, -2)", fn, s);
    }
    double o = arg_num(a, n, i, fn);
    if (o == 1 || o == -1) { double best = o < 0 ? INFINITY : 0; for (int c = 0; c < C; c++) { double t = 0; for (int r = 0; r < R; r++) t += fabs(p->d[(size_t)r * C + c]); if (o < 0 ? t < best : t > best) best = t; } return C ? best : 0; }
    if (o == 2 || o == -2) { Svd sv = svd_of(p, fn); double v = sv.k ? (o > 0 ? sv.S[0] : sv.S[sv.k - 1]) : 0; svd_free(&sv); return v; }
    NNERR("nn.%s: unsupported matrix norm order %g (use 1, -1, 2, -2, \"inf\", \"-inf\", \"fro\" or \"nuc\")", fn, o); return 0;
}
FN(f_matrix_norm) { NNAPI(); need_n(n, 1, 2, "matrix_norm"); TArg x; NnTensor *p = targ_get(&x, a, n, 0, "matrix_norm"); double v = matrix_norm_of(p, a, n, 1, "matrix_norm"); targ_free(&x); return kc_float(v); }
FN(f_cond) {
    NNAPI(); need_n(n, 1, 2, "cond"); TArg x; NnTensor *p = targ_get(&x, a, n, 0, "cond"); need_2d(p, "cond");
    bool two = n < 2 || a[1].type == VAL_NULL || (a[1].type != VAL_STRING && fabs(arg_num(a, n, 1, "cond")) == 2.0), neg2 = n >= 2 && a[1].type != VAL_STRING && a[1].type != VAL_NULL && arg_num(a, n, 1, "cond") == -2.0;
    double v;
    if (two) { Svd s = svd_of(p, "cond"); if (s.k == 0) NNERR("nn.cond: empty matrix"); double hi = s.S[0], lo = s.S[s.k - 1]; v = neg2 ? (hi == 0 ? INFINITY : lo / hi) : (lo == 0 ? INFINITY : hi / lo); svd_free(&s); }
    else {
        if (p->shape[0] != p->shape[1]) NNERR("nn.cond: this norm order needs a square matrix");
        int d = p->shape[0]; NnTensor inv; nt_init(&inv); nt_alloc(&inv, 2, p->shape);
        if (!nl_inv(p->d, d, inv.d)) v = INFINITY; else v = matrix_norm_of(p, a, n, 1, "cond") * matrix_norm_of(&inv, a, n, 1, "cond");
        nt_free(&inv);
    }
    targ_free(&x); return kc_float(v);
}

/* ---- matrix functions ---- */
FN(f_matrix_power) {
    NNAPI(); need_n(n, 2, 2, "matrix_power"); TArg x; NnTensor *p = targ_get(&x, a, n, 0, "matrix_power"); int64_t e = arg_int(a, n, 1, "matrix_power"); int d; size_t B; sq_dims(p, "matrix_power", &d, &B);
    if (e > 100000000 || e < -100000000) NNERR("nn.matrix_power: exponent is too large"); NnTensor *r = newt(); nt_alloc(r, p->nd, p->shape); size_t mm = (size_t)d * (size_t)d;
    double *base = nn_malloc(sizeof(double) * (mm ? mm : 1)), *acc = nn_malloc(sizeof(double) * (mm ? mm : 1)), *tmp = nn_malloc(sizeof(double) * (mm ? mm : 1));
    for (size_t b = 0; b < B; b++) {
        if (e < 0) { if (!nl_inv(p->d + b * mm, d, base)) NNERR("nn.matrix_power: matrix is singular, cannot raise to a negative power"); } else memcpy(base, p->d + b * mm, sizeof(double) * mm);
        memset(acc, 0, sizeof(double) * mm); for (int i = 0; i < d; i++) acc[(size_t)i * d + i] = 1.0;
        for (int64_t k = e < 0 ? -e : e; k > 0; k >>= 1) { if (k & 1) { nl_matmul(d, d, d, acc, base, tmp); memcpy(acc, tmp, sizeof(double) * mm); } if (k > 1) { nl_matmul(d, d, d, base, base, tmp); memcpy(base, tmp, sizeof(double) * mm); } }
        memcpy(r->d + b * mm, acc, sizeof(double) * mm);
    }
    free(base); free(acc); free(tmp); targ_free(&x); return nn_ret_tensor(r);
}
FN(f_expm) {
    NNAPI(); need_n(n, 1, 1, "expm"); TArg x; NnTensor *p = targ_get(&x, a, n, 0, "expm"); int d; size_t B; sq_dims(p, "expm", &d, &B); NnTensor *r = newt(); nt_alloc(r, p->nd, p->shape); size_t mm = (size_t)d * (size_t)d;
    for (size_t b = 0; b < B; b++) nl_expm(p->d + b * mm, d, r->d + b * mm); targ_free(&x); return nn_ret_tensor(r);
}

const NnEntry nn_linalg_entries[] = {
    E("det", f_det), E("slogdet", f_slogdet), E("inv", f_inv), E("solve", f_solve), E("lu", f_lu), E("cholesky", f_cholesky), E("chol_solve", f_chol_solve),
    E("qr", f_qr), E("eigh", f_eigh), E("eigvalsh", f_eigvalsh), E("svd", f_svd), E("svdvals", f_svdvals), E("pinv", f_pinv), E("lstsq", f_lstsq),
    E("matrix_rank", f_matrix_rank), E("matrix_norm", f_matrix_norm), E("cond", f_cond), E("matrix_power", f_matrix_power), E("expm", f_expm),
};
const int nn_linalg_count = (int)(sizeof nn_linalg_entries / sizeof nn_linalg_entries[0]);
