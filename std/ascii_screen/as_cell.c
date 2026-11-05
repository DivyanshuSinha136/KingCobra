/* std.ascii_screen — cell canvases, text, shapes, boxes, hi-res (half/quad/braille) pixel layers, export. */
#include "as_internal.h"

int S_target = 0;
Style S_style = { 0, 0, 0 };

/* ================================================================ canvases */
static Canvas **CVS; static int cv_cap;

Canvas *cv_get(int id) {
    if (id == 0) return S_screen;
    if (id < 1 || id >= cv_cap || !CVS[id] || !CVS[id]->used) return NULL;
    return CVS[id];
}
Canvas *cv_must(int id, const char *who) {
    Canvas *c = cv_get(id);
    if (!c) {
        if (id == 0) { term_ensure(); c = S_screen; if (c) return c; }
        as_err("%s: invalid canvas %d", who, id);
    }
    return c;
}
Canvas *cv_target(void) {
    if (S_target == 0) { if (!S_screen) term_ensure(); return S_screen; }
    Canvas *c = cv_get(S_target);
    if (!c) { S_target = 0; if (!S_screen) term_ensure(); return S_screen; }
    return c;
}
static void cv_alloc_cells(Canvas *c, int w, int h) {
    c->w = w; c->h = h; c->c = kc_realloc(c->c, (size_t)w * h * sizeof(Cell));
    c->cx0 = 0; c->cy0 = 0; c->cx1 = w; c->cy1 = h;
}
void cv_clear(Canvas *c, uint32_t ch, Col fg, Col bg, uint16_t attr) {
    Cell e = { ch ? ch : ' ', fg, bg, attr };
    for (size_t i = 0; i < (size_t)c->w * c->h; i++) c->c[i] = e;
}
int cv_new(int w, int h) {
    if (w < 1) w = 1; if (h < 1) h = 1;
    if ((int64_t)w * h > 4000000) as_err("canvas: %dx%d cells is too large", w, h);
    int id = -1;
    for (int i = 1; i < cv_cap; i++) if (!CVS[i] || !CVS[i]->used) { id = i; break; }
    if (id < 0) {
        int nc = cv_cap ? cv_cap * 2 : 16; CVS = kc_realloc(CVS, (size_t)nc * sizeof *CVS);
        for (int i = cv_cap; i < nc; i++) CVS[i] = NULL;
        id = cv_cap ? cv_cap : 1; cv_cap = nc;
    }
    if (!CVS[id]) { CVS[id] = kc_malloc(sizeof(Canvas)); memset(CVS[id], 0, sizeof(Canvas)); }
    Canvas *c = CVS[id]; c->used = true; c->c = NULL; cv_alloc_cells(c, w, h); cv_clear(c, ' ', 0, 0, 0);
    return id;
}
void cv_free(int id) {
    if (id < 1 || id >= cv_cap || !CVS[id] || !CVS[id]->used) return;
    free(CVS[id]->c); CVS[id]->c = NULL; CVS[id]->used = false;
    if (S_target == id) S_target = 0;
}
void cv_resize(Canvas *c, int w, int h) {
    if (w < 1) w = 1; if (h < 1) h = 1;
    if (w == c->w && h == c->h) return;
    Cell *n = kc_malloc((size_t)w * h * sizeof(Cell)); Cell e = { ' ', 0, 0, 0 };
    for (size_t i = 0; i < (size_t)w * h; i++) n[i] = e;
    for (int y = 0; y < h && y < c->h; y++) for (int x = 0; x < w && x < c->w; x++) n[(size_t)y * w + x] = c->c[(size_t)y * c->w + x];
    for (int y = 0; y < h && y < c->h; y++) if (c->w > w || true) { Cell *r = &n[(size_t)y * w + (w - 1)]; if (cp_width(r->ch) == 2) r->ch = ' '; }
    free(c->c); c->c = n; c->w = w; c->h = h; c->cx0 = 0; c->cy0 = 0; c->cx1 = w; c->cy1 = h;
}
Cell *cv_at(Canvas *c, int x, int y) { return (x < 0 || y < 0 || x >= c->w || y >= c->h) ? NULL : &c->c[(size_t)y * c->w + x]; }
void cv_set_clip(Canvas *c, int x, int y, int w, int h) {
    c->cx0 = as_clampi(x, 0, c->w); c->cy0 = as_clampi(y, 0, c->h); c->cx1 = as_clampi(x + w, c->cx0, c->w); c->cy1 = as_clampi(y + h, c->cy0, c->h);
}
void cv_reset_clip(Canvas *c) { c->cx0 = 0; c->cy0 = 0; c->cx1 = c->w; c->cy1 = c->h; }

static void cv_put_impl(Canvas *c, int x, int y, uint32_t ch, Col fg, Col bg, uint16_t attr, bool keep_bg) {
    if (x < c->cx0 || x >= c->cx1 || y < c->cy0 || y >= c->cy1) return;
    if (ch == 0) ch = ' ';
    int w = cp_width(ch);
    if (w == 0) return;
    if (w == 2 && x + 1 >= c->cx1) { ch = ' '; w = 1; }
    Cell *p = &c->c[(size_t)y * c->w + x];
    if (p->ch == CH_CONT && x > 0) p[-1].ch = ' ';                                   /* overwrote the tail of a wide char */
    else if (cp_width(p->ch) == 2 && x + 1 < c->w) p[1].ch = ' ';                    /* overwrote the head */
    if (w == 2) { Cell *r = p + 1; if (cp_width(r->ch) == 2 && x + 2 < c->w) r[1].ch = ' '; }
    p->ch = ch; p->fg = fg; if (!keep_bg || bg) p->bg = bg; p->attr = attr;
    if (w == 2) { Cell *r = p + 1; r->ch = CH_CONT; r->fg = fg; r->bg = p->bg; r->attr = attr; }
}
void cv_put(Canvas *c, int x, int y, uint32_t ch, Col fg, Col bg, uint16_t attr) { cv_put_impl(c, x, y, ch, fg, bg, attr, true); }

int cv_text(Canvas *c, int x, int y, const char *s, size_t len, Col fg, Col bg, uint16_t attr) {
    const char *p = s, *end = s + len; int adv = 0;
    while (p < end) {
        uint32_t cp = utf8_next(&p);
        if (cp == '\t') { int n = 4 - (adv % 4); for (int i = 0; i < n; i++) cv_put(c, x + adv + i, y, ' ', fg, bg, attr); adv += n; continue; }
        if (cp < 32) continue;
        int w = cp_width(cp); if (w == 0) continue;
        cv_put(c, x + adv, y, cp, fg, bg, attr); adv += w;
    }
    return adv;
}
void cv_fill(Canvas *c, int x, int y, int w, int h, uint32_t ch, Col fg, Col bg, uint16_t attr) {
    for (int j = y; j < y + h; j++) for (int i = x; i < x + w; i++) cv_put_impl(c, i, j, ch, fg, bg, attr, false);
}
void cv_blit(Canvas *dst, Canvas *src, int sx, int sy, int sw, int sh, int dx, int dy, bool skip_space, bool skip_empty_bg) {
    if (sw <= 0) sw = src->w - sx; if (sh <= 0) sh = src->h - sy;
    for (int j = 0; j < sh; j++) for (int i = 0; i < sw; i++) {
        Cell *s = cv_at(src, sx + i, sy + j); if (!s || s->ch == CH_CONT) continue;
        if (skip_space && s->ch == ' ' && (!s->bg || skip_empty_bg)) continue;
        cv_put_impl(dst, dx + i, dy + j, s->ch, s->fg, s->bg, s->attr, skip_space);
    }
}

/* ================================================================ shapes */
static const struct { const char *name; uint32_t c[6]; } BOX[] = {
    { "single", { 0x250C, 0x2510, 0x2514, 0x2518, 0x2500, 0x2502 } }, { "double", { 0x2554, 0x2557, 0x255A, 0x255D, 0x2550, 0x2551 } },
    { "round", { 0x256D, 0x256E, 0x2570, 0x256F, 0x2500, 0x2502 } },  { "bold", { 0x250F, 0x2513, 0x2517, 0x251B, 0x2501, 0x2503 } },
    { "ascii", { '+', '+', '+', '+', '-', '|' } },                    { "block", { 0x2588, 0x2588, 0x2588, 0x2588, 0x2588, 0x2588 } },
    { "dashed", { 0x250C, 0x2510, 0x2514, 0x2518, 0x254C, 0x254E } }, { "dots", { '.', '.', '\'', '\'', '.', ':' } },
    { "thick_half", { 0x259B, 0x259C, 0x2599, 0x259F, 0x2580, 0x258C } },
};
static const uint32_t *box_cps(const char *style) {
    for (size_t i = 0; i < sizeof BOX / sizeof BOX[0]; i++) if (!strcmp(BOX[i].name, style)) return BOX[i].c;
    return NULL;
}
void draw_box(Canvas *c, int x, int y, int w, int h, const char *style, Col fg, Col bg, uint16_t attr, bool fill, const char *title, Col title_fg) {
    if (w < 1 || h < 1) return;
    const uint32_t *b = box_cps(style ? style : "single");
    if (!b) as_err("box: unknown style \"%s\" (single, double, round, bold, ascii, block, dashed, dots, thick_half or \"none\")", style);
    if (fill) cv_fill(c, x, y, w, h, ' ', fg, bg, attr);
    if (w == 1 || h == 1) { cv_fill(c, x, y, w, h, w == 1 ? b[5] : b[4], fg, bg, attr); return; }
    for (int i = 1; i < w - 1; i++) { cv_put(c, x + i, y, b[4], fg, bg, attr); cv_put(c, x + i, y + h - 1, b[4], fg, bg, attr); }
    for (int j = 1; j < h - 1; j++) { cv_put(c, x, y + j, b[5], fg, bg, attr); cv_put(c, x + w - 1, y + j, b[5], fg, bg, attr); }
    cv_put(c, x, y, b[0], fg, bg, attr); cv_put(c, x + w - 1, y, b[1], fg, bg, attr); cv_put(c, x, y + h - 1, b[2], fg, bg, attr); cv_put(c, x + w - 1, y + h - 1, b[3], fg, bg, attr);
    if (title && *title && w > 4) {
        char buf[256]; snprintf(buf, sizeof buf, " %s ", title);
        int tw = str_width(buf), maxw = w - 4; const char *p = buf; int used = 0;
        if (tw > maxw) { /* truncate */ while (*p && used < maxw - 1) { const char *q = p; uint32_t cp = utf8_next(&q); int cw = cp_width(cp); if (used + cw > maxw - 1) break; used += cw; p = q; } buf[p - buf] = 0; strcat(buf, "\xE2\x80\xA6"); }
        cv_text(c, x + 2, y, buf, strlen(buf), title_fg ? title_fg : fg, bg, attr);
    }
}
static uint32_t auto_line_char(int dx, int dy) {
    int ax = abs(dx), ay = abs(dy);
    if (ay * 3 < ax) return '-'; if (ax * 3 < ay) return '|';
    return ((dx > 0) == (dy > 0)) ? '\\' : '/';
}
void draw_line(Canvas *c, int x0, int y0, int x1, int y1, uint32_t ch, Col fg, Col bg, uint16_t attr) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1, err = dx + dy;
    uint32_t g = ch ? ch : auto_line_char(x1 - x0, y1 - y0);
    for (int guard = 0; guard < 100000; guard++) {
        cv_put(c, x0, y0, g, fg, bg, attr);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}
void draw_ellipse(Canvas *c, int cx, int cy, int rx, int ry, uint32_t ch, Col fg, Col bg, uint16_t attr, bool fill) {
    if (rx < 0 || ry < 0) return;
    if (rx == 0 || ry == 0) { draw_line(c, cx - rx, cy - ry, cx + rx, cy + ry, ch ? ch : '*', fg, bg, attr); return; }
    uint32_t g = ch ? ch : '*';
    if (fill) {
        for (int y = -ry; y <= ry; y++) {
            double t = 1.0 - (double)(y * y) / ((double)ry * ry); if (t < 0) t = 0;
            int hw = (int)floor(rx * sqrt(t) + 0.5);
            for (int x = -hw; x <= hw; x++) cv_put(c, cx + x, cy + y, g, fg, bg, attr);
        }
        return;
    }
    /* parametric outline, dense enough that no gaps appear */
    int steps = (rx + ry) * 8 + 16;
    for (int i = 0; i < steps; i++) {
        double a = 2.0 * M_PI * i / steps;
        cv_put(c, cx + (int)lround(rx * cos(a)), cy + (int)lround(ry * sin(a)), g, fg, bg, attr);
    }
}
void draw_triangle(Canvas *c, int x0, int y0, int x1, int y1, int x2, int y2, uint32_t ch, Col fg, Col bg, uint16_t attr) {
    int minx = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2), maxx = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
    int miny = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2), maxy = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);
    minx = minx < 0 ? 0 : minx; miny = miny < 0 ? 0 : miny; maxx = maxx >= c->w ? c->w - 1 : maxx; maxy = maxy >= c->h ? c->h - 1 : maxy;
    long area = (long)(x1 - x0) * (y2 - y0) - (long)(x2 - x0) * (y1 - y0);
    if (area == 0) { draw_line(c, x0, y0, x1, y1, ch ? ch : '#', fg, bg, attr); draw_line(c, x1, y1, x2, y2, ch ? ch : '#', fg, bg, attr); return; }
    for (int y = miny; y <= maxy; y++) for (int x = minx; x <= maxx; x++) {
        long w0 = (long)(x1 - x) * (y2 - y) - (long)(x2 - x) * (y1 - y), w1 = (long)(x2 - x) * (y0 - y) - (long)(x0 - x) * (y2 - y), w2 = (long)(x0 - x) * (y1 - y) - (long)(x1 - x) * (y0 - y);
        if (area < 0) { w0 = -w0; w1 = -w1; w2 = -w2; }
        if (w0 >= 0 && w1 >= 0 && w2 >= 0) cv_put(c, x, y, ch ? ch : '#', fg, bg, attr);
    }
}
void draw_flood(Canvas *c, int x, int y, uint32_t ch, Col fg, Col bg, uint16_t attr) {
    Cell *seed = cv_at(c, x, y); if (!seed) return;
    Cell target = *seed;
    if (target.ch == (ch ? ch : ' ') && target.fg == fg && (target.bg == bg || !bg)) return;
    size_t cap = 256, sp = 0; int *st = kc_malloc(cap * 2 * sizeof(int));
    st[0] = x; st[1] = y; sp = 1;
    while (sp) {
        sp--; int px = st[sp * 2], py = st[sp * 2 + 1];
        Cell *q = cv_at(c, px, py); if (!q || q->ch != target.ch || q->fg != target.fg || q->bg != target.bg) continue;
        cv_put_impl(c, px, py, ch ? ch : ' ', fg, bg, attr, true);
        if (q->ch == target.ch && q->fg == target.fg && q->bg == target.bg) continue;   /* put was clipped: stop to avoid looping */
        if (sp + 4 >= cap) { cap *= 2; st = kc_realloc(st, cap * 2 * sizeof(int)); }
        st[sp * 2] = px + 1; st[sp * 2 + 1] = py; sp++; st[sp * 2] = px - 1; st[sp * 2 + 1] = py; sp++;
        st[sp * 2] = px; st[sp * 2 + 1] = py + 1; sp++; st[sp * 2] = px; st[sp * 2 + 1] = py - 1; sp++;
    }
    free(st);
}
static void wrap_emit(char ***L, int *n, int *cap, const char *a, const char *b) {
    size_t ll = (size_t)(b - a); while (ll && a[ll - 1] == ' ') ll--;
    if (*n >= *cap) { *cap *= 2; *L = kc_realloc(*L, (size_t)*cap * sizeof **L); }
    char *ln = kc_malloc(ll + 1); memcpy(ln, a, ll); ln[ll] = 0; (*L)[(*n)++] = ln;
}
int cv_wrap_lines(const char *s, int width, char ***lines_out) {
    if (width < 1) width = 1;
    int cap = 8, n = 0; char **L = kc_malloc((size_t)cap * sizeof *L);
    const char *p = s;
    while (1) {
        const char *e = strchr(p, '\n'), *pend = e ? e : p + strlen(p);
        const char *ls = p, *q = p, *sp = NULL; int lw = 0;
        while (q < pend) {
            const char *cs = q; uint32_t cp = utf8_next(&q); int cw = cp_width(cp);
            if (cp == ' ') sp = cs;
            if (lw + cw > width) {
                if (sp && sp > ls) { wrap_emit(&L, &n, &cap, ls, sp); q = sp + 1; ls = q; }
                else if (cs == ls) { wrap_emit(&L, &n, &cap, ls, q); ls = q; }
                else { wrap_emit(&L, &n, &cap, ls, cs); q = cs; ls = cs; }
                lw = 0; sp = NULL; continue;
            }
            lw += cw;
        }
        wrap_emit(&L, &n, &cap, ls, pend);
        if (!e) break;
        p = e + 1;
    }
    *lines_out = L; return n;
}

/* ============================================================ hi-res layers */
static Hires **HRS; static int hr_cap;
bool hr_mode_parse(const char *s, HrMode *m) {
    if (!strcmp(s, "half")) *m = HR_HALF; else if (!strcmp(s, "quad")) *m = HR_QUAD; else if (!strcmp(s, "braille")) *m = HR_BRAILLE; else return false;
    return true;
}
Hires *hr_get(int id) { return (id < 1 || id >= hr_cap || !HRS[id] || !HRS[id]->used) ? NULL : HRS[id]; }
int hr_new(int cw, int ch, HrMode mode) {
    if (cw < 1) cw = 1; if (ch < 1) ch = 1;
    if ((int64_t)cw * ch > 2000000) as_err("hires: %dx%d cells is too large", cw, ch);
    int id = -1;
    for (int i = 1; i < hr_cap; i++) if (!HRS[i] || !HRS[i]->used) { id = i; break; }
    if (id < 0) { int nc = hr_cap ? hr_cap * 2 : 8; HRS = kc_realloc(HRS, (size_t)nc * sizeof *HRS); for (int i = hr_cap; i < nc; i++) HRS[i] = NULL; id = hr_cap ? hr_cap : 1; hr_cap = nc; }
    if (!HRS[id]) { HRS[id] = kc_malloc(sizeof(Hires)); memset(HRS[id], 0, sizeof(Hires)); }
    Hires *h = HRS[id]; free(h->px);
    h->used = true; h->cw = cw; h->ch = ch; h->mode = mode;
    h->w = cw * (mode == HR_HALF ? 1 : 2); h->h = ch * (mode == HR_HALF ? 2 : mode == HR_QUAD ? 2 : 4);
    h->px = kc_malloc((size_t)h->w * h->h * sizeof(Col)); memset(h->px, 0, (size_t)h->w * h->h * sizeof(Col));
    return id;
}
void hr_free(int id) { Hires *h = hr_get(id); if (!h) return; free(h->px); h->px = NULL; h->used = false; }
static Col avg_cols(const Col *c, int n) {
    int r = 0, g = 0, b = 0, k = 0;
    for (int i = 0; i < n; i++) if (c[i] & COL_SET) { r += COL_R(c[i]); g += COL_G(c[i]); b += COL_B(c[i]); k++; }
    return k ? col_rgb(r / k, g / k, b / k) : 0;
}
static const uint32_t QUAD_CH[16] = { ' ', 0x2598, 0x259D, 0x2580, 0x2596, 0x258C, 0x259E, 0x259B, 0x2597, 0x259A, 0x2590, 0x259C, 0x2584, 0x2599, 0x259F, 0x2588 };
void hr_to_cells(const Col *px, int pw, int ph, HrMode mode, Canvas *c, int x, int y, Col bg, uint16_t attr) {
    int sx = mode == HR_HALF ? 1 : 2, sy = mode == HR_BRAILLE ? 4 : 2, cw = pw / sx, chh = ph / sy;
    for (int cy = 0; cy < chh; cy++) for (int cx = 0; cx < cw; cx++) {
        Col q[8]; int n = sx * sy; bool any = false;
        for (int j = 0; j < sy; j++) for (int i = 0; i < sx; i++) { q[j * sx + i] = px[(size_t)(cy * sy + j) * pw + cx * sx + i]; if (q[j * sx + i] & COL_SET) any = true; }
        if (!any) continue;
        if (mode == HR_HALF) {
            Col t = q[0], b = q[1];
            if ((t & COL_SET) && (b & COL_SET)) { if (t == b) cv_put_impl(c, x + cx, y + cy, 0x2588, t, bg, attr, true); else cv_put_impl(c, x + cx, y + cy, 0x2580, t, b, attr, false); }
            else if (t & COL_SET) cv_put_impl(c, x + cx, y + cy, 0x2580, t, bg, attr, true);
            else cv_put_impl(c, x + cx, y + cy, 0x2584, b, bg, attr, true);
        } else if (mode == HR_QUAD) {
            int bits = 0, on = 0; for (int i = 0; i < 4; i++) if (q[i] & COL_SET) { bits |= 1 << i; on++; }
            if (on == 4) {                                    /* all lit: split by luminance into two colours for extra detail */
                float mean = 0; for (int i = 0; i < 4; i++) mean += col_luma(q[i]); mean /= 4;
                int hb = 0; Col hi[4], lo[4]; int nh = 0, nl = 0;
                for (int i = 0; i < 4; i++) { if (col_luma(q[i]) > mean + 0.02f) { hb |= 1 << i; hi[nh++] = q[i]; } else lo[nl++] = q[i]; }
                if (!nh || !nl) { cv_put_impl(c, x + cx, y + cy, 0x2588, avg_cols(q, 4), bg, attr, true); }
                else cv_put_impl(c, x + cx, y + cy, QUAD_CH[hb], avg_cols(hi, nh), avg_cols(lo, nl), attr, false);
            } else { Col on_c[4]; int k = 0; for (int i = 0; i < 4; i++) if (q[i] & COL_SET) on_c[k++] = q[i]; cv_put_impl(c, x + cx, y + cy, QUAD_CH[bits], avg_cols(on_c, k), bg, attr, true); }
        } else {
            static const int BR[4][2] = { { 0x01, 0x08 }, { 0x02, 0x10 }, { 0x04, 0x20 }, { 0x40, 0x80 } };
            int bits = 0; Col on_c[8]; int k = 0;
            for (int j = 0; j < 4; j++) for (int i = 0; i < 2; i++) if (q[j * 2 + i] & COL_SET) { bits |= BR[j][i]; on_c[k++] = q[j * 2 + i]; }
            cv_put_impl(c, x + cx, y + cy, 0x2800u + (uint32_t)bits, avg_cols(on_c, k), bg, attr, true);
        }
        (void)n;
    }
}
void hr_draw(Hires *h, Canvas *c, int x, int y, Col bg, uint16_t attr) { hr_to_cells(h->px, h->w, h->h, h->mode, c, x, y, bg, attr); }

/* ================================================================ export */
char *cv_to_text(Canvas *c, int x0, int y0, int w, int h, int cmode, bool trim, size_t *len) {
    if (w <= 0) w = c->w - x0; if (h <= 0) h = c->h - y0;
    Buf b; buf_init(&b); Col cf = 0, cb = 0; uint16_t ca = 0;
    for (int y = y0; y < y0 + h && y < c->h; y++) {
        int last = x0 + w - 1;
        if (trim && cmode == 0) { while (last >= x0 && (cv_at(c, last, y) == NULL || cv_at(c, last, y)->ch == ' ' || cv_at(c, last, y)->ch == CH_CONT)) { if (cv_at(c, last, y) && cv_at(c, last, y)->ch == CH_CONT) break; last--; } }
        for (int x = x0; x <= last && x < c->w; x++) {
            Cell *q = cv_at(c, x, y); if (!q) continue; if (q->ch == CH_CONT) continue;
            if (cmode > 0) sgr_apply(&b, cmode, q->fg, q->bg, q->attr, &cf, &cb, &ca);
            char u[4]; int l = utf8_put(u, q->ch ? q->ch : ' '); buf_add(&b, u, (size_t)l);
        }
        if (cmode > 0 && (cf || cb || ca)) { buf_adds(&b, "\x1b[0m"); cf = cb = 0; ca = 0; }
        if (y + 1 < y0 + h && y + 1 < c->h) buf_addc(&b, '\n');
    }
    if (!b.d) buf_add(&b, "", 0);
    if (len) *len = b.n;
    return b.d;
}
char *cv_to_html(Canvas *c, size_t *len) {
    Buf b; buf_init(&b);
    buf_adds(&b, "<!DOCTYPE html>\n<meta charset=\"utf-8\"><title>ascii_screen</title>\n<body style=\"background:#0b0b0e;margin:0\"><pre style=\"color:#d8d8d8;background:#0b0b0e;font:14px/1.0 'DejaVu Sans Mono',Menlo,Consolas,monospace;margin:12px;letter-spacing:0\">");
    Col lf = 1, lb = 1; uint16_t la = 0xFFFF; bool open = false;
    for (int y = 0; y < c->h; y++) {
        for (int x = 0; x < c->w; x++) {
            Cell *q = cv_at(c, x, y); if (q->ch == CH_CONT) continue;
            if (q->fg != lf || q->bg != lb || q->attr != la) {
                if (open) buf_adds(&b, "</span>");
                buf_adds(&b, "<span style=\"");
                Col fg = q->fg, bg = q->bg; if (q->attr & AT_REVERSE) { Col t = fg; fg = bg; bg = t; }
                if (fg & COL_SET) buf_printf(&b, "color:#%02x%02x%02x;", COL_R(fg), COL_G(fg), COL_B(fg));
                if (bg & COL_SET) buf_printf(&b, "background:#%02x%02x%02x;", COL_R(bg), COL_G(bg), COL_B(bg));
                if (q->attr & AT_BOLD) buf_adds(&b, "font-weight:bold;"); if (q->attr & AT_ITALIC) buf_adds(&b, "font-style:italic;");
                if (q->attr & AT_UNDER) buf_adds(&b, "text-decoration:underline;"); if (q->attr & AT_DIM) buf_adds(&b, "opacity:.6;");
                buf_adds(&b, "\">"); open = true; lf = q->fg; lb = q->bg; la = q->attr;
            }
            uint32_t ch = q->ch ? q->ch : ' ';
            if (ch == '<') buf_adds(&b, "&lt;"); else if (ch == '>') buf_adds(&b, "&gt;"); else if (ch == '&') buf_adds(&b, "&amp;");
            else { char u[4]; int l = utf8_put(u, ch); buf_add(&b, u, (size_t)l); }
        }
        buf_addc(&b, '\n');
    }
    if (open) buf_adds(&b, "</span>");
    buf_adds(&b, "</pre></body>\n");
    if (len) *len = b.n;
    return b.d;
}
