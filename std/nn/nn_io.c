/* std.nn serialization.
 *   model  : "KCNN" u32 version | layer tree | u32 crc32(of everything before)
 *   weights: "KCNW" u32 version | u32 nparams | params | u32 crc32
 *   layer  : u8 kind | u8 nhp | nhp*f64 | u8 namelen,name | u8 nparams | per param (u8 frozen, u8 nd, nd*i32, n*f64) | u32 nkids | kids...
 * All integers/floats are little-endian. Loading never trusts a size field: every read is bounds-checked.
 */
#include "nn_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#define NN_VERSION 1u

/* ------------------------------------------------------------------ crc32 */
static uint32_t crc_tab[256]; static bool crc_ready = false;
static uint32_t crc32_of(const unsigned char *p, size_t n) {
    if (!crc_ready) { for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1; crc_tab[i] = c; } crc_ready = true; }
    uint32_t c = 0xFFFFFFFFu; for (size_t i = 0; i < n; i++) c = crc_tab[(c ^ p[i]) & 0xFF] ^ (c >> 8); return c ^ 0xFFFFFFFFu;
}

/* ------------------------------------------------------------------ writer */
void nnbuf_init(NnBuf *b) { b->p = NULL; b->len = b->cap = 0; }
void nnbuf_free(NnBuf *b) { free(b->p); nnbuf_init(b); }
void nnbuf_put(NnBuf *b, const void *p, size_t n) { if (b->len + n > b->cap) { b->cap = (b->len + n) * 2 + 64; b->p = nn_realloc(b->p, b->cap); } if (n) memcpy(b->p + b->len, p, n); b->len += n; }
static void w8(NnBuf *b, unsigned v) { unsigned char c = (unsigned char)v; nnbuf_put(b, &c, 1); }
static void w32(NnBuf *b, uint32_t v) { unsigned char c[4] = { (unsigned char)v, (unsigned char)(v >> 8), (unsigned char)(v >> 16), (unsigned char)(v >> 24) }; nnbuf_put(b, c, 4); }
static void wf64(NnBuf *b, double d) { uint64_t u; memcpy(&u, &d, 8); unsigned char c[8]; for (int i = 0; i < 8; i++) c[i] = (unsigned char)(u >> (8 * i)); nnbuf_put(b, c, 8); }

static void write_param(NnBuf *b, const NnParam *p) {
    w8(b, p->frozen ? 1 : 0); w8(b, (unsigned)p->w.nd); for (int i = 0; i < p->w.nd; i++) w32(b, (uint32_t)p->w.shape[i]); for (size_t i = 0; i < p->w.n; i++) wf64(b, p->w.d[i]);
}
static void write_layer(NnBuf *b, const NnLayer *L) {
    w8(b, (unsigned)L->kind); w8(b, (unsigned)L->nhp); for (int i = 0; i < L->nhp; i++) wf64(b, L->hp[i]);
    size_t nl = strlen(L->name); if (nl > 31) nl = 31; w8(b, (unsigned)nl); nnbuf_put(b, L->name, nl);
    w8(b, (unsigned)L->nparams); for (int i = 0; i < L->nparams; i++) write_param(b, &L->params[i]);
    w32(b, (uint32_t)L->nkids); for (int i = 0; i < L->nkids; i++) write_layer(b, L->kids[i]);
}
unsigned char *nn_serialize(const NnLayer *m, size_t *len) {
    NnBuf b; nnbuf_init(&b); nnbuf_put(&b, "KCNN", 4); w32(&b, NN_VERSION); write_layer(&b, m); w32(&b, crc32_of(b.p, b.len)); *len = b.len; return b.p;
}

/* ------------------------------------------------------------------ reader */
typedef struct { const unsigned char *p; size_t len, pos; bool bad; char err[200]; } Rd;
static void rd_fail(Rd *r, const char *msg) { if (!r->bad) { r->bad = true; snprintf(r->err, sizeof r->err, "%s", msg); } }
static bool rd_need(Rd *r, size_t n) { if (r->bad) return false; if (n > r->len - r->pos) { rd_fail(r, "file is truncated"); return false; } return true; }
static unsigned r8(Rd *r) { if (!rd_need(r, 1)) return 0; return r->p[r->pos++]; }
static uint32_t r32(Rd *r) { if (!rd_need(r, 4)) return 0; const unsigned char *q = r->p + r->pos; r->pos += 4; return (uint32_t)q[0] | (uint32_t)q[1] << 8 | (uint32_t)q[2] << 16 | (uint32_t)q[3] << 24; }
static double rf64(Rd *r) { if (!rd_need(r, 8)) return 0; uint64_t u = 0; for (int i = 0; i < 8; i++) u |= (uint64_t)r->p[r->pos + (size_t)i] << (8 * i); r->pos += 8; double d; memcpy(&d, &u, 8); return d; }

static void read_param_into(Rd *r, NnParam *p) {
    unsigned fr = r8(r); unsigned nd = r8(r); if (nd != (unsigned)p->w.nd) { rd_fail(r, "parameter rank does not match the layer definition"); return; }
    for (unsigned i = 0; i < nd; i++) if ((int)r32(r) != p->w.shape[i]) { rd_fail(r, "parameter shape does not match the layer definition"); return; }
    if (!rd_need(r, p->w.n * 8)) return; for (size_t i = 0; i < p->w.n; i++) p->w.d[i] = rf64(r); p->frozen = fr != 0;
}
static NnLayer *read_layer(Rd *r, int depth) {
    if (depth > 64) { rd_fail(r, "layer nesting is too deep"); return NULL; }
    unsigned kind = r8(r), nhp = r8(r); if (r->bad) return NULL;
    if (kind == 0 || kind >= NNL_KIND_MAX) { rd_fail(r, "unknown layer type"); return NULL; } if (nhp > 16) { rd_fail(r, "invalid hyper-parameter count"); return NULL; }
    double hp[16] = { 0 }; for (unsigned i = 0; i < nhp; i++) { hp[i] = rf64(r); if (!isfinite(hp[i]) || fabs(hp[i]) > 1e9) rd_fail(r, "invalid hyper-parameter value"); }
    unsigned nl = r8(r); char name[40] = ""; if (nl > 31) rd_fail(r, "invalid layer name"); else if (rd_need(r, nl)) { memcpy(name, r->p + r->pos, nl); name[nl] = 0; r->pos += nl; }
    if (r->bad) return NULL;
    NnLayer *L = nn_layer_new((int)kind, hp, (int)nhp, NNI_NONE, 0.0); snprintf(L->name, sizeof L->name, "%s", name);
    unsigned np = r8(r); if (np != (unsigned)L->nparams) rd_fail(r, "parameter count does not match the layer definition");
    for (int i = 0; i < L->nparams && !r->bad; i++) read_param_into(r, &L->params[i]);
    uint32_t nk = r32(r); if (nk > 1000000u) rd_fail(r, "invalid child count");
    for (uint32_t i = 0; i < nk && !r->bad; i++) { NnLayer *k = read_layer(r, depth + 1); if (k) nn_container_append(L, k); }
    if (r->bad) { nn_layer_free(L); return NULL; } return L;
}
NnLayer *nn_deserialize(const unsigned char *p, size_t len, char *err, size_t errn) {
    Rd r = { p, len, 0, false, "" };
    if (len < 12 || memcmp(p, "KCNN", 4) != 0) { snprintf(err, errn, "not a KingCobra nn model file (bad magic)"); return NULL; }
    uint32_t stored = (uint32_t)p[len - 4] | (uint32_t)p[len - 3] << 8 | (uint32_t)p[len - 2] << 16 | (uint32_t)p[len - 1] << 24;
    if (crc32_of(p, len - 4) != stored) { snprintf(err, errn, "model file is corrupt (checksum mismatch)"); return NULL; }
    r.len = len - 4; r.pos = 4; uint32_t ver = r32(&r); if (ver != NN_VERSION) { snprintf(err, errn, "unsupported model file version %u (this build reads version %u)", ver, NN_VERSION); return NULL; }
    NnLayer *m = read_layer(&r, 0); if (!m) { snprintf(err, errn, "%s", r.err[0] ? r.err : "invalid model file"); return NULL; }
    if (r.pos != r.len) { nn_layer_free(m); snprintf(err, errn, "model file has trailing data"); return NULL; }
    if (m->kind != NNL_SEQ) { nn_layer_free(m); snprintf(err, errn, "model file does not contain a sequential model"); return NULL; } return m;
}
NnLayer *nn_clone(const NnLayer *L) {
    size_t n; unsigned char *b = nn_serialize(L, &n); char err[200]; NnLayer *c = nn_deserialize(b, n, err, sizeof err); free(b);
    if (!c) nn_fail("clone failed: %s", err); return c;
}

/* ------------------------------------------------------------------ files */
static bool write_file(const char *path, const unsigned char *b, size_t n, char *err, size_t errn) {
    char tmp[1100]; snprintf(tmp, sizeof tmp, "%s.tmp", path); FILE *f = fopen(tmp, "wb");
    if (!f) { snprintf(err, errn, "cannot open '%s' for writing", tmp); return false; }
    bool ok = fwrite(b, 1, n, f) == n; if (fclose(f) != 0) ok = false; if (!ok) { remove(tmp); snprintf(err, errn, "failed while writing '%s' (disk full?)", tmp); return false; }
    if (rename(tmp, path) != 0) { remove(path); if (rename(tmp, path) != 0) { remove(tmp); snprintf(err, errn, "cannot replace '%s'", path); return false; } } return true;
}
static unsigned char *read_file(const char *path, size_t *n, char *err, size_t errn) {
    FILE *f = fopen(path, "rb"); if (!f) { snprintf(err, errn, "cannot open '%s'", path); return NULL; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET); if (sz < 0 || (unsigned long)sz > (1UL << 31)) { fclose(f); snprintf(err, errn, "'%s' has an invalid size", path); return NULL; }
    unsigned char *b = nn_malloc((size_t)sz); if (fread(b, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(b); snprintf(err, errn, "failed reading '%s'", path); return NULL; } fclose(f); *n = (size_t)sz; return b;
}
bool nn_save_file(const NnLayer *m, const char *path, char *err, size_t errn) { size_t n; unsigned char *b = nn_serialize(m, &n); bool ok = write_file(path, b, n, err, errn); free(b); return ok; }
NnLayer *nn_load_file(const char *path, char *err, size_t errn) { size_t n; unsigned char *b = read_file(path, &n, err, errn); if (!b) return NULL; NnLayer *m = nn_deserialize(b, n, err, errn); free(b); return m; }

bool nn_save_weights(const NnLayer *m, const char *path, char *err, size_t errn) {
    NnParam **ps; int n = nn_collect_params((NnLayer *)m, &ps); NnBuf b; nnbuf_init(&b); nnbuf_put(&b, "KCNW", 4); w32(&b, NN_VERSION); w32(&b, (uint32_t)n);
    for (int i = 0; i < n; i++) write_param(&b, ps[i]); w32(&b, crc32_of(b.p, b.len)); free(ps); bool ok = write_file(path, b.p, b.len, err, errn); nnbuf_free(&b); return ok;
}
bool nn_load_weights(NnLayer *m, const char *path, char *err, size_t errn) {
    size_t len; unsigned char *p = read_file(path, &len, err, errn); if (!p) return false;
    if (len < 16 || memcmp(p, "KCNW", 4) != 0) { free(p); snprintf(err, errn, "not a KingCobra nn weights file (bad magic)"); return false; }
    uint32_t stored = (uint32_t)p[len - 4] | (uint32_t)p[len - 3] << 8 | (uint32_t)p[len - 2] << 16 | (uint32_t)p[len - 1] << 24;
    if (crc32_of(p, len - 4) != stored) { free(p); snprintf(err, errn, "weights file is corrupt (checksum mismatch)"); return false; }
    Rd r = { p, len - 4, 4, false, "" }; uint32_t ver = r32(&r), cnt = r32(&r); NnParam **ps; int n = nn_collect_params(m, &ps);
    if (ver != NN_VERSION) { snprintf(err, errn, "unsupported weights file version %u", ver); free(ps); free(p); return false; }
    if ((int)cnt != n) { snprintf(err, errn, "weights file has %u parameter tensors but the model has %d (different architecture?)", cnt, n); free(ps); free(p); return false; }
    NnTensor *stage = nn_calloc((size_t)(n ? n : 1), sizeof(NnTensor)); bool ok = true;
    for (int i = 0; i < n && !r.bad; i++) { unsigned fr = r8(&r), nd = r8(&r); (void)fr; if (nd != (unsigned)ps[i]->w.nd) { rd_fail(&r, "a parameter has a different rank than the model expects"); break; }
        for (unsigned k = 0; k < nd; k++) if ((int)r32(&r) != ps[i]->w.shape[k]) { rd_fail(&r, "a parameter has a different shape than the model expects (different architecture?)"); break; }
        if (r.bad) break; nt_copy(&stage[i], &ps[i]->w); if (!rd_need(&r, stage[i].n * 8)) break; for (size_t k = 0; k < stage[i].n; k++) stage[i].d[k] = rf64(&r); }
    if (r.bad) { snprintf(err, errn, "%s", r.err); ok = false; } else for (int i = 0; i < n; i++) nt_copy(&ps[i]->w, &stage[i]);     /* all-or-nothing */
    for (int i = 0; i < n; i++) nt_free(&stage[i]); free(stage); free(ps); free(p); return ok;
}
void nn_copy_weights(NnLayer *dst, const NnLayer *src, double tau) {
    NnParam **a, **b; int na = nn_collect_params(dst, &a), nb = nn_collect_params((NnLayer *)src, &b);
    if (na != nb) { free(a); free(b); nn_fail("copy_weights: the models have different numbers of parameter tensors (%d vs %d)", na, nb); }
    for (int i = 0; i < na; i++) if (!nt_same_shape(&a[i]->w, &b[i]->w)) { free(a); free(b); nn_fail("copy_weights: parameter %d has shape %s in the target but %s in the source", i, nt_shape_str(a[i]->w.nd, a[i]->w.shape), nt_shape_str(b[i]->w.nd, b[i]->w.shape)); }
    for (int i = 0; i < na; i++) for (size_t k = 0; k < a[i]->w.n; k++) a[i]->w.d[k] = (a[i]->is_buffer || tau >= 1.0) ? b[i]->w.d[k] : tau * b[i]->w.d[k] + (1.0 - tau) * a[i]->w.d[k];
    free(a); free(b);
}
