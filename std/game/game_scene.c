/* std.game — entities (3D), particles (2D + 3D), sprite sheets + animations, streamed 2D tilemaps. */
#include "game_internal.h"

float G_gravity = 26.0f;

/* ================================================================ 2D camera */
void world_to_screen2d(float x, float y, float *sx, float *sy) {
    *sx = (x - G.c2x) * G.c2zoom + G.w * 0.5f; *sy = (y - G.c2y) * G.c2zoom + G.h * 0.5f;
}

/* ================================================================== sprites */
static bool draw_region(Tex *t, int rx, int ry, int rw, int rh, float x, float y, const Opts *o) {
    if (!t) return false;
    float sc_x = 1, sc_y = 1; const KcValue *sv = opt_find(o, "scale");
    if (sv) { if (sv->type == VAL_ARRAY && sv->as.arr->count >= 2) { sc_x = (float)V_num(sv->as.arr->items[0], "scale"); sc_y = (float)V_num(sv->as.arr->items[1], "scale"); } else sc_x = sc_y = (float)V_num(*sv, "scale"); }
    float w = (float)opt_num(o, "w", (rw > 0 ? rw : t->w - rx) * sc_x), h = (float)opt_num(o, "h", (rh > 0 ? rh : t->h - ry) * sc_y);
    bool world = opt_bool(o, "world", false), center = opt_bool(o, "center", false); float rot = DEG2RAD((float)opt_num(o, "rot", 0));
    int flip = (opt_bool(o, "flip_x", false) ? 1 : 0) | (opt_bool(o, "flip_y", false) ? 2 : 0);
    uint32_t tint = opt_col(o, "tint", 0xFFFFFFFFu); float al = (float)opt_num(o, "alpha", 1.0); if (al < 1) tint = (tint & 0x00FFFFFFu) | ((uint32_t)(CA(tint) * al) << 24);
    if (world) { world_to_screen2d(x, y, &x, &y); w *= G.c2zoom; h *= G.c2zoom; }
    if (center) { x -= w * 0.5f; y -= h * 0.5f; }
    gfx_blit(t, rx, ry, rw, rh, x, y, w, h, rot, flip, tint);
    return true;
}
bool sprite_draw(int tex, float x, float y, const Opts *o) {
    Tex *t = tex_get(tex); if (!t) return false;
    return draw_region(t, opt_int(o, "sx", 0), opt_int(o, "sy", 0), opt_int(o, "sw", 0), opt_int(o, "sh", 0), x, y, o);
}

typedef struct { int tex, cw, ch, margin, spacing, cols, rows; bool alive; } Sheet;
static Sheet *SH; static int nsh;
int sheet_new(int tex, int cw, int ch, int margin, int spacing) {
    Tex *t = tex_get(tex); if (!t) gm_err("sheet: invalid texture %d", tex);
    if (cw < 1 || ch < 1) gm_err("sheet: cell size must be positive");
    SH = kc_realloc(SH, sizeof(Sheet) * (size_t)(nsh + 1)); Sheet *s = &SH[nsh];
    s->tex = tex; s->cw = cw; s->ch = ch; s->margin = margin; s->spacing = spacing; s->alive = true;
    s->cols = imaxi(1, (t->w - 2 * margin + spacing) / (cw + spacing)); s->rows = imaxi(1, (t->h - 2 * margin + spacing) / (ch + spacing));
    return nsh++;
}
bool sheet_draw(int sheet, int index, float x, float y, const Opts *o) {
    if (sheet < 0 || sheet >= nsh) gm_err("sheet_draw: invalid sheet %d", sheet);
    Sheet *s = &SH[sheet]; if (index < 0 || index >= s->cols * s->rows) return false;
    Tex *t = tex_get(s->tex); int cx = index % s->cols, cy = index / s->cols;
    return draw_region(t, s->margin + cx * (s->cw + s->spacing), s->margin + cy * (s->ch + s->spacing), s->cw, s->ch, x, y, o);
}

typedef struct { int sheet; int *frames; int nf; float fps, t, speed; bool loop, playing, alive; } Anim;
static Anim *AN; static int nanim;
int anim_new(int sheet, const int *frames, int nf, float fps, bool loop) {
    if (sheet < 0 || sheet >= nsh) gm_err("anim: invalid sheet %d", sheet);
    if (nf < 1) gm_err("anim: needs at least one frame");
    AN = kc_realloc(AN, sizeof(Anim) * (size_t)(nanim + 1)); Anim *a = &AN[nanim];
    a->sheet = sheet; a->frames = kc_malloc(sizeof(int) * (size_t)nf); memcpy(a->frames, frames, sizeof(int) * (size_t)nf);
    a->nf = nf; a->fps = fps; a->loop = loop; a->t = 0; a->speed = 1; a->playing = true; a->alive = true; return nanim++;
}
bool anim_draw(int id, float x, float y, const Opts *o) {
    if (id < 0 || id >= nanim) gm_err("anim_draw: invalid animation %d", id);
    Anim *a = &AN[id]; int k = (int)(a->t * a->fps); k = a->loop ? k % a->nf : imini(k, a->nf - 1);
    return sheet_draw(a->sheet, a->frames[k], x, y, o);
}
void anim_ctl(int id, const char *what) {
    if (id < 0 || id >= nanim) gm_err("anim: invalid animation %d", id);
    Anim *a = &AN[id];
    if (!strcmp(what, "play")) a->playing = true; else if (!strcmp(what, "pause") || !strcmp(what, "stop")) { a->playing = false; if (!strcmp(what, "stop")) a->t = 0; }
    else if (!strcmp(what, "reset")) a->t = 0; else gm_err("anim: unknown control \"%s\" (play, pause, stop, reset)", what);
}

/* ================================================================ entities */
typedef struct { bool alive, visible, gravity, collide, grounded, wire; int model, tex, tag; V3 pos, rot, scale, vel; uint32_t tint; float hw, hh, spin; } Ent;
static Ent *E; static int ne, ce;
int ent_count(void) { int n = 0; for (int i = 0; i < ne; i++) if (E[i].alive) n++; return n; }
bool ent_alive(int id) { return id >= 0 && id < ne && E[id].alive; }
int ent_spawn(int model, V3 pos) {
    if (!model_info(model)) gm_err("spawn: invalid model %d", model);
    int id = -1; for (int i = 0; i < ne; i++) if (!E[i].alive) { id = i; break; }
    if (id < 0) { if (ne == ce) { ce = ce ? ce * 2 : 64; E = kc_realloc(E, sizeof(Ent) * (size_t)ce); } id = ne++; }
    Ent *e = &E[id]; memset(e, 0, sizeof *e); e->alive = true; e->visible = true; e->model = model; e->tex = -1; e->pos = pos; e->scale = v3(1, 1, 1); e->tint = 0xFFFFFFFFu; e->hw = 0.3f; e->hh = 1.0f;
    return id;
}
void ent_despawn(int id) { if (ent_alive(id)) E[id].alive = false; }
static V3 vnum3(KcValue *v, int n, const char *who) {
    if (n >= 3) return v3((float)V_num(v[0], who), (float)V_num(v[1], who), (float)V_num(v[2], who));
    if (n == 1 && v[0].type == VAL_ARRAY && v[0].as.arr->count >= 3) { KcValue *i = v[0].as.arr->items; return v3((float)V_num(i[0], who), (float)V_num(i[1], who), (float)V_num(i[2], who)); }
    if (n == 1 && V_isnum(v[0])) { float s = (float)V_num(v[0], who); return v3(s, s, s); }
    gm_err("%s: expected a number, [x, y, z], or three numbers", who);
}
bool ent_set(int id, const char *k, KcValue *v, int nv) {
    if (!ent_alive(id)) return false; Ent *e = &E[id]; if (nv < 1) gm_err("set: missing value for \"%s\"", k);
    if (!strcmp(k, "pos")) e->pos = vnum3(v, nv, k); else if (!strcmp(k, "x")) e->pos.x = (float)V_num(v[0], k); else if (!strcmp(k, "y")) e->pos.y = (float)V_num(v[0], k); else if (!strcmp(k, "z")) e->pos.z = (float)V_num(v[0], k);
    else if (!strcmp(k, "rot")) e->rot = vnum3(v, nv, k); else if (!strcmp(k, "yaw")) e->rot.y = (float)V_num(v[0], k); else if (!strcmp(k, "pitch")) e->rot.x = (float)V_num(v[0], k); else if (!strcmp(k, "roll")) e->rot.z = (float)V_num(v[0], k);
    else if (!strcmp(k, "scale")) e->scale = vnum3(v, nv, k);
    else if (!strcmp(k, "vel")) e->vel = vnum3(v, nv, k); else if (!strcmp(k, "vx")) e->vel.x = (float)V_num(v[0], k); else if (!strcmp(k, "vy")) e->vel.y = (float)V_num(v[0], k); else if (!strcmp(k, "vz")) e->vel.z = (float)V_num(v[0], k);
    else if (!strcmp(k, "tint")) e->tint = col_from_value(v[0], k); else if (!strcmp(k, "visible")) e->visible = kc_truthy(v[0]); else if (!strcmp(k, "gravity")) e->gravity = kc_truthy(v[0]);
    else if (!strcmp(k, "collide")) e->collide = kc_truthy(v[0]); else if (!strcmp(k, "wire")) e->wire = kc_truthy(v[0]);
    else if (!strcmp(k, "radius")) e->hw = (float)V_num(v[0], k); else if (!strcmp(k, "height")) e->hh = (float)V_num(v[0], k); else if (!strcmp(k, "spin")) e->spin = (float)V_num(v[0], k);
    else if (!strcmp(k, "tex")) e->tex = (int)V_num(v[0], k); else if (!strcmp(k, "tag")) e->tag = (int)V_num(v[0], k);
    else if (!strcmp(k, "model")) { int m = (int)V_num(v[0], k); if (!model_info(m)) gm_err("set: invalid model %d", m); e->model = m; }
    else gm_err("set: unknown entity property \"%s\" (pos x y z rot yaw pitch roll scale vel vx vy vz tint visible gravity collide wire radius height spin tex tag model)", k);
    return true;
}
KcValue ent_get(int id, const char *k) {
    if (!ent_alive(id)) return kc_null(); Ent *e = &E[id];
    if (!strcmp(k, "pos")) return arr3(e->pos.x, e->pos.y, e->pos.z); if (!strcmp(k, "x")) return kc_float(e->pos.x); if (!strcmp(k, "y")) return kc_float(e->pos.y); if (!strcmp(k, "z")) return kc_float(e->pos.z);
    if (!strcmp(k, "rot")) return arr3(e->rot.x, e->rot.y, e->rot.z); if (!strcmp(k, "yaw")) return kc_float(e->rot.y); if (!strcmp(k, "pitch")) return kc_float(e->rot.x); if (!strcmp(k, "roll")) return kc_float(e->rot.z);
    if (!strcmp(k, "scale")) return arr3(e->scale.x, e->scale.y, e->scale.z); if (!strcmp(k, "vel")) return arr3(e->vel.x, e->vel.y, e->vel.z);
    if (!strcmp(k, "vx")) return kc_float(e->vel.x); if (!strcmp(k, "vy")) return kc_float(e->vel.y); if (!strcmp(k, "vz")) return kc_float(e->vel.z);
    if (!strcmp(k, "visible")) return kc_bool(e->visible); if (!strcmp(k, "gravity")) return kc_bool(e->gravity); if (!strcmp(k, "collide")) return kc_bool(e->collide); if (!strcmp(k, "grounded")) return kc_bool(e->grounded);
    if (!strcmp(k, "radius")) return kc_float(e->hw); if (!strcmp(k, "height")) return kc_float(e->hh); if (!strcmp(k, "tag")) return kc_int(e->tag); if (!strcmp(k, "model")) return kc_int(e->model); if (!strcmp(k, "tex")) return kc_int(e->tex);
    if (!strcmp(k, "tint")) return kc_int(e->tint); if (!strcmp(k, "spin")) return kc_float(e->spin);
    gm_err("get: unknown entity property \"%s\"", k);
}
void ent_apply_opts(int id, const Opts *o) {
    static const char *keys[] = { "pos", "rot", "yaw", "scale", "tint", "tex", "gravity", "collide", "radius", "height", "tag", "visible", "spin", "vel", "wire" };
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) { const KcValue *v = opt_find(o, keys[i]); if (v) { KcValue tmp = *v; ent_set(id, keys[i], &tmp, 1); } }
}
bool ent_move(int id, V3 d) {
    if (!ent_alive(id)) return false; Ent *e = &E[id];
    if (!e->collide || !world_active()) { e->pos = v3add(e->pos, d); return false; }
    bool fl[4]; world_move(&e->pos, e->hw, e->hh, d, 0.6f, fl); e->grounded = fl[3]; return fl[3];
}
int ent_pick(V3 o, V3 d, float maxd, float *dist) {
    int best = -1; float bt = maxd; d = v3norm(d);
    for (int i = 0; i < ne; i++) {
        Ent *e = &E[i]; if (!e->alive || !e->visible) continue; Mesh *m = model_mesh(e->model); if (!m) continue;
        V3 mn = v3add(e->pos, v3(m->bmin.x * e->scale.x, m->bmin.y * e->scale.y, m->bmin.z * e->scale.z)), mx = v3add(e->pos, v3(m->bmax.x * e->scale.x, m->bmax.y * e->scale.y, m->bmax.z * e->scale.z));
        float t0 = 0, t1 = bt; bool ok = true; float oo[3] = { o.x, o.y, o.z }, dd[3] = { d.x, d.y, d.z }, a[3] = { fminf(mn.x, mx.x), fminf(mn.y, mx.y), fminf(mn.z, mx.z) }, b[3] = { fmaxf(mn.x, mx.x), fmaxf(mn.y, mx.y), fmaxf(mn.z, mx.z) };
        for (int k = 0; k < 3 && ok; k++) {
            if (fabsf(dd[k]) < 1e-9f) { if (oo[k] < a[k] || oo[k] > b[k]) ok = false; }
            else { float ta = (a[k] - oo[k]) / dd[k], tb = (b[k] - oo[k]) / dd[k]; if (ta > tb) { float x = ta; ta = tb; tb = x; } if (ta > t0) t0 = ta; if (tb < t1) t1 = tb; if (t0 > t1) ok = false; }
        }
        if (ok && t0 < bt) { bt = t0; best = i; }
    }
    if (best >= 0 && dist) *dist = bt; return best;
}

/* ================================================================ particles */
typedef struct { V3 p, v; float life, max, size, grav; uint32_t c0, c1; bool d3, alive; } Part3;
static Part3 PT[4096]; static int pt_next;
void part_emit(bool d3, V3 p, int count, const Opts *o) {
    float speed = (float)opt_num(o, "speed", d3 ? 3.0 : 80.0), life = (float)opt_num(o, "life", 1.0), size = (float)opt_num(o, "size", d3 ? 0.12 : 3.0), grav = (float)opt_num(o, "gravity", d3 ? 9.8 : 200.0), spread = (float)opt_num(o, "spread", 1.0);
    uint32_t c0 = opt_col(o, "color", 0xFFFFC040u), c1 = opt_col(o, "color_end", c0 & 0x00FFFFFFu); V3 dir = opt_v3(o, "dir", v3(0, d3 ? 1.0f : -1.0f, 0));
    for (int i = 0; i < count; i++) {
        Part3 *q = &PT[pt_next]; pt_next = (pt_next + 1) & 4095;
        V3 r = v3(rnd_range(-1, 1), rnd_range(-1, 1), d3 ? rnd_range(-1, 1) : 0); if (v3len(r) < 1e-4f) r = v3(0, 1, 0); r = v3norm(r);
        V3 v = v3mul(v3add(v3mul(r, spread), v3norm(dir)), speed * rnd_range(0.5f, 1.0f));
        q->p = p; q->v = v; q->life = q->max = life * rnd_range(0.7f, 1.0f); q->size = size; q->grav = grav; q->c0 = c0; q->c1 = c1; q->d3 = d3; q->alive = true;
    }
}
static void parts_update(float dt) {
    for (int i = 0; i < 4096; i++) { Part3 *q = &PT[i]; if (!q->alive) continue; q->life -= dt; if (q->life <= 0) { q->alive = false; continue; }
        q->v.y += (q->d3 ? -q->grav : q->grav) * dt; q->p = v3add(q->p, v3mul(q->v, dt)); }
}
void part_draw2d(void) {
    int n = 0;
    for (int i = 0; i < 4096; i++) { Part3 *q = &PT[i]; if (!q->alive || q->d3) continue; float t = 1.0f - q->life / q->max, sx, sy; world_to_screen2d(q->p.x, q->p.y, &sx, &sy);
        int s = imaxi(1, (int)(q->size * G.c2zoom)); gfx_rect((int)sx - s / 2, (int)sy - s / 2, s, s, col_mix(q->c0, q->c1, t), true); n++; }
    G.st_particles += n;
}

/* =============================================================== tilemaps */
#define TC 32
typedef struct { int cx, cy; uint16_t t[TC * TC]; bool dirty; uint64_t last; } TChunk;
typedef struct { bool alive; int tw, th, sheet; char *dir; TChunk **tab; int cap, cnt; uint8_t solid[8192]; int maxc; } Tmap;
static Tmap *TM; static int ntm;
bool tmap_valid(int id) { return id >= 0 && id < ntm && TM[id].alive; }
static Tmap *tm(int id) { if (!tmap_valid(id)) gm_err("tilemap: invalid id %d", id); return &TM[id]; }
int tmap_new(int tw, int th, int sheet, const char *dir) {
    if (tw < 1 || th < 1) gm_err("tilemap: tile size must be positive");
    TM = kc_realloc(TM, sizeof(Tmap) * (size_t)(ntm + 1)); Tmap *m = &TM[ntm]; memset(m, 0, sizeof *m);
    m->alive = true; m->tw = tw; m->th = th; m->sheet = sheet; m->dir = (dir && *dir) ? kc_strdup(dir) : NULL; m->maxc = 256; if (m->dir) dir_make(m->dir);
    return ntm++;
}
static uint32_t tkey(int cx, int cy) { return hash3(cx, cy, 1, 0x7e57u); }
static TChunk *tfind(Tmap *m, int cx, int cy) {
    if (!m->cap) return NULL; uint32_t h = tkey(cx, cy) & (uint32_t)(m->cap - 1);
    for (int n = 0; n < m->cap; n++) { TChunk *c = m->tab[h]; if (!c) return NULL; if (c != (TChunk *)1 && c->cx == cx && c->cy == cy) return c; h = (h + 1) & (uint32_t)(m->cap - 1); }
    return NULL;
}
static char *tpath(Tmap *m, int cx, int cy) { char n[48]; snprintf(n, sizeof n, "t.%d.%d.kct", cx, cy); return path_join(m->dir, n); }
static void tinsert(Tmap *m, TChunk *c) {
    if ((m->cnt + 1) * 2 > m->cap) { int nc = m->cap ? m->cap * 2 : 64; TChunk **nt = kc_malloc(sizeof(TChunk *) * (size_t)nc); memset(nt, 0, sizeof(TChunk *) * (size_t)nc);
        for (int i = 0; i < m->cap; i++) if (m->tab[i] && m->tab[i] != (TChunk *)1) { uint32_t h = tkey(m->tab[i]->cx, m->tab[i]->cy) & (uint32_t)(nc - 1); while (nt[h]) h = (h + 1) & (uint32_t)(nc - 1); nt[h] = m->tab[i]; }
        free(m->tab); m->tab = nt; m->cap = nc; }
    uint32_t h = tkey(c->cx, c->cy) & (uint32_t)(m->cap - 1); while (m->tab[h] && m->tab[h] != (TChunk *)1) h = (h + 1) & (uint32_t)(m->cap - 1); m->tab[h] = c; m->cnt++;
}
static bool tsave(Tmap *m, TChunk *c) {
    if (!m->dir) return false; char *p = tpath(m, c->cx, c->cy); bool ok = write_file_all(p, c->t, sizeof c->t); free(p); if (ok) c->dirty = false; return ok;
}
static TChunk *tload(Tmap *m, int cx, int cy, bool create) {
    TChunk *c = tfind(m, cx, cy); if (c) { c->last = G.frame; return c; }
    if (m->dir) { char *p = tpath(m, cx, cy); size_t n; char *d = read_file_all(p, &n); free(p);
        if (d && n == sizeof c->t) { c = kc_malloc(sizeof *c); c->cx = cx; c->cy = cy; memcpy(c->t, d, n); c->dirty = false; c->last = G.frame; tinsert(m, c); free(d); return c; } free(d); }
    if (!create) return NULL;
    c = kc_malloc(sizeof *c); memset(c, 0, sizeof *c); c->cx = cx; c->cy = cy; c->last = G.frame; tinsert(m, c); return c;
}
bool tmap_set(int id, int x, int y, int tile) {
    Tmap *m = tm(id); if (tile < 0 || tile > 65535) return false;
    TChunk *c = tload(m, floordiv(x, TC), floordiv(y, TC), tile != 0); if (!c) return true;
    c->t[floormod(y, TC) * TC + floormod(x, TC)] = (uint16_t)tile; c->dirty = true; return true;
}
int tmap_get(int id, int x, int y) {
    Tmap *m = tm(id); TChunk *c = tload(m, floordiv(x, TC), floordiv(y, TC), false); return c ? c->t[floormod(y, TC) * TC + floormod(x, TC)] : 0;
}
void tmap_solid(int id, int tile, bool solid) { Tmap *m = tm(id); if (tile < 0 || tile > 65535) return; if (solid) m->solid[tile >> 3] |= (uint8_t)(1 << (tile & 7)); else m->solid[tile >> 3] &= (uint8_t)~(1 << (tile & 7)); }
static bool tsolid(Tmap *m, int tx, int ty) { int t = tmap_get((int)(m - TM), tx, ty); return t && (m->solid[t >> 3] >> (t & 7) & 1); }
bool tmap_hit(int id, float x, float y, float w, float h) {
    Tmap *m = tm(id); int x0 = (int)floorf(x / m->tw), x1 = (int)floorf((x + w - 0.001f) / m->tw), y0 = (int)floorf(y / m->th), y1 = (int)floorf((y + h - 0.001f) / m->th);
    for (int ty = y0; ty <= y1; ty++) for (int tx = x0; tx <= x1; tx++) if (tsolid(m, tx, ty)) return true;
    return false;
}
static void tmap_step(Tmap *m, int id, float *x, float *y, float w, float h, float dx, float dy, bool *hx, bool *hy) {
    if (dx != 0) { float nx = *x + dx; if (!tmap_hit(id, nx, *y, w, h)) *x = nx; else { *hx = true; *x = dx > 0 ? floorf((nx + w - 0.001f) / m->tw) * m->tw - w - 0.001f : (floorf(nx / m->tw) + 1) * m->tw + 0.001f; } }
    if (dy != 0) { float ny = *y + dy; if (!tmap_hit(id, *x, ny, w, h)) *y = ny; else { *hy = true; *y = dy > 0 ? floorf((ny + h - 0.001f) / m->th) * m->th - h - 0.001f : (floorf(ny / m->th) + 1) * m->th + 0.001f; } }
}
void tmap_move(int id, float *x, float *y, float w, float h, float dx, float dy, bool *hx, bool *hy) {
    Tmap *m = tm(id); *hx = *hy = false;
    float big = fmaxf(fabsf(dx), fabsf(dy)), lim = fminf((float)m->tw, (float)m->th) * 0.5f; int steps = (int)ceilf(big / lim); if (steps < 1) steps = 1; if (steps > 500) steps = 500;
    float sx = dx / (float)steps, sy = dy / (float)steps;
    for (int i = 0; i < steps; i++) { bool a = false, b = false; tmap_step(m, id, x, y, w, h, sx, sy, &a, &b); if (a) { *hx = true; sx = 0; } if (b) { *hy = true; sy = 0; } }
}
void tmap_draw(int id, const Opts *o) {
    Tmap *m = tm(id); (void)o; if (m->sheet < 0 || m->sheet >= nsh) return; Sheet *s = &SH[m->sheet]; Tex *t = tex_get(s->tex); if (!t) return;
    float z = G.c2zoom; float vx0 = G.c2x - G.w * 0.5f / z, vy0 = G.c2y - G.h * 0.5f / z, vx1 = G.c2x + G.w * 0.5f / z, vy1 = G.c2y + G.h * 0.5f / z;
    int tx0 = (int)floorf(vx0 / m->tw), ty0 = (int)floorf(vy0 / m->th), tx1 = (int)floorf(vx1 / m->tw), ty1 = (int)floorf(vy1 / m->th); float dw = m->tw * z, dh = m->th * z;
    TChunk *cc = NULL; int ccx = 1 << 30, ccy = 1 << 30;
    for (int ty = ty0; ty <= ty1; ty++) for (int tx = tx0; tx <= tx1; tx++) {
        int cx = floordiv(tx, TC), cy = floordiv(ty, TC); if (cx != ccx || cy != ccy) { cc = tload(m, cx, cy, false); ccx = cx; ccy = cy; } if (!cc) continue;
        int tile = cc->t[floormod(ty, TC) * TC + floormod(tx, TC)]; if (!tile) continue; int idx = tile - 1; if (idx >= s->cols * s->rows) continue;
        float sx, sy; world_to_screen2d((float)(tx * m->tw), (float)(ty * m->th), &sx, &sy);
        gfx_blit(t, s->margin + (idx % s->cols) * (s->cw + s->spacing), s->margin + (idx / s->cols) * (s->ch + s->spacing), s->cw, s->ch, floorf(sx), floorf(sy), ceilf(dw + 0.01f), ceilf(dh + 0.01f), 0, 0, 0xFFFFFFFFu);
    }
    /* stream out chunks that are far from the view (only possible when disk-backed) */
    if (m->dir && m->cnt > m->maxc) {
        for (int i = 0; i < m->cap && m->cnt > m->maxc * 3 / 4; i++) { TChunk *c = m->tab[i]; if (!c || c == (TChunk *)1 || c->last + 120 > G.frame) continue;
            if (c->dirty) tsave(m, c); free(c); m->tab[i] = (TChunk *)1; m->cnt--; }
    }
}
int tmap_flush(int id) { Tmap *m = tm(id); int n = 0; for (int i = 0; i < m->cap; i++) { TChunk *c = m->tab[i]; if (c && c != (TChunk *)1 && c->dirty && tsave(m, c)) n++; } return n; }
int tmap_import_csv(int id, const char *path, int ox, int oy) {
    FILE *f = fopen(path, "r"); if (!f) gm_err("tilemap_import_csv: cannot open %s", path);
    char *line = NULL; size_t cap = 0; int y = 0, n = 0; ssize_t len;
    #if defined(_WIN32)
    cap = 1 << 16; line = kc_malloc(cap); while (fgets(line, (int)cap, f)) { len = (ssize_t)strlen(line);
    #else
    while ((len = getline(&line, &cap, f)) >= 0) {
    #endif
        int x = 0; char *p = line; (void)len;
        while (*p && *p != '\n' && *p != '\r') { char *q; long v = strtol(p, &q, 10); if (q == p) { p++; continue; } if (v > 0) { tmap_set(id, ox + x, oy + y, (int)v); n++; } x++; p = q; if (*p == ',') p++; }
        y++;
    }
    free(line); fclose(f); return n;
}
void tmap_stats(int id, int *chunks, int *bytes) { Tmap *m = tm(id); *chunks = m->cnt; *bytes = m->cnt * (int)sizeof(TChunk); }

/* ================================================================ frame hooks */
void scene_update(float dt) {
    for (int i = 0; i < nanim; i++) if (AN[i].alive && AN[i].playing) AN[i].t += dt * AN[i].speed;
    parts_update(dt);
    for (int i = 0; i < ne; i++) {
        Ent *e = &E[i]; if (!e->alive) continue; e->rot.y += e->spin * dt;
        if (world_active() && (e->gravity || e->collide)) {
            if (!world_chunk_loaded(floordiv((int)floorf(e->pos.x), 16), floordiv((int)floorf(e->pos.z), 16))) continue;          /* its chunk is not streamed in yet: keep still */
            if (e->gravity) e->vel.y -= G_gravity * dt; if (e->vel.y < -60) e->vel.y = -60;
            if (e->collide) { bool fl[4]; world_move(&e->pos, e->hw, e->hh, v3mul(e->vel, dt), 0, fl); e->grounded = fl[3]; if (fl[0]) e->vel.x = 0; if (fl[2]) e->vel.z = 0; if (fl[1]) e->vel.y = 0; }
            else e->pos = v3add(e->pos, v3mul(e->vel, dt));
        } else if (e->vel.x || e->vel.y || e->vel.z) {
            if (e->gravity) e->vel.y -= G_gravity * dt;
            e->pos = v3add(e->pos, v3mul(e->vel, dt));
        }
    }
}
void scene_draw3d(void) {
    float reach = (R_fog.mode == 0 ? G.zfar : (R_fog.autofog && R_fog.cur_end > 0 ? R_fog.cur_end : R_fog.end)) * 1.15f;
    for (int i = 0; i < ne; i++) {
        Ent *e = &E[i]; if (!e->alive || !e->visible) continue;
        Mesh *m = model_mesh(e->model); if (!m) continue; Model *mi = model_info(e->model);
        V3 d = v3sub(e->pos, G.cam); if (v3len(d) > reach + 4.0f) continue;
        float sm = fmaxf(e->scale.x, fmaxf(e->scale.y, e->scale.z)); V3 mn = v3add(e->pos, v3mul(v3(-1, -1, -1), 0)), mx = mn; (void)mn; (void)mx;
        float rad = fmaxf(v3len(v3sub(m->bmax, m->bmin)), 0.1f) * sm; V3 a = v3sub(e->pos, v3(rad, rad, rad)), b = v3add(e->pos, v3(rad, rad, rad));
        if (!r3d_aabb_visible(a, b)) continue;
        M4 mm = m4_trs(e->pos, e->rot, e->scale); uint32_t tint = col_mul(e->tint, mi ? mi->tint : 0xFFFFFFFFu);
        r3d_draw_mesh(m, &mm, tint, e->tex >= 0 ? e->tex : (mi ? mi->tex_override : -1), e->wire ? RF_WIRE : 0); G.st_ents_drawn++;
    }
    int n = 0;
    for (int i = 0; i < 4096; i++) { Part3 *q = &PT[i]; if (!q->alive || !q->d3) continue; r3d_point(q->p, q->size, col_mix(q->c0, q->c1, 1.0f - q->life / q->max)); n++; }
    G.st_particles += n;
}
void scene_reset(void) {
    free(E); E = NULL; ne = ce = 0; memset(PT, 0, sizeof PT);
    for (int i = 0; i < nanim; i++) free(AN[i].frames); free(AN); AN = NULL; nanim = 0; free(SH); SH = NULL; nsh = 0;
    for (int i = 0; i < ntm; i++) { Tmap *m = &TM[i]; if (!m->alive) continue; for (int k = 0; k < m->cap; k++) { TChunk *c = m->tab[k]; if (c && c != (TChunk *)1) { if (c->dirty) tsave(m, c); free(c); } } free(m->tab); free(m->dir); }
    free(TM); TM = NULL; ntm = 0;
}
