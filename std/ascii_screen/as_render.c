/* std.ascii_screen — image -> ASCII/Unicode-art renderers.
 *
 * Modes:  ramp (luminance -> character density)   shape (best-matching glyph per 5x7 cell, crisp edges)
 *         half (▀ half blocks, 2 px/cell)          quad (▘▝▖▗ quadrant blocks, 2x2 px/cell)
 *         braille (2x4 dots/cell)                  edges (Sobel edge orientation  - / | \ )
 *         blocks (ramp using ░▒▓█)
 * Colour: none | fg | fg+bg;  dithering: none | floyd | bayer;  fit: contain | cover | stretch.
 */
#include "as_internal.h"

float *im_resample(const Image *src, int cx, int cy, int cw, int ch, int dw, int dh);   /* as_image.c */
void render_dims(const Image *im, const RenderOpts *r, int *W, int *H);

const char *RAMP_NAMES[] = { "classic", "detailed", "blocks", "minimal", "binary", "dots", NULL };
const char *ramp_preset(const char *n) {
    if (!strcmp(n, "classic")) return " .:-=+*#%@";
    if (!strcmp(n, "detailed")) return " .'`^\",:;Il!i><~+_-?][}{1)(|\\/tfjrxnuvczXYUJCLQ0OZmwqpdbkhao*#MW&8%B@$";
    if (!strcmp(n, "blocks")) return " \xE2\x96\x91\xE2\x96\x92\xE2\x96\x93\xE2\x96\x88";
    if (!strcmp(n, "minimal")) return " .oO@";
    if (!strcmp(n, "binary")) return " #";
    if (!strcmp(n, "dots")) return " .\xC2\xB7\xE2\x80\xA2\xE2\x97\x8F";
    return NULL;
}
void render_opts_default(RenderOpts *r) {
    memset(r, 0, sizeof *r);
    r->mode = "ramp"; r->ramp = NULL; r->color = 0; r->fit = "contain"; r->dither = "none";
    r->contrast = 1.0f; r->brightness = 0.0f; r->gamma = 1.0f; r->threshold = -1.0f; r->sharpen = 0.0f; r->aspect = 0.0f;
    r->matte = col_rgb(0, 0, 0); r->edge_min = 40; r->saturation = 1.0f; r->transparent = false;
}
void render_opts_parse(RenderOpts *r, const Opts *o) {
    r->width = opt_int(o, "width", r->width); r->height = opt_int(o, "height", r->height);
    r->mode = opt_str(o, "mode", r->mode); r->fit = opt_str(o, "fit", r->fit); r->dither = opt_str(o, "dither", r->dither);
    const KcValue *rv = opt_find(o, "ramp");
    if (rv) { if (rv->type != VAL_STRING) as_err("option \"ramp\" must be a string"); const char *pr = ramp_preset(rv->as.s->data); r->ramp = pr ? pr : rv->as.s->data; }
    const KcValue *cv = opt_find(o, "color");
    if (cv) {
        if (cv->type == VAL_STRING) { const char *s = cv->as.s->data; r->color = !strcmp(s, "none") || !strcmp(s, "off") ? 0 : (!strcmp(s, "bg") || !strcmp(s, "both") || !strcmp(s, "fg+bg")) ? 2 : 1; }
        else r->color = kc_truthy(*cv) ? 1 : 0;
    }
    r->contrast = (float)opt_num(o, "contrast", r->contrast); r->brightness = (float)opt_num(o, "brightness", r->brightness);
    r->gamma = (float)opt_num(o, "gamma", r->gamma); r->sharpen = (float)opt_num(o, "sharpen", r->sharpen); r->saturation = (float)opt_num(o, "saturation", r->saturation);
    r->aspect = (float)opt_num(o, "aspect", r->aspect); r->invert = opt_bool(o, "invert", r->invert); r->light_bg = opt_bool(o, "light_bg", r->light_bg);
    r->transparent = opt_bool(o, "transparent", r->transparent); r->matte = opt_col(o, "matte", r->matte); if (!(r->matte & COL_SET)) r->matte = col_rgb(0, 0, 0);
    r->edge_min = opt_int(o, "edge_min", r->edge_min); r->edge_fill = opt_bool(o, "edge_fill", r->edge_fill);
    const KcValue *tv = opt_find(o, "threshold"); if (tv) { if (tv->type == VAL_STRING) r->threshold = -1.0f; else r->threshold = (float)V_num(*tv, "threshold"); }
    if (r->gamma <= 0.01f) r->gamma = 1.0f;
    static const char *MODES[] = { "ramp", "shape", "half", "quad", "braille", "edges", "blocks", NULL }; bool ok = false;
    for (int i = 0; MODES[i]; i++) if (!strcmp(r->mode, MODES[i])) ok = true;
    if (!ok) as_err("unknown mode \"%s\" (ramp, shape, half, quad, braille, edges, blocks)", r->mode);
    if (strcmp(r->fit, "contain") && strcmp(r->fit, "cover") && strcmp(r->fit, "stretch")) as_err("unknown fit \"%s\" (contain, cover, stretch)", r->fit);
    if (strcmp(r->dither, "none") && strcmp(r->dither, "floyd") && strcmp(r->dither, "bayer")) as_err("unknown dither \"%s\" (none, floyd, bayer)", r->dither);
}

/* pixels per cell */
static void mode_px(const char *m, int *sx, int *sy) {
    if (!strcmp(m, "half")) { *sx = 1; *sy = 2; } else if (!strcmp(m, "quad")) { *sx = 2; *sy = 2; } else if (!strcmp(m, "braille")) { *sx = 2; *sy = 4; }
    else if (!strcmp(m, "shape")) { *sx = 5; *sy = 7; } else { *sx = 1; *sy = 1; }
}
void render_dims(const Image *im, const RenderOpts *r, int *Wo, int *Ho) {
    double aspect = r->aspect > 0 ? r->aspect : (T.cell_aspect > 0 ? T.cell_aspect : 2.0);
    double iw = im->w, ih = im->h; int W = r->width, H = r->height;
    /* half/quad/braille cells are still 1:aspect cells, so the cell-level maths is identical for every mode */
    if (W <= 0 && H <= 0) W = 80;
    if (W > 0 && H <= 0) H = (int)floor(W * ih / (iw * aspect) + 0.5);
    else if (H > 0 && W <= 0) W = (int)floor(H * aspect * iw / ih + 0.5);
    else if (!strcmp(r->fit, "contain")) {
        double w2 = H * aspect * iw / ih;
        if (w2 <= W) W = (int)floor(w2 + 0.5); else H = (int)floor(W * ih / (iw * aspect) + 0.5);
    }
    *Wo = as_clampi(W, 1, 1000); *Ho = as_clampi(H, 1, 600);
}

/* ------------------------------------------------------------ dithering */
static const uint8_t BAYER8[8][8] = {
    { 0, 32, 8, 40, 2, 34, 10, 42 }, { 48, 16, 56, 24, 50, 18, 58, 26 }, { 12, 44, 4, 36, 14, 46, 6, 38 }, { 60, 28, 52, 20, 62, 30, 54, 22 },
    { 3, 35, 11, 43, 1, 33, 9, 41 }, { 51, 19, 59, 27, 49, 17, 57, 25 }, { 15, 47, 7, 39, 13, 45, 5, 37 }, { 63, 31, 55, 23, 61, 29, 53, 21 } };
/* quantise ink[0..1] to `levels` levels; out[i] = level index */
static void quantize(const float *ink, int w, int h, int levels, const char *dither, int *out) {
    if (levels < 2) levels = 2;
    float L = (float)(levels - 1);
    if (!strcmp(dither, "floyd")) {
        float *e = kc_malloc((size_t)w * h * sizeof(float)); memcpy(e, ink, (size_t)w * h * sizeof(float));
        for (int y = 0; y < h; y++) {
            bool rev = y & 1;
            for (int xi = 0; xi < w; xi++) {
                int x = rev ? w - 1 - xi : xi; size_t i = (size_t)y * w + x;
                float v = as_clampf(e[i], 0, 1); int q = (int)floorf(v * L + 0.5f); out[i] = q; float err = v - (float)q / L; int dx = rev ? -1 : 1;
                if (x + dx >= 0 && x + dx < w) e[i + dx] += err * 7 / 16;
                if (y + 1 < h) { if (x - dx >= 0 && x - dx < w) e[i + w - dx] += err * 3 / 16; e[i + w] += err * 5 / 16; if (x + dx >= 0 && x + dx < w) e[i + w + dx] += err * 1 / 16; }
            }
        }
        free(e);
    } else if (!strcmp(dither, "bayer")) {
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { float t = ((float)BAYER8[y & 7][x & 7] + 0.5f) / 64.0f - 0.5f; out[(size_t)y * w + x] = as_clampi((int)floorf(ink[(size_t)y * w + x] * L + 0.5f + t), 0, levels - 1); }
    } else for (size_t i = 0; i < (size_t)w * h; i++) out[i] = as_clampi((int)floorf(ink[i] * L + 0.5f), 0, levels - 1);
}
static float otsu(const float *v, size_t n) {
    size_t hist[256] = { 0 }; for (size_t i = 0; i < n; i++) hist[as_clampi((int)(v[i] * 255.0f + 0.5f), 0, 255)]++;
    double sum = 0; for (int i = 0; i < 256; i++) sum += (double)i * (double)hist[i];
    double sb = 0, best = -1, thr = 127; size_t wb = 0;
    for (int t = 0; t < 256; t++) { wb += hist[t]; if (!wb) continue; size_t wf = n - wb; if (!wf) break; sb += (double)t * (double)hist[t]; double mb = sb / (double)wb, mf = (sum - sb) / (double)wf, bt = (double)wb * (double)wf * (mb - mf) * (mb - mf); if (bt > best) { best = bt; thr = t; } }
    return (float)(thr / 255.0);
}

/* ------------------------------------------------------------- planes */
typedef struct { int w, h; float *rgb; float *al; float *lum; } Planes;
static void planes_free(Planes *p) { free(p->rgb); free(p->al); free(p->lum); }
static void planes_build(Planes *p, float *f, int w, int h, const RenderOpts *r) {
    size_t n = (size_t)w * h; p->w = w; p->h = h; p->rgb = kc_malloc(n * 3 * sizeof(float)); p->al = kc_malloc(n * sizeof(float)); p->lum = kc_malloc(n * sizeof(float));
    float mr = (float)COL_R(r->matte) / 255.0f, mg = (float)COL_G(r->matte) / 255.0f, mb = (float)COL_B(r->matte) / 255.0f;
    for (size_t i = 0; i < n; i++) {
        float a = f[i * 4 + 3] / 255.0f, cr = 0, cg = 0, cb = 0;
        if (a > 0.0001f) { cr = f[i * 4] / 255.0f / a; cg = f[i * 4 + 1] / 255.0f / a; cb = f[i * 4 + 2] / 255.0f / a; }
        a = as_clampf(a, 0, 1);
        p->rgb[i * 3] = cr * a + mr * (1 - a); p->rgb[i * 3 + 1] = cg * a + mg * (1 - a); p->rgb[i * 3 + 2] = cb * a + mb * (1 - a); p->al[i] = a;
    }
    if (r->sharpen > 0.001f && w > 2 && h > 2) {
        float *c = kc_malloc(n * 3 * sizeof(float)); memcpy(c, p->rgb, n * 3 * sizeof(float));
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) for (int k = 0; k < 3; k++) {
            float s = 0; int cnt = 0;
            for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) { int xx = as_clampi(x + dx, 0, w - 1), yy = as_clampi(y + dy, 0, h - 1); s += c[((size_t)yy * w + xx) * 3 + k]; cnt++; }
            float blur = s / (float)cnt, v = c[((size_t)y * w + x) * 3 + k]; p->rgb[((size_t)y * w + x) * 3 + k] = v + r->sharpen * (v - blur);
        }
        free(c);
    }
    for (size_t i = 0; i < n; i++) {
        float v[3] = { p->rgb[i * 3], p->rgb[i * 3 + 1], p->rgb[i * 3 + 2] };
        for (int k = 0; k < 3; k++) { v[k] = (v[k] - 0.5f) * r->contrast + 0.5f + r->brightness; v[k] = as_clampf(v[k], 0, 1); if (r->gamma != 1.0f) v[k] = powf(v[k], 1.0f / r->gamma); }
        float lm = 0.2126f * v[0] + 0.7152f * v[1] + 0.0722f * v[2];
        if (r->saturation != 1.0f) for (int k = 0; k < 3; k++) v[k] = as_clampf(lm + (v[k] - lm) * r->saturation, 0, 1);
        p->rgb[i * 3] = v[0]; p->rgb[i * 3 + 1] = v[1]; p->rgb[i * 3 + 2] = v[2]; p->lum[i] = lm;
    }
}
static inline Col px_col(const Planes *p, size_t i) { return col_rgb((int)(p->rgb[i * 3] * 255.0f + 0.5f), (int)(p->rgb[i * 3 + 1] * 255.0f + 0.5f), (int)(p->rgb[i * 3 + 2] * 255.0f + 0.5f)); }
static inline float ink_of(const Planes *p, size_t i, const RenderOpts *r) { float v = p->lum[i]; if (r->light_bg) v = 1 - v; if (r->invert) v = 1 - v; return v; }

#define MONO_MARK 0x03FFFFFFu
static void strip_mono(Canvas *c, int x, int y, int w, int h) {
    for (int j = y; j < y + h; j++) for (int i = x; i < x + w; i++) { Cell *q = cv_at(c, i, j); if (q && q->fg == MONO_MARK) q->fg = 0; }
}

/* ------------------------------------------------------------- core */
void render_image_into(const Image *im, const RenderOpts *r, Canvas *dst, int x0, int y0) {
    int sx, sy; mode_px(r->mode, &sx, &sy);
    int W, H; render_dims(im, r, &W, &H);
    double aspect = r->aspect > 0 ? r->aspect : (T.cell_aspect > 0 ? T.cell_aspect : 2.0);
    int cx = 0, cy = 0, cw = im->w, ch = im->h;
    if (!strcmp(r->fit, "cover") && r->width > 0 && r->height > 0) {
        double rt = (double)W / ((double)H * aspect), ri = (double)im->w / im->h;
        if (ri > rt) { cw = (int)floor(im->h * rt + 0.5); cx = (im->w - cw) / 2; } else { ch = (int)floor(im->w / rt + 0.5); cy = (im->h - ch) / 2; }
        if (cw < 1) cw = 1; if (ch < 1) ch = 1;
    }
    int pw = W * sx, ph = H * sy;
    float *f = im_resample(im, cx, cy, cw, ch, pw, ph);
    Planes P; planes_build(&P, f, pw, ph, r); free(f);
    size_t n = (size_t)pw * ph;
    bool color = r->color >= 1; const char *m = r->mode;
    if (!strcmp(m, "ramp") || !strcmp(m, "blocks")) {
        const char *rs = r->ramp ? r->ramp : (!strcmp(m, "blocks") ? ramp_preset("blocks") : ramp_preset("classic"));
        uint32_t rp[256]; int N = 0; const char *q = rs; while (*q && N < 256) rp[N++] = utf8_next(&q);
        if (N < 2) as_err("ramp needs at least 2 characters");
        float *ink = kc_malloc(n * sizeof(float)); for (size_t i = 0; i < n; i++) ink[i] = ink_of(&P, i, r);
        int *idx = kc_malloc(n * sizeof(int)); quantize(ink, pw, ph, N, r->dither, idx);
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
            size_t i = (size_t)y * W + x; if (r->transparent && P.al[i] < 0.35f) continue;
            Col fg = color ? px_col(&P, i) : 0, bg = r->color == 2 ? col_scale(px_col(&P, i), 0.28f) : 0;
            if (r->color == 2) cv_fill(dst, x0 + x, y0 + y, 1, 1, rp[idx[i]], fg, bg, 0); else cv_put(dst, x0 + x, y0 + y, rp[idx[i]], fg, bg, 0);
        }
        free(ink); free(idx);
    } else if (!strcmp(m, "half") || !strcmp(m, "quad") || !strcmp(m, "braille")) {
        HrMode hm; hr_mode_parse(m, &hm); bool binary = !strcmp(m, "braille") || !color;
        Col *px = kc_malloc(n * sizeof(Col));
        if (binary) {
            float *ink = kc_malloc(n * sizeof(float)); for (size_t i = 0; i < n; i++) ink[i] = ink_of(&P, i, r);
            float thr = r->threshold >= 0 ? r->threshold : otsu(ink, n);
            if (!strcmp(r->dither, "none")) { for (size_t i = 0; i < n; i++) px[i] = ink[i] > thr ? (color ? px_col(&P, i) : MONO_MARK) : 0; }
            else {
                float *adj = kc_malloc(n * sizeof(float)); float lo = as_clampf(thr, 0.02f, 0.98f);
                for (size_t i = 0; i < n; i++) adj[i] = ink[i] < lo ? ink[i] * 0.5f / lo : 0.5f + (ink[i] - lo) * 0.5f / (1 - lo);   /* remap so the threshold sits at 0.5 */
                int *q = kc_malloc(n * sizeof(int)); quantize(adj, pw, ph, 2, r->dither, q);
                for (size_t i = 0; i < n; i++) px[i] = q[i] ? (color ? px_col(&P, i) : MONO_MARK) : 0;
                free(adj); free(q);
            }
            free(ink);
        } else for (size_t i = 0; i < n; i++) px[i] = (r->transparent && P.al[i] < 0.35f) ? 0 : px_col(&P, i);
        if (r->transparent) for (size_t i = 0; i < n; i++) if (P.al[i] < 0.35f) px[i] = 0;
        hr_to_cells(px, pw, ph, hm, dst, x0, y0, 0, 0); free(px);
        if (!color || binary) strip_mono(dst, x0, y0, W, H);
    } else if (!strcmp(m, "shape")) {
        static float masks[95][35]; static int mcnt[95]; static bool init = false;
        if (!init) { for (int g = 0; g < 95; g++) { uint8_t rows[7]; glyph_mask_5x7((uint32_t)(32 + g), rows); mcnt[g] = 0; for (int yy = 0; yy < 7; yy++) for (int xx = 0; xx < 5; xx++) { float v = (rows[yy] >> (4 - xx)) & 1 ? 1.0f : 0.0f; masks[g][yy * 5 + xx] = v; mcnt[g] += (int)v; } } init = true; }
        bool allow[95]; int na = 0;
        if (r->ramp) { memset(allow, 0, sizeof allow); const char *q = r->ramp; while (*q) { uint32_t cp = utf8_next(&q); if (cp >= 32 && cp < 127) { allow[cp - 32] = true; na++; } } if (!na) as_err("shape mode needs ASCII characters in \"ramp\""); }
        else { for (int g = 0; g < 95; g++) allow[g] = true; }
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
            size_t ci = (size_t)(y * 7) * pw + (size_t)x * 5; float ink[35]; float ar = 0, ag = 0, ab = 0, aa = 0;
            for (int yy = 0; yy < 7; yy++) for (int xx = 0; xx < 5; xx++) { size_t i = ci + (size_t)yy * pw + xx; ink[yy * 5 + xx] = ink_of(&P, i, r); ar += P.rgb[i * 3]; ag += P.rgb[i * 3 + 1]; ab += P.rgb[i * 3 + 2]; aa += P.al[i]; }
            if (r->transparent && aa / 35.0f < 0.35f) continue;
            int best = 0; float bs = 1e9f;
            for (int g = 0; g < 95; g++) { if (!allow[g]) continue; float dot = 0; for (int k = 0; k < 35; k++) if (masks[g][k] > 0) dot += ink[k]; float sc = (float)mcnt[g] - 2.0f * dot; if (sc < bs) { bs = sc; best = g; } }
            Col fg = color ? col_rgb((int)(ar / 35 * 255), (int)(ag / 35 * 255), (int)(ab / 35 * 255)) : 0, bg = r->color == 2 ? col_scale(fg, 0.28f) : 0;
            if (r->color == 2) cv_fill(dst, x0 + x, y0 + y, 1, 1, (uint32_t)(32 + best), fg, bg, 0); else cv_put(dst, x0 + x, y0 + y, (uint32_t)(32 + best), fg, bg, 0);
        }
    } else {                                                                                     /* edges */
        const char *cls = ramp_preset("classic"); size_t cl = strlen(cls);
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
            size_t i = (size_t)y * W + x; if (r->transparent && P.al[i] < 0.35f) continue;
            #define LUM(xx, yy) P.lum[(size_t)as_clampi(yy, 0, H - 1) * W + as_clampi(xx, 0, W - 1)]
            float gx = (LUM(x + 1, y - 1) + 2 * LUM(x + 1, y) + LUM(x + 1, y + 1)) - (LUM(x - 1, y - 1) + 2 * LUM(x - 1, y) + LUM(x - 1, y + 1));
            float gy = (LUM(x - 1, y + 1) + 2 * LUM(x, y + 1) + LUM(x + 1, y + 1)) - (LUM(x - 1, y - 1) + 2 * LUM(x, y - 1) + LUM(x + 1, y - 1));
            #undef LUM
            gy /= (float)aspect;
            float mag = hypotf(gx, gy) / 4.0f * 255.0f; uint32_t ch2 = ' ';
            if (mag >= (float)r->edge_min) {
                float tx = -gy, ty = gx, ang = atan2f(-ty, tx); if (ang < 0) ang += (float)M_PI; if (ang >= (float)M_PI) ang -= (float)M_PI;
                float a8 = ang / ((float)M_PI / 8.0f);
                ch2 = (a8 < 1 || a8 >= 7) ? '-' : a8 < 3 ? '/' : a8 < 5 ? '|' : '\\';
            } else if (r->edge_fill) { float v = ink_of(&P, i, r); ch2 = (uint32_t)(unsigned char)cls[as_clampi((int)(v * (float)(cl - 1) + 0.5f), 0, (int)cl - 1)]; }
            else continue;
            Col fg = color ? px_col(&P, i) : 0; cv_put(dst, x0 + x, y0 + y, ch2, fg, 0, 0);
        }
    }
    planes_free(&P);
}
int render_image_to_canvas(const Image *im, const RenderOpts *r, int *ow, int *oh) {
    int W, H; render_dims(im, r, &W, &H);
    int id = cv_new(W, H); Canvas *c = cv_get(id);
    render_image_into(im, r, c, 0, 0);
    if (ow) *ow = W; if (oh) *oh = H;
    return id;
}
