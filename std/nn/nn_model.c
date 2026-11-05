/* std.nn model layer: registry, containers, parameters, summary, gradient check */
#include "nn_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* ------------------------------------------------------------------ registry */
static const struct { void (*setup)(NnLayer *, int, double); const NnOps *ops; const char *name; } REG[NNL_KIND_MAX] = {
    [NNL_DENSE] = { nn_setup_dense, &nn_ops_dense, "dense" }, [NNL_ACT] = { nn_setup_act, &nn_ops_act, "activation" },
    [NNL_DROPOUT] = { nn_setup_dropout, &nn_ops_dropout, "dropout" }, [NNL_FLATTEN] = { nn_setup_flatten, &nn_ops_flatten, "flatten" },
    [NNL_RESHAPE] = { nn_setup_reshape, &nn_ops_reshape, "reshape" }, [NNL_EMBED] = { nn_setup_embed, &nn_ops_embed, "embedding" },
    [NNL_BATCHNORM] = { nn_setup_batchnorm, &nn_ops_batchnorm, "batchnorm" }, [NNL_LAYERNORM] = { nn_setup_layernorm, &nn_ops_layernorm, "layernorm" },
    [NNL_CONV] = { nn_setup_conv, &nn_ops_conv, "conv" }, [NNL_POOL] = { nn_setup_pool, &nn_ops_pool, "pool" }, [NNL_GAP] = { nn_setup_gap, &nn_ops_gap, "global_avg_pool" },
    [NNL_RNN] = { nn_setup_rnn, &nn_ops_rnn, "recurrent" }, [NNL_ATTN] = { nn_setup_attn, &nn_ops_attn, "attention" }, [NNL_POSENC] = { nn_setup_posenc, &nn_ops_posenc, "pos_encoding" },
    [NNL_SEQ] = { NULL, &nn_ops_seq, "sequential" }, [NNL_RESIDUAL] = { NULL, &nn_ops_residual, "residual" },
};
const char *nn_kind_name(int k) { return (k > 0 && k < NNL_KIND_MAX && REG[k].name) ? REG[k].name : "?"; }

void nn_shape_fail(const NnLayer *L, const char *expect, const NnTensor *x) {
    nn_fail("layer '%s' (%s) expects an input with %s, but got shape %s", L->name, nn_kind_name(L->kind), expect, nt_shape_str(x->nd, x->shape));
}
#define NN_MAX_LAYER_PARAMS 16
NnParam *nn_param_add(NnLayer *L, const char *name, int nd, const int *shape, bool decay, bool buffer) {
    /* fixed-capacity array: NnParam pointers stay valid for the layer's whole life (setup code holds them across adds) */
    if (!L->params) L->params = nn_calloc(NN_MAX_LAYER_PARAMS, sizeof(NnParam));
    if (L->nparams >= NN_MAX_LAYER_PARAMS) nn_fail("layer '%s' has too many parameter tensors", L->name);
    NnParam *p = &L->params[L->nparams++]; memset(p, 0, sizeof *p);
    snprintf(p->name, sizeof p->name, "%s", name); nt_alloc(&p->w, nd, shape); if (!buffer) nt_alloc(&p->g, nd, shape);
    p->decay = decay; p->is_buffer = buffer; return p;
}
NnLayer *nn_layer_new(int kind, const double *hp, int nhp, int init, double arg) {
    if (kind <= 0 || kind >= NNL_KIND_MAX || !REG[kind].ops) nn_fail("unknown layer kind %d", kind);
    if (nhp < 0 || nhp > 16) nn_fail("too many layer hyper-parameters (%d)", nhp);
    NnLayer *L = nn_calloc(1, sizeof *L); L->kind = kind; L->ops = REG[kind].ops; L->nhp = nhp; if (nhp) memcpy(L->hp, hp, sizeof(double) * (size_t)nhp);
    snprintf(L->name, sizeof L->name, "%s", REG[kind].name); nt_init(&L->out); nt_init(&L->dx);
    if (REG[kind].setup) REG[kind].setup(L, init, arg);
    return L;
}
void nn_layer_free(NnLayer *L) {
    if (!L) return; if (L->ops && L->ops->destroy) L->ops->destroy(L);
    for (int i = 0; i < L->nparams; i++) { nt_free(&L->params[i].w); nt_free(&L->params[i].g); } free(L->params);
    for (int i = 0; i < L->nkids; i++) nn_layer_free(L->kids[i]); free(L->kids); nt_free(&L->out); nt_free(&L->dx); free(L);
}
void nn_container_append(NnLayer *C, NnLayer *k) {
    if (C->kind != NNL_SEQ && C->kind != NNL_RESIDUAL) nn_fail("only sequential and residual layers can contain other layers");
    if (C->kind == NNL_RESIDUAL && C->nkids >= 1) nn_fail("a residual block wraps exactly one inner sequential; append layers to that");
    if (k->has_parent) nn_fail("this layer already belongs to a model; create a new layer (or use nn.clone on the model) instead of reusing it");
    k->has_parent = true;
    if (C->nkids == C->kidcap) { C->kidcap = C->kidcap ? C->kidcap * 2 : 4; C->kids = nn_realloc(C->kids, sizeof(NnLayer *) * (size_t)C->kidcap); }
    C->kids[C->nkids++] = k;
}
void nn_forward(NnLayer *L, const NnTensor *x, int training) { L->ops->forward(L, x, training); L->has_forward = true; }
void nn_backward(NnLayer *L, const NnTensor *dy) { if (!L->has_forward) nn_fail("backward called before forward on layer '%s'", L->name); L->ops->backward(L, dy); }

/* ------------------------------------------------------------------ sequential: priv = held copy of the input */
static void seq_fwd(NnLayer *L, const NnTensor *x, int training) {
    NnTensor *held = L->priv; if (!held) { held = nn_calloc(1, sizeof *held); nt_init(held); L->priv = held; }
    nt_copy(held, x); L->in = held; L->training = training != 0; const NnTensor *cur = held;
    for (int i = 0; i < L->nkids; i++) { nn_forward(L->kids[i], cur, training); cur = &L->kids[i]->out; }
    nt_copy(&L->out, cur);
}
static void seq_bwd(NnLayer *L, const NnTensor *dy) {
    const NnTensor *g = dy;
    for (int i = L->nkids - 1; i >= 0; i--) { L->kids[i]->need_dx = (i > 0) || L->need_dx; nn_backward(L->kids[i], g); g = &L->kids[i]->dx; }
    if (L->need_dx) nt_copy(&L->dx, L->nkids ? g : dy);
}
static void seq_destroy(NnLayer *L) { if (L->priv) { nt_delete(L->priv); L->priv = NULL; } }
const NnOps nn_ops_seq = { seq_fwd, seq_bwd, seq_destroy };

/* residual: y = x + f(x); kids[0] is the inner sequential */
static void res_fwd(NnLayer *L, const NnTensor *x, int training) {
    if (L->nkids != 1) nn_fail("residual block has no inner layers"); NnLayer *f = L->kids[0]; nn_forward(f, x, training);
    if (!nt_same_shape(&f->out, x)) nn_fail("residual block: inner layers change the shape %s to %s, so the skip connection cannot be added", nt_shape_str(x->nd, x->shape), nt_shape_str(f->out.nd, f->out.shape));
    nt_resize(&L->out, x->nd, x->shape); for (size_t i = 0; i < x->n; i++) L->out.d[i] = x->d[i] + f->out.d[i]; L->in = x;
}
static void res_bwd(NnLayer *L, const NnTensor *dy) {
    NnLayer *f = L->kids[0]; f->need_dx = L->need_dx; nn_backward(f, dy);
    if (L->need_dx) { nt_resize(&L->dx, dy->nd, dy->shape); for (size_t i = 0; i < dy->n; i++) L->dx.d[i] = dy->d[i] + f->dx.d[i]; }
}
const NnOps nn_ops_residual = { res_fwd, res_bwd, NULL };

/* ------------------------------------------------------------------ parameters */
static void collect(NnLayer *L, NnParam ***out, int *n, int *cap) {
    for (int i = 0; i < L->nparams; i++) { if (*n == *cap) { *cap = *cap ? *cap * 2 : 16; *out = nn_realloc(*out, sizeof(NnParam *) * (size_t)*cap); } (*out)[(*n)++] = &L->params[i]; }
    for (int i = 0; i < L->nkids; i++) collect(L->kids[i], out, n, cap);
}
int nn_collect_params(NnLayer *L, NnParam ***out) { int n = 0, cap = 0; *out = NULL; collect(L, out, &n, &cap); return n; }
long nn_count_params(NnLayer *L, bool trainable_only) {
    NnParam **ps; int n = nn_collect_params(L, &ps); long t = 0;
    for (int i = 0; i < n; i++) { if (ps[i]->is_buffer) continue; if (trainable_only && ps[i]->frozen) continue; t += (long)ps[i]->w.n; }
    free(ps); return t;
}
void nn_zero_grad(NnLayer *L) { NnParam **ps; int n = nn_collect_params(L, &ps); for (int i = 0; i < n; i++) if (!ps[i]->is_buffer) nt_zero(&ps[i]->g); free(ps); }
void nn_set_frozen(NnLayer *L, bool f) { for (int i = 0; i < L->nparams; i++) L->params[i].frozen = f; for (int i = 0; i < L->nkids; i++) nn_set_frozen(L->kids[i], f); }

/* ------------------------------------------------------------------ summary */
static void describe(const NnLayer *L, char *b, size_t n) {
    const double *h = L->hp;
    switch (L->kind) {
    case NNL_DENSE: snprintf(b, n, "dense %d->%d%s", (int)h[0], (int)h[1], h[2] ? "" : " (no bias)"); break;
    case NNL_ACT: if ((int)h[0] == NNA_LEAKY || (int)h[0] == NNA_ELU) snprintf(b, n, "%s(%g)", nn_act_name((int)h[0]), h[1]); else snprintf(b, n, "%s", nn_act_name((int)h[0])); break;
    case NNL_DROPOUT: snprintf(b, n, "dropout p=%g", h[0]); break;
    case NNL_EMBED: snprintf(b, n, "embedding %d x %d", (int)h[0], (int)h[1]); break;
    case NNL_BATCHNORM: snprintf(b, n, "batchnorm %d", (int)h[0]); break;
    case NNL_LAYERNORM: snprintf(b, n, "layernorm %d", (int)h[0]); break;
    case NNL_CONV: if ((int)h[0] == 1) snprintf(b, n, "conv1d %d->%d k=%d s=%d p=%d", (int)h[1], (int)h[2], (int)h[4], (int)h[6], (int)h[8]); else snprintf(b, n, "conv2d %d->%d k=%dx%d s=%d p=%d", (int)h[1], (int)h[2], (int)h[3], (int)h[4], (int)h[5], (int)h[7]); break;
    case NNL_POOL: snprintf(b, n, "%spool%dd k=%d s=%d", h[0] ? "avg" : "max", (int)h[1], (int)h[1] == 2 ? (int)h[2] : (int)h[3], (int)h[1] == 2 ? (int)h[4] : (int)h[5]); break;
    case NNL_RNN: snprintf(b, n, "%s %d->%d%s", (int)h[0] == 0 ? "rnn" : (int)h[0] == 1 ? "lstm" : "gru", (int)h[1], (int)h[2], h[3] ? " seq" : ""); break;
    case NNL_ATTN: snprintf(b, n, "attention d=%d heads=%d%s", (int)h[0], (int)h[1], h[2] ? " causal" : ""); break;
    case NNL_RESHAPE: snprintf(b, n, "reshape"); break;
    case NNL_SEQ: snprintf(b, n, "sequential"); break;
    case NNL_RESIDUAL: snprintf(b, n, "residual"); break;
    default: snprintf(b, n, "%s", nn_kind_name(L->kind)); break;
    }
}
static long layer_params(NnLayer *L) { return nn_count_params(L, false); }
static void summ_rows(const NnLayer *L, int depth, char **buf, size_t *len, size_t *cap, int *idx) {
    for (int i = 0; i < L->nkids; i++) {
        NnLayer *k = L->kids[i]; char d[96], line[256], shp[64] = "";
        describe(k, d, sizeof d);
        if (k->out.nd > 1) { size_t o = 0; o += (size_t)snprintf(shp, sizeof shp, "["); for (int a = 1; a < k->out.nd && o < 48; a++) o += (size_t)snprintf(shp + o, sizeof shp - o, "%s%d", a > 1 ? "," : "", k->out.shape[a]); snprintf(shp + o, sizeof shp - o, "]"); }
        else if (k->out.nd == 1) snprintf(shp, sizeof shp, "[]");
        char name[128]; snprintf(name, sizeof name, "%*s%s", depth * 2, "", d);
        int w = snprintf(line, sizeof line, "%3d  %-36s %-18s %10ld\n", (*idx)++, name, shp, layer_params(k));
        if (*len + (size_t)w + 1 > *cap) { while (*len + (size_t)w + 1 > *cap) *cap *= 2; *buf = nn_realloc(*buf, *cap); }
        memcpy(*buf + *len, line, (size_t)w); *len += (size_t)w; (*buf)[*len] = 0;
        if (k->nkids) summ_rows(k, depth + 1, buf, len, cap, idx);
    }
}
char *nn_summary(NnLayer *m, int nd, const int *input_shape) {
    NnTensor x; nt_init(&x); nt_alloc(&x, nd, input_shape); NnRngState st; nn_rng_save(&st);
    for (int attempt = 0; attempt < 1; attempt++) nn_forward(m, &x, 0);          /* shapes only; eval mode, batchnorm statistics untouched */
    nn_rng_restore(&st); nt_free(&x);
    size_t cap = 1024, len = 0; char *buf = nn_malloc(cap); buf[0] = 0; char head[200];
    int w = snprintf(head, sizeof head, "Model: sequential (%d layer%s)\n%3s  %-36s %-18s %10s\n", m->nkids, m->nkids == 1 ? "" : "s", "#", "Layer", "Output shape", "Params");
    memcpy(buf, head, (size_t)w); len = (size_t)w; buf[len] = 0; int idx = 0; summ_rows(m, 0, &buf, &len, &cap, &idx);
    long tot = nn_count_params(m, false), tr = nn_count_params(m, true); char tail[200];
    NnParam **ps; int np = nn_collect_params(m, &ps); long bufs = 0; for (int i = 0; i < np; i++) if (ps[i]->is_buffer) bufs += (long)ps[i]->w.n; free(ps);
    w = snprintf(tail, sizeof tail, "Total params: %ld  (trainable: %ld, frozen: %ld, buffers: %ld)\n", tot, tr, tot - tr, bufs);
    if (len + (size_t)w + 1 > cap) { cap = len + (size_t)w + 1; buf = nn_realloc(buf, cap); } memcpy(buf + len, tail, (size_t)w + 1); return buf;
}

/* ------------------------------------------------------------------ gradient check (central differences) */
static double gc_eval(NnLayer *m, NnLoss *loss, const NnTensor *x, const NnTensor *y, const NnRngState *st) {
    nn_rng_restore(st); nn_forward(m, x, 1); return nn_loss_eval(loss, &m->out, y, false);
}
bool nn_gradcheck(NnLayer *m, NnLoss *loss, const NnTensor *x, const NnTensor *y, double eps, int samples,
                  double *max_rel, long *checked, char *worst, size_t worstn) {
    NnRngState st; nn_rng_save(&st); NnParam **ps; int np = nn_collect_params(m, &ps);
    NnTensor *snap = nn_calloc((size_t)np, sizeof(NnTensor));                      /* running statistics are restored afterwards */
    for (int i = 0; i < np; i++) if (ps[i]->is_buffer) nt_copy(&snap[i], &ps[i]->w);
    nn_zero_grad(m); nn_rng_restore(&st); nn_forward(m, x, 1); nn_loss_eval(loss, &m->out, y, true);
    NnTensor dx; nt_init(&dx); nn_backward_loss(m, loss, true, &dx);
    double mr = 0; long cnt = 0; snprintf(worst, worstn, "-"); uint64_t lcg = 88172645463325252ULL;
    NnTensor xw; nt_init(&xw); nt_copy(&xw, x);
    for (int pi = -1; pi < np; pi++) {
        NnTensor *w; const nn_real *g; const char *nm;
        if (pi < 0) { w = &xw; g = dx.d; nm = "input"; if (dx.n != xw.n || (m->nkids && m->kids[0]->kind == NNL_EMBED)) continue; }   /* token ids are not differentiable */ else { if (ps[pi]->is_buffer) continue; w = &ps[pi]->w; g = ps[pi]->g.d; nm = ps[pi]->name; }
        size_t n = w->n; int take = (samples <= 0 || (size_t)samples >= n) ? (int)n : samples;
        for (int s = 0; s < take; s++) {
            size_t k; if (take == (int)n) k = (size_t)s; else { lcg = lcg * 6364136223846793005ULL + 1442695040888963407ULL; k = (size_t)((lcg >> 33) % n); }
            if (pi >= 0 && (long)k >= ps[pi]->skip_lo && (long)k < ps[pi]->skip_hi) continue;     /* intentionally fixed (padding row) */
            nn_real orig = w->d[k]; w->d[k] = orig + eps; double fp = gc_eval(m, loss, pi < 0 ? &xw : x, y, &st);
            w->d[k] = orig - eps; double fm = gc_eval(m, loss, pi < 0 ? &xw : x, y, &st); w->d[k] = orig;
            double num = (fp - fm) / (2 * eps), an = g[k], den = fabs(num) + fabs(an); cnt++;
            if (den < 1e-7) continue;               /* gradient itself is at finite-difference noise level (e.g. a bias feeding batchnorm is exactly 0) */
            double rel = fabs(num - an) / den;
            if (rel > mr) { mr = rel; snprintf(worst, worstn, "%s[%zu] analytic=%.6g numeric=%.6g", nm, k, an, num); }
        }
    }
    for (int i = 0; i < np; i++) if (ps[i]->is_buffer) { nt_copy(&ps[i]->w, &snap[i]); nt_free(&snap[i]); }
    free(snap); free(ps); nt_free(&xw); nt_free(&dx); nn_rng_restore(&st);
    *max_rel = mr; *checked = cnt; return mr < 1e-4;
}
