/* std.ascii_screen — ASCII animations (GIF, image sequences, video, .kca, text frames), procedural
 * effects (plasma, fire, matrix, starfield, rain, snow, waves, tunnel, life, noise), transitions,
 * and the text / ANSI-art -> canvas loader. */
#include "as_internal.h"

/* ================================================== text / ANSI art loader */
static uint32_t CP437[256]; static bool cp437_init = false;
static void init_cp437(void) {
    static const char *hi = "\xC3\x87\xC3\xBC\xC3\xA9\xC3\xA2\xC3\xA4\xC3\xA0\xC3\xA5\xC3\xA7\xC3\xAA\xC3\xAB\xC3\xA8\xC3\xAF\xC3\xAE\xC3\xAC\xC3\x84\xC3\x85\xC3\x89\xC3\xA6\xC3\x86\xC3\xB4\xC3\xB6\xC3\xB2\xC3\xBB\xC3\xB9\xC3\xBF\xC3\x96\xC3\x9C\xC2\xA2\xC2\xA3\xC2\xA5\xE2\x82\xA7\xC6\x92"
        "\xC3\xA1\xC3\xAD\xC3\xB3\xC3\xBA\xC3\xB1\xC3\x91\xC2\xAA\xC2\xBA\xC2\xBF\xE2\x8C\x90\xC2\xAC\xC2\xBD\xC2\xBC\xC2\xA1\xC2\xAB\xC2\xBB\xE2\x96\x91\xE2\x96\x92\xE2\x96\x93\xE2\x94\x82\xE2\x94\xA4\xE2\x95\xA1\xE2\x95\xA2\xE2\x95\x96\xE2\x95\x95\xE2\x95\xA3\xE2\x95\x91\xE2\x95\x97\xE2\x95\x9D\xE2\x95\x9C\xE2\x95\x9B\xE2\x94\x90"
        "\xE2\x94\x94\xE2\x94\xB4\xE2\x94\xAC\xE2\x94\x9C\xE2\x94\x80\xE2\x94\xBC\xE2\x95\x9E\xE2\x95\x9F\xE2\x95\x9A\xE2\x95\x94\xE2\x95\xA9\xE2\x95\xA6\xE2\x95\xA0\xE2\x95\x90\xE2\x95\xAC\xE2\x95\xA7\xE2\x95\xA8\xE2\x95\xA4\xE2\x95\xA5\xE2\x95\x99\xE2\x95\x98\xE2\x95\x92\xE2\x95\x93\xE2\x95\xAB\xE2\x95\xAA\xE2\x94\x98\xE2\x94\x8C\xE2\x96\x88\xE2\x96\x84\xE2\x96\x8C\xE2\x96\x90\xE2\x96\x80"
        "\xCE\xB1\xC3\x9F\xCE\x93\xCF\x80\xCE\xA3\xCF\x83\xC2\xB5\xCF\x84\xCE\xA6\xCE\x98\xCE\xA9\xCE\xB4\xE2\x88\x9E\xCF\x86\xCE\xB5\xE2\x88\xA9\xE2\x89\xA1\xC2\xB1\xE2\x89\xA5\xE2\x89\xA4\xE2\x8C\xA0\xE2\x8C\xA1\xC3\xB7\xE2\x89\x88\xC2\xB0\xE2\x88\x99\xC2\xB7\xE2\x88\x9A\xE2\x81\xBF\xC2\xB2\xE2\x96\xA0\xC2\xA0";
    const char *p = hi; int n = 128; for (int i = 0; i < 128; i++) CP437[i] = (uint32_t)i;
    for (int i = 0; i < 128 && *p; i++) { CP437[128 + i] = utf8_next(&p); n--; }
    (void)n; static const uint32_t low[32] = { 0x20, 0x263A, 0x263B, 0x2665, 0x2666, 0x2663, 0x2660, 0x2022, 0x25D8, 0x25CB, 0x25D9, 0x2642, 0x2640, 0x266A, 0x266B, 0x263C, 0x25BA, 0x25C4, 0x2195, 0x203C, 0xB6, 0xA7, 0x25AC, 0x21A8, 0x2191, 0x2193, 0x2192, 0x2190, 0x221F, 0x2194, 0x25B2, 0x25BC };
    for (int i = 1; i < 32; i++) CP437[i] = low[i]; cp437_init = true;
}
int cv_from_text(const char *s, size_t n, bool cp437, int wrap) {
    if (cp437 && !cp437_init) init_cp437();
    for (size_t i = 0; i < n; i++) if (s[i] == 0x1A) { n = i; break; }               /* SAUCE record */
    int RW = wrap > 0 ? wrap : 512, cap = 64, rows = 1, maxw = 1; Cell **g = kc_malloc((size_t)cap * sizeof(Cell *));
    #define NEWROW(i) do { g[i] = kc_malloc((size_t)RW * sizeof(Cell)); for (int k_ = 0; k_ < RW; k_++) { g[i][k_].ch = ' '; g[i][k_].fg = g[i][k_].bg = 0; g[i][k_].attr = 0; } } while (0)
    NEWROW(0); int cx = 0, cy = 0; Col fg = 0, bg = 0; uint16_t at = 0; bool bold_bright = cp437; int fgi = -1;
    for (size_t i = 0; i < n;) {
        unsigned char b = (unsigned char)s[i];
        if (b == 0x1b && i + 1 < n && s[i + 1] == '[') {
            size_t j = i + 2; int pv[16], np = 0; int cur = -1;
            while (j < n && ((s[j] >= '0' && s[j] <= '9') || s[j] == ';' || s[j] == '?')) { if (s[j] == ';') { if (np < 16) pv[np++] = cur < 0 ? 0 : cur; cur = -1; } else if (s[j] != '?') cur = (cur < 0 ? 0 : cur) * 10 + (s[j] - '0'); j++; }
            if (np < 16) pv[np++] = cur < 0 ? -1 : cur; char f = j < n ? s[j] : 0; i = j + 1; int p0 = np && pv[0] > 0 ? pv[0] : 1;
            if (f == 'm') {
                if (np == 1 && pv[0] < 0) pv[0] = 0;
                for (int k = 0; k < np; k++) { int c = pv[k] < 0 ? 0 : pv[k];
                    if (c == 0) { fg = bg = 0; at = 0; fgi = -1; } else if (c == 1) { at |= AT_BOLD; if (bold_bright && fgi >= 0 && fgi < 8) fg = col_from_ansi(fgi + 8); } else if (c == 2) at |= AT_DIM; else if (c == 3) at |= AT_ITALIC; else if (c == 4) at |= AT_UNDER; else if (c == 5) at |= AT_BLINK; else if (c == 7) at |= AT_REVERSE; else if (c == 9) at |= AT_STRIKE;
                    else if (c == 22) at &= (uint16_t)~(AT_BOLD | AT_DIM); else if (c == 23) at &= (uint16_t)~AT_ITALIC; else if (c == 24) at &= (uint16_t)~AT_UNDER; else if (c == 25) at &= (uint16_t)~AT_BLINK; else if (c == 27) at &= (uint16_t)~AT_REVERSE; else if (c == 29) at &= (uint16_t)~AT_STRIKE;
                    else if (c >= 30 && c <= 37) { fgi = c - 30; fg = col_from_ansi(fgi + ((at & AT_BOLD) && bold_bright ? 8 : 0)); } else if (c == 39) { fg = 0; fgi = -1; }
                    else if (c >= 40 && c <= 47) bg = col_from_ansi(c - 40); else if (c == 49) bg = 0; else if (c >= 90 && c <= 97) { fg = col_from_ansi(c - 90 + 8); fgi = -1; } else if (c >= 100 && c <= 107) bg = col_from_ansi(c - 100 + 8);
                    else if ((c == 38 || c == 48) && k + 1 < np) { Col *t = c == 38 ? &fg : &bg; if (pv[k + 1] == 5 && k + 2 < np) { *t = col_from_ansi(pv[k + 2]); k += 2; } else if (pv[k + 1] == 2 && k + 4 < np) { *t = col_rgb(pv[k + 2], pv[k + 3], pv[k + 4]); k += 4; } if (c == 38) fgi = -1; } }
            } else if (f == 'A') cy = cy - p0 < 0 ? 0 : cy - p0; else if (f == 'B') { cy += p0; } else if (f == 'C') cx += p0; else if (f == 'D') cx = cx - p0 < 0 ? 0 : cx - p0;
            else if (f == 'H' || f == 'f') { cy = (np >= 1 && pv[0] > 0 ? pv[0] : 1) - 1; cx = (np >= 2 && pv[1] > 0 ? pv[1] : 1) - 1; }
            else if (f == 'J' && np && pv[0] == 2) { for (int y = 0; y < rows; y++) for (int x = 0; x < RW; x++) { g[y][x].ch = ' '; g[y][x].fg = g[y][x].bg = 0; g[y][x].attr = 0; } cx = cy = 0; }
            while (cy >= rows && rows < 8000) { if (rows >= cap) { cap *= 2; g = kc_realloc(g, (size_t)cap * sizeof(Cell *)); } NEWROW(rows); rows++; }
            continue;
        }
        if (b == '\r') { cx = 0; i++; continue; } if (b == '\n') { cx = 0; cy++; i++; while (cy >= rows && rows < 8000) { if (rows >= cap) { cap *= 2; g = kc_realloc(g, (size_t)cap * sizeof(Cell *)); } NEWROW(rows); rows++; } continue; }
        uint32_t cp; if (cp437) { cp = CP437[b]; i++; if (b == 0x1b) continue; } else { const char *q = s + i; cp = utf8_next(&q); i = (size_t)(q - s); }
        if (cp == '\t') { cx = (cx / 8 + 1) * 8; continue; }
        if (cp < 32 && !cp437) continue; int w = cp_width(cp); if (w == 0) continue;
        if (wrap > 0 && cx + w > wrap) { cx = 0; cy++; while (cy >= rows && rows < 8000) { if (rows >= cap) { cap *= 2; g = kc_realloc(g, (size_t)cap * sizeof(Cell *)); } NEWROW(rows); rows++; } }
        if (cx + w >= RW || cy >= rows) { continue; }
        g[cy][cx].ch = cp; g[cy][cx].fg = fg; g[cy][cx].bg = bg; g[cy][cx].attr = at; if (w == 2) { g[cy][cx + 1].ch = CH_CONT; g[cy][cx + 1].fg = fg; g[cy][cx + 1].bg = bg; g[cy][cx + 1].attr = at; }
        cx += w; if (cx > maxw) maxw = cx;
    }
    while (rows > 1) { bool blank = true; for (int x = 0; x < maxw; x++) if (g[rows - 1][x].ch != ' ' || g[rows - 1][x].bg) { blank = false; break; } if (!blank) break; free(g[rows - 1]); rows--; }
    if (wrap > 0 && maxw < wrap) maxw = wrap;
    int id = cv_new(maxw, rows); Canvas *c = cv_get(id);
    for (int y = 0; y < rows; y++) { memcpy(&c->c[(size_t)y * maxw], g[y], (size_t)maxw * sizeof(Cell)); free(g[y]); }
    free(g); return id;
}

/* ===================================================================== animations */
typedef struct {
    bool used; int w, h, n, cap; Cell **frames; int *delay; double fps;
    int cur, loop, dir; double t, speed; bool playing, finished; int64_t last_frame;
    bool stream; FILE *ff; char *path; RenderOpts ro; int pw, ph, sx, sy; double src_fps, start; Cell *live; uint8_t *raw; bool ff_loop; int64_t produced; bool have_frame; char *ro_ramp;
} Anim;
static Anim *ANS; static int nans, capans;
static Anim *an_get(int id) { return (id < 1 || id >= nans || !ANS[id].used) ? NULL : &ANS[id]; }
bool anim_valid(int id) { return an_get(id) != NULL; }
int anim_new(double fps) {
    int id = -1; for (int i = 1; i < nans; i++) if (!ANS[i].used) { id = i; break; }
    if (id < 0) { if (nans == 0) nans = 1; if (nans >= capans) { capans = capans ? capans * 2 : 8; ANS = kc_realloc(ANS, (size_t)capans * sizeof(Anim)); } id = nans++; }
    Anim *a = &ANS[id]; memset(a, 0, sizeof *a); a->used = true; a->fps = fps > 0 ? fps : 12; a->speed = 1; a->loop = 1; a->dir = 1; a->playing = true; a->last_frame = -1; return id;
}
static void an_release(Anim *a) {
    for (int i = 0; i < a->n; i++) free(a->frames[i]); free(a->frames); free(a->delay); free(a->live); free(a->raw); free(a->path); free(a->ro_ramp);
    if (a->ff) ffmpeg_close(a->ff); a->used = false;
}
void anim_free(int id) { Anim *a = an_get(id); if (a) an_release(a); }
void anims_free_all(void) { for (int i = 1; i < nans; i++) if (ANS[i].used) an_release(&ANS[i]); free(ANS); ANS = NULL; nans = capans = 0; }
void anim_add_canvas(int id, Canvas *c, int delay_ms) {
    Anim *a = an_get(id); if (!a) return;
    if (a->n == 0) { a->w = c->w; a->h = c->h; } else if (c->w != a->w || c->h != a->h) as_err("anim_add: frame is %dx%d but the animation is %dx%d", c->w, c->h, a->w, a->h);
    if (a->n >= a->cap) { a->cap = a->cap ? a->cap * 2 : 16; a->frames = kc_realloc(a->frames, (size_t)a->cap * sizeof(Cell *)); a->delay = kc_realloc(a->delay, (size_t)a->cap * sizeof(int)); }
    a->frames[a->n] = kc_malloc((size_t)a->w * a->h * sizeof(Cell)); memcpy(a->frames[a->n], c->c, (size_t)a->w * a->h * sizeof(Cell)); a->delay[a->n] = delay_ms > 0 ? delay_ms : (int)(1000.0 / a->fps + 0.5); a->n++;
}
static void put_u32(Buf *b, uint32_t v) { char t[4] = { (char)v, (char)(v >> 8), (char)(v >> 16), (char)(v >> 24) }; buf_add(b, t, 4); }
static void put_u16(Buf *b, uint16_t v) { char t[2] = { (char)v, (char)(v >> 8) }; buf_add(b, t, 2); }
static uint32_t get_u32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t get_u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
bool anim_save_file(int id, const char *path) {
    Anim *a = an_get(id); if (!a) return false; Buf b; buf_init(&b);
    size_t pl = strlen(path); bool txt = pl > 4 && !strcmp(path + pl - 4, ".txt");
    if (txt) {
        for (int f = 0; f < a->n; f++) { Canvas tmp = { a->w, a->h, a->frames[f], 0, 0, a->w, a->h, true }; size_t l; char *t = cv_to_text(&tmp, 0, 0, a->w, a->h, 0, true, &l); buf_add(&b, t, l); free(t); buf_adds(&b, "\n\x0c\n"); }
    } else {
        buf_add(&b, "KCA1", 4); put_u16(&b, (uint16_t)a->w); put_u16(&b, (uint16_t)a->h); put_u32(&b, (uint32_t)a->n); float fp = (float)a->fps; uint32_t fb; memcpy(&fb, &fp, 4); put_u32(&b, fb);
        for (int f = 0; f < a->n; f++) {
            put_u32(&b, (uint32_t)a->delay[f]); Cell *c = a->frames[f]; size_t total = (size_t)a->w * a->h, i = 0;
            while (i < total) { size_t j = i + 1; while (j < total && j - i < 65535 && c[j].ch == c[i].ch && c[j].fg == c[i].fg && c[j].bg == c[i].bg && c[j].attr == c[i].attr) j++; put_u16(&b, (uint16_t)(j - i)); put_u32(&b, c[i].ch); put_u32(&b, c[i].fg); put_u32(&b, c[i].bg); put_u16(&b, c[i].attr); i = j; }
        }
    }
    bool ok = as_write_file(path, b.d, b.n); buf_free(&b); return ok;
}
static bool kca_load(int id, const uint8_t *d, size_t n, char *err) {
    Anim *a = an_get(id); if (n < 16 || memcmp(d, "KCA1", 4)) { snprintf(err, 200, "not a .kca file"); return false; }
    int w = get_u16(d + 4), h = get_u16(d + 6); uint32_t nf = get_u32(d + 8); float fp; uint32_t fb = get_u32(d + 12); memcpy(&fp, &fb, 4);
    if (w < 1 || h < 1 || nf > 100000 || (int64_t)w * h > 4000000) { snprintf(err, 200, "bad .kca header"); return false; }
    a->w = w; a->h = h; a->fps = fp > 0 ? fp : 12; size_t p = 16, total = (size_t)w * h;
    for (uint32_t f = 0; f < nf; f++) {
        if (p + 4 > n) break; int delay = (int)get_u32(d + p); p += 4; Cell *c = kc_malloc(total * sizeof(Cell)); size_t i = 0;
        while (i < total && p + 16 <= n) { size_t run = get_u16(d + p); Cell cell = { get_u32(d + p + 2), get_u32(d + p + 6), get_u32(d + p + 10), get_u16(d + p + 14) }; p += 16; for (size_t k = 0; k < run && i < total; k++) c[i++] = cell; }
        if (i < total) { free(c); snprintf(err, 200, "truncated .kca"); return false; }
        if (a->n >= a->cap) { a->cap = a->cap ? a->cap * 2 : 16; a->frames = kc_realloc(a->frames, (size_t)a->cap * sizeof(Cell *)); a->delay = kc_realloc(a->delay, (size_t)a->cap * sizeof(int)); }
        a->frames[a->n] = c; a->delay[a->n] = delay > 0 ? delay : 83; a->n++;
    }
    return a->n > 0;
}
static void mode_dims(const char *m, int *sx, int *sy) { if (!strcmp(m, "half")) { *sx = 1; *sy = 2; } else if (!strcmp(m, "quad")) { *sx = 2; *sy = 2; } else if (!strcmp(m, "braille")) { *sx = 2; *sy = 4; } else if (!strcmp(m, "shape")) { *sx = 5; *sy = 7; } else { *sx = 1; *sy = 1; } }
static void render_frame_cells(Anim *a, const Image *im) {            /* stream: render `im` into a->live at the fixed cell size */
    int id = cv_new(a->w, a->h); Canvas *c = cv_get(id); RenderOpts r = a->ro; r.width = a->w; r.height = a->h; r.fit = "stretch"; render_image_into(im, &r, c, 0, 0);
    memcpy(a->live, c->c, (size_t)a->w * a->h * sizeof(Cell)); cv_free(id);
}
static bool stream_next(Anim *a) {
    if (!a->ff) return false;
    size_t need = (size_t)a->pw * a->ph * 3, got = 0; while (got < need) { size_t r = fread(a->raw + got, 1, need - got, a->ff); if (r == 0) break; got += r; }
    if (got < need) { return false; }
    Image im; memset(&im, 0, sizeof im); im.w = a->pw; im.h = a->ph; im.px = kc_malloc((size_t)a->pw * a->ph * 4);
    for (size_t i = 0; i < (size_t)a->pw * a->ph; i++) im.px[i] = 0xFF000000u | (uint32_t)a->raw[i * 3] << 16 | (uint32_t)a->raw[i * 3 + 1] << 8 | a->raw[i * 3 + 2];
    render_frame_cells(a, &im); free(im.px); a->produced++; a->have_frame = true; return true;
}
static bool stream_open(Anim *a, double start) {
    if (a->ff) ffmpeg_close(a->ff);
    a->ff = ffmpeg_open_frames(a->path, a->pw, a->ph, a->src_fps, start, "rgb24", false); a->start = start; return a->ff != NULL;
}
static int seq_expand(const char *pat, char ***out) {
    int cap = 16, n = 0; char **L = kc_malloc((size_t)cap * sizeof(char *)); char buf[1024];
    for (int i = 0; i < 100000; i++) { snprintf(buf, sizeof buf, pat, i); if (!as_file_exists(buf)) { if (n == 0 && i == 0) continue; if (i > 1 && n == 0) break; break; } if (n >= cap) { cap *= 2; L = kc_realloc(L, (size_t)cap * sizeof(char *)); } L[n] = kc_malloc(strlen(buf) + 1); strcpy(L[n++], buf); }
    if (n == 0) for (int i = 1; i < 100000; i++) { snprintf(buf, sizeof buf, pat, i); if (!as_file_exists(buf)) break; if (n >= cap) { cap *= 2; L = kc_realloc(L, (size_t)cap * sizeof(char *)); } L[n] = kc_malloc(strlen(buf) + 1); strcpy(L[n++], buf); }
    *out = L; return n;
}
static bool has_suffix(const char *p, const char *s) { size_t a = strlen(p), b = strlen(s); if (a < b) return false; for (size_t i = 0; i < b; i++) if (tolower((unsigned char)p[a - b + i]) != s[i]) return false; return true; }
/* Load any supported animation source. Returns anim id. */
int anim_load_file(const char *path, const Opts *o) {
    RenderOpts r; render_opts_default(&r); r.color = (T.colors > 0 || !T.open) ? 1 : 0; render_opts_parse(&r, o);
    int id = anim_new(opt_num(o, "fps", 12)); Anim *a = &ANS[id]; char err[300] = ""; int maxf = opt_int(o, "max_frames", 3000); a->loop = 1;
    bool stream = opt_bool(o, "stream", false);
    size_t n = 0; uint8_t *d = NULL; bool ok = false;
    if (strchr(path, '%') && !as_file_exists(path)) {                                        /* image sequence: "frames/%04d.png" */
        char **L; int cnt = seq_expand(path, &L); if (cnt == 0) { an_release(a); as_err("anim_load: no files match '%s'", path); }
        for (int i = 0; i < cnt && i < maxf; i++) { int iid = im_load(L[i], 0, 0); Image *im = im_get(iid); int w, h; int cid = render_image_to_canvas(im, &r, &w, &h); a = &ANS[id]; anim_add_canvas(id, cv_get(cid), 0); cv_free(cid); im_free(iid); a = &ANS[id]; }
        for (int i = 0; i < cnt; i++) free(L[i]); free(L); return id;
    }
    d = as_read_file(path, &n); if (!d) { an_release(a); as_err("anim_load: cannot open '%s'", path); }
    if (n >= 4 && !memcmp(d, "KCA1", 4)) ok = kca_load(id, d, n, err);
    else if (n >= 6 && !memcmp(d, "GIF8", 4)) {
        Image g; memset(&g, 0, sizeof g); ok = gif_decode(d, n, &g, err);
        if (ok) for (int f = 0; f < g.nframes && f < maxf; f++) { Image v = g; v.px = g.frames[f]; int w, h, cid = render_image_to_canvas(&v, &r, &w, &h); anim_add_canvas(id, cv_get(cid), g.delays_ms[f]); cv_free(cid); }
        im_release(&g);
    } else if (has_suffix(path, ".txt") || has_suffix(path, ".ans") || has_suffix(path, ".asc")) {
        bool cp = has_suffix(path, ".ans") && !opt_has(o, "utf8"); const char *s = (const char *)d, *e = s + n; int wrap = opt_int(o, "wrap", cp ? 80 : 0);
        while (s < e) { const char *ff = memchr(s, 0x0c, (size_t)(e - s)); size_t l = ff ? (size_t)(ff - s) : (size_t)(e - s); while (l && (s[l - 1] == '\n' || s[l - 1] == '\r')) l--; while (l && *s == '\n') { s++; l--; }
            if (l) { int cid = cv_from_text(s, l, cp, wrap); Canvas *c = cv_get(cid); if (a->n == 0 || (c->w == a->w && c->h == a->h)) anim_add_canvas(id, c, 0); else { /* pad to first frame size */ int pid = cv_new(a->w, a->h); cv_blit(cv_get(pid), c, 0, 0, 0, 0, 0, 0, false, false); anim_add_canvas(id, cv_get(pid), 0); cv_free(pid); } cv_free(cid); }
            if (!ff) break; s = ff + 1; if (*s == '\n') s++; }
        ok = a->n > 0; if (!ok) snprintf(err, sizeof err, "no frames found");
    } else {                                                                                 /* video (ffmpeg) or single image */
        free(d); d = NULL;
        if (!ffmpeg_available()) { an_release(a); as_err("anim_load: '%s' needs ffmpeg for video (GIF, frame sequences, .kca and .txt work without it)", path); }
        int vw, vh; double vfps, dur; if (!ffmpeg_probe(path, &vw, &vh, &vfps, &dur)) { an_release(a); as_err("anim_load: ffmpeg could not read '%s'", path); }
        Image probe; memset(&probe, 0, sizeof probe); probe.w = vw; probe.h = vh; int W, H; render_dims(&probe, &r, &W, &H); int sx, sy; mode_dims(r.mode, &sx, &sy);
        a->w = W; a->h = H; a->pw = W * sx; a->ph = H * sy; a->ro = r; a->src_fps = opt_has(o, "fps") ? opt_num(o, "fps", 12) : (vfps > 30 ? 30 : vfps); a->fps = a->src_fps; a->path = kc_malloc(strlen(path) + 1); strcpy(a->path, path);
        if (r.ramp) { a->ro_ramp = kc_malloc(strlen(r.ramp) + 1); strcpy(a->ro_ramp, r.ramp); a->ro.ramp = a->ro_ramp; }
        double start = opt_num(o, "start", 0), len = opt_num(o, "duration", 0); a->raw = kc_malloc((size_t)a->pw * a->ph * 3);
        if (stream) { a->stream = true; a->live = kc_malloc((size_t)W * H * sizeof(Cell)); for (size_t i = 0; i < (size_t)W * H; i++) { a->live[i].ch = ' '; a->live[i].fg = a->live[i].bg = 0; a->live[i].attr = 0; } if (!stream_open(a, start)) { an_release(a); as_err("anim_load: cannot start ffmpeg"); } stream_next(a); return id; }
        if (!stream_open(a, start)) { an_release(a); as_err("anim_load: cannot start ffmpeg"); }
        a->live = kc_malloc((size_t)W * H * sizeof(Cell)); int limit = maxf; if (len > 0 && (int)(len * a->src_fps) < limit) limit = (int)(len * a->src_fps);
        while (a->n < limit && stream_next(a)) { Canvas tmp = { W, H, a->live, 0, 0, W, H, true }; anim_add_canvas(id, &tmp, 0); a = &ANS[id]; }
        ffmpeg_close(a->ff); a->ff = NULL; free(a->live); a->live = NULL; free(a->raw); a->raw = NULL; free(a->path); a->path = NULL; ok = a->n > 0; if (!ok) snprintf(err, sizeof err, "ffmpeg produced no frames");
    }
    free(d);
    if (!ok) { an_release(a); as_err("anim_load: '%s': %s", path, err[0] ? err : "decode failed"); }
    return id;
}
void anim_info(int id, int *w, int *h, int *n, double *fps, int *cur, bool *playing, bool *finished, bool *stream) {
    Anim *a = an_get(id); if (!a) as_err("invalid animation %d", id);
    *w = a->w; *h = a->h; *n = a->stream ? (int)a->produced : a->n; *fps = a->fps; *cur = a->cur; *playing = a->playing; *finished = a->finished; *stream = a->stream;
}
void anim_ctl(int id, const char *op, double v) {
    Anim *a = an_get(id); if (!a) as_err("invalid animation %d", id);
    if (!strcmp(op, "play")) { a->playing = true; if (a->finished) { a->finished = false; a->cur = 0; a->t = 0; } }
    else if (!strcmp(op, "pause")) a->playing = false; else if (!strcmp(op, "stop")) { a->playing = false; a->cur = 0; a->t = 0; a->finished = false; if (a->stream) { stream_open(a, a->start); stream_next(a); } }
    else if (!strcmp(op, "speed")) a->speed = v > 0.01 ? v : 0.01; else if (!strcmp(op, "loop")) a->loop = (int)v;
    else if (!strcmp(op, "seek")) { if (a->stream) { stream_open(a, v); a->cur = 0; a->t = 0; stream_next(a); } else { a->cur = as_clampi((int)v, 0, a->n - 1); a->t = 0; a->finished = false; } }
    else as_err("anim_ctl: unknown control \"%s\"", op);
}
static void an_advance(Anim *a, double dt) {
    if (!a->playing || a->finished) return;
    a->t += dt * a->speed;
    if (a->stream) {
        double per = 1.0 / a->src_fps; int guard = 0;
        while (a->t >= per && guard++ < 8) { a->t -= per; if (!stream_next(a)) { if (a->loop) { stream_open(a, a->start); stream_next(a); } else { a->finished = true; a->playing = false; } break; } a->cur++; }
        if (a->t > per * 8) a->t = 0; return;
    }
    int guard = 0;
    while (a->n > 0 && a->t >= a->delay[a->cur] / 1000.0 && guard++ < 10000) {
        a->t -= a->delay[a->cur] / 1000.0; int nx = a->cur + a->dir;
        if (nx >= a->n || nx < 0) { if (a->loop == 1) nx = a->dir > 0 ? 0 : a->n - 1; else if (a->loop == 2) { a->dir = -a->dir; nx = as_clampi(a->cur + a->dir, 0, a->n - 1); } else { a->finished = true; a->playing = false; a->t = 0; break; } }
        a->cur = nx;
    }
}
void anims_update_frame(double dt, int64_t frame) {
    for (int i = 1; i < nans; i++) if (ANS[i].used && ANS[i].last_frame != frame) { ANS[i].last_frame = frame; an_advance(&ANS[i], dt); }
}
void anim_draw_into(int id, Canvas *dst, int x, int y, bool transparent) {
    Anim *a = an_get(id); if (!a) as_err("invalid animation %d", id); if (a->n == 0 && !a->stream) return;
    Cell *cells = a->stream ? a->live : a->frames[as_clampi(a->cur, 0, a->n - 1)]; if (!cells) return;
    Canvas tmp = { a->w, a->h, cells, 0, 0, a->w, a->h, true }; cv_blit(dst, &tmp, 0, 0, 0, 0, x, y, transparent, transparent);
}
Canvas *anim_frame_canvas(int id, int frame, Canvas *tmp) {
    Anim *a = an_get(id); if (!a || a->n == 0 || frame < 0 || frame >= a->n) return NULL; tmp->w = a->w; tmp->h = a->h; tmp->c = a->frames[frame]; tmp->cx0 = tmp->cy0 = 0; tmp->cx1 = a->w; tmp->cy1 = a->h; tmp->used = true; return tmp;
}

/* ===================================================================== effects */
typedef struct { bool used; int kind, w, h; float *a, *b; float t, acc, speed; uint32_t seed; const char *ramp; uint32_t rp[64]; int nrp; bool color, transparent; Col tint; float p1; int n; } Effect;
enum { E_PLASMA, E_FIRE, E_MATRIX, E_STARS, E_RAIN, E_SNOW, E_WAVES, E_TUNNEL, E_LIFE, E_NOISE };
static const char *EFFECT_NAMES[] = { "plasma", "fire", "matrix", "starfield", "rain", "snow", "waves", "tunnel", "life", "noise", NULL };
static Effect *EFS; static int nef, capef;
int effect_new(const char *name, int w, int h, const Opts *o) {
    int kind = -1; for (int i = 0; EFFECT_NAMES[i]; i++) if (!strcmp(EFFECT_NAMES[i], name)) kind = i;
    if (kind < 0) as_err("effect: unknown effect \"%s\" (plasma, fire, matrix, starfield, rain, snow, waves, tunnel, life, noise)", name);
    if (w < 1 || h < 1 || (int64_t)w * h > 1000000) as_err("effect: bad size %dx%d", w, h);
    int id = -1; for (int i = 1; i < nef; i++) if (!EFS[i].used) { id = i; break; }
    if (id < 0) { if (nef == 0) nef = 1; if (nef >= capef) { capef = capef ? capef * 2 : 8; EFS = kc_realloc(EFS, (size_t)capef * sizeof(Effect)); } id = nef++; }
    Effect *e = &EFS[id]; memset(e, 0, sizeof *e); e->used = true; e->kind = kind; e->w = w; e->h = h; e->speed = (float)opt_num(o, "speed", 1.0); e->seed = (uint32_t)opt_int(o, "seed", (int)as_rand_u32()); e->color = opt_bool(o, "color", true); e->tint = opt_col(o, "fg", 0);
    e->transparent = opt_bool(o, "transparent", kind == E_MATRIX || kind == E_STARS || kind == E_RAIN || kind == E_SNOW);
    const char *rs = opt_str(o, "ramp", NULL); const char *pr = rs ? ramp_preset(rs) : NULL; if (!rs) rs = kind == E_FIRE ? " .:^*xsS#$" : kind == E_LIFE ? " #" : " .:-=+*#%@"; if (pr) rs = pr;
    const char *q = rs; while (*q && e->nrp < 64) e->rp[e->nrp++] = utf8_next(&q); if (e->nrp < 2) as_err("effect: ramp needs 2+ characters");
    size_t cells = (size_t)w * h;
    if (kind == E_FIRE || kind == E_LIFE) { e->a = kc_malloc(cells * sizeof(float)); e->b = kc_malloc(cells * sizeof(float)); memset(e->a, 0, cells * sizeof(float)); memset(e->b, 0, cells * sizeof(float)); }
    if (kind == E_LIFE) { float dens = (float)opt_num(o, "density", 0.3); for (size_t i = 0; i < cells; i++) e->a[i] = as_rand() < dens ? 1.0f : 0.0f; e->p1 = (float)opt_num(o, "rate", 10); }
    if (kind == E_MATRIX) { e->a = kc_malloc((size_t)w * 3 * sizeof(float)); for (int x = 0; x < w; x++) { e->a[x * 3] = -as_rand() * h; e->a[x * 3 + 1] = 6 + as_rand() * 14; e->a[x * 3 + 2] = 5 + as_rand() * (float)h * 0.7f; } }
    if (kind == E_STARS) { e->n = opt_int(o, "count", w * h / 12 + 10); e->a = kc_malloc((size_t)e->n * 3 * sizeof(float)); for (int i = 0; i < e->n; i++) { e->a[i * 3] = as_rand() * 2 - 1; e->a[i * 3 + 1] = as_rand() * 2 - 1; e->a[i * 3 + 2] = as_rand() * 0.98f + 0.02f; } }
    if (kind == E_RAIN || kind == E_SNOW) { e->n = opt_int(o, "count", w * h / (kind == E_RAIN ? 10 : 14) + 8); e->a = kc_malloc((size_t)e->n * 4 * sizeof(float)); for (int i = 0; i < e->n; i++) { e->a[i * 4] = as_rand() * (float)w; e->a[i * 4 + 1] = as_rand() * (float)h; e->a[i * 4 + 2] = (kind == E_RAIN ? 14 : 3) * (0.6f + as_rand() * 0.8f); e->a[i * 4 + 3] = as_rand() * 6.28f; } }
    return id;
}
void effect_free(int id) { if (id < 1 || id >= nef || !EFS[id].used) return; free(EFS[id].a); free(EFS[id].b); EFS[id].used = false; }
bool effect_valid(int id) { return id >= 1 && id < nef && EFS[id].used; }
void effects_free_all(void) { for (int i = 1; i < nef; i++) if (EFS[i].used) effect_free(i); free(EFS); EFS = NULL; nef = capef = 0; }
void effect_step(int id, double dtd) {
    if (id < 1 || id >= nef || !EFS[id].used) as_err("invalid effect %d", id);
    Effect *e = &EFS[id]; float dt = (float)dtd * e->speed; e->t += dt; int w = e->w, h = e->h;
    switch (e->kind) {
        case E_FIRE: {
            e->acc += dt; float step = 1.0f / 24.0f; int it = 0;
            while (e->acc >= step && it++ < 4) { e->acc -= step;
                for (int x = 0; x < w; x++) e->a[(size_t)(h - 1) * w + x] = as_rand() > 0.35f ? 1.0f : 0.55f + as_rand() * 0.3f;
                for (int y = 0; y < h - 1; y++) for (int x = 0; x < w; x++) { int xl = (x + (int)(as_rand() * 3) - 1 + w) % w; float v = (e->a[(size_t)(y + 1) * w + x] + e->a[(size_t)(y + 1) * w + xl] + e->a[(size_t)(y + 1) * w + (x + 1) % w] + (y + 2 < h ? e->a[(size_t)(y + 2) * w + x] : 0)) / 4.0f; v -= 1.0f / (float)h * 1.15f; e->b[(size_t)y * w + x] = v < 0 ? 0 : v; }
                memcpy(e->a, e->b, (size_t)(h - 1) * w * sizeof(float)); }
            break; }
        case E_MATRIX: for (int x = 0; x < w; x++) { e->a[x * 3] += e->a[x * 3 + 1] * dt; if (e->a[x * 3] - e->a[x * 3 + 2] > (float)h) { e->a[x * 3] = -as_rand() * 8; e->a[x * 3 + 1] = 6 + as_rand() * 14; e->a[x * 3 + 2] = 5 + as_rand() * (float)h * 0.7f; } } break;
        case E_STARS: for (int i = 0; i < e->n; i++) { e->a[i * 3 + 2] -= dt * 0.35f; if (e->a[i * 3 + 2] <= 0.02f) { e->a[i * 3] = as_rand() * 2 - 1; e->a[i * 3 + 1] = as_rand() * 2 - 1; e->a[i * 3 + 2] = 1; } } break;
        case E_RAIN: case E_SNOW: for (int i = 0; i < e->n; i++) { float *d = &e->a[i * 4]; d[1] += d[2] * dt; if (e->kind == E_SNOW) d[0] += sinf(e->t * 1.3f + d[3]) * dt * 2.2f; else d[0] -= dt * 1.5f; if (d[1] >= (float)h) { d[1] = -as_rand() * 3; d[0] = as_rand() * (float)w; } if (d[0] < 0) d[0] += (float)w; if (d[0] >= (float)w) d[0] -= (float)w; } break;
        case E_LIFE: { e->acc += dt; float step = 1.0f / (e->p1 > 0 ? e->p1 : 10); int it = 0; while (e->acc >= step && it++ < 6) { e->acc -= step; for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { int nb = 0; for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) if (dx || dy) nb += e->a[(size_t)((y + dy + h) % h) * w + (x + dx + w) % w] > 0.5f; float c = e->a[(size_t)y * w + x]; e->b[(size_t)y * w + x] = (c > 0.5f ? (nb == 2 || nb == 3) : nb == 3) ? 1.0f : (c > 0.5f ? 0.0f : c * 0.0f); } memcpy(e->a, e->b, (size_t)w * h * sizeof(float)); } break; }
        default: break;
    }
}
static Col heat_col(float v) { if (v < 0.33f) return col_rgb((int)(v * 3 * 200), 0, 0); if (v < 0.66f) return col_rgb(200 + (int)((v - 0.33f) * 3 * 55), (int)((v - 0.33f) * 3 * 160), 0); return col_rgb(255, 160 + (int)((v - 0.66f) * 3 * 95), (int)((v - 0.66f) * 3 * 200)); }
void effect_draw(int id, Canvas *c, int x0, int y0, Col tint) {
    if (id < 1 || id >= nef || !EFS[id].used) as_err("invalid effect %d", id);
    Effect *e = &EFS[id]; int w = e->w, h = e->h; float t = e->t; Col base = tint ? tint : e->tint;
    #define EMIT(x, y, ch, col) do { uint32_t c_ = (ch); if (c_ != ' ' || !e->transparent) cv_put(c, x0 + (x), y0 + (y), c_, e->color ? (col) : 0, 0, 0); } while (0)
    #define RAMPCH(v) e->rp[as_clampi((int)((v) * (float)(e->nrp - 1) + 0.5f), 0, e->nrp - 1)]
    switch (e->kind) {
        case E_PLASMA: for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { float fx = (float)x / 6.0f, fy = (float)y / 3.0f; float v = sinf(fx + t) + sinf(fy + t * 1.3f) + sinf((fx + fy + t) * 0.7f) + sinf(sqrtf((fx - (float)w / 12) * (fx - (float)w / 12) + (fy - (float)h / 6) * (fy - (float)h / 6)) + t); v = (v + 4) / 8; EMIT(x, y, RAMPCH(v), base ? col_scale(base, 0.4f + v * 0.6f) : col_hsv(v + t * 0.05f, 0.8f, 0.5f + v * 0.5f)); } break;
        case E_FIRE: for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { float v = e->a[(size_t)y * w + x]; EMIT(x, y, RAMPCH(v), base ? col_scale(base, v) : heat_col(v)); } break;
        case E_MATRIX: for (int x = 0; x < w; x++) { float head = e->a[x * 3], len = e->a[x * 3 + 2]; for (int y = 0; y < h; y++) { float d = head - (float)y; if (d < 0 || d > len) continue; float k = 1.0f - d / len; uint32_t hsh = as_hash2(x, y + (int)(t * 6), e->seed); uint32_t ch = d < 1.0f ? (0x30 + hsh % 10) : (hsh & 1 ? 0x30A0 + (hsh >> 1) % 90 : 0x21 + (hsh >> 1) % 90); if (cp_width(ch) == 2) ch = 0x30 + hsh % 10; Col col = d < 1.0f ? col_rgb(220, 255, 220) : (base ? col_scale(base, k) : col_rgb(0, (int)(70 + 185 * k), (int)(30 * k))); EMIT(x, y, ch, col); } } break;
        case E_STARS: for (int i = 0; i < e->n; i++) { float z = e->a[i * 3 + 2], sx = e->a[i * 3] / z, sy = e->a[i * 3 + 1] / z; int px = (int)((sx * 0.5f + 0.5f) * (float)w), py = (int)((sy * 0.5f + 0.5f) * (float)h); if (px < 0 || px >= w || py < 0 || py >= h) continue; float k = 1 - z; uint32_t ch = k > 0.8f ? '*' : k > 0.5f ? '+' : k > 0.25f ? '.' : 0xB7; int g = (int)(90 + 165 * k); EMIT(px, py, ch, base ? col_scale(base, 0.4f + k * 0.6f) : col_rgb(g, g, g)); } break;
        case E_RAIN: case E_SNOW: for (int i = 0; i < e->n; i++) { float *d = &e->a[i * 4]; int px = (int)d[0], py = (int)d[1]; if (px < 0 || px >= w || py < 0 || py >= h) continue; uint32_t ch = e->kind == E_RAIN ? (d[2] > 14 ? '|' : '\'') : (d[2] > 3 ? '*' : '.'); EMIT(px, py, ch, base ? base : (e->kind == E_RAIN ? col_rgb(110, 150, 230) : col_rgb(235, 240, 255))); } break;
        case E_WAVES: for (int x = 0; x < w; x++) for (int y = 0; y < h; y++) { float yy = (float)y / (float)h; float wv = 0.5f + 0.18f * sinf((float)x * 0.18f + t * 1.6f) + 0.08f * sinf((float)x * 0.43f - t * 2.3f); float d = yy - wv; uint32_t ch = ' '; float k = 0; if (fabsf(d) < 0.5f / (float)h + 0.02f) { ch = '~'; k = 1; } else if (d > 0) { ch = d < 0.25f ? '\'' : ' '; k = 0.5f; if (d < 0.12f) ch = 0x2248; } if (ch == 0x2248) ch = '='; EMIT(x, y, ch, base ? col_scale(base, 0.5f + k * 0.5f) : col_rgb(40, (int)(110 + 100 * k), (int)(190 + 60 * k))); } break;
        case E_TUNNEL: for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { float dx = ((float)x - (float)w / 2) / ((float)w / 2), dy = ((float)y - (float)h / 2) / ((float)h / 2) * 0.9f, r = sqrtf(dx * dx + dy * dy) + 0.0001f, a = atan2f(dy, dx); float u = 0.35f / r + t * 0.8f, v = a / 3.14159f * 4 + t * 0.3f; float k = (sinf(u * 6.28f) * sinf(v * 3.14f) + 1) / 2 * as_clampf(r * 1.8f, 0, 1); EMIT(x, y, RAMPCH(k), base ? col_scale(base, 0.3f + k * 0.7f) : col_hsv(u * 0.1f + 0.55f, 0.6f, 0.3f + k * 0.7f)); } break;
        case E_LIFE: for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { float v = e->a[(size_t)y * w + x]; if (v > 0.5f) EMIT(x, y, e->rp[e->nrp - 1], base ? base : col_rgb(120, 230, 150)); else if (!e->transparent) EMIT(x, y, ' ', 0); } break;
        case E_NOISE: for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { float v = as_fbm((float)x * 0.08f + t * 0.4f, (float)y * 0.16f - t * 0.2f, e->seed, 4); v = as_clampf((v - 0.25f) * 1.8f, 0, 1); EMIT(x, y, RAMPCH(v), base ? col_scale(base, 0.3f + v * 0.7f) : col_mix(col_rgb(30, 40, 90), col_rgb(240, 245, 255), v)); } break;
    }
    #undef EMIT
    #undef RAMPCH
}

/* ================================================================== transitions */
void transition_draw(const char *kind, Canvas *a, Canvas *b, Canvas *dst, float t, int x0, int y0) {
    t = as_clampf(t, 0, 1); int w = a ? a->w : b->w, h = a ? a->h : b->h; if (b && b->w > w) w = b->w; if (b && b->h > h) h = b->h;
    bool slide_l = !strcmp(kind, "slide_left"), slide_r = !strcmp(kind, "slide_right");
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        bool from_b; int sx = x, sxb = x;
        if (!strcmp(kind, "fade") || !strcmp(kind, "dissolve")) from_b = (as_hash2(x, y, 7) & 1023) < (uint32_t)(t * 1024);
        else if (!strcmp(kind, "wipe_right")) from_b = (float)x < t * (float)w; else if (!strcmp(kind, "wipe_left")) from_b = (float)x >= (1 - t) * (float)w;
        else if (!strcmp(kind, "wipe_down")) from_b = (float)y < t * (float)h; else if (!strcmp(kind, "wipe_up")) from_b = (float)y >= (1 - t) * (float)h;
        else if (!strcmp(kind, "iris")) { float dx = ((float)x - (float)w / 2) / 2.0f, dy = (float)y - (float)h / 2, rmax = hypotf((float)w / 4, (float)h / 2); from_b = hypotf(dx, dy) < t * rmax; }
        else if (!strcmp(kind, "curtain")) from_b = fabsf((float)x - (float)w / 2) < t * (float)w / 2;
        else if (!strcmp(kind, "blinds")) from_b = (float)(y % 4) < t * 4.0f;
        else if (slide_l) { int off = (int)(t * (float)w); if (x + off < w) { from_b = false; sx = x + off; } else { from_b = true; sxb = x + off - w; } }
        else if (slide_r) { int off = (int)(t * (float)w); if (x - off >= 0) { from_b = false; sx = x - off; } else { from_b = true; sxb = x - off + w; } }
        else as_err("transition: unknown kind \"%s\" (fade, dissolve, wipe_left, wipe_right, wipe_up, wipe_down, iris, curtain, blinds, slide_left, slide_right)", kind);
        Canvas *src = from_b ? b : a; int ux = from_b ? sxb : sx; if (!src) continue; Cell *q = cv_at(src, ux, y); if (!q || q->ch == CH_CONT) continue;
        cv_put(dst, x0 + x, y0 + y, q->ch, q->fg, q->bg, q->attr);
        if (q->bg == 0) { Cell *d2 = cv_at(dst, x0 + x, y0 + y); if (d2) d2->bg = 0; }
    }
}
