/* std.game — software 3D pipeline.
 *
 * Right-handed, +Y up, camera looks down -Z at yaw 0. Triangles are front-facing when
 * counter-clockwise. Per-vertex lighting (Lambert + ambient), perspective-correct
 * texturing with alpha cut-out, 1/w depth buffer, per-pixel fog / animated cloud-fog.
 */
#include "game_internal.h"

FogCfg R_fog = { 3, 60.0f, 160.0f, 0.01f, 0xFFC8D8E8u, true, true, 0, 0 };
uint32_t R_sky_top = 0xFF4A86D6u, R_sky_bot = 0xFFBFD9F2u;
V3 R_light_dir = { -0.45f, -0.8f, -0.35f }; uint32_t R_light_col = 0xFFFFFFFFu; float R_ambient = 0.5f;
bool R_sun = true, R_sky_clouds = false;

static M4 s_view, s_proj, s_vp;
static V3 s_fwd, s_right, s_up;
static float s_alpha = 1.0f;
static float s_fs, s_fe, s_finv; static uint32_t s_fcol;
static float s_tanh;

void r3d_set_alpha(float a) { s_alpha = fclampf(a, 0, 1); }
const M4 *r3d_viewproj(void) { return &s_vp; }
const M4 *r3d_view(void) { return &s_view; }
V3 r3d_forward(void) { return s_fwd; }
V3 r3d_right(void) { return s_right; }
V3 r3d_up(void) { return s_up; }

void r3d_matrices(void) {
    float cp = cosf(G.pitch), sp = sinf(G.pitch), cy = cosf(G.yaw), sy = sinf(G.yaw);
    s_fwd = v3(-sy * cp, sp, -cy * cp);
    V3 worldup = v3(0, 1, 0);
    V3 r = v3cross(s_fwd, worldup); if (v3len(r) < 1e-5f) r = v3(cy, 0, -sy); r = v3norm(r);
    V3 u = v3cross(r, s_fwd);
    if (G.roll != 0) { float cr = cosf(G.roll), sr = sinf(G.roll); V3 r2 = v3add(v3mul(r, cr), v3mul(u, sr)); V3 u2 = v3sub(v3mul(u, cr), v3mul(r, sr)); r = r2; u = u2; }
    s_right = r; s_up = u;
    M4 v = m4_identity();
    v.m[0] = r.x; v.m[4] = r.y; v.m[8] = r.z;  v.m[12] = -v3dot(r, G.cam);
    v.m[1] = u.x; v.m[5] = u.y; v.m[9] = u.z;  v.m[13] = -v3dot(u, G.cam);
    v.m[2] = -s_fwd.x; v.m[6] = -s_fwd.y; v.m[10] = -s_fwd.z; v.m[14] = v3dot(s_fwd, G.cam);
    s_view = v;
    float aspect = (float)G.w / (float)G.h;
    s_proj = m4_perspective(DEG2RAD(G.fov), aspect, G.znear, G.zfar);
    s_vp = m4_mul(&s_proj, &s_view);
    s_tanh = tanf(DEG2RAD(G.fov) * 0.5f);
    /* fog parameters for this frame */
    float end = R_fog.autofog ? R_fog.cur_end : R_fog.end;
    if (end < 1) end = R_fog.end;
    float start = R_fog.autofog ? end * (R_fog.mode == 3 ? 0.35f : 0.55f) : R_fog.start;
    if (start >= end) start = end * 0.5f;
    s_fs = start; s_fe = end; s_finv = 1.0f / (end - start);
    uint32_t fc = R_fog.auto_color ? R_sky_bot : R_fog.color;
    if (R_fog.mode == 3 && R_fog.auto_color) fc = col_mix(fc, 0xFFFFFFFFu, 0.3f);
    s_fcol = fc;
}

void r3d_fog_update(float safe, float dt) {
    float cfg = R_fog.end;
    if (R_fog.mode == 0) { R_fog.cur_end = cfg; return; }
    float target = cfg;
    if (R_fog.autofog && safe > 0 && safe < target) target = safe;
    if (R_fog.cur_end <= 0) R_fog.cur_end = target;
    if (target < R_fog.cur_end) R_fog.cur_end = lerpf(R_fog.cur_end, target, fclampf(dt * 8.0f, 0.05f, 1.0f));      /* close in fast */
    else R_fog.cur_end = fminf(target, R_fog.cur_end + 50.0f * dt);                                              /* open up slowly */
    R_fog.cloud_t += dt;
}

float r3d_fog_factor(float d, int px, int py) {
    if (R_fog.mode == 0 || d <= s_fs * 0.6f) return 0;
    float f;
    switch (R_fog.mode) {
        case 2: { float k = d * R_fog.density; f = 1.0f - expf(-k * k); break; }
        case 3: {
            f = (d - s_fs) * s_finv;
            if (f > -0.3f) { float n = vnoise2((float)px * 0.035f + R_fog.cloud_t * 0.15f + d * 0.004f, (float)py * 0.06f, 77); f += (n - 0.5f) * 0.5f * fclampf(f + 0.5f, 0, 1); }
            break; }
        default: f = (d - s_fs) * s_finv; break;
    }
    return f < 0 ? 0 : (f > 1 ? 1 : f);
}

/* ------------------------------------------------------------------ sky */
static float cloud_density(V3 dir, float t) {
    if (dir.y <= 0.02f) return 0;
    float k = 1.0f / dir.y; float x = dir.x * k * 0.9f + t * 0.02f, z = dir.z * k * 0.9f;
    float n = fbm2(x, z, 3, 2.0f, 0.5f, 99);
    n = (n - 0.52f) * 3.2f; if (n < 0) n = 0; if (n > 1) n = 1;
    return n * fclampf(dir.y * 3.0f, 0, 1);
}
void r3d_sun_screen(float *sx, float *sy, bool *vis) {
    V3 d = v3norm(v3mul(R_light_dir, -1.0f)); V3 p = v3add(G.cam, v3mul(d, 1000.0f)); float depth;
    *vis = r3d_project(p, sx, sy, &depth);
}
void r3d_begin(void) {
    r3d_matrices();
    int W = G.w, H = G.h;
    float horizon = H * 0.5f + (tanf(G.pitch) / s_tanh) * H * 0.5f;
    for (int y = 0; y < H; y++) {
        float t = horizon > 1 ? (horizon - y) / (horizon * 1.15f + 40.0f) : 1.0f; t = fclampf(t, 0, 1); t = sqrtf(t);
        uint32_t c = (y >= horizon) ? col_mix(R_sky_bot, R_sky_bot, 0) : col_mix(R_sky_bot, R_sky_top, t);
        if (y >= horizon && R_fog.mode == 3 && R_fog.auto_color) c = s_fcol;
        uint32_t *row = G.fb + (size_t)y * W; uint32_t v = c & 0xFFFFFF;
        if (R_sky_clouds && y < horizon) {
            float ay = (1.0f - 2.0f * (y + 0.5f) / H) * s_tanh, aspect = (float)W / H;
            for (int x = 0; x < W; x++) {
                float ax = (2.0f * (x + 0.5f) / W - 1.0f) * s_tanh * aspect;
                V3 dir = v3norm(v3add(s_fwd, v3add(v3mul(s_right, ax), v3mul(s_up, ay))));
                float cd = cloud_density(dir, (float)G.time);
                row[x] = cd > 0.01f ? (col_mix(c, 0xFFFFFFFFu, cd * 0.85f) & 0xFFFFFF) : v;
            }
        } else for (int x = 0; x < W; x++) row[x] = v;
    }
    memset(G.zb, 0, (size_t)W * H * sizeof(float));
    if (R_sun) {
        float sx, sy; bool vis; r3d_sun_screen(&sx, &sy, &vis);
        if (vis) { int r = imaxi(3, H / 28); gfx_circle((int)sx, (int)sy, r + 3, 0x30FFF2C0u, true); gfx_circle((int)sx, (int)sy, r, 0xFFFFF4D0u, true); }
    }
    G.begun = true;
}

/* ------------------------------------------------------------ projection */
bool r3d_project(V3 p, float *sx, float *sy, float *depth) {
    const float *m = s_vp.m;
    float cx = m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12], cy = m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13], cw = m[3] * p.x + m[7] * p.y + m[11] * p.z + m[15];
    if (cw < G.znear) return false;
    float iw = 1.0f / cw;
    *sx = (cx * iw * 0.5f + 0.5f) * G.w; *sy = (0.5f - cy * iw * 0.5f) * G.h; if (depth) *depth = cw;
    return true;
}
bool r3d_aabb_visible(V3 mn, V3 mx) {
    const float *m = s_vp.m; int out[6] = { 0, 0, 0, 0, 0, 0 };
    for (int i = 0; i < 8; i++) {
        V3 p = v3((i & 1) ? mx.x : mn.x, (i & 2) ? mx.y : mn.y, (i & 4) ? mx.z : mn.z);
        float cx = m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12], cy = m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13], cw = m[3] * p.x + m[7] * p.y + m[11] * p.z + m[15];
        out[0] += cx < -cw; out[1] += cx > cw; out[2] += cy < -cw; out[3] += cy > cw; out[4] += cw < G.znear; out[5] += cw > G.zfar;
    }
    for (int k = 0; k < 6; k++) if (out[k] == 8) return false;
    return true;
}
void r3d_ray(float sx, float sy, V3 *org, V3 *dir) {
    float aspect = (float)G.w / G.h, x = (2.0f * sx / G.w - 1.0f) * s_tanh * aspect, y = (1.0f - 2.0f * sy / G.h) * s_tanh;
    *org = G.cam; *dir = v3norm(v3add(s_fwd, v3add(v3mul(s_right, x), v3mul(s_up, y))));
}

/* --------------------------------------------------------------- raster */
typedef struct { float x, y, iw, uw, vw, r, g, b, dw; } SV;

static void raster_tri(const SV *a, const SV *b, const SV *c, Tex *tex, int flags) {
    float area = (b->x - a->x) * (c->y - a->y) - (c->x - a->x) * (b->y - a->y);
    if (area == 0) return;
    bool two = (flags & RF_TWOSIDED) != 0;
    if (area > 0 && !two) return;                                    /* back-facing (screen y is down: front = negative) */
    float minx = fminf(a->x, fminf(b->x, c->x)), maxx = fmaxf(a->x, fmaxf(b->x, c->x)), miny = fminf(a->y, fminf(b->y, c->y)), maxy = fmaxf(a->y, fmaxf(b->y, c->y));
    if (maxx < 0 || maxy < 0 || minx >= G.w || miny >= G.h) return;
    int y0 = imaxi(0, (int)ceilf(miny - 0.5f)), y1 = imini(G.h, (int)ceilf(maxy - 0.5f));
    float inv = 1.0f / area;
    float X0 = a->x, Y0 = a->y;
#define GRAD(f, DX, DY) float DX = (((b->f) - (a->f)) * (c->y - a->y) - ((c->f) - (a->f)) * (b->y - a->y)) * inv, DY = (((c->f) - (a->f)) * (b->x - a->x) - ((b->f) - (a->f)) * (c->x - a->x)) * inv;
    GRAD(iw, iwx, iwy) GRAD(uw, uwx, uwy) GRAD(vw, vwx, vwy) GRAD(r, rx, ry) GRAD(g, gx, gy) GRAD(b, bx, by) GRAD(dw, dwx, dwy)
    const SV *V[3] = { a, b, c };
    bool blend = (flags & RF_BLEND) != 0 || s_alpha < 0.995f; bool zw = !(flags & RF_NOZWRITE); bool nofog = (flags & RF_NOFOG) != 0 || R_fog.mode == 0;
    int tw = tex ? tex->w : 0, th = tex ? tex->h : 0; const uint32_t *tp = tex ? tex->px : NULL; int alpha255 = (int)(s_alpha * 255.0f + 0.5f);
    uint32_t fcol = s_fcol; int fr = CR(fcol), fg = CG(fcol), fb_ = CB(fcol);
    for (int y = y0; y < y1; y++) {
        float yc = (float)y + 0.5f, xs[2]; int k = 0;
        for (int e = 0; e < 3 && k < 2; e++) {
            const SV *p = V[e], *q = V[(e + 1) % 3];
            if ((p->y <= yc && q->y > yc) || (q->y <= yc && p->y > yc)) xs[k++] = p->x + (yc - p->y) / (q->y - p->y) * (q->x - p->x);
        }
        if (k < 2) continue;
        float l = fminf(xs[0], xs[1]), r = fmaxf(xs[0], xs[1]);
        int xa = imaxi(0, (int)ceilf(l - 0.5f)), xb = imini(G.w, (int)ceilf(r - 0.5f));
        if (xa >= xb) continue;
        float dx0 = (float)xa + 0.5f - X0, dy0 = yc - Y0;
        float IW = a->iw + iwx * dx0 + iwy * dy0, UW = a->uw + uwx * dx0 + uwy * dy0, VW = a->vw + vwx * dx0 + vwy * dy0;
        float R = a->r + rx * dx0 + ry * dy0, Gc = a->g + gx * dx0 + gy * dy0, B = a->b + bx * dx0 + by * dy0, DW = a->dw + dwx * dx0 + dwy * dy0;
        uint32_t *fbr = G.fb + (size_t)y * G.w; float *zbr = G.zb + (size_t)y * G.w;
        for (int x = xa; x < xb; x++, IW += iwx, UW += uwx, VW += vwx, R += rx, Gc += gx, B += bx, DW += dwx) {
            if (IW <= zbr[x]) continue;
            float w = 1.0f / IW; int cr = 255, cg = 255, cb = 255, ca = 255;
            if (tp) {
                float u = UW * w, v = VW * w; u -= floorf(u); v -= floorf(v);
                int tx = (int)(u * tw), ty = (int)(v * th); if (tx >= tw) tx = tw - 1; if (ty >= th) ty = th - 1;
                uint32_t t = tp[(size_t)ty * tw + tx]; ca = CA(t);
                if (ca < 128 && !blend) continue;
                if (ca == 0) continue;
                cr = CR(t); cg = CG(t); cb = CB(t);
            }
            cr = (int)(cr * R); cg = (int)(cg * Gc); cb = (int)(cb * B);
            if (cr > 255) cr = 255; if (cg > 255) cg = 255; if (cb > 255) cb = 255; if (cr < 0) cr = 0; if (cg < 0) cg = 0; if (cb < 0) cb = 0;
            if (!nofog) {
                float f = r3d_fog_factor(DW * w, x, y);
                if (f > 0) { int fi = (int)(f * 256.0f); cr += ((fr - cr) * fi) >> 8; cg += ((fg - cg) * fi) >> 8; cb += ((fb_ - cb) * fi) >> 8; }
            }
            if (blend) {
                int al = (alpha255 * ca) / 255; if (al <= 0) continue;
                uint32_t o = fbr[x]; int ia = 255 - al;
                cr = (cr * al + CR(o) * ia) / 255; cg = (cg * al + CG(o) * ia) / 255; cb = (cb * al + CB(o) * ia) / 255;
            }
            fbr[x] = (uint32_t)(cr << 16 | cg << 8 | cb);
            if (zw) zbr[x] = IW;
        }
    }
    G.st_tris_drawn++;
}

static SV to_screen(const RV *v) {
    SV s; float iw = 1.0f / v->w;
    s.x = (v->x * iw * 0.5f + 0.5f) * G.w; s.y = (0.5f - v->y * iw * 0.5f) * G.h; s.iw = iw;
    s.uw = v->u * iw; s.vw = v->v * iw; s.r = v->r; s.g = v->g; s.b = v->b; s.dw = v->d * iw;
    return s;
}
static RV lerp_rv(const RV *a, const RV *b, float t) {
    RV r; r.x = lerpf(a->x, b->x, t); r.y = lerpf(a->y, b->y, t); r.z = lerpf(a->z, b->z, t); r.w = lerpf(a->w, b->w, t);
    r.u = lerpf(a->u, b->u, t); r.v = lerpf(a->v, b->v, t); r.r = lerpf(a->r, b->r, t); r.g = lerpf(a->g, b->g, t); r.b = lerpf(a->b, b->b, t); r.d = lerpf(a->d, b->d, t);
    return r;
}

void r3d_tri(const RV *a, const RV *b, const RV *c, Tex *tex, int flags) {
    G.st_tris++;
    /* trivial frustum reject */
    if ((a->x < -a->w && b->x < -b->w && c->x < -c->w) || (a->x > a->w && b->x > b->w && c->x > c->w) ||
        (a->y < -a->w && b->y < -b->w && c->y < -c->w) || (a->y > a->w && b->y > b->w && c->y > c->w)) return;
    float n = G.znear;
    const RV *in[3] = { a, b, c }; RV poly[4]; int np = 0;
    int inside = (a->w >= n) + (b->w >= n) + (c->w >= n);
    if (inside == 0) return;
    if (inside == 3) { SV s0 = to_screen(a), s1 = to_screen(b), s2 = to_screen(c); raster_tri(&s0, &s1, &s2, tex, flags); return; }
    for (int i = 0; i < 3; i++) {
        const RV *p = in[i], *q = in[(i + 1) % 3]; bool pi = p->w >= n, qi = q->w >= n;
        if (pi) poly[np++] = *p;
        if (pi != qi) { float t = (n - p->w) / (q->w - p->w); poly[np++] = lerp_rv(p, q, t); poly[np - 1].w = n; }
    }
    SV s[4]; for (int i = 0; i < np; i++) s[i] = to_screen(&poly[i]);
    for (int i = 1; i + 1 < np; i++) raster_tri(&s[0], &s[i], &s[i + 1], tex, flags);
}

/* ------------------------------------------------------------- meshes */
static inline void shade(V3 n, bool two, float *out) {
    V3 L = v3mul(R_light_dir, -1.0f); float d = v3dot(n, v3norm(L)); if (two) d = fabsf(d); else if (d < 0) d = 0;
    float k = R_ambient + (1.0f - R_ambient) * d; *out = k;
}
static RV *s_tv; static int s_tvcap; static V3 *s_tn;

void r3d_draw_mesh(Mesh *m, const M4 *model, uint32_t tint, int tex_override, int flags) {
    if (!m || !m->nv) return;
    if (m->nv > s_tvcap) { s_tvcap = m->nv * 2; s_tv = kc_realloc(s_tv, sizeof(RV) * (size_t)s_tvcap); s_tn = kc_realloc(s_tn, sizeof(V3) * (size_t)s_tvcap); }
    M4 mvp = m4_mul(&s_vp, model); const float *q = mvp.m;
    for (int i = 0; i < m->nv; i++) {
        V3 p = m->v[i].p; RV *o = &s_tv[i];
        o->x = q[0] * p.x + q[4] * p.y + q[8] * p.z + q[12]; o->y = q[1] * p.x + q[5] * p.y + q[9] * p.z + q[13];
        o->z = q[2] * p.x + q[6] * p.y + q[10] * p.z + q[14]; o->w = q[3] * p.x + q[7] * p.y + q[11] * p.z + q[15];
        V3 wp = m4_point(model, p); o->d = v3len(v3sub(wp, G.cam)); o->u = m->v[i].u; o->v = m->v[i].v;
        s_tn[i] = v3norm(m4_dir(model, m->v[i].n));
    }
    for (int pi = 0; pi < m->np; pi++) {
        Part *pt = &m->parts[pi]; int tid = tex_override >= 0 ? tex_override : pt->tex; Tex *tx = tid >= 0 ? tex_get(tid) : NULL;
        uint32_t base = col_mul(pt->col, tint); int fl = flags | (pt->two_sided ? RF_TWOSIDED : 0) | (pt->unlit ? RF_UNLIT : 0);
        float alpha = CA(base) / 255.0f; bool trans = pt->translucent || alpha < 0.99f;
        float prev = s_alpha; s_alpha = trans ? alpha : 1.0f; if (trans) fl |= RF_BLEND | RF_NOZWRITE;
        float br = CR(base) / 255.0f, bg = CG(base) / 255.0f, bb = CB(base) / 255.0f;
        float lr = CR(R_light_col) / 255.0f, lg = CG(R_light_col) / 255.0f, lb = CB(R_light_col) / 255.0f;
        bool unlit = (fl & RF_UNLIT) != 0, two = (fl & RF_TWOSIDED) != 0;
        for (uint32_t t = pt->first; t + 2 < pt->first + pt->count; t += 3) {
            RV tri[3];
            for (int k = 0; k < 3; k++) {
                uint32_t vi = m->idx[t + k]; tri[k] = s_tv[vi]; uint32_t vc = m->v[vi].c; float kk = 1.0f;
                if (!unlit) shade(s_tn[vi], two, &kk);
                tri[k].r = br * (CR(vc) / 255.0f) * kk * (unlit ? 1.0f : lr); tri[k].g = bg * (CG(vc) / 255.0f) * kk * (unlit ? 1.0f : lg); tri[k].b = bb * (CB(vc) / 255.0f) * kk * (unlit ? 1.0f : lb);
            }
            if (flags & RF_WIRE) {
                for (int k = 0; k < 3; k++) { const RV *p = &tri[k], *r = &tri[(k + 1) % 3]; if (p->w < G.znear || r->w < G.znear) continue;
                    int x0 = (int)((p->x / p->w * 0.5f + 0.5f) * G.w), y0 = (int)((0.5f - p->y / p->w * 0.5f) * G.h), x1 = (int)((r->x / r->w * 0.5f + 0.5f) * G.w), y1 = (int)((0.5f - r->y / r->w * 0.5f) * G.h);
                    if (abs(x0) < 20000 && abs(x1) < 20000 && abs(y0) < 20000 && abs(y1) < 20000) gfx_line(x0, y0, x1, y1, base | 0xFF000000u, 1); }
            } else r3d_tri(&tri[0], &tri[1], &tri[2], tx, fl);
        }
        s_alpha = prev;
    }
    G.st_draws++;
}

void r3d_billboard(int tex, V3 p, float w, float h, uint32_t tint, int flags) {
    Tex *tx = tex >= 0 ? tex_get(tex) : NULL; V3 r = v3mul(s_right, w * 0.5f), u = v3mul(s_up, h * 0.5f);
    V3 c[4] = { v3sub(v3sub(p, r), u), v3sub(v3add(p, r), u), v3add(v3add(p, r), u), v3add(v3sub(p, r), u) }; float uv[4][2] = { { 0, 1 }, { 1, 1 }, { 1, 0 }, { 0, 0 } };
    RV q[4]; const float *m = s_vp.m; float al = CA(tint) / 255.0f; float prev = s_alpha; s_alpha = al;
    for (int i = 0; i < 4; i++) {
        q[i].x = m[0] * c[i].x + m[4] * c[i].y + m[8] * c[i].z + m[12]; q[i].y = m[1] * c[i].x + m[5] * c[i].y + m[9] * c[i].z + m[13]; q[i].z = 0;
        q[i].w = m[3] * c[i].x + m[7] * c[i].y + m[11] * c[i].z + m[15]; q[i].u = uv[i][0]; q[i].v = uv[i][1];
        q[i].r = CR(tint) / 255.0f; q[i].g = CG(tint) / 255.0f; q[i].b = CB(tint) / 255.0f; q[i].d = v3len(v3sub(c[i], G.cam));
    }
    int fl = flags | RF_TWOSIDED | (al < 0.99f ? RF_BLEND : 0);
    r3d_tri(&q[0], &q[1], &q[2], tx, fl); r3d_tri(&q[0], &q[2], &q[3], tx, fl);
    s_alpha = prev;
}
void r3d_point(V3 p, float size, uint32_t c) {
    float sx, sy, d; if (!r3d_project(p, &sx, &sy, &d)) return;
    float fs = G.h * 0.5f / s_tanh; int px = (int)fmaxf(1.0f, size * fs / d); float iw = 1.0f / d;
    float fog = r3d_fog_factor(d, (int)sx, (int)sy);
    if (fog >= 0.98f) return;
    uint32_t cc = fog > 0 ? col_mix(c, s_fcol, fog) | (c & 0xFF000000u) : c;
    for (int y = (int)sy - px / 2; y <= (int)sy + px / 2; y++) for (int x = (int)sx - px / 2; x <= (int)sx + px / 2; x++) {
        if (x < 0 || y < 0 || x >= G.w || y >= G.h) continue; size_t i = (size_t)y * G.w + x;
        if (iw <= G.zb[i]) continue; if (CA(cc) == 255) G.fb[i] = cc & 0xFFFFFF; else { uint32_t o = G.fb[i]; int a = CA(cc); G.fb[i] = col_pack((CR(cc) * a + CR(o) * (255 - a)) / 255, (CG(cc) * a + CG(o) * (255 - a)) / 255, (CB(cc) * a + CB(o) * (255 - a)) / 255, 255) & 0xFFFFFF; }
    }
}
void r3d_line(V3 a, V3 b, uint32_t c) {
    const float *m = s_vp.m;
    float aw = m[3] * a.x + m[7] * a.y + m[11] * a.z + m[15], bw = m[3] * b.x + m[7] * b.y + m[11] * b.z + m[15];
    if (aw < G.znear && bw < G.znear) return;
    if (aw < G.znear) { float t = (G.znear - aw) / (bw - aw); a = v3add(a, v3mul(v3sub(b, a), t)); }
    else if (bw < G.znear) { float t = (G.znear - bw) / (aw - bw); b = v3add(b, v3mul(v3sub(a, b), t)); }
    float x0, y0, d0, x1, y1, d1; if (!r3d_project(a, &x0, &y0, &d0) || !r3d_project(b, &x1, &y1, &d1)) return;
    if (fabsf(x0) > 20000 || fabsf(x1) > 20000 || fabsf(y0) > 20000 || fabsf(y1) > 20000) return;
    int n = (int)fmaxf(fabsf(x1 - x0), fabsf(y1 - y0)) + 1;
    for (int i = 0; i <= n; i++) {
        float t = (float)i / n; int x = (int)lerpf(x0, x1, t), y = (int)lerpf(y0, y1, t); float iw = lerpf(1.0f / d0, 1.0f / d1, t);
        if (x < 0 || y < 0 || x >= G.w || y >= G.h) continue; size_t k = (size_t)y * G.w + x;
        if (iw * 1.002f >= G.zb[k]) G.fb[k] = c & 0xFFFFFF;
    }
}
void r3d_box(V3 mn, V3 mx, uint32_t c) {
    V3 p[8]; for (int i = 0; i < 8; i++) p[i] = v3((i & 1) ? mx.x : mn.x, (i & 2) ? mx.y : mn.y, (i & 4) ? mx.z : mn.z);
    static const int E[12][2] = { {0,1},{2,3},{4,5},{6,7},{0,2},{1,3},{4,6},{5,7},{0,4},{1,5},{2,6},{3,7} };
    for (int i = 0; i < 12; i++) r3d_line(p[E[i][0]], p[E[i][1]], c);
}
