/* std.nn script API: layers, models, losses, optimizers, training, persistence */
#include "nn_glue.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#define FN(name) static KcValue name(KcInterp *ip, KcValue *a, int n, KcPos pos)
#define NN_VERSION_STR "1.0.0"

/* ------------------------------------------------------------------ construction helpers */
static KcValue mk_layer(int kind, const double *hp, int nhp, const char *init, double iarg, const char *name) {
    int id = nn_init_from_name(init);
    if (id < 0 || id == NNI_NONE) NNERR("nn: unknown initializer '%s' (xavier, xavier_normal, he, he_uniform, lecun, lecun_uniform, normal, uniform, zeros, ones, constant, orthogonal)", init);
    NnLayer *L = nn_layer_new(kind, hp, nhp, id, iarg); if (name) snprintf(L->name, sizeof L->name, "%s", name); return nnh_wrap(NNH_LAYER, L);
}
static NnLayer *lay(KcValue *a, int n, int i, const char *fn) { return nnh_need(a, n, i, NNH_LAYER, fn); }
static NnLayer *seq_arg(KcValue *a, int n, int i, const char *fn) {
    NnLayer *m = lay(a, n, i, fn);
    if (m->kind != NNL_SEQ) NNERR("nn.%s: needs a model built with nn.sequential/nn.mlp (wrap a single layer as nn.sequential([layer]))", fn); return m;
}
static void pair_opt(KcValue *v, int def, int *h, int *w, const char *fn, const char *what) {
    if (!v) { *h = *w = def; return; }
    if (v->type == VAL_INT || v->type == VAL_FLOAT) { double d = v->type == VAL_INT ? (double)v->as.i : v->as.f; if (d != floor(d)) NNERR("nn.%s: %s must be an integer", fn, what); *h = *w = (int)d; return; }
    if (v->type == VAL_ARRAY && v->as.arr->count == 2) { KcValue p[2] = { v->as.arr->items[0], v->as.arr->items[1] }; for (int i = 0; i < 2; i++) { if (p[i].type != VAL_INT) NNERR("nn.%s: %s must be an integer or [height, width]", fn, what); } *h = (int)p[0].as.i; *w = (int)p[1].as.i; return; }
    NNERR("nn.%s: %s must be an integer or [height, width]", fn, what);
}

/* ------------------------------------------------------------------ layers */
FN(f_dense) {
    NNAPI(); need_n(n, 2, 40, "dense"); int64_t in = arg_int(a, n, 0, "dense"), out = arg_int(a, n, 1, "dense"); NnOpts o; nnopts_parse(&o, a, n, 2, "dense");
    bool bias = nnopts_bool(&o, "bias", true); const char *init = nnopts_str(&o, "init", "xavier"); double ia = nnopts_num(&o, "init_arg", 0); const char *nm = nnopts_str(&o, "name", NULL); nnopts_done(&o);
    if (in < 1 || out < 1 || in > 1000000 || out > 1000000) NNERR("nn.dense: sizes must be between 1 and 1000000"); double hp[3] = { (double)in, (double)out, bias }; return mk_layer(NNL_DENSE, hp, 3, init, ia, nm);
}
static KcValue act_layer(int id, double param, const char *name) { double hp[2] = { (double)id, param }; return mk_layer(NNL_ACT, hp, 2, "xavier", 0, name); }
#define ACTFN(fname, label, id, defp) FN(fname) { NNAPI(); need_n(n, 0, 1, label); double p = n ? arg_num(a, n, 0, label) : defp; return act_layer(id, p, NULL); }
ACTFN(f_relu, "relu", NNA_RELU, 0) ACTFN(f_relu6, "relu6", NNA_RELU6, 0) ACTFN(f_leaky_relu, "leaky_relu", NNA_LEAKY, 0.01) ACTFN(f_elu, "elu", NNA_ELU, 1.0) ACTFN(f_selu, "selu", NNA_SELU, 0)
ACTFN(f_gelu, "gelu", NNA_GELU, 0) ACTFN(f_sigmoid, "sigmoid", NNA_SIGMOID, 0) ACTFN(f_tanh, "tanh", NNA_TANH, 0) ACTFN(f_softplus, "softplus", NNA_SOFTPLUS, 0) ACTFN(f_silu, "silu", NNA_SILU, 0)
ACTFN(f_mish, "mish", NNA_MISH, 0) ACTFN(f_softsign, "softsign", NNA_SOFTSIGN, 0) ACTFN(f_hard_sigmoid, "hard_sigmoid", NNA_HARDSIG, 0) ACTFN(f_log_softmax, "log_softmax", NNA_LOGSOFTMAX, 0)
FN(f_activation) { NNAPI(); need_n(n, 1, 2, "activation"); const char *nm = arg_str(a, n, 0, "activation"); int id = nn_act_from_name(nm); if (id < 0) NNERR("nn.activation: unknown activation '%s'", nm); return act_layer(id, n > 1 ? arg_num(a, n, 1, "activation") : (id == NNA_LEAKY ? 0.01 : 1.0), NULL); }
FN(f_apply) {
    NNAPI(); need_n(n, 2, 3, "apply"); const char *nm = arg_str(a, n, 0, "apply"); int id = nn_act_from_name(nm); if (id < 0) NNERR("nn.apply: unknown activation '%s'", nm);
    TArg x; NnTensor *t = targ_get(&x, a, n, 1, "apply"); double hp[2] = { (double)id, n > 2 ? arg_num(a, n, 2, "apply") : (id == NNA_LEAKY ? 0.01 : 1.0) }; NnLayer *L = nn_layer_new(NNL_ACT, hp, 2, NNI_ZEROS, 0);
    nn_forward(L, t, 0); NnTensor *r = nn_malloc(sizeof *r); nt_init(r); nt_copy(r, &L->out); nn_layer_free(L); targ_free(&x); return nn_ret_tensor(r);
}
FN(f_softmax) {                                   /* nn.softmax() -> layer;  nn.softmax(tensor [, axis]) -> tensor */
    NNAPI(); if (n == 0) return act_layer(NNA_SOFTMAX, 0, NULL);
    TArg x; NnTensor *p = targ_get(&x, a, n, 0, "softmax"); if (p->nd < 1) { targ_free(&x); NNERR("nn.softmax: needs at least 1 dimension"); }
    int axis = nt_norm_axis(n > 1 ? (int)arg_int(a, n, 1, "softmax") : -1, p->nd, "softmax"); NnTensor *r = nn_malloc(sizeof *r); nt_init(r); nt_softmax_axis(p, axis, r); targ_free(&x); return nn_ret_tensor(r);
}
FN(f_dropout) { NNAPI(); need_n(n, 1, 1, "dropout"); double p = arg_num(a, n, 0, "dropout"); if (!(p >= 0 && p < 1)) NNERR("nn.dropout: rate must be in [0, 1)"); double hp[1] = { p }; return mk_layer(NNL_DROPOUT, hp, 1, "xavier", 0, NULL); }
FN(f_flatten) { NNAPI(); need_n(n, 0, 0, "flatten"); return mk_layer(NNL_FLATTEN, NULL, 0, "xavier", 0, NULL); }
FN(f_reshape_layer) {
    NNAPI(); need_n(n, 1, 1, "reshape_layer"); int sh[NN_MAXDIM]; int nd = arg_shape(a, n, 0, sh, "reshape_layer"); if (nd < 1 || nd > NN_MAXDIM - 1) NNERR("nn.reshape_layer: give 1 to %d per-sample dimensions", NN_MAXDIM - 1);
    double hp[8] = { (double)nd }; for (int i = 0; i < nd; i++) hp[1 + i] = sh[i]; return mk_layer(NNL_RESHAPE, hp, 1 + nd, "xavier", 0, NULL);
}
FN(f_embedding) {
    NNAPI(); need_n(n, 2, 40, "embedding"); int64_t V = arg_int(a, n, 0, "embedding"), D = arg_int(a, n, 1, "embedding"); NnOpts o; nnopts_parse(&o, a, n, 2, "embedding");
    int64_t pad = nnopts_int(&o, "padding_idx", -1); double ia = nnopts_num(&o, "init_arg", 0); const char *nm = nnopts_str(&o, "name", NULL); nnopts_done(&o);
    if (V < 1 || D < 1 || V > 100000000 || D > 100000) NNERR("nn.embedding: vocabulary and dimension must be positive"); double hp[3] = { (double)V, (double)D, (double)pad }; return mk_layer(NNL_EMBED, hp, 3, "xavier", ia, nm);
}
FN(f_batchnorm) {
    NNAPI(); need_n(n, 1, 40, "batchnorm"); int64_t F = arg_int(a, n, 0, "batchnorm"); NnOpts o; nnopts_parse(&o, a, n, 1, "batchnorm");
    double mom = nnopts_num(&o, "momentum", 0.1), eps = nnopts_num(&o, "eps", 1e-5); bool aff = nnopts_bool(&o, "affine", true); nnopts_done(&o); if (F < 1) NNERR("nn.batchnorm: features must be >= 1");
    double hp[4] = { (double)F, mom, eps, aff }; return mk_layer(NNL_BATCHNORM, hp, 4, "xavier", 0, NULL);
}
FN(f_layernorm) {
    NNAPI(); need_n(n, 1, 40, "layernorm"); int64_t F = arg_int(a, n, 0, "layernorm"); NnOpts o; nnopts_parse(&o, a, n, 1, "layernorm"); double eps = nnopts_num(&o, "eps", 1e-5); bool aff = nnopts_bool(&o, "affine", true); nnopts_done(&o);
    if (F < 1) NNERR("nn.layernorm: features must be >= 1"); double hp[3] = { (double)F, eps, aff }; return mk_layer(NNL_LAYERNORM, hp, 3, "xavier", 0, NULL);
}
static KcValue conv_api(KcValue *a, int n, KcPos pos, int dims) {
    NN_POS = &pos; const char *fn = dims == 2 ? "conv2d" : "conv1d"; need_n(n, 3, 40, fn); int64_t ci = arg_int(a, n, 0, fn), co = arg_int(a, n, 1, fn); NnOpts o; nnopts_parse(&o, a, n, 3, fn);
    int kh, kw, sh, sw, ph, pw, dh, dw; KcValue kv = a[2]; pair_opt(&kv, 1, &kh, &kw, fn, "kernel"); pair_opt(nnopts_val(&o, "stride"), 1, &sh, &sw, fn, "stride"); pair_opt(nnopts_val(&o, "dilation"), 1, &dh, &dw, fn, "dilation");
    KcValue *pv = nnopts_val(&o, "padding"); if (pv && pv->type == VAL_STRING) { if (strcmp(pv->as.s->data, "same")) NNERR("nn.%s: padding must be an integer, [h, w] or \"same\"", fn); int th = dh * (kh - 1), tw = dw * (kw - 1);
        if (th % 2 || tw % 2) NNERR("nn.%s: \"same\" padding needs odd kernel sizes (with dilation 1)", fn); ph = th / 2; pw = tw / 2; } else pair_opt(pv, 0, &ph, &pw, fn, "padding");
    bool bias = nnopts_bool(&o, "bias", true); const char *init = nnopts_str(&o, "init", "he"); double ia = nnopts_num(&o, "init_arg", 0); const char *nm = nnopts_str(&o, "name", NULL); nnopts_done(&o);
    if (dims == 1) { if (kh != kw && a[2].type == VAL_ARRAY) NNERR("nn.conv1d: kernel must be an integer"); kw = kh; kh = 1; sw = sh; sh = 1; pw = ph; ph = 0; dw = dh; dh = 1; }
    if (ci < 1 || co < 1) NNERR("nn.%s: channel counts must be >= 1", fn); double hp[12] = { (double)dims, (double)ci, (double)co, (double)kh, (double)kw, (double)sh, (double)sw, (double)ph, (double)pw, (double)dh, (double)dw, (double)bias };
    return mk_layer(NNL_CONV, hp, 12, init, ia, nm);
}
FN(f_conv2d) { return conv_api(a, n, pos, 2); }
FN(f_conv1d) { return conv_api(a, n, pos, 1); }
static KcValue pool_api(KcValue *a, int n, KcPos pos, int avg, int dims) {
    NN_POS = &pos; const char *fn = avg ? (dims == 2 ? "avgpool2d" : "avgpool1d") : (dims == 2 ? "maxpool2d" : "maxpool1d"); need_n(n, 1, 40, fn); NnOpts o; nnopts_parse(&o, a, n, 1, fn);
    int kh, kw, sh, sw, ph, pw; KcValue kv = a[0]; pair_opt(&kv, 2, &kh, &kw, fn, "kernel"); KcValue *sv = nnopts_val(&o, "stride"); if (sv) pair_opt(sv, 1, &sh, &sw, fn, "stride"); else { sh = kh; sw = kw; } pair_opt(nnopts_val(&o, "padding"), 0, &ph, &pw, fn, "padding"); nnopts_done(&o);
    if (dims == 1) { kw = kh; kh = 1; sw = sh; sh = 1; pw = ph; ph = 0; } double hp[8] = { (double)avg, (double)dims, (double)kh, (double)kw, (double)sh, (double)sw, (double)ph, (double)pw }; return mk_layer(NNL_POOL, hp, 8, "xavier", 0, NULL);
}
FN(f_maxpool2d) { return pool_api(a, n, pos, 0, 2); } FN(f_avgpool2d) { return pool_api(a, n, pos, 1, 2); } FN(f_maxpool1d) { return pool_api(a, n, pos, 0, 1); } FN(f_avgpool1d) { return pool_api(a, n, pos, 1, 1); }
FN(f_gap) { NNAPI(); need_n(n, 0, 0, "global_avg_pool"); return mk_layer(NNL_GAP, NULL, 0, "xavier", 0, NULL); }
static KcValue rnn_api(KcValue *a, int n, KcPos pos, int cell, const char *fn) {
    NN_POS = &pos; need_n(n, 2, 40, fn); int64_t F = arg_int(a, n, 0, fn), H = arg_int(a, n, 1, fn); NnOpts o; nnopts_parse(&o, a, n, 2, fn);
    bool seq = nnopts_bool(&o, "return_sequences", false); double fb = nnopts_num(&o, "forget_bias", 1.0); const char *init = nnopts_str(&o, "init", "xavier"); double ia = nnopts_num(&o, "init_arg", 0); const char *nm = nnopts_str(&o, "name", NULL); nnopts_done(&o);
    if (F < 1 || H < 1) NNERR("nn.%s: input and hidden sizes must be >= 1", fn); double hp[5] = { (double)cell, (double)F, (double)H, (double)seq, fb }; return mk_layer(NNL_RNN, hp, 5, init, ia, nm);
}
FN(f_rnn) { return rnn_api(a, n, pos, 0, "rnn"); } FN(f_lstm) { return rnn_api(a, n, pos, 1, "lstm"); } FN(f_gru) { return rnn_api(a, n, pos, 2, "gru"); }
FN(f_attention) {
    NNAPI(); need_n(n, 2, 40, "attention"); int64_t D = arg_int(a, n, 0, "attention"), H = arg_int(a, n, 1, "attention"); NnOpts o; nnopts_parse(&o, a, n, 2, "attention");
    bool causal = nnopts_bool(&o, "causal", false); const char *init = nnopts_str(&o, "init", "xavier"); double ia = nnopts_num(&o, "init_arg", 0); const char *nm = nnopts_str(&o, "name", NULL); nnopts_done(&o);
    double hp[3] = { (double)D, (double)H, (double)causal }; return mk_layer(NNL_ATTN, hp, 3, init, ia, nm);
}
FN(f_pos_encoding) { NNAPI(); need_n(n, 0, 0, "pos_encoding"); return mk_layer(NNL_POSENC, NULL, 0, "xavier", 0, NULL); }

/* ------------------------------------------------------------------ containers & builders */
static NnLayer *new_seq(void) { return nn_layer_new(NNL_SEQ, NULL, 0, NNI_NONE, 0); }
static void fill_seq(NnLayer *s, KcValue v, const char *fn) {
    if (v.type != VAL_ARRAY) NNERR("nn.%s: expected an array of layers", fn);
    for (int i = 0; i < v.as.arr->count; i++) { NnLayer *k = nnh_get(v.as.arr->items[i], NNH_LAYER); if (!k) NNERR("nn.%s: item %d is not a layer", fn, i + 1); nn_container_append(s, k); }
}
FN(f_sequential) { NNAPI(); need_n(n, 0, 1, "sequential"); NnLayer *s = new_seq(); if (n) fill_seq(s, a[0], "sequential"); return nnh_wrap(NNH_LAYER, s); }
FN(f_residual) {
    NNAPI(); need_n(n, 1, 1, "residual"); NnLayer *inner = new_seq(); fill_seq(inner, a[0], "residual"); NnLayer *r = nn_layer_new(NNL_RESIDUAL, NULL, 0, NNI_NONE, 0); nn_container_append(r, inner); return nnh_wrap(NNH_LAYER, r);
}
FN(f_append) {
    NNAPI(); need_n(n, 2, 2, "append"); NnLayer *m = lay(a, n, 0, "append"); NnLayer *k = lay(a, n, 1, "append"); if (m->kind != NNL_SEQ) NNERR("nn.append: the first argument must be a sequential model");
    if (k == m) NNERR("nn.append: a model cannot contain itself"); nn_container_append(m, k); return a[0];
}
FN(f_layers) { NNAPI(); need_n(n, 1, 1, "layers"); NnLayer *m = lay(a, n, 0, "layers"); KcValue r = kc_array_new(); for (int i = 0; i < m->nkids; i++) kc_array_push(r, nnh_wrap(NNH_LAYER, m->kids[i])); return r; }
FN(f_mlp) {
    NNAPI(); need_n(n, 1, 40, "mlp"); if (a[0].type != VAL_ARRAY || a[0].as.arr->count < 2) NNERR("nn.mlp: first argument must be an array of at least two layer sizes, e.g. [784, 128, 10]");
    int k = a[0].as.arr->count; int64_t *sz = nn_malloc(sizeof(int64_t) * (size_t)k); for (int i = 0; i < k; i++) { sz[i] = arg_int(a[0].as.arr->items, k, i, "mlp"); if (sz[i] < 1) NNERR("nn.mlp: layer sizes must be >= 1"); }
    NnOpts o; nnopts_parse(&o, a, n, 1, "mlp"); const char *act = nnopts_str(&o, "activation", "relu"), *oact = nnopts_str(&o, "out_activation", "none"); double dr = nnopts_num(&o, "dropout", 0); bool bn = nnopts_bool(&o, "batchnorm", false); bool ln = nnopts_bool(&o, "layernorm", false);
    const char *init = nnopts_str(&o, "init", NULL); nnopts_done(&o); int aid = nn_act_from_name(act), oid = nn_act_from_name(oact); if (aid < 0) NNERR("nn.mlp: unknown activation '%s'", act); if (oid < 0) NNERR("nn.mlp: unknown out_activation '%s'", oact);
    if (!(dr >= 0 && dr < 1)) NNERR("nn.mlp: dropout must be in [0, 1)"); if (!init) init = (aid == NNA_RELU || aid == NNA_RELU6 || aid == NNA_LEAKY || aid == NNA_ELU || aid == NNA_GELU || aid == NNA_SILU || aid == NNA_MISH) ? "he" : (aid == NNA_SELU ? "lecun" : "xavier");
    NnLayer *s = new_seq();
    for (int i = 0; i + 1 < k; i++) {
        double hp[3] = { (double)sz[i], (double)sz[i + 1], 1 }; int iid = nn_init_from_name(init); if (iid < 0 || iid == NNI_NONE) NNERR("nn.mlp: unknown initializer '%s'", init);
        nn_container_append(s, nn_layer_new(NNL_DENSE, hp, 3, (i + 2 == k) ? NNI_XAVIER_U : iid, 0));
        if (i + 2 < k) {
            if (bn) { double b[4] = { (double)sz[i + 1], 0.1, 1e-5, 1 }; nn_container_append(s, nn_layer_new(NNL_BATCHNORM, b, 4, NNI_NONE, 0)); }
            if (ln) { double l[3] = { (double)sz[i + 1], 1e-5, 1 }; nn_container_append(s, nn_layer_new(NNL_LAYERNORM, l, 3, NNI_NONE, 0)); }
            double h2[2] = { (double)aid, aid == NNA_LEAKY ? 0.01 : 1.0 }; nn_container_append(s, nn_layer_new(NNL_ACT, h2, 2, NNI_NONE, 0));
            if (dr > 0) { double d[1] = { dr }; nn_container_append(s, nn_layer_new(NNL_DROPOUT, d, 1, NNI_NONE, 0)); }
        } else if (oid != NNA_IDENTITY) { double h2[2] = { (double)oid, 1.0 }; nn_container_append(s, nn_layer_new(NNL_ACT, h2, 2, NNI_NONE, 0)); }
    }
    free(sz); return nnh_wrap(NNH_LAYER, s);
}
FN(f_transformer_block) {
    NNAPI(); need_n(n, 3, 40, "transformer_block"); int64_t D = arg_int(a, n, 0, "transformer_block"), H = arg_int(a, n, 1, "transformer_block"), FF = arg_int(a, n, 2, "transformer_block");
    NnOpts o; nnopts_parse(&o, a, n, 3, "transformer_block"); double dr = nnopts_num(&o, "dropout", 0); bool causal = nnopts_bool(&o, "causal", false); nnopts_done(&o);
    if (D < 1 || H < 1 || FF < 1) NNERR("nn.transformer_block: sizes must be >= 1"); if (D % H) NNERR("nn.transformer_block: d_model (%lld) must be divisible by heads (%lld)", (long long)D, (long long)H);
    if (!(dr >= 0 && dr < 1)) NNERR("nn.transformer_block: dropout must be in [0, 1)"); NnLayer *blk = new_seq();
    NnLayer *s1 = new_seq(); double ln[3] = { (double)D, 1e-5, 1 }, at[3] = { (double)D, (double)H, (double)causal }; nn_container_append(s1, nn_layer_new(NNL_LAYERNORM, ln, 3, NNI_NONE, 0)); nn_container_append(s1, nn_layer_new(NNL_ATTN, at, 3, NNI_XAVIER_U, 0));
    if (dr > 0) { double d[1] = { dr }; nn_container_append(s1, nn_layer_new(NNL_DROPOUT, d, 1, NNI_NONE, 0)); }
    NnLayer *r1 = nn_layer_new(NNL_RESIDUAL, NULL, 0, NNI_NONE, 0); nn_container_append(r1, s1); nn_container_append(blk, r1);
    NnLayer *s2 = new_seq(); double d1[3] = { (double)D, (double)FF, 1 }, d2[3] = { (double)FF, (double)D, 1 }, ge[2] = { NNA_GELU, 0 };
    nn_container_append(s2, nn_layer_new(NNL_LAYERNORM, ln, 3, NNI_NONE, 0)); nn_container_append(s2, nn_layer_new(NNL_DENSE, d1, 3, NNI_XAVIER_U, 0)); nn_container_append(s2, nn_layer_new(NNL_ACT, ge, 2, NNI_NONE, 0)); nn_container_append(s2, nn_layer_new(NNL_DENSE, d2, 3, NNI_XAVIER_U, 0));
    if (dr > 0) { double d[1] = { dr }; nn_container_append(s2, nn_layer_new(NNL_DROPOUT, d, 1, NNI_NONE, 0)); }
    NnLayer *r2 = nn_layer_new(NNL_RESIDUAL, NULL, 0, NNI_NONE, 0); nn_container_append(r2, s2); nn_container_append(blk, r2); return nnh_wrap(NNH_LAYER, blk);
}

/* ------------------------------------------------------------------ losses & optimizers */
static NnLoss *make_loss(const char *name, NnOpts *o) {
    int k = nn_loss_from_name(name); if (k < 0) NNERR("nn: unknown loss '%s' (mse, mae, huber, logcosh, bce, bce_logits, ce, nll, kl, hinge)", name);
    NnLoss *l = nn_loss_new(k); if (o) { l->delta = nnopts_num(o, "delta", 1.0); l->smoothing = nnopts_num(o, "smoothing", 0.0); l->pos_weight = nnopts_num(o, "pos_weight", 1.0);
        if (nnopts_has(o, "ignore_index")) { l->has_ignore = true; l->ignore_index = nnopts_int(o, "ignore_index", -1); } if (l->smoothing < 0 || l->smoothing >= 1) NNERR("nn: smoothing must be in [0, 1)"); if (l->delta <= 0) NNERR("nn: delta must be > 0"); }
    return l;
}
FN(f_loss) { NNAPI(); need_n(n, 1, 40, "loss"); const char *nm = arg_str(a, n, 0, "loss"); NnOpts o; nnopts_parse(&o, a, n, 1, "loss"); NnLoss *l = make_loss(nm, &o); nnopts_done(&o); return nnh_wrap(NNH_LOSS, l); }
static NnLoss *loss_arg(KcValue *a, int n, int i, const char *fn, bool *temp) {
    *temp = false; if (i < n && a[i].type == VAL_STRING) { *temp = true; return make_loss(a[i].as.s->data, NULL); } return nnh_need(a, n, i, NNH_LOSS, fn);
}
static void opt_config(NnOpt *o, NnOpts *op, double lr_default_is_set, bool lr_given, double lr) {
    (void)lr_default_is_set; if (lr_given) o->lr = lr; o->lr = nnopts_num(op, "lr", o->lr); o->momentum = nnopts_num(op, "momentum", o->momentum); o->nesterov = nnopts_bool(op, "nesterov", o->nesterov);
    o->wd = nnopts_num(op, "weight_decay", nnopts_num(op, "wd", o->wd)); o->beta1 = nnopts_num(op, "beta1", o->beta1); o->beta2 = nnopts_num(op, "beta2", o->beta2); o->eps = nnopts_num(op, "eps", o->eps);
    o->amsgrad = nnopts_bool(op, "amsgrad", o->amsgrad); o->alpha = nnopts_num(op, "alpha", o->alpha); o->centered = nnopts_bool(op, "centered", o->centered); o->decay_all = nnopts_bool(op, "decay_all", o->decay_all);
    if (o->lr <= 0) NNERR("nn: learning rate must be > 0"); if (o->nesterov && o->momentum <= 0) NNERR("nn: nesterov needs momentum > 0"); if (o->wd < 0) NNERR("nn: weight_decay must be >= 0");
    if (o->beta1 < 0 || o->beta1 >= 1 || o->beta2 < 0 || o->beta2 >= 1) NNERR("nn: beta1/beta2 must be in [0, 1)"); o->base_lr = o->lr;
}
static KcValue opt_api(KcValue *a, int n, KcPos pos, int kind, const char *fn) {
    NN_POS = &pos; need_n(n, 1, 40, fn); NnLayer *m = seq_arg(a, n, 0, fn); int start = 1; bool lg = false; double lr = 0; if (n > 1 && (a[1].type == VAL_INT || a[1].type == VAL_FLOAT)) { lr = arg_num(a, n, 1, fn); lg = true; start = 2; }
    NnOpts o; nnopts_parse(&o, a, n, start, fn); NnOpt *op = nn_opt_new(kind, m); opt_config(op, &o, 0, lg, lr); nnopts_done(&o); return nnh_wrap(NNH_OPT, op);
}
FN(f_sgd) { return opt_api(a, n, pos, NNO_SGD, "sgd"); } FN(f_adam) { return opt_api(a, n, pos, NNO_ADAM, "adam"); } FN(f_adamw) { return opt_api(a, n, pos, NNO_ADAMW, "adamw"); }
FN(f_rmsprop) { return opt_api(a, n, pos, NNO_RMSPROP, "rmsprop"); } FN(f_adagrad) { return opt_api(a, n, pos, NNO_ADAGRAD, "adagrad"); }
FN(f_set_lr) { NNAPI(); need_n(n, 2, 2, "set_lr"); NnOpt *o = nnh_need(a, n, 0, NNH_OPT, "set_lr"); double lr = arg_num(a, n, 1, "set_lr"); if (!(lr > 0)) NNERR("nn.set_lr: learning rate must be > 0"); o->lr = o->base_lr = lr; return kc_null(); }
FN(f_get_lr) { NNAPI(); need_n(n, 1, 1, "get_lr"); NnOpt *o = nnh_need(a, n, 0, NNH_OPT, "get_lr"); return kc_float(o->lr); }
FN(f_lr_at) {
    NNAPI(); need_n(n, 4, 5, "lr_at"); const char *sp = arg_str(a, n, 0, "lr_at"); char err[200]; if (!nn_sched_valid(sp, err, sizeof err)) NNERR("nn.lr_at: %s", err);
    return kc_float(nn_sched_lr(sp, arg_num(a, n, 1, "lr_at"), (int)arg_int(a, n, 2, "lr_at"), (int)arg_int(a, n, 3, "lr_at"), n > 4 ? (int)arg_int(a, n, 4, "lr_at") : 0));
}
FN(f_clip_grad_norm) { NNAPI(); need_n(n, 2, 2, "clip_grad_norm"); NnLayer *m = lay(a, n, 0, "clip_grad_norm"); double mx = arg_num(a, n, 1, "clip_grad_norm"); if (!(mx > 0)) NNERR("nn.clip_grad_norm: max norm must be > 0"); return kc_float(nn_clip_grad_norm(m, mx)); }
FN(f_clip_grad_value) { NNAPI(); need_n(n, 2, 2, "clip_grad_value"); NnLayer *m = lay(a, n, 0, "clip_grad_value"); double v = arg_num(a, n, 1, "clip_grad_value"); if (!(v > 0)) NNERR("nn.clip_grad_value: value must be > 0"); nn_clip_grad_value(m, v); return kc_null(); }

/* ------------------------------------------------------------------ low-level training steps */
FN(f_forward) {
    NNAPI(); need_n(n, 2, 3, "forward"); NnLayer *m = seq_arg(a, n, 0, "forward"); TArg x; NnTensor *t = targ_get(&x, a, n, 1, "forward"); bool train = n > 2 ? kc_truthy(a[2]) : true;
    nn_forward(m, t, train); NnTensor *r = nn_malloc(sizeof *r); nt_init(r); nt_copy(r, &m->out); targ_free(&x); return nn_ret_tensor(r);
}
FN(f_loss_value) {
    NNAPI(); need_n(n, 3, 3, "loss_value"); bool tmp; NnLoss *l = loss_arg(a, n, 0, "loss_value", &tmp); TArg p, y; NnTensor *pr = targ_get(&p, a, n, 1, "loss_value"), *tg = targ_get(&y, a, n, 2, "loss_value");
    double v = nn_loss_eval(l, pr, tg, true); targ_free(&p); targ_free(&y); if (tmp) nn_loss_free(l); return kc_float(v);
}
FN(f_backward) {
    NNAPI(); need_n(n, 2, 3, "backward"); NnLayer *m = seq_arg(a, n, 0, "backward"); NnLoss *l = nnh_need(a, n, 1, NNH_LOSS, "backward"); bool ig = n > 2 && kc_truthy(a[2]);
    if (!m->has_forward || l->grad.n != m->out.n) NNERR("nn.backward: run nn.forward and then nn.loss_value on that output first (loss gradient has %zu values, model output has %zu)", l->grad.n, m->out.n);
    NnTensor *dx = ig ? nn_malloc(sizeof *dx) : NULL; if (dx) nt_init(dx); nn_backward_loss(m, l, ig, dx); return ig ? nn_ret_tensor(dx) : kc_null();
}
FN(f_zero_grad) { NNAPI(); need_n(n, 1, 1, "zero_grad"); nn_zero_grad(lay(a, n, 0, "zero_grad")); return kc_null(); }
FN(f_step) { NNAPI(); need_n(n, 1, 1, "step"); nn_opt_step(nnh_need(a, n, 0, NNH_OPT, "step")); return kc_null(); }
FN(f_train_step) {
    NNAPI(); need_n(n, 5, 5, "train_step"); NnLayer *m = seq_arg(a, n, 0, "train_step"); bool tmp; NnLoss *l = loss_arg(a, n, 1, "train_step", &tmp); NnOpt *o = nnh_need(a, n, 2, NNH_OPT, "train_step");
    TArg x, y; NnTensor *xt = targ_get(&x, a, n, 3, "train_step"), *yt = targ_get(&y, a, n, 4, "train_step"); double v = nn_train_step(m, l, o, xt, yt); targ_free(&x); targ_free(&y); if (tmp) nn_loss_free(l); return kc_float(v);
}

/* ------------------------------------------------------------------ fit / evaluate / predict */
typedef struct { KcInterp *ip; KcValue fn; KcPos pos; } CbCtx;
static int cb_tramp(void *ud, int epoch, double loss, double vloss, double metric) {
    CbCtx *c = ud; KcValue args[4] = { kc_int(epoch), kc_float(loss), isnan(vloss) ? kc_null() : kc_float(vloss), kc_float(metric) };
    KcValue r = kc_call_value(c->ip, c->fn, args, 4, c->pos); return kc_truthy(r);
}
static int default_metric(int loss_kind) { return (loss_kind == NNLS_CE || loss_kind == NNLS_NLL || loss_kind == NNLS_BCE || loss_kind == NNLS_BCE_LOGITS || loss_kind == NNLS_HINGE) ? NNM_ACC : NNM_NONE; }
static void parse_metric(NnOpts *o, int loss_kind, int *kind, int *k, const char *fn) {
    const char *nm = nnopts_str(o, "metric", NULL); *k = 0; if (!nm) { *kind = default_metric(loss_kind); return; } if (!nn_metric_parse(nm, kind, k)) NNERR("nn.%s: unknown metric '%s' (accuracy, top_k:N, mae, mse, rmse, r2, none)", fn, nm);
}
static KcValue hist_array(const double *v, int n, bool nan_null) { KcValue r = kc_array_new(); for (int i = 0; i < n; i++) kc_array_push(r, (nan_null && isnan(v[i])) ? kc_null() : kc_float(v[i])); return r; }
FN(f_fit) {
    NNAPI(); need_n(n, 5, 60, "fit"); NnLayer *m = seq_arg(a, n, 0, "fit"); bool ltmp; NnLoss *loss = loss_arg(a, n, 1, "fit", &ltmp);
    TArg X, Y; NnTensor *xt = targ_get(&X, a, n, 3, "fit"), *yt = targ_get(&Y, a, n, 4, "fit"); NnOpts o; nnopts_parse(&o, a, n, 5, "fit");
    NnOpt *opt; bool otmp = false; if (a[2].type == VAL_STRING) { int ok = nn_opt_from_name(a[2].as.s->data); if (ok < 0) NNERR("nn.fit: unknown optimizer '%s' (sgd, adam, adamw, rmsprop, adagrad)", a[2].as.s->data); opt = nn_opt_new(ok, m); otmp = true; opt_config(opt, &o, 0, false, 0); }
    else opt = nnh_need(a, n, 2, NNH_OPT, "fit");
    if (opt->model != m) NNERR("nn.fit: this optimizer was created for a different model");
    NnFitCfg c; memset(&c, 0, sizeof c); c.epochs = (int)nnopts_int(&o, "epochs", 10); c.batch_size = (int)nnopts_int(&o, "batch_size", 32); c.shuffle = nnopts_bool(&o, "shuffle", true); c.val_split = nnopts_num(&o, "val_split", 0);
    c.verbose = (int)nnopts_int(&o, "verbose", 1); c.clip_norm = nnopts_num(&o, "clip_norm", 0); c.clip_value = nnopts_num(&o, "clip_value", 0); c.patience = (int)nnopts_int(&o, "patience", 0); c.min_delta = nnopts_num(&o, "min_delta", 0);
    c.restore_best = nnopts_bool(&o, "restore_best", c.patience > 0); c.warmup = (int)nnopts_int(&o, "warmup", 0); c.accum = (int)nnopts_int(&o, "accum", 1); c.drop_last = nnopts_bool(&o, "drop_last", false); c.val_every = (int)nnopts_int(&o, "val_every", 1);
    c.schedule = nnopts_str(&o, "schedule", "const"); parse_metric(&o, loss->kind, &c.metric, &c.metric_k, "fit"); KcValue *cbv = nnopts_val(&o, "callback"); KcValue *valv = nnopts_val(&o, "val"); if (nnopts_has(&o, "seed")) nn_seed((uint64_t)nnopts_int(&o, "seed", 0));
    nnopts_done(&o); char err[200]; if (!nn_sched_valid(c.schedule, err, sizeof err)) NNERR("nn.fit: %s", err);
    if (c.warmup < 0 || c.accum < 1 || c.verbose < 0 || c.patience < 0 || c.val_every < 1) NNERR("nn.fit: warmup, patience, verbose must be >= 0 and accum, val_every >= 1");
    TArg VX, VY; memset(&VX, 0, sizeof VX); memset(&VY, 0, sizeof VY); bool hasval = false;
    if (valv) { if (valv->type != VAL_ARRAY || valv->as.arr->count != 2) NNERR("nn.fit: option 'val' must be an array [X_val, Y_val]"); c.vx = targ_get(&VX, valv->as.arr->items, 2, 0, "fit"); c.vy = targ_get(&VY, valv->as.arr->items, 2, 1, "fit"); hasval = true; if (c.val_split > 0) NNERR("nn.fit: use either 'val' or 'val_split', not both"); }
    CbCtx cb; if (cbv) { cb.ip = ip; cb.fn = *cbv; cb.pos = pos; c.callback = cb_tramp; c.ud = &cb; }
    NnHistory h; nn_fit(m, loss, opt, xt, yt, &c, &h);
    const char *nm[] = { "epochs", "loss", "val_loss", "metric", "val_metric", "lr", "best_epoch", "best_loss", "stopped_early", "seconds", "metric_name" };
    KcValue v[] = { kc_int(h.epochs_run), hist_array(h.loss, h.epochs_run, false), hist_array(h.val_loss, h.epochs_run, true), hist_array(h.metric, h.epochs_run, false), hist_array(h.val_metric, h.epochs_run, true), hist_array(h.lr, h.epochs_run, false),
                    kc_int(h.best_epoch + 1), kc_float(h.best_val), kc_bool(h.stopped_early), kc_float(h.seconds), kc_string(c.metric == NNM_NONE ? "none" : nn_metric_name(c.metric, c.metric_k)) };
    nn_history_free(&h); targ_free(&X); targ_free(&Y); if (hasval) { targ_free(&VX); targ_free(&VY); } if (ltmp) nn_loss_free(loss); if (otmp) nn_opt_free(opt); return mk_struct("History", 11, nm, v);
}
FN(f_evaluate) {
    NNAPI(); need_n(n, 4, 40, "evaluate"); NnLayer *m = seq_arg(a, n, 0, "evaluate"); bool tmp; NnLoss *l = loss_arg(a, n, 1, "evaluate", &tmp); TArg X, Y; NnTensor *xt = targ_get(&X, a, n, 2, "evaluate"), *yt = targ_get(&Y, a, n, 3, "evaluate");
    NnOpts o; nnopts_parse(&o, a, n, 4, "evaluate"); int mk, mkk; parse_metric(&o, l->kind, &mk, &mkk, "evaluate"); int bs = (int)nnopts_int(&o, "batch_size", 256); nnopts_done(&o);
    double met = 0; double lv = nn_evaluate(m, l, xt, yt, bs, mk, mkk, &met); const char *nm[] = { "loss", "metric", "metric_name" }; KcValue v[] = { kc_float(lv), mk == NNM_NONE ? kc_null() : kc_float(met), kc_string(mk == NNM_NONE ? "none" : nn_metric_name(mk, mkk)) };
    targ_free(&X); targ_free(&Y); if (tmp) nn_loss_free(l); return mk_struct("Evaluation", 3, nm, v);
}
static int batch_opt(KcValue *a, int n, int start, const char *fn, double *thr) {
    NnOpts o; nnopts_parse(&o, a, n, start, fn); int bs = (int)nnopts_int(&o, "batch_size", 256); if (thr) *thr = nnopts_num(&o, "threshold", 0.5); nnopts_done(&o); if (bs < 1) NNERR("nn.%s: batch_size must be >= 1", fn); return bs;
}
FN(f_predict) {
    NNAPI(); need_n(n, 2, 40, "predict"); NnLayer *m = seq_arg(a, n, 0, "predict"); TArg X; NnTensor *xt = targ_get(&X, a, n, 1, "predict"); int bs = batch_opt(a, n, 2, "predict", NULL);
    if (xt->nd < 1) NNERR("nn.predict: input needs a batch dimension"); NnTensor out; nt_init(&out); nn_predict(m, xt, bs, &out); KcValue r = nn_to_nested(&out); nt_free(&out); targ_free(&X); return r;
}
static KcValue nested_ints(const int64_t *v, const int *shape, int nd, size_t *off, int level) {
    if (level == nd) return kc_int(v[(*off)++]); KcValue r = kc_array_new(); for (int i = 0; i < shape[level]; i++) kc_array_push(r, nested_ints(v, shape, nd, off, level + 1)); return r;
}
FN(f_predict_class) {
    NNAPI(); need_n(n, 2, 40, "predict_class"); NnLayer *m = seq_arg(a, n, 0, "predict_class"); TArg X; NnTensor *xt = targ_get(&X, a, n, 1, "predict_class"); double thr; int bs = batch_opt(a, n, 2, "predict_class", &thr);
    NnTensor out; nt_init(&out); nn_predict(m, xt, bs, &out); size_t C = out.nd ? (size_t)out.shape[out.nd - 1] : 1; bool multi = out.nd >= 2 && C > 1; int nd = (multi || (out.nd >= 2 && C == 1)) ? out.nd - 1 : out.nd;
    size_t rows = multi ? out.n / C : out.n; int64_t *cls = nn_malloc(sizeof(int64_t) * (rows ? rows : 1));
    for (size_t r = 0; r < rows; r++) { if (multi) { size_t am = 0; for (size_t c = 1; c < C; c++) if (out.d[r * C + c] > out.d[r * C + am]) am = c; cls[r] = (int64_t)am; } else cls[r] = out.d[r] >= thr; }
    size_t off = 0; KcValue res = nested_ints(cls, out.shape, nd, &off, 0); free(cls); nt_free(&out); targ_free(&X); return res;
}
FN(f_gradcheck) {
    NNAPI(); need_n(n, 4, 40, "gradcheck"); NnLayer *m = seq_arg(a, n, 0, "gradcheck"); bool tmp; NnLoss *l = loss_arg(a, n, 1, "gradcheck", &tmp); TArg X, Y; NnTensor *xt = targ_get(&X, a, n, 2, "gradcheck"), *yt = targ_get(&Y, a, n, 3, "gradcheck");
    NnOpts o; nnopts_parse(&o, a, n, 4, "gradcheck"); double eps = nnopts_num(&o, "eps", 1e-6); int samples = (int)nnopts_int(&o, "samples", 40); nnopts_done(&o); if (!(eps > 0)) NNERR("nn.gradcheck: eps must be > 0");
    double mr; long cnt; char worst[200]; bool ok = nn_gradcheck(m, l, xt, yt, eps, samples, &mr, &cnt, worst, sizeof worst); const char *nm[] = { "ok", "max_rel_error", "checked", "worst" }; KcValue v[] = { kc_bool(ok), kc_float(mr), kc_int(cnt), kc_string(worst) };
    targ_free(&X); targ_free(&Y); if (tmp) nn_loss_free(l); return mk_struct("GradCheck", 4, nm, v);
}

/* ------------------------------------------------------------------ introspection & parameters */
FN(f_summary) {
    NNAPI(); need_n(n, 2, 2, "summary"); NnLayer *m = seq_arg(a, n, 0, "summary"); int sh[NN_MAXDIM]; int nd = arg_shape(a, n, 1, sh, "summary"); if (nd >= NN_MAXDIM) NNERR("nn.summary: input shape has too many dimensions");
    int full[NN_MAXDIM]; full[0] = 1; for (int i = 0; i < nd; i++) { if (sh[i] < 1) NNERR("nn.summary: input shape entries must be >= 1 (give the per-sample shape, without the batch dimension)"); full[1 + i] = sh[i]; }
    char *s = nn_summary(m, nd + 1, full); KcValue r = kc_string(s); free(s); return r;
}
FN(f_count_params) { NNAPI(); need_n(n, 1, 2, "count_params"); NnLayer *m = lay(a, n, 0, "count_params"); bool tr = n > 1 && kc_truthy(a[1]); return kc_int(nn_count_params(m, tr)); }
static void names_rec(NnLayer *L, const char *path, KcValue arr) {
    for (int i = 0; i < L->nparams; i++) { char b[160]; snprintf(b, sizeof b, "%s%s%s.%s", path, path[0] ? "." : "", L->name, L->params[i].name); kc_array_push(arr, kc_string(b)); }
    for (int i = 0; i < L->nkids; i++) { char p[120]; snprintf(p, sizeof p, "%s%s%d", path, path[0] ? "." : "", i); names_rec(L->kids[i], p, arr); }
}
FN(f_param_names) { NNAPI(); need_n(n, 1, 1, "param_names"); NnLayer *m = lay(a, n, 0, "param_names"); KcValue r = kc_array_new(); names_rec(m, "", r); return r; }
FN(f_get_params) {
    NNAPI(); need_n(n, 1, 1, "get_params"); NnLayer *m = lay(a, n, 0, "get_params"); NnParam **ps; int np = nn_collect_params(m, &ps); KcValue r = kc_array_new();
    for (int i = 0; i < np; i++) { NnTensor *t = nn_malloc(sizeof *t); nt_init(t); nt_copy(t, &ps[i]->w); kc_array_push(r, nn_ret_tensor(t)); } free(ps); return r;
}
FN(f_set_params) {
    NNAPI(); need_n(n, 2, 2, "set_params"); NnLayer *m = lay(a, n, 0, "set_params"); if (a[1].type != VAL_ARRAY) NNERR("nn.set_params: second argument must be an array of tensors (as returned by nn.get_params)");
    NnParam **ps; int np = nn_collect_params(m, &ps); if (a[1].as.arr->count != np) { free(ps); NNERR("nn.set_params: model has %d parameter tensors but %d were given", np, a[1].as.arr->count); }
    NnTensor *stage = nn_calloc((size_t)(np ? np : 1), sizeof(NnTensor)); for (int i = 0; i < np; i++) { nt_init(&stage[i]); if (!nn_value_to_tensor(a[1].as.arr->items[i], &stage[i], "set_params")) NNERR("nn.set_params: item %d is not a tensor", i + 1);
        if (!nt_same_shape(&stage[i], &ps[i]->w)) NNERR("nn.set_params: item %d has shape %s but the parameter is %s", i + 1, nt_shape_str(stage[i].nd, stage[i].shape), nt_shape_str(ps[i]->w.nd, ps[i]->w.shape)); }
    for (int i = 0; i < np; i++) { nt_copy(&ps[i]->w, &stage[i]); nt_free(&stage[i]); } free(stage); free(ps); return kc_null();
}
static KcValue freeze_api(KcValue *a, int n, KcPos pos, bool fr, const char *fn) {
    NN_POS = &pos; need_n(n, 1, 2, fn); NnLayer *m = nnh_need(a, n, 0, NNH_LAYER, fn);
    if (n == 1) nn_set_frozen(m, fr); else { int64_t i = arg_int(a, n, 1, fn); if (i < 0) i += m->nkids; if (i < 0 || i >= m->nkids) NNERR("nn.%s: layer index out of range (model has %d layers)", fn, m->nkids); nn_set_frozen(m->kids[i], fr); }
    return kc_null();
}
FN(f_freeze) { return freeze_api(a, n, pos, true, "freeze"); } FN(f_unfreeze) { return freeze_api(a, n, pos, false, "unfreeze"); }
FN(f_seed) { NNAPI(); need_n(n, 1, 1, "seed"); nn_seed((uint64_t)arg_int(a, n, 0, "seed")); return kc_null(); }
FN(f_version) { NNAPI(); (void)a; (void)n; return kc_string(NN_VERSION_STR); }

/* ------------------------------------------------------------------ persistence & cloning */
FN(f_clone) { NNAPI(); need_n(n, 1, 1, "clone"); NnLayer *m = seq_arg(a, n, 0, "clone"); return nnh_wrap(NNH_LAYER, nn_clone(m)); }
FN(f_copy_weights) {
    NNAPI(); need_n(n, 2, 3, "copy_weights"); NnLayer *d = lay(a, n, 0, "copy_weights"), *s = lay(a, n, 1, "copy_weights"); double tau = n > 2 ? arg_num(a, n, 2, "copy_weights") : 1.0;
    if (!(tau > 0 && tau <= 1)) NNERR("nn.copy_weights: tau must be in (0, 1]"); nn_copy_weights(d, s, tau); return kc_null();
}
FN(f_save) { NNAPI(); need_n(n, 2, 2, "save"); NnLayer *m = seq_arg(a, n, 0, "save"); const char *p = arg_str(a, n, 1, "save"); char err[300]; if (!nn_save_file(m, p, err, sizeof err)) NNERR("nn.save: %s", err); return kc_null(); }
FN(f_load) { NNAPI(); need_n(n, 1, 1, "load"); const char *p = arg_str(a, n, 0, "load"); char err[300]; NnLayer *m = nn_load_file(p, err, sizeof err); if (!m) NNERR("nn.load: %s", err); return nnh_wrap(NNH_LAYER, m); }
FN(f_save_weights) { NNAPI(); need_n(n, 2, 2, "save_weights"); NnLayer *m = lay(a, n, 0, "save_weights"); const char *p = arg_str(a, n, 1, "save_weights"); char err[300]; if (!nn_save_weights(m, p, err, sizeof err)) NNERR("nn.save_weights: %s", err); return kc_null(); }
FN(f_load_weights) { NNAPI(); need_n(n, 2, 2, "load_weights"); NnLayer *m = lay(a, n, 0, "load_weights"); const char *p = arg_str(a, n, 1, "load_weights"); char err[300]; if (!nn_load_weights(m, p, err, sizeof err)) NNERR("nn.load_weights: %s", err); return kc_null(); }

#define E(name, fn) { name, fn }
const NnEntry nn_model_entries[] = {
    E("dense", f_dense), E("relu", f_relu), E("relu6", f_relu6), E("leaky_relu", f_leaky_relu), E("elu", f_elu), E("selu", f_selu), E("gelu", f_gelu), E("sigmoid", f_sigmoid), E("tanh", f_tanh), E("softplus", f_softplus),
    E("silu", f_silu), E("mish", f_mish), E("softsign", f_softsign), E("hard_sigmoid", f_hard_sigmoid), E("softmax", f_softmax), E("log_softmax", f_log_softmax), E("activation", f_activation), E("apply", f_apply),
    E("dropout", f_dropout), E("flatten", f_flatten), E("reshape_layer", f_reshape_layer), E("embedding", f_embedding), E("batchnorm", f_batchnorm), E("layernorm", f_layernorm),
    E("conv1d", f_conv1d), E("conv2d", f_conv2d), E("maxpool1d", f_maxpool1d), E("maxpool2d", f_maxpool2d), E("avgpool1d", f_avgpool1d), E("avgpool2d", f_avgpool2d), E("global_avg_pool", f_gap),
    E("rnn", f_rnn), E("lstm", f_lstm), E("gru", f_gru), E("attention", f_attention), E("pos_encoding", f_pos_encoding),
    E("sequential", f_sequential), E("residual", f_residual), E("append", f_append), E("layers", f_layers), E("mlp", f_mlp), E("transformer_block", f_transformer_block),
    E("loss", f_loss), E("sgd", f_sgd), E("adam", f_adam), E("adamw", f_adamw), E("rmsprop", f_rmsprop), E("adagrad", f_adagrad), E("set_lr", f_set_lr), E("get_lr", f_get_lr), E("lr_at", f_lr_at),
    E("clip_grad_norm", f_clip_grad_norm), E("clip_grad_value", f_clip_grad_value),
    E("forward", f_forward), E("loss_value", f_loss_value), E("backward", f_backward), E("zero_grad", f_zero_grad), E("step", f_step), E("train_step", f_train_step),
    E("fit", f_fit), E("evaluate", f_evaluate), E("predict", f_predict), E("predict_class", f_predict_class), E("gradcheck", f_gradcheck),
    E("summary", f_summary), E("count_params", f_count_params), E("param_names", f_param_names), E("get_params", f_get_params), E("set_params", f_set_params), E("freeze", f_freeze), E("unfreeze", f_unfreeze),
    E("seed", f_seed), E("version", f_version), E("clone", f_clone), E("copy_weights", f_copy_weights), E("save", f_save), E("load", f_load), E("save_weights", f_save_weights), E("load_weights", f_load_weights),
};
const int nn_model_count = (int)(sizeof nn_model_entries / sizeof nn_model_entries[0]);
