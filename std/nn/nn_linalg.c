/* KingCobra std.nn — dense linear algebra kernels (double precision, row-major).
 *
 *   LU with partial pivoting (det / slogdet / inverse / solve), Cholesky, Householder QR,
 *   symmetric eigendecomposition (cyclic Jacobi), SVD (one-sided Jacobi), matrix exponential.
 *
 * Jacobi methods are chosen deliberately: they are short, unconditionally convergent and deliver
 * high relative accuracy (small singular values and eigenvalues are not swamped by the large ones).
 * Cost is O(n^3) per sweep with a handful of sweeps, which is fine for the matrix sizes used in
 * game/AI tooling; this is not a replacement for LAPACK on thousand-by-thousand problems.
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com>
 */
#include "nn_internal.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define A_(i, j) A[(size_t)(i) * (size_t)n + (size_t)(j)]

void nl_matmul(int M, int N, int K, const double *A, const double *B, double *C) {
    memset(C, 0, sizeof(double) * (size_t)M * (size_t)N);
    if (M && N && K) nn_gemm_nn(M, N, K, A, B, C);
}

/* ================================================================== LU */
int nl_lu(double *A, int n, int *piv) {                                    /* keeps factoring past a zero pivot so lu() of a singular matrix is still meaningful */
    int sign = 1; bool singular = false;
    for (int k = 0; k < n; k++) {
        int p = k; double mx = fabs(A_(k, k));
        for (int i = k + 1; i < n; i++) if (fabs(A_(i, k)) > mx) { mx = fabs(A_(i, k)); p = i; }
        piv[k] = p;
        if (mx == 0) { singular = true; continue; }                        /* whole column is zero: nothing to eliminate */
        if (p != k) { for (int j = 0; j < n; j++) { double t = A_(k, j); A_(k, j) = A_(p, j); A_(p, j) = t; } sign = -sign; }
        double d = A_(k, k);
        for (int i = k + 1; i < n; i++) {
            double f = A_(i, k) / d; A_(i, k) = f;
            if (f != 0) for (int j = k + 1; j < n; j++) A_(i, j) -= f * A_(k, j);
        }
    }
    return singular ? 0 : sign;
}
void nl_lu_solve(const double *A, const int *piv, int n, double *B, int k) {
    for (int i = 0; i < n; i++) if (piv[i] != i) for (int c = 0; c < k; c++) { double t = B[(size_t)i * k + c]; B[(size_t)i * k + c] = B[(size_t)piv[i] * k + c]; B[(size_t)piv[i] * k + c] = t; }
    for (int i = 0; i < n; i++)                                            /* L y = P b  (unit lower) */
        for (int j = 0; j < i; j++) { double f = A_(i, j); if (f != 0) for (int c = 0; c < k; c++) B[(size_t)i * k + c] -= f * B[(size_t)j * k + c]; }
    for (int i = n - 1; i >= 0; i--) {                                     /* U x = y */
        for (int j = i + 1; j < n; j++) { double f = A_(i, j); if (f != 0) for (int c = 0; c < k; c++) B[(size_t)i * k + c] -= f * B[(size_t)j * k + c]; }
        double d = A_(i, i); for (int c = 0; c < k; c++) B[(size_t)i * k + c] /= d;
    }
}
double nl_det(const double *A0, int n) {
    if (n == 0) return 1.0;
    double *A = nn_malloc(sizeof(double) * (size_t)n * (size_t)n); int *piv = nn_malloc(sizeof(int) * (size_t)n);
    memcpy(A, A0, sizeof(double) * (size_t)n * (size_t)n); int s = nl_lu(A, n, piv); double d = (double)s;
    if (s != 0) for (int i = 0; i < n; i++) d *= A_(i, i);
    free(A); free(piv); return d;
}
void nl_slogdet(const double *A0, int n, double *sign, double *logabs) {
    if (n == 0) { *sign = 1; *logabs = 0; return; }
    double *A = nn_malloc(sizeof(double) * (size_t)n * (size_t)n); int *piv = nn_malloc(sizeof(int) * (size_t)n);
    memcpy(A, A0, sizeof(double) * (size_t)n * (size_t)n); int s = nl_lu(A, n, piv);
    if (s == 0) { *sign = 0; *logabs = -INFINITY; }
    else { double sg = (double)s, la = 0; for (int i = 0; i < n; i++) { double d = A_(i, i); if (d < 0) sg = -sg; la += log(fabs(d)); } *sign = sg; *logabs = la; }
    free(A); free(piv);
}
bool nl_solve(const double *A0, int n, const double *B, int k, double *X) {
    if (n == 0) return true;
    double *A = nn_malloc(sizeof(double) * (size_t)n * (size_t)n); int *piv = nn_malloc(sizeof(int) * (size_t)n);
    memcpy(A, A0, sizeof(double) * (size_t)n * (size_t)n); bool ok = nl_lu(A, n, piv) != 0;
    if (ok) { if (X != B) memcpy(X, B, sizeof(double) * (size_t)n * (size_t)k); nl_lu_solve(A, piv, n, X, k); }
    free(A); free(piv); return ok;
}
bool nl_inv(const double *A, int n, double *out) {
    if (n == 0) return true;
    double *I = nn_calloc((size_t)n * (size_t)n, sizeof(double)); for (int i = 0; i < n; i++) I[(size_t)i * (size_t)n + (size_t)i] = 1.0;
    bool ok = nl_solve(A, n, I, n, out); free(I); return ok;
}

/* ================================================================== Cholesky */
bool nl_cholesky(const double *A, int n, double *L) {
    memset(L, 0, sizeof(double) * (size_t)n * (size_t)n);
    for (int j = 0; j < n; j++) {
        double s = A_(j, j); for (int k = 0; k < j; k++) s -= L[(size_t)j * n + k] * L[(size_t)j * n + k];
        if (!(s > 0)) return false;                                        /* also rejects NaN */
        double d = sqrt(s); L[(size_t)j * n + j] = d;
        for (int i = j + 1; i < n; i++) {
            double v = A_(i, j); for (int k = 0; k < j; k++) v -= L[(size_t)i * n + k] * L[(size_t)j * n + k];
            L[(size_t)i * n + j] = v / d;
        }
    }
    return true;
}
void nl_chol_solve(const double *L, int n, double *B, int k) {
    for (int i = 0; i < n; i++) {                                          /* L y = b */
        for (int j = 0; j < i; j++) { double f = L[(size_t)i * n + j]; for (int c = 0; c < k; c++) B[(size_t)i * k + c] -= f * B[(size_t)j * k + c]; }
        for (int c = 0; c < k; c++) B[(size_t)i * k + c] /= L[(size_t)i * n + i];
    }
    for (int i = n - 1; i >= 0; i--) {                                     /* L^T x = y */
        for (int j = i + 1; j < n; j++) { double f = L[(size_t)j * n + i]; for (int c = 0; c < k; c++) B[(size_t)i * k + c] -= f * B[(size_t)j * k + c]; }
        for (int c = 0; c < k; c++) B[(size_t)i * k + c] /= L[(size_t)i * n + i];
    }
}

/* ================================================================== QR (Householder, reduced form) */
void nl_qr(const double *A, int m, int n, double *Q, double *R) {
    int k = m < n ? m : n;
    double *W = nn_malloc(sizeof(double) * (size_t)m * (size_t)n), *V = nn_calloc((size_t)m * (size_t)(k ? k : 1), sizeof(double)), *beta = nn_calloc((size_t)(k ? k : 1), sizeof(double));
    memcpy(W, A, sizeof(double) * (size_t)m * (size_t)n);
#define Wm(i, c) W[(size_t)(i) * (size_t)n + (size_t)(c)]
#define Vm(i, j) V[(size_t)(i) * (size_t)k + (size_t)(j)]
    for (int j = 0; j < k; j++) {
        double nrm = 0; for (int i = j; i < m; i++) nrm += Wm(i, j) * Wm(i, j); nrm = sqrt(nrm);
        if (nrm == 0) continue;
        double alpha = Wm(j, j) > 0 ? -nrm : nrm;
        for (int i = j; i < m; i++) Vm(i, j) = Wm(i, j);
        Vm(j, j) -= alpha;
        double vn = 0; for (int i = j; i < m; i++) vn += Vm(i, j) * Vm(i, j);
        if (vn == 0) continue;
        beta[j] = 2.0 / vn;
        for (int c = j; c < n; c++) { double dot = 0; for (int i = j; i < m; i++) dot += Vm(i, j) * Wm(i, c); double f = beta[j] * dot; for (int i = j; i < m; i++) Wm(i, c) -= f * Vm(i, j); }
    }
    for (int i = 0; i < k; i++) for (int c = 0; c < n; c++) R[(size_t)i * (size_t)n + (size_t)c] = c >= i ? Wm(i, c) : 0.0;
    memset(Q, 0, sizeof(double) * (size_t)m * (size_t)k); for (int i = 0; i < k; i++) Q[(size_t)i * (size_t)k + (size_t)i] = 1.0;
    for (int j = k - 1; j >= 0; j--) {
        if (beta[j] == 0) continue;
        for (int c = 0; c < k; c++) { double dot = 0; for (int i = j; i < m; i++) dot += Vm(i, j) * Q[(size_t)i * k + c]; double f = beta[j] * dot; for (int i = j; i < m; i++) Q[(size_t)i * k + c] -= f * Vm(i, j); }
    }
#undef Wm
#undef Vm
    free(W); free(V); free(beta);
}

/* ================================================================== symmetric eigendecomposition: cyclic Jacobi */
void nl_eigh(const double *A0, int n, double *w, double *V) {
    double *A = nn_malloc(sizeof(double) * (size_t)(n ? n : 1) * (size_t)(n ? n : 1));
    for (int i = 0; i < n; i++) for (int j = 0; j <= i; j++) { A_(i, j) = A0[(size_t)i * n + j]; A_(j, i) = A0[(size_t)i * n + j]; }   /* lower triangle wins */
    memset(V, 0, sizeof(double) * (size_t)n * (size_t)n); for (int i = 0; i < n; i++) V[(size_t)i * n + i] = 1.0;
    double fro = 0; for (size_t i = 0; i < (size_t)n * (size_t)n; i++) fro += A[i] * A[i];
    for (int sweep = 0; sweep < 100; sweep++) {
        double off = 0; for (int i = 0; i < n; i++) for (int j = i + 1; j < n; j++) off += A_(i, j) * A_(i, j);
        if (!(off > 1e-32 * fro) || off == 0) break;                       /* relative off-diagonal energy ~1e-16 */
        for (int p = 0; p < n - 1; p++) for (int q = p + 1; q < n; q++) {
            double apq = A_(p, q); if (apq == 0) continue;
            double theta = (A_(q, q) - A_(p, p)) / (2.0 * apq);
            double t = (theta >= 0 ? 1.0 : -1.0) / (fabs(theta) + sqrt(theta * theta + 1.0)), c = 1.0 / sqrt(t * t + 1.0), s = t * c;
            for (int k = 0; k < n; k++) { double akp = A_(k, p), akq = A_(k, q); A_(k, p) = c * akp - s * akq; A_(k, q) = s * akp + c * akq; }
            for (int k = 0; k < n; k++) { double apk = A_(p, k), aqk = A_(q, k); A_(p, k) = c * apk - s * aqk; A_(q, k) = s * apk + c * aqk; }
            A_(p, q) = A_(q, p) = 0;
            for (int k = 0; k < n; k++) { double vkp = V[(size_t)k * n + p], vkq = V[(size_t)k * n + q]; V[(size_t)k * n + p] = c * vkp - s * vkq; V[(size_t)k * n + q] = s * vkp + c * vkq; }
        }
    }
    int *ord = nn_malloc(sizeof(int) * (size_t)(n ? n : 1)); for (int i = 0; i < n; i++) ord[i] = i;
    for (int i = 1; i < n; i++) { int o = ord[i], j = i - 1; while (j >= 0 && A_(ord[j], ord[j]) > A_(o, o)) { ord[j + 1] = ord[j]; j--; } ord[j + 1] = o; }
    double *Vs = nn_malloc(sizeof(double) * (size_t)(n ? n : 1) * (size_t)(n ? n : 1));
    for (int c = 0; c < n; c++) { w[c] = A_(ord[c], ord[c]); for (int r = 0; r < n; r++) Vs[(size_t)r * n + c] = V[(size_t)r * n + ord[c]]; }
    memcpy(V, Vs, sizeof(double) * (size_t)n * (size_t)n);
    free(A); free(ord); free(Vs);
}

/* ================================================================== SVD: one-sided Jacobi (Hestenes) */
static void svd_tall(const double *A0, int m, int n, double *U, double *S, double *V) {      /* requires m >= n; U[m,n] S[n] V[n,n] */
    double *W = nn_malloc(sizeof(double) * (size_t)m * (size_t)(n ? n : 1)); memcpy(W, A0, sizeof(double) * (size_t)m * (size_t)n);
    memset(V, 0, sizeof(double) * (size_t)n * (size_t)n); for (int i = 0; i < n; i++) V[(size_t)i * n + i] = 1.0;
    for (int sweep = 0; sweep < 80; sweep++) {
        bool rotated = false;
        for (int p = 0; p < n - 1; p++) for (int q = p + 1; q < n; q++) {
            double alpha = 0, beta = 0, gamma = 0;
            for (int i = 0; i < m; i++) { double wp = W[(size_t)i * n + p], wq = W[(size_t)i * n + q]; alpha += wp * wp; beta += wq * wq; gamma += wp * wq; }
            if (gamma == 0 || fabs(gamma) <= 1e-15 * sqrt(alpha * beta)) continue;
            rotated = true;
            double zeta = (beta - alpha) / (2.0 * gamma), t = (zeta >= 0 ? 1.0 : -1.0) / (fabs(zeta) + sqrt(1.0 + zeta * zeta)), c = 1.0 / sqrt(1.0 + t * t), s = c * t;
            for (int i = 0; i < m; i++) { double wp = W[(size_t)i * n + p], wq = W[(size_t)i * n + q]; W[(size_t)i * n + p] = c * wp - s * wq; W[(size_t)i * n + q] = s * wp + c * wq; }
            for (int i = 0; i < n; i++) { double vp = V[(size_t)i * n + p], vq = V[(size_t)i * n + q]; V[(size_t)i * n + p] = c * vp - s * vq; V[(size_t)i * n + q] = s * vp + c * vq; }
        }
        if (!rotated) break;
    }
    double *sv = nn_malloc(sizeof(double) * (size_t)(n ? n : 1)); int *ord = nn_malloc(sizeof(int) * (size_t)(n ? n : 1));
    for (int j = 0; j < n; j++) { double s = 0; for (int i = 0; i < m; i++) s += W[(size_t)i * n + j] * W[(size_t)i * n + j]; sv[j] = sqrt(s); ord[j] = j; }
    for (int i = 1; i < n; i++) { int o = ord[i], j = i - 1; while (j >= 0 && sv[ord[j]] < sv[o]) { ord[j + 1] = ord[j]; j--; } ord[j + 1] = o; }
    double *Vs = nn_malloc(sizeof(double) * (size_t)(n ? n : 1) * (size_t)(n ? n : 1)); bool *zero = nn_calloc((size_t)(n ? n : 1), sizeof(bool));
    for (int c = 0; c < n; c++) {
        int o = ord[c]; S[c] = sv[o]; zero[c] = !(sv[o] > 0);
        for (int i = 0; i < m; i++) U[(size_t)i * n + c] = zero[c] ? 0.0 : W[(size_t)i * n + o] / sv[o];
        for (int r = 0; r < n; r++) Vs[(size_t)r * n + c] = V[(size_t)r * n + o];
    }
    memcpy(V, Vs, sizeof(double) * (size_t)n * (size_t)n);
    for (int c = 0; c < n; c++) if (zero[c]) {                              /* exactly-zero singular value: complete U with an orthonormal vector */
        for (int e = 0; e < m; e++) {
            double *col = nn_calloc((size_t)m, sizeof(double)); col[e] = 1.0;
            for (int pass = 0; pass < 2; pass++) for (int o = 0; o < n; o++) if (o != c && !(zero[o] && o > c)) {
                double dot = 0; for (int i = 0; i < m; i++) dot += col[i] * U[(size_t)i * n + o]; for (int i = 0; i < m; i++) col[i] -= dot * U[(size_t)i * n + o];
            }
            double nr = 0; for (int i = 0; i < m; i++) nr += col[i] * col[i]; nr = sqrt(nr);
            if (nr > 1e-8) { for (int i = 0; i < m; i++) U[(size_t)i * n + c] = col[i] / nr; free(col); break; }
            free(col);
        }
    }
    free(W); free(sv); free(ord); free(Vs); free(zero);
}
void nl_svd(const double *A, int m, int n, double *U, double *S, double *Vt) {
    int k = m < n ? m : n; if (k == 0) return;
    if (m >= n) {
        double *V = nn_malloc(sizeof(double) * (size_t)n * (size_t)n); svd_tall(A, m, n, U, S, V);
        for (int i = 0; i < n; i++) for (int j = 0; j < n; j++) Vt[(size_t)i * n + j] = V[(size_t)j * n + i];
        free(V);
    } else {
        double *At = nn_malloc(sizeof(double) * (size_t)m * (size_t)n), *U2 = nn_malloc(sizeof(double) * (size_t)n * (size_t)m), *V2 = nn_malloc(sizeof(double) * (size_t)m * (size_t)m);
        for (int i = 0; i < m; i++) for (int j = 0; j < n; j++) At[(size_t)j * m + i] = A[(size_t)i * n + j];
        svd_tall(At, n, m, U2, S, V2);                                     /* A^T = U2 S V2^T  =>  A = V2 S U2^T */
        memcpy(U, V2, sizeof(double) * (size_t)m * (size_t)m);
        for (int i = 0; i < m; i++) for (int j = 0; j < n; j++) Vt[(size_t)i * n + j] = U2[(size_t)j * m + i];
        free(At); free(U2); free(V2);
    }
}

/* ================================================================== matrix exponential: scaling & squaring with a Taylor core */
void nl_expm(const double *A, int n, double *out) {
    if (n == 0) return;
    size_t nn = (size_t)n * (size_t)n; double norm1 = 0;
    for (int j = 0; j < n; j++) { double s = 0; for (int i = 0; i < n; i++) s += fabs(A_(i, j)); if (s > norm1) norm1 = s; }
    if (!isfinite(norm1)) { for (size_t i = 0; i < nn; i++) out[i] = NAN; return; }
    int sq = norm1 > 0.5 ? (int)ceil(log2(norm1 / 0.5)) : 0; if (sq > 1000) sq = 1000;
    double sc = ldexp(1.0, -sq), *As = nn_malloc(sizeof(double) * nn), *term = nn_malloc(sizeof(double) * nn), *tmp = nn_malloc(sizeof(double) * nn);
    for (size_t i = 0; i < nn; i++) As[i] = A[i] * sc;
    memset(out, 0, sizeof(double) * nn); memset(term, 0, sizeof(double) * nn); for (int i = 0; i < n; i++) { out[(size_t)i * n + i] = 1.0; term[(size_t)i * n + i] = 1.0; }
    for (int k = 1; k <= 20; k++) { nl_matmul(n, n, n, term, As, tmp); for (size_t i = 0; i < nn; i++) { term[i] = tmp[i] / k; out[i] += term[i]; } }
    for (int s = 0; s < sq; s++) { nl_matmul(n, n, n, out, out, tmp); memcpy(out, tmp, sizeof(double) * nn); }
    free(As); free(term); free(tmp);
}
