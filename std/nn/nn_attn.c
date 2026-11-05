/* std.nn attention.
 * attn   hp = {d_model, heads, causal}; params Wq Wk Wv Wo [D,D], bq bk bv bo [D]; input [N,T,D] -> [N,T,D]
 * posenc hp = {} ; adds fixed sinusoidal position codes to [N,T,D]
 */
#include "nn_internal.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>

#define P(i) (L->params[i])
typedef struct { NnTensor Q, K, V, Pr, O, dO, dQ, dK, dV, dP; } AttnPriv;

void nn_setup_attn(NnLayer *L, int init, double arg) {
    int D = (int)L->hp[0], Hh = (int)L->hp[1]; if (D < 1 || Hh < 1 || D % Hh) nn_fail("attention: d_model (%d) must be divisible by heads (%d)", D, Hh);
    int ws[2] = { D, D }, bs[1] = { D }; const char *wn[] = { "Wq", "Wk", "Wv", "Wo" }, *bn[] = { "bq", "bk", "bv", "bo" };
    for (int i = 0; i < 4; i++) { NnParam *w = nn_param_add(L, wn[i], 2, ws, true, false); nn_init_tensor(&w->w, D, D, init, arg); }
    for (int i = 0; i < 4; i++) nn_param_add(L, bn[i], 1, bs, false, false);
    L->priv = nn_calloc(1, sizeof(AttnPriv));
}
static void attn_destroy(NnLayer *L) {
    AttnPriv *pv = L->priv; NnTensor *ts[] = { &pv->Q, &pv->K, &pv->V, &pv->Pr, &pv->O, &pv->dO, &pv->dQ, &pv->dK, &pv->dV, &pv->dP };
    for (size_t i = 0; i < sizeof ts / sizeof ts[0]; i++) nt_free(ts[i]); free(pv);
}
static void proj(const NnTensor *x, const NnParam *w, const NnParam *b, NnTensor *out, int D) {
    size_t rows = x->n / (size_t)D; int sh[2] = { (int)rows, D }; nt_resize(out, 2, sh);
    for (size_t r = 0; r < rows; r++) memcpy(out->d + r * (size_t)D, b->w.d, sizeof(nn_real) * (size_t)D);
    nn_gemm_nn((int)rows, D, D, x->d, w->w.d, out->d);
}
static void attn_fwd(NnLayer *L, const NnTensor *x, int training) {
    (void)training; AttnPriv *pv = L->priv; int D = (int)L->hp[0], Hh = (int)L->hp[1]; bool causal = L->hp[2] != 0;
    if (x->nd != 3 || x->shape[2] != D) nn_shape_fail(L, "[batch, time, d_model]", x);
    int N = x->shape[0], T = x->shape[1], dk = D / Hh; double scale = 1.0 / sqrt((double)dk);
    proj(x, &P(0), &P(4), &pv->Q, D); proj(x, &P(1), &P(5), &pv->K, D); proj(x, &P(2), &P(6), &pv->V, D);
    int ps[4] = { N, Hh, T, T }, os[2] = { N * T, D }; nt_resize(&pv->Pr, 4, ps); nt_resize(&pv->O, 2, os); nt_zero(&pv->O);
    for (int n = 0; n < N; n++) for (int h = 0; h < Hh; h++) for (int i = 0; i < T; i++) {
        nn_real *p = pv->Pr.d + (((size_t)n * (size_t)Hh + (size_t)h) * (size_t)T + (size_t)i) * (size_t)T; const nn_real *q = pv->Q.d + ((size_t)n * (size_t)T + (size_t)i) * (size_t)D + (size_t)h * (size_t)dk;
        int lim = causal ? i + 1 : T; nn_real m = -DBL_MAX;
        for (int j = 0; j < lim; j++) { const nn_real *k = pv->K.d + ((size_t)n * (size_t)T + (size_t)j) * (size_t)D + (size_t)h * (size_t)dk; nn_real s = 0; for (int e = 0; e < dk; e++) s += q[e] * k[e]; p[j] = s * scale; if (p[j] > m) m = p[j]; }
        nn_real sum = 0; for (int j = 0; j < lim; j++) { p[j] = exp(p[j] - m); sum += p[j]; } for (int j = 0; j < lim; j++) p[j] /= sum; for (int j = lim; j < T; j++) p[j] = 0;
        nn_real *o = pv->O.d + ((size_t)n * (size_t)T + (size_t)i) * (size_t)D + (size_t)h * (size_t)dk;
        for (int j = 0; j < lim; j++) { const nn_real *v = pv->V.d + ((size_t)n * (size_t)T + (size_t)j) * (size_t)D + (size_t)h * (size_t)dk; nn_real pj = p[j]; for (int e = 0; e < dk; e++) o[e] += pj * v[e]; }
    }
    nt_resize(&L->out, 3, x->shape); for (int r = 0; r < N * T; r++) memcpy(L->out.d + (size_t)r * (size_t)D, P(7).w.d, sizeof(nn_real) * (size_t)D);
    nn_gemm_nn(N * T, D, D, pv->O.d, P(3).w.d, L->out.d); L->in = x;
}
static void bias_grad(NnParam *b, const nn_real *g, size_t rows, int D) { for (size_t r = 0; r < rows; r++) for (int k = 0; k < D; k++) b->g.d[k] += g[r * (size_t)D + (size_t)k]; }
static void attn_bwd(NnLayer *L, const NnTensor *dy) {
    AttnPriv *pv = L->priv; int D = (int)L->hp[0], Hh = (int)L->hp[1]; bool causal = L->hp[2] != 0; const NnTensor *x = L->in;
    int N = x->shape[0], T = x->shape[1], dk = D / Hh; double scale = 1.0 / sqrt((double)dk); size_t rows = (size_t)N * (size_t)T; int rs[2] = { N * T, D };
    nt_resize(&pv->dO, 2, rs); nt_zero(&pv->dO); nt_resize(&pv->dQ, 2, rs); nt_zero(&pv->dQ); nt_resize(&pv->dK, 2, rs); nt_zero(&pv->dK); nt_resize(&pv->dV, 2, rs); nt_zero(&pv->dV);
    nn_gemm_tn(D, D, N * T, pv->O.d, dy->d, P(3).g.d); bias_grad(&P(7), dy->d, rows, D); nn_gemm_nt(N * T, D, D, dy->d, P(3).w.d, pv->dO.d);
    nn_real *dp = nn_malloc(sizeof(nn_real) * (size_t)T);
    for (int n = 0; n < N; n++) for (int h = 0; h < Hh; h++) for (int i = 0; i < T; i++) {
        const nn_real *p = pv->Pr.d + (((size_t)n * (size_t)Hh + (size_t)h) * (size_t)T + (size_t)i) * (size_t)T; const nn_real *dO = pv->dO.d + ((size_t)n * (size_t)T + (size_t)i) * (size_t)D + (size_t)h * (size_t)dk;
        const nn_real *q = pv->Q.d + ((size_t)n * (size_t)T + (size_t)i) * (size_t)D + (size_t)h * (size_t)dk; nn_real *dq = pv->dQ.d + ((size_t)n * (size_t)T + (size_t)i) * (size_t)D + (size_t)h * (size_t)dk;
        int lim = causal ? i + 1 : T; nn_real dot = 0;
        for (int j = 0; j < lim; j++) { const nn_real *v = pv->V.d + ((size_t)n * (size_t)T + (size_t)j) * (size_t)D + (size_t)h * (size_t)dk; nn_real s = 0; for (int e = 0; e < dk; e++) s += dO[e] * v[e]; dp[j] = s; dot += s * p[j]; }
        for (int j = 0; j < lim; j++) {
            nn_real ds = p[j] * (dp[j] - dot) * scale; size_t jo = ((size_t)n * (size_t)T + (size_t)j) * (size_t)D + (size_t)h * (size_t)dk;
            const nn_real *k = pv->K.d + jo; nn_real *dk_ = pv->dK.d + jo, *dv = pv->dV.d + jo;
            for (int e = 0; e < dk; e++) { dq[e] += ds * k[e]; dk_[e] += ds * q[e]; dv[e] += p[j] * dO[e]; }
        }
    }
    free(dp);
    const NnTensor *gs[3] = { &pv->dQ, &pv->dK, &pv->dV };
    if (L->need_dx) { nt_resize(&L->dx, x->nd, x->shape); nt_zero(&L->dx); }
    for (int w = 0; w < 3; w++) {
        nn_gemm_tn(D, D, N * T, x->d, gs[w]->d, P(w).g.d); bias_grad(&P(4 + w), gs[w]->d, rows, D);
        if (L->need_dx) nn_gemm_nt(N * T, D, D, gs[w]->d, P(w).w.d, L->dx.d);
    }
}
const NnOps nn_ops_attn = { attn_fwd, attn_bwd, attn_destroy };

/* ================================================================== sinusoidal positional encoding */
void nn_setup_posenc(NnLayer *L, int init, double arg) { (void)L; (void)init; (void)arg; }
static void pe_fwd(NnLayer *L, const NnTensor *x, int training) {
    (void)training; if (x->nd != 3) nn_shape_fail(L, "[batch, time, d_model]", x);
    int N = x->shape[0], T = x->shape[1], D = x->shape[2]; nt_resize(&L->out, 3, x->shape);
    for (int n = 0; n < N; n++) for (int t = 0; t < T; t++) for (int d = 0; d < D; d++) {
        double ang = (double)t / pow(10000.0, (double)(2 * (d / 2)) / (double)D); double pe = (d % 2 == 0) ? sin(ang) : cos(ang);
        size_t o = ((size_t)n * (size_t)T + (size_t)t) * (size_t)D + (size_t)d; L->out.d[o] = x->d[o] + pe;
    }
    L->in = x;
}
static void pe_bwd(NnLayer *L, const NnTensor *dy) { if (!L->need_dx) return; nt_resize(&L->dx, dy->nd, dy->shape); memcpy(L->dx.d, dy->d, sizeof(nn_real) * dy->n); }
const NnOps nn_ops_posenc = { pe_fwd, pe_bwd, NULL };
