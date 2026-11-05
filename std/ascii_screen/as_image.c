/* std.ascii_screen — image store, decoders (GIF, baseline JPEG, PGM; PNG/BMP/TGA/PPM come from std.game),
 * the optional ffmpeg bridge (any other image format, any video), and box/linear resampling. */
#include "as_internal.h"
#include <errno.h>
#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <io.h>
#  include <fcntl.h>
#endif

/* std.game's decoder (PNG / BMP / TGA / PPM P3+P6). Layout matches Image's first three fields. */
typedef struct { int w, h; uint32_t *px; } GmImg;
bool img_decode(const uint8_t *d, size_t n, GmImg *out, char *err);
bool img_save(const char *path, const uint32_t *px, int w, int h, char *err);

#define ERRF(...) do { snprintf(err, 256, __VA_ARGS__); return false; } while (0)

/* ============================================================== store */
static Image **IMS; static int im_cap;
Image *im_get(int id) { return (id < 1 || id >= im_cap || !IMS[id] || !IMS[id]->used) ? NULL : IMS[id]; }
Image *im_must(int id, const char *who) { Image *im = im_get(id); if (!im) as_err("%s: invalid image %d", who, id); return im; }
void im_release(Image *im) {
    if (!im) return;
    free(im->px); im->px = NULL;
    if (im->frames) { for (int i = 0; i < im->nframes; i++) if (im->frames[i] != im->px) free(im->frames[i]); free(im->frames); im->frames = NULL; }
    free(im->delays_ms); im->delays_ms = NULL; im->nframes = 0; im->w = im->h = 0;
}
static int im_slot(void) {
    for (int i = 1; i < im_cap; i++) if (!IMS[i] || !IMS[i]->used) return i;
    int nc = im_cap ? im_cap * 2 : 16; IMS = kc_realloc(IMS, (size_t)nc * sizeof *IMS);
    for (int i = im_cap; i < nc; i++) IMS[i] = NULL;
    int id = im_cap ? im_cap : 1; im_cap = nc; return id;
}
static int im_register(Image *src) {            /* moves *src into a slot */
    int id = im_slot();
    if (!IMS[id]) IMS[id] = kc_malloc(sizeof(Image));
    *IMS[id] = *src; IMS[id]->used = true; memset(src, 0, sizeof *src);
    return id;
}
int im_new(int w, int h, uint32_t fill) {
    if (w < 1 || h < 1 || (int64_t)w * h > 100000000) as_err("image: invalid size %dx%d", w, h);
    Image im; memset(&im, 0, sizeof im); im.w = w; im.h = h; im.px = kc_malloc((size_t)w * h * 4);
    for (size_t i = 0; i < (size_t)w * h; i++) im.px[i] = fill;
    return im_register(&im);
}
int im_from_rgba(int w, int h, uint32_t *px) { Image im; memset(&im, 0, sizeof im); im.w = w; im.h = h; im.px = px; return im_register(&im); }
void im_free(int id) { Image *im = im_get(id); if (!im) return; im_release(im); im->used = false; }

/* ============================================================== PGM (P2/P5) */
static bool pgm_decode(const uint8_t *d, size_t n, Image *out, char *err) {
    size_t p = 2; int v[3], k = 0;
    while (k < 3 && p < n) {
        while (p < n && (isspace(d[p]) || d[p] == '#')) { if (d[p] == '#') while (p < n && d[p] != '\n') p++; else p++; }
        if (p >= n || !isdigit(d[p])) ERRF("PGM: bad header");
        v[k++] = atoi((const char *)d + p); while (p < n && isdigit(d[p])) p++;
    }
    if (k < 3 || v[0] < 1 || v[1] < 1 || v[2] < 1 || v[2] > 255 || (int64_t)v[0] * v[1] > 100000000) ERRF("PGM: unsupported header");
    p++;
    out->w = v[0]; out->h = v[1]; out->px = kc_malloc((size_t)v[0] * v[1] * 4);
    for (size_t i = 0; i < (size_t)v[0] * v[1]; i++) {
        int g;
        if (d[1] == '5') { if (p >= n) { free(out->px); out->px = NULL; ERRF("PGM: truncated"); } g = d[p++]; }
        else { while (p < n && isspace(d[p])) p++; if (p >= n) { free(out->px); out->px = NULL; ERRF("PGM: truncated"); } g = atoi((const char *)d + p); while (p < n && isdigit(d[p])) p++; }
        g = g * 255 / v[2]; out->px[i] = 0xFF000000u | (uint32_t)g << 16 | (uint32_t)g << 8 | (uint32_t)g;
    }
    return true;
}

/* ================================================================ GIF */
typedef struct { const uint8_t *d; size_t n, p; } Rd;
static bool lzw_decode_inner(Rd *r, int min_code, uint8_t *out, size_t out_n, size_t *blk_left_out, bool *term_out) {
    int clear = 1 << min_code, eoi = clear + 1, next = eoi + 1, size = min_code + 1, prev = -1;
    static uint16_t prefix[4096]; static uint8_t suffix[4096], stack[4097];
    for (int i = 0; i < clear; i++) { prefix[i] = 0xFFFF; suffix[i] = (uint8_t)i; }
    size_t o = 0; uint32_t bits = 0; int nb = 0; size_t blk_left = 0; *term_out = false; *blk_left_out = 0;
    while (o < out_n) {
        while (nb < size) {                                           /* refill from sub-blocks */
            if (blk_left == 0) { if (r->p >= r->n) { *blk_left_out = 0; *term_out = true; return o > 0; } blk_left = r->d[r->p++]; if (blk_left == 0) { *term_out = true; *blk_left_out = 0; return true; } }
            if (r->p >= r->n) { *blk_left_out = 0; *term_out = true; return o > 0; }
            bits |= (uint32_t)r->d[r->p++] << nb; nb += 8; blk_left--;
        }
        int code = (int)(bits & ((1u << size) - 1)); bits >>= size; nb -= size;
        if (code == clear) { next = eoi + 1; size = min_code + 1; prev = -1; continue; }
        if (code == eoi) break;
        int cur = code, sp = 0;
        if (prev < 0) { if (code >= clear) return false; out[o++] = (uint8_t)code; prev = code; continue; }
        if (code >= next) { if (code > next) return false; stack[sp++] = 0; cur = prev; }   /* KwKwK case: first byte patched below */
        while (cur >= clear) { if (sp >= 4096) return false; stack[sp++] = suffix[cur]; cur = prefix[cur]; }
        stack[sp++] = suffix[cur]; uint8_t first = suffix[cur];
        if (code >= next) stack[0] = first;
        while (sp > 0 && o < out_n) out[o++] = stack[--sp];
        if (next < 4096) { prefix[next] = (uint16_t)prev; suffix[next] = first; next++; if (next == (1 << size) && size < 12) size++; }
        prev = code;
    }
    *blk_left_out = blk_left;
    return true;
}
static bool lzw_decode(Rd *r, int min_code, uint8_t *out, size_t out_n) {
    size_t left = 0; bool term = false; bool ok = lzw_decode_inner(r, min_code, out, out_n, &left, &term);
    if (!term) { r->p += left; while (r->p < r->n) { uint8_t sz = r->d[r->p++]; if (!sz) break; r->p += sz; } }
    return ok;
}
bool gif_decode(const uint8_t *d, size_t n, Image *out, char *err) {
    if (n < 13 || memcmp(d, "GIF8", 4) || (d[4] != '7' && d[4] != '9')) ERRF("not a GIF");
    int W = d[6] | d[7] << 8, H = d[8] | d[9] << 8; if (W < 1 || H < 1 || (int64_t)W * H > 50000000) ERRF("GIF: unsupported size");
    Rd r = { d, n, 13 }; uint8_t flags = d[10]; uint32_t gct[256]; int ngct = 0;
    if (flags & 0x80) { ngct = 2 << (flags & 7); if (r.p + (size_t)ngct * 3 > n) ERRF("GIF: truncated"); for (int i = 0; i < ngct; i++) { gct[i] = 0xFF000000u | (uint32_t)d[r.p] << 16 | (uint32_t)d[r.p + 1] << 8 | d[r.p + 2]; r.p += 3; } }
    size_t fb = (size_t)W * H; uint32_t *canvas = kc_malloc(fb * 4), *prevc = kc_malloc(fb * 4); memset(canvas, 0, fb * 4);
    int gce_disp = 0, gce_trans = -1, gce_delay = 10; int cap = 8, cnt = 0; uint32_t **frames = kc_malloc((size_t)cap * sizeof *frames); int *delays = kc_malloc((size_t)cap * sizeof *delays);
    bool ok = true;
    while (r.p < n) {
        uint8_t b = d[r.p++];
        if (b == 0x3B) break;
        if (b == 0x21) {
            if (r.p >= n) break; uint8_t lab = d[r.p++];
            if (lab == 0xF9 && r.p + 5 < n && d[r.p] == 4) { uint8_t f = d[r.p + 1]; gce_disp = (f >> 2) & 7; gce_delay = d[r.p + 2] | d[r.p + 3] << 8; gce_trans = (f & 1) ? d[r.p + 4] : -1; }
            while (r.p < n) { uint8_t sz = d[r.p++]; if (!sz) break; r.p += sz; }
            continue;
        }
        if (b != 0x2C) { ok = cnt > 0; break; }
        if (r.p + 9 > n) { ok = cnt > 0; break; }
        int ix = d[r.p] | d[r.p + 1] << 8, iy = d[r.p + 2] | d[r.p + 3] << 8, iw = d[r.p + 4] | d[r.p + 5] << 8, ih = d[r.p + 6] | d[r.p + 7] << 8; uint8_t lf = d[r.p + 8]; r.p += 9;
        uint32_t lct[256]; uint32_t *pal = gct; int npal = ngct;
        if (lf & 0x80) { npal = 2 << (lf & 7); if (r.p + (size_t)npal * 3 > n) { ok = cnt > 0; break; } for (int i = 0; i < npal; i++) { lct[i] = 0xFF000000u | (uint32_t)d[r.p] << 16 | (uint32_t)d[r.p + 1] << 8 | d[r.p + 2]; r.p += 3; } pal = lct; }
        if (r.p >= n) { ok = cnt > 0; break; }
        int minc = d[r.p++]; if (minc < 2 || minc > 11) { ok = cnt > 0; break; }
        size_t pix = (size_t)iw * ih; uint8_t *idx = kc_malloc(pix ? pix : 1); memset(idx, 0, pix ? pix : 1);
        bool good = lzw_decode(&r, minc, idx, pix);
        if (!good && cnt == 0) { free(idx); ok = false; break; }
        if (gce_disp == 3) memcpy(prevc, canvas, fb * 4);
        /* de-interlace row order */
        for (int row = 0; row < ih; row++) {
            int dy = row;
            if (lf & 0x40) { int h4 = (ih + 7) / 8, h2 = (ih + 3) / 8 + h4, h1 = (ih + 1) / 4 + h2; if (row < h4) dy = row * 8; else if (row < h2) dy = (row - h4) * 8 + 4; else if (row < h1) dy = (row - h2) * 4 + 2; else dy = (row - h1) * 2 + 1; }
            int y = iy + dy; if (y < 0 || y >= H) continue;
            for (int x = 0; x < iw; x++) { int cx = ix + x; if (cx < 0 || cx >= W) continue; int ci = idx[(size_t)row * iw + x]; if (ci == gce_trans) continue; canvas[(size_t)y * W + cx] = ci < npal ? pal[ci] : 0xFF000000u; }
        }
        free(idx);
        if (cnt >= cap) { cap *= 2; frames = kc_realloc(frames, (size_t)cap * sizeof *frames); delays = kc_realloc(delays, (size_t)cap * sizeof *delays); }
        frames[cnt] = kc_malloc(fb * 4); memcpy(frames[cnt], canvas, fb * 4); delays[cnt] = gce_delay <= 1 ? 100 : gce_delay * 10; cnt++;
        if (gce_disp == 2) { for (int y = iy; y < iy + ih && y < H; y++) for (int x = ix; x < ix + iw && x < W; x++) if (y >= 0 && x >= 0) canvas[(size_t)y * W + x] = 0; }
        else if (gce_disp == 3) memcpy(canvas, prevc, fb * 4);
        gce_disp = 0; gce_trans = -1; gce_delay = 10;
        if (cnt >= 5000) break;
    }
    free(canvas); free(prevc);
    if (!ok || cnt == 0) { for (int i = 0; i < cnt; i++) free(frames[i]); free(frames); free(delays); ERRF("GIF: no decodable frames"); }
    out->w = W; out->h = H; out->px = kc_malloc(fb * 4); memcpy(out->px, frames[0], fb * 4);
    out->nframes = cnt; out->frames = frames; out->delays_ms = delays;
    return true;
}

/* =========================================================== baseline JPEG */
typedef struct { uint8_t bits[17]; uint8_t vals[256]; int mincode[17], maxcode[18], valptr[17]; bool set; } Huff;
typedef struct { int id, h, v, tq, dc, ac, pred; uint8_t *plane; int pw, ph; } JComp;
typedef struct { const uint8_t *d; size_t n, p; uint32_t buf; int cnt; bool hit_marker; } JBits;
static const uint8_t ZZ[64] = { 0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63 };

static void huff_build(Huff *h) {
    int code = 0, k = 0;
    for (int l = 1; l <= 16; l++) { h->valptr[l] = k; h->mincode[l] = code; code += h->bits[l]; k += h->bits[l]; h->maxcode[l] = h->bits[l] ? code - 1 : -1; code <<= 1; }
    h->maxcode[17] = 0x7FFFFFFF; h->set = true;
}
static int jb_bit(JBits *b) {
    if (b->cnt == 0) {
        int c = 0;
        if (b->hit_marker || b->p >= b->n) { b->hit_marker = true; c = 0; }
        else { c = b->d[b->p++]; if (c == 0xFF) { int c2 = b->p < b->n ? b->d[b->p] : 0xD9; if (c2 == 0) b->p++; else { b->p--; b->hit_marker = true; c = 0; } } }
        b->buf = (uint32_t)c; b->cnt = 8;
    }
    b->cnt--; return (int)((b->buf >> b->cnt) & 1);
}
static int jb_bits(JBits *b, int n) { int v = 0; while (n--) v = (v << 1) | jb_bit(b); return v; }
static int huff_dec(JBits *b, const Huff *h) {
    int code = 0;
    for (int l = 1; l <= 16; l++) { code = (code << 1) | jb_bit(b); if (h->maxcode[l] >= 0 && code <= h->maxcode[l] && code >= h->mincode[l]) return h->vals[h->valptr[l] + code - h->mincode[l]]; }
    return -1;
}
static float COSTAB[8][8]; static bool cos_init = false;
static void idct8(const int16_t *in, uint8_t *out, int stride) {
    if (!cos_init) { for (int x = 0; x < 8; x++) for (int u = 0; u < 8; u++) COSTAB[x][u] = (float)((u == 0 ? 0.35355339059327373 : 0.5) * cos((2 * x + 1) * u * M_PI / 16.0)); cos_init = true; }
    float tmp[64];
    for (int v = 0; v < 8; v++) for (int x = 0; x < 8; x++) { float s = 0; for (int u = 0; u < 8; u++) s += COSTAB[x][u] * (float)in[v * 8 + u]; tmp[v * 8 + x] = s; }
    for (int x = 0; x < 8; x++) for (int y = 0; y < 8; y++) { float s = 0; for (int v = 0; v < 8; v++) s += COSTAB[y][v] * tmp[v * 8 + x]; int q = (int)lroundf(s) + 128; out[y * stride + x] = (uint8_t)(q < 0 ? 0 : q > 255 ? 255 : q); }
}
bool jpeg_decode(const uint8_t *d, size_t n, Image *out, char *err) {
    if (n < 4 || d[0] != 0xFF || d[1] != 0xD8) ERRF("not a JPEG");
    uint16_t qt[4][64]; memset(qt, 0, sizeof qt); Huff dc[4], ac[4]; memset(dc, 0, sizeof dc); memset(ac, 0, sizeof ac);
    JComp comp[4]; memset(comp, 0, sizeof comp); int ncomp = 0, W = 0, H = 0, hmax = 1, vmax = 1, restart = 0, orient = 1; bool got_sof = false, adobe = false; int adobe_tf = -1;
    size_t p = 2; bool done = false;
    while (p + 4 <= n && !done) {
        if (d[p] != 0xFF) { p++; continue; }
        uint8_t m = d[p + 1]; if (m == 0xFF) { p++; continue; }
        if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7) || m == 0x01 || m == 0) { p += 2; continue; }
        if (m == 0xD9) break;
        size_t len = (size_t)d[p + 2] << 8 | d[p + 3]; if (len < 2 || p + 2 + len > n) ERRF("JPEG: truncated segment");
        const uint8_t *s = d + p + 4; size_t sl = len - 2;
        if (m == 0xDB) { size_t q = 0; while (q < sl) { int pq = s[q] >> 4, tq = s[q] & 15; q++; if (tq > 3) ERRF("JPEG: bad DQT"); for (int i = 0; i < 64; i++) { if (pq) { if (q + 1 >= sl) ERRF("JPEG: bad DQT"); qt[tq][ZZ[i]] = (uint16_t)(s[q] << 8 | s[q + 1]); q += 2; } else { if (q >= sl) ERRF("JPEG: bad DQT"); qt[tq][ZZ[i]] = s[q++]; } } } }
        else if (m == 0xC4) { size_t q = 0; while (q + 17 <= sl) { int tc = s[q] >> 4, th = s[q] & 15; if (th > 3) ERRF("JPEG: bad DHT"); Huff *h = tc ? &ac[th] : &dc[th]; h->bits[0] = 0; int tot = 0; for (int i = 1; i <= 16; i++) { h->bits[i] = s[q + i]; tot += h->bits[i]; } q += 17; if (tot > 256 || q + (size_t)tot > sl) ERRF("JPEG: bad DHT"); memcpy(h->vals, s + q, (size_t)tot); q += (size_t)tot; huff_build(h); } }
        else if (m == 0xC0 || m == 0xC1) {
            if (sl < 6 || s[0] != 8) ERRF("JPEG: only 8-bit samples are supported");
            H = s[1] << 8 | s[2]; W = s[3] << 8 | s[4]; ncomp = s[5];
            if (W < 1 || H < 1 || (int64_t)W * H > 100000000) ERRF("JPEG: unsupported size");
            if ((ncomp != 1 && ncomp != 3) || sl < 6 + (size_t)ncomp * 3) ERRF("JPEG: %d-component images are not supported natively", ncomp);
            for (int i = 0; i < ncomp; i++) { comp[i].id = s[6 + i * 3]; comp[i].h = s[7 + i * 3] >> 4; comp[i].v = s[7 + i * 3] & 15; comp[i].tq = s[8 + i * 3] & 3; if (comp[i].h < 1 || comp[i].h > 4 || comp[i].v < 1 || comp[i].v > 4) ERRF("JPEG: bad sampling"); if (comp[i].h > hmax) hmax = comp[i].h; if (comp[i].v > vmax) vmax = comp[i].v; }
            got_sof = true;
        }
        else if (m == 0xC2 || m == 0xC3 || (m >= 0xC5 && m <= 0xCF && m != 0xC8 && m != 0xCC && m != 0xC4)) ERRF("JPEG: progressive/lossless/arithmetic JPEG is not supported natively (install ffmpeg to open it)");
        else if (m == 0xDD) { if (sl >= 2) restart = s[0] << 8 | s[1]; }
        else if (m == 0xEE && sl >= 12 && !memcmp(s, "Adobe", 5)) { adobe = true; adobe_tf = s[11]; }
        else if (m == 0xE1 && sl > 14 && !memcmp(s, "Exif\0\0", 6)) {                       /* EXIF orientation */
            const uint8_t *t = s + 6; size_t tl = sl - 6; bool le = t[0] == 'I';
            #define R16(o) (le ? (t[o] | t[(o) + 1] << 8) : (t[o] << 8 | t[(o) + 1]))
            #define R32(o) (le ? (t[o] | t[(o) + 1] << 8 | t[(o) + 2] << 16 | (uint32_t)t[(o) + 3] << 24) : ((uint32_t)t[o] << 24 | t[(o) + 1] << 16 | t[(o) + 2] << 8 | t[(o) + 3]))
            if (tl > 8) { size_t o = R32(4); if (o + 2 < tl) { int ne = R16(o); for (int i = 0; i < ne && o + 2 + (size_t)i * 12 + 12 <= tl; i++) { size_t e = o + 2 + (size_t)i * 12; if (R16(e) == 0x0112) orient = R16(e + 8); } } }
        }
        else if (m == 0xDA) {
            if (!got_sof) ERRF("JPEG: scan before frame header");
            int ns = s[0]; if (ns != ncomp) ERRF("JPEG: non-interleaved scans are not supported natively");
            for (int i = 0; i < ns; i++) { int cid = s[1 + i * 2], tb = s[2 + i * 2]; JComp *c = NULL; for (int k = 0; k < ncomp; k++) if (comp[k].id == cid) c = &comp[k]; if (!c) ERRF("JPEG: bad scan component"); c->dc = tb >> 4; c->ac = tb & 15; if (c->dc > 3 || c->ac > 3 || !dc[c->dc].set || !ac[c->ac].set) ERRF("JPEG: missing Huffman table"); }
            int mcux = (W + 8 * hmax - 1) / (8 * hmax), mcuy = (H + 8 * vmax - 1) / (8 * vmax);
            for (int i = 0; i < ncomp; i++) { comp[i].pw = mcux * comp[i].h * 8; comp[i].ph = mcuy * comp[i].v * 8; comp[i].plane = kc_malloc((size_t)comp[i].pw * comp[i].ph); memset(comp[i].plane, 128, (size_t)comp[i].pw * comp[i].ph); comp[i].pred = 0; }
            JBits b = { d, n, p + 2 + len, 0, 0, false }; int rst = 0;
            for (int my = 0; my < mcuy; my++) for (int mx = 0; mx < mcux; mx++) {
                if (restart && rst == restart) {                                                  /* byte-align, consume RSTn */
                    b.cnt = 0; b.hit_marker = false;
                    while (b.p + 1 < n && !(d[b.p] == 0xFF && d[b.p + 1] >= 0xD0 && d[b.p + 1] <= 0xD7)) { if (d[b.p] == 0xFF && d[b.p + 1] != 0 && d[b.p + 1] != 0xFF) break; b.p++; }
                    if (b.p + 1 < n && d[b.p] == 0xFF && d[b.p + 1] >= 0xD0 && d[b.p + 1] <= 0xD7) b.p += 2;
                    for (int i = 0; i < ncomp; i++) comp[i].pred = 0; rst = 0;
                }
                for (int ci = 0; ci < ncomp; ci++) {
                    JComp *c = &comp[ci];
                    for (int by = 0; by < c->v; by++) for (int bx = 0; bx < c->h; bx++) {
                        int16_t blk[64]; memset(blk, 0, sizeof blk);
                        int t = huff_dec(&b, &dc[c->dc]); if (t < 0 || t > 16) { for (int i = 0; i < ncomp; i++) free(comp[i].plane); ERRF("JPEG: corrupt data"); }
                        int diff = t ? jb_bits(&b, t) : 0; if (t && diff < (1 << (t - 1))) diff += 1 - (1 << t);
                        c->pred += diff; blk[0] = (int16_t)(c->pred * qt[c->tq][0]);
                        for (int k = 1; k < 64;) {
                            int rs = huff_dec(&b, &ac[c->ac]); if (rs < 0) { for (int i = 0; i < ncomp; i++) free(comp[i].plane); ERRF("JPEG: corrupt data"); }
                            int r = rs >> 4, sz = rs & 15;
                            if (sz == 0) { if (r == 15) { k += 16; continue; } break; }
                            k += r; if (k > 63) break;
                            int v = jb_bits(&b, sz); if (v < (1 << (sz - 1))) v += 1 - (1 << sz);
                            blk[ZZ[k]] = (int16_t)(v * qt[c->tq][ZZ[k]]); k++;
                        }
                        idct8(blk, c->plane + (size_t)(my * c->v * 8 + by * 8) * c->pw + (size_t)(mx * c->h * 8 + bx * 8), c->pw);
                    }
                }
                rst++;
            }
            done = true; break;
        }
        p += 2 + len;
    }
    if (!done) ERRF("JPEG: no image data");
    uint32_t *px = kc_malloc((size_t)W * H * 4);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int s[3];
        for (int i = 0; i < ncomp; i++) { int sx = x * comp[i].h / hmax, sy = y * comp[i].v / vmax; s[i] = comp[i].plane[(size_t)sy * comp[i].pw + sx]; }
        int r, g, bl;
        if (ncomp == 1) r = g = bl = s[0];
        else if (adobe && adobe_tf == 0) { r = s[0]; g = s[1]; bl = s[2]; }
        else { float Y = (float)s[0], cb = (float)s[1] - 128, cr = (float)s[2] - 128; r = (int)lroundf(Y + 1.402f * cr); g = (int)lroundf(Y - 0.344136f * cb - 0.714136f * cr); bl = (int)lroundf(Y + 1.772f * cb); }
        px[(size_t)y * W + x] = 0xFF000000u | (uint32_t)as_clampi(r, 0, 255) << 16 | (uint32_t)as_clampi(g, 0, 255) << 8 | (uint32_t)as_clampi(bl, 0, 255);
    }
    for (int i = 0; i < ncomp; i++) free(comp[i].plane);
    if (orient > 1 && orient <= 8) {                                                              /* apply EXIF orientation */
        bool swap = orient >= 5; int nw = swap ? H : W, nh = swap ? W : H; uint32_t *q = kc_malloc((size_t)nw * nh * 4);
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
            int dx, dy;
            switch (orient) { case 2: dx = W - 1 - x; dy = y; break; case 3: dx = W - 1 - x; dy = H - 1 - y; break; case 4: dx = x; dy = H - 1 - y; break;
                              case 5: dx = y; dy = x; break; case 6: dx = H - 1 - y; dy = x; break; case 7: dx = H - 1 - y; dy = W - 1 - x; break; default: dx = y; dy = W - 1 - x; break; }
            q[(size_t)dy * nw + dx] = px[(size_t)y * W + x];
        }
        free(px); px = q; W = nw; H = nh;
    }
    out->w = W; out->h = H; out->px = px; return true;
}

/* ================================================================ ffmpeg */
static int ff_state = -1;
static void shell_quote(Buf *b, const char *s) {
#if defined(_WIN32)
    buf_addc(b, '"'); for (; *s; s++) if (*s != '"' && *s != '\r' && *s != '\n') buf_addc(b, *s); buf_addc(b, '"');
#else
    buf_addc(b, '\''); for (; *s; s++) { if (*s == '\'') buf_adds(b, "'\\''"); else if (*s != '\n' && *s != '\r') buf_addc(b, *s); } buf_addc(b, '\'');
#endif
}
bool ffmpeg_available(void) {
    if (ff_state >= 0) return ff_state == 1;
#if defined(_WIN32)
    ff_state = system("ffmpeg -version >NUL 2>&1") == 0;
#else
    ff_state = system("ffmpeg -version >/dev/null 2>&1") == 0;
#endif
    return ff_state == 1;
}
#if defined(_WIN32)
#  define POPEN(c, m) _popen(c, m)
#  define PCLOSE(f) _pclose(f)
#else
#  define POPEN(c, m) popen(c, m)
#  define PCLOSE(f) pclose(f)
#endif
bool ffmpeg_probe(const char *path, int *w, int *h, double *fps, double *duration) {
    Buf c; buf_init(&c);
    buf_adds(&c, "ffprobe -v error -select_streams v:0 -show_entries stream=width,height,r_frame_rate,duration:format=duration -of default=noprint_wrappers=1 ");
    shell_quote(&c, path);
#if defined(_WIN32)
    buf_adds(&c, " 2>NUL");
#else
    buf_adds(&c, " 2>/dev/null");
#endif
    FILE *f = POPEN(c.d, "r"); buf_free(&c);
    int ww = 0, hh = 0; double fr = 0, du = 0;
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            if (!strncmp(line, "width=", 6)) ww = atoi(line + 6); else if (!strncmp(line, "height=", 7)) hh = atoi(line + 7);
            else if (!strncmp(line, "r_frame_rate=", 13)) { int a = 0, b = 1; if (sscanf(line + 13, "%d/%d", &a, &b) >= 1 && b > 0) fr = (double)a / b; }
            else if (!strncmp(line, "duration=", 9)) { double v = atof(line + 9); if (v > 0 && du <= 0) du = v; }
        }
        PCLOSE(f);
    }
    if (ww < 1 || hh < 1) {                                                                  /* no ffprobe: parse ffmpeg's banner */
        buf_init(&c); buf_adds(&c, "ffmpeg -hide_banner -nostdin -i "); shell_quote(&c, path);
#if defined(_WIN32)
        buf_adds(&c, " 2>&1");
#else
        buf_adds(&c, " 2>&1");
#endif
        f = POPEN(c.d, "r"); buf_free(&c);
        if (f) {
            char line[512];
            while (fgets(line, sizeof line, f)) {
                char *v = strstr(line, "Video:");
                if (v && ww < 1) {
                    for (char *q = v; *q; q++) if (isdigit((unsigned char)q[0]) && (q == v || !isalnum((unsigned char)q[-1])) ) { int a, b; int used = 0; if (sscanf(q, "%dx%d%n", &a, &b, &used) == 2 && a >= 16 && b >= 16 && a < 20000 && b < 20000 && (!isalnum((unsigned char)q[used]) || q[used] == ',' )) { ww = a; hh = b; break; } }
                    char *fp = strstr(v, " fps"); if (fp) { char *s = fp; while (s > v && (isdigit((unsigned char)s[-1]) || s[-1] == '.')) s--; fr = atof(s); }
                }
                char *dp = strstr(line, "Duration:"); if (dp) { int hh2, mm, ss, cs; if (sscanf(dp + 9, " %d:%d:%d.%d", &hh2, &mm, &ss, &cs) == 4) du = hh2 * 3600 + mm * 60 + ss + cs / 100.0; }
            }
            PCLOSE(f);
        }
    }
    if (ww < 1 || hh < 1) return false;
    if (w) *w = ww; if (h) *h = hh; if (fps) *fps = fr > 0 ? fr : 25; if (duration) *duration = du;
    return true;
}
FILE *ffmpeg_open_frames(const char *path, int w, int h, double fps, double start, const char *pix_fmt, bool loop) {
    Buf c; buf_init(&c);
    buf_adds(&c, "ffmpeg -v quiet -nostdin -an -sn");
    if (start > 0) buf_printf(&c, " -ss %.3f", start);
    if (loop) buf_adds(&c, " -stream_loop -1");
    buf_adds(&c, " -i "); shell_quote(&c, path);
    buf_printf(&c, " -vf \"scale=%d:%d:flags=area%s", w, h, "");
    if (fps > 0) buf_printf(&c, ",fps=%.4f", fps);
    buf_printf(&c, "\" -f rawvideo -pix_fmt %s -", pix_fmt);
    FILE *f = POPEN(c.d, "r"); buf_free(&c);
#if defined(_WIN32)
    if (f) _setmode(_fileno(f), 0x8000);
#endif
    return f;
}
void ffmpeg_close(FILE *f) { if (f) PCLOSE(f); }
bool ext_decode_image(const char *path, Image *out, char *err) {
    if (!ffmpeg_available()) ERRF("cannot decode this image format natively and ffmpeg was not found on PATH");
    int w, h; if (!ffmpeg_probe(path, &w, &h, NULL, NULL)) ERRF("ffmpeg could not read '%s'", path);
    if ((int64_t)w * h > 60000000) ERRF("image is too large (%dx%d)", w, h);
    FILE *f = ffmpeg_open_frames(path, w, h, 0, 0, "rgba", false); if (!f) ERRF("cannot run ffmpeg");
    size_t need = (size_t)w * h * 4; uint8_t *raw = kc_malloc(need); size_t got = 0;
    while (got < need) { size_t r = fread(raw + got, 1, need - got, f); if (r == 0) break; got += r; }
    ffmpeg_close(f);
    if (got < need) { free(raw); ERRF("ffmpeg produced no pixel data for '%s'", path); }
    out->w = w; out->h = h; out->px = kc_malloc((size_t)w * h * 4);
    for (size_t i = 0; i < (size_t)w * h; i++) out->px[i] = (uint32_t)raw[i * 4 + 3] << 24 | (uint32_t)raw[i * 4] << 16 | (uint32_t)raw[i * 4 + 1] << 8 | raw[i * 4 + 2];
    free(raw); return true;
}

/* ================================================================ loader */
static bool has_ext(const char *p, const char *ext) { size_t a = strlen(p), b = strlen(ext); if (a < b) return false; for (size_t i = 0; i < b; i++) if (tolower((unsigned char)p[a - b + i]) != ext[i]) return false; return true; }
int im_load(const char *path, int svg_w, int svg_h) {
    size_t n; uint8_t *d = as_read_file(path, &n);
    if (!d) as_err("image: cannot open '%s'", path);
    Image im; memset(&im, 0, sizeof im); char err[300] = ""; bool ok = false;
    if (n >= 8 && !memcmp(d, "\x89PNG\r\n\x1a\n", 8)) { GmImg g; ok = img_decode(d, n, &g, err); if (ok) { im.w = g.w; im.h = g.h; im.px = g.px; } }
    else if (n >= 6 && !memcmp(d, "GIF8", 4)) ok = gif_decode(d, n, &im, err);
    else if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8) { ok = jpeg_decode(d, n, &im, err); }
    else if (n >= 3 && d[0] == 'P' && (d[1] == '5' || d[1] == '2')) ok = pgm_decode(d, n, &im, err);
    else if ((n >= 2 && d[0] == 'B' && d[1] == 'M') || (n >= 3 && d[0] == 'P' && (d[1] == '6' || d[1] == '3')) || (n >= 18 && (has_ext(path, ".tga") || has_ext(path, ".tpic")))) { GmImg g; ok = img_decode(d, n, &g, err); if (ok) { im.w = g.w; im.h = g.h; im.px = g.px; } }
    else {
        bool is_svg = has_ext(path, ".svg") || has_ext(path, ".svgz");
        if (!is_svg) { for (size_t i = 0; i < n && i < 512; i++) if (d[i] == '<') { if (!strncmp((const char *)d + i, "<svg", 4) || !strncmp((const char *)d + i, "<?xml", 5)) is_svg = true; break; } }
        if (is_svg) ok = svg_render((const char *)d, n, svg_w, svg_h, &im, err);
        else { ok = ext_decode_image(path, &im, err); if (!ok && !ffmpeg_available()) snprintf(err, sizeof err, "unsupported image format (native: PNG, JPEG(baseline), GIF, BMP, TGA, PPM/PGM, SVG; install ffmpeg for WebP/AVIF/TIFF/progressive JPEG/...)"); }
    }
    /* a JPEG we cannot decode natively (progressive etc.) can still go through ffmpeg */
    if (!ok && n >= 3 && d[0] == 0xFF && d[1] == 0xD8 && ffmpeg_available()) { char e2[300]; memset(&im, 0, sizeof im); ok = ext_decode_image(path, &im, e2); }
    free(d);
    if (!ok) as_err("image: '%s': %s", path, err[0] ? err : "decode failed");
    return im_register(&im);
}

/* ============================================================ resampling */
typedef struct { int n; int *lo; int *cnt; float *w; int cap_w; } Axis;
static void axis_build(Axis *a, int src0, int srcn, int dst) {                      /* source range [src0, src0+srcn) -> dst samples */
    a->n = dst; a->lo = kc_malloc((size_t)dst * sizeof(int)); a->cnt = kc_malloc((size_t)dst * sizeof(int));
    double scale = (double)srcn / dst; size_t cap = 0; a->w = NULL;
    for (int i = 0; i < dst; i++) {
        double s0 = src0 + i * scale, s1 = s0 + scale;
        int need;
        if (scale >= 1.0) { int lo = (int)floor(s0), hi = (int)ceil(s1); need = hi - lo; a->lo[i] = lo; a->cnt[i] = need; }
        else { need = 2; a->lo[i] = (int)floor((s0 + s1) / 2 - 0.5); a->cnt[i] = 2; }
        cap += (size_t)need;
    }
    a->w = kc_malloc(cap * sizeof(float)); size_t o = 0;
    for (int i = 0; i < dst; i++) {
        double s0 = src0 + i * scale, s1 = s0 + scale;
        if (scale >= 1.0) {
            double tot = 0; size_t base = o;
            for (int k = 0; k < a->cnt[i]; k++) { int sx = a->lo[i] + k; double lo = sx > s0 ? sx : s0, hi = (sx + 1) < s1 ? (sx + 1) : s1; double wgt = hi > lo ? hi - lo : 0; a->w[o++] = (float)wgt; tot += wgt; }
            if (tot <= 0) { a->w[base] = 1; tot = 1; }
            for (size_t k = base; k < o; k++) a->w[k] = (float)(a->w[k] / tot);
        } else {
            double c = (s0 + s1) / 2 - 0.5; int l = (int)floor(c); double f = c - l;
            a->w[o++] = (float)(1 - f); a->w[o++] = (float)f;
        }
    }
}
static void axis_free(Axis *a) { free(a->lo); free(a->cnt); free(a->w); }
/* Resample a crop of src to dw x dh. Returns malloc'd float RGBA, premultiplied, 0..255 (4 floats / pixel). */
float *im_resample(const Image *src, int cx, int cy, int cw, int ch, int dw, int dh) {
    if (cw < 1) cw = 1; if (ch < 1) ch = 1;
    Axis ax, ay; axis_build(&ax, cx, cw, dw); axis_build(&ay, cy, ch, dh);
    float *tmp = kc_malloc((size_t)dw * ch * 4 * sizeof(float));
    /* horizontal pass over the crop rows */
    for (int y = 0; y < ch; y++) {
        int sy = as_clampi(cy + y, 0, src->h - 1); const uint32_t *row = src->px + (size_t)sy * src->w; size_t wo = 0;
        for (int x = 0; x < dw; x++) {
            float r = 0, g = 0, b = 0, a = 0;
            for (int k = 0; k < ax.cnt[x]; k++) {
                int sx = as_clampi(ax.lo[x] + k, 0, src->w - 1); uint32_t p = row[sx]; float w = ax.w[wo + (size_t)k], al = (float)(p >> 24) / 255.0f;
                r += w * al * (float)((p >> 16) & 255); g += w * al * (float)((p >> 8) & 255); b += w * al * (float)(p & 255); a += w * al * 255.0f;
            }
            wo += (size_t)ax.cnt[x];
            float *o = tmp + ((size_t)y * dw + x) * 4; o[0] = r; o[1] = g; o[2] = b; o[3] = a;
        }
    }
    float *out = kc_malloc((size_t)dw * dh * 4 * sizeof(float)); size_t wo = 0;
    for (int y = 0; y < dh; y++) {
        for (int x = 0; x < dw; x++) {
            float r = 0, g = 0, b = 0, a = 0;
            for (int k = 0; k < ay.cnt[y]; k++) {
                int sy = as_clampi(ay.lo[y] - cy + k, 0, ch - 1); float w = ay.w[wo + (size_t)k]; const float *p = tmp + ((size_t)sy * dw + x) * 4;
                r += w * p[0]; g += w * p[1]; b += w * p[2]; a += w * p[3];
            }
            float *o = out + ((size_t)y * dw + x) * 4; o[0] = r; o[1] = g; o[2] = b; o[3] = a;
        }
        wo += (size_t)ay.cnt[y];
    }
    free(tmp); axis_free(&ax); axis_free(&ay);
    return out;
}
Image *im_resize_box(const Image *src, int dw, int dh) {
    float *f = im_resample(src, 0, 0, src->w, src->h, dw, dh);
    Image *o = kc_malloc(sizeof(Image)); memset(o, 0, sizeof *o); o->w = dw; o->h = dh; o->px = kc_malloc((size_t)dw * dh * 4);
    for (size_t i = 0; i < (size_t)dw * dh; i++) {
        float a = f[i * 4 + 3]; float k = a > 0 ? 255.0f / a : 0;
        int r = (int)lroundf(f[i * 4] * k), g = (int)lroundf(f[i * 4 + 1] * k), b = (int)lroundf(f[i * 4 + 2] * k), al = (int)lroundf(a);
        o->px[i] = (uint32_t)as_clampi(al, 0, 255) << 24 | (uint32_t)as_clampi(r, 0, 255) << 16 | (uint32_t)as_clampi(g, 0, 255) << 8 | (uint32_t)as_clampi(b, 0, 255);
    }
    free(f); return o;
}

bool im_save(const Image *im, const char *path, char *err) { return img_save(path, im->px, im->w, im->h, err); }
