/* std.game — framebuffer, 2D drawing, textures (lazy + LRU), image decode/encode. */
#include "game_internal.h"
#include "../gui/gui_font.h"
#if defined(_WIN32)
#  define strcasecmp _stricmp
#else
#  include <strings.h>
#endif

/* ---- bridge to std.http's inflate (gzip/zlib/raw) ---- */
typedef struct { char *p; size_t len, cap; } GmSysBuf;
typedef enum { GM_INF_RAW, GM_INF_ZLIB, GM_INF_GZIP, GM_INF_AUTO } GmInfMode;
bool kc_inflate(const uint8_t *in, size_t n, GmInfMode mode, GmSysBuf *out, size_t max_out, char *err);

uint8_t *gm_inflate(const uint8_t *in, size_t n, int mode, size_t *outlen, char *err) {
    GmSysBuf b; memset(&b, 0, sizeof b);
    if (!kc_inflate(in, n, (GmInfMode)mode, &b, 0x7fffffffu, err)) { free(b.p); return NULL; }
    if (!b.p) { b.p = kc_malloc(1); b.len = 0; }
    *outlen = b.len;
    return (uint8_t *)b.p;
}

uint32_t gm_crc32(uint32_t crc, const uint8_t *d, size_t n) {
    static uint32_t tab[256]; static bool init = false;
    if (!init) { for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; tab[i] = c; } init = true; }
    crc = ~crc;
    for (size_t i = 0; i < n; i++) crc = tab[(crc ^ d[i]) & 255] ^ (crc >> 8);
    return ~crc;
}

/* ============================================================ framebuffer */
static int cx0, cy0, cx1, cy1;

void gfx_resize(int w, int h) {
    if (w < 16) w = 16; if (h < 16) h = 16;
    free(G.fb); free(G.zb);
    G.w = w; G.h = h;
    G.fb = kc_malloc((size_t)w * h * 4); G.zb = kc_malloc((size_t)w * h * 4);
    memset(G.fb, 0, (size_t)w * h * 4); memset(G.zb, 0, (size_t)w * h * 4);
    cx0 = 0; cy0 = 0; cx1 = w; cy1 = h;
}
void gfx_clip(int x0, int y0, int x1, int y1) { cx0 = imaxi(0, x0); cy0 = imaxi(0, y0); cx1 = imini(G.w, x1); cy1 = imini(G.h, y1); }
void gfx_clip_reset(void) { cx0 = 0; cy0 = 0; cx1 = G.w; cy1 = G.h; }
void gfx_clear(uint32_t c) {
    uint32_t v = c & 0xFFFFFF; size_t n = (size_t)G.w * G.h;
    for (size_t i = 0; i < n; i++) G.fb[i] = v;
    memset(G.zb, 0, n * 4);
    G.begun = true;
}
static inline void blend_px(uint32_t *d, uint32_t c) {
    int a = CA(c);
    if (a >= 255) { *d = c & 0xFFFFFF; return; }
    if (a <= 0) return;
    uint32_t o = *d; int ia = 255 - a;
    int r = (CR(c) * a + CR(o) * ia + 127) / 255, g = (CG(c) * a + CG(o) * ia + 127) / 255, b = (CB(c) * a + CB(o) * ia + 127) / 255;
    *d = (uint32_t)(r << 16 | g << 8 | b);
}
void gfx_px(int x, int y, uint32_t c) {
    if (x < cx0 || y < cy0 || x >= cx1 || y >= cy1) return;
    blend_px(&G.fb[(size_t)y * G.w + x], c);
}
uint32_t gfx_get(int x, int y) { return (x < 0 || y < 0 || x >= G.w || y >= G.h) ? 0 : (0xFF000000u | G.fb[(size_t)y * G.w + x]); }

static void fill_rect(int x, int y, int w, int h, uint32_t c) {
    int x1 = x + w, y1 = y + h; x = imaxi(x, cx0); y = imaxi(y, cy0); x1 = imini(x1, cx1); y1 = imini(y1, cy1);
    if (x >= x1 || y >= y1) return;
    int a = CA(c);
    if (a <= 0) return;
    if (a >= 255) { uint32_t v = c & 0xFFFFFF; for (int j = y; j < y1; j++) { uint32_t *row = G.fb + (size_t)j * G.w; for (int i = x; i < x1; i++) row[i] = v; } return; }
    for (int j = y; j < y1; j++) { uint32_t *row = G.fb + (size_t)j * G.w; for (int i = x; i < x1; i++) blend_px(&row[i], c); }
}
void gfx_rect(int x, int y, int w, int h, uint32_t c, bool fill) {
    if (w <= 0 || h <= 0) return;
    if (fill) { fill_rect(x, y, w, h, c); return; }
    fill_rect(x, y, w, 1, c); fill_rect(x, y + h - 1, w, 1, c);
    if (h > 2) { fill_rect(x, y + 1, 1, h - 2, c); fill_rect(x + w - 1, y + 1, 1, h - 2, c); }
}
void gfx_rect_grad(int x, int y, int w, int h, uint32_t top, uint32_t bot) {
    for (int j = 0; j < h; j++) fill_rect(x, y + j, w, 1, col_mix(top, bot, h > 1 ? (float)j / (float)(h - 1) : 0));
}
void gfx_line(int x0, int y0, int x1, int y1, uint32_t c, int thick) {
    int dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
    if (thick < 1) thick = 1;
    int guard = 0;
    for (;;) {
        if (thick == 1) gfx_px(x0, y0, c); else fill_rect(x0 - thick / 2, y0 - thick / 2, thick, thick, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
        if (++guard > 100000) break;
    }
}
void gfx_circle(int cx, int cy, int r, uint32_t c, bool fill) {
    if (r < 0) return;
    if (fill) {
        for (int y = -r; y <= r; y++) { int w = (int)sqrtf((float)(r * r + r - y * y)); fill_rect(cx - w, cy + y, 2 * w + 1, 1, c); }
        return;
    }
    int x = r, y = 0, e = 1 - r;
    while (x >= y) {
        gfx_px(cx + x, cy + y, c); gfx_px(cx - x, cy + y, c); gfx_px(cx + x, cy - y, c); gfx_px(cx - x, cy - y, c);
        gfx_px(cx + y, cy + x, c); gfx_px(cx - y, cy + x, c); gfx_px(cx + y, cy - x, c); gfx_px(cx - y, cy - x, c);
        y++; if (e < 0) e += 2 * y + 1; else { x--; e += 2 * (y - x) + 1; }
    }
}
void gfx_tri(int x0, int y0, int x1, int y1, int x2, int y2, uint32_t c, bool fill) {
    if (!fill) { gfx_line(x0, y0, x1, y1, c, 1); gfx_line(x1, y1, x2, y2, c, 1); gfx_line(x2, y2, x0, y0, c, 1); return; }
    int minx = imaxi(cx0, imini(x0, imini(x1, x2))), maxx = imini(cx1 - 1, imaxi(x0, imaxi(x1, x2)));
    int miny = imaxi(cy0, imini(y0, imini(y1, y2))), maxy = imini(cy1 - 1, imaxi(y0, imaxi(y1, y2)));
    long area = (long)(x1 - x0) * (y2 - y0) - (long)(x2 - x0) * (y1 - y0);
    if (!area) return;
    for (int y = miny; y <= maxy; y++) for (int x = minx; x <= maxx; x++) {
        long w0 = (long)(x1 - x0) * (y - y0) - (long)(x - x0) * (y1 - y0);
        long w1 = (long)(x2 - x1) * (y - y1) - (long)(x - x1) * (y2 - y1);
        long w2 = (long)(x0 - x2) * (y - y2) - (long)(x - x2) * (y0 - y2);
        if (area > 0 ? (w0 >= 0 && w1 >= 0 && w2 >= 0) : (w0 <= 0 && w1 <= 0 && w2 <= 0)) gfx_px(x, y, c);
    }
}

/* ----------------------------------------------------------------- text */
static void glyph(int x, int y, int ch, uint32_t c, int s) {
    if (ch < 32 || ch > 126) ch = '?';
    const uint8_t *g = KC_FONT8[ch - 32];
    for (int r = 0; r < 8; r++) { uint8_t row = g[r]; if (!row) continue; for (int col = 0; col < 8; col++) if ((row >> col) & 1) fill_rect(x + col * s, y + r * s, s, s, c); }
}
int gfx_text_w(const char *s, int scale) {
    int best = 0, cur = 0;
    for (; *s; s++) { if (*s == '\n') { if (cur > best) best = cur; cur = 0; } else if (((unsigned char)*s & 0xC0) != 0x80) cur++; }
    if (cur > best) best = cur;
    return best * 8 * scale;
}
int gfx_text_h(const char *s, int scale) { int n = 1; for (; *s; s++) if (*s == '\n') n++; return n * 10 * scale - 2 * scale; }
void gfx_text(int x, int y, const char *s, uint32_t c, int scale) {
    if (scale < 1) scale = 1;
    int px = x, py = y;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '\n') { px = x; py += 10 * scale; continue; }
        if ((*p & 0xC0) == 0x80) continue;                  /* UTF-8 continuation */
        glyph(px, py, *p < 0x80 ? *p : '?', c, scale); px += 8 * scale;
    }
}
void gfx_text_shadow(int x, int y, const char *s, uint32_t c, int scale, uint32_t shadow) {
    if (CA(shadow)) gfx_text(x + scale, y + scale, s, shadow, scale);
    gfx_text(x, y, s, c, scale);
}

/* ------------------------------------------------------------------ blit */
static inline uint32_t tint_mul(uint32_t s, uint32_t t) {
    if (t == 0xFFFFFFFFu) return s;
    return col_pack(CR(s) * CR(t) / 255, CG(s) * CG(t) / 255, CB(s) * CB(t) / 255, CA(s) * CA(t) / 255);
}
void gfx_blit(Tex *t, int sx, int sy, int sw, int sh, float dx, float dy, float dw, float dh, float rot, int flip, uint32_t tint) {
    if (!t || !t->px || dw == 0 || dh == 0) return;
    if (sw <= 0) sw = t->w - sx; if (sh <= 0) sh = t->h - sy;
    sx = iclampi(sx, 0, t->w - 1); sy = iclampi(sy, 0, t->h - 1); sw = imini(sw, t->w - sx); sh = imini(sh, t->h - sy);
    if (sw <= 0 || sh <= 0) return;
    bool fx = (flip & 1) != 0, fy = (flip & 2) != 0;
    if (dw < 0) { dx += dw; dw = -dw; fx = !fx; }
    if (dh < 0) { dy += dh; dh = -dh; fy = !fy; }
    float cxm = dx + dw * 0.5f, cym = dy + dh * 0.5f;
    int bx0, by0, bx1, by1;
    float cs = 1, sn = 0;
    if (rot != 0) {
        cs = cosf(rot); sn = sinf(rot);
        float hw = dw * 0.5f, hh = dh * 0.5f, ex = fabsf(hw * cs) + fabsf(hh * sn), ey = fabsf(hw * sn) + fabsf(hh * cs);
        bx0 = (int)floorf(cxm - ex); bx1 = (int)ceilf(cxm + ex); by0 = (int)floorf(cym - ey); by1 = (int)ceilf(cym + ey);
    } else { bx0 = (int)floorf(dx); bx1 = (int)ceilf(dx + dw); by0 = (int)floorf(dy); by1 = (int)ceilf(dy + dh); }
    bx0 = imaxi(bx0, cx0); by0 = imaxi(by0, cy0); bx1 = imini(bx1, cx1); by1 = imini(by1, cy1);
    float iw = (float)sw / dw, ih = (float)sh / dh;
    for (int y = by0; y < by1; y++) {
        uint32_t *row = G.fb + (size_t)y * G.w;
        for (int x = bx0; x < bx1; x++) {
            float lx = (float)x + 0.5f - cxm, ly = (float)y + 0.5f - cym;
            if (rot != 0) { float rx = lx * cs + ly * sn, ry = -lx * sn + ly * cs; lx = rx; ly = ry; }
            lx += dw * 0.5f; ly += dh * 0.5f;
            if (lx < 0 || ly < 0 || lx >= dw || ly >= dh) continue;
            int u = (int)(lx * iw), v = (int)(ly * ih);
            if (fx) u = sw - 1 - u; if (fy) v = sh - 1 - v;
            u = iclampi(u, 0, sw - 1); v = iclampi(v, 0, sh - 1);
            uint32_t px = t->px[(size_t)(sy + v) * t->w + (sx + u)];
            if (!CA(px)) continue;
            blend_px(&row[x], tint_mul(px, tint));
        }
    }
}

/* ============================================================== textures */
static Tex *T; static int nt, capt;
static size_t resident_bytes;

static int tex_slot(void) {
    for (int i = 0; i < nt; i++) if (!T[i].alive) return i;
    if (nt == capt) { capt = capt ? capt * 2 : 64; T = kc_realloc(T, sizeof(Tex) * (size_t)capt); }
    return nt++;
}
static void tex_init(Tex *t) { memset(t, 0, sizeof *t); t->alive = true; }

int tex_new(int w, int h, uint32_t fill) {
    if (w < 1 || h < 1 || w > 16384 || h > 16384) gm_err("texture size out of range (1..16384): %dx%d", w, h);
    int id = tex_slot(); Tex *t = &T[id]; tex_init(t);
    t->w = w; t->h = h; t->px = kc_malloc((size_t)w * h * 4);
    for (size_t i = 0; i < (size_t)w * h; i++) t->px[i] = fill;
    t->resident = true; t->protect = true; t->last = G.frame;
    resident_bytes += (size_t)w * h * 4;
    return id;
}
int tex_from_img(Img *img, const char *name, bool protect) {
    int id = tex_slot(); Tex *t = &T[id]; tex_init(t);
    t->w = img->w; t->h = img->h; t->px = img->px; img->px = NULL;
    t->resident = true; t->protect = protect; t->last = G.frame; t->name = name ? kc_strdup(name) : NULL;
    resident_bytes += (size_t)t->w * t->h * 4;
    return id;
}
int tex_mem(const uint8_t *d, size_t n, const char *name, char *err) {
    Img im;
    if (!img_decode(d, n, &im, err)) return -1;
    return tex_from_img(&im, name, true);
}
int tex_file(const char *path) {
    for (int i = 0; i < nt; i++) if (T[i].alive && T[i].path && !strcmp(T[i].path, path)) return i;   /* de-dup */
    int id = tex_slot(); Tex *t = &T[id]; tex_init(t);
    t->path = kc_strdup(path); t->last = G.frame;
    return id;
}
static void tex_make_missing(Tex *t) {
    t->w = t->h = 16; t->px = kc_malloc(16 * 16 * 4);
    for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) t->px[y * 16 + x] = (((x >> 2) + (y >> 2)) & 1) ? 0xFFFF00FF : 0xFF220022;
    t->resident = true; t->protect = true; resident_bytes += 16 * 16 * 4;
}
Tex *tex_peek(int id) { return (id >= 0 && id < nt && T[id].alive) ? &T[id] : NULL; }
Tex *tex_get(int id) {
    if (id < 0 || id >= nt || !T[id].alive) return NULL;
    Tex *t = &T[id];
    if (!t->resident) {
        size_t n; char *buf = t->path ? read_file_all(t->path, &n) : NULL; char err[256] = "";
        Img im;
        if (!buf) { gm_warn("texture '%s': cannot read file", t->path ? t->path : "?"); tex_make_missing(t); }
        else if (!img_decode((const uint8_t *)buf, n, &im, err)) { gm_warn("texture '%s': %s", t->path, err); tex_make_missing(t); }
        else { t->w = im.w; t->h = im.h; t->px = im.px; t->resident = true; resident_bytes += (size_t)im.w * im.h * 4; }
        free(buf);
    }
    t->last = G.frame;
    return t;
}
void tex_free(int id) {
    Tex *t = tex_peek(id); if (!t) return;
    if (t->resident) resident_bytes -= (size_t)t->w * t->h * 4;
    free(t->px); free(t->path); free(t->name); memset(t, 0, sizeof *t);
}
void tex_evict(size_t budget) {
    while (resident_bytes > budget) {
        int best = -1; uint64_t bl = ~0ull;
        for (int i = 0; i < nt; i++) { Tex *t = &T[i]; if (t->alive && t->resident && !t->protect && t->path && t->last + 60 < G.frame && t->last < bl) { bl = t->last; best = i; } }
        if (best < 0) break;
        Tex *t = &T[best]; resident_bytes -= (size_t)t->w * t->h * 4; free(t->px); t->px = NULL; t->resident = false;
    }
}
size_t tex_resident_bytes(void) { return resident_bytes; }
int tex_count(void) { int n = 0; for (int i = 0; i < nt; i++) if (T[i].alive) n++; return n; }
uint32_t tex_sample(const Tex *t, float u, float v) {
    int x = (int)floorf(u * t->w) % t->w, y = (int)floorf(v * t->h) % t->h;
    if (x < 0) x += t->w; if (y < 0) y += t->h;
    return t->px[(size_t)y * t->w + x];
}
uint32_t tex_average(const Tex *t) {
    uint64_t r = 0, g = 0, b = 0, n = 0;
    for (size_t i = 0; i < (size_t)t->w * t->h; i++) { uint32_t c = t->px[i]; if (CA(c) < 128) continue; r += CR(c); g += CG(c); b += CB(c); n++; }
    return n ? col_pack((int)(r / n), (int)(g / n), (int)(b / n), 255) : 0xFF808080;
}

/* ------------------------------------------------------ procedural textures */
enum { PK_CHECKER, PK_NOISE, PK_GRASS, PK_DIRT, PK_STONE, PK_SAND, PK_BRICK, PK_WOOD, PK_PLANKS, PK_WATER, PK_METAL, PK_SNOW, PK_LEAVES, PK_GRADIENT, PK_STRIPES, PK_DOTS, PK_N };
static const char *PK_NAMES[PK_N] = { "checker", "noise", "grass", "dirt", "stone", "sand", "brick", "wood", "planks", "water", "metal", "snow", "leaves", "gradient", "stripes", "dots" };

static uint32_t proc_px(int k, int x, int y, int S, uint32_t sd, uint32_t c1, uint32_t c2, bool has1, bool has2) {
    float fx = (float)x / S, fy = (float)y / S;
    float sp = (float)(hash3(x, y, 7, sd) & 255) / 255.0f;                      /* per-pixel speckle */
    switch (k) {
        case PK_CHECKER: { int q = ((x * 8 / S) + (y * 8 / S)) & 1; return q ? (has2 ? c2 : 0xFF9A9A9A) : (has1 ? c1 : 0xFFE6E6E6); }
        case PK_NOISE: return col_mix(has1 ? c1 : 0xFF202020, has2 ? c2 : 0xFFE0E0E0, fbm2(fx * 8, fy * 8, 4, 2, 0.5f, sd));
        case PK_GRASS: { float n = fbm2(fx * 6, fy * 6, 3, 2, 0.5f, sd); uint32_t a = has1 ? c1 : 0xFF63A33D, b = has2 ? c2 : 0xFF3C7A2A; return col_scale(col_mix(a, b, n * 0.7f + sp * 0.5f), 0.85f + 0.3f * sp); }
        case PK_DIRT: { float n = fbm2(fx * 5, fy * 5, 3, 2, 0.5f, sd); return col_scale(col_mix(has1 ? c1 : 0xFF86603B, has2 ? c2 : 0xFF5E4026, n * 0.6f + sp * 0.4f), 0.9f + 0.2f * sp); }
        case PK_STONE: { float n = fbm2(fx * 5, fy * 5, 4, 2, 0.55f, sd); float crack = fabsf(vnoise2(fx * 7, fy * 7, sd + 5) - 0.5f) < 0.025f ? 0.7f : 1.0f; return col_scale(col_mix(has1 ? c1 : 0xFF8D8D93, has2 ? c2 : 0xFF6A6A72, n), crack * (0.92f + 0.16f * sp)); }
        case PK_SAND: return col_scale(col_mix(has1 ? c1 : 0xFFE3D39A, has2 ? c2 : 0xFFCDBB7C, fbm2(fx * 9, fy * 9, 2, 2, 0.5f, sd) * 0.6f + sp * 0.4f), 0.95f + 0.1f * sp);
        case PK_BRICK: {
            int rows = 4, rh = S / rows, row = y / rh, off = (row & 1) ? S / 4 : 0, bw = S / 2, bx = (x + off) % bw, by = y % rh;
            if (by == 0 || bx == 0) return 0xFFB9B4A8;
            float n = vnoise2(fx * 12 + row, fy * 12, sd);
            return col_scale(col_mix(has1 ? c1 : 0xFFA5472F, has2 ? c2 : 0xFF873A27, n), 0.9f + 0.2f * sp);
        }
        case PK_WOOD: { float ring = sinf((fx * 9.0f + fbm2(fx * 3, fy * 3, 3, 2, 0.5f, sd) * 4.0f) * 3.14159f); return col_scale(col_mix(has1 ? c1 : 0xFF9A6B3A, has2 ? c2 : 0xFF6E4A26, ring * 0.5f + 0.5f), 0.92f + 0.12f * sp); }
        case PK_PLANKS: { int ph = S / 4; if (y % ph == 0) return 0xFF4A3217; float g = vnoise2(fx * 2, (float)(y / ph) * 3 + fy * 0.2f, sd) ; return col_scale(col_mix(has1 ? c1 : 0xFFC89B5C, has2 ? c2 : 0xFFA97F42, g), 0.93f + 0.14f * sp); }
        case PK_WATER: { float w = sinf((fx * 6 + fy * 3) * 3.14159f) * 0.5f + 0.5f; return col_mix(has1 ? c1 : 0xFF2F6FCF, has2 ? c2 : 0xFF4D8FE8, w * 0.5f + sp * 0.2f); }
        case PK_METAL: return col_scale(col_mix(has1 ? c1 : 0xFFB5BCC4, has2 ? c2 : 0xFF8A929C, fbm2(fx * 2, fy * 40, 2, 2, 0.5f, sd)), 0.95f + 0.1f * sp);
        case PK_SNOW: return col_scale(col_mix(has1 ? c1 : 0xFFF4F8FF, has2 ? c2 : 0xFFD6E2F2, sp * 0.6f + fbm2(fx * 6, fy * 6, 2, 2, 0.5f, sd) * 0.4f), 1.0f);
        case PK_LEAVES: { uint32_t c = col_scale(col_mix(has1 ? c1 : 0xFF3F8F3A, has2 ? c2 : 0xFF2A6B2B, fbm2(fx * 8, fy * 8, 2, 2, 0.5f, sd)), 0.85f + 0.3f * sp); if (sp > 0.82f) c &= 0x00FFFFFFu; return c; }
        case PK_GRADIENT: return col_mix(has1 ? c1 : 0xFFFFFFFF, has2 ? c2 : 0xFF000000, fy);
        case PK_STRIPES: return ((x * 8 / S) & 1) ? (has2 ? c2 : 0xFF303030) : (has1 ? c1 : 0xFFE0E0E0);
        case PK_DOTS: { float dx = fx * 4 - floorf(fx * 4) - 0.5f, dy = fy * 4 - floorf(fy * 4) - 0.5f; return (dx * dx + dy * dy < 0.12f) ? (has1 ? c1 : 0xFFE0E0E0) : (has2 ? c2 : 0xFF303030); }
    }
    return 0xFFFF00FF;
}
int tex_proc(const char *kind, int size, uint32_t seed, uint32_t c1, uint32_t c2) {
    int k = -1;
    for (int i = 0; i < PK_N; i++) if (!strcmp(kind, PK_NAMES[i])) k = i;
    if (k < 0) {
        char list[256] = ""; for (int i = 0; i < PK_N; i++) { strcat(list, PK_NAMES[i]); if (i + 1 < PK_N) strcat(list, ", "); }
        gm_err("texture_proc: unknown kind \"%s\" (available: %s)", kind, list);
    }
    if (size < 2) size = 2; if (size > 2048) size = 2048;
    int id = tex_new(size, size, 0); Tex *t = &T[id];
    for (int y = 0; y < size; y++) for (int x = 0; x < size; x++) t->px[y * size + x] = proc_px(k, x, y, size, seed, c1, c2, c1 != 0, c2 != 0);
    return id;
}

/* ============================================================ image decode */
static uint32_t rd32be(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint32_t rd32le(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static int paeth(int a, int b, int c) { int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c); return (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c); }
#define IMG_FAIL(...) do { snprintf(err, 256, __VA_ARGS__); return false; } while (0)
static bool img_alloc(Img *o, uint32_t w, uint32_t h, char *err) {
    if (w < 1 || h < 1 || w > 16384 || h > 16384 || (uint64_t)w * h > 120000000ull) IMG_FAIL("unsupported image size %ux%u", w, h);
    o->w = (int)w; o->h = (int)h; o->px = kc_malloc((size_t)w * h * 4); return true;
}

static bool dec_png(const uint8_t *d, size_t n, Img *out, char *err) {
    size_t p = 8; uint32_t w = 0, h = 0; int bd = 0, ct = 0, il = 0; uint32_t pal[256]; int npal = 0; uint8_t trns[256]; int ntrns = 0;
    uint8_t *idat = NULL; size_t idl = 0, idc = 0; bool got_hdr = false; uint16_t tr_gray = 0xFFFF, tr_rgb[3] = { 0xFFFF, 0xFFFF, 0xFFFF }; bool tr_has = false;
    memset(trns, 255, sizeof trns);
    while (p + 12 <= n) {
        uint32_t len = rd32be(d + p); const uint8_t *tp = d + p + 4, *cd = d + p + 8;
        if ((size_t)len > n - p - 12) { free(idat); IMG_FAIL("PNG: truncated chunk"); }
        if (!memcmp(tp, "IHDR", 4) && len >= 13) { w = rd32be(cd); h = rd32be(cd + 4); bd = cd[8]; ct = cd[9]; il = cd[12]; got_hdr = true; }
        else if (!memcmp(tp, "PLTE", 4)) { npal = imini((int)len / 3, 256); for (int i = 0; i < npal; i++) pal[i] = 0xFF000000u | (uint32_t)cd[i * 3] << 16 | (uint32_t)cd[i * 3 + 1] << 8 | cd[i * 3 + 2]; }
        else if (!memcmp(tp, "tRNS", 4)) {
            if (ct == 3) { ntrns = imini((int)len, 256); memcpy(trns, cd, (size_t)ntrns); }
            else if (ct == 0 && len >= 2) { tr_gray = (uint16_t)(cd[0] << 8 | cd[1]); tr_has = true; }
            else if (ct == 2 && len >= 6) { for (int i = 0; i < 3; i++) tr_rgb[i] = (uint16_t)(cd[i * 2] << 8 | cd[i * 2 + 1]); tr_has = true; }
        }
        else if (!memcmp(tp, "IDAT", 4)) { if (idl + len > idc) { idc = (idl + len) * 2; idat = kc_realloc(idat, idc); } memcpy(idat + idl, cd, len); idl += len; }
        else if (!memcmp(tp, "IEND", 4)) break;
        p += 12 + len;
    }
    if (!got_hdr) { free(idat); IMG_FAIL("PNG: missing IHDR"); }
    if (il) { free(idat); IMG_FAIL("PNG: interlaced images are not supported (re-save without interlacing)"); }
    int chans = ct == 0 ? 1 : ct == 2 ? 3 : ct == 3 ? 1 : ct == 4 ? 2 : ct == 6 ? 4 : 0;
    if (!chans || (bd != 1 && bd != 2 && bd != 4 && bd != 8 && bd != 16)) { free(idat); IMG_FAIL("PNG: unsupported colour type/bit depth"); }
    if (!img_alloc(out, w, h, err)) { free(idat); return false; }
    size_t rawlen; char ierr[256]; uint8_t *raw = gm_inflate(idat, idl, 1, &rawlen, ierr); free(idat);
    if (!raw) { free(out->px); out->px = NULL; IMG_FAIL("PNG: %.200s", ierr); }
    size_t bpp_bits = (size_t)chans * bd, stride = (w * bpp_bits + 7) / 8, bpp = imaxi(1, (int)(bpp_bits / 8));
    if (rawlen < (stride + 1) * h) { free(raw); free(out->px); out->px = NULL; IMG_FAIL("PNG: image data too short"); }
    uint8_t *prev = kc_malloc(stride), *cur;
    memset(prev, 0, stride);
    for (uint32_t y = 0; y < h; y++) {
        cur = raw + y * (stride + 1); int ft = cur[0]; cur++;
        for (size_t i = 0; i < stride; i++) {
            int a = i >= bpp ? cur[i - bpp] : 0, b = prev[i], c = i >= bpp ? prev[i - bpp] : 0, x = cur[i];
            switch (ft) { case 1: x += a; break; case 2: x += b; break; case 3: x += (a + b) / 2; break; case 4: x += paeth(a, b, c); break; default: break; }
            cur[i] = (uint8_t)x;
        }
        for (uint32_t x = 0; x < w; x++) {
            uint32_t col = 0xFF000000u; uint16_t s[4] = { 0, 0, 0, 0 };
            for (int ch = 0; ch < chans; ch++) {
                size_t bit = ((size_t)x * chans + ch) * bd;
                if (bd == 8) s[ch] = cur[bit / 8];
                else if (bd == 16) s[ch] = (uint16_t)(cur[bit / 8] << 8 | cur[bit / 8 + 1]);
                else s[ch] = (uint16_t)((cur[bit / 8] >> (8 - bd - (bit % 8))) & ((1 << bd) - 1));
            }
            int sh = bd == 16 ? 8 : 0; int maxv = bd == 16 ? 255 : (1 << bd) - 1;
            #define S8(v) ((bd == 16) ? ((v) >> sh) : (bd == 8 ? (v) : ((v) * 255 / maxv)))
            if (ct == 0) { int g = S8(s[0]); int a = (tr_has && s[0] == tr_gray) ? 0 : 255; col = col_pack(g, g, g, a); }
            else if (ct == 2) { int a = (tr_has && s[0] == tr_rgb[0] && s[1] == tr_rgb[1] && s[2] == tr_rgb[2]) ? 0 : 255; col = col_pack(S8(s[0]), S8(s[1]), S8(s[2]), a); }
            else if (ct == 3) { int idx = s[0]; col = idx < npal ? (pal[idx] & 0xFFFFFF) | ((uint32_t)(idx < ntrns ? trns[idx] : 255) << 24) : 0xFFFF00FF; }
            else if (ct == 4) { int g = S8(s[0]); col = col_pack(g, g, g, S8(s[1])); }
            else col = col_pack(S8(s[0]), S8(s[1]), S8(s[2]), S8(s[3]));
            out->px[(size_t)y * w + x] = col;
        }
        memcpy(prev, cur, stride);
    }
    free(prev); free(raw);
    return true;
}

static bool dec_bmp(const uint8_t *d, size_t n, Img *out, char *err) {
    if (n < 54) IMG_FAIL("BMP: truncated");
    uint32_t off = rd32le(d + 10), hs = rd32le(d + 14); int32_t w = (int32_t)rd32le(d + 18), h = (int32_t)rd32le(d + 22);
    int bpp = d[28] | d[29] << 8; uint32_t comp = rd32le(d + 30), ncol = rd32le(d + 46);
    if (hs < 40) IMG_FAIL("BMP: unsupported header");
    bool topdown = h < 0; if (topdown) h = -h;
    if (comp != 0 && comp != 3) IMG_FAIL("BMP: compressed bitmaps are not supported");
    if (bpp != 8 && bpp != 24 && bpp != 32 && bpp != 4 && bpp != 1) IMG_FAIL("BMP: unsupported bit depth %d", bpp);
    if (w <= 0 || !img_alloc(out, (uint32_t)w, (uint32_t)h, err)) return false;
    uint32_t pal[256]; int np = 0;
    if (bpp <= 8) { np = ncol ? (int)ncol : 1 << bpp; if (np > 256) np = 256; for (int i = 0; i < np; i++) { size_t q = 14 + hs + (size_t)i * 4; if (q + 4 <= n) pal[i] = 0xFF000000u | (uint32_t)d[q + 2] << 16 | (uint32_t)d[q + 1] << 8 | d[q]; else pal[i] = 0xFF000000u; } }
    size_t stride = (((size_t)w * bpp + 31) / 32) * 4;
    for (int y = 0; y < h; y++) {
        size_t row = off + (size_t)(topdown ? y : h - 1 - y) * stride;
        if (row + stride > n) { free(out->px); out->px = NULL; IMG_FAIL("BMP: pixel data truncated"); }
        for (int x = 0; x < w; x++) {
            uint32_t c;
            if (bpp == 24) { const uint8_t *q = d + row + (size_t)x * 3; c = 0xFF000000u | (uint32_t)q[2] << 16 | (uint32_t)q[1] << 8 | q[0]; }
            else if (bpp == 32) { const uint8_t *q = d + row + (size_t)x * 4; uint8_t a = q[3]; c = (uint32_t)a << 24 | (uint32_t)q[2] << 16 | (uint32_t)q[1] << 8 | q[0]; if (comp == 0 && a == 0) c |= 0xFF000000u; }
            else if (bpp == 8) c = pal[d[row + x] % (np ? np : 1)];
            else if (bpp == 4) c = pal[((d[row + x / 2] >> ((x & 1) ? 0 : 4)) & 15) % (np ? np : 1)];
            else c = pal[((d[row + x / 8] >> (7 - (x & 7))) & 1) % (np ? np : 1)];
            out->px[(size_t)y * w + x] = c;
        }
    }
    return true;
}

static bool dec_tga(const uint8_t *d, size_t n, Img *out, char *err) {
    if (n < 18) IMG_FAIL("TGA: truncated");
    int idlen = d[0], cmt = d[1], type = d[2], cmo = d[3] | d[4] << 8, cml = d[5] | d[6] << 8, cmb = d[7];
    int w = d[12] | d[13] << 8, h = d[14] | d[15] << 8, bpp = d[16], desc = d[17];
    bool rle = type >= 9; int base = type & 7;
    if ((base != 1 && base != 2 && base != 3) || w < 1 || h < 1) IMG_FAIL("TGA: unsupported image type");
    if (base == 1 && (!cmt || cmb < 15)) IMG_FAIL("TGA: bad colour map");
    if (bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) IMG_FAIL("TGA: unsupported bit depth %d", bpp);
    if (!img_alloc(out, (uint32_t)w, (uint32_t)h, err)) return false;
    size_t p = 18 + (size_t)idlen; uint32_t pal[256]; int bytes = bpp / 8;
    if (cmt) {
        int eb = (cmb + 7) / 8;
        for (int i = 0; i < cml; i++) { size_t q = p + (size_t)i * eb; if (q + eb > n) break; uint32_t c = 0xFF000000u; if (eb == 3 || eb == 4) c = (uint32_t)(eb == 4 ? d[q + 3] : 255) << 24 | (uint32_t)d[q + 2] << 16 | (uint32_t)d[q + 1] << 8 | d[q]; if (i + cmo < 256 && i + cmo >= 0) pal[i + cmo] = c; }
        p += (size_t)cml * eb;
    }
    size_t total = (size_t)w * h, got = 0; bool top = (desc & 0x20) != 0;
    #define TGA_PIX(q, dst) do { \
        uint32_t c_; \
        if (base == 1) c_ = pal[(q)[0]]; \
        else if (base == 3) c_ = col_pack((q)[0], (q)[0], (q)[0], bytes == 2 ? (q)[1] : 255); \
        else if (bytes == 2) { int v_ = (q)[0] | (q)[1] << 8; c_ = col_pack(((v_ >> 10) & 31) * 255 / 31, ((v_ >> 5) & 31) * 255 / 31, (v_ & 31) * 255 / 31, 255); } \
        else c_ = col_pack((q)[2], (q)[1], (q)[0], bytes == 4 ? (q)[3] : 255); \
        (dst) = c_; } while (0)
    uint32_t *lin = kc_malloc(total * 4);
    while (got < total) {
        if (p >= n) { free(lin); free(out->px); out->px = NULL; IMG_FAIL("TGA: pixel data truncated"); }
        if (rle) {
            int hd = d[p++]; int cnt = (hd & 127) + 1;
            if (hd & 128) { if (p + bytes > n) break; uint32_t c; TGA_PIX(d + p, c); p += bytes; for (int i = 0; i < cnt && got < total; i++) lin[got++] = c; }
            else for (int i = 0; i < cnt && got < total; i++) { if (p + bytes > n) break; TGA_PIX(d + p, lin[got]); got++; p += bytes; }
        } else { if (p + bytes > n) break; TGA_PIX(d + p, lin[got]); got++; p += bytes; }
    }
    for (int y = 0; y < h; y++) { int sy = top ? y : h - 1 - y; memcpy(out->px + (size_t)y * w, lin + (size_t)sy * w, (size_t)w * 4); }
    free(lin);
    return true;
}

static bool dec_ppm(const uint8_t *d, size_t n, Img *out, char *err) {
    size_t p = 2; long v[3]; int got = 0; bool ascii = d[1] == '3';
    while (got < 3 && p < n) {
        while (p < n && (d[p] == ' ' || d[p] == '\n' || d[p] == '\r' || d[p] == '\t')) p++;
        if (p < n && d[p] == '#') { while (p < n && d[p] != '\n') p++; continue; }
        long x = 0; bool any = false; while (p < n && d[p] >= '0' && d[p] <= '9') { x = x * 10 + (d[p] - '0'); p++; any = true; }
        if (!any) IMG_FAIL("PPM: bad header"); v[got++] = x;
    }
    if (got < 3 || v[2] < 1 || v[2] > 255) IMG_FAIL("PPM: only 8-bit images are supported");
    p++;
    if (!img_alloc(out, (uint32_t)v[0], (uint32_t)v[1], err)) return false;
    for (long i = 0; i < v[0] * v[1]; i++) {
        int c[3];
        for (int k = 0; k < 3; k++) {
            if (ascii) { while (p < n && !(d[p] >= '0' && d[p] <= '9')) p++; int x = 0; while (p < n && d[p] >= '0' && d[p] <= '9') { x = x * 10 + (d[p] - '0'); p++; } c[k] = x; }
            else { if (p >= n) { free(out->px); out->px = NULL; IMG_FAIL("PPM: truncated"); } c[k] = d[p++]; }
        }
        out->px[i] = col_pack(c[0] * 255 / (int)v[2], c[1] * 255 / (int)v[2], c[2] * 255 / (int)v[2], 255);
    }
    return true;
}

bool img_decode(const uint8_t *d, size_t n, Img *out, char *err) {
    out->px = NULL; out->w = out->h = 0; err[0] = 0;
    if (n >= 8 && !memcmp(d, "\x89PNG\r\n\x1a\n", 8)) return dec_png(d, n, out, err);
    if (n >= 2 && d[0] == 'B' && d[1] == 'M') return dec_bmp(d, n, out, err);
    if (n >= 3 && d[0] == 'P' && (d[1] == '6' || d[1] == '3')) return dec_ppm(d, n, out, err);
    if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8) IMG_FAIL("JPEG is not supported — convert to PNG, TGA or BMP");
    if (n >= 4 && !memcmp(d, "DDS ", 4)) IMG_FAIL("DDS is not supported — convert to PNG or TGA");
    if (n >= 18 && (d[2] == 1 || d[2] == 2 || d[2] == 3 || d[2] == 9 || d[2] == 10 || d[2] == 11) && (d[1] == 0 || d[1] == 1)) return dec_tga(d, n, out, err);
    IMG_FAIL("unrecognised image format (supported: PNG, BMP, TGA, PPM)");
}

/* ============================================================ image encode */
static void put32be(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void put_chunk(FILE *f, const char *type, const uint8_t *d, size_t n) {
    uint8_t h[8]; put32be(h, (uint32_t)n); memcpy(h + 4, type, 4); fwrite(h, 1, 8, f);
    if (n) fwrite(d, 1, n, f);
    uint8_t *tmp = kc_malloc(n + 4); memcpy(tmp, type, 4); if (n) memcpy(tmp + 4, d, n);
    uint32_t crc = gm_crc32(0, tmp, n + 4); free(tmp);
    uint8_t c[4]; put32be(c, crc); fwrite(c, 1, 4, f);
}
bool img_save(const char *path, const uint32_t *px, int w, int h, char *err) {
    size_t pl = strlen(path); const char *ext = pl >= 4 ? path + pl - 4 : "";
    FILE *f = fopen(path, "wb");
    if (!f) { snprintf(err, 256, "cannot open '%s' for writing", path); return false; }
    if (!strcasecmp(ext, ".bmp")) {
        uint32_t rowb = ((uint32_t)w * 3 + 3) & ~3u, dsz = rowb * (uint32_t)h, fsz = 54 + dsz;
        uint8_t hd[54]; memset(hd, 0, 54); hd[0] = 'B'; hd[1] = 'M';
        hd[2] = (uint8_t)fsz; hd[3] = (uint8_t)(fsz >> 8); hd[4] = (uint8_t)(fsz >> 16); hd[5] = (uint8_t)(fsz >> 24); hd[10] = 54; hd[14] = 40;
        hd[18] = (uint8_t)w; hd[19] = (uint8_t)(w >> 8); hd[20] = (uint8_t)(w >> 16); hd[22] = (uint8_t)h; hd[23] = (uint8_t)(h >> 8); hd[24] = (uint8_t)(h >> 16);
        hd[26] = 1; hd[28] = 24; hd[34] = (uint8_t)dsz; hd[35] = (uint8_t)(dsz >> 8); hd[36] = (uint8_t)(dsz >> 16); hd[37] = (uint8_t)(dsz >> 24);
        fwrite(hd, 1, 54, f);
        uint8_t *row = kc_malloc(rowb);
        for (int y = h - 1; y >= 0; y--) { memset(row, 0, rowb); for (int x = 0; x < w; x++) { uint32_t c = px[(size_t)y * w + x]; row[x * 3] = (uint8_t)CB(c); row[x * 3 + 1] = (uint8_t)CG(c); row[x * 3 + 2] = (uint8_t)CR(c); } fwrite(row, 1, rowb, f); }
        free(row);
    } else if (!strcasecmp(ext, ".ppm")) {
        fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (size_t i = 0; i < (size_t)w * h; i++) { uint8_t c[3] = { (uint8_t)CR(px[i]), (uint8_t)CG(px[i]), (uint8_t)CB(px[i]) }; fwrite(c, 1, 3, f); }
    } else {                                                    /* PNG (stored deflate: big but dependency-free) */
        fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
        uint8_t ih[13]; put32be(ih, (uint32_t)w); put32be(ih + 4, (uint32_t)h); ih[8] = 8; ih[9] = 2; ih[10] = ih[11] = ih[12] = 0;
        put_chunk(f, "IHDR", ih, 13);
        size_t rawn = (size_t)h * (1 + (size_t)w * 3); uint8_t *raw = kc_malloc(rawn), *q = raw;
        for (int y = 0; y < h; y++) { *q++ = 0; for (int x = 0; x < w; x++) { uint32_t c = px[(size_t)y * w + x]; *q++ = (uint8_t)CR(c); *q++ = (uint8_t)CG(c); *q++ = (uint8_t)CB(c); } }
        size_t nblk = (rawn + 65534) / 65535; size_t zn = 2 + nblk * 5 + rawn + 4; uint8_t *z = kc_malloc(zn), *zp = z;
        *zp++ = 0x78; *zp++ = 0x01;
        uint32_t a = 1, b = 0;
        for (size_t o = 0; o < rawn; o += 65535) {
            size_t len = rawn - o < 65535 ? rawn - o : 65535; bool last = o + len >= rawn;
            *zp++ = last ? 1 : 0; *zp++ = (uint8_t)len; *zp++ = (uint8_t)(len >> 8); *zp++ = (uint8_t)~len; *zp++ = (uint8_t)(~len >> 8);
            memcpy(zp, raw + o, len); zp += len;
            for (size_t i = 0; i < len; i++) { a = (a + raw[o + i]) % 65521; b = (b + a) % 65521; }
        }
        put32be(zp, b << 16 | a); zp += 4;
        put_chunk(f, "IDAT", z, (size_t)(zp - z)); put_chunk(f, "IEND", NULL, 0);
        free(z); free(raw);
    }
    if (fclose(f) != 0) { snprintf(err, 256, "write error on '%s'", path); return false; }
    return true;
}
