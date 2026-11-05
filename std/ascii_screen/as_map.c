/* std.ascii_screen — tile maps and streamed worlds.
 *
 *  fixed maps    w x h grid of tile ids (0..255), built from text rows, filled, or generated (dungeon, cave, terrain)
 *  streamed      infinite worlds made of 32x32 chunks generated on demand from a seed (flat, terrain, caves, noise);
 *  worlds        only chunks near the focus stay loaded, edited chunks are kept, everything else is re-generated
 *                deterministically — memory stays bounded however far you walk.
 *  extras        distance fog, field of view with explored-memory, line of sight, A* path finding, DDA raycaster.
 */
#include "as_internal.h"

#define CS 32
typedef struct Chunk { int cx, cy, used_at; bool mod; uint8_t t[CS * CS]; struct Chunk *next; } Chunk;
typedef struct {
    bool used, infinite; int w, h; uint8_t *t, *seen; int gen; uint32_t seed; int radius; Chunk **hash; int hcap, nchunks, stamp; double fx, fy;
    uint8_t *vis; int vx0, vy0, vr; int spawn_x, spawn_y; bool has_spawn;
} Map;
static Map *MAPS; static int nmaps, capmaps; int G_map = 0; int G_xscale = 1;
static TileDef TD[256];

TileDef *tile_def_get(int id) { static TileDef u; if (id < 0 || id > 255) { u.ch = '?'; u.defined = false; return &u; } return &TD[id]; }
void tile_define(int id, uint32_t ch, Col fg, Col bg, bool solid, bool opaque, int flags, const char *name) {
    if (id < 0 || id > 255) as_err("tile ids must be 0..255 (got %d)", id);
    TileDef *t = &TD[id]; t->ch = ch ? ch : ' '; t->fg = fg; t->bg = bg; t->solid = solid; t->opaque = opaque; t->flags = flags; t->defined = true; snprintf(t->name, sizeof t->name, "%s", name ? name : "");
}
int tile_id_of(KcValue v, const char *who) {
    if (v.type == VAL_STRING) { const char *s = v.as.s->data; if (!s[0] || s[1]) as_err("%s: a tile given as a string must be exactly one ASCII character", who); return (unsigned char)s[0]; }
    int id = (int)V_num(v, who); if (id < 0 || id > 255) as_err("%s: tile id %d out of range 0..255", who, id); return id;
}
static void def_if(int id, uint32_t ch, Col fg, bool solid, bool opaque, const char *name) { if (!TD[id].defined) tile_define(id, ch, fg, 0, solid, opaque, 0, name); }
static void tiles_defaults(void) {
    static bool done = false; if (done) return; done = true;
    def_if('#', 0x2588, col_rgb(130, 130, 145), true, true, "wall"); def_if('.', 0xB7, col_rgb(90, 90, 100), false, false, "floor"); def_if('+', '+', col_rgb(200, 150, 70), false, true, "door");
    def_if('~', '~', col_rgb(70, 130, 230), false, false, "water"); def_if('"', '"', col_rgb(80, 180, 90), false, false, "grass"); def_if('T', 0x2663, col_rgb(40, 140, 60), true, true, "tree");
    def_if(1, 0x2248, col_rgb(30, 70, 170), false, false, "deep water"); def_if(2, '~', col_rgb(70, 130, 230), false, false, "water"); def_if(3, '.', col_rgb(230, 210, 140), false, false, "sand"); def_if(4, '"', col_rgb(80, 180, 90), false, false, "grass");
    def_if(5, 0x2663, col_rgb(40, 130, 60), false, true, "forest"); def_if(6, '^', col_rgb(150, 130, 100), false, false, "hills"); def_if(7, 0x25B2, col_rgb(170, 170, 180), true, true, "mountain"); def_if(8, '*', col_rgb(240, 245, 255), true, true, "snow");
    def_if(10, 0x2593, col_rgb(120, 115, 125), true, true, "rock"); def_if(11, '$', col_rgb(255, 210, 80), true, true, "ore"); def_if(12, 0x2592, col_rgb(140, 100, 60), true, true, "dirt"); def_if(13, 0x2580, col_rgb(80, 190, 90), true, true, "grass block");
}

/* ------------------------------------------------------------ generators */
enum { GEN_FLAT, GEN_TERRAIN, GEN_CAVES, GEN_NOISE };
static int gen_tile(int gen, uint32_t seed, int x, int y) {
    switch (gen) {
        case GEN_FLAT: { if (y < 0) return 0; if (y == 0) return 13; float hill = as_fbm((float)x * 0.04f, 3.3f, seed, 3); int top = (int)(hill * 8) - 4; if (y < top + 1 && y > 0) return 0; if (y == top + 1 && top + 1 > 0) return 13; if (y < 5 + top / 2) return 12; uint32_t hh = as_hash2(x, y, seed); return (hh % 90 == 0) ? 11 : 10; }
        case GEN_TERRAIN: { float e = as_fbm((float)x * 0.03f, (float)y * 0.03f, seed, 5), f = as_fbm((float)x * 0.09f + 40, (float)y * 0.09f + 40, seed + 9, 3);
            if (e < 0.36f) return 1; if (e < 0.43f) return 2; if (e < 0.47f) return 3; if (e < 0.63f) return f > 0.58f ? 5 : 4; if (e < 0.72f) return 6; if (e < 0.80f) return 7; return 8; }
        case GEN_CAVES: { float n = as_fbm((float)x * 0.07f, (float)y * 0.07f, seed, 4); if (x * x + y * y < 36) return 0; if (n > 0.54f) return (as_hash2(x, y, seed) % 120 == 0) ? 11 : 10; return 0; }
        default: return 0;
    }
}
static const char *GEN_NAMES[] = { "flat", "terrain", "caves", "noise", NULL };

/* ------------------------------------------------------------ chunks */
static Map *mp(int id) { return (id < 1 || id >= nmaps || !MAPS[id].used) ? NULL : &MAPS[id]; }
bool map_valid(int m) { return mp(m) != NULL; }
static Map *mmust(int id) { Map *m = mp(id); if (!m) as_err("invalid map %d", id); return m; }
static int mnew(void) {
    int id = -1; for (int i = 1; i < nmaps; i++) if (!MAPS[i].used) { id = i; break; }
    if (id < 0) { if (nmaps == 0) nmaps = 1; if (nmaps >= capmaps) { capmaps = capmaps ? capmaps * 2 : 8; MAPS = kc_realloc(MAPS, (size_t)capmaps * sizeof(Map)); } id = nmaps++; }
    memset(&MAPS[id], 0, sizeof(Map)); MAPS[id].used = true; tiles_defaults(); if (!G_map) G_map = id; return id;
}
static int fldiv(int a, int b) { int q = a / b; return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q; }
static Chunk *chunk_get(Map *m, int cx, int cy, bool create) {
    uint32_t h = (uint32_t)(cx * 73856093) ^ (uint32_t)(cy * 19349663); size_t b = h % (uint32_t)m->hcap;
    for (Chunk *c = m->hash[b]; c; c = c->next) if (c->cx == cx && c->cy == cy) { c->used_at = m->stamp; return c; }
    if (!create) return NULL;
    Chunk *c = kc_malloc(sizeof(Chunk)); c->cx = cx; c->cy = cy; c->mod = false; c->used_at = m->stamp;
    for (int j = 0; j < CS; j++) for (int i = 0; i < CS; i++) c->t[j * CS + i] = (uint8_t)gen_tile(m->gen, m->seed, cx * CS + i, cy * CS + j);
    c->next = m->hash[b]; m->hash[b] = c; m->nchunks++; return c;
}
int map_new_fixed(int w, int h, int fill) {
    if (w < 1 || h < 1 || (int64_t)w * h > 16000000) as_err("map: bad size %dx%d", w, h);
    int id = mnew(); Map *m = &MAPS[id]; m->w = w; m->h = h; m->t = kc_malloc((size_t)w * h); memset(m->t, fill, (size_t)w * h); m->seen = kc_malloc((size_t)w * h); memset(m->seen, 0, (size_t)w * h); return id;
}
int map_new_world(const char *gen, uint32_t seed, int radius) {
    int g = -1; for (int i = 0; GEN_NAMES[i]; i++) if (!strcmp(GEN_NAMES[i], gen)) g = i;
    if (g < 0) as_err("world: unknown generator \"%s\" (flat, terrain, caves, noise)", gen);
    int id = mnew(); Map *m = &MAPS[id]; m->infinite = true; m->gen = g; m->seed = seed; m->radius = radius < 1 ? 3 : radius > 16 ? 16 : radius; m->hcap = 1031; m->hash = kc_malloc((size_t)m->hcap * sizeof(Chunk *)); memset(m->hash, 0, (size_t)m->hcap * sizeof(Chunk *));
    m->has_spawn = true; m->spawn_x = 0; m->spawn_y = g == GEN_FLAT ? -2 : 0;
    if (g == GEN_TERRAIN) { for (int r = 0; r < 200; r++) { bool ok = false; for (int a = 0; a < 16 && !ok; a++) { int x = (a % 4 - 2) * r, y = (a / 4 - 2) * r; int t = gen_tile(g, seed, x, y); if (t >= 3 && t <= 4) { m->spawn_x = x; m->spawn_y = y; ok = true; } } if (ok) break; } }
    return id;
}
int map_from_rows(KcValue rows) {
    if (rows.type != VAL_ARRAY || rows.as.arr->count < 1) as_err("map: expected an array of row strings");
    int h = rows.as.arr->count, w = 0; for (int i = 0; i < h; i++) { if (rows.as.arr->items[i].type != VAL_STRING) as_err("map: row %d is not a string", i); int l = (int)strlen(rows.as.arr->items[i].as.s->data); if (l > w) w = l; }
    int id = map_new_fixed(w ? w : 1, h, 0); Map *m = &MAPS[id];
    for (int y = 0; y < h; y++) { const char *r = rows.as.arr->items[y].as.s->data; for (int x = 0; r[x]; x++) { unsigned char ch = (unsigned char)r[x]; m->t[(size_t)y * w + x] = ch == ' ' ? 0 : ch; if (ch == '@' && !m->has_spawn) { m->has_spawn = true; m->spawn_x = x; m->spawn_y = y; m->t[(size_t)y * w + x] = '.'; } } }
    return id;
}
void map_free(int id) {
    Map *m = mp(id); if (!m) return; free(m->t); free(m->seen); free(m->vis);
    if (m->hash) for (int i = 0; i < m->hcap; i++) { Chunk *c = m->hash[i]; while (c) { Chunk *n = c->next; free(c); c = n; } } free(m->hash);
    m->used = false; if (G_map == id) G_map = 0;
}
void maps_free_all(void) { for (int i = 1; i < nmaps; i++) if (MAPS[i].used) map_free(i); free(MAPS); MAPS = NULL; nmaps = capmaps = 0; G_map = 0; }
int map_tile(int id, int x, int y) {
    Map *m = mp(id); if (!m) return 0;
    if (!m->infinite) return (x < 0 || y < 0 || x >= m->w || y >= m->h) ? 0 : m->t[(size_t)y * m->w + x];
    Chunk *c = chunk_get(m, fldiv(x, CS), fldiv(y, CS), true); return c->t[(y - c->cy * CS) * CS + (x - c->cx * CS)];
}
void map_set(int id, int x, int y, int t) {
    Map *m = mmust(id); if (t < 0 || t > 255) as_err("map_set: tile id out of range"); if (!m->infinite) { if (x >= 0 && y >= 0 && x < m->w && y < m->h) m->t[(size_t)y * m->w + x] = (uint8_t)t; return; }
    Chunk *c = chunk_get(m, fldiv(x, CS), fldiv(y, CS), true); c->t[(y - c->cy * CS) * CS + (x - c->cx * CS)] = (uint8_t)t; c->mod = true;
}
bool map_solid(int id, int x, int y) { Map *m = mp(id); if (!m) return false; if (!m->infinite && (x < 0 || y < 0 || x >= m->w || y >= m->h)) return true; return TD[map_tile(id, x, y)].solid; }
bool map_opaque(int id, int x, int y) { Map *m = mp(id); if (!m) return false; if (!m->infinite && (x < 0 || y < 0 || x >= m->w || y >= m->h)) return true; return TD[map_tile(id, x, y)].opaque; }
bool map_size(int id, int *w, int *h) { Map *m = mmust(id); if (m->infinite) { *w = *h = 0; return false; } *w = m->w; *h = m->h; return true; }
bool map_spawn(int id, int *x, int *y) { Map *m = mmust(id); if (!m->has_spawn) return false; *x = m->spawn_x; *y = m->spawn_y; return true; }
void map_stats(int id, int *loaded, int *modified, bool *inf) {
    Map *m = mmust(id); *inf = m->infinite; *loaded = m->infinite ? m->nchunks : 1; int mod = 0;
    if (m->infinite) for (int i = 0; i < m->hcap; i++) for (Chunk *c = m->hash[i]; c; c = c->next) if (c->mod) mod++; *modified = mod;
}
void map_focus(int id, double x, double y) {
    Map *m = mmust(id); if (!m->infinite) return; m->fx = x; m->fy = y; m->stamp++;
    int ccx = fldiv((int)floor(x), CS), ccy = fldiv((int)floor(y), CS), R = m->radius;
    for (int j = -R; j <= R; j++) for (int i = -R; i <= R; i++) chunk_get(m, ccx + i, ccy + j, true);
    int keep = R + 2;                                                              /* evict unmodified chunks beyond the keep radius */
    for (int b = 0; b < m->hcap; b++) { Chunk **pp = &m->hash[b]; while (*pp) { Chunk *c = *pp; if (!c->mod && (abs(c->cx - ccx) > keep || abs(c->cy - ccy) > keep)) { *pp = c->next; free(c); m->nchunks--; } else pp = &c->next; } }
}
void map_update(double dt) { (void)dt; }
void map_fill_rect(int id, int x, int y, int w, int h, int t) { for (int j = y; j < y + h; j++) for (int i = x; i < x + w; i++) map_set(id, i, j, t); }

/* fixed-map generators */
void map_generate(int id, const char *kind, uint32_t seed) {
    Map *m = mmust(id); if (m->infinite) as_err("map_generate: only fixed maps can be regenerated (worlds are generated on demand)");
    as_seed(seed); int W = m->w, H = m->h;
    if (!strcmp(kind, "dungeon")) {
        memset(m->t, '#', (size_t)W * H); int nr = 0, rx[64], ry[64], maxr = 6 + (W * H) / 400; if (maxr > 60) maxr = 60;
        for (int tries = 0; tries < 400 && nr < maxr; tries++) {
            int w = 4 + (int)(as_rand() * 9), h = 3 + (int)(as_rand() * 5), x = 1 + (int)(as_rand() * (float)(W - w - 2 > 0 ? W - w - 2 : 1)), y = 1 + (int)(as_rand() * (float)(H - h - 2 > 0 ? H - h - 2 : 1)); bool ok = x + w < W - 1 && y + h < H - 1;
            for (int i = 0; i < nr && ok; i++) { int ox = rx[i], oy = ry[i]; if (abs(x + w / 2 - ox) < w / 2 + 6 && abs(y + h / 2 - oy) < h / 2 + 4) ok = false; } if (!ok) continue;
            for (int j = y; j < y + h; j++) for (int i = x; i < x + w; i++) m->t[(size_t)j * W + i] = '.';
            if (nr > 0) { int cx = x + w / 2, cy = y + h / 2, px = rx[nr - 1], py = ry[nr - 1]; int sx = cx < px ? 1 : -1; for (int i = cx; i != px; i += sx) m->t[(size_t)cy * W + i] = '.'; int sy = cy < py ? 1 : -1; for (int j = cy; j != py + sy; j += sy) m->t[(size_t)j * W + px] = '.'; }
            rx[nr] = x + w / 2; ry[nr] = y + h / 2; nr++;
        }
        if (nr) { m->has_spawn = true; m->spawn_x = rx[0]; m->spawn_y = ry[0]; }
    } else if (!strcmp(kind, "cave")) {
        for (size_t i = 0; i < (size_t)W * H; i++) m->t[i] = as_rand() < 0.45f ? '#' : '.';
        uint8_t *tmp = kc_malloc((size_t)W * H);
        for (int it = 0; it < 5; it++) { for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) { int n = 0; for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) { int xx = x + dx, yy = y + dy; if (xx < 0 || yy < 0 || xx >= W || yy >= H || m->t[(size_t)yy * W + xx] == '#') n++; } tmp[(size_t)y * W + x] = n >= 5 ? '#' : '.'; } memcpy(m->t, tmp, (size_t)W * H); }
        free(tmp); for (int tries = 0; tries < 10000; tries++) { int x = (int)(as_rand() * (float)W), y = (int)(as_rand() * (float)H); if (m->t[(size_t)y * W + x] == '.') { m->has_spawn = true; m->spawn_x = x; m->spawn_y = y; break; } }
    } else if (!strcmp(kind, "terrain")) { for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) m->t[(size_t)y * W + x] = (uint8_t)gen_tile(GEN_TERRAIN, seed, x, y); }
    else as_err("map_generate: unknown kind \"%s\" (dungeon, cave, terrain)", kind);
    memset(m->seen, 0, (size_t)W * H);
}

/* ------------------------------------------------------------ fov / los / path */
bool map_los(int id, int x0, int y0, int x1, int y1) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1, err = dx + dy;
    while (1) { if ((x0 != x1 || y0 != y1) && !(x0 == x1 && y0 == y1) && map_opaque(id, x0, y0) && !(x0 == x1 && y0 == y1)) { /* blocked unless this is the start */ } if (x0 == x1 && y0 == y1) return true; int e2 = 2 * err; if (e2 >= dy) { err += dy; x0 += sx; } if (e2 <= dx) { err += dx; y0 += sy; } if (!(x0 == x1 && y0 == y1) && map_opaque(id, x0, y0)) return false; }
}
void map_fov(int id, double fx, double fy, int r) {
    Map *m = mmust(id); if (r < 1) r = 1; if (r > 60) r = 60; int n = 2 * r + 1; free(m->vis); m->vis = kc_malloc((size_t)n * n); memset(m->vis, 0, (size_t)n * n);
    int cx = (int)floor(fx), cy = (int)floor(fy); m->vx0 = cx - r; m->vy0 = cy - r; m->vr = r;
    for (int j = -r; j <= r; j++) for (int i = -r; i <= r; i++) {
        double d = hypot((double)i, (double)j * 1.0); if (d > r + 0.5) continue; int x = cx + i, y = cy + j;
        if (map_los(id, cx, cy, x, y)) { m->vis[(size_t)(j + r) * n + (i + r)] = 1; if (!m->infinite && x >= 0 && y >= 0 && x < m->w && y < m->h) m->seen[(size_t)y * m->w + x] = 1; }
    }
}
static bool vis_at(Map *m, int x, int y) { if (!m->vis) return false; int n = 2 * m->vr + 1, i = x - m->vx0, j = y - m->vy0; return i >= 0 && j >= 0 && i < n && j < n && m->vis[(size_t)j * n + i]; }
typedef struct { int idx; float f; } HN;
int map_path(int id, int x0, int y0, int x1, int y1, int **out) {
    Map *m = mmust(id); (void)m; int pad = 24, minx = (x0 < x1 ? x0 : x1) - pad, maxx = (x0 > x1 ? x0 : x1) + pad, miny = (y0 < y1 ? y0 : y1) - pad, maxy = (y0 > y1 ? y0 : y1) + pad;
    if (maxx - minx > 220) { minx = x0 - 110; maxx = x0 + 110; } if (maxy - miny > 220) { miny = y0 - 110; maxy = y0 + 110; }
    int W = maxx - minx + 1, H = maxy - miny + 1; if (x1 < minx || x1 > maxx || y1 < miny || y1 > maxy) return 0;
    size_t N = (size_t)W * H; float *g = kc_malloc(N * sizeof(float)); int *par = kc_malloc(N * sizeof(int)); uint8_t *closed = kc_malloc(N); for (size_t i = 0; i < N; i++) { g[i] = 1e30f; par[i] = -1; closed[i] = 0; }
    HN *hp = kc_malloc(N * 4 * sizeof(HN)); size_t hn = 0;
    #define HPUSH(ix, ff) do { size_t i_ = hn++; hp[i_].idx = (ix); hp[i_].f = (ff); while (i_ > 0) { size_t p_ = (i_ - 1) / 2; if (hp[p_].f <= hp[i_].f) break; HN t_ = hp[p_]; hp[p_] = hp[i_]; hp[i_] = t_; i_ = p_; } } while (0)
    int s = (y0 - miny) * W + (x0 - minx), gl = (y1 - miny) * W + (x1 - minx); g[s] = 0; HPUSH(s, 0); int found = 0;
    while (hn) {
        HN top = hp[0]; hp[0] = hp[--hn]; size_t i = 0; while (1) { size_t l = 2 * i + 1, r = l + 1, sm = i; if (l < hn && hp[l].f < hp[sm].f) sm = l; if (r < hn && hp[r].f < hp[sm].f) sm = r; if (sm == i) break; HN t = hp[sm]; hp[sm] = hp[i]; hp[i] = t; i = sm; }
        int cur = top.idx; if (closed[cur]) continue; closed[cur] = 1; if (cur == gl) { found = 1; break; }
        int cx = cur % W + minx, cy = cur / W + miny; static const int D[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
        for (int k = 0; k < 4; k++) { int nx = cx + D[k][0], ny = cy + D[k][1]; if (nx < minx || nx > maxx || ny < miny || ny > maxy) continue; int ni = (ny - miny) * W + (nx - minx); if (closed[ni]) continue; if (ni != gl && map_solid(id, nx, ny)) continue; float ng = g[cur] + 1; if (ng < g[ni]) { g[ni] = ng; par[ni] = cur; float hh = (float)(abs(nx - x1) + abs(ny - y1)); if (hn + 1 < N * 4) HPUSH(ni, ng + hh); } }
    }
    #undef HPUSH
    int cnt = 0; if (found) { for (int c = gl; c >= 0; c = par[c]) cnt++; *out = kc_malloc((size_t)cnt * 2 * sizeof(int)); int k = cnt - 1; for (int c = gl; c >= 0; c = par[c], k--) { (*out)[k * 2] = c % W + minx; (*out)[k * 2 + 1] = c / W + miny; } }
    free(g); free(par); free(closed); free(hp); return cnt;
}

/* ------------------------------------------------------------ draw */
static float smooth01(float a, float b, float x) { float t = as_clampf((x - a) / (b - a), 0, 1); return t * t * (3 - 2 * t); }
void map_draw(int id, Canvas *c, const MapDraw *d) {
    Map *m = mmust(id); int xs = d->xscale < 1 ? 1 : d->xscale > 4 ? 4 : d->xscale; int vw = d->vw, vh = d->vh; int tilesw = (vw + xs - 1) / xs;
    int tx0 = (int)floor(d->camx - tilesw / 2.0 + 0.5), ty0 = (int)floor(d->camy - vh / 2.0 + 0.5);
    for (int r = 0; r < vh; r++) for (int ci = 0; ci < tilesw; ci++) {
        int wx = tx0 + ci, wy = ty0 + r; int t = map_tile(id, wx, wy); TileDef *td = &TD[t]; uint32_t ch = td->defined ? td->ch : (t >= 33 && t < 127 ? (uint32_t)t : ' '); Col fg = td->fg, bg = td->bg; if (t == 0 && !td->defined) ch = ' ';
        float k = 1.0f;
        if (d->fov) { bool v = vis_at(m, wx, wy); if (!v) { bool seen = !m->infinite && wx >= 0 && wy >= 0 && wx < m->w && wy < m->h && m->seen[(size_t)wy * m->w + wx]; if (!seen) { ch = ' '; fg = 0; bg = 0; k = 1; } else k = 0.32f; } }
        if (d->fog > 0) { float dd = (float)hypot((wx - d->camx) * xs / 2.0, wy - d->camy); k *= 1.0f - smooth01(d->fog * 0.55f, d->fog, dd); if (k <= 0.02f) { ch = ' '; fg = 0; bg = 0; } }
        if (k < 1.0f && ch != ' ') { Col tint = d->fog_col ? d->fog_col : col_rgb(0, 0, 0); fg = fg ? col_mix(tint, fg, k) : col_mix(tint, col_rgb(200, 200, 200), k); if (bg) bg = col_mix(tint, bg, k); }
        bool dup = (ch >= 0x2580 && ch <= 0x259F) || ch == '#' || ch == '=' || ch == 0x2593 || ch == 0x2592;
        for (int s = 0; s < xs; s++) { int sx = d->vx + ci * xs + s; if (sx >= d->vx + vw) break; uint32_t cc = s == 0 || dup ? ch : ' '; Cell *q = cv_at(c, sx, d->vy + r); if (!q) continue; if (cc == ' ' && !bg) { q->ch = ' '; q->fg = 0; q->bg = 0; q->attr = 0; } else cv_put(c, sx, d->vy + r, cc, fg, bg, 0), (bg ? 0 : (q->bg = 0)); }
    }
}

/* ------------------------------------------------------------ raycasting */
bool map_raycast(int id, double px, double py, double ang, double maxd, RayHit *out) {
    double dx = cos(ang), dy = sin(ang); int mx = (int)floor(px), my = (int)floor(py);
    double ddx = dx == 0 ? 1e30 : fabs(1.0 / dx), ddy = dy == 0 ? 1e30 : fabs(1.0 / dy); int sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
    double sdx = dx < 0 ? (px - mx) * ddx : (mx + 1.0 - px) * ddx, sdy = dy < 0 ? (py - my) * ddy : (my + 1.0 - py) * ddy; int side = 0;
    for (int i = 0; i < 512; i++) {
        if (sdx < sdy) { sdx += ddx; mx += sx; side = 0; } else { sdy += ddy; my += sy; side = 1; }
        double dist = side == 0 ? sdx - ddx : sdy - ddy; if (dist > maxd) return false;
        if (map_opaque(id, mx, my)) { out->dist = dist; out->tile = map_tile(id, mx, my); out->side = side; out->hx = px + dx * dist; out->hy = py + dy * dist; return true; }
    }
    return false;
}
extern int ent_visible_billboards(double px, double py, double ang, double fov, int vw, double maxd, int cap, double *dist, int *col, uint32_t *ch, Col *fg, int *hscale);
void raycast_view(int id, Canvas *c, double px, double py, double ang, double fov_deg, int vx, int vy, int vw, int vh, const Opts *o) {
    double fov = fov_deg * M_PI / 180.0, maxd = opt_num(o, "max_dist", 24); bool color = opt_bool(o, "color", true), sprites = opt_bool(o, "sprites", true); double fog = opt_num(o, "fog", maxd);
    const char *rs = opt_str(o, "ramp", NULL); const char *pr = rs ? ramp_preset(rs) : NULL; uint32_t wall[16]; int nw = 0; const char *wr = pr ? pr : (rs ? rs : "\xE2\x96\x88\xE2\x96\x93\xE2\x96\x92\xE2\x96\x91"); while (*wr && nw < 16) wall[nw++] = utf8_next(&wr); if (nw < 2) as_err("raycast_view: ramp needs 2+ characters");
    Col ceil_c = opt_col(o, "ceiling", col_rgb(18, 20, 34)), floor_c = opt_col(o, "floor", col_rgb(40, 34, 30)); double *zbuf = kc_malloc((size_t)vw * sizeof(double));
    for (int x = 0; x < vw; x++) {
        double cam = 2.0 * x / (vw - 1 > 0 ? vw - 1 : 1) - 1.0, ra = ang + atan(cam * tan(fov / 2)); RayHit h; bool hit = map_raycast(id, px, py, ra, maxd, &h);
        double dist = hit ? h.dist * cos(ra - ang) : maxd * 2; zbuf[x] = dist; if (dist < 0.05) dist = 0.05;
        int lh = hit ? (int)((double)vh / dist * 0.9 * (fov_deg > 0 ? 66.0 / fov_deg : 1)) : 0; if (lh > vh * 4) lh = vh * 4; int top = vh / 2 - lh / 2, bot = vh / 2 + lh / 2;
        for (int y = 0; y < vh; y++) {
            if (y < top) { float t = (float)y / (float)(vh / 2); cv_put(c, vx + x, vy + y, ' ', 0, col_scale(ceil_c, 0.5f + t * 0.7f), 0); cv_fill(c, vx + x, vy + y, 1, 1, ' ', 0, col_scale(ceil_c, 0.5f + t * 0.7f), 0); }
            else if (y >= bot || !hit) { float t = (float)(y - vh / 2) / (float)(vh / 2 > 0 ? vh / 2 : 1); double fd = 1.0 / (t + 0.05); uint32_t fc = t > 0.6f ? ':' : t > 0.3f ? '.' : ','; float k = (float)as_clampf((float)(1.0 - fd / (fog * 1.0)), 0.1f, 1);
                cv_fill(c, vx + x, vy + y, 1, 1, fc, col_scale(floor_c, 1.6f * k), col_scale(floor_c, 0.6f * k), 0); }
            else { double k = 1.0 - dist / fog; k = k < 0 ? 0 : k > 1 ? 1 : k; int ri = (int)((1 - k) * (nw - 1) + 0.5); if (h.side) ri = ri + 1 < nw ? ri + 1 : ri; TileDef *td = &TD[h.tile]; Col base = color ? (td->fg ? td->fg : col_rgb(190, 190, 200)) : 0; float sh = (h.side ? 0.7f : 1.0f) * (0.25f + 0.75f * (float)k);
                cv_fill(c, vx + x, vy + y, 1, 1, wall[ri], color ? col_scale(base, sh) : 0, color ? col_scale(base, sh * 0.35f) : 0, 0); }
        }
    }
    if (sprites) {
        double bd[64]; int bc[64], bh[64]; uint32_t bch[64]; Col bfg[64]; int n = ent_visible_billboards(px, py, ang, fov, vw, maxd, 64, bd, bc, bch, bfg, bh);
        for (int i = 0; i < n; i++) { int hh = (int)((double)vh / bd[i] * 0.9 * bh[i]); if (hh < 1) hh = 1; int top = vh / 2 + (int)((double)vh / bd[i] * 0.45) - hh; int w2 = hh > 1 ? hh / 2 : 1; if (w2 < 1) w2 = 1;
            for (int sx = bc[i] - w2 / 2; sx <= bc[i] + w2 / 2; sx++) { if (sx < 0 || sx >= vw || bd[i] >= zbuf[sx]) continue; for (int y = top; y < top + hh; y++) { if (y < 0 || y >= vh) continue; float k = (float)as_clampf((float)(1 - bd[i] / fog), 0.2f, 1); Cell *q = cv_at(c, vx + sx, vy + y); cv_put(c, vx + sx, vy + y, bch[i], color ? col_scale(bfg[i] ? bfg[i] : col_rgb(255, 255, 255), k) : 0, q ? q->bg : 0, 0); } } }
    }
    free(zbuf);
}
