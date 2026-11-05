/* std.game — meshes, the mesh builder, the built-in "custom" model library,
 * the model registry (lazy load + LRU eviction) and the .kcm text format. */
#include "game_internal.h"
#include <ctype.h>
#if defined(_WIN32)
#  define strcasecmp _stricmp
#else
#  include <strings.h>
#endif

/* ================================================================ builder */
static struct {
    Vtx *v; int nv, cv; uint32_t *idx; int ni, ci; Part *parts; int np, cp; bool has_part;
    int *own; int nown, cown; bool force_flat;
} MB;

static void mb_free_arrays(void) {
    free(MB.v); free(MB.idx); free(MB.parts); free(MB.own);
    memset(&MB, 0, sizeof MB);
}
void mb_begin(void) { mb_free_arrays(); }
int mb_vcount(void) { return MB.nv; }
void mb_flat_normals(bool on) { MB.force_flat = on; }
void mb_own_tex(int id) {
    if (MB.nown == MB.cown) { MB.cown = MB.cown ? MB.cown * 2 : 8; MB.own = kc_realloc(MB.own, sizeof(int) * (size_t)MB.cown); }
    MB.own[MB.nown++] = id;
}
int mb_vertex(V3 p, V3 n, float u, float v, uint32_t c, bool has_n) {
    if (MB.nv == MB.cv) { MB.cv = MB.cv ? MB.cv * 2 : 256; MB.v = kc_realloc(MB.v, sizeof(Vtx) * (size_t)MB.cv); }
    Vtx *q = &MB.v[MB.nv]; q->p = p; q->n = has_n ? n : v3(0, 0, 0); q->u = u; q->v = v; q->c = c;
    return MB.nv++;
}
static void part_push(Part p) {
    if (MB.np == MB.cp) { MB.cp = MB.cp ? MB.cp * 2 : 8; MB.parts = kc_realloc(MB.parts, sizeof(Part) * (size_t)MB.cp); }
    MB.parts[MB.np++] = p;
}
void mb_part(int tex, uint32_t col, bool two_sided, bool translucent, bool unlit) {
    if (MB.has_part && MB.np > 0) {
        Part *last = &MB.parts[MB.np - 1];
        last->count = (uint32_t)MB.ni - last->first;
        if (last->count == 0) { MB.np--; }                      /* replace an empty part */
    }
    Part p = { (uint32_t)MB.ni, 0, tex, col, two_sided, translucent, unlit };
    part_push(p); MB.has_part = true;
}
void mb_tri(int a, int b, int c) {
    if (a < 0 || b < 0 || c < 0 || a >= MB.nv || b >= MB.nv || c >= MB.nv) return;
    if (!MB.has_part) mb_part(-1, 0xFFFFFFFFu, false, false, false);
    if (MB.ni + 3 > MB.ci) { MB.ci = MB.ci ? MB.ci * 2 : 768; MB.idx = kc_realloc(MB.idx, sizeof(uint32_t) * (size_t)MB.ci); }
    MB.idx[MB.ni++] = (uint32_t)a; MB.idx[MB.ni++] = (uint32_t)b; MB.idx[MB.ni++] = (uint32_t)c;
}
Mesh *mb_end(void) {
    if (MB.has_part && MB.np > 0) MB.parts[MB.np - 1].count = (uint32_t)MB.ni - MB.parts[MB.np - 1].first;
    int w = 0;
    for (int i = 0; i < MB.np; i++) if (MB.parts[i].count) MB.parts[w++] = MB.parts[i];
    MB.np = w;
    /* normals for vertices that have none (or all of them when forced flat) */
    bool *need = kc_malloc((size_t)(MB.nv ? MB.nv : 1)); int nneed = 0;
    for (int i = 0; i < MB.nv; i++) { need[i] = MB.force_flat || (MB.v[i].n.x == 0 && MB.v[i].n.y == 0 && MB.v[i].n.z == 0); if (need[i]) { MB.v[i].n = v3(0, 0, 0); nneed++; } }
    if (nneed) {
        for (int t = 0; t + 2 < MB.ni; t += 3) {
            uint32_t a = MB.idx[t], b = MB.idx[t + 1], c = MB.idx[t + 2];
            V3 fn = v3cross(v3sub(MB.v[b].p, MB.v[a].p), v3sub(MB.v[c].p, MB.v[a].p));
            if (need[a]) MB.v[a].n = v3add(MB.v[a].n, fn);
            if (need[b]) MB.v[b].n = v3add(MB.v[b].n, fn);
            if (need[c]) MB.v[c].n = v3add(MB.v[c].n, fn);
        }
        for (int i = 0; i < MB.nv; i++) if (need[i]) { V3 n = v3norm(MB.v[i].n); MB.v[i].n = (n.x || n.y || n.z) ? n : v3(0, 1, 0); }
    }
    free(need);
    Mesh *m = kc_malloc(sizeof *m); memset(m, 0, sizeof *m);
    m->v = MB.v; m->nv = MB.nv; m->idx = MB.idx; m->ni = MB.ni; m->parts = MB.parts; m->np = MB.np; m->own_tex = MB.own; m->n_own = MB.nown;
    if (m->nv) { m->bmin = m->bmax = m->v[0].p; for (int i = 1; i < m->nv; i++) { V3 p = m->v[i].p;
        if (p.x < m->bmin.x) m->bmin.x = p.x; if (p.y < m->bmin.y) m->bmin.y = p.y; if (p.z < m->bmin.z) m->bmin.z = p.z;
        if (p.x > m->bmax.x) m->bmax.x = p.x; if (p.y > m->bmax.y) m->bmax.y = p.y; if (p.z > m->bmax.z) m->bmax.z = p.z; } }
    m->bytes = sizeof(Mesh) + sizeof(Vtx) * (size_t)m->nv + 4 * (size_t)m->ni + sizeof(Part) * (size_t)m->np;
    memset(&MB, 0, sizeof MB);
    return m;
}
void mesh_free(Mesh *m) {
    if (!m) return;
    for (int i = 0; i < m->n_own; i++) tex_free(m->own_tex[i]);
    free(m->v); free(m->idx); free(m->parts); free(m->own_tex); free(m);
}

/* ============================================================== primitives */
static M4 CUR; static bool CUR_ID = true;
static void xf_set(V3 p, V3 rot, V3 s) { CUR = m4_trs(p, rot, s); CUR_ID = false; }
static void xf_reset(void) { CUR_ID = true; }
static int pv(V3 p, V3 n, float u, float v, uint32_t c) {
    if (!CUR_ID) { p = m4_point(&CUR, p); n = v3norm(m4_dir(&CUR, n)); }
    return mb_vertex(p, n, u, v, c, true);
}
static void quad4(int a, int b, int c, int d) { mb_tri(a, b, c); mb_tri(a, c, d); }

static void p_box(V3 s, uint32_t c) {
    float x = s.x * 0.5f, y = s.y * 0.5f, z = s.z * 0.5f;
    struct { V3 n; V3 a, b, c2, d; } F[6] = {
        { {0, 0, 1},  {-x, -y, z},  {x, -y, z},   {x, y, z},   {-x, y, z} },
        { {0, 0, -1}, {x, -y, -z},  {-x, -y, -z}, {-x, y, -z}, {x, y, -z} },
        { {1, 0, 0},  {x, -y, z},   {x, -y, -z},  {x, y, -z},  {x, y, z} },
        { {-1, 0, 0}, {-x, -y, -z}, {-x, -y, z},  {-x, y, z},  {-x, y, -z} },
        { {0, 1, 0},  {-x, y, z},   {x, y, z},    {x, y, -z},  {-x, y, -z} },
        { {0, -1, 0}, {-x, -y, -z}, {x, -y, -z},  {x, -y, z},  {-x, -y, z} } };
    for (int f = 0; f < 6; f++) {
        int a = pv(F[f].a, F[f].n, 0, 1, c), b = pv(F[f].b, F[f].n, 1, 1, c), cc = pv(F[f].c2, F[f].n, 1, 0, c), d = pv(F[f].d, F[f].n, 0, 0, c);
        quad4(a, b, cc, d);
    }
}
/* frustum of a cone along +Y from y=0 to h */
static void p_cyl(float r0, float r1, float h, int seg, uint32_t c, bool cap_top, bool cap_bot) {
    if (seg < 3) seg = 3;
    float slope = (r0 - r1) / (h > 1e-6f ? h : 1e-6f);
    int first = mb_vcount();
    for (int i = 0; i <= seg; i++) {
        float a = (float)i / seg * 6.2831853f, ca = cosf(a), sa = sinf(a); V3 n = v3norm(v3(ca, slope, sa));
        pv(v3(ca * r0, 0, sa * r0), n, (float)i / seg, 1, c); pv(v3(ca * r1, h, sa * r1), n, (float)i / seg, 0, c);
    }
    for (int i = 0; i < seg; i++) { int a = first + i * 2; mb_tri(a, a + 2, a + 1); mb_tri(a + 1, a + 2, a + 3); }
    for (int cap = 0; cap < 2; cap++) {
        float r = cap ? r1 : r0, y = cap ? h : 0; if ((cap && !cap_top) || (!cap && !cap_bot) || r <= 1e-6f) continue;
        V3 n = v3(0, cap ? 1 : -1, 0); int ctr = pv(v3(0, y, 0), n, 0.5f, 0.5f, c), f0 = mb_vcount();
        for (int i = 0; i <= seg; i++) { float a = (float)i / seg * 6.2831853f; pv(v3(cosf(a) * r, y, sinf(a) * r), n, 0.5f + 0.5f * cosf(a), 0.5f + 0.5f * sinf(a), c); }
        for (int i = 0; i < seg; i++) { if (cap) mb_tri(ctr, f0 + i + 1, f0 + i); else mb_tri(ctr, f0 + i, f0 + i + 1); }
    }
}
static void p_sphere(V3 rad, int rings, int seg, uint32_t c, float jitter) {
    if (rings < 2) rings = 2; if (seg < 3) seg = 3;
    int first = mb_vcount();
    for (int j = 0; j <= rings; j++) {
        float t = (float)j / rings, phi = t * 3.14159265f, y = cosf(phi), rr = sinf(phi);
        for (int i = 0; i <= seg; i++) {
            float a = (float)i / seg * 6.2831853f; V3 n = v3(cosf(a) * rr, y, sinf(a) * rr);
            float k = 1.0f; if (jitter > 0) k += (vnoise3(n.x * 2.3f + 5, n.y * 2.3f + 5, n.z * 2.3f + 5, 17) - 0.5f) * jitter;
            pv(v3(n.x * rad.x * k, n.y * rad.y * k, n.z * rad.z * k), v3norm(v3(n.x / (rad.x > 1e-6f ? rad.x : 1), n.y / (rad.y > 1e-6f ? rad.y : 1), n.z / (rad.z > 1e-6f ? rad.z : 1))), (float)i / seg, t, c);
        }
    }
    for (int j = 0; j < rings; j++) for (int i = 0; i < seg; i++) {
        int a = first + j * (seg + 1) + i, b = a + seg + 1;
        if (j > 0) mb_tri(a, a + 1, b);
        if (j < rings - 1) mb_tri(a + 1, b + 1, b);
    }
}
static void p_plane(float w, float d, uint32_t c) {
    V3 n = v3(0, 1, 0);
    int a = pv(v3(-w / 2, 0, d / 2), n, 0, 1, c), b = pv(v3(w / 2, 0, d / 2), n, 1, 1, c), cc = pv(v3(w / 2, 0, -d / 2), n, 1, 0, c), dd = pv(v3(-w / 2, 0, -d / 2), n, 0, 0, c);
    quad4(a, b, cc, dd);
}
static void p_torus(float R, float r, int seg, int sides, uint32_t c) {
    int first = mb_vcount();
    for (int i = 0; i <= seg; i++) {
        float a = (float)i / seg * 6.2831853f, ca = cosf(a), sa = sinf(a);
        for (int j = 0; j <= sides; j++) {
            float b = (float)j / sides * 6.2831853f, cb = cosf(b), sb = sinf(b);
            pv(v3((R + r * cb) * ca, r * sb, (R + r * cb) * sa), v3(cb * ca, sb, cb * sa), (float)i / seg, (float)j / sides, c);
        }
    }
    for (int i = 0; i < seg; i++) for (int j = 0; j < sides; j++) { int a = first + i * (sides + 1) + j, b = a + sides + 1; mb_tri(a, a + 1, b); mb_tri(a + 1, b + 1, b); }
}

/* ============================================================ built-in models */
#define P(x, y, z) v3(x, y, z)
#define COL(r, g, b) (0xFF000000u | (uint32_t)(r) << 16 | (uint32_t)(g) << 8 | (uint32_t)(b))
#define AT(x, y, z) xf_set(v3(x, y, z), v3(0, 0, 0), v3(1, 1, 1))
#define ATR(x, y, z, rx, ry, rz) xf_set(v3(x, y, z), v3(rx, ry, rz), v3(1, 1, 1))

static void bi_cube(void)     { AT(0, .5f, 0); p_box(P(1, 1, 1), COL(220, 220, 220)); }
static void bi_sphere(void)   { AT(0, .5f, 0); p_sphere(P(.5f, .5f, .5f), 16, 24, COL(220, 220, 220), 0); }
static void bi_cylinder(void) { xf_reset(); p_cyl(.5f, .5f, 1, 24, COL(220, 220, 220), true, true); }
static void bi_cone(void)     { xf_reset(); p_cyl(.5f, 0, 1, 24, COL(220, 220, 220), false, true); }
static void bi_pyramid(void)  { ATR(0, 0, 0, 0, 45, 0); p_cyl(.7071f, 0, 1, 4, COL(230, 200, 120), false, true); }
static void bi_plane(void)    { xf_reset(); p_plane(1, 1, COL(200, 200, 200)); }
static void bi_quad(void)     { AT(0, .5f, 0); V3 n = v3(0, 0, 1); int a = pv(P(-.5f, -.5f, 0), n, 0, 1, 0xFFFFFFFF), b = pv(P(.5f, -.5f, 0), n, 1, 1, 0xFFFFFFFF), c = pv(P(.5f, .5f, 0), n, 1, 0, 0xFFFFFFFF), d = pv(P(-.5f, .5f, 0), n, 0, 0, 0xFFFFFFFF); mb_part(-1, 0xFFFFFFFF, true, false, false); quad4(a, b, c, d); }
static void bi_torus(void)    { AT(0, .5f, 0); p_torus(.35f, .15f, 28, 14, COL(230, 180, 60)); }
static void bi_capsule(void)  { AT(0, .5f, 0); p_cyl(.3f, .3f, .8f, 20, COL(120, 170, 230), false, false); AT(0, 1.3f, 0); p_sphere(P(.3f, .3f, .3f), 10, 20, COL(120, 170, 230), 0); AT(0, .5f, 0); p_sphere(P(.3f, .3f, .3f), 10, 20, COL(120, 170, 230), 0); }
static void bi_stairs(void)   { for (int i = 0; i < 4; i++) { AT(0, .125f + i * .25f, -.375f + i * .25f); p_box(P(1, .25f + i * .25f, .25f), COL(170 + i * 10, 170 + i * 10, 175 + i * 10)); } }
static void bi_arch(void)     { AT(-.4f, .5f, 0); p_box(P(.2f, 1, .3f), COL(190, 185, 175)); AT(.4f, .5f, 0); p_box(P(.2f, 1, .3f), COL(190, 185, 175)); AT(0, 1.1f, 0); p_box(P(1, .2f, .3f), COL(200, 195, 185)); AT(0, 1.25f, 0); p_box(P(.4f, .12f, .3f), COL(210, 205, 195)); }
static void bi_pine(void) {
    AT(0, 0, 0); p_cyl(.18f, .14f, 1.2f, 8, COL(100, 70, 40), false, true);
    for (int i = 0; i < 4; i++) { AT(0, .9f + i * .9f, 0); p_cyl(1.5f - i * .3f, 0.05f, 1.6f, 10, COL(30 + i * 6, 100 + i * 10, 55), false, true); }
}
static void bi_tree(void) {
    AT(0, 0, 0); p_cyl(.22f, .16f, 2.4f, 8, COL(110, 78, 44), false, true);
    AT(0, 3.2f, 0); p_sphere(P(1.5f, 1.3f, 1.5f), 8, 12, COL(64, 150, 62), .35f);
    AT(.7f, 2.6f, .3f); p_sphere(P(1, .9f, 1), 8, 12, COL(76, 165, 70), .3f);
    AT(-.6f, 2.8f, -.4f); p_sphere(P(.9f, .8f, .9f), 8, 12, COL(56, 138, 58), .3f);
}
static void bi_bush(void) { AT(0, .4f, 0); p_sphere(P(.6f, .45f, .6f), 8, 12, COL(58, 140, 60), .4f); AT(.45f, .3f, .2f); p_sphere(P(.4f, .32f, .4f), 7, 10, COL(70, 155, 66), .4f); AT(-.4f, .3f, -.2f); p_sphere(P(.42f, .3f, .4f), 7, 10, COL(52, 130, 56), .4f); }
static void bi_rock(void) { AT(0, .35f, 0); p_sphere(P(.6f, .4f, .5f), 9, 14, COL(130, 130, 136), .7f); AT(.5f, .15f, .3f); p_sphere(P(.25f, .18f, .22f), 6, 9, COL(112, 112, 118), .6f); }
static void bi_house(void) {
    AT(0, 1, 0); p_box(P(4, 2, 3), COL(226, 208, 170));
    ATR(0, 2, 0, 0, 0, 0); xf_set(v3(0, 2, 0), v3(0, 90, 0), v3(1, 1, 1)); p_cyl(2.7f, 0, 1.4f, 4, COL(158, 66, 52), false, true);
    AT(0, .8f, 1.51f); p_box(P(.8f, 1.6f, .1f), COL(100, 66, 38));
    AT(-1.2f, 1.2f, 1.51f); p_box(P(.7f, .7f, .08f), COL(120, 170, 215)); AT(1.2f, 1.2f, 1.51f); p_box(P(.7f, .7f, .08f), COL(120, 170, 215));
    AT(1.3f, 2.9f, -.4f); p_box(P(.4f, 1, .4f), COL(130, 120, 112));
}
static void bi_tower(void) { xf_reset(); p_cyl(1.2f, 1.0f, 5, 16, COL(170, 165, 158), false, true); AT(0, 5, 0); p_cyl(1.3f, 1.3f, .5f, 16, COL(150, 145, 140), true, false); AT(0, 5.5f, 0); p_cyl(1.3f, 0, 1.6f, 16, COL(150, 60, 50), false, false); AT(0, 2.5f, 1.05f); p_box(P(.4f, .8f, .2f), COL(60, 50, 45)); }
static void bi_fence(void) { for (int i = 0; i < 3; i++) { AT(-1 + i, .5f, 0); p_box(P(.12f, 1, .12f), COL(140, 100, 60)); } AT(0, .75f, 0); p_box(P(2.1f, .1f, .06f), COL(160, 115, 70)); AT(0, .35f, 0); p_box(P(2.1f, .1f, .06f), COL(160, 115, 70)); }
static void bi_lamp(void) { xf_reset(); p_cyl(.08f, .05f, 3, 8, COL(60, 60, 66), false, true); AT(0, 3.15f, 0); p_box(P(.35f, .3f, .35f), COL(255, 238, 170)); mb_part(-1, 0xFFFFFFFF, false, false, true); AT(0, 3.15f, 0); p_box(P(.3f, .25f, .3f), COL(255, 244, 190)); }
static void bi_crate(void) { AT(0, .5f, 0); p_box(P(1, 1, 1), COL(176, 128, 72)); for (int s = 0; s < 4; s++) { float a = s * 90.0f; xf_set(v3(0, .5f, 0), v3(0, a, 0), v3(1, 1, 1)); V3 sz = P(1.03f, 1.03f, .08f); (void)sz; } AT(0, .5f, 0); p_box(P(1.04f, .1f, 1.04f), COL(120, 82, 44)); AT(0, .08f, 0); p_box(P(1.04f, .1f, 1.04f), COL(120, 82, 44)); AT(0, .92f, 0); p_box(P(1.04f, .1f, 1.04f), COL(120, 82, 44)); }
static void bi_barrel(void) { xf_reset(); p_cyl(.4f, .5f, .5f, 14, COL(140, 92, 52), false, true); AT(0, .5f, 0); p_cyl(.5f, .4f, .5f, 14, COL(140, 92, 52), true, false); AT(0, .12f, 0); p_cyl(.44f, .44f, .06f, 14, COL(70, 70, 76), false, false); AT(0, .82f, 0); p_cyl(.44f, .44f, .06f, 14, COL(70, 70, 76), false, false); }
static void bi_flower(void) { xf_reset(); p_cyl(.03f, .03f, .6f, 6, COL(60, 140, 60), false, true); for (int i = 0; i < 6; i++) { float a = i * 60.0f * 0.0174533f; AT(cosf(a) * .12f, .66f, sinf(a) * .12f); p_sphere(P(.09f, .05f, .09f), 5, 7, COL(235, 80 + i * 10, 120), 0); } AT(0, .68f, 0); p_sphere(P(.07f, .05f, .07f), 5, 7, COL(250, 210, 60), 0); }
static void bi_mushroom(void) { xf_reset(); p_cyl(.12f, .1f, .4f, 10, COL(235, 225, 205), false, true); AT(0, .4f, 0); p_sphere(P(.35f, .25f, .35f), 8, 14, COL(200, 50, 45), 0); AT(.15f, .55f, .1f); p_sphere(P(.06f, .03f, .06f), 4, 6, COL(250, 245, 235), 0); AT(-.12f, .52f, -.15f); p_sphere(P(.05f, .03f, .05f), 4, 6, COL(250, 245, 235), 0); }
static void bi_humanoid(void) {
    uint32_t skin = COL(232, 190, 150), shirt = COL(60, 110, 190), pants = COL(60, 62, 84), shoe = COL(40, 36, 34);
    AT(-.12f, .45f, 0); p_box(P(.16f, .9f, .2f), pants); AT(.12f, .45f, 0); p_box(P(.16f, .9f, .2f), pants);
    AT(-.12f, .06f, .04f); p_box(P(.17f, .12f, .3f), shoe); AT(.12f, .06f, .04f); p_box(P(.17f, .12f, .3f), shoe);
    AT(0, 1.2f, 0); p_box(P(.5f, .6f, .26f), shirt);
    AT(-.34f, 1.2f, 0); p_box(P(.14f, .56f, .16f), shirt); AT(.34f, 1.2f, 0); p_box(P(.14f, .56f, .16f), shirt);
    AT(-.34f, .88f, 0); p_box(P(.13f, .12f, .15f), skin); AT(.34f, .88f, 0); p_box(P(.13f, .12f, .15f), skin);
    AT(0, 1.65f, 0); p_sphere(P(.2f, .22f, .2f), 10, 14, skin, 0);
    AT(0, 1.74f, -.02f); p_sphere(P(.205f, .15f, .2f), 8, 12, COL(70, 46, 30), 0);
}
static void bi_car(void) {
    AT(0, .45f, 0); p_box(P(1.8f, .5f, 3.8f), COL(200, 50, 50)); AT(0, .9f, -.2f); p_box(P(1.5f, .5f, 1.9f), COL(190, 45, 45));
    AT(0, .92f, -.2f); p_box(P(1.52f, .32f, 1.5f), COL(120, 170, 210));
    for (int i = 0; i < 4; i++) { float x = (i & 1) ? .95f : -.95f, z = (i & 2) ? 1.25f : -1.25f; xf_set(v3(x, .32f, z), v3(0, 0, 90), v3(1, 1, 1)); p_cyl(.32f, .32f, .22f, 14, COL(30, 30, 34), true, true); }
    AT(-.6f, .55f, 1.91f); p_box(P(.3f, .15f, .06f), COL(255, 240, 180)); AT(.6f, .55f, 1.91f); p_box(P(.3f, .15f, .06f), COL(255, 240, 180));
}
static void bi_cloud(void) { mb_part(-1, 0xFFFFFFFF, false, false, true); uint32_t w = COL(250, 252, 255); AT(0, 0, 0); p_sphere(P(1.4f, .8f, 1), 8, 12, w, .2f); AT(1.2f, -.1f, .2f); p_sphere(P(1, .65f, .8f), 8, 12, w, .2f); AT(-1.2f, -.15f, -.1f); p_sphere(P(1, .6f, .8f), 8, 12, w, .2f); AT(.2f, .3f, -.2f); p_sphere(P(1, .7f, .8f), 8, 12, w, .2f); }
static void bi_gem(void) { AT(0, .5f, 0); p_sphere(P(.35f, .5f, .35f), 2, 6, COL(70, 220, 230), 0); }
static void bi_arrow(void) { xf_reset(); p_cyl(.05f, .05f, .7f, 8, COL(200, 60, 60), false, true); AT(0, .7f, 0); p_cyl(.15f, 0, .3f, 10, COL(220, 70, 70), false, true); }
static void bi_coin(void) { ATR(0, .5f, 0, 90, 0, 0); p_cyl(.4f, .4f, .08f, 24, COL(250, 200, 60), true, true); }
static void bi_campfire(void) { for (int i = 0; i < 5; i++) { xf_set(v3(0, .12f, 0), v3(80, i * 72.0f, 0), v3(1, 1, 1)); p_cyl(.07f, .07f, .7f, 7, COL(100, 66, 36), true, true); } mb_part(-1, 0xFFFFFFFF, false, false, true); AT(0, .12f, 0); p_cyl(.22f, 0, .6f, 8, COL(255, 150, 40), false, true); AT(0, .12f, 0); p_cyl(.12f, 0, .45f, 8, COL(255, 220, 90), false, true); }
static void bi_sign(void) { xf_reset(); p_cyl(.05f, .05f, 1.2f, 6, COL(110, 78, 44), false, true); AT(0, 1.0f, .02f); p_box(P(.9f, .45f, .06f), COL(176, 128, 72)); }

static const struct { const char *name; void (*fn)(void); } BUILTINS[] = {
    {"cube", bi_cube}, {"sphere", bi_sphere}, {"cylinder", bi_cylinder}, {"cone", bi_cone}, {"pyramid", bi_pyramid}, {"plane", bi_plane},
    {"quad", bi_quad}, {"torus", bi_torus}, {"capsule", bi_capsule}, {"stairs", bi_stairs}, {"arch", bi_arch}, {"pine", bi_pine},
    {"tree", bi_tree}, {"bush", bi_bush}, {"rock", bi_rock}, {"house", bi_house}, {"tower", bi_tower}, {"fence", bi_fence},
    {"lamp", bi_lamp}, {"crate", bi_crate}, {"barrel", bi_barrel}, {"flower", bi_flower}, {"mushroom", bi_mushroom},
    {"humanoid", bi_humanoid}, {"car", bi_car}, {"cloud", bi_cloud}, {"gem", bi_gem}, {"arrow", bi_arrow}, {"coin", bi_coin},
    {"campfire", bi_campfire}, {"sign", bi_sign},
};
#define N_BUILTIN ((int)(sizeof BUILTINS / sizeof BUILTINS[0]))

KcValue model_builtin_names(void) {
    KcValue a = kc_array_new();
    for (int i = 0; i < N_BUILTIN; i++) kc_array_push(a, kc_string(BUILTINS[i].name));
    return a;
}
KcValue model_formats(void) {
    KcValue a = kc_array_new();
    const char *f[] = { ".obj (+.mtl)", ".gltf / .glb (glTF 2.0)", ".fbx (binary + ASCII, Unreal/Blender/Maya)", ".psk / .pskx (Unreal ActorX static & skeletal mesh)",
                        ".stl (binary + ASCII)", ".ply (ascii + binary)", ".kcm (KingCobra custom model text)" };
    for (size_t i = 0; i < sizeof f / sizeof f[0]; i++) kc_array_push(a, kc_string(f[i]));
    return a;
}

/* =============================================================== registry */
static Model *M; static int nm, cm;
static int builtin_id[64];
static size_t model_bytes;

static int model_slot(void) {
    for (int i = 0; i < nm; i++) if (!M[i].alive) return i;
    if (nm == cm) { cm = cm ? cm * 2 : 64; M = kc_realloc(M, sizeof(Model) * (size_t)cm); }
    return nm++;
}
Model *model_info(int id) { return (id >= 0 && id < nm && M[id].alive) ? &M[id] : NULL; }
int model_count(void) { int n = 0; for (int i = 0; i < nm; i++) if (M[i].alive) n++; return n; }
size_t model_resident_bytes(void) { return model_bytes; }

int model_from_mesh(Mesh *me, const char *name) {
    int id = model_slot(); Model *m = &M[id]; memset(m, 0, sizeof *m);
    m->alive = true; m->name = kc_strdup(name ? name : "model"); m->mesh = me; m->scale = 1; m->tint = 0xFFFFFFFFu; m->tex_override = -1; m->last = G.frame;
    if (me) { snprintf(me->fmt, sizeof me->fmt, "custom"); model_bytes += me->bytes; }
    return id;
}
int model_builtin(const char *name) {
    for (int i = 0; i < N_BUILTIN; i++) if (!strcmp(BUILTINS[i].name, name)) {
        if (builtin_id[i] > 0 && M[builtin_id[i] - 1].alive) return builtin_id[i] - 1;
        mb_begin(); xf_reset(); BUILTINS[i].fn(); xf_reset();
        Mesh *me = mb_end(); int id = model_from_mesh(me, BUILTINS[i].name); M[id].builtin = true; snprintf(me->fmt, sizeof me->fmt, "builtin");
        builtin_id[i] = id + 1; return id;
    }
    for (int i = 0; i < nm; i++) if (M[i].alive && M[i].name && !strcmp(M[i].name, name)) return i;     /* user-named custom model */
    return -1;
}
void model_free(int id) {
    Model *m = model_info(id); if (!m) return;
    if (m->mesh) { model_bytes -= m->mesh->bytes; mesh_free(m->mesh); }
    for (int i = 0; i < N_BUILTIN; i++) if (builtin_id[i] == id + 1) builtin_id[i] = 0;
    free(m->name); free(m->path); memset(m, 0, sizeof *m);
}

/* ---- .kcm text format ----
 *   kcmodel 1
 *   part [color=#rrggbb] [tex=file.png] [twosided] [unlit] [translucent]
 *   v x y z [u v] [color=#rrggbb]          (vertex, index = order of appearance, from 0)
 *   n nx ny nz                             (normal of the last vertex)
 *   f a b c [d ...]                        (polygon, fan-triangulated)
 *   box sx sy sz [color=..] [at=x,y,z] [rot=x,y,z]   cyl r0 r1 h [seg=] ...  sphere rx ry rz ...  cone r h ...  plane w d ...  torus R r ...
 */
static uint32_t kc_hexcol(const char *s) {
    if (*s == '#') s++;
    unsigned long v = strtoul(s, NULL, 16); size_t l = strlen(s);
    if (l == 6) return 0xFF000000u | (uint32_t)v;
    if (l == 8) return (uint32_t)v;
    if (l == 3) { int r = (v >> 8) & 15, g = (v >> 4) & 15, b = v & 15; return COL(r * 17, g * 17, b * 17); }
    return 0xFFFFFFFFu;
}
static bool kcm_parse(const char *text, const char *dir, char *err) {
    char *buf = kc_strdup(text), *save = NULL; int lineno = 0; int base = 0; bool ok = true;
    for (char *line = strtok_r(buf, "\n", &save); line && ok; line = strtok_r(NULL, "\n", &save)) {
        lineno++;
        char *c = strchr(line, '#'); if (c && (c == line || c[-1] == ' ') && c[1] == ' ') *c = 0;
        if (line[0] == '/' && line[1] == '/') continue;
        char *tok[40]; int nt = 0; char *sv2 = NULL;
        for (char *t = strtok_r(line, " \t\r", &sv2); t && nt < 40; t = strtok_r(NULL, " \t\r", &sv2)) tok[nt++] = t;
        if (!nt) continue;
        const char *k = tok[0];
        uint32_t col = 0xFFFFFFFFu; V3 at = v3(0, 0, 0), rot = v3(0, 0, 0); int seg = 16; int tex = -1; bool two = false, unlit = false, trans = false;
        double nums[16]; int nn = 0;
        for (int i = 1; i < nt; i++) {
            if (!strncmp(tok[i], "color=", 6)) col = kc_hexcol(tok[i] + 6);
            else if (!strncmp(tok[i], "at=", 3)) sscanf(tok[i] + 3, "%f,%f,%f", &at.x, &at.y, &at.z);
            else if (!strncmp(tok[i], "rot=", 4)) sscanf(tok[i] + 4, "%f,%f,%f", &rot.x, &rot.y, &rot.z);
            else if (!strncmp(tok[i], "seg=", 4)) seg = atoi(tok[i] + 4);
            else if (!strncmp(tok[i], "tex=", 4)) { char *p = path_join(dir, tok[i] + 4); tex = tex_file(p); free(p); }
            else if (!strcmp(tok[i], "twosided")) two = true;
            else if (!strcmp(tok[i], "unlit")) unlit = true;
            else if (!strcmp(tok[i], "translucent")) trans = true;
            else if (nn < 16 && (isdigit((unsigned char)tok[i][0]) || tok[i][0] == '-' || tok[i][0] == '.')) nums[nn++] = atof(tok[i]);
        }
        if (!strcmp(k, "kcmodel")) continue;
        else if (!strcmp(k, "part")) mb_part(tex, col, two, trans, unlit);
        else if (!strcmp(k, "v")) {
            if (nn < 3) { snprintf(err, 256, "line %d: v needs x y z", lineno); ok = false; break; }
            mb_vertex(v3((float)nums[0], (float)nums[1], (float)nums[2]), v3(0, 0, 0), nn >= 5 ? (float)nums[3] : 0, nn >= 5 ? (float)nums[4] : 0, col, false);
        } else if (!strcmp(k, "n")) {
            int last = mb_vcount() - 1; (void)last; /* normals via builder not retro-settable: handled by recompute */
        } else if (!strcmp(k, "f")) {
            int idx[40], ni = 0; for (int i = 1; i < nt && ni < 40; i++) if (isdigit((unsigned char)tok[i][0])) idx[ni++] = atoi(tok[i]);
            if (ni < 3) { snprintf(err, 256, "line %d: f needs at least 3 indices", lineno); ok = false; break; }
            for (int i = 1; i + 1 < ni; i++) mb_tri(idx[0] + base, idx[i] + base, idx[i + 1] + base);
        } else if (!strcmp(k, "box") || !strcmp(k, "cyl") || !strcmp(k, "sphere") || !strcmp(k, "cone") || !strcmp(k, "plane") || !strcmp(k, "torus")) {
            if (tex != -1 || two || unlit || trans) mb_part(tex, 0xFFFFFFFFu, two, trans, unlit);
            xf_set(at, rot, v3(1, 1, 1));
            if (!strcmp(k, "box") && nn >= 3) p_box(v3((float)nums[0], (float)nums[1], (float)nums[2]), col);
            else if (!strcmp(k, "cyl") && nn >= 3) p_cyl((float)nums[0], (float)nums[1], (float)nums[2], seg, col, true, true);
            else if (!strcmp(k, "cone") && nn >= 2) p_cyl((float)nums[0], 0, (float)nums[1], seg, col, false, true);
            else if (!strcmp(k, "sphere") && nn >= 3) p_sphere(v3((float)nums[0], (float)nums[1], (float)nums[2]), imaxi(4, seg / 2), seg, col, 0);
            else if (!strcmp(k, "plane") && nn >= 2) p_plane((float)nums[0], (float)nums[1], col);
            else if (!strcmp(k, "torus") && nn >= 2) p_torus((float)nums[0], (float)nums[1], seg, imaxi(6, seg / 2), col);
            else { snprintf(err, 256, "line %d: bad arguments for %s", lineno, k); ok = false; }
            xf_reset();
        } else { snprintf(err, 256, "line %d: unknown directive '%s'", lineno, k); ok = false; }
    }
    free(buf);
    return ok;
}
bool model_save_kcm(Mesh *m, const char *path) {
    FILE *f = fopen(path, "wb"); if (!f) return false;
    fprintf(f, "kcmodel 1\n");
    for (int i = 0; i < m->nv; i++) fprintf(f, "v %g %g %g %g %g color=#%08x\n", m->v[i].p.x, m->v[i].p.y, m->v[i].p.z, m->v[i].u, m->v[i].v, (unsigned)m->v[i].c);
    for (int p = 0; p < m->np; p++) {
        Part *pt = &m->parts[p];
        fprintf(f, "part color=#%08x%s%s%s\n", (unsigned)pt->col, pt->two_sided ? " twosided" : "", pt->unlit ? " unlit" : "", pt->translucent ? " translucent" : "");
        for (uint32_t t = pt->first; t < pt->first + pt->count; t += 3) fprintf(f, "f %u %u %u\n", m->idx[t], m->idx[t + 1], m->idx[t + 2]);
    }
    return fclose(f) == 0;
}

/* ---- load a mesh from disk (any supported format) ---- */
static const char *ext_of(const char *p) { const char *d = strrchr(p, '.'); return d ? d : ""; }

static Mesh *build_from_file(Model *m, char *err) {
    LoadOpts lo = { m->unreal, m->scale > 0 ? m->scale : 1.0f, m->meters, m->flip_winding, m->flip_uv };
    mb_begin();
    bool ok;
    if (!strcasecmp(ext_of(m->path), ".kcm")) {
        size_t n; char *t = read_file_all(m->path, &n);
        if (!t) { snprintf(err, 256, "cannot read '%s'", m->path); return NULL; }
        char *dir = path_dir(m->path); ok = kcm_parse(t, dir, err); free(dir); free(t);
    } else ok = modelio_load(m->path, &lo, err);
    if (!ok) { mb_begin(); return NULL; }
    if (mb_vcount() == 0) { snprintf(err, 256, "no geometry found in '%s'", m->path); mb_begin(); return NULL; }
    Mesh *me = mb_end();
    snprintf(me->fmt, sizeof me->fmt, "%s", ext_of(m->path) + (ext_of(m->path)[0] ? 1 : 0));
    if (m->center || m->ground) {
        V3 off = v3(m->center ? -(me->bmin.x + me->bmax.x) * .5f : 0, -me->bmin.y, m->center ? -(me->bmin.z + me->bmax.z) * .5f : 0);
        for (int i = 0; i < me->nv; i++) me->v[i].p = v3add(me->v[i].p, off);
        me->bmin = v3add(me->bmin, off); me->bmax = v3add(me->bmax, off);
    }
    return me;
}

Mesh *model_mesh(int id) {
    Model *m = model_info(id); if (!m) return NULL;
    m->last = G.frame;
    if (m->mesh) return m->mesh;
    if (m->failed || !m->path) return NULL;
    char err[256] = "";
    Mesh *me = build_from_file(m, err);
    if (!me) { gm_warn("model '%s': %s", m->path, err); m->failed = true; return NULL; }
    m->mesh = me; model_bytes += me->bytes;
    return me;
}
int model_load(const char *path, const Opts *o) {
    for (int i = 0; i < nm; i++) if (M[i].alive && M[i].path && !strcmp(M[i].path, path) && !o->has_obj && o->n <= o->start) return i;   /* de-dup plain loads */
    const char *ext = ext_of(path);
    if (!strcasecmp(ext, ".uasset") || !strcasecmp(ext, ".umap") || !strcasecmp(ext, ".uexp"))
        gm_err("'%s' is a cooked Unreal package. These are version-locked and usually compressed, so they cannot be read directly. "
               "Export the mesh from the Unreal editor (Asset Actions > Export... as FBX, or glTF) or with ActorX/UModel as .pskx, then load that.", path);
    if (!file_exists(path)) gm_err("model_load: file not found: %s", path);
    int id = model_slot(); Model *m = &M[id]; memset(m, 0, sizeof *m);
    m->alive = true; m->path = kc_strdup(path); m->last = G.frame; m->tex_override = -1; m->tint = 0xFFFFFFFFu;
    const char *name = opt_str(o, "name", NULL);
    if (name) m->name = kc_strdup(name); else { const char *sl = strrchr(path, '/'); const char *bs = strrchr(path, '\\'); const char *b = sl > bs ? sl + 1 : (bs ? bs + 1 : path); m->name = kc_strdup(b); }
    bool is_psk = !strcasecmp(ext, ".psk") || !strcasecmp(ext, ".pskx");
    m->unreal = opt_bool(o, "unreal", is_psk);
    m->scale = (float)opt_num(o, "scale", 1.0);
    m->meters = opt_bool(o, "meters", true);
    m->flip_uv = opt_bool(o, "flip_uv", false); m->flip_winding = opt_bool(o, "flip_winding", false);
    m->center = opt_bool(o, "center", false); m->ground = opt_bool(o, "ground", false);
    m->tint = opt_col(o, "tint", 0xFFFFFFFFu);
    m->lazy = opt_bool(o, "lazy", true);
    if (!m->lazy) {
        char err[256] = ""; Mesh *me = build_from_file(m, err);
        if (!me) { char msg[256]; snprintf(msg, sizeof msg, "%s", err); model_free(id); gm_err("model_load('%s'): %s", path, msg); }
        m->mesh = me; model_bytes += me->bytes;
    }
    return id;
}
void model_evict(size_t budget) {
    while (model_bytes > budget) {
        int best = -1; uint64_t bl = ~0ull;
        for (int i = 0; i < nm; i++) if (M[i].alive && M[i].mesh && M[i].path && !M[i].builtin && M[i].last + 60 < G.frame && M[i].last < bl) { bl = M[i].last; best = i; }
        if (best < 0) break;
        model_bytes -= M[best].mesh->bytes; mesh_free(M[best].mesh); M[best].mesh = NULL;
    }
}

int model_from_kcm(const char *text, const char *name, char *err) {
    mb_begin();
    if (!kcm_parse(text, "", err)) { mb_begin(); return -1; }
    if (mb_vcount() == 0) { snprintf(err, 256, "model text defines no geometry"); mb_begin(); return -1; }
    Mesh *me = mb_end(); int id = model_from_mesh(me, name); snprintf(me->fmt, sizeof me->fmt, "kcm"); return id;
}
