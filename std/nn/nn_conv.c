/* std.nn convolution and pooling.
 * conv  hp = {dims, in, out, kh, kw, sh, sw, ph, pw, dh, dw, bias}   (1-D layers use kh=1, H=1)
 * pool  hp = {mode(0 max/1 avg), dims, kh, kw, sh, sw, ph, pw}
 */
#include "nn_internal.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>

#define P(i) (L->params[i])

typedef struct { int N, C, H, W, Ho, Wo; } Geom;
typedef struct { nn_real *col; size_t colcap; int *arg; size_t argcap; } ConvPriv;

static void geom_from(const NnLayer *L, const NnTensor *x, int dims, int cin, Geom *g, const char *what) {
    if (x->nd != dims + 2) nn_shape_fail(L, dims == 2 ? "[batch, channels, height, width]" : "[batch, channels, length]", x);
    (void)what; g->N = x->shape[0]; g->C = x->shape[1];
    if (cin >= 0 && g->C != cin) nn_shape_fail(L, "the layer's channel count at dimension 1", x);
    if (dims == 2) { g->H = x->shape[2]; g->W = x->shape[3]; } else { g->H = 1; g->W = x->shape[2]; }
}
static int outdim(int in, int pad, int dil, int k, int stride) { int span = dil * (k - 1) + 1; int v = in + 2 * pad - span; return v < 0 ? 0 : v / stride + 1; }
static void set_out(NnLayer *L, int dims, int N, int C, int Ho, int Wo) {
    int sh[4]; sh[0] = N; sh[1] = C; if (dims == 2) { sh[2] = Ho; sh[3] = Wo; nt_resize(&L->out, 4, sh); } else { sh[2] = Wo; nt_resize(&L->out, 3, sh); }
}

/* ================================================================== conv */
void nn_setup_conv(NnLayer *L, int init, double arg) {
    int dims = (int)L->hp[0], ci = (int)L->hp[1], co = (int)L->hp[2], kh = (int)L->hp[3], kw = (int)L->hp[4];
    if (dims != 1 && dims != 2) nn_fail("conv: dims must be 1 or 2"); if (ci < 1 || co < 1 || kh < 1 || kw < 1) nn_fail("conv: channels and kernel sizes must be positive");
    if (L->hp[5] < 1 || L->hp[6] < 1 || L->hp[9] < 1 || L->hp[10] < 1 || L->hp[7] < 0 || L->hp[8] < 0) nn_fail("conv: stride/dilation must be >= 1 and padding >= 0");
    int ws[4] = { co, ci, kh, kw }, bs[1] = { co };
    NnParam *w = nn_param_add(L, "weight", 4, ws, true, false); nn_init_tensor(&w->w, ci * kh * kw, co * kh * kw, init, arg);
    if (L->hp[11] != 0) nn_param_add(L, "bias", 1, bs, false, false);
    L->priv = nn_calloc(1, sizeof(ConvPriv));
}
static void conv_destroy(NnLayer *L) { ConvPriv *cp = L->priv; free(cp->col); free(cp->arg); free(cp); }
static void im2col(const nn_real *x, int C, int H, int W, int kh, int kw, int sh, int sw, int ph, int pw, int dh, int dw, int Ho, int Wo, nn_real *col) {
    size_t HW = (size_t)Ho * (size_t)Wo;
    for (int c = 0; c < C; c++) for (int i = 0; i < kh; i++) for (int j = 0; j < kw; j++) {
        nn_real *row = col + (((size_t)c * (size_t)kh + (size_t)i) * (size_t)kw + (size_t)j) * HW;
        for (int oh = 0; oh < Ho; oh++) {
            int ih = oh * sh - ph + i * dh;
            for (int ow = 0; ow < Wo; ow++) { int iw = ow * sw - pw + j * dw; row[(size_t)oh * (size_t)Wo + (size_t)ow] = (ih >= 0 && ih < H && iw >= 0 && iw < W) ? x[((size_t)c * (size_t)H + (size_t)ih) * (size_t)W + (size_t)iw] : 0.0; }
        }
    }
}
static void col2im_add(const nn_real *col, int C, int H, int W, int kh, int kw, int sh, int sw, int ph, int pw, int dh, int dw, int Ho, int Wo, nn_real *x) {
    size_t HW = (size_t)Ho * (size_t)Wo;
    for (int c = 0; c < C; c++) for (int i = 0; i < kh; i++) for (int j = 0; j < kw; j++) {
        const nn_real *row = col + (((size_t)c * (size_t)kh + (size_t)i) * (size_t)kw + (size_t)j) * HW;
        for (int oh = 0; oh < Ho; oh++) {
            int ih = oh * sh - ph + i * dh; if (ih < 0 || ih >= H) continue;
            for (int ow = 0; ow < Wo; ow++) { int iw = ow * sw - pw + j * dw; if (iw >= 0 && iw < W) x[((size_t)c * (size_t)H + (size_t)ih) * (size_t)W + (size_t)iw] += row[(size_t)oh * (size_t)Wo + (size_t)ow]; }
        }
    }
}
static void conv_params(const NnLayer *L, int *dims, int *ci, int *co, int *kh, int *kw, int *sh, int *sw, int *ph, int *pw, int *dh, int *dw) {
    *dims = (int)L->hp[0]; *ci = (int)L->hp[1]; *co = (int)L->hp[2]; *kh = (int)L->hp[3]; *kw = (int)L->hp[4];
    *sh = (int)L->hp[5]; *sw = (int)L->hp[6]; *ph = (int)L->hp[7]; *pw = (int)L->hp[8]; *dh = (int)L->hp[9]; *dw = (int)L->hp[10];
}
static void conv_fwd(NnLayer *L, const NnTensor *x, int training) {
    (void)training; int dims, ci, co, kh, kw, sh, sw, ph, pw, dh, dw; conv_params(L, &dims, &ci, &co, &kh, &kw, &sh, &sw, &ph, &pw, &dh, &dw);
    ConvPriv *cp = L->priv; Geom g; geom_from(L, x, dims, ci, &g, "conv");
    g.Ho = outdim(g.H, ph, dh, kh, sh); g.Wo = outdim(g.W, pw, dw, kw, sw);
    if (g.Ho < 1 || g.Wo < 1) nn_shape_fail(L, "an input at least as large as the (dilated) kernel", x);
    set_out(L, dims, g.N, co, g.Ho, g.Wo);
    size_t HW = (size_t)g.Ho * (size_t)g.Wo, CK = (size_t)ci * (size_t)kh * (size_t)kw, need = CK * HW;
    if (need > cp->colcap) { free(cp->col); cp->col = nn_malloc(sizeof(nn_real) * need); cp->colcap = need; }
    for (int n = 0; n < g.N; n++) {
        nn_real *y = L->out.d + (size_t)n * (size_t)co * HW; const nn_real *xn = x->d + (size_t)n * (size_t)ci * (size_t)g.H * (size_t)g.W;
        for (int o = 0; o < co; o++) { nn_real b = L->nparams > 1 ? P(1).w.d[o] : 0.0; for (size_t k = 0; k < HW; k++) y[(size_t)o * HW + k] = b; }
        im2col(xn, ci, g.H, g.W, kh, kw, sh, sw, ph, pw, dh, dw, g.Ho, g.Wo, cp->col);
        nn_gemm_nn(co, (int)HW, (int)CK, P(0).w.d, cp->col, y);
    }
    L->in = x;
}
static void conv_bwd(NnLayer *L, const NnTensor *dy) {
    int dims, ci, co, kh, kw, sh, sw, ph, pw, dh, dw; conv_params(L, &dims, &ci, &co, &kh, &kw, &sh, &sw, &ph, &pw, &dh, &dw);
    ConvPriv *cp = L->priv; const NnTensor *x = L->in; Geom g; geom_from(L, x, dims, ci, &g, "conv");
    g.Ho = outdim(g.H, ph, dh, kh, sh); g.Wo = outdim(g.W, pw, dw, kw, sw);
    size_t HW = (size_t)g.Ho * (size_t)g.Wo, CK = (size_t)ci * (size_t)kh * (size_t)kw;
    nn_real *dcol = L->need_dx ? nn_malloc(sizeof(nn_real) * CK * HW) : NULL;
    if (L->need_dx) { nt_resize(&L->dx, x->nd, x->shape); nt_zero(&L->dx); }
    for (int n = 0; n < g.N; n++) {
        const nn_real *d = dy->d + (size_t)n * (size_t)co * HW; const nn_real *xn = x->d + (size_t)n * (size_t)ci * (size_t)g.H * (size_t)g.W;
        im2col(xn, ci, g.H, g.W, kh, kw, sh, sw, ph, pw, dh, dw, g.Ho, g.Wo, cp->col);
        nn_gemm_nt(co, (int)CK, (int)HW, d, cp->col, P(0).g.d);
        if (L->nparams > 1) for (int o = 0; o < co; o++) { nn_real s = 0; for (size_t k = 0; k < HW; k++) s += d[(size_t)o * HW + k]; P(1).g.d[o] += s; }
        if (L->need_dx) {
            memset(dcol, 0, sizeof(nn_real) * CK * HW); nn_gemm_tn((int)CK, (int)HW, co, P(0).w.d, d, dcol);
            col2im_add(dcol, ci, g.H, g.W, kh, kw, sh, sw, ph, pw, dh, dw, g.Ho, g.Wo, L->dx.d + (size_t)n * (size_t)ci * (size_t)g.H * (size_t)g.W);
        }
    }
    free(dcol);
}
const NnOps nn_ops_conv = { conv_fwd, conv_bwd, conv_destroy };

/* ================================================================== pooling */
void nn_setup_pool(NnLayer *L, int init, double arg) {
    (void)init; (void)arg; if (L->hp[2] < 1 || L->hp[3] < 1 || L->hp[4] < 1 || L->hp[5] < 1 || L->hp[6] < 0 || L->hp[7] < 0) nn_fail("pool: kernel/stride must be >= 1 and padding >= 0");
    if (L->hp[6] > L->hp[2] / 2.0 || L->hp[7] > L->hp[3] / 2.0) nn_fail("pool: padding must be at most half the kernel size");
    L->priv = nn_calloc(1, sizeof(ConvPriv));
}
static void pool_fwd(NnLayer *L, const NnTensor *x, int training) {
    (void)training; int avg = (int)L->hp[0], dims = (int)L->hp[1], kh = (int)L->hp[2], kw = (int)L->hp[3], sh = (int)L->hp[4], sw = (int)L->hp[5], ph = (int)L->hp[6], pw = (int)L->hp[7];
    ConvPriv *cp = L->priv; Geom g; geom_from(L, x, dims, -1, &g, "pool"); g.Ho = outdim(g.H, ph, 1, kh, sh); g.Wo = outdim(g.W, pw, 1, kw, sw);
    if (g.Ho < 1 || g.Wo < 1) nn_shape_fail(L, "an input at least as large as the pooling window", x);
    set_out(L, dims, g.N, g.C, g.Ho, g.Wo); size_t planes = (size_t)g.N * (size_t)g.C, HWo = (size_t)g.Ho * (size_t)g.Wo, HWi = (size_t)g.H * (size_t)g.W;
    if (!avg && planes * HWo > cp->argcap) { free(cp->arg); cp->arg = nn_malloc(sizeof(int) * planes * HWo); cp->argcap = planes * HWo; }
    for (size_t p = 0; p < planes; p++) {
        const nn_real *in = x->d + p * HWi; nn_real *out = L->out.d + p * HWo;
        for (int oh = 0; oh < g.Ho; oh++) for (int ow = 0; ow < g.Wo; ow++) {
            nn_real best = -DBL_MAX, sum = 0; int bi = -1;
            for (int i = 0; i < kh; i++) { int ih = oh * sh - ph + i; if (ih < 0 || ih >= g.H) continue;
                for (int j = 0; j < kw; j++) { int iw = ow * sw - pw + j; if (iw < 0 || iw >= g.W) continue; nn_real v = in[(size_t)ih * (size_t)g.W + (size_t)iw]; sum += v; if (v > best) { best = v; bi = ih * g.W + iw; } } }
            size_t o = (size_t)oh * (size_t)g.Wo + (size_t)ow;
            if (avg) out[o] = sum / (nn_real)(kh * kw); else { out[o] = bi >= 0 ? best : 0.0; cp->arg[p * HWo + o] = bi; }
        }
    }
    L->in = x;
}
static void pool_bwd(NnLayer *L, const NnTensor *dy) {
    if (!L->need_dx) return; int avg = (int)L->hp[0], dims = (int)L->hp[1], kh = (int)L->hp[2], kw = (int)L->hp[3], sh = (int)L->hp[4], sw = (int)L->hp[5], ph = (int)L->hp[6], pw = (int)L->hp[7];
    ConvPriv *cp = L->priv; const NnTensor *x = L->in; Geom g; geom_from(L, x, dims, -1, &g, "pool"); g.Ho = outdim(g.H, ph, 1, kh, sh); g.Wo = outdim(g.W, pw, 1, kw, sw);
    nt_resize(&L->dx, x->nd, x->shape); nt_zero(&L->dx); size_t planes = (size_t)g.N * (size_t)g.C, HWo = (size_t)g.Ho * (size_t)g.Wo, HWi = (size_t)g.H * (size_t)g.W;
    for (size_t p = 0; p < planes; p++) {
        nn_real *dx = L->dx.d + p * HWi; const nn_real *d = dy->d + p * HWo;
        for (int oh = 0; oh < g.Ho; oh++) for (int ow = 0; ow < g.Wo; ow++) {
            size_t o = (size_t)oh * (size_t)g.Wo + (size_t)ow;
            if (avg) { nn_real v = d[o] / (nn_real)(kh * kw);
                for (int i = 0; i < kh; i++) { int ih = oh * sh - ph + i; if (ih < 0 || ih >= g.H) continue; for (int j = 0; j < kw; j++) { int iw = ow * sw - pw + j; if (iw >= 0 && iw < g.W) dx[(size_t)ih * (size_t)g.W + (size_t)iw] += v; } } }
            else { int bi = cp->arg[p * HWo + o]; if (bi >= 0) dx[bi] += d[o]; }
        }
    }
}
const NnOps nn_ops_pool = { pool_fwd, pool_bwd, conv_destroy };

/* ================================================================== global average pooling: [N,C,...] -> [N,C] */
void nn_setup_gap(NnLayer *L, int init, double arg) { (void)L; (void)init; (void)arg; }
static void gap_fwd(NnLayer *L, const NnTensor *x, int training) {
    (void)training; if (x->nd < 3) nn_shape_fail(L, "[batch, channels, ...] with at least one spatial dimension", x);
    int sh[2] = { x->shape[0], x->shape[1] }; nt_resize(&L->out, 2, sh); size_t planes = (size_t)sh[0] * (size_t)sh[1], S = planes ? x->n / planes : 0;
    for (size_t p = 0; p < planes; p++) { nn_real s = 0; for (size_t k = 0; k < S; k++) s += x->d[p * S + k]; L->out.d[p] = S ? s / (nn_real)S : 0; }
    L->in = x;
}
static void gap_bwd(NnLayer *L, const NnTensor *dy) {
    if (!L->need_dx) return; const NnTensor *x = L->in; nt_resize(&L->dx, x->nd, x->shape); size_t planes = (size_t)x->shape[0] * (size_t)x->shape[1], S = planes ? x->n / planes : 0;
    for (size_t p = 0; p < planes; p++) for (size_t k = 0; k < S; k++) L->dx.d[p * S + k] = dy->d[p] / (nn_real)S;
}
const NnOps nn_ops_gap = { gap_fwd, gap_bwd, NULL };
