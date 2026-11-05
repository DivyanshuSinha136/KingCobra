/* std.nn training engine: metrics, fit, evaluate, predict */
#include "nn_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <time.h>

/* ------------------------------------------------------------------ metrics */
bool nn_metric_parse(const char *s, int *kind, int *k) {
    *k = 0;
    if (!strcmp(s, "none") || !*s) { *kind = NNM_NONE; return true; }
    if (!strcmp(s, "accuracy") || !strcmp(s, "acc")) { *kind = NNM_ACC; return true; }
    if (!strcmp(s, "mae")) { *kind = NNM_MAE; return true; } if (!strcmp(s, "mse")) { *kind = NNM_MSE; return true; }
    if (!strcmp(s, "rmse")) { *kind = NNM_RMSE; return true; } if (!strcmp(s, "r2")) { *kind = NNM_R2; return true; }
    if (!strncmp(s, "top_k:", 6) || !strncmp(s, "top", 3)) { const char *d = !strncmp(s, "top_k:", 6) ? s + 6 : s + 3; char *e; long v = strtol(d, &e, 10); if (e != d && *e == 0 && v >= 1 && v <= 1000) { *kind = NNM_TOPK; *k = (int)v; return true; } }
    return false;
}
const char *nn_metric_name(int kind, int k) {
    static char b[16]; switch (kind) { case NNM_ACC: return "acc"; case NNM_MAE: return "mae"; case NNM_MSE: return "mse"; case NNM_RMSE: return "rmse"; case NNM_R2: return "r2";
    case NNM_TOPK: snprintf(b, sizeof b, "top%d", k); return b; default: return "metric"; }
}
void nn_metric_update(NnMetric *m, const NnTensor *p, const NnTensor *t) {
    if (m->kind == NNM_NONE || p->n == 0) return; size_t n = p->n;
    if (m->kind == NNM_ACC || m->kind == NNM_TOPK) {
        size_t C = p->nd ? (size_t)p->shape[p->nd - 1] : 1, R = C ? n / C : 0; bool idx = t->n == R, dist = t->n == n;
        if (!idx && !dist) nn_fail("metric: target shape %s does not match predictions %s", nt_shape_str(t->nd, t->shape), nt_shape_str(p->nd, p->shape));
        for (size_t r = 0; r < R; r++) {
            const nn_real *row = p->d + r * C; long cls;
            if (C == 1) { cls = idx ? (long)(t->d[r] >= 0.5) : (long)(t->d[r] >= 0.5); m->a += ((row[0] >= 0.5) == (cls != 0)); m->n++; continue; }
            if (idx) cls = (long)t->d[r]; else { cls = 0; for (size_t c = 1; c < C; c++) if (t->d[r * C + c] > t->d[r * C + (size_t)cls]) cls = (long)c; }
            if (cls < 0 || cls >= (long)C) { m->n++; continue; }
            if (m->kind == NNM_ACC) { size_t am = 0; for (size_t c = 1; c < C; c++) if (row[c] > row[am]) am = c; m->a += (long)am == cls; }
            else { size_t better = 0; for (size_t c = 0; c < C; c++) if (row[c] > row[cls]) better++; m->a += better < (size_t)m->k; }
            m->n++;
        }
        return;
    }
    if (t->n != n) nn_fail("metric '%s': target has %zu values but predictions have %zu", nn_metric_name(m->kind, m->k), t->n, n);
    for (size_t i = 0; i < n; i++) { double d = p->d[i] - t->d[i];
        if (m->kind == NNM_MAE) m->a += fabs(d); else if (m->kind == NNM_MSE || m->kind == NNM_RMSE) m->a += d * d; else { m->a += d * d; m->b += t->d[i]; m->c += t->d[i] * t->d[i]; } }
    m->n += (long)n;
}
double nn_metric_value(const NnMetric *m) {
    if (m->n == 0) return 0.0;
    switch (m->kind) { case NNM_ACC: case NNM_TOPK: case NNM_MAE: case NNM_MSE: return m->a / (double)m->n; case NNM_RMSE: return sqrt(m->a / (double)m->n);
    case NNM_R2: { double tot = m->c - m->b * m->b / (double)m->n; return tot > 1e-12 ? 1.0 - m->a / tot : 0.0; } default: return 0.0; }
}

/* ------------------------------------------------------------------ helpers */
static void gather(const NnTensor *src, const int64_t *idx, int n, NnTensor *dst) {
    int sh[NN_MAXDIM]; memcpy(sh, src->shape, sizeof(int) * (size_t)src->nd); sh[0] = n; nt_resize(dst, src->nd, sh);
    size_t row = src->shape[0] ? src->n / (size_t)src->shape[0] : 0;
    for (int i = 0; i < n; i++) memcpy(dst->d + (size_t)i * row, src->d + (size_t)idx[i] * row, sizeof(nn_real) * row);
}
static void slice_rows(const NnTensor *src, int64_t from, int n, NnTensor *dst) {
    int sh[NN_MAXDIM]; memcpy(sh, src->shape, sizeof(int) * (size_t)src->nd); sh[0] = n; nt_resize(dst, src->nd, sh);
    size_t row = src->shape[0] ? src->n / (size_t)src->shape[0] : 0; if (row != 0 && n != 0) memcpy(dst->d, src->d + (size_t)from * row, sizeof(nn_real) * row * (size_t)n);
}
void nn_backward_loss(NnLayer *m, NnLoss *loss, bool want_dx, NnTensor *dx_out) {
    m->need_dx = want_dx; nn_backward(m, &loss->grad); if (want_dx && dx_out) nt_copy(dx_out, &m->dx); m->need_dx = false;
}
double nn_train_step(NnLayer *m, NnLoss *loss, NnOpt *opt, const NnTensor *x, const NnTensor *y) {
    nn_zero_grad(m); nn_forward(m, x, 1); double l = nn_loss_eval(loss, &m->out, y, true);
    if (!isfinite(l)) nn_fail("loss is %s; training has diverged (lower the learning rate or clip gradients)", isnan(l) ? "NaN" : "infinite");
    nn_backward_loss(m, loss, false, NULL); nn_opt_step(opt); return l;
}
double nn_evaluate(NnLayer *m, NnLoss *loss, const NnTensor *X, const NnTensor *Y, int batch, int mk, int mkk, double *metric_out) {
    int N = X->shape[0]; if (Y->shape[0] != N) nn_fail("evaluate: X has %d samples but Y has %d", N, Y->shape[0]); if (batch < 1) batch = 256;
    NnTensor xb, yb; nt_init(&xb); nt_init(&yb); NnMetric me; memset(&me, 0, sizeof me); me.kind = mk; me.k = mkk; double sum = 0; long cnt = 0;
    for (int s = 0; s < N; s += batch) { int bs = N - s < batch ? N - s : batch; slice_rows(X, s, bs, &xb); slice_rows(Y, s, bs, &yb);
        nn_forward(m, &xb, 0); sum += nn_loss_eval(loss, &m->out, &yb, false) * bs; cnt += bs; nn_metric_update(&me, &m->out, &yb); }
    nt_free(&xb); nt_free(&yb); if (metric_out) *metric_out = nn_metric_value(&me); return cnt ? sum / (double)cnt : 0.0;
}
void nn_predict(NnLayer *m, const NnTensor *X, int batch, NnTensor *out) {
    int N = X->shape[0]; if (batch < 1) batch = 256; NnTensor xb, res; nt_init(&xb); nt_init(&res); size_t row = 0;
    for (int s = 0; s < N; s += batch) { int bs = N - s < batch ? N - s : batch; slice_rows(X, s, bs, &xb); nn_forward(m, &xb, 0);
        if (s == 0) { int sh[NN_MAXDIM]; memcpy(sh, m->out.shape, sizeof(int) * (size_t)m->out.nd); sh[0] = N; nt_resize(&res, m->out.nd, sh); row = bs ? m->out.n / (size_t)bs : 0; }
        memcpy(res.d + (size_t)s * row, m->out.d, sizeof(nn_real) * m->out.n); }
    nt_free(&xb); if (N == 0) nt_resize(&res, 1, (int[1]){ 0 }); nt_free(out); *out = res;
}

/* ------------------------------------------------------------------ fit */
static void snap_save(NnLayer *m, NnTensor **snap, int *nsnap) {
    NnParam **ps; int n = nn_collect_params(m, &ps); if (!*snap) *snap = nn_calloc((size_t)(n ? n : 1), sizeof(NnTensor)); *nsnap = n;
    for (int i = 0; i < n; i++) nt_copy(&(*snap)[i], &ps[i]->w); free(ps);
}
static void snap_restore(NnLayer *m, NnTensor *snap) { NnParam **ps; int n = nn_collect_params(m, &ps); for (int i = 0; i < n; i++) nt_copy(&ps[i]->w, &snap[i]); free(ps); }
static void hist_push(NnHistory *h, double l, double vl, double me, double vme, double lr) {
    if (h->epochs_run == h->cap) { h->cap = h->cap ? h->cap * 2 : 32; double **a[] = { &h->loss, &h->val_loss, &h->metric, &h->val_metric, &h->lr }; for (int i = 0; i < 5; i++) *a[i] = nn_realloc(*a[i], sizeof(double) * (size_t)h->cap); }
    int e = h->epochs_run++; h->loss[e] = l; h->val_loss[e] = vl; h->metric[e] = me; h->val_metric[e] = vme; h->lr[e] = lr;
}
void nn_history_free(NnHistory *h) { free(h->loss); free(h->val_loss); free(h->metric); free(h->val_metric); free(h->lr); memset(h, 0, sizeof *h); }

void nn_fit(NnLayer *m, NnLoss *loss, NnOpt *opt, const NnTensor *X, const NnTensor *Y, const NnFitCfg *c, NnHistory *h) {
    if (X->nd < 1 || Y->nd < 1) nn_fail("fit: X and Y must have at least one dimension (samples first)");
    int N = X->shape[0]; if (Y->shape[0] != N) nn_fail("fit: X has %d samples but Y has %d", N, Y->shape[0]); if (N < 1) nn_fail("fit: no training samples");
    if (c->epochs < 1) nn_fail("fit: epochs must be >= 1"); if (c->batch_size < 1) nn_fail("fit: batch_size must be >= 1");
    memset(h, 0, sizeof *h); h->best_epoch = -1; h->best_val = INFINITY;
    int64_t *perm = nn_malloc(sizeof(int64_t) * (size_t)N); for (int i = 0; i < N; i++) perm[i] = i;
    NnTensor vx_own, vy_own; nt_init(&vx_own); nt_init(&vy_own); const NnTensor *vx = c->vx, *vy = c->vy; int ntrain = N;
    if (!vx && c->val_split > 0) {
        if (c->val_split >= 1) nn_fail("fit: val_split must be in [0, 1)"); for (int i = N - 1; i > 0; i--) { int64_t j = nn_randint(i + 1); int64_t t = perm[i]; perm[i] = perm[j]; perm[j] = t; }
        int nv = (int)floor(c->val_split * N + 0.5); if (nv < 1) nv = 1; if (nv >= N) nv = N - 1; if (nv < 1) nn_fail("fit: not enough samples to hold out a validation split");
        ntrain = N - nv; gather(X, perm + ntrain, nv, &vx_own); gather(Y, perm + ntrain, nv, &vy_own); vx = &vx_own; vy = &vy_own;
    }
    if (vx && (vx->nd < 1 || vy->shape[0] != vx->shape[0])) nn_fail("fit: validation X and Y must have the same number of samples");
    bool plateau = c->schedule && !strncmp(c->schedule, "plateau", 7); double pl_pat = 3, pl_fac = 0.5, pl_min = 0; double base = opt->base_lr, cur_lr = base;
    if (plateau) { double a[3] = { 3, 0.5, 0 }; const char *s = c->schedule; int k = 0; while ((s = strchr(s, ':')) && k < 3) { a[k++] = strtod(++s, NULL); } pl_pat = a[0]; pl_fac = a[1]; pl_min = a[2]; }
    NnTensor xb, yb; nt_init(&xb); nt_init(&yb); NnMetric mt; NnTensor *snap = NULL; int nsnap = 0; double best = INFINITY, pl_best = INFINITY; int wait = 0, pl_wait = 0; clock_t t_all = clock();
    int accum = c->accum < 1 ? 1 : c->accum; int64_t *bidx = nn_malloc(sizeof(int64_t) * (size_t)c->batch_size);
    for (int e = 0; e < c->epochs; e++) {
        clock_t t0 = clock(); if (!plateau) cur_lr = nn_sched_lr(c->schedule, base, e, c->epochs, c->warmup); opt->lr = cur_lr;
        if (c->shuffle) for (int i = ntrain - 1; i > 0; i--) { int64_t j = nn_randint(i + 1); int64_t t = perm[i]; perm[i] = perm[j]; perm[j] = t; }
        memset(&mt, 0, sizeof mt); mt.kind = c->metric; mt.k = c->metric_k; double lsum = 0; long lcnt = 0; int pending = 0;
        for (int s = 0; s < ntrain; s += c->batch_size) {
            int bs = ntrain - s < c->batch_size ? ntrain - s : c->batch_size; if (c->drop_last && bs < c->batch_size && ntrain > c->batch_size) break;
            for (int i = 0; i < bs; i++) bidx[i] = perm[s + i]; gather(X, bidx, bs, &xb); gather(Y, bidx, bs, &yb);
            if (pending == 0) nn_zero_grad(m);
            nn_forward(m, &xb, 1); double l = nn_loss_eval(loss, &m->out, &yb, true);
            if (!isfinite(l)) nn_fail("training diverged: loss is %s at epoch %d (lower the learning rate, add clip_norm, or normalise the inputs)", isnan(l) ? "NaN" : "infinite", e + 1);
            nn_backward_loss(m, loss, false, NULL); nn_metric_update(&mt, &m->out, &yb); lsum += l * bs; lcnt += bs; pending++;
            bool last = s + c->batch_size >= ntrain;
            if (pending >= accum || last) {
                if (pending > 1) { NnParam **ps; int np = nn_collect_params(m, &ps); for (int i = 0; i < np; i++) if (!ps[i]->is_buffer) for (size_t j = 0; j < ps[i]->g.n; j++) ps[i]->g.d[j] /= (double)pending; free(ps); }
                if (c->clip_value > 0) nn_clip_grad_value(m, c->clip_value); if (c->clip_norm > 0) nn_clip_grad_norm(m, c->clip_norm);
                nn_opt_step(opt); pending = 0;
            }
        }
        double tl = lcnt ? lsum / (double)lcnt : 0, tm = nn_metric_value(&mt), vl = NAN, vm = NAN;
        if (vx && (e + 1) % (c->val_every < 1 ? 1 : c->val_every) == 0) { double vmv = 0; vl = nn_evaluate(m, loss, vx, vy, c->batch_size, c->metric, c->metric_k, &vmv); vm = vmv; }
        hist_push(h, tl, vl, tm, vm, cur_lr); h->epochs_run = e + 1;
        double mon = (vx && !isnan(vl)) ? vl : tl;
        if (mon < best - c->min_delta) { best = mon; h->best_epoch = e; h->best_val = mon; wait = 0; if (c->restore_best) snap_save(m, &snap, &nsnap); } else wait++;
        if (plateau) { if (mon < pl_best - 1e-12) { pl_best = mon; pl_wait = 0; } else if (++pl_wait > (int)pl_pat) { cur_lr *= pl_fac; if (cur_lr < pl_min) cur_lr = pl_min; pl_wait = 0; } }
        h->seconds = (double)(clock() - t_all) / CLOCKS_PER_SEC;
        if (c->verbose > 0 && ((e + 1) % c->verbose == 0 || e == 0 || e + 1 == c->epochs)) {
            printf("Epoch %*d/%d  loss %.5g", (int)floor(log10((double)c->epochs)) + 1, e + 1, c->epochs, tl); if (c->metric != NNM_NONE) printf("  %s %.4f", nn_metric_name(c->metric, c->metric_k), tm);
            if (!isnan(vl)) { printf("  val_loss %.5g", vl); if (c->metric != NNM_NONE) printf("  val_%s %.4f", nn_metric_name(c->metric, c->metric_k), vm); }
            printf("  lr %.3g  (%.2fs)\n", cur_lr, (double)(clock() - t0) / CLOCKS_PER_SEC); fflush(stdout);
        }
        if (c->callback && c->callback(c->ud, e + 1, tl, vl, tm)) { h->stopped_early = true; break; }
        if (c->patience > 0 && wait >= c->patience) { h->stopped_early = true; if (c->verbose > 0) printf("Early stopping at epoch %d (best epoch %d)\n", e + 1, h->best_epoch + 1); break; }
    }
    if (c->restore_best && snap) { snap_restore(m, snap); for (int i = 0; i < nsnap; i++) nt_free(&snap[i]); }
    free(snap); free(bidx); free(perm); nt_free(&xb); nt_free(&yb); nt_free(&vx_own); nt_free(&vy_own); opt->lr = base;
}
