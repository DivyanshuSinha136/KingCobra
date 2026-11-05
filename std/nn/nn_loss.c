/* std.nn loss functions. All reduce by mean and produce d(loss)/d(pred) in l->grad. */
#include "nn_internal.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>

static const char *LN[] = { "mse", "mae", "huber", "logcosh", "bce", "bce_logits", "ce", "nll", "kl", "hinge" };
int nn_loss_from_name(const char *n) {
    if (!strcmp(n, "l1")) return NNLS_MAE; if (!strcmp(n, "l2")) return NNLS_MSE; if (!strcmp(n, "smooth_l1")) return NNLS_HUBER;
    if (!strcmp(n, "bce_with_logits")) return NNLS_BCE_LOGITS; if (!strcmp(n, "binary_crossentropy")) return NNLS_BCE;
    if (!strcmp(n, "cross_entropy") || !strcmp(n, "crossentropy")) return NNLS_CE; if (!strcmp(n, "kl_div")) return NNLS_KL;
    for (int i = 0; i < NNLS_COUNT; i++) if (!strcmp(n, LN[i])) return i;
    return -1;
}
const char *nn_loss_name(int id) { return (id >= 0 && id < NNLS_COUNT) ? LN[id] : "?"; }
NnLoss *nn_loss_new(int kind) { NnLoss *l = nn_calloc(1, sizeof *l); l->kind = kind; l->delta = 1.0; l->pos_weight = 1.0; nt_init(&l->grad); return l; }
void nn_loss_free(NnLoss *l) { if (!l) return; nt_free(&l->grad); free(l); }

static inline double sigm(double x) { return x >= 0 ? 1.0 / (1.0 + exp(-x)) : exp(x) / (1.0 + exp(x)); }

double nn_loss_eval(NnLoss *l, const NnTensor *p, const NnTensor *t, bool wg) {
    int k = l->kind; size_t n = p->n; if (n == 0) nn_fail("loss: prediction tensor is empty");
    nt_resize(&l->grad, p->nd, p->shape); nn_real *g = l->grad.d; double L = 0; const nn_real *x = p->d, *y = t->d;
    if (k != NNLS_CE && k != NNLS_NLL && t->n != n)
        nn_fail("loss '%s': prediction shape %s and target shape %s must have the same number of elements", nn_loss_name(k), nt_shape_str(p->nd, p->shape), nt_shape_str(t->nd, t->shape));
    switch (k) {
    case NNLS_MSE: for (size_t i = 0; i < n; i++) { double d = x[i] - y[i]; L += d * d; g[i] = 2.0 * d / (double)n; } L /= (double)n; break;
    case NNLS_MAE: for (size_t i = 0; i < n; i++) { double d = x[i] - y[i]; L += fabs(d); g[i] = ((d > 0) - (d < 0)) / (double)n; } L /= (double)n; break;
    case NNLS_HUBER: { double dl = l->delta; for (size_t i = 0; i < n; i++) { double d = x[i] - y[i], a = fabs(d);
        if (a <= dl) { L += 0.5 * d * d; g[i] = d / (double)n; } else { L += dl * (a - 0.5 * dl); g[i] = dl * ((d > 0) - (d < 0)) / (double)n; } } L /= (double)n; break; }
    case NNLS_LOGCOSH: for (size_t i = 0; i < n; i++) { double d = x[i] - y[i], a = fabs(d); L += a + log1p(exp(-2.0 * a)) - 0.693147180559945309; g[i] = tanh(d) / (double)n; } L /= (double)n; break;
    case NNLS_BCE: { const double e = 1e-12; for (size_t i = 0; i < n; i++) { double q = x[i] < e ? e : (x[i] > 1 - e ? 1 - e : x[i]); L += -(y[i] * log(q) + (1 - y[i]) * log(1 - q)); g[i] = (q - y[i]) / (q * (1 - q)) / (double)n; } L /= (double)n; break; }
    case NNLS_BCE_LOGITS: { double pw = l->pos_weight; for (size_t i = 0; i < n; i++) { double z = x[i], w = 1.0 + (pw - 1.0) * y[i];
        L += (1.0 - y[i]) * z + w * (log1p(exp(-fabs(z))) + (-z > 0 ? -z : 0)); g[i] = ((1.0 - y[i]) - w * (1.0 - sigm(z))) / (double)n; } L /= (double)n; break; }
    case NNLS_HINGE: for (size_t i = 0; i < n; i++) { double s = y[i] > 0 ? 1.0 : -1.0, m = 1.0 - s * x[i]; if (m > 0) { L += m; g[i] = -s / (double)n; } else g[i] = 0; } L /= (double)n; break;
    case NNLS_KL: { size_t C = (size_t)p->shape[p->nd - 1], R = n / C; for (size_t i = 0; i < n; i++) { if (y[i] > 0) L += y[i] * (log(y[i]) - x[i]); g[i] = -y[i] / (double)R; } L /= (double)R; break; }
    case NNLS_CE: case NNLS_NLL: {
        if (p->nd < 1) nn_fail("loss '%s': predictions need a class dimension", nn_loss_name(k));
        size_t C = (size_t)p->shape[p->nd - 1], R = C ? n / C : 0; bool idx = t->n == R, dist = t->n == n;
        if (!idx && !dist) nn_fail("loss '%s': target shape %s matches neither class indices (%zu values) nor a distribution (%zu values)", nn_loss_name(k), nt_shape_str(t->nd, t->shape), R, n);
        if (k == NNLS_NLL && !idx) nn_fail("loss 'nll' expects class-index targets"); double eps = l->smoothing; size_t cnt = 0;
        for (size_t r = 0; r < R; r++) {
            const nn_real *row = x + r * C; nn_real *gr = g + r * C; long cls = -1;
            if (idx) { cls = (long)y[r]; if (l->has_ignore && cls == (long)l->ignore_index) { for (size_t c = 0; c < C; c++) gr[c] = 0; continue; }
                if ((double)cls != y[r] || cls < 0 || cls >= (long)C) nn_fail("loss '%s': class index %g at row %zu is outside 0..%zu", nn_loss_name(k), y[r], r, C - 1); }
            cnt++;
            if (k == NNLS_NLL) { L += -row[cls]; for (size_t c = 0; c < C; c++) gr[c] = 0; gr[cls] = -1.0; continue; }
            double m = -DBL_MAX, s = 0; for (size_t c = 0; c < C; c++) if (row[c] > m) m = row[c]; for (size_t c = 0; c < C; c++) s += exp(row[c] - m); double lse = m + log(s);
            for (size_t c = 0; c < C; c++) { double q = idx ? ((long)c == cls ? 1.0 : 0.0) : y[r * C + c]; if (eps > 0) q = q * (1.0 - eps) + eps / (double)C; L += -q * (row[c] - lse); gr[c] = exp(row[c] - lse) - q; }
        }
        double inv = cnt ? 1.0 / (double)cnt : 0.0; L *= inv; for (size_t i = 0; i < n; i++) g[i] *= inv; break; }
    }
    if (!wg) { /* gradient is cheap; keep it so nn_backward can follow without recomputation */ }
    return L;
}
