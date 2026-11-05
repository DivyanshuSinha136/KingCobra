/* std.ascii_screen — ASCII game engine core: sprites (static/animated, coloured, flippable), entities with
 * tile-collision physics (platformer + top-down), particles, camera (follow/deadzone/shake/bounds),
 * keyboard + mouse action bindings with axes. Maps/worlds live in as_map.c. */
#include "as_internal.h"

double G_cam_x = 0, G_cam_y = 0;
static double shake_amt = 0, shake_t = 0, shake_total = 0, g_gravity = 0, g_drag = 0;
static int vp_x = 0, vp_y = 0, vp_w = 0, vp_h = 0, cam_follow = 0, cam_bounds_map = 0; static double cam_smooth = 8, dz_w = 0, dz_h = 0;

/* ================================================================== sprites */
typedef struct { bool used; int w, h, n; Cell **fr; double fps; int ax, ay; } Sprite;
static Sprite *SP; static int nsp, capsp;
static Sprite *sp_get(int id) { return (id < 1 || id >= nsp || !SP[id].used) ? NULL : &SP[id]; }
bool sprite_valid(int id) { return sp_get(id) != NULL; }
static int sp_slot(void) { for (int i = 1; i < nsp; i++) if (!SP[i].used) return i; if (nsp == 0) nsp = 1; if (nsp >= capsp) { capsp = capsp ? capsp * 2 : 16; SP = kc_realloc(SP, (size_t)capsp * sizeof(Sprite)); } return nsp++; }
void sprite_free(int id) { Sprite *s = sp_get(id); if (!s) return; for (int i = 0; i < s->n; i++) free(s->fr[i]); free(s->fr); s->used = false; }
void sprite_size(int id, int *w, int *h, int *n) { Sprite *s = sp_get(id); if (!s) as_err("invalid sprite %d", id); *w = s->w; *h = s->h; *n = s->n; }
void sprite_set_anchor(int id, int ax, int ay) { Sprite *s = sp_get(id); if (!s) as_err("invalid sprite %d", id); s->ax = ax; s->ay = ay; }
Cell *sprite_cells(int id, int frame, int *w, int *h) { Sprite *s = sp_get(id); if (!s || !s->n) return NULL; *w = s->w; *h = s->h; return s->fr[as_clampi(frame, 0, s->n - 1)]; }
static void sp_add_frame(Sprite *s, Canvas *c) {
    s->fr = kc_realloc(s->fr, (size_t)(s->n + 1) * sizeof(Cell *)); s->fr[s->n] = kc_malloc((size_t)s->w * s->h * sizeof(Cell));
    Cell e = { ' ', 0, 0, 0 }; for (size_t i = 0; i < (size_t)s->w * s->h; i++) s->fr[s->n][i] = e;
    for (int y = 0; y < s->h && y < c->h; y++) for (int x = 0; x < s->w && x < c->w; x++) s->fr[s->n][(size_t)y * s->w + x] = c->c[(size_t)y * c->w + x];
    s->n++;
}
static void parse_palette(const char *p, Col *tab /*256*/) {
    while (*p) { while (*p == ' ' || *p == ',') p++; if (!*p) break; unsigned char k = (unsigned char)*p++; if (*p != '=') break; p++; char name[32]; size_t i = 0; while (*p && *p != ',' && i < 31) name[i++] = *p++; name[i] = 0; Col c; if (!col_parse(name, &c)) as_err("sprite palette: unknown colour \"%s\"", name); tab[k] = c; }
}
static char *rows_join(KcValue v, const char *who) {
    if (v.type == VAL_STRING) { char *d = kc_malloc(strlen(v.as.s->data) + 1); strcpy(d, v.as.s->data); return d; }
    if (v.type == VAL_ARRAY) { Buf b; buf_init(&b); for (int i = 0; i < v.as.arr->count; i++) { if (v.as.arr->items[i].type != VAL_STRING) as_err("%s: rows must be strings", who); if (i) buf_addc(&b, '\n'); buf_adds(&b, v.as.arr->items[i].as.s->data); } if (!b.d) buf_add(&b, "", 0); return b.d; }
    as_err("%s: expected a string or an array of row strings", who);
}
static int build_frame_canvas(KcValue text, const Opts *o) {
    char *txt = rows_join(text, "sprite"); Col fg = opt_col(o, "fg", 0), bg = opt_col(o, "bg", 0); Col pal[256]; memset(pal, 0, sizeof pal); const char *ps = opt_str(o, "palette", NULL); if (ps) parse_palette(ps, pal);
    const KcValue *cv = opt_find(o, "colors"); char *colors = cv ? rows_join(*cv, "sprite colors") : NULL; const char *tr = opt_str(o, "transparent", " "); uint32_t trc = (uint32_t)(unsigned char)tr[0];
    int w = 0, h = 1; { int cur = 0; for (const char *p = txt; ; p++) { if (*p == '\n' || !*p) { if (cur > w) w = cur; cur = 0; if (!*p) break; h++; } else { const char *q = p; cur += cp_width(utf8_next(&q)); p = q - 1; } } }
    if (w < 1) w = 1; int id = cv_new(w, h); Canvas *c = cv_get(id); const char *p = txt, *cp = colors; int y = 0;
    while (y < h) { const char *e = strchr(p, '\n'); size_t l = e ? (size_t)(e - p) : strlen(p); const char *ce = cp ? strchr(cp, '\n') : NULL; size_t cl = cp ? (ce ? (size_t)(ce - cp) : strlen(cp)) : 0; int x = 0; const char *q = p;
        while (q < p + l) { uint32_t ch = utf8_next(&q); int cw = cp_width(ch); if (cw == 0) continue; Col col = fg; if (cp && (size_t)x < cl) { Col pc = pal[(unsigned char)cp[x]]; if (pc) col = pc; }
            if (ch == trc || (ch == ' ' && trc == ' ')) { x += cw; continue; } cv_put(c, x, y, ch, col, bg, 0); x += cw; }
        y++; if (!e) break; p = e + 1; if (cp) cp = ce ? ce + 1 : cp + cl; }
    free(txt); free(colors); return id;
}
int sprite_from_canvas(Canvas *c, double fps) { int id = sp_slot(); Sprite *s = &SP[id]; memset(s, 0, sizeof *s); s->used = true; s->w = c->w; s->h = c->h; s->fps = fps > 0 ? fps : 8; sp_add_frame(s, c); return id; }
int sprite_create(KcValue text, const Opts *o) { int cid = build_frame_canvas(text, o); int id = sprite_from_canvas(cv_get(cid), 8); cv_free(cid); return id; }
int sprite_create_anim(KcValue frames, double fps, const Opts *o) {
    if (frames.type != VAL_ARRAY || frames.as.arr->count < 1) as_err("sprite_anim: frames must be a non-empty array (each frame a string or array of rows)");
    int n = frames.as.arr->count, cids[256]; if (n > 256) as_err("sprite_anim: at most 256 frames"); int w = 1, h = 1;
    for (int i = 0; i < n; i++) { cids[i] = build_frame_canvas(frames.as.arr->items[i], o); Canvas *c = cv_get(cids[i]); if (c->w > w) w = c->w; if (c->h > h) h = c->h; }
    int id = sp_slot(); Sprite *s = &SP[id]; memset(s, 0, sizeof *s); s->used = true; s->w = w; s->h = h; s->fps = fps > 0 ? fps : 8; for (int i = 0; i < n; i++) { sp_add_frame(s, cv_get(cids[i])); cv_free(cids[i]); }
    return id;
}
static uint32_t mirror_ch(uint32_t c) {
    switch (c) { case '/': return '\\'; case '\\': return '/'; case '(': return ')'; case ')': return '('; case '<': return '>'; case '>': return '<'; case '[': return ']'; case ']': return '['; case '{': return '}'; case '}': return '{';
        case 0x258C: return 0x2590; case 0x2590: return 0x258C; case 0x2598: return 0x259D; case 0x259D: return 0x2598; case 0x2596: return 0x2597; case 0x2597: return 0x2596; case 0x259B: return 0x259C; case 0x259C: return 0x259B;
        case 0x2599: return 0x259F; case 0x259F: return 0x2599; case 0x259E: return 0x259A; case 0x259A: return 0x259E; case 0x25C4: return 0x25BA; case 0x25BA: return 0x25C4; case 0x2190: return 0x2192; case 0x2192: return 0x2190; case 'd': return 'b'; case 'b': return 'd'; case 'p': return 'q'; case 'q': return 'p'; }
    return c;
}
void sprite_draw(int id, Canvas *c, int x, int y, int frame, bool fx, bool fy, Col tint, double t0) {
    Sprite *s = sp_get(id); if (!s || !s->n) return;
    if (frame < 0) frame = s->n > 1 ? (int)floor((T.time - t0) * s->fps) % s->n : 0; frame = as_clampi(frame, 0, s->n - 1);
    Cell *cells = s->fr[frame]; x -= s->ax; y -= s->ay;
    for (int j = 0; j < s->h; j++) for (int i = 0; i < s->w; i++) {
        int si = fx ? s->w - 1 - i : i, sj = fy ? s->h - 1 - j : j; Cell *q = &cells[(size_t)sj * s->w + si]; if (q->ch == CH_CONT) continue; if (q->ch == ' ' && !q->bg) continue;
        uint32_t ch = q->ch; if (fx) ch = mirror_ch(ch); if (fy && ch == 0x2580) ch = 0x2584; else if (fy && ch == 0x2584) ch = 0x2580;
        if (cp_width(ch) == 2 && fx) { /* wide glyphs keep order: put head at the mirrored left cell */ }
        cv_put(c, x + i, y + j, ch, tint ? tint : q->fg, q->bg, q->attr);
    }
}

/* ================================================================== entities */
typedef struct { char name[20]; KcValue v; } Prop;
typedef struct {
    bool used, visible, solid, physics, collide, face, bb; int gen, sprite, z, flip; double x, y, vx, vy, w, h, gscale, hp, ttl, bounce, drag, t0; uint32_t glyph; Col fg, bg; char tag[24]; bool ground, has_ttl;
    Prop *props; int np;
} Ent;
static Ent *ES; static int nes, capes, alive_n;
static Ent *en_get(int id) { int slot = (id & 0xFFFF) - 1, gen = id >> 16; if (slot < 0 || slot >= nes || !ES[slot].used || ES[slot].gen != gen) return NULL; return &ES[slot]; }
bool ent_alive(int id) { return en_get(id) != NULL; }
static Ent *en_must(int id, const char *who) { Ent *e = en_get(id); if (!e) as_err("%s: entity %d does not exist (despawned?)", who, id); return e; }
int game_entity_count(void) { return alive_n; }
static int ent_id(int slot) { return (ES[slot].gen << 16) | (slot + 1); }
int ent_spawn(double x, double y, const Opts *o) {
    int slot = -1; for (int i = 0; i < nes; i++) if (!ES[i].used) { slot = i; break; }
    if (slot < 0) { if (nes >= 65535) as_err("spawn: too many entities (max 65535)"); if (nes >= capes) { capes = capes ? capes * 2 : 64; ES = kc_realloc(ES, (size_t)capes * sizeof(Ent)); } memset(&ES[nes], 0, sizeof(Ent)); slot = nes++; }
    int gen = (ES[slot].gen + 1) & 0x7FFF; if (!gen) gen = 1; free(ES[slot].props); Ent *e = &ES[slot]; memset(e, 0, sizeof *e); e->gen = gen; e->used = true; e->visible = true; e->physics = true; e->collide = true; e->bb = true; e->gscale = 1; e->x = x; e->y = y; e->w = 1; e->h = 1; e->glyph = '@'; alive_n++;
    e->sprite = opt_int(o, "sprite", 0); e->t0 = T.time;
    if (e->sprite) { int sw, sh, sn; if (!sprite_valid(e->sprite)) as_err("spawn: invalid sprite %d", e->sprite); sprite_size(e->sprite, &sw, &sh, &sn); e->w = (sw + G_xscale - 1) / G_xscale; e->h = sh; }
    const char *g = opt_str(o, "glyph", opt_str(o, "char", NULL)); if (g && *g) { const char *q = g; e->glyph = utf8_next(&q); }
    e->fg = opt_col(o, "fg", 0); e->bg = opt_col(o, "bg", 0); e->vx = opt_num(o, "vx", 0); e->vy = opt_num(o, "vy", 0); e->w = opt_num(o, "w", e->w); e->h = opt_num(o, "h", e->h);
    snprintf(e->tag, sizeof e->tag, "%s", opt_str(o, "tag", "")); e->solid = opt_bool(o, "solid", false); e->z = opt_int(o, "z", 0); e->hp = opt_num(o, "hp", 1); e->gscale = opt_num(o, "gravity", 1);
    e->bounce = opt_num(o, "bounce", 0); e->drag = opt_num(o, "drag", 0); e->visible = opt_bool(o, "visible", true); e->physics = opt_bool(o, "physics", true); e->collide = opt_bool(o, "collide", true); e->face = opt_bool(o, "face", false); e->bb = opt_bool(o, "billboard", true);
    if (opt_has(o, "ttl")) { e->ttl = opt_num(o, "ttl", 1); e->has_ttl = true; }
    return ent_id(slot);
}
void ent_despawn(int id) { Ent *e = en_get(id); if (!e) return; free(e->props); e->props = NULL; e->np = 0; e->used = false; alive_n--; if (cam_follow == id) cam_follow = 0; }
void ent_clear(void) { for (int i = 0; i < nes; i++) if (ES[i].used) { free(ES[i].props); ES[i].used = false; } nes = 0; alive_n = 0; cam_follow = 0; }
void ent_set(int id, const char *k, KcValue v) {
    Ent *e = en_must(id, "set");
    #define NUM(f) do { e->f = V_num(v, k); return; } while (0)
    if (!strcmp(k, "x")) NUM(x); if (!strcmp(k, "y")) NUM(y); if (!strcmp(k, "vx")) NUM(vx); if (!strcmp(k, "vy")) NUM(vy); if (!strcmp(k, "w")) NUM(w); if (!strcmp(k, "h")) NUM(h); if (!strcmp(k, "hp")) NUM(hp);
    if (!strcmp(k, "bounce")) NUM(bounce); if (!strcmp(k, "drag")) NUM(drag); if (!strcmp(k, "gravity")) NUM(gscale);
    if (!strcmp(k, "ttl")) { e->ttl = V_num(v, k); e->has_ttl = true; return; }
    if (!strcmp(k, "z")) { e->z = (int)V_num(v, k); return; } if (!strcmp(k, "sprite")) { e->sprite = (int)V_num(v, k); e->t0 = T.time; return; }
    if (!strcmp(k, "visible")) { e->visible = kc_truthy(v); return; } if (!strcmp(k, "solid")) { e->solid = kc_truthy(v); return; } if (!strcmp(k, "physics")) { e->physics = kc_truthy(v); return; }
    if (!strcmp(k, "collide")) { e->collide = kc_truthy(v); return; } if (!strcmp(k, "face")) { e->face = kc_truthy(v); return; } if (!strcmp(k, "billboard")) { e->bb = kc_truthy(v); return; }
    if (!strcmp(k, "flip")) { e->flip = kc_truthy(v); return; } if (!strcmp(k, "fg")) { e->fg = col_from_value(v, k); return; } if (!strcmp(k, "bg")) { e->bg = col_from_value(v, k); return; }
    if (!strcmp(k, "tag")) { if (v.type != VAL_STRING) as_err("set: tag must be a string"); snprintf(e->tag, sizeof e->tag, "%s", v.as.s->data); return; }
    if (!strcmp(k, "glyph")) { if (v.type != VAL_STRING || !v.as.s->data[0]) as_err("set: glyph must be a non-empty string"); const char *q = v.as.s->data; e->glyph = utf8_next(&q); return; }
    for (int i = 0; i < e->np; i++) if (!strcmp(e->props[i].name, k)) { e->props[i].v = v; return; }
    e->props = kc_realloc(e->props, (size_t)(e->np + 1) * sizeof(Prop)); snprintf(e->props[e->np].name, sizeof e->props[e->np].name, "%s", k); e->props[e->np].v = v; e->np++;
    #undef NUM
}
KcValue ent_get(int id, const char *k) {
    Ent *e = en_must(id, "get");
    if (!strcmp(k, "x")) return kc_float(e->x); if (!strcmp(k, "y")) return kc_float(e->y); if (!strcmp(k, "vx")) return kc_float(e->vx); if (!strcmp(k, "vy")) return kc_float(e->vy); if (!strcmp(k, "w")) return kc_float(e->w); if (!strcmp(k, "h")) return kc_float(e->h);
    if (!strcmp(k, "hp")) return kc_float(e->hp); if (!strcmp(k, "tag")) return kc_string(e->tag); if (!strcmp(k, "z")) return kc_int(e->z); if (!strcmp(k, "sprite")) return kc_int(e->sprite); if (!strcmp(k, "visible")) return kc_bool(e->visible);
    if (!strcmp(k, "solid")) return kc_bool(e->solid); if (!strcmp(k, "on_ground")) return kc_bool(e->ground); if (!strcmp(k, "ttl")) return kc_float(e->ttl); if (!strcmp(k, "id")) return kc_int(id);
    if (!strcmp(k, "cx")) return kc_float(e->x + e->w / 2); if (!strcmp(k, "cy")) return kc_float(e->y + e->h / 2); if (!strcmp(k, "bounce")) return kc_float(e->bounce);
    for (int i = 0; i < e->np; i++) if (!strcmp(e->props[i].name, k)) return e->props[i].v;
    return kc_null();
}
static bool box_hits_tile(const Ent *e, double x, double y) {
    if (!G_map || !map_valid(G_map)) return false; int x0 = (int)floor(x + 1e-6), x1 = (int)floor(x + e->w - 1e-6), y0 = (int)floor(y + 1e-6), y1 = (int)floor(y + e->h - 1e-6);
    for (int j = y0; j <= y1; j++) for (int i = x0; i <= x1; i++) if (map_solid(G_map, i, j)) return true; return false;
}
static bool box_hits_ent(const Ent *e, double x, double y) {
    if (!e->solid) return false;
    for (int i = 0; i < nes; i++) { const Ent *o = &ES[i]; if (!o->used || o == e || !o->solid) continue; if (x < o->x + o->w - 1e-6 && x + e->w > o->x + 1e-6 && y < o->y + o->h - 1e-6 && y + e->h > o->y + 1e-6) return true; } return false;
}
static bool move_axis(Ent *e, double d, bool horiz) {
    if (d == 0) return true;
    if (!horiz == 0) { /* horizontal */ }
    double nx = e->x + (horiz ? d : 0), ny = e->y + (horiz ? 0 : d);
    bool hit = (e->collide && box_hits_tile(e, nx, ny)) || box_hits_ent(e, nx, ny);
    if (!hit) { e->x = nx; e->y = ny; return true; }
    /* push as close as possible: binary search along the step */
    double lo = 0, hi = 1; for (int i = 0; i < 12; i++) { double mid = (lo + hi) / 2, tx = e->x + (horiz ? d * mid : 0), ty = e->y + (horiz ? 0 : d * mid); bool h2 = (e->collide && box_hits_tile(e, tx, ty)) || box_hits_ent(e, tx, ty); if (h2) hi = mid; else lo = mid; }
    e->x += horiz ? d * lo : 0; e->y += horiz ? 0 : d * lo; return false;
}
bool ent_move(int id, double dx, double dy) { Ent *e = en_must(id, "move"); bool a = move_axis(e, dx, true), b = move_axis(e, dy, false); return a && b; }
void game_physics(double gravity, double drag) { g_gravity = gravity; g_drag = drag; }
static bool ent_matches(const Ent *e, const char *tag) { return !tag || !tag[0] || !strcmp(e->tag, tag); }
KcValue ent_query(int mode, double x, double y, double w, double h, const char *tag, int self) {
    KcValue arr = kc_array_new(); Ent *se = self ? en_get(self) : NULL;
    for (int i = 0; i < nes; i++) { Ent *e = &ES[i]; if (!e->used || e == se || !ent_matches(e, tag)) continue; bool ok;
        if (mode == 0) ok = x < e->x + e->w && x + w > e->x && y < e->y + e->h && y + h > e->y; else if (mode == 1) { double cx = e->x + e->w / 2 - x, cy = e->y + e->h / 2 - y; ok = hypot(cx / 1.0, cy) <= w; } else ok = true;
        if (ok) kc_array_push(arr, kc_int(ent_id(i))); }
    return arr;
}
KcValue ent_touching(int id, const char *tag) { Ent *e = en_must(id, "touching"); return ent_query(0, e->x, e->y, e->w, e->h, tag, id); }

/* ================================================================== particles */
typedef struct { float x, y, vx, vy, life, total, grav, drag; uint32_t ch[8]; int nch; Col c0, c1; bool screen; } Particle;
static Particle PA[4096]; static int npa;
void emit_particles(double x, double y, const Opts *o) {
    int n = opt_int(o, "count", 10); double sp = opt_num(o, "speed", 6), spread = opt_num(o, "spread", 360), ang = opt_num(o, "angle", 90), life = opt_num(o, "life", 0.8), grav = opt_num(o, "gravity", 0), drag = opt_num(o, "drag", 0.5);
    const char *gl = opt_str(o, "glyphs", "*+."); Col c0 = opt_col(o, "fg", col_rgb(255, 220, 120)), c1 = opt_col(o, "fg2", col_rgb(200, 60, 30)); bool screen = opt_bool(o, "screen", false);
    for (int i = 0; i < n && npa < 4096; i++) { Particle *p = &PA[npa++]; double a = (ang + (as_rand() - 0.5) * spread) * M_PI / 180.0, s = sp * (0.4 + as_rand() * 0.6);
        p->x = (float)x; p->y = (float)y; p->vx = (float)(cos(a) * s * (screen ? 1 : 1)); p->vy = (float)(-sin(a) * s * 0.55); p->life = p->total = (float)(life * (0.6 + as_rand() * 0.4)); p->grav = (float)grav; p->drag = (float)drag; p->c0 = c0; p->c1 = c1; p->screen = screen; p->nch = 0;
        const char *q = gl; while (*q && p->nch < 8) p->ch[p->nch++] = utf8_next(&q); if (!p->nch) { p->ch[0] = '*'; p->nch = 1; } }
}
static void particles_update(float dt) {
    for (int i = 0; i < npa;) { Particle *p = &PA[i]; p->life -= dt; if (p->life <= 0) { PA[i] = PA[--npa]; continue; } p->x += p->vx * dt; p->y += p->vy * dt; p->vy += p->grav * dt; float k = 1 - p->drag * dt; if (k < 0) k = 0; p->vx *= k; p->vy *= k; i++; }
}

/* ================================================================== camera */
void camera_set(double x, double y) { G_cam_x = x; G_cam_y = y; }
void camera_get(double *x, double *y) { *x = G_cam_x; *y = G_cam_y; }
void camera_follow(int id, double smooth, double w, double h) { if (id && !en_get(id)) as_err("camera_follow: entity %d does not exist", id); cam_follow = id; cam_smooth = smooth; dz_w = w; dz_h = h; }
void camera_shake(double amt, double secs) { shake_amt = amt; shake_t = shake_total = secs; }
void camera_bounds(int m) { cam_bounds_map = m; }
void viewport_set(int x, int y, int w, int h) { vp_x = x; vp_y = y; vp_w = w; vp_h = h; }
static void viewport_eff(int *x, int *y, int *w, int *h) { *x = vp_x; *y = vp_y; *w = vp_w > 0 ? vp_w : (S_screen ? S_screen->w : 80); *h = vp_h > 0 ? vp_h : (S_screen ? S_screen->h : 24); }
void viewport_get(int *x, int *y, int *w, int *h) { term_ensure(); viewport_eff(x, y, w, h); }
static void cam_base(double *bx, double *by, double *offx, double *offy) {
    int vx, vy, vw, vh; viewport_eff(&vx, &vy, &vw, &vh); int xs = G_xscale; double tw = (vw + xs - 1) / xs; double sx = 0, sy = 0;
    if (shake_t > 0) { double k = shake_amt * (shake_t / (shake_total > 0 ? shake_total : 1)); sx = (as_rand() * 2 - 1) * k; sy = (as_rand() * 2 - 1) * k * 0.5; }
    *bx = floor(G_cam_x + sx - tw / 2.0 + 0.5); *by = floor(G_cam_y + sy - vh / 2.0 + 0.5); if (offx) *offx = sx; if (offy) *offy = sy;
}
void world_to_screen(double wx, double wy, double *sx, double *sy) { term_ensure(); int vx, vy, vw, vh; viewport_eff(&vx, &vy, &vw, &vh); double bx, by; cam_base(&bx, &by, NULL, NULL); *sx = vx + (wx - bx) * G_xscale; *sy = vy + (wy - by); }
void screen_to_world(double sx, double sy, double *wx, double *wy) { term_ensure(); int vx, vy, vw, vh; viewport_eff(&vx, &vy, &vw, &vh); double bx, by; cam_base(&bx, &by, NULL, NULL); *wx = (sx - vx) / G_xscale + bx; *wy = (sy - vy) + by; }

/* ================================================================== update / draw */
void game_update(float dt) {
    if (shake_t > 0) shake_t -= dt;
    for (int i = 0; i < nes; i++) { Ent *e = &ES[i]; if (!e->used) continue;
        if (e->has_ttl) { e->ttl -= dt; if (e->ttl <= 0) { ent_despawn(ent_id(i)); continue; } }
        if (!e->physics) continue;
        e->vy += g_gravity * e->gscale * dt; double dr = e->drag > 0 ? e->drag : g_drag; if (dr > 0) { double k = 1 - dr * dt; if (k < 0) k = 0; e->vx *= k; if (g_gravity == 0) e->vy *= k; }
        if (e->face && fabs(e->vx) > 0.05) e->flip = e->vx < 0;
        bool xok = move_axis(e, e->vx * dt, true); if (!xok) e->vx = e->bounce > 0 ? -e->vx * e->bounce : 0;
        bool was_ground = e->ground; e->ground = false; double dy = e->vy * dt; bool yok = move_axis(e, dy, false);
        if (!yok) { if (dy > 0) e->ground = true; e->vy = (fabs(e->vy) > 2 && e->bounce > 0) ? -e->vy * e->bounce : 0; if (e->bounce > 0 && fabs(e->vy) > 0) e->ground = false; }
        else if (g_gravity > 0 && was_ground && dy == 0) e->ground = true;
        if (g_gravity > 0 && !e->ground && e->vy == 0 && e->collide && box_hits_tile(e, e->x, e->y + 0.02)) e->ground = true;
    }
    particles_update(dt);
    if (cam_follow) { Ent *f = en_get(cam_follow); if (f) { double tx = f->x + f->w / 2, ty = f->y + f->h / 2;
        if (dz_w > 0 || dz_h > 0) { double ddx = tx - G_cam_x, ddy = ty - G_cam_y; if (fabs(ddx) <= dz_w / 2) tx = G_cam_x; else tx -= (ddx > 0 ? 1 : -1) * dz_w / 2; if (fabs(ddy) <= dz_h / 2) ty = G_cam_y; else ty -= (ddy > 0 ? 1 : -1) * dz_h / 2; }
        double k = cam_smooth > 0 ? 1 - exp(-cam_smooth * dt) : 1; G_cam_x += (tx - G_cam_x) * k; G_cam_y += (ty - G_cam_y) * k; } else cam_follow = 0; }
    if (cam_bounds_map && map_valid(cam_bounds_map)) { int mw, mh; if (map_size(cam_bounds_map, &mw, &mh)) { int vx, vy, vw, vh; viewport_eff(&vx, &vy, &vw, &vh); double hw = (double)vw / G_xscale / 2, hh = (double)vh / 2;
        G_cam_x = mw <= hw * 2 ? mw / 2.0 : as_clampf((float)G_cam_x, (float)hw, (float)(mw - hw)); G_cam_y = mh <= hh * 2 ? mh / 2.0 : as_clampf((float)G_cam_y, (float)hh, (float)(mh - hh)); } }
    if (G_map && map_valid(G_map)) map_focus(G_map, G_cam_x, G_cam_y);
}
static int cmp_ent(const void *a, const void *b) { const Ent *x = &ES[*(const int *)a], *y = &ES[*(const int *)b]; if (x->z != y->z) return x->z - y->z; return x->y < y->y ? -1 : x->y > y->y; }
void game_draw_world(Canvas *c, const Opts *o) {
    int vx, vy, vw, vh; viewport_eff(&vx, &vy, &vw, &vh); int sx0 = c->cx0, sy0 = c->cy0, sx1 = c->cx1, sy1 = c->cy1; cv_set_clip(c, vx, vy, vw, vh);
    int mid = opt_int(o, "map", G_map);
    if (mid && map_valid(mid) && opt_bool(o, "tiles", true)) { MapDraw d = { vx, vy, vw, vh, G_cam_x, G_cam_y, G_xscale, (float)opt_num(o, "fog", 0), opt_bool(o, "fov", false), opt_col(o, "fog_color", 0) };
        double bx, by, ox, oy; cam_base(&bx, &by, &ox, &oy); d.camx += ox; d.camy += oy; map_draw(mid, c, &d); }
    if (opt_bool(o, "entities", true)) {
        int *ord = kc_malloc((size_t)(nes ? nes : 1) * sizeof(int)); int n = 0; for (int i = 0; i < nes; i++) if (ES[i].used && ES[i].visible) ord[n++] = i; qsort(ord, (size_t)n, sizeof(int), cmp_ent);
        for (int k = 0; k < n; k++) { Ent *e = &ES[ord[k]]; double sx, sy; world_to_screen(e->x, e->y, &sx, &sy); int px = (int)floor(sx + 0.5), py = (int)floor(sy + 0.5);
            if (e->sprite && sprite_valid(e->sprite)) sprite_draw(e->sprite, c, px, py, -1, e->flip != 0, false, e->fg, e->t0);
            else { cv_put(c, px, py, e->glyph, e->fg ? e->fg : col_rgb(255, 255, 255), e->bg, 0); } }
        free(ord);
    }
    if (opt_bool(o, "particles", true)) for (int i = 0; i < npa; i++) { Particle *p = &PA[i]; double sx = p->x, sy = p->y; if (!p->screen) world_to_screen(p->x, p->y, &sx, &sy); float f = 1 - p->life / p->total; int ci = as_clampi((int)(f * (float)p->nch), 0, p->nch - 1); cv_put(c, (int)floor(sx + 0.5), (int)floor(sy + 0.5), p->ch[ci], col_mix(p->c0, p->c1, f), 0, 0); }
    c->cx0 = sx0; c->cy0 = sy0; c->cx1 = sx1; c->cy1 = sy1;
}
int ent_visible_billboards(double px, double py, double ang, double fov, int vw, double maxd, int cap, double *dist, int *col, uint32_t *ch, Col *fg, int *hscale) {
    int n = 0; double dx = cos(ang), dy = sin(ang), plx = -dy * tan(fov / 2), ply = dx * tan(fov / 2);
    for (int i = 0; i < nes && n < cap; i++) { Ent *e = &ES[i]; if (!e->used || !e->visible || !e->bb) continue; double ex = e->x + e->w / 2 - px, ey = e->y + e->h / 2 - py, inv = 1.0 / (plx * dy - dx * ply), tx = inv * (dy * ex - dx * ey), ty = inv * (-ply * ex + plx * ey);
        if (ty <= 0.1 || ty > maxd) continue; int sx = (int)((vw / 2.0) * (1 + tx / ty)); if (sx < -vw || sx > 2 * vw) continue; dist[n] = ty; col[n] = sx; ch[n] = e->glyph; fg[n] = e->fg; hscale[n] = 1;
        if (e->sprite && sprite_valid(e->sprite)) { int w = 0, h = 0; Cell *cells = sprite_cells(e->sprite, 0, &w, &h); for (int k = 0; cells && k < w * h; k++) if (cells[k].ch != ' ' && cells[k].ch != CH_CONT) { ch[n] = cells[k].ch; if (!fg[n]) fg[n] = cells[k].fg; break; } hscale[n] = h > 1 ? (h + 1) / 2 : 1; }
        n++; }
    for (int a = 1; a < n; a++) { double d = dist[a]; int c = col[a]; uint32_t g = ch[a]; Col f = fg[a]; int hs = hscale[a]; int b = a - 1; while (b >= 0 && dist[b] < d) { dist[b + 1] = dist[b]; col[b + 1] = col[b]; ch[b + 1] = ch[b]; fg[b + 1] = fg[b]; hscale[b + 1] = hscale[b]; b--; } dist[b + 1] = d; col[b + 1] = c; ch[b + 1] = g; fg[b + 1] = f; hscale[b + 1] = hs; }
    return n;
}

/* ================================================================== controller */
typedef struct { char name[24]; int codes[10]; int n; } Action; typedef struct { char name[24], neg[24], pos[24]; } Axis;
static Action ACT[96]; static int nact; static Axis AX[24]; static int nax;
static int bind_code(KcValue v) {
    if (v.type != VAL_STRING) as_err("bind: keys are given by name, e.g. \"left\", \"w\", \"space\", \"mouse_left\"");
    const char *s = v.as.s->data; if (!strcmp(s, "mouse_left")) return 1001; if (!strcmp(s, "mouse_middle")) return 1002; if (!strcmp(s, "mouse_right")) return 1003; if (!strcmp(s, "wheel_up")) return 1010; if (!strcmp(s, "wheel_down")) return 1011;
    int k = key_code(s); if (k < 0) as_err("bind: unknown key \"%s\"", s); return k;
}
static Action *act_find(const char *n) { for (int i = 0; i < nact; i++) if (!strcmp(ACT[i].name, n)) return &ACT[i]; return NULL; }
void bind_action(const char *name, KcValue *keys, int n) {
    Action *a = act_find(name); if (!a) { if (nact >= 96) as_err("bind: too many actions (max 96)"); a = &ACT[nact++]; snprintf(a->name, sizeof a->name, "%s", name); a->n = 0; }
    for (int i = 0; i < n && a->n < 10; i++) { int c = bind_code(keys[i]); bool dup = false; for (int k = 0; k < a->n; k++) if (a->codes[k] == c) dup = true; if (!dup) a->codes[a->n++] = c; }
}
void unbind_action(const char *name) { for (int i = 0; i < nact; i++) if (!strcmp(ACT[i].name, name)) { ACT[i] = ACT[--nact]; return; } }
void bind_axis(const char *name, const char *neg, const char *pos) { for (int i = 0; i < nax; i++) if (!strcmp(AX[i].name, name)) { snprintf(AX[i].neg, 24, "%s", neg); snprintf(AX[i].pos, 24, "%s", pos); return; } if (nax >= 24) as_err("bind_axis: too many axes"); snprintf(AX[nax].name, 24, "%s", name); snprintf(AX[nax].neg, 24, "%s", neg); snprintf(AX[nax].pos, 24, "%s", pos); nax++; }
static void bind_s(const char *name, const char *k1, const char *k2, const char *k3) { KcValue v[3]; int n = 0; if (k1) v[n++] = kc_string(k1); if (k2) v[n++] = kc_string(k2); if (k3) v[n++] = kc_string(k3); bind_action(name, v, n); }
void controller_preset(const char *p) {
    bool arrows = !strcmp(p, "arrows") || !strcmp(p, "both"), wasd = !strcmp(p, "wasd") || !strcmp(p, "both"), vim = !strcmp(p, "vim") || !strcmp(p, "both");
    if (!arrows && !wasd && !vim) as_err("controller: unknown preset \"%s\" (arrows, wasd, vim, both)", p);
    nact = 0; nax = 0;
    const char *dirs[4] = { "up", "down", "left", "right" }, *wk[4] = { "w", "s", "a", "d" }, *vk[4] = { "k", "j", "h", "l" };
    for (int i = 0; i < 4; i++) { bind_s(dirs[i], arrows ? dirs[i] : NULL, wasd ? wk[i] : NULL, vim ? vk[i] : NULL); }
    bind_s("confirm", "enter", "space", NULL); bind_s("cancel", "escape", "q", NULL); bind_s("pause", "p", NULL, NULL); bind_s("fire", "space", "mouse_left", wasd ? "j" : "z"); bind_s("jump", "space", wasd ? "w" : "up", NULL); bind_s("alt", "x", "mouse_right", NULL); bind_s("inventory", "i", "tab", NULL);
    bind_axis("move_x", "left", "right"); bind_axis("move_y", "up", "down");
}
bool action_state(const char *name, int mode) {
    if (hud_input_focused()) return false; Action *a = act_find(name); if (!a) as_err("action: \"%s\" is not bound (use bind() or controller())", name);
    for (int i = 0; i < a->n; i++) { int c = a->codes[i];
        if (c >= 1001 && c <= 1003) { int b = c - 1000; if (mode == 0 ? term_mouse_down(b) : mode == 1 || mode == 3 ? term_mouse_pressed(b) : term_mouse_released(b)) return true; }
        else if (c == 1010) { if (mode != 2 && T.wheel_y > 0) return true; } else if (c == 1011) { if (mode != 2 && T.wheel_y < 0) return true; }
        else if (mode == 0 ? term_key(c) : mode == 1 ? term_key_pressed(c) : mode == 2 ? term_key_released(c) : term_key_repeat(c)) return true; }
    return false;
}
double axis_value(const char *name) {
    for (int i = 0; i < nax; i++) if (!strcmp(AX[i].name, name)) { Action *n = act_find(AX[i].neg), *p = act_find(AX[i].pos); double v = 0; if (n && action_state(AX[i].neg, 0)) v -= 1; if (p && action_state(AX[i].pos, 0)) v += 1; return v; }
    as_err("axis: \"%s\" is not defined (use bind_axis(name, negative_action, positive_action) or controller())", name);
}
KcValue actions_list(void) { KcValue a = kc_array_new(); for (int i = 0; i < nact; i++) kc_array_push(a, kc_string(ACT[i].name)); return a; }
void game_reset(void) {
    ent_clear(); npa = 0; G_cam_x = G_cam_y = 0; shake_t = 0; cam_follow = 0; cam_bounds_map = 0; vp_x = vp_y = vp_w = vp_h = 0; g_gravity = g_drag = 0; G_xscale = 1; nact = nax = 0;
    for (int i = 1; i < nsp; i++) if (SP[i].used) sprite_free(i);
}
