/* std.nn layers: dense, activations, dropout, flatten, reshape, embedding, batchnorm, layernorm */
#include "nn_internal.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>

#define P(i) (L->params[i])

static void out_like(NnLayer *L, const NnTensor *x) { nt_resize(&L->out, x->nd, x->shape); }
static void dx_like(NnLayer *L, const NnTensor *x) { nt_resize(&L->dx, x->nd, x->shape); }
static inline nn_real sigm(nn_real x) { return x >= 0 ? 1.0 / (1.0 + exp(-x)) : exp(x) / (1.0 + exp(x)); }

/* ================================================================== dense: hp = {in, out, bias} */
void nn_setup_dense(NnLayer *L, int init, double arg) {
    int in = (int)L->hp[0], out = (int)L->hp[1]; int ws[2] = { in, out }, bs[1] = { out };
    NnParam *w = nn_param_add(L, "weight", 2, ws, true, false);
    nn_init_tensor(&w->w, in, out, init == NNI_NONE ? NNI_NONE : init, arg);
    if (L->hp[2] != 0) nn_param_add(L, "bias", 1, bs, false, false);
}
static void dense_fwd(NnLayer *L, const NnTensor *x, int training) {
    (void)training; int in = (int)L->hp[0], out = (int)L->hp[1];
    if (x->nd < 1 || x->shape[x->nd - 1] != in) nn_shape_fail(L, "last dimension equal to the layer's input size", x);
    int sh[NN_MAXDIM]; memcpy(sh, x->shape, sizeof(int) * (size_t)x->nd); sh[x->nd - 1] = out;
    nt_resize(&L->out, x->nd, sh); size_t rows = in ? x->n / (size_t)in : nt_numel(x->nd - 1, x->shape);
    if (L->nparams > 1) for (size_t r = 0; r < rows; r++) memcpy(L->out.d + r * (size_t)out, P(1).w.d, sizeof(nn_real) * (size_t)out);
    else nt_zero(&L->out);
    nn_gemm_nn((int)rows, out, in, x->d, P(0).w.d, L->out.d); L->in = x;
}
static void dense_bwd(NnLayer *L, const NnTensor *dy) {
    int in = (int)L->hp[0], out = (int)L->hp[1]; const NnTensor *x = L->in; size_t rows = in ? x->n / (size_t)in : 0;
    nn_gemm_tn(in, out, (int)rows, x->d, dy->d, P(0).g.d);
    if (L->nparams > 1) for (size_t r = 0; r < rows; r++) for (int j = 0; j < out; j++) P(1).g.d[j] += dy->d[r * (size_t)out + (size_t)j];
    if (L->need_dx) { dx_like(L, x); nt_zero(&L->dx); nn_gemm_nt((int)rows, in, out, dy->d, P(0).w.d, L->dx.d); }
}
const NnOps nn_ops_dense = { dense_fwd, dense_bwd, NULL };

/* ================================================================== activations: hp = {id, param} */
static const char *ACT_NAMES[] = { "relu", "relu6", "leaky_relu", "elu", "selu", "gelu", "sigmoid", "tanh", "softplus", "silu", "mish",
                                   "softsign", "hard_sigmoid", "softmax", "log_softmax", "identity" };
int nn_act_from_name(const char *n) {
    if (!strcmp(n, "swish")) return NNA_SILU; if (!strcmp(n, "leakyrelu")) return NNA_LEAKY;
    if (!strcmp(n, "linear") || !strcmp(n, "none")) return NNA_IDENTITY; if (!strcmp(n, "hardsigmoid")) return NNA_HARDSIG;
    for (int i = 0; i < NNA_COUNT; i++) if (!strcmp(n, ACT_NAMES[i])) return i;
    return -1;
}
const char *nn_act_name(int id) { return (id >= 0 && id < NNA_COUNT) ? ACT_NAMES[id] : "?"; }
void nn_setup_act(NnLayer *L, int init, double arg) { (void)L; (void)init; (void)arg; }

#define SELU_S 1.0507009873554805
#define SELU_A 1.6732632423543772
void nn_act_forward_elems(int id, double a, size_t n, const nn_real *x, nn_real *y) {
    switch (id) {
    case NNA_RELU: for (size_t i = 0; i < n; i++) y[i] = x[i] > 0 ? x[i] : 0; break;
    case NNA_RELU6: for (size_t i = 0; i < n; i++) y[i] = x[i] < 0 ? 0 : (x[i] > 6 ? 6 : x[i]); break;
    case NNA_LEAKY: for (size_t i = 0; i < n; i++) y[i] = x[i] > 0 ? x[i] : a * x[i]; break;
    case NNA_ELU: for (size_t i = 0; i < n; i++) y[i] = x[i] > 0 ? x[i] : a * expm1(x[i]); break;
    case NNA_SELU: for (size_t i = 0; i < n; i++) y[i] = x[i] > 0 ? SELU_S * x[i] : SELU_S * SELU_A * expm1(x[i]); break;
    case NNA_GELU: for (size_t i = 0; i < n; i++) { nn_real v = x[i]; y[i] = 0.5 * v * (1.0 + tanh(0.7978845608028654 * (v + 0.044715 * v * v * v))); } break;
    case NNA_SIGMOID: for (size_t i = 0; i < n; i++) y[i] = sigm(x[i]); break;
    case NNA_TANH: for (size_t i = 0; i < n; i++) y[i] = tanh(x[i]); break;
    case NNA_SOFTPLUS: for (size_t i = 0; i < n; i++) y[i] = x[i] > 30 ? x[i] : log1p(exp(x[i])); break;
    case NNA_SILU: for (size_t i = 0; i < n; i++) y[i] = x[i] * sigm(x[i]); break;
    case NNA_MISH: for (size_t i = 0; i < n; i++) { nn_real sp = x[i] > 30 ? x[i] : log1p(exp(x[i])); y[i] = x[i] * tanh(sp); } break;
    case NNA_SOFTSIGN: for (size_t i = 0; i < n; i++) y[i] = x[i] / (1.0 + fabs(x[i])); break;
    case NNA_HARDSIG: for (size_t i = 0; i < n; i++) { nn_real v = 0.2 * x[i] + 0.5; y[i] = v < 0 ? 0 : (v > 1 ? 1 : v); } break;
    default: memcpy(y, x, sizeof(nn_real) * n); break;
    }
}
static void act_fwd(NnLayer *L, const NnTensor *x, int training) {
    (void)training; int id = (int)L->hp[0]; out_like(L, x); L->in = x;
    if (id == NNA_SOFTMAX || id == NNA_LOGSOFTMAX) {
        if (x->nd < 1) nn_shape_fail(L, "at least one dimension", x);
        size_t D = (size_t)x->shape[x->nd - 1], rows = D ? x->n / D : 0;
        for (size_t r = 0; r < rows; r++) {
            const nn_real *p = x->d + r * D; nn_real *q = L->out.d + r * D; nn_real m = -DBL_MAX, s = 0;
            for (size_t k = 0; k < D; k++) if (p[k] > m) m = p[k];
            for (size_t k = 0; k < D; k++) s += exp(p[k] - m);
            if (id == NNA_SOFTMAX) for (size_t k = 0; k < D; k++) q[k] = exp(p[k] - m) / s;
            else { nn_real ls = m + log(s); for (size_t k = 0; k < D; k++) q[k] = p[k] - ls; }
        }
        return;
    }
    nn_act_forward_elems(id, L->hp[1], x->n, x->d, L->out.d);
}
static void act_bwd(NnLayer *L, const NnTensor *dy) {
    if (!L->need_dx) return;
    int id = (int)L->hp[0]; double a = L->hp[1]; const NnTensor *x = L->in; const nn_real *y = L->out.d; size_t n = x->n;
    dx_like(L, x); nn_real *g = L->dx.d; const nn_real *d = dy->d, *xv = x->d;
    switch (id) {
    case NNA_RELU: for (size_t i = 0; i < n; i++) g[i] = xv[i] > 0 ? d[i] : 0; break;
    case NNA_RELU6: for (size_t i = 0; i < n; i++) g[i] = (xv[i] > 0 && xv[i] < 6) ? d[i] : 0; break;
    case NNA_LEAKY: for (size_t i = 0; i < n; i++) g[i] = xv[i] > 0 ? d[i] : a * d[i]; break;
    case NNA_ELU: for (size_t i = 0; i < n; i++) g[i] = xv[i] > 0 ? d[i] : d[i] * (y[i] + a); break;
    case NNA_SELU: for (size_t i = 0; i < n; i++) g[i] = xv[i] > 0 ? d[i] * SELU_S : d[i] * (y[i] + SELU_S * SELU_A); break;
    case NNA_GELU: for (size_t i = 0; i < n; i++) {
        nn_real v = xv[i], u = 0.7978845608028654 * (v + 0.044715 * v * v * v), t = tanh(u);
        g[i] = d[i] * (0.5 * (1.0 + t) + 0.5 * v * (1.0 - t * t) * 0.7978845608028654 * (1.0 + 3.0 * 0.044715 * v * v)); } break;
    case NNA_SIGMOID: for (size_t i = 0; i < n; i++) g[i] = d[i] * y[i] * (1.0 - y[i]); break;
    case NNA_TANH: for (size_t i = 0; i < n; i++) g[i] = d[i] * (1.0 - y[i] * y[i]); break;
    case NNA_SOFTPLUS: for (size_t i = 0; i < n; i++) g[i] = d[i] * sigm(xv[i]); break;
    case NNA_SILU: for (size_t i = 0; i < n; i++) { nn_real s = sigm(xv[i]); g[i] = d[i] * (s + xv[i] * s * (1.0 - s)); } break;
    case NNA_MISH: for (size_t i = 0; i < n; i++) {
        nn_real sp = xv[i] > 30 ? xv[i] : log1p(exp(xv[i])), t = tanh(sp), s = sigm(xv[i]);
        g[i] = d[i] * (t + xv[i] * (1.0 - t * t) * s); } break;
    case NNA_SOFTSIGN: for (size_t i = 0; i < n; i++) { nn_real q = 1.0 + fabs(xv[i]); g[i] = d[i] / (q * q); } break;
    case NNA_HARDSIG: for (size_t i = 0; i < n; i++) { nn_real v = 0.2 * xv[i] + 0.5; g[i] = (v > 0 && v < 1) ? 0.2 * d[i] : 0; } break;
    case NNA_SOFTMAX: case NNA_LOGSOFTMAX: {
        size_t D = (size_t)x->shape[x->nd - 1], rows = D ? n / D : 0;
        for (size_t r = 0; r < rows; r++) {
            const nn_real *yy = y + r * D, *dd = d + r * D; nn_real *gg = g + r * D; nn_real s = 0;
            if (id == NNA_SOFTMAX) { for (size_t k = 0; k < D; k++) s += dd[k] * yy[k]; for (size_t k = 0; k < D; k++) gg[k] = yy[k] * (dd[k] - s); }
            else { for (size_t k = 0; k < D; k++) s += dd[k]; for (size_t k = 0; k < D; k++) gg[k] = dd[k] - exp(yy[k]) * s; }
        } break; }
    default: memcpy(g, d, sizeof(nn_real) * n); break;
    }
}
const NnOps nn_ops_act = { act_fwd, act_bwd, NULL };

/* ================================================================== dropout: hp = {p}; priv = mask tensor */
void nn_setup_dropout(NnLayer *L, int init, double arg) {
    (void)init; (void)arg; if (!(L->hp[0] >= 0 && L->hp[0] < 1)) nn_fail("dropout rate must be in [0, 1), got %g", L->hp[0]);
    NnTensor *m = nn_malloc(sizeof *m); nt_init(m); L->priv = m;
}
static void drop_fwd(NnLayer *L, const NnTensor *x, int training) {
    double p = L->hp[0]; NnTensor *m = L->priv; out_like(L, x); L->in = x; L->training = training != 0;
    if (!training || p <= 0) { memcpy(L->out.d, x->d, sizeof(nn_real) * x->n); return; }
    nt_resize(m, x->nd, x->shape); nn_real keep = 1.0 / (1.0 - p);
    for (size_t i = 0; i < x->n; i++) { m->d[i] = nn_rand() < p ? 0.0 : keep; L->out.d[i] = x->d[i] * m->d[i]; }
}
static void drop_bwd(NnLayer *L, const NnTensor *dy) {
    if (!L->need_dx) return; NnTensor *m = L->priv; dx_like(L, L->in);
    if (!L->training || L->hp[0] <= 0) memcpy(L->dx.d, dy->d, sizeof(nn_real) * dy->n);
    else for (size_t i = 0; i < dy->n; i++) L->dx.d[i] = dy->d[i] * m->d[i];
}
static void drop_destroy(NnLayer *L) { nt_delete(L->priv); }
const NnOps nn_ops_dropout = { drop_fwd, drop_bwd, drop_destroy };

/* ================================================================== flatten / reshape */
void nn_setup_flatten(NnLayer *L, int init, double arg) { (void)L; (void)init; (void)arg; }
static void flat_fwd(NnLayer *L, const NnTensor *x, int training) {
    (void)training; if (x->nd < 1) nn_shape_fail(L, "at least 1 dimension", x);
    int sh[2] = { x->shape[0], x->shape[0] ? (int)(x->n / (size_t)x->shape[0]) : 0 }; nt_resize(&L->out, 2, sh);
    memcpy(L->out.d, x->d, sizeof(nn_real) * x->n); L->in = x;
}
static void shape_bwd(NnLayer *L, const NnTensor *dy) {
    if (!L->need_dx) return; dx_like(L, L->in); memcpy(L->dx.d, dy->d, sizeof(nn_real) * dy->n);
}
const NnOps nn_ops_flatten = { flat_fwd, shape_bwd, NULL };

/* reshape: hp = {nd, d1..d5}; per-sample target shape, one dimension may be -1 */
void nn_setup_reshape(NnLayer *L, int init, double arg) {
    (void)init; (void)arg; int nd = (int)L->hp[0]; if (nd < 1 || nd > NN_MAXDIM - 1) nn_fail("reshape: need 1..%d target dimensions", NN_MAXDIM - 1);
    int unk = 0; for (int i = 0; i < nd; i++) { if (L->hp[1 + i] == -1) unk++; else if (L->hp[1 + i] < 1) nn_fail("reshape: dimensions must be positive or -1"); }
    if (unk > 1) nn_fail("reshape: only one dimension can be -1");
}
static void reshape_fwd(NnLayer *L, const NnTensor *x, int training) {
    (void)training; int nd = (int)L->hp[0]; if (x->nd < 1) nn_shape_fail(L, "a batch dimension", x);
    int N = x->shape[0]; size_t per = N ? x->n / (size_t)N : 0, known = 1; int unk = -1; int sh[NN_MAXDIM]; sh[0] = N;
    for (int i = 0; i < nd; i++) { if (L->hp[1 + i] == -1) unk = i; else known *= (size_t)L->hp[1 + i]; sh[1 + i] = (int)L->hp[1 + i]; }
    if (unk >= 0) { if (known == 0 || per % known) nn_shape_fail(L, "a size divisible by the fixed target dimensions", x); sh[1 + unk] = (int)(per / known); }
    else if (known != per) nn_shape_fail(L, "the same number of elements per sample as the target shape", x);
    nt_resize(&L->out, nd + 1, sh); memcpy(L->out.d, x->d, sizeof(nn_real) * x->n); L->in = x;
}
const NnOps nn_ops_reshape = { reshape_fwd, shape_bwd, NULL };

/* ================================================================== embedding: hp = {vocab, dim, padding_idx} */
void nn_setup_embed(NnLayer *L, int init, double arg) {
    int V = (int)L->hp[0], D = (int)L->hp[1]; int ws[2] = { V, D };
    NnParam *w = nn_param_add(L, "weight", 2, ws, true, false);
    if (init != NNI_NONE) { nn_init_tensor(&w->w, V, D, NNI_NORMAL, arg > 0 ? arg : 1.0 / sqrt((double)(D > 0 ? D : 1))); int pad = (int)L->hp[2]; if (pad >= 0 && pad < V) memset(w->w.d + (size_t)pad * (size_t)D, 0, sizeof(nn_real) * (size_t)D); }
    { int pad = (int)L->hp[2]; if (pad >= 0 && pad < V) { w->skip_lo = (long)pad * D; w->skip_hi = (long)(pad + 1) * D; } }
}
static void emb_fwd(NnLayer *L, const NnTensor *x, int training) {
    (void)training; int V = (int)L->hp[0], D = (int)L->hp[1]; int sh[NN_MAXDIM];
    if (x->nd < 1 || x->nd >= NN_MAXDIM) nn_shape_fail(L, "integer token ids with 1 to 5 dimensions", x);
    memcpy(sh, x->shape, sizeof(int) * (size_t)x->nd); sh[x->nd] = D; nt_resize(&L->out, x->nd + 1, sh);
    for (size_t i = 0; i < x->n; i++) {
        double v = x->d[i]; long id = (long)v;
        if (v != (double)id || id < 0 || id >= V) nn_fail("%s: token id %g at position %zu is outside the vocabulary (0..%d)", L->name, v, i, V - 1);
        memcpy(L->out.d + i * (size_t)D, P(0).w.d + (size_t)id * (size_t)D, sizeof(nn_real) * (size_t)D);
    }
    L->in = x;
}
static void emb_bwd(NnLayer *L, const NnTensor *dy) {
    int D = (int)L->hp[1], pad = (int)L->hp[2]; const NnTensor *x = L->in;
    for (size_t i = 0; i < x->n; i++) { long id = (long)x->d[i]; if (id == pad) continue; nn_real *g = P(0).g.d + (size_t)id * (size_t)D; const nn_real *d = dy->d + i * (size_t)D; for (int k = 0; k < D; k++) g[k] += d[k]; }
    if (L->need_dx) { dx_like(L, x); nt_zero(&L->dx); }
}
const NnOps nn_ops_embed = { emb_fwd, emb_bwd, NULL };

/* ================================================================== batchnorm: hp = {features, momentum, eps, affine}
 * params: [gamma, beta,] running_mean, running_var ; priv: xhat + invstd */
typedef struct { NnTensor xhat, invstd; } BnPriv;
void nn_setup_batchnorm(NnLayer *L, int init, double arg) {
    (void)arg; int C = (int)L->hp[0]; int s[1] = { C }; BnPriv *pv = nn_calloc(1, sizeof *pv); nt_init(&pv->xhat); nt_init(&pv->invstd); L->priv = pv;
    if (L->hp[3] != 0) { NnParam *g = nn_param_add(L, "gamma", 1, s, false, false); NnParam *b = nn_param_add(L, "beta", 1, s, false, false); (void)b; (void)init; for (int i = 0; i < C; i++) g->w.d[i] = 1.0; }
    NnParam *rv; nn_param_add(L, "running_mean", 1, s, false, true); rv = nn_param_add(L, "running_var", 1, s, false, true); for (int i = 0; i < C; i++) rv->w.d[i] = 1.0;
}
static void bn_destroy(NnLayer *L) { BnPriv *pv = L->priv; nt_free(&pv->xhat); nt_free(&pv->invstd); free(pv); }
static void bn_fwd(NnLayer *L, const NnTensor *x, int training) {
    int C = (int)L->hp[0]; double mom = L->hp[1], eps = L->hp[2]; bool affine = L->hp[3] != 0; BnPriv *pv = L->priv;
    if (x->nd < 2 || x->shape[1] != C) nn_shape_fail(L, "shape [batch, features, ...] with the layer's feature count at dimension 1", x);
    size_t N = (size_t)x->shape[0], S = (N != 0 && C != 0) ? x->n / (N * (size_t)C) : 0, m = N * S; int ri = affine ? 2 : 0;
    nn_real *rmean = P(ri).w.d, *rvar = P(ri + 1).w.d; out_like(L, x); nt_resize(&pv->xhat, x->nd, x->shape); int cs[1] = { C }; nt_resize(&pv->invstd, 1, cs);
    L->training = training != 0; L->in = x;
    for (int c = 0; c < C; c++) {
        nn_real mean, var;
        if (training) {
            double sum = 0; for (size_t n = 0; n < N; n++) { const nn_real *p = x->d + (n * (size_t)C + (size_t)c) * S; for (size_t s = 0; s < S; s++) sum += p[s]; }
            mean = sum / (double)m; double vs = 0;
            for (size_t n = 0; n < N; n++) { const nn_real *p = x->d + (n * (size_t)C + (size_t)c) * S; for (size_t s = 0; s < S; s++) { double d = p[s] - mean; vs += d * d; } }
            var = vs / (double)m; rmean[c] = (1.0 - mom) * rmean[c] + mom * mean; rvar[c] = (1.0 - mom) * rvar[c] + mom * (m > 1 ? var * (double)m / (double)(m - 1) : var);
        } else { mean = rmean[c]; var = rvar[c]; }
        nn_real inv = 1.0 / sqrt(var + eps); pv->invstd.d[c] = inv; nn_real g = affine ? P(0).w.d[c] : 1.0, b = affine ? P(1).w.d[c] : 0.0;
        for (size_t n = 0; n < N; n++) { size_t o = (n * (size_t)C + (size_t)c) * S; for (size_t s = 0; s < S; s++) { nn_real xh = (x->d[o + s] - mean) * inv; pv->xhat.d[o + s] = xh; L->out.d[o + s] = g * xh + b; } }
    }
}
static void bn_bwd(NnLayer *L, const NnTensor *dy) {
    int C = (int)L->hp[0]; bool affine = L->hp[3] != 0; BnPriv *pv = L->priv; const NnTensor *x = L->in;
    size_t N = (size_t)x->shape[0], S = (N != 0 && C != 0) ? x->n / (N * (size_t)C) : 0, m = N * S; if (L->need_dx) dx_like(L, x);
    for (int c = 0; c < C; c++) {
        double sdy = 0, sdyx = 0;
        for (size_t n = 0; n < N; n++) { size_t o = (n * (size_t)C + (size_t)c) * S; for (size_t s = 0; s < S; s++) { sdy += dy->d[o + s]; sdyx += dy->d[o + s] * pv->xhat.d[o + s]; } }
        if (affine) { P(0).g.d[c] += sdyx; P(1).g.d[c] += sdy; }
        if (!L->need_dx) continue;
        nn_real g = affine ? P(0).w.d[c] : 1.0, inv = pv->invstd.d[c];
        for (size_t n = 0; n < N; n++) { size_t o = (n * (size_t)C + (size_t)c) * S; for (size_t s = 0; s < S; s++) {
            if (L->training) L->dx.d[o + s] = g * inv / (double)m * ((double)m * dy->d[o + s] - sdy - pv->xhat.d[o + s] * sdyx);
            else L->dx.d[o + s] = dy->d[o + s] * g * inv; } }
    }
}
const NnOps nn_ops_batchnorm = { bn_fwd, bn_bwd, bn_destroy };

/* ================================================================== layernorm: hp = {features, eps, affine} */
void nn_setup_layernorm(NnLayer *L, int init, double arg) {
    (void)init; (void)arg; int D = (int)L->hp[0]; int s[1] = { D }; BnPriv *pv = nn_calloc(1, sizeof *pv); nt_init(&pv->xhat); nt_init(&pv->invstd); L->priv = pv;
    if (L->hp[2] != 0) { NnParam *g = nn_param_add(L, "gamma", 1, s, false, false); nn_param_add(L, "beta", 1, s, false, false); for (int i = 0; i < D; i++) g->w.d[i] = 1.0; }
}
static void ln_fwd(NnLayer *L, const NnTensor *x, int training) {
    (void)training; int D = (int)L->hp[0]; double eps = L->hp[1]; bool affine = L->hp[2] != 0; BnPriv *pv = L->priv;
    if (x->nd < 1 || x->shape[x->nd - 1] != D) nn_shape_fail(L, "last dimension equal to the layer's feature count", x);
    size_t rows = D ? x->n / (size_t)D : 0; out_like(L, x); nt_resize(&pv->xhat, x->nd, x->shape); int rs[1] = { (int)rows }; nt_resize(&pv->invstd, 1, rs); L->in = x;
    for (size_t r = 0; r < rows; r++) {
        const nn_real *p = x->d + r * (size_t)D; double mean = 0, var = 0; for (int k = 0; k < D; k++) mean += p[k]; mean /= D;
        for (int k = 0; k < D; k++) { double d = p[k] - mean; var += d * d; } var /= D; nn_real inv = 1.0 / sqrt(var + eps); pv->invstd.d[r] = inv;
        for (int k = 0; k < D; k++) { nn_real xh = (p[k] - mean) * inv; pv->xhat.d[r * (size_t)D + (size_t)k] = xh; L->out.d[r * (size_t)D + (size_t)k] = affine ? P(0).w.d[k] * xh + P(1).w.d[k] : xh; }
    }
}
static void ln_bwd(NnLayer *L, const NnTensor *dy) {
    int D = (int)L->hp[0]; bool affine = L->hp[2] != 0; BnPriv *pv = L->priv; size_t rows = D ? dy->n / (size_t)D : 0; if (L->need_dx) dx_like(L, L->in);
    for (size_t r = 0; r < rows; r++) {
        const nn_real *d = dy->d + r * (size_t)D, *xh = pv->xhat.d + r * (size_t)D; double s1 = 0, s2 = 0;
        for (int k = 0; k < D; k++) { nn_real dh = affine ? d[k] * P(0).w.d[k] : d[k]; s1 += dh; s2 += dh * xh[k]; if (affine) { P(0).g.d[k] += d[k] * xh[k]; P(1).g.d[k] += d[k]; } }
        if (L->need_dx) for (int k = 0; k < D; k++) { nn_real dh = affine ? d[k] * P(0).w.d[k] : d[k]; L->dx.d[r * (size_t)D + (size_t)k] = pv->invstd.d[r] * (dh - s1 / D - xh[k] * s2 / D); }
    }
}
const NnOps nn_ops_layernorm = { ln_fwd, ln_bwd, bn_destroy };
