/* std.game — streamed chunk world.
 *
 * The world is an unbounded grid of 16x16-column chunks (height configurable, default 64).
 * Nothing is ever loaded as a whole: each frame only the chunks inside the load radius around
 * the camera exist in memory. A chunk comes from (1) its file in the world directory if it was
 * edited/baked before, else (2) the deterministic procedural generator. Chunks leaving the radius
 * are saved (if edited) and freed; a hard cap on resident chunks bounds memory.
 * Meshes are built only for loaded chunks, drawn only when inside the frustum and the fog
 * horizon. The fog end is clamped to the nearest chunk that is still loading, so unloaded
 * areas are never visible: they are hidden by fog / cloud.
 */
#include "game_internal.h"

#define CS 16
enum { B_AIR, B_GRASS, B_DIRT, B_STONE, B_SAND, B_WATER, B_LOG, B_LEAVES, B_SNOW, B_PLANKS, B_BRICK, B_DEFAULTS };

typedef struct { uint8_t x, y, z, face, block, ao, p0, p1; } Quad;
typedef struct Chunk {
    int cx, cz; uint8_t *b; uint8_t *top_y, *top_b;
    Quad *q, *wq; int nq, cq, nwq, cwq;
    bool edited, meshed, mesh_dirty, alive; uint64_t last;
} Chunk;
typedef struct { char *name; int tile[3]; bool solid, translucent, cutout; uint32_t color; bool used; } BlockDef;

static BlockDef BD[256];
static int atlas_tex = -1, atlas_n;
static bool bd_init;

static struct {
    bool active; char *dir; int H, sea; uint32_t seed; char gen[16]; float tree_density;
    int radius, extra, max_chunks, loads_pf, meshes_pf;
    Chunk **tab; int tcap, tcount; Chunk **list; int nlist, clist;
    int64_t loads, unloads, saves, from_disk, generated, meshes;
    int need_total, need_ready; size_t quad_bytes;
    float horizon;
} W = { .H = 64, .sea = 22, .seed = 1, .gen = "nature", .tree_density = 1.0f, .radius = 7, .extra = 2, .max_chunks = 700, .loads_pf = 3, .meshes_pf = 4 };

int world_chunk_size(void) { return CS; }
int world_height(void) { return W.H; }
bool world_active(void) { return W.active; }

/* ============================================================ blocks/atlas */
static int atlas_add(int texid) {
    Tex *s = tex_get(texid), *a = tex_get(atlas_tex); if (!s || !a || atlas_n >= 256) return -1;
    int t = atlas_n++, ox = (t % 16) * 16, oy = (t / 16) * 16;
    for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) a->px[(size_t)(oy + y) * 256 + ox + x] = s->px[(size_t)(y * s->h / 16) * s->w + (x * s->w / 16)];
    return t;
}
static uint32_t tile_avg(int t) {
    Tex *a = tex_get(atlas_tex); uint64_t r = 0, g = 0, b = 0, n = 0; int ox = (t % 16) * 16, oy = (t / 16) * 16;
    for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) { uint32_t c = a->px[(size_t)(oy + y) * 256 + ox + x]; if (CA(c) < 128) continue; r += CR(c); g += CG(c); b += CB(c); n++; }
    return n ? col_pack((int)(r / n), (int)(g / n), (int)(b / n), 255) : 0xFF404040;
}
static bool tile_cutout(int t) {
    Tex *a = tex_get(atlas_tex); int ox = (t % 16) * 16, oy = (t / 16) * 16;
    for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) if (CA(a->px[(size_t)(oy + y) * 256 + ox + x]) < 128) return true;
    return false;
}
int block_define(const char *name, int ttop, int tside, int tbot, bool solid, bool trans, uint32_t color) {
    int id = -1; for (int i = 1; i < 256; i++) if (BD[i].used && !strcmp(BD[i].name, name)) id = i;
    if (id < 0) for (int i = 1; i < 256; i++) if (!BD[i].used) { id = i; break; }
    if (id < 0) gm_err("block_define: all 255 block ids are in use");
    if (atlas_tex < 0) { atlas_tex = tex_new(256, 256, 0); tex_peek(atlas_tex)->name = kc_strdup("block-atlas"); }
    BlockDef *b = &BD[id]; if (b->used) free(b->name);
    b->used = true; b->name = kc_strdup(name); b->solid = solid; b->translucent = trans;
    b->tile[0] = ttop >= 0 ? atlas_add(ttop) : 0; b->tile[1] = tside >= 0 ? atlas_add(tside) : b->tile[0]; b->tile[2] = tbot >= 0 ? atlas_add(tbot) : b->tile[1];
    if (b->tile[0] < 0) b->tile[0] = 0; if (b->tile[1] < 0) b->tile[1] = b->tile[0]; if (b->tile[2] < 0) b->tile[2] = b->tile[1];
    b->cutout = tile_cutout(b->tile[0]) || tile_cutout(b->tile[1]); b->color = color ? color : tile_avg(b->tile[0]);
    return id;
}
int block_by_name(const char *name) { for (int i = 0; i < 256; i++) if (BD[i].used && !strcmp(BD[i].name, name)) return i; return -1; }
const char *block_name(int id) { return (id > 0 && id < 256 && BD[id].used) ? BD[id].name : (id == 0 ? "air" : NULL); }
uint32_t block_color(int id) { return (id > 0 && id < 256 && BD[id].used) ? BD[id].color : 0xFF000000u; }

static void defaults_init(void) {
    if (bd_init) return; bd_init = true;
    BD[0].used = true; BD[0].name = kc_strdup("air");
    int grass = tex_proc("grass", 16, 1, 0, 0), dirt = tex_proc("dirt", 16, 2, 0, 0), stone = tex_proc("stone", 16, 3, 0, 0), sand = tex_proc("sand", 16, 4, 0, 0);
    int water = tex_proc("water", 16, 5, 0, 0), wood = tex_proc("wood", 16, 6, 0xFF5E4630, 0xFF3F2D1C), logtop = tex_proc("wood", 16, 7, 0xFFB08A58, 0xFF8A6A3E), leaves = tex_proc("leaves", 16, 8, 0, 0);
    int snow = tex_proc("snow", 16, 9, 0, 0), planks = tex_proc("planks", 16, 10, 0, 0), brick = tex_proc("brick", 16, 11, 0, 0);
    int gside = tex_new(16, 16, 0); { Tex *g = tex_peek(gside), *d = tex_peek(dirt), *gr = tex_peek(grass);
        for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) { int depth = 3 + (int)(hash3(x, 0, 0, 5) % 3); g->px[y * 16 + x] = y < depth ? gr->px[y * 16 + x] : d->px[y * 16 + x]; } }
    struct { const char *n; int t, s, b; bool solid, trans; } D[] = {
        {"grass", grass, gside, dirt, 1, 0}, {"dirt", dirt, dirt, dirt, 1, 0}, {"stone", stone, stone, stone, 1, 0}, {"sand", sand, sand, sand, 1, 0},
        {"water", water, water, water, 0, 1}, {"log", logtop, wood, logtop, 1, 0}, {"leaves", leaves, leaves, leaves, 1, 0}, {"snow", snow, snow, snow, 1, 0},
        {"planks", planks, planks, planks, 1, 0}, {"brick", brick, brick, brick, 1, 0} };
    for (size_t i = 0; i < sizeof D / sizeof D[0]; i++) block_define(D[i].n, D[i].t, D[i].s, D[i].b, D[i].solid, D[i].trans, 0);
    int ids[] = { grass, dirt, stone, sand, water, wood, logtop, leaves, snow, planks, brick, gside }; for (size_t i = 0; i < sizeof ids / sizeof ids[0]; i++) tex_free(ids[i]);
}
static inline bool b_opaque(int b) { return b > 0 && BD[b].solid && !BD[b].translucent && !BD[b].cutout; }

/* ================================================================ chunks */
static uint32_t ckey(int cx, int cz) { return hash3(cx, cz, 0, 0x51ed270b); }
static Chunk *chunk_find(int cx, int cz) {
    if (!W.tcap) return NULL; uint32_t h = ckey(cx, cz) & (uint32_t)(W.tcap - 1);
    for (int n = 0; n < W.tcap; n++) { Chunk *c = W.tab[h]; if (!c) return NULL; if (c != (Chunk *)1 && c->cx == cx && c->cz == cz) return c; h = (h + 1) & (uint32_t)(W.tcap - 1); }
    return NULL;
}
static void tab_insert(Chunk *c) {
    if ((W.tcount + 1) * 2 > W.tcap) {
        int nc = W.tcap ? W.tcap * 2 : 256; Chunk **nt = kc_malloc(sizeof(Chunk *) * (size_t)nc); memset(nt, 0, sizeof(Chunk *) * (size_t)nc);
        for (int i = 0; i < W.tcap; i++) if (W.tab[i] && W.tab[i] != (Chunk *)1) { uint32_t h = ckey(W.tab[i]->cx, W.tab[i]->cz) & (uint32_t)(nc - 1); while (nt[h]) h = (h + 1) & (uint32_t)(nc - 1); nt[h] = W.tab[i]; }
        free(W.tab); W.tab = nt; W.tcap = nc;
    }
    uint32_t h = ckey(c->cx, c->cz) & (uint32_t)(W.tcap - 1); while (W.tab[h] && W.tab[h] != (Chunk *)1) h = (h + 1) & (uint32_t)(W.tcap - 1);
    W.tab[h] = c; W.tcount++;
    if (W.nlist == W.clist) { W.clist = W.clist ? W.clist * 2 : 128; W.list = kc_realloc(W.list, sizeof(Chunk *) * (size_t)W.clist); }
    W.list[W.nlist++] = c;
}
static void tab_remove(Chunk *c) {
    uint32_t h = ckey(c->cx, c->cz) & (uint32_t)(W.tcap - 1);
    for (int n = 0; n < W.tcap; n++) { if (W.tab[h] == c) { W.tab[h] = (Chunk *)1; break; } h = (h + 1) & (uint32_t)(W.tcap - 1); }
    W.tcount--;
    for (int i = 0; i < W.nlist; i++) if (W.list[i] == c) { W.list[i] = W.list[--W.nlist]; break; }
}
static inline size_t bidx(int x, int y, int z) { return ((size_t)y * CS + (size_t)z) * CS + (size_t)x; }
static void chunk_tops(Chunk *c) {
    for (int z = 0; z < CS; z++) for (int x = 0; x < CS; x++) {
        int y = W.H - 1; while (y > 0 && c->b[bidx(x, y, z)] == 0) y--;
        c->top_y[z * CS + x] = (uint8_t)y; c->top_b[z * CS + x] = c->b[bidx(x, y, z)];
    }
}

/* ------------------------------------------------------------- generator */
static float gen_height(int x, int z) {
    uint32_t s = W.seed; float sea = (float)W.sea, H = (float)W.H;
    if (!strcmp(W.gen, "flat")) return sea + 2;
    float n1 = fbm2(x * 0.008f, z * 0.008f, 4, 2, 0.5f, s), n2 = fbm2(x * 0.03f, z * 0.03f, 3, 2, 0.5f, s + 7);
    if (!strcmp(W.gen, "hills")) return fclampf(sea + (n1 - 0.4f) * 16 + (n2 - 0.5f) * 5, 2, H - 4);
    float mt = fbm2(x * 0.004f + 50, z * 0.004f + 50, 3, 2, 0.5f, s + 3); float m = fclampf((mt - 0.52f) / 0.28f, 0, 1); m = m * m * (3 - 2 * m);
    if (!strcmp(W.gen, "mountains")) return fclampf(sea + (n1 - 0.35f) * 26 + (n2 - 0.5f) * 8 + m * 34 + 6, 2, H - 3);
    if (!strcmp(W.gen, "islands")) { float d = fbm2(x * 0.012f, z * 0.012f, 4, 2, 0.5f, s + 21); return fclampf(sea - 9 + (d - 0.45f) * 55 + (n2 - 0.5f) * 4, 2, H - 4); }
    return fclampf(sea + (n1 - 0.45f) * 30 + (n2 - 0.5f) * 6 + m * 30, 2, H - 4);                       /* nature */
}
static int surf_block(int wx, int wz, int h) {
    float temp = fbm2(wx * 0.005f, wz * 0.005f, 2, 2, 0.5f, W.seed + 11);
    if (h <= W.sea + 1) return B_SAND;
    if (h > (int)(W.H * 0.74f) || (temp < 0.3f && h > W.sea + 10)) return B_SNOW;
    return B_GRASS;
}
static void gen_chunk(Chunk *c) {
    int H = W.H; memset(c->b, 0, (size_t)CS * CS * (size_t)H);
    if (!strcmp(W.gen, "empty") || !strcmp(W.gen, "none")) return;
    enum { R = 3, N = CS + 2 * R };
    int hs[N * N]; int ox = c->cx * CS - R, oz = c->cz * CS - R;
    for (int j = 0; j < N; j++) for (int i = 0; i < N; i++) hs[j * N + i] = (int)gen_height(ox + i, oz + j);
    for (int z = 0; z < CS; z++) for (int x = 0; x < CS; x++) {
        int h = hs[(z + R) * N + x + R], wx = c->cx * CS + x, wz = c->cz * CS + z, sb = surf_block(wx, wz, h);
        for (int y = 0; y <= h && y < H; y++) c->b[bidx(x, y, z)] = (uint8_t)(y == h ? sb : (y >= h - 3 ? (sb == B_SAND ? B_SAND : B_DIRT) : B_STONE));
        for (int y = h + 1; y <= W.sea && y < H; y++) c->b[bidx(x, y, z)] = B_WATER;
    }
    if (W.tree_density <= 0 || !strcmp(W.gen, "flat")) { chunk_tops(c); return; }
    for (int tj = 0; tj < N; tj++) for (int ti = 0; ti < N; ti++) {
        int tx = ox + ti, tz = oz + tj, h = hs[tj * N + ti];
        if (h <= W.sea + 1 || h >= H - 14 || surf_block(tx, tz, h) != B_GRASS) continue;
        uint32_t hv = hash3(tx, tz, 0, W.seed ^ 0xabc1u); float forest = fbm2(tx * 0.02f, tz * 0.02f, 2, 2, 0.5f, W.seed + 31);
        float prob = (forest > 0.52f ? 0.045f : 0.006f) * W.tree_density;
        if ((hv & 0xFFFF) >= (uint32_t)(prob * 65536.0f)) continue;
        bool pine = h > W.sea + 14 || (hv >> 20 & 3) == 0; int th = 4 + (int)((hv >> 16) % 3);
        #define PUT(px, py, pz, blk, only_air) do { int lx_ = (px) - c->cx * CS, lz_ = (pz) - c->cz * CS, ly_ = (py); \
            if (lx_ >= 0 && lx_ < CS && lz_ >= 0 && lz_ < CS && ly_ > 0 && ly_ < H) { uint8_t *q_ = &c->b[bidx(lx_, ly_, lz_)]; if (!(only_air) || *q_ == 0) *q_ = (uint8_t)(blk); } } while (0)
        if (pine) {
            int top = h + th + 3;
            for (int y = h + 3; y <= top; y++) { int k = top - y; int r = k < 1 ? 0 : (k < 3 ? 1 : 2); if (k >= 5) r = 3 - (k & 1);
                for (int dx = -r; dx <= r; dx++) for (int dz = -r; dz <= r; dz++) if (dx * dx + dz * dz <= r * r + 1) PUT(tx + dx, y, tz + dz, B_LEAVES, true); }
        } else {
            int cy = h + th;
            for (int dx = -3; dx <= 3; dx++) for (int dy = -2; dy <= 3; dy++) for (int dz = -3; dz <= 3; dz++) if (dx * dx + dz * dz + dy * dy * 1.6f <= 8.2f) PUT(tx + dx, cy + dy, tz + dz, B_LEAVES, true);
        }
        for (int y = h + 1; y <= h + th + (pine ? 2 : 0); y++) PUT(tx, y, tz, B_LOG, false);
    }
    chunk_tops(c);
}

/* ------------------------------------------------------------ disk store */
static char *chunk_path(int cx, int cz) {
    char name[64]; snprintf(name, sizeof name, "c.%d.%d.kcc", cx, cz); return path_join(W.dir, name);
}
static bool chunk_save(Chunk *c) {
    if (!W.dir || !*W.dir) return false;
    size_t total = (size_t)CS * CS * (size_t)W.H, cap = 64 + total / 2 + 16, n = 0; uint8_t *o = kc_malloc(cap);
    memcpy(o, "KCC1", 4); n = 4; int32_t v[2] = { c->cx, c->cz }; memcpy(o + n, v, 8); n += 8; uint16_t hh = (uint16_t)W.H; memcpy(o + n, &hh, 2); n += 2;
    for (size_t i = 0; i < total;) {
        size_t j = i; while (j < total && c->b[j] == c->b[i]) j++; size_t run = j - i;
        if (n + 8 > cap) { cap *= 2; o = kc_realloc(o, cap); }
        o[n++] = c->b[i]; while (run >= 128) { o[n++] = (uint8_t)(run | 128); run >>= 7; } o[n++] = (uint8_t)run; i = j;
    }
    char *p = chunk_path(c->cx, c->cz); bool ok = write_file_all(p, o, n); free(p); free(o);
    if (ok) { W.saves++; c->edited = false; }
    return ok;
}
static bool chunk_read(Chunk *c) {
    if (!W.dir || !*W.dir) return false;
    char *p = chunk_path(c->cx, c->cz); size_t n; uint8_t *d = (uint8_t *)read_file_all(p, &n); free(p);
    if (!d) return false;
    bool ok = n >= 14 && !memcmp(d, "KCC1", 4); uint16_t hh = 0; if (ok) memcpy(&hh, d + 12, 2);
    if (ok && hh != W.H) { gm_warn("chunk %d,%d was saved with height %d (world height is %d): regenerating", c->cx, c->cz, hh, W.H); ok = false; }
    if (ok) {
        size_t total = (size_t)CS * CS * (size_t)W.H, o = 0, i = 14;
        while (i < n && o < total) { uint8_t b = d[i++]; size_t run = 0; int sh = 0; while (i < n) { uint8_t x = d[i++]; run |= (size_t)(x & 127) << sh; sh += 7; if (!(x & 128)) break; } if (o + run > total) run = total - o; memset(c->b + o, b, run); o += run; }
        ok = o == total;
    }
    free(d); if (ok) chunk_tops(c);
    return ok;
}
static Chunk *chunk_new(int cx, int cz) {
    Chunk *c = kc_malloc(sizeof *c); memset(c, 0, sizeof *c); c->cx = cx; c->cz = cz; c->alive = true;
    c->b = kc_malloc((size_t)CS * CS * (size_t)W.H); c->top_y = kc_malloc(CS * CS); c->top_b = kc_malloc(CS * CS);
    if (chunk_read(c)) W.from_disk++; else { gen_chunk(c); W.generated++; }
    c->last = G.frame; W.loads++; tab_insert(c); return c;
}
static void chunk_free(Chunk *c) {
    if (c->edited && W.dir && *W.dir) chunk_save(c);
    tab_remove(c); W.unloads++;
    free(c->b); free(c->top_y); free(c->top_b); free(c->q); free(c->wq); free(c);
}

/* ---------------------------------------------------------------- access */
static inline Chunk *chunk_at_block(int x, int z) { return chunk_find(floordiv(x, CS), floordiv(z, CS)); }
int world_get(int x, int y, int z, bool *loaded) {
    Chunk *c = chunk_at_block(x, z);
    if (!c) { if (loaded) *loaded = false; return 0; }
    if (loaded) *loaded = true;
    if (y < 0 || y >= W.H) return 0;
    return c->b[bidx(floormod(x, CS), y, floormod(z, CS))];
}
static void mark_dirty_around(Chunk *c, int lx, int lz) {
    c->mesh_dirty = true;
    if (lx == 0) { Chunk *n = chunk_find(c->cx - 1, c->cz); if (n) n->mesh_dirty = true; }
    if (lx == CS - 1) { Chunk *n = chunk_find(c->cx + 1, c->cz); if (n) n->mesh_dirty = true; }
    if (lz == 0) { Chunk *n = chunk_find(c->cx, c->cz - 1); if (n) n->mesh_dirty = true; }
    if (lz == CS - 1) { Chunk *n = chunk_find(c->cx, c->cz + 1); if (n) n->mesh_dirty = true; }
}
bool world_set(int x, int y, int z, int block) {
    Chunk *c = chunk_at_block(x, z); if (!c || y < 0 || y >= W.H || block < 0 || block > 255 || (block && !BD[block].used)) return false;
    int lx = floormod(x, CS), lz = floormod(z, CS); uint8_t *p = &c->b[bidx(lx, y, lz)]; if (*p == block) return true;
    *p = (uint8_t)block; c->edited = true; mark_dirty_around(c, lx, lz);
    int ty = W.H - 1; while (ty > 0 && c->b[bidx(lx, ty, lz)] == 0) ty--; c->top_y[lz * CS + lx] = (uint8_t)ty; c->top_b[lz * CS + lx] = c->b[bidx(lx, ty, lz)];
    return true;
}
bool world_solid_at(int x, int y, int z, bool unloaded_solid) {
    bool ld; int b = world_get(x, y, z, &ld);
    if (!ld) return unloaded_solid;
    if (y < 0) return true;
    return b > 0 && BD[b].solid;
}
int world_surface(int x, int z, int *block) {
    Chunk *c = chunk_at_block(x, z); if (!c) return -1;
    int i = floormod(z, CS) * CS + floormod(x, CS); if (block) *block = c->top_b[i]; return c->top_y[i];
}
uint32_t world_gen_color(int x, int z) {
    int b; int y = world_surface(x, z, &b); if (y < 0) return 0;
    float k = 0.7f + 0.3f * (float)y / (float)W.H; return col_scale(block_color(b), k) | 0xFF000000u;
}
bool world_chunk_loaded(int cx, int cz) { return chunk_find(cx, cz) != NULL; }

/* --------------------------------------------------------------- meshing */
static const int FD[6][3] = { {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1} };
static const uint8_t FC[6][4][3] = {
    { {1,0,1},{1,0,0},{1,1,0},{1,1,1} }, { {0,0,0},{0,0,1},{0,1,1},{0,1,0} }, { {0,1,1},{1,1,1},{1,1,0},{0,1,0} },
    { {0,0,0},{1,0,0},{1,0,1},{0,0,1} }, { {0,0,1},{1,0,1},{1,1,1},{0,1,1} }, { {1,0,0},{0,0,0},{0,1,0},{1,1,0} } };
static const float FUV[4][2] = { {0,1}, {1,1}, {1,0}, {0,0} };

static inline int mblk(Chunk *c, Chunk *nb[4], int x, int y, int z) {
    if (y < 0) return 255; if (y >= W.H) return 0;
    if (x >= 0 && x < CS && z >= 0 && z < CS) return c->b[bidx(x, y, z)];
    Chunk *n = x < 0 ? nb[0] : x >= CS ? nb[1] : z < 0 ? nb[2] : nb[3];
    if (!n) return 255;
    return n->b[bidx(floormod(x, CS), y, floormod(z, CS))];
}
static inline bool opq(int b) { return b == 255 || b_opaque(b); }
static void qpush(Quad **a, int *n, int *cap, Quad q) { if (*n == *cap) { *cap = *cap ? *cap * 2 : 512; *a = kc_realloc(*a, sizeof(Quad) * (size_t)*cap); } (*a)[(*n)++] = q; }

static void chunk_mesh(Chunk *c) {
    Chunk *nb[4] = { chunk_find(c->cx - 1, c->cz), chunk_find(c->cx + 1, c->cz), chunk_find(c->cx, c->cz - 1), chunk_find(c->cx, c->cz + 1) };
    W.quad_bytes -= (size_t)(c->cq + c->cwq) * sizeof(Quad); c->nq = c->nwq = 0;
    int maxy = 0; for (int i = 0; i < CS * CS; i++) if (c->top_y[i] > maxy) maxy = c->top_y[i];
    for (int y = 0; y <= maxy; y++) for (int z = 0; z < CS; z++) for (int x = 0; x < CS; x++) {
        int b = c->b[bidx(x, y, z)]; if (!b) continue; bool tr = BD[b].translucent;
        for (int f = 0; f < 6; f++) {
            int nx = x + FD[f][0], ny = y + FD[f][1], nz = z + FD[f][2]; int n = mblk(c, nb, nx, ny, nz);
            if (opq(n)) continue;
            if (n == b && (tr || BD[b].cutout)) continue;
            if (tr && (f == 3 || (n != 0 && BD[n].translucent))) continue;
            uint8_t ao = 3 << 6 | 3 << 4 | 3 << 2 | 3;
            if (!tr) {
                int a1 = f < 2 ? 1 : 0, a2 = f < 4 ? 2 : 1; if (f < 2) { a1 = 1; a2 = 2; } else if (f < 4) { a1 = 0; a2 = 2; } else { a1 = 0; a2 = 1; }
                ao = 0;
                for (int k = 0; k < 4; k++) {
                    int sg1 = FC[f][k][a1] ? 1 : -1, sg2 = FC[f][k][a2] ? 1 : -1; int d1[3] = { 0, 0, 0 }, d2[3] = { 0, 0, 0 }; d1[a1] = sg1; d2[a2] = sg2;
                    int s1 = opq(mblk(c, nb, nx + d1[0], ny + d1[1], nz + d1[2])), s2 = opq(mblk(c, nb, nx + d2[0], ny + d2[1], nz + d2[2])), cn = opq(mblk(c, nb, nx + d1[0] + d2[0], ny + d1[1] + d2[1], nz + d1[2] + d2[2]));
                    int v = (s1 && s2) ? 0 : 3 - (s1 + s2 + cn); ao |= (uint8_t)(v << (k * 2));
                }
            }
            Quad q = { (uint8_t)x, (uint8_t)y, (uint8_t)z, (uint8_t)f, (uint8_t)b, ao, 0, 0 };
            if (tr) qpush(&c->wq, &c->nwq, &c->cwq, q); else qpush(&c->q, &c->nq, &c->cq, q);
        }
    }
    W.quad_bytes += (size_t)(c->cq + c->cwq) * sizeof(Quad);
    c->meshed = true; c->mesh_dirty = false; W.meshes++;
}

/* -------------------------------------------------------------- streaming */
static void unload_far(int ccx, int ccz) {
    int U = W.radius + W.extra;
    for (int i = W.nlist - 1; i >= 0; i--) {
        Chunk *c = W.list[i]; int dx = c->cx - ccx, dz = c->cz - ccz;
        if (dx * dx + dz * dz > U * U && (c->edited ? (W.dir && *W.dir) : true)) { for (int k = 0; k < 4; k++) { static const int o[4][2] = { {-1,0},{1,0},{0,-1},{0,1} }; Chunk *n = chunk_find(c->cx + o[k][0], c->cz + o[k][1]); if (n) n->mesh_dirty = true; } chunk_free(c); }
    }
    while (W.nlist > W.max_chunks) {
        Chunk *far = NULL; int bd = -1;
        for (int i = 0; i < W.nlist; i++) { Chunk *c = W.list[i]; if (c->edited && !(W.dir && *W.dir)) continue; int dx = c->cx - ccx, dz = c->cz - ccz, d = dx * dx + dz * dz; if (d > bd) { bd = d; far = c; } }
        if (!far) break; chunk_free(far);
    }
}
void world_update(float dt) {
    if (!W.active) { r3d_fog_update(-1, dt); W.horizon = -1; return; }
    double t0 = clock_s(); bool timed = !G.headless;
    int ccx = floordiv((int)floorf(G.cam.x), CS), ccz = floordiv((int)floorf(G.cam.z), CS), R = W.radius;
    unload_far(ccx, ccz);
    V3 f = r3d_forward(); float fl = sqrtf(f.x * f.x + f.z * f.z); float fx = fl > 1e-4f ? f.x / fl : 0, fz = fl > 1e-4f ? f.z / fl : 0;
    typedef struct { int cx, cz; float pri; } Cand; static Cand *cand; static int ccap;
    int nc = 0, need_total = 0;
    for (int dz = -R; dz <= R; dz++) for (int dx = -R; dx <= R; dx++) {
        int d2 = dx * dx + dz * dz; if (d2 > R * R + R) continue; need_total++;
        if (chunk_find(ccx + dx, ccz + dz)) continue;
        if (nc == ccap) { ccap = ccap ? ccap * 2 : 256; cand = kc_realloc(cand, sizeof(Cand) * (size_t)ccap); }
        cand[nc].cx = ccx + dx; cand[nc].cz = ccz + dz; cand[nc].pri = (float)d2 - 3.0f * fmaxf(0, fx * dx + fz * dz); nc++;
    }
    for (int k = 0; k < W.loads_pf && nc > 0; k++) {
        int bi = 0; for (int i = 1; i < nc; i++) if (cand[i].pri < cand[bi].pri) bi = i;
        Chunk *c = chunk_new(cand[bi].cx, cand[bi].cz);
        for (int q = 0; q < 4; q++) { static const int o[4][2] = { {-1,0},{1,0},{0,-1},{0,1} }; Chunk *n = chunk_find(c->cx + o[q][0], c->cz + o[q][1]); if (n && n->meshed) n->mesh_dirty = true; }
        cand[bi] = cand[--nc];
        if (timed && clock_s() - t0 > 0.006) break;
    }
    /* meshing: nearest first */
    for (int k = 0; k < W.meshes_pf; k++) {
        Chunk *best = NULL; int bd = 1 << 30;
        for (int i = 0; i < W.nlist; i++) { Chunk *c = W.list[i]; if (c->meshed && !c->mesh_dirty) continue; int dx = c->cx - ccx, dz = c->cz - ccz, d = dx * dx + dz * dz; if (d > R * R + R + 4) continue; if (!c->meshed) d -= 1000; if (d < bd) { bd = d; best = c; } }
        if (!best) break; chunk_mesh(best);
        if (timed && clock_s() - t0 > 0.010) break;
    }
    /* readiness + fog horizon */
    int ready = 0; float horizon = 1e9f; float cosc = cosf(DEG2RAD(G.fov) * 0.5f * ((float)G.w / G.h) + 0.6f); if (cosc < -1) cosc = -1;
    for (int dz = -R; dz <= R; dz++) for (int dx = -R; dx <= R; dx++) {
        int d2 = dx * dx + dz * dz; if (d2 > R * R + R) continue; Chunk *c = chunk_find(ccx + dx, ccz + dz);
        if (c && c->meshed) { ready++; continue; }
        float px = (ccx + dx) * CS + CS * 0.5f - G.cam.x, pz = (ccz + dz) * CS + CS * 0.5f - G.cam.z, dist = sqrtf(px * px + pz * pz);
        float cosang = dist > 1e-3f ? (fx * px + fz * pz) / dist : 1.0f;
        if (dist < CS * 2.0f || cosang > cosc) { float e = dist - CS * 0.5f; if (e < horizon) horizon = e; }
    }
    W.need_total = need_total; W.need_ready = ready;
    float cap = (float)(R - 1) * CS; if (cap < CS * 2.0f) cap = CS * 2.0f;
    float safe = horizon < 1e8f ? fmaxf(horizon, CS * 1.5f) : cap; if (safe > cap) safe = cap;
    W.horizon = safe; r3d_fog_update(safe, dt);
    G.st_stream_ms = (clock_s() - t0) * 1000.0;
}
float world_horizon(void) { return W.horizon; }

/* ----------------------------------------------------------------- drawing */
typedef struct { Chunk *c; float d; } Vis;
static int cmp_far(const void *a, const void *b) { float x = ((const Vis *)a)->d, y = ((const Vis *)b)->d; return x < y ? 1 : (x > y ? -1 : 0); }

static void draw_quads(Chunk *c, const Quad *qs, int nq, bool water, const float fint[6], Tex *atlas, float pad) {
    const float *m = r3d_viewproj()->m; float bx0 = (float)(c->cx * CS), bz0 = (float)(c->cz * CS);
    static const float AOF[4] = { 0.5f, 0.68f, 0.84f, 1.0f }; V3 cam = G.cam;
    float lr = CR(R_light_col) / 255.0f, lg = CG(R_light_col) / 255.0f, lb = CB(R_light_col) / 255.0f;
    float lim = (R_fog.mode == 0 ? G.zfar : (R_fog.autofog ? R_fog.cur_end : R_fog.end)) + pad; lim *= lim;
    for (int i = 0; i < nq; i++) {
        const Quad *q = &qs[i]; float bx = bx0 + q->x, by = (float)q->y, bz = bz0 + q->z; int f = q->face;
        if (f == 0 ? cam.x < bx + 1 : f == 1 ? cam.x > bx : f == 2 ? cam.y < by + 1 : f == 3 ? cam.y > by : f == 4 ? cam.z < bz + 1 : cam.z > bz) { if (!water || f != 2) continue; }
        float ddx = bx + 0.5f - cam.x, ddy = by + 0.5f - cam.y, ddz = bz + 0.5f - cam.z; if (ddx * ddx + ddy * ddy + ddz * ddz > lim) continue;
        const BlockDef *bd = &BD[q->block]; int tile = bd->tile[f == 2 ? 0 : f == 3 ? 2 : 1]; float u0 = ((tile % 16) * 16 + 0.3f) / 256.0f, u1 = ((tile % 16) * 16 + 15.7f) / 256.0f, v0 = ((tile / 16) * 16 + 0.3f) / 256.0f, v1 = ((tile / 16) * 16 + 15.7f) / 256.0f;
        RV r[4];
        for (int k = 0; k < 4; k++) {
            float wx = bx + FC[f][k][0], wy = by + FC[f][k][1], wz = bz + FC[f][k][2]; if (water && f == 2 && FC[f][k][1]) wy -= 0.12f;
            r[k].x = m[0] * wx + m[4] * wy + m[8] * wz + m[12]; r[k].y = m[1] * wx + m[5] * wy + m[9] * wz + m[13]; r[k].z = 0;
            r[k].w = m[3] * wx + m[7] * wy + m[11] * wz + m[15];
            float dx = wx - cam.x, dy = wy - cam.y, dz = wz - cam.z; r[k].d = sqrtf(dx * dx + dy * dy + dz * dz);
            r[k].u = FUV[k][0] ? u1 : u0; r[k].v = FUV[k][1] ? v1 : v0;
            float a = AOF[(q->ao >> (k * 2)) & 3] * fint[f]; r[k].r = a * lr; r[k].g = a * lg; r[k].b = a * lb;
        }
        int fl = water ? (RF_BLEND | RF_NOZWRITE | RF_TWOSIDED) : 0;
        r3d_tri(&r[0], &r[1], &r[2], atlas, fl); r3d_tri(&r[0], &r[2], &r[3], atlas, fl);
        G.st_quads++;
    }
}
void world_draw(void) {
    if (!W.active || atlas_tex < 0) return;
    Tex *atlas = tex_get(atlas_tex); float fint[6]; V3 L = v3norm(v3mul(R_light_dir, -1.0f));
    for (int f = 0; f < 6; f++) { float d = v3dot(v3(FD[f][0], FD[f][1], FD[f][2]), L); if (d < 0) d = 0; fint[f] = R_ambient + (1.0f - R_ambient) * d; }
    float fogend = (R_fog.mode == 0 ? G.zfar : (R_fog.autofog ? R_fog.cur_end : R_fog.end)); float reach = fogend + CS * 1.5f;
    static Vis *vis; static int vcap; int nv = 0;
    for (int i = 0; i < W.nlist; i++) {
        Chunk *c = W.list[i]; if (!c->meshed) continue; c->last = G.frame;
        float cx = c->cx * CS + CS * 0.5f - G.cam.x, cz = c->cz * CS + CS * 0.5f - G.cam.z, d = sqrtf(cx * cx + cz * cz);
        if (d > reach) { G.st_chunks_culled++; continue; }
        V3 mn = v3((float)(c->cx * CS), 0, (float)(c->cz * CS)), mx = v3(mn.x + CS, (float)W.H, mn.z + CS);
        if (!r3d_aabb_visible(mn, mx)) { G.st_chunks_culled++; continue; }
        if (nv == vcap) { vcap = vcap ? vcap * 2 : 128; vis = kc_realloc(vis, sizeof(Vis) * (size_t)vcap); }
        vis[nv].c = c; vis[nv].d = d; nv++;
    }
    for (int i = 0; i < nv; i++) { draw_quads(vis[i].c, vis[i].c->q, vis[i].c->nq, false, fint, atlas, CS); G.st_chunks_drawn++; }
    qsort(vis, (size_t)nv, sizeof(Vis), cmp_far);
    r3d_set_alpha(0.62f);
    for (int i = 0; i < nv; i++) if (vis[i].c->nwq) draw_quads(vis[i].c, vis[i].c->wq, vis[i].c->nwq, true, fint, atlas, CS);
    r3d_set_alpha(1.0f);
}

/* --------------------------------------------------------- rays + physics */
bool world_raycast(V3 o, V3 d, float maxd, int *hx, int *hy, int *hz, int *nx, int *ny, int *nz, int *blk, float *dist, bool *unl) {
    if (unl) *unl = false; d = v3norm(d); if (v3len(d) < 1e-6f) return false;
    int x = (int)floorf(o.x), y = (int)floorf(o.y), z = (int)floorf(o.z); int sx = d.x > 0 ? 1 : -1, sy = d.y > 0 ? 1 : -1, sz = d.z > 0 ? 1 : -1;
    float tdx = d.x != 0 ? fabsf(1.0f / d.x) : 1e30f, tdy = d.y != 0 ? fabsf(1.0f / d.y) : 1e30f, tdz = d.z != 0 ? fabsf(1.0f / d.z) : 1e30f;
    float tmx = d.x != 0 ? (sx > 0 ? (x + 1 - o.x) : (o.x - x)) * tdx : 1e30f, tmy = d.y != 0 ? (sy > 0 ? (y + 1 - o.y) : (o.y - y)) * tdy : 1e30f, tmz = d.z != 0 ? (sz > 0 ? (z + 1 - o.z) : (o.z - z)) * tdz : 1e30f;
    int fnx = 0, fny = 0, fnz = 0; float t = 0;
    for (int i = 0; i < 2048; i++) {
        bool ld; int b = world_get(x, y, z, &ld);
        if (!ld) { if (unl) *unl = true; return false; }
        if (b > 0 && BD[b].solid) { *hx = x; *hy = y; *hz = z; *nx = fnx; *ny = fny; *nz = fnz; *blk = b; *dist = t; return true; }
        if (tmx < tmy && tmx < tmz) { x += sx; t = tmx; tmx += tdx; fnx = -sx; fny = 0; fnz = 0; }
        else if (tmy < tmz) { y += sy; t = tmy; tmy += tdy; fnx = 0; fny = -sy; fnz = 0; }
        else { z += sz; t = tmz; tmz += tdz; fnx = 0; fny = 0; fnz = -sz; }
        if (t > maxd) return false;
        if (y < -2 || y > W.H + 2) { if ((sy < 0 && y < 0) || (sy > 0 && y >= W.H)) return false; }
    }
    return false;
}
static bool box_hits(float px, float py, float pz, float hw, float hh) {
    int x0 = (int)floorf(px - hw), x1 = (int)floorf(px + hw), y0 = (int)floorf(py), y1 = (int)floorf(py + hh - 1e-4f), z0 = (int)floorf(pz - hw), z1 = (int)floorf(pz + hw);
    for (int y = y0; y <= y1; y++) for (int z = z0; z <= z1; z++) for (int x = x0; x <= x1; x++) if (world_solid_at(x, y, z, true)) return true;
    return false;
}
static void move_step(V3 *p, float hw, float hh, V3 d, float stepup, bool fl[4]) {
    fl[0] = fl[1] = fl[2] = fl[3] = false; const float E = 1e-3f;
    for (int ax = 0; ax < 2; ax++) {
        float dd = ax == 0 ? d.x : d.z; if (dd == 0) continue;
        V3 np = *p; if (ax == 0) np.x += dd; else np.z += dd;
        if (!box_hits(np.x, np.y, np.z, hw, hh)) { *p = np; continue; }
        if (stepup > 0 && !box_hits(np.x, np.y + stepup, np.z, hw, hh) && box_hits(p->x, p->y - 0.05f, p->z, hw, hh)) { np.y += stepup; *p = np; continue; }
        if (ax == 0) { p->x = dd > 0 ? floorf(p->x + dd + hw) - hw - E : floorf(p->x + dd - hw) + 1 + hw + E; if (box_hits(p->x, p->y, p->z, hw, hh)) p->x = p->x - dd * 0 ; fl[0] = true; }
        else { p->z = dd > 0 ? floorf(p->z + dd + hw) - hw - E : floorf(p->z + dd - hw) + 1 + hw + E; fl[2] = true; }
    }
    if (d.y != 0) {
        V3 np = *p; np.y += d.y;
        if (!box_hits(np.x, np.y, np.z, hw, hh)) *p = np;
        else { fl[1] = true; if (d.y < 0) { p->y = floorf(np.y) + 1 + E; fl[3] = true; } else p->y = floorf(np.y + hh) - hh - E; }
    }
    if (!fl[3]) fl[3] = box_hits(p->x, p->y - 0.02f, p->z, hw, hh);
}

void world_move(V3 *p, float hw, float hh, V3 d, float stepup, bool fl[4]) {
    float m = fmaxf(fabsf(d.x), fmaxf(fabsf(d.y), fabsf(d.z))); int steps = (int)ceilf(m / 0.4f); if (steps < 1) steps = 1; if (steps > 400) steps = 400;
    V3 sd = v3mul(d, 1.0f / (float)steps); fl[0] = fl[1] = fl[2] = fl[3] = false;
    for (int i = 0; i < steps; i++) {
        bool f[4]; move_step(p, hw, hh, sd, stepup, f);
        if (f[0]) { fl[0] = true; sd.x = 0; } if (f[2]) { fl[2] = true; sd.z = 0; } if (f[1]) { fl[1] = true; sd.y = 0; } if (f[3]) fl[3] = true;
    }
}

/* -------------------------------------------------------------- lifecycle */
void world_flush(void) { for (int i = 0; i < W.nlist; i++) if (W.list[i]->edited) chunk_save(W.list[i]); }
void world_reset(void) {
    if (W.active) world_flush();
    for (int i = W.nlist - 1; i >= 0; i--) { Chunk *c = W.list[i]; free(c->b); free(c->top_y); free(c->top_b); free(c->q); free(c->wq); free(c); }
    free(W.tab); free(W.list); free(W.dir);
    memset(&W, 0, sizeof W); W.H = 64; W.sea = 22; W.seed = 1; snprintf(W.gen, sizeof W.gen, "nature"); W.tree_density = 1; W.radius = 7; W.extra = 2; W.max_chunks = 700; W.loads_pf = 3; W.meshes_pf = 4;
}
void world_config_num(const char *k, double v) {
    if (!strcmp(k, "radius")) W.radius = iclampi((int)v, 2, 32);
    else if (!strcmp(k, "keep")) W.extra = iclampi((int)v, 1, 8);
    else if (!strcmp(k, "max_chunks")) W.max_chunks = imaxi((int)v, 25);
    else if (!strcmp(k, "loads_per_frame")) W.loads_pf = iclampi((int)v, 1, 64);
    else if (!strcmp(k, "meshes_per_frame")) W.meshes_pf = iclampi((int)v, 1, 64);
    else if (!strcmp(k, "trees")) W.tree_density = (float)v;
    else gm_err("world_config: unknown number setting \"%s\" (radius, keep, max_chunks, loads_per_frame, meshes_per_frame, trees)", k);
    if (W.max_chunks < (2 * (W.radius + W.extra) + 1) * (2 * (W.radius + W.extra) + 1) * 3 / 4) W.max_chunks = (2 * (W.radius + W.extra) + 1) * (2 * (W.radius + W.extra) + 1);
}
void world_config_str(const char *k, const char *v) { gm_err("world_config: unknown text setting \"%s\"", k); (void)v; }

bool world_open(const char *dir, const Opts *o) {
    world_reset(); defaults_init();
    W.dir = kc_strdup(dir ? dir : ""); W.H = iclampi(opt_int(o, "height", 64), 32, 256); W.sea = opt_int(o, "sea", W.H * 22 / 64); W.seed = (uint32_t)opt_num(o, "seed", 1);
    snprintf(W.gen, sizeof W.gen, "%s", opt_str(o, "gen", "nature"));
    const char *known[] = { "nature", "hills", "mountains", "islands", "flat", "empty", "none" }; bool kn = false; for (size_t i = 0; i < 7; i++) if (!strcmp(known[i], W.gen)) kn = true;
    if (!kn) gm_err("world: unknown generator \"%s\" (nature, hills, mountains, islands, flat, empty)", W.gen);
    W.tree_density = (float)opt_num(o, "trees", 1.0);
    if (*W.dir) {
        dir_make(W.dir); char *meta = path_join(W.dir, "world.kcw"); size_t n; char *t = read_file_all(meta, &n);
        if (t) { char *sv = NULL; for (char *l = strtok_r(t, "\n", &sv); l; l = strtok_r(NULL, "\n", &sv)) {
                if (!strncmp(l, "seed=", 5)) W.seed = (uint32_t)strtoul(l + 5, NULL, 10); else if (!strncmp(l, "height=", 7)) W.H = atoi(l + 7); else if (!strncmp(l, "sea=", 4)) W.sea = atoi(l + 4);
                else if (!strncmp(l, "gen=", 4)) snprintf(W.gen, sizeof W.gen, "%s", l + 4); else if (!strncmp(l, "trees=", 6)) W.tree_density = (float)atof(l + 6); }
            free(t); }
        else { char buf[200]; snprintf(buf, sizeof buf, "KCW1\nseed=%u\nheight=%d\nsea=%d\ngen=%s\ntrees=%g\n", W.seed, W.H, W.sea, W.gen, W.tree_density); write_file_all(meta, buf, strlen(buf)); }
        free(meta);
    }
    W.radius = iclampi(opt_int(o, "radius", 7), 2, 32); W.extra = 2; W.max_chunks = (2 * (W.radius + W.extra) + 1) * (2 * (W.radius + W.extra) + 1);
    W.loads_pf = opt_int(o, "loads_per_frame", 3); W.meshes_pf = opt_int(o, "meshes_per_frame", 4);
    W.active = true; R_fog.cur_end = 0;
    return true;
}
int world_bake(int radius) {
    if (!W.active || !W.dir || !*W.dir) gm_err("world_bake: open the world with a directory first (game.world(\"dir\"))");
    int ccx = floordiv((int)floorf(G.cam.x), CS), ccz = floordiv((int)floorf(G.cam.z), CS), n = 0;
    for (int dz = -radius; dz <= radius; dz++) for (int dx = -radius; dx <= radius; dx++) {
        Chunk *c = chunk_find(ccx + dx, ccz + dz); bool temp = false; if (!c) { c = chunk_new(ccx + dx, ccz + dz); temp = true; }
        if (chunk_save(c)) n++;
        if (temp) chunk_free(c);
    }
    return n;
}
void world_stats(KcValue o) {
    size_t bytes = (size_t)W.nlist * ((size_t)CS * CS * (size_t)W.H + 2 * CS * CS + sizeof(Chunk));
    obj_int(o, "chunks_loaded", W.nlist); obj_int(o, "chunks_needed", W.need_total); obj_int(o, "chunks_ready", W.need_ready);
    obj_int(o, "chunks_loading", W.need_total - W.need_ready); obj_int(o, "chunk_loads", W.loads); obj_int(o, "chunk_unloads", W.unloads);
    obj_int(o, "chunks_from_disk", W.from_disk); obj_int(o, "chunks_generated", W.generated); obj_int(o, "chunks_saved", W.saves); obj_int(o, "chunk_meshes", W.meshes);
    obj_int(o, "world_bytes", (int64_t)bytes); obj_int(o, "mesh_bytes", (int64_t)W.quad_bytes); obj_num(o, "stream_ms", G.st_stream_ms); obj_num(o, "fog_horizon", W.horizon);
    obj_int(o, "radius", W.radius);
}
void world_counts(int *loaded, int *needed, int *ready, size_t *bytes) {
    *loaded = W.nlist; *needed = W.need_total; *ready = W.need_ready;
    *bytes = (size_t)W.nlist * ((size_t)CS * CS * (size_t)W.H) + W.quad_bytes;
}
void game_world_defaults(void) { defaults_init(); }
