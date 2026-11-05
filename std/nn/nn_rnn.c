/* std.nn recurrent layers with backprop through time.
 * hp = {cell(0 rnn / 1 lstm / 2 gru), input, hidden, return_sequences, forget_bias}
 * params: Wx[F, G*H], Wh[H, G*H], b[G*H]   (G = 1, 4, 3; LSTM gates i,f,g,o; GRU gates z,r,n; GRU is the reset-before form)
 */
#include "nn_internal.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define P(i) (L->params[i])
typedef struct { NnTensor pre, hs, cs, gates, dpre, hprev, tmp, rh, dh, dc, dhn; } RnnPriv;

static int gates_of(int cell) { return cell == 1 ? 4 : cell == 2 ? 3 : 1; }
static inline nn_real sg(nn_real x) { return x >= 0 ? 1.0 / (1.0 + exp(-x)) : exp(x) / (1.0 + exp(x)); }

/* strided GEMMs (leading dimensions), accumulating */
static void g_nn(int M, int N, int K, const nn_real *A, int lda, const nn_real *B, int ldb, nn_real *C, int ldc) {
    for (int i = 0; i < M; i++) for (int k = 0; k < K; k++) { nn_real a = A[(size_t)i * (size_t)lda + (size_t)k]; if (a == 0) continue; const nn_real *b = B + (size_t)k * (size_t)ldb; nn_real *c = C + (size_t)i * (size_t)ldc; for (int j = 0; j < N; j++) c[j] += a * b[j]; }
}
static void g_nt(int M, int N, int K, const nn_real *A, int lda, const nn_real *B, int ldb, nn_real *C, int ldc) {
    for (int i = 0; i < M; i++) for (int j = 0; j < N; j++) { nn_real s = 0; const nn_real *a = A + (size_t)i * (size_t)lda, *b = B + (size_t)j * (size_t)ldb; for (int k = 0; k < K; k++) s += a[k] * b[k]; C[(size_t)i * (size_t)ldc + (size_t)j] += s; }
}
static void g_tn(int M, int N, int K, const nn_real *A, int lda, const nn_real *B, int ldb, nn_real *C, int ldc) {
    for (int k = 0; k < K; k++) for (int i = 0; i < M; i++) { nn_real a = A[(size_t)k * (size_t)lda + (size_t)i]; if (a == 0) continue; const nn_real *b = B + (size_t)k * (size_t)ldb; nn_real *c = C + (size_t)i * (size_t)ldc; for (int j = 0; j < N; j++) c[j] += a * b[j]; }
}

void nn_setup_rnn(NnLayer *L, int init, double arg) {
    int cell = (int)L->hp[0], F = (int)L->hp[1], H = (int)L->hp[2], G = gates_of(cell);
    if (cell < 0 || cell > 2 || F < 1 || H < 1) nn_fail("recurrent layer needs input and hidden sizes >= 1");
    int xs[2] = { F, G * H }, hs[2] = { H, G * H }, bs[1] = { G * H };
    NnParam *wx = nn_param_add(L, "Wx", 2, xs, true, false); NnParam *wh = nn_param_add(L, "Wh", 2, hs, true, false); NnParam *b = nn_param_add(L, "bias", 1, bs, false, false);
    if (init != NNI_NONE) {
        nn_init_tensor(&wx->w, F, G * H, init, arg);
        for (int g = 0; g < G; g++) {                                       /* orthogonal H x H block per gate */
            NnTensor blk; nt_init(&blk); int bsz[2] = { H, H }; nt_alloc(&blk, 2, bsz); nn_init_tensor(&blk, H, H, NNI_ORTHOGONAL, 1.0);
            for (int r = 0; r < H; r++) memcpy(wh->w.d + (size_t)r * (size_t)(G * H) + (size_t)g * (size_t)H, blk.d + (size_t)r * (size_t)H, sizeof(nn_real) * (size_t)H);
            nt_free(&blk);
        }
        if (cell == 1) for (int k = 0; k < H; k++) b->w.d[H + k] = L->hp[4];
    }
    RnnPriv *pv = nn_calloc(1, sizeof *pv); L->priv = pv;
}
static void rnn_destroy(NnLayer *L) {
    RnnPriv *pv = L->priv; NnTensor *ts[] = { &pv->pre, &pv->hs, &pv->cs, &pv->gates, &pv->dpre, &pv->hprev, &pv->tmp, &pv->rh, &pv->dh, &pv->dc, &pv->dhn };
    for (size_t i = 0; i < sizeof ts / sizeof ts[0]; i++) nt_free(ts[i]); free(pv);
}
static void rnn_fwd(NnLayer *L, const NnTensor *x, int training) {
    (void)training; RnnPriv *pv = L->priv; int cell = (int)L->hp[0], F = (int)L->hp[1], H = (int)L->hp[2], G = gates_of(cell), GH = G * H; bool seq = L->hp[3] != 0;
    if (x->nd != 3 || x->shape[2] != F || x->shape[1] < 1) nn_shape_fail(L, "[batch, time, features] with the layer's input size and time >= 1", x);
    int N = x->shape[0], T = x->shape[1]; int s3[3] = { N, T, GH }, sh[3] = { N, T, H }, sn[2] = { N, H }, sg2[2] = { N, GH };
    nt_resize(&pv->pre, 3, s3); nt_resize(&pv->gates, 3, s3); nt_resize(&pv->hs, 3, sh); nt_resize(&pv->hprev, 2, sn); nt_resize(&pv->tmp, 2, sg2); nt_resize(&pv->rh, 2, sn);
    if (cell == 1) nt_resize(&pv->cs, 3, sh);
    nn_real *Wx = P(0).w.d, *Wh = P(1).w.d, *b = P(2).w.d;
    for (int r = 0; r < N * T; r++) memcpy(pv->pre.d + (size_t)r * (size_t)GH, b, sizeof(nn_real) * (size_t)GH);
    nn_gemm_nn(N * T, GH, F, x->d, Wx, pv->pre.d);
    for (int t = 0; t < T; t++) {
        for (int n = 0; n < N; n++) { if (t == 0) memset(pv->hprev.d + (size_t)n * (size_t)H, 0, sizeof(nn_real) * (size_t)H); else memcpy(pv->hprev.d + (size_t)n * (size_t)H, pv->hs.d + ((size_t)n * (size_t)T + (size_t)t - 1) * (size_t)H, sizeof(nn_real) * (size_t)H); }
        if (cell == 2) {
            memset(pv->tmp.d, 0, sizeof(nn_real) * (size_t)N * (size_t)GH);
            g_nn(N, 2 * H, H, pv->hprev.d, H, Wh, GH, pv->tmp.d, GH);                       /* h_prev x [Uz | Ur] */
            for (int n = 0; n < N; n++) {
                size_t o = ((size_t)n * (size_t)T + (size_t)t) * (size_t)GH; const nn_real *hp = pv->hprev.d + (size_t)n * (size_t)H;
                for (int k = 0; k < H; k++) { nn_real z = sg(pv->pre.d[o + (size_t)k] + pv->tmp.d[(size_t)n * (size_t)GH + (size_t)k]); nn_real r = sg(pv->pre.d[o + (size_t)H + (size_t)k] + pv->tmp.d[(size_t)n * (size_t)GH + (size_t)H + (size_t)k]);
                    pv->gates.d[o + (size_t)k] = z; pv->gates.d[o + (size_t)H + (size_t)k] = r; pv->rh.d[(size_t)n * (size_t)H + (size_t)k] = r * hp[k]; }
            }
            memset(pv->tmp.d, 0, sizeof(nn_real) * (size_t)N * (size_t)GH);
            g_nn(N, H, H, pv->rh.d, H, Wh + 2 * H, GH, pv->tmp.d, GH);                       /* (r*h_prev) x Un */
            for (int n = 0; n < N; n++) {
                size_t o = ((size_t)n * (size_t)T + (size_t)t) * (size_t)GH, ho = ((size_t)n * (size_t)T + (size_t)t) * (size_t)H; const nn_real *hp = pv->hprev.d + (size_t)n * (size_t)H;
                for (int k = 0; k < H; k++) { nn_real nv = tanh(pv->pre.d[o + 2 * (size_t)H + (size_t)k] + pv->tmp.d[(size_t)n * (size_t)GH + (size_t)k]); nn_real z = pv->gates.d[o + (size_t)k];
                    pv->gates.d[o + 2 * (size_t)H + (size_t)k] = nv; pv->hs.d[ho + (size_t)k] = z * hp[k] + (1.0 - z) * nv; }
            }
        } else {
            memset(pv->tmp.d, 0, sizeof(nn_real) * (size_t)N * (size_t)GH);
            g_nn(N, GH, H, pv->hprev.d, H, Wh, GH, pv->tmp.d, GH);
            for (int n = 0; n < N; n++) {
                size_t o = ((size_t)n * (size_t)T + (size_t)t) * (size_t)GH, ho = ((size_t)n * (size_t)T + (size_t)t) * (size_t)H; const nn_real *tp = pv->tmp.d + (size_t)n * (size_t)GH;
                if (cell == 0) for (int k = 0; k < H; k++) { nn_real h = tanh(pv->pre.d[o + (size_t)k] + tp[k]); pv->gates.d[o + (size_t)k] = h; pv->hs.d[ho + (size_t)k] = h; }
                else for (int k = 0; k < H; k++) {
                    nn_real i = sg(pv->pre.d[o + (size_t)k] + tp[k]), f = sg(pv->pre.d[o + (size_t)H + (size_t)k] + tp[H + k]), g = tanh(pv->pre.d[o + 2 * (size_t)H + (size_t)k] + tp[2 * H + k]), oo = sg(pv->pre.d[o + 3 * (size_t)H + (size_t)k] + tp[3 * H + k]);
                    nn_real cp = t == 0 ? 0.0 : pv->cs.d[ho - (size_t)H + (size_t)k]; nn_real c = f * cp + i * g;
                    pv->gates.d[o + (size_t)k] = i; pv->gates.d[o + (size_t)H + (size_t)k] = f; pv->gates.d[o + 2 * (size_t)H + (size_t)k] = g; pv->gates.d[o + 3 * (size_t)H + (size_t)k] = oo;
                    pv->cs.d[ho + (size_t)k] = c; pv->hs.d[ho + (size_t)k] = oo * tanh(c);
                }
            }
        }
    }
    if (seq) { nt_resize(&L->out, 3, sh); memcpy(L->out.d, pv->hs.d, sizeof(nn_real) * pv->hs.n); }
    else { nt_resize(&L->out, 2, sn); for (int n = 0; n < N; n++) memcpy(L->out.d + (size_t)n * (size_t)H, pv->hs.d + ((size_t)n * (size_t)T + (size_t)T - 1) * (size_t)H, sizeof(nn_real) * (size_t)H); }
    L->in = x;
}
static void rnn_bwd(NnLayer *L, const NnTensor *dy) {
    RnnPriv *pv = L->priv; int cell = (int)L->hp[0], F = (int)L->hp[1], H = (int)L->hp[2], G = gates_of(cell), GH = G * H; bool seq = L->hp[3] != 0;
    const NnTensor *x = L->in; int N = x->shape[0], T = x->shape[1]; nn_real *Wh = P(1).w.d, *dWh = P(1).g.d;
    int s3[3] = { N, T, GH }, sn[2] = { N, H }; nt_resize(&pv->dpre, 3, s3); nt_zero(&pv->dpre); nt_resize(&pv->dh, 2, sn); nt_resize(&pv->dhn, 2, sn); nt_zero(&pv->dhn);
    if (cell == 1) { nt_resize(&pv->dc, 2, sn); nt_zero(&pv->dc); }
    nn_real *dh = pv->dh.d, *dhn = pv->dhn.d;
    for (int t = T - 1; t >= 0; t--) {
        for (int n = 0; n < N; n++) for (int k = 0; k < H; k++) {
            nn_real v = dhn[(size_t)n * (size_t)H + (size_t)k];
            if (seq) v += dy->d[((size_t)n * (size_t)T + (size_t)t) * (size_t)H + (size_t)k]; else if (t == T - 1) v += dy->d[(size_t)n * (size_t)H + (size_t)k];
            dh[(size_t)n * (size_t)H + (size_t)k] = v;
        }
        for (int n = 0; n < N; n++) { if (t == 0) memset(pv->hprev.d + (size_t)n * (size_t)H, 0, sizeof(nn_real) * (size_t)H); else memcpy(pv->hprev.d + (size_t)n * (size_t)H, pv->hs.d + ((size_t)n * (size_t)T + (size_t)t - 1) * (size_t)H, sizeof(nn_real) * (size_t)H); }
        memset(dhn, 0, sizeof(nn_real) * (size_t)N * (size_t)H);
        if (cell == 0) {
            for (int n = 0; n < N; n++) { size_t o = ((size_t)n * (size_t)T + (size_t)t) * (size_t)GH; for (int k = 0; k < H; k++) { nn_real h = pv->gates.d[o + (size_t)k]; pv->dpre.d[o + (size_t)k] = dh[(size_t)n * (size_t)H + (size_t)k] * (1.0 - h * h); } }
            g_tn(H, GH, N, pv->hprev.d, H, pv->dpre.d + (size_t)t * (size_t)GH, T * GH, dWh, GH);
            g_nt(N, H, GH, pv->dpre.d + (size_t)t * (size_t)GH, T * GH, Wh, GH, dhn, H);
        } else if (cell == 1) {
            for (int n = 0; n < N; n++) {
                size_t o = ((size_t)n * (size_t)T + (size_t)t) * (size_t)GH, ho = ((size_t)n * (size_t)T + (size_t)t) * (size_t)H;
                for (int k = 0; k < H; k++) {
                    nn_real i = pv->gates.d[o + (size_t)k], f = pv->gates.d[o + (size_t)H + (size_t)k], g = pv->gates.d[o + 2 * (size_t)H + (size_t)k], oo = pv->gates.d[o + 3 * (size_t)H + (size_t)k];
                    nn_real c = pv->cs.d[ho + (size_t)k], tc = tanh(c), cprev = t == 0 ? 0.0 : pv->cs.d[ho - (size_t)H + (size_t)k]; nn_real hh = dh[(size_t)n * (size_t)H + (size_t)k];
                    nn_real dc = pv->dc.d[(size_t)n * (size_t)H + (size_t)k] + hh * oo * (1.0 - tc * tc);
                    pv->dpre.d[o + (size_t)k] = dc * g * i * (1.0 - i); pv->dpre.d[o + (size_t)H + (size_t)k] = dc * cprev * f * (1.0 - f);
                    pv->dpre.d[o + 2 * (size_t)H + (size_t)k] = dc * i * (1.0 - g * g); pv->dpre.d[o + 3 * (size_t)H + (size_t)k] = hh * tc * oo * (1.0 - oo);
                    pv->dc.d[(size_t)n * (size_t)H + (size_t)k] = dc * f;
                }
            }
            g_tn(H, GH, N, pv->hprev.d, H, pv->dpre.d + (size_t)t * (size_t)GH, T * GH, dWh, GH);
            g_nt(N, H, GH, pv->dpre.d + (size_t)t * (size_t)GH, T * GH, Wh, GH, dhn, H);
        } else {                                                                          /* GRU */
            nt_resize(&pv->rh, 2, sn); nt_resize(&pv->tmp, 2, (int[2]){ N, H });
            for (int n = 0; n < N; n++) {
                size_t o = ((size_t)n * (size_t)T + (size_t)t) * (size_t)GH; const nn_real *hp = pv->hprev.d + (size_t)n * (size_t)H;
                for (int k = 0; k < H; k++) {
                    nn_real z = pv->gates.d[o + (size_t)k], r = pv->gates.d[o + (size_t)H + (size_t)k], nv = pv->gates.d[o + 2 * (size_t)H + (size_t)k]; nn_real hh = dh[(size_t)n * (size_t)H + (size_t)k];
                    pv->dpre.d[o + (size_t)k] = hh * (hp[k] - nv) * z * (1.0 - z); pv->dpre.d[o + 2 * (size_t)H + (size_t)k] = hh * (1.0 - z) * (1.0 - nv * nv);
                    dhn[(size_t)n * (size_t)H + (size_t)k] += hh * z; pv->rh.d[(size_t)n * (size_t)H + (size_t)k] = r * hp[k];
                }
            }
            nt_zero(&pv->tmp);                                                            /* d(r*h_prev) = dpre_n x Un^T */
            g_nt(N, H, H, pv->dpre.d + (size_t)t * (size_t)GH + 2 * (size_t)H, T * GH, Wh + 2 * H, GH, pv->tmp.d, H);
            for (int n = 0; n < N; n++) {
                size_t o = ((size_t)n * (size_t)T + (size_t)t) * (size_t)GH; const nn_real *hp = pv->hprev.d + (size_t)n * (size_t)H;
                for (int k = 0; k < H; k++) { nn_real r = pv->gates.d[o + (size_t)H + (size_t)k], drh = pv->tmp.d[(size_t)n * (size_t)H + (size_t)k];
                    pv->dpre.d[o + (size_t)H + (size_t)k] = drh * hp[k] * r * (1.0 - r); dhn[(size_t)n * (size_t)H + (size_t)k] += drh * r; }
            }
            g_tn(H, 2 * H, N, pv->hprev.d, H, pv->dpre.d + (size_t)t * (size_t)GH, T * GH, dWh, GH);                 /* h_prev^T x [dz | dr] */
            g_tn(H, H, N, pv->rh.d, H, pv->dpre.d + (size_t)t * (size_t)GH + 2 * (size_t)H, T * GH, dWh + 2 * H, GH); /* (r*h)^T x dn */
            g_nt(N, H, 2 * H, pv->dpre.d + (size_t)t * (size_t)GH, T * GH, Wh, GH, dhn, H);
        }
    }
    nn_gemm_tn(F, GH, N * T, x->d, pv->dpre.d, P(0).g.d);
    for (int r = 0; r < N * T; r++) for (int j = 0; j < GH; j++) P(2).g.d[j] += pv->dpre.d[(size_t)r * (size_t)GH + (size_t)j];
    if (L->need_dx) { nt_resize(&L->dx, x->nd, x->shape); nt_zero(&L->dx); nn_gemm_nt(N * T, F, GH, pv->dpre.d, P(0).w.d, L->dx.d); }
}
const NnOps nn_ops_rnn = { rnn_fwd, rnn_bwd, rnn_destroy };
