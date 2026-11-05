/* std.nn optimizers, gradient clipping, learning-rate schedules */
#include "nn_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

static const char *ON[] = { "sgd", "adam", "adamw", "rmsprop", "adagrad" };
int nn_opt_from_name(const char *n) { for (int i = 0; i < NNO_COUNT; i++) if (!strcmp(n, ON[i])) return i; return -1; }

NnOpt *nn_opt_new(int kind, NnLayer *model) {
    NnOpt *o = nn_calloc(1, sizeof *o); o->kind = kind; o->model = model; o->beta1 = 0.9; o->beta2 = 0.999; o->eps = 1e-8; o->alpha = 0.99;
    switch (kind) { case NNO_SGD: o->lr = 0.01; break; case NNO_ADAMW: o->lr = 1e-3; o->wd = 0.01; break; case NNO_ADAGRAD: o->lr = 0.01; o->eps = 1e-10; break; default: o->lr = 1e-3; break; }
    o->base_lr = o->lr; o->np = nn_collect_params(model, &o->params);
    o->s1 = nn_calloc((size_t)(o->np ? o->np : 1), sizeof(NnTensor)); o->s2 = nn_calloc((size_t)(o->np ? o->np : 1), sizeof(NnTensor)); o->s3 = nn_calloc((size_t)(o->np ? o->np : 1), sizeof(NnTensor));
    for (int i = 0; i < o->np; i++) { if (o->params[i]->is_buffer) continue; NnParam *p = o->params[i]; nt_alloc(&o->s1[i], p->w.nd, p->w.shape); if (kind != NNO_ADAGRAD) nt_alloc(&o->s2[i], p->w.nd, p->w.shape);
        if (kind == NNO_ADAM || kind == NNO_ADAMW || kind == NNO_RMSPROP) nt_alloc(&o->s3[i], p->w.nd, p->w.shape); }
    return o;
}
void nn_opt_free(NnOpt *o) {
    if (!o) return; for (int i = 0; i < o->np; i++) { nt_free(&o->s1[i]); nt_free(&o->s2[i]); nt_free(&o->s3[i]); }
    free(o->s1); free(o->s2); free(o->s3); free(o->params); free(o);
}
void nn_opt_step(NnOpt *o) {
    NnParam **cur; int n = nn_collect_params(o->model, &cur); free(cur);
    if (n != o->np) nn_fail("the model's layers changed after this optimizer was created; create a new optimizer");
    o->step++; double lr = o->lr, bc1 = 1.0 - pow(o->beta1, (double)o->step), bc2 = 1.0 - pow(o->beta2, (double)o->step);
    for (int i = 0; i < o->np; i++) {
        NnParam *p = o->params[i]; if (p->is_buffer || p->frozen) continue;
        nn_real *w = p->w.d, *g = p->g.d, *a = o->s1[i].d, *b = o->s2[i].d, *c = o->s3[i].d; size_t N = p->w.n; double wd = (o->decay_all || p->decay) ? o->wd : 0.0;
        switch (o->kind) {
        case NNO_SGD:
            for (size_t j = 0; j < N; j++) { double gj = g[j] + wd * w[j];
                if (o->momentum > 0) { a[j] = o->momentum * a[j] + gj; gj = o->nesterov ? gj + o->momentum * a[j] : a[j]; } w[j] -= lr * gj; } break;
        case NNO_ADAM: case NNO_ADAMW:
            for (size_t j = 0; j < N; j++) {
                double gj = g[j]; if (o->kind == NNO_ADAM) gj += wd * w[j]; else if (wd > 0) w[j] *= (1.0 - lr * wd);
                a[j] = o->beta1 * a[j] + (1.0 - o->beta1) * gj; b[j] = o->beta2 * b[j] + (1.0 - o->beta2) * gj * gj;
                double vh = b[j] / bc2; if (o->amsgrad) { if (vh > c[j]) c[j] = vh; vh = c[j]; }
                w[j] -= lr * (a[j] / bc1) / (sqrt(vh) + o->eps); } break;
        case NNO_RMSPROP:
            for (size_t j = 0; j < N; j++) { double gj = g[j] + wd * w[j]; a[j] = o->alpha * a[j] + (1.0 - o->alpha) * gj * gj; double avg;
                if (o->centered) { c[j] = o->alpha * c[j] + (1.0 - o->alpha) * gj; double v = a[j] - c[j] * c[j]; avg = sqrt(v > 0 ? v : 0) + o->eps; } else avg = sqrt(a[j]) + o->eps;
                if (o->momentum > 0) { b[j] = o->momentum * b[j] + gj / avg; w[j] -= lr * b[j]; } else w[j] -= lr * gj / avg; } break;
        case NNO_ADAGRAD:
            for (size_t j = 0; j < N; j++) { double gj = g[j] + wd * w[j]; a[j] += gj * gj; w[j] -= lr * gj / (sqrt(a[j]) + o->eps); } break;
        }
    }
}

double nn_clip_grad_norm(NnLayer *m, double max_norm) {
    NnParam **ps; int n = nn_collect_params(m, &ps); double s = 0;
    for (int i = 0; i < n; i++) if (!ps[i]->is_buffer && !ps[i]->frozen) for (size_t j = 0; j < ps[i]->g.n; j++) s += ps[i]->g.d[j] * ps[i]->g.d[j];
    double norm = sqrt(s);
    if (max_norm > 0 && norm > max_norm && isfinite(norm)) { double f = max_norm / (norm + 1e-12); for (int i = 0; i < n; i++) if (!ps[i]->is_buffer && !ps[i]->frozen) for (size_t j = 0; j < ps[i]->g.n; j++) ps[i]->g.d[j] *= f; }
    free(ps); return norm;
}
void nn_clip_grad_value(NnLayer *m, double v) {
    NnParam **ps; int n = nn_collect_params(m, &ps);
    for (int i = 0; i < n; i++) if (!ps[i]->is_buffer) for (size_t j = 0; j < ps[i]->g.n; j++) { if (ps[i]->g.d[j] > v) ps[i]->g.d[j] = v; else if (ps[i]->g.d[j] < -v) ps[i]->g.d[j] = -v; }
    free(ps);
}

/* ------------------------------------------------------------------ schedules */
static int split_spec(const char *spec, char *name, size_t nn_, double *args, int maxa, char *err, size_t errn) {
    const char *c = strchr(spec, ':'); size_t l = c ? (size_t)(c - spec) : strlen(spec); if (l >= nn_) l = nn_ - 1; memcpy(name, spec, l); name[l] = 0; int na = 0;
    while (c) { const char *s = c + 1; char *e; double v = strtod(s, &e); if (e == s || (*e != ':' && *e != 0)) { if (err) snprintf(err, errn, "bad number in schedule '%s'", spec); return -1; }
        if (na >= maxa) { if (err) snprintf(err, errn, "too many arguments in schedule '%s'", spec); return -1; } args[na++] = v; c = *e == ':' ? e : NULL; }
    return na;
}
bool nn_sched_valid(const char *spec, char *err, size_t errn) {
    char name[16]; double a[4]; int na = split_spec(spec, name, sizeof name, a, 4, err, errn); if (na < 0) return false;
    struct { const char *n; int lo, hi; } t[] = { { "const", 0, 0 }, { "step", 2, 2 }, { "exp", 1, 1 }, { "cosine", 0, 1 }, { "linear", 0, 1 }, { "poly", 1, 2 }, { "plateau", 2, 3 } };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) if (!strcmp(name, t[i].n)) { if (na < t[i].lo || na > t[i].hi) { if (err) snprintf(err, errn, "schedule '%s' takes %d to %d numbers, got %d", name, t[i].lo, t[i].hi, na); return false; }
        if (!strcmp(name, "step") && a[0] < 1) { if (err) snprintf(err, errn, "step schedule needs a period >= 1"); return false; } return true; }
    if (err) snprintf(err, errn, "unknown schedule '%s' (const, step:E:G, exp:G, cosine[:min], linear[:end], poly:p[:min], plateau:patience:factor[:min])", name); return false;
}
double nn_sched_lr(const char *spec, double base, int epoch, int total, int warmup) {
    char name[16]; double a[4] = { 0, 0, 0, 0 }; int na = split_spec(spec ? spec : "const", name, sizeof name, a, 4, NULL, 0); if (na < 0) return base;
    if (warmup > 0 && epoch < warmup) return base * (double)(epoch + 1) / (double)(warmup + 1);
    int e = epoch - (warmup > 0 ? warmup : 0), T = total - (warmup > 0 ? warmup : 0); if (T < 1) T = 1; double f = (double)e / (double)T; if (f > 1) f = 1;
    if (!strcmp(name, "step")) return base * pow(a[1], floor((double)e / a[0]));
    if (!strcmp(name, "exp")) return base * pow(a[0], (double)e);
    if (!strcmp(name, "cosine")) { double mn = na > 0 ? a[0] : 0; return mn + (base - mn) * 0.5 * (1.0 + cos(3.14159265358979323846 * f)); }
    if (!strcmp(name, "linear")) { double en = na > 0 ? a[0] : 0; return base + (en - base) * f; }
    if (!strcmp(name, "poly")) { double mn = na > 1 ? a[1] : 0; return mn + (base - mn) * pow(1.0 - f, a[0]); }
    return base;
}
