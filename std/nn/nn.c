/* KingCobra std.nn — script glue core: errors, handles, conversions, argument & option parsing, module table.
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com> */
#include "nn_glue.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

KcPos *NN_POS = NULL;

void nn_fail(const char *fmt, ...) {
    char buf[700]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    if (NN_POS) kc_error_at(*NN_POS, "nn: %s", buf); else fprintf(stderr, "nn: %s\n", buf);
    kc_fatal(65);
}

/* ------------------------------------------------------------------ handles */
KcValue nnh_wrap(int sub, void *p) {
    KcHandle *h = kc_malloc(sizeof *h); h->kind = H_NN; h->ptr = p; h->size = 0; h->extra = sub; KcValue v; v.type = VAL_HANDLE; v.as.handle = h; return v;
}
void *nnh_get(KcValue v, int sub) { if (v.type != VAL_HANDLE || v.as.handle->kind != H_NN || v.as.handle->extra != sub) return NULL; return v.as.handle->ptr; }
static const char *SUBNAME[] = { "tensor", "model or layer", "loss", "optimizer" };
void *nnh_need(KcValue *a, int n, int i, int sub, const char *fn) {
    if (i >= n) NNERR("nn.%s: missing %s argument (position %d)", fn, SUBNAME[sub], i + 1);
    if (a[i].type == VAL_HANDLE && a[i].as.handle->kind == H_NN && a[i].as.handle->extra == sub && !a[i].as.handle->ptr) NNERR("nn.%s: argument %d was already freed", fn, i + 1);
    void *p = nnh_get(a[i], sub); if (!p) NNERR("nn.%s: argument %d must be a %s (got %s)", fn, i + 1, SUBNAME[sub], kc_type_name(a[i])); return p;
}
KcValue nn_ret_tensor(NnTensor *t) { return nnh_wrap(NNH_TENSOR, t); }

/* ------------------------------------------------------------------ nested array <-> tensor */
static bool infer_shape(KcValue v, int *shape, int *nd) {
    *nd = 0; KcValue cur = v;
    while (cur.type == VAL_ARRAY) { if (*nd >= NN_MAXDIM) return false; shape[(*nd)++] = cur.as.arr->count; if (cur.as.arr->count == 0) break; cur = cur.as.arr->items[0]; }
    return true;
}
static bool fill(KcValue v, int level, int nd, const int *shape, nn_real **p) {
    if (level == nd) { if (v.type == VAL_INT) *(*p)++ = (nn_real)v.as.i; else if (v.type == VAL_FLOAT) *(*p)++ = v.as.f; else if (v.type == VAL_BOOL) *(*p)++ = v.as.b ? 1.0 : 0.0; else return false; return true; }
    if (v.type != VAL_ARRAY || v.as.arr->count != shape[level]) return false;
    for (int i = 0; i < v.as.arr->count; i++) if (!fill(v.as.arr->items[i], level + 1, nd, shape, p)) return false; return true;
}
bool nn_value_to_tensor(KcValue v, NnTensor *out, const char *fn) {
    TArg dummy; (void)dummy;
    NnTensor *h = nnh_get(v, NNH_TENSOR);
    if (v.type == VAL_HANDLE && v.as.handle->kind == H_NN && v.as.handle->extra == NNH_TENSOR && !v.as.handle->ptr) NNERR("nn.%s: the tensor was already freed", fn);
    if (h) { nt_copy(out, h); return true; }
    if (v.type == VAL_INT || v.type == VAL_FLOAT || v.type == VAL_BOOL) { nt_resize(out, 0, NULL); out->d[0] = v.type == VAL_INT ? (nn_real)v.as.i : v.type == VAL_FLOAT ? v.as.f : (v.as.b ? 1.0 : 0.0); return true; }
    if (v.type != VAL_ARRAY) return false;
    int shape[NN_MAXDIM], nd; if (!infer_shape(v, shape, &nd)) NNERR("nn.%s: nested arrays can have at most %d dimensions", fn, NN_MAXDIM);
    nt_resize(out, nd, shape); nn_real *p = out->d; if (!fill(v, 0, nd, shape, &p)) NNERR("nn.%s: expected a rectangular nested array of numbers (found ragged rows or a non-numeric value)", fn); return true;
}
NnTensor *targ_get(TArg *ta, KcValue *a, int n, int i, const char *fn) {
    ta->owned = false; nt_init(&ta->tmp); ta->t = NULL;
    if (i >= n) NNERR("nn.%s: missing tensor argument (position %d)", fn, i + 1);
    NnTensor *h = nnh_get(a[i], NNH_TENSOR); if (h) { ta->t = h; return h; }
    if (!nn_value_to_tensor(a[i], &ta->tmp, fn)) NNERR("nn.%s: argument %d must be a tensor, a number or a nested array of numbers (got %s)", fn, i + 1, kc_type_name(a[i]));
    ta->owned = true; ta->t = &ta->tmp; return ta->t;
}
void targ_free(TArg *ta) { if (ta->owned) nt_free(&ta->tmp); ta->owned = false; }
static KcValue nest(const NnTensor *t, int level, size_t *off) {
    if (level == t->nd) return kc_float(t->d[(*off)++]);
    KcValue arr = kc_array_new(); for (int i = 0; i < t->shape[level]; i++) kc_array_push(arr, nest(t, level + 1, off)); return arr;
}
KcValue nn_to_nested(const NnTensor *t) { size_t off = 0; return nest(t, 0, &off); }

/* ------------------------------------------------------------------ scalar argument helpers */
void need_n(int n, int lo, int hi, const char *fn) {
    if (n >= lo && n <= hi) return;
    if (lo == hi) NNERR("nn.%s expects %d argument%s, got %d", fn, lo, lo == 1 ? "" : "s", n); NNERR("nn.%s expects %d to %d arguments, got %d", fn, lo, hi, n);
}
int64_t arg_int(KcValue *a, int n, int i, const char *fn) {
    if (i >= n) NNERR("nn.%s: missing argument %d", fn, i + 1);
    if (a[i].type == VAL_INT) return a[i].as.i; if (a[i].type == VAL_FLOAT && a[i].as.f == floor(a[i].as.f) && fabs(a[i].as.f) < 9e15) return (int64_t)a[i].as.f;
    NNERR("nn.%s: argument %d must be an integer (got %s)", fn, i + 1, kc_type_name(a[i])); return 0;
}
double arg_num(KcValue *a, int n, int i, const char *fn) {
    if (i >= n) NNERR("nn.%s: missing argument %d", fn, i + 1);
    if (a[i].type == VAL_INT) return (double)a[i].as.i; if (a[i].type == VAL_FLOAT) return a[i].as.f;
    NNERR("nn.%s: argument %d must be a number (got %s)", fn, i + 1, kc_type_name(a[i])); return 0;
}
const char *arg_str(KcValue *a, int n, int i, const char *fn) {
    if (i >= n || a[i].type != VAL_STRING) NNERR("nn.%s: argument %d must be a string", fn, i + 1); return a[i].as.s->data;
}
int arg_shape(KcValue *a, int n, int i, int *shape, const char *fn) {
    if (i >= n) NNERR("nn.%s: missing shape argument", fn);
    if (a[i].type == VAL_INT || a[i].type == VAL_FLOAT) { shape[0] = (int)arg_int(a, n, i, fn); if (shape[0] < 0) NNERR("nn.%s: negative dimension", fn); return 1; }
    if (a[i].type != VAL_ARRAY) NNERR("nn.%s: shape must be an array of integers like [2, 3] (got %s)", fn, kc_type_name(a[i]));
    int nd = a[i].as.arr->count; if (nd > NN_MAXDIM) NNERR("nn.%s: at most %d dimensions are supported", fn, NN_MAXDIM);
    for (int k = 0; k < nd; k++) { KcValue v = a[i].as.arr->items[k]; double d = v.type == VAL_INT ? (double)v.as.i : v.type == VAL_FLOAT ? v.as.f : -1;
        if (d < -1 || d != floor(d) || d > 2147483647.0) NNERR("nn.%s: shape entries must be integers (got %s)", fn, kc_type_name(v)); shape[k] = (int)d; if (shape[k] < -1) NNERR("nn.%s: negative dimension", fn); }
    return nd;
}
KcValue mk_struct(const char *type, int cnt, const char **names, KcValue *vals) {
    KcStructInst *s = kc_struct_alloc(); s->type_name = kc_strdup(type); s->count = cnt; s->names = kc_malloc(sizeof(char *) * (size_t)cnt); s->values = kc_malloc(sizeof(KcValue) * (size_t)cnt);
    for (int i = 0; i < cnt; i++) { s->names[i] = kc_strdup(names[i]); s->values[i] = vals[i]; } KcValue v; v.type = VAL_STRUCT; v.as.strct = s; return v;
}
KcValue arr_of_doubles(const double *d, int n) { KcValue r = kc_array_new(); for (int i = 0; i < n; i++) kc_array_push(r, kc_float(d[i])); return r; }
KcValue arr_of_ints(const int64_t *d, int n) { KcValue r = kc_array_new(); for (int i = 0; i < n; i++) kc_array_push(r, kc_int(d[i])); return r; }

/* ------------------------------------------------------------------ options */
void nnopts_parse(NnOpts *O, KcValue *a, int n, int start, const char *fn) {
    O->n = 0; O->nseen = 0; O->fn = fn;
    if (start < n && a[start].type == VAL_STRUCT) {
        if (n - start != 1) NNERR("nn.%s: pass either one options struct or name/value pairs, not both", fn);
        KcStructInst *s = a[start].as.strct; for (int i = 0; i < s->count && O->n < 48; i++) { O->o[O->n].k = s->names[i]; O->o[O->n].v = s->values[i]; O->o[O->n].used = false; O->n++; } return;
    }
    if ((n - start) % 2) NNERR("nn.%s: options must come in name/value pairs (e.g. \"epochs\", 20)", fn);
    for (int i = start; i + 1 < n; i += 2) {
        if (a[i].type != VAL_STRING) NNERR("nn.%s: option names must be strings (got %s at position %d)", fn, kc_type_name(a[i]), i + 1);
        if (O->n >= 48) NNERR("nn.%s: too many options", fn); O->o[O->n].k = a[i].as.s->data; O->o[O->n].v = a[i + 1]; O->o[O->n].used = false; O->n++;
    }
}
static void seen(NnOpts *O, const char *k) { for (int i = 0; i < O->nseen; i++) if (!strcmp(O->seen[i], k)) return; if (O->nseen < 64) O->seen[O->nseen++] = k; }
KcValue *nnopts_val(NnOpts *O, const char *key) { seen(O, key); for (int i = 0; i < O->n; i++) if (!strcmp(O->o[i].k, key)) { O->o[i].used = true; return &O->o[i].v; } return NULL; }
bool nnopts_has(NnOpts *O, const char *key) { return nnopts_val(O, key) != NULL; }
double nnopts_num(NnOpts *O, const char *key, double def) {
    KcValue *v = nnopts_val(O, key); if (!v) return def; if (v->type == VAL_INT) return (double)v->as.i; if (v->type == VAL_FLOAT) return v->as.f;
    NNERR("nn.%s: option '%s' must be a number (got %s)", O->fn, key, kc_type_name(*v)); return 0;
}
int64_t nnopts_int(NnOpts *O, const char *key, int64_t def) {
    KcValue *v = nnopts_val(O, key); if (!v) return def; if (v->type == VAL_INT) return v->as.i; if (v->type == VAL_FLOAT && v->as.f == floor(v->as.f)) return (int64_t)v->as.f;
    NNERR("nn.%s: option '%s' must be an integer (got %s)", O->fn, key, kc_type_name(*v)); return 0;
}
bool nnopts_bool(NnOpts *O, const char *key, bool def) {
    KcValue *v = nnopts_val(O, key); if (!v) return def; if (v->type == VAL_BOOL) return v->as.b; if (v->type == VAL_INT) return v->as.i != 0;
    NNERR("nn.%s: option '%s' must be true or false (got %s)", O->fn, key, kc_type_name(*v)); return false;
}
const char *nnopts_str(NnOpts *O, const char *key, const char *def) {
    KcValue *v = nnopts_val(O, key); if (!v) return def; if (v->type == VAL_STRING) return v->as.s->data;
    NNERR("nn.%s: option '%s' must be a string (got %s)", O->fn, key, kc_type_name(*v)); return NULL;
}
void nnopts_done(NnOpts *O) {
    for (int i = 0; i < O->n; i++) if (!O->o[i].used) {
        char list[400] = ""; size_t o = 0; for (int k = 0; k < O->nseen && o < sizeof list - 40; k++) o += (size_t)snprintf(list + o, sizeof list - o, "%s%s", k ? ", " : "", O->seen[k]);
        NNERR("nn.%s: unknown option '%s' (valid options: %s)", O->fn, O->o[i].k, list);
    }
}

/* ------------------------------------------------------------------ display */
typedef struct { char *p; size_t len, cap; } Sb;
static void sb(Sb *b, const char *fmt, ...) {
    char t[128]; va_list ap; va_start(ap, fmt); int w = vsnprintf(t, sizeof t, fmt, ap); va_end(ap); if (w < 0) return; if ((size_t)w >= sizeof t) w = (int)sizeof t - 1;
    if (b->len + (size_t)w + 1 > b->cap) { b->cap = (b->len + (size_t)w + 1) * 2; b->p = realloc(b->p, b->cap); } memcpy(b->p + b->len, t, (size_t)w); b->len += (size_t)w; b->p[b->len] = 0;
}
static void sb_tensor(Sb *b, const NnTensor *t, int level, size_t *off) {
    if (level == t->nd) { sb(b, "%.6g", t->d[(*off)++]); return; }
    sb(b, "["); for (int i = 0; i < t->shape[level]; i++) { if (i) sb(b, ", "); sb_tensor(b, t, level + 1, off); } sb(b, "]");
}
char *nn_handle_display(KcHandle *h) {
    Sb b = { NULL, 0, 0 }; sb(&b, "");
    if (!h->ptr) { sb(&b, "<nn.freed>"); return b.p; }
    switch (h->extra) {
    case NNH_TENSOR: { NnTensor *t = h->ptr; sb(&b, "tensor%s ", nt_shape_str(t->nd, t->shape));
        if (t->n <= 120) { size_t off = 0; sb_tensor(&b, t, 0, &off); }
        else { double mn = t->d[0], mx = t->d[0], s = 0; for (size_t i = 0; i < t->n; i++) { if (t->d[i] < mn) mn = t->d[i]; if (t->d[i] > mx) mx = t->d[i]; s += t->d[i]; } sb(&b, "<%zu values: min %.4g, max %.4g, mean %.4g>", t->n, mn, mx, s / (double)t->n); }
        break; }
    case NNH_LAYER: { NnLayer *L = h->ptr; if (L->kind == NNL_SEQ) sb(&b, "<nn.sequential: %d layers, %ld params>", L->nkids, nn_count_params(L, false)); else if (L->kind == NNL_RESIDUAL) sb(&b, "<nn.residual>"); else sb(&b, "<nn.%s '%s'>", nn_kind_name(L->kind), L->name); break; }
    case NNH_LOSS: { NnLoss *l = h->ptr; sb(&b, "<nn.loss %s>", nn_loss_name(l->kind)); break; }
    case NNH_OPT: { NnOpt *o = h->ptr; static const char *nm[] = { "sgd", "adam", "adamw", "rmsprop", "adagrad" }; sb(&b, "<nn.%s lr=%g>", nm[o->kind], o->lr); break; }
    default: sb(&b, "<nn handle>"); }
    return b.p;
}

/* ------------------------------------------------------------------ module table */
KcModule *kc_mod_nn(void) {
    static KcModule m; static bool ready = false;
    if (!ready) {
        int total = nn_tensor_count + nn_model_count + nn_math_count + nn_linalg_count; KcModuleEntry *e = malloc(sizeof(KcModuleEntry) * (size_t)total); int k = 0;
        for (int i = 0; i < nn_tensor_count; i++) { e[k].name = (char *)nn_tensor_entries[i].name; e[k].fn = nn_tensor_entries[i].fn; k++; }
        for (int i = 0; i < nn_model_count; i++) { e[k].name = (char *)nn_model_entries[i].name; e[k].fn = nn_model_entries[i].fn; k++; }
        for (int i = 0; i < nn_math_count; i++) { e[k].name = (char *)nn_math_entries[i].name; e[k].fn = nn_math_entries[i].fn; k++; }
        for (int i = 0; i < nn_linalg_count; i++) { e[k].name = (char *)nn_linalg_entries[i].name; e[k].fn = nn_linalg_entries[i].fn; k++; }
        m.name = "nn"; m.entries = e; m.count = total; ready = true;
    }
    return &m;
}
