/* std.ascii_screen — text fonts.
 *
 *  built-in pixel fonts (5x7 glyphs):  "block"  square pixels from full blocks (██)
 *                                      "half"   ▀▄█ half blocks (compact, smooth)
 *                                      "quad"   quadrant blocks (very compact)
 *                                      "braille" 2x4 dot cells (tiny)
 *                                      "ascii"  one character per pixel ("char" option)
 *  loadable fonts:  FIGlet .flf/.tlf (full width, fitted and smushing layouts), BDF bitmap fonts
 *  user fonts:      font_define(name, chars, glyph_rows)  — glyphs drawn as rows of "#"/"." text
 */
#include "as_internal.h"

static const uint8_t F57[95][5] = {
    {0,0,0,0,0},{0,0,0x5F,0,0},{0,7,0,7,0},{0x14,0x7F,0x14,0x7F,0x14},{0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,8,0x64,0x62},{0x36,0x49,0x55,0x22,0x50},{0,5,3,0,0},
    {0,0x1C,0x22,0x41,0},{0,0x41,0x22,0x1C,0},{0x14,8,0x3E,8,0x14},{8,8,0x3E,8,8},{0,0x50,0x30,0,0},{8,8,8,8,8},{0,0x60,0x60,0,0},{0x20,0x10,8,4,2},
    {0x3E,0x51,0x49,0x45,0x3E},{0,0x42,0x7F,0x40,0},{0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},{0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x30},{1,0x71,9,5,3},
    {0x36,0x49,0x49,0x49,0x36},{6,0x49,0x49,0x29,0x1E},{0,0x36,0x36,0,0},{0,0x56,0x36,0,0},{8,0x14,0x22,0x41,0},{0x14,0x14,0x14,0x14,0x14},{0,0x41,0x22,0x14,8},{2,1,0x51,9,6},
    {0x32,0x49,0x79,0x41,0x3E},{0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},{0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},{0x7F,9,9,9,1},{0x3E,0x41,0x49,0x49,0x7A},
    {0x7F,8,8,8,0x7F},{0,0x41,0x7F,0x41,0},{0x20,0x40,0x41,0x3F,1},{0x7F,8,0x14,0x22,0x41},{0x7F,0x40,0x40,0x40,0x40},{0x7F,2,0x0C,2,0x7F},{0x7F,4,8,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,9,9,9,6},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,9,0x19,0x29,0x46},{0x46,0x49,0x49,0x49,0x31},{1,1,0x7F,1,1},{0x3F,0x40,0x40,0x40,0x3F},{0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F},
    {0x63,0x14,8,0x14,0x63},{7,8,0x70,8,7},{0x61,0x51,0x49,0x45,0x43},{0,0x7F,0x41,0x41,0},{2,4,8,0x10,0x20},{0,0x41,0x41,0x7F,0},{4,2,1,2,4},{0x40,0x40,0x40,0x40,0x40},
    {0,1,2,4,0},{0x20,0x54,0x54,0x54,0x78},{0x7F,0x48,0x44,0x44,0x38},{0x38,0x44,0x44,0x44,0x20},{0x38,0x44,0x44,0x48,0x7F},{0x38,0x54,0x54,0x54,0x18},{8,0x7E,9,1,2},{0x0C,0x52,0x52,0x52,0x3E},
    {0x7F,8,4,4,0x78},{0,0x44,0x7D,0x40,0},{0x20,0x40,0x44,0x3D,0},{0x7F,0x10,0x28,0x44,0},{0,0x41,0x7F,0x40,0},{0x7C,4,0x18,4,0x78},{0x7C,8,4,4,0x78},{0x38,0x44,0x44,0x44,0x38},
    {0x7C,0x14,0x14,0x14,8},{8,0x14,0x14,0x18,0x7C},{0x7C,8,4,4,8},{0x48,0x54,0x54,0x54,0x20},{4,0x3F,0x44,0x40,0x20},{0x3C,0x40,0x40,0x20,0x7C},{0x1C,0x20,0x40,0x20,0x1C},{0x3C,0x40,0x30,0x40,0x3C},
    {0x44,0x28,0x10,0x28,0x44},{0x0C,0x50,0x50,0x50,0x3C},{0x44,0x64,0x54,0x4C,0x44},{0,8,0x36,0x41,0},{0,0,0x7F,0,0},{0,0x41,0x36,8,0},{0x10,8,8,0x10,8},
};
void glyph_mask_5x7(uint32_t ch, uint8_t rows[7]) {
    memset(rows, 0, 7);
    if (ch < 32 || ch > 126) return;
    for (int x = 0; x < 5; x++) for (int y = 0; y < 7; y++) if ((F57[ch - 32][x] >> y) & 1) rows[y] |= (uint8_t)(1 << (4 - x));
}

typedef struct {
    bool used; char name[48]; int kind;                    /* 0 builtin 5x7, 1 bitmap font, 2 FIGlet */
    int style;                                              /* builtin: 0 block 1 half 2 quad 3 braille 4 ascii */
    int gh, ng; uint32_t *cps; uint8_t **bm; int *gw;       /* bitmap glyphs (gw x gh, 1 byte / pixel) */
    int fl_h, old_layout, full_layout; char hard; char **fl;/* FIGlet: fl[(cp-32)*fl_h + row] */
} Font;
static Font *FONTS; static int nfonts, capfonts;
const char *FONT_BUILTIN_NAMES[] = { "block", "half", "quad", "braille", "ascii", NULL };

static int font_slot(void) {
    for (int i = 0; i < nfonts; i++) if (!FONTS[i].used) { memset(&FONTS[i], 0, sizeof(Font)); return i; }
    if (nfonts >= capfonts) { capfonts = capfonts ? capfonts * 2 : 16; FONTS = kc_realloc(FONTS, (size_t)capfonts * sizeof(Font)); }
    memset(&FONTS[nfonts], 0, sizeof(Font)); return nfonts++;
}
static void fonts_init(void) {
    if (nfonts) return;
    for (int i = 0; FONT_BUILTIN_NAMES[i]; i++) { int s = font_slot(); FONTS[s].used = true; FONTS[s].kind = 0; FONTS[s].style = i; snprintf(FONTS[s].name, sizeof FONTS[s].name, "%s", FONT_BUILTIN_NAMES[i]); }
}
static void font_release(Font *f) {
    for (int i = 0; i < f->ng; i++) free(f->bm[i]);
    free(f->bm); free(f->cps); free(f->gw);
    if (f->fl) { for (int i = 0; i < 95 * f->fl_h; i++) free(f->fl[i]); free(f->fl); }
    f->used = false;
}
void fonts_free_all(void) { for (int i = 0; i < nfonts; i++) if (FONTS[i].used) font_release(&FONTS[i]); free(FONTS); FONTS = NULL; nfonts = capfonts = 0; }
void font_free_id(int id) { fonts_init(); if (id >= 5 && id < nfonts && FONTS[id].used) font_release(&FONTS[id]); }
int font_find(KcValue v, const char *who) {
    fonts_init();
    if (V_isnum(v) && v.type != VAL_BOOL) { int id = (int)V_num(v, who); if (id < 0 || id >= nfonts || !FONTS[id].used) as_err("%s: invalid font %d", who, id); return id; }
    if (v.type == VAL_STRING) { for (int i = 0; i < nfonts; i++) if (FONTS[i].used && !strcmp(FONTS[i].name, v.as.s->data)) return i; as_err("%s: unknown font \"%s\" (built-in: block, half, quad, braille, ascii, or one loaded with font_load/font_define)", who, v.as.s->data); }
    as_err("%s: font must be a name or id", who);
}
int font_by_name(const char *n) { fonts_init(); for (int i = 0; i < nfonts; i++) if (FONTS[i].used && !strcmp(FONTS[i].name, n)) return i; return -1; }

/* ------------------------------------------------------------- loaders */
static void bm_add(Font *f, uint32_t cp, int w, uint8_t *bm) {
    f->cps = kc_realloc(f->cps, (size_t)(f->ng + 1) * sizeof(uint32_t)); f->bm = kc_realloc(f->bm, (size_t)(f->ng + 1) * sizeof(uint8_t *)); f->gw = kc_realloc(f->gw, (size_t)(f->ng + 1) * sizeof(int));
    f->cps[f->ng] = cp; f->bm[f->ng] = bm; f->gw[f->ng] = w; f->ng++;
}
static const char *base_name(const char *p) { const char *s = strrchr(p, '/'); const char *b = strrchr(p, '\\'); if (b && (!s || b > s)) s = b; return s ? s + 1 : p; }
static void set_name(Font *f, const char *name, const char *path) {
    char tmp[48]; if (name && *name) snprintf(tmp, sizeof tmp, "%s", name); else { snprintf(tmp, sizeof tmp, "%s", base_name(path)); char *d = strrchr(tmp, '.'); if (d) *d = 0; }
    snprintf(f->name, sizeof f->name, "%s", tmp);
}
static char *line_dup(const char *s, size_t n) { char *d = kc_malloc(n + 1); memcpy(d, s, n); d[n] = 0; return d; }
static int load_figlet(uint8_t *d, size_t n, Font *f, char *err) {
    if (n < 6 || memcmp(d, "flf2a", 5)) { snprintf(err, 200, "not a FIGlet font (missing flf2a header)"); return 0; }
    const char *p = (const char *)d, *end = p + n; char hb = p[5];
    int h = 0, bl = 0, ml = 0, ol = 0, cl = 0, pd = 0, fl = -1;
    int got = sscanf(p + 6, "%d %d %d %d %d %d %d", &h, &bl, &ml, &ol, &cl, &pd, &fl);
    if (got < 5 || h < 1 || h > 64) { snprintf(err, 200, "bad FIGlet header"); return 0; }
    f->kind = 2; f->fl_h = h; f->old_layout = ol; f->full_layout = got >= 7 ? fl : -2; f->hard = hb;
    const char *q = strchr(p, '\n'); if (!q) { snprintf(err, 200, "truncated FIGlet font"); return 0; } q++;
    for (int i = 0; i < cl && q < end; i++) { const char *e = memchr(q, '\n', (size_t)(end - q)); if (!e) break; q = e + 1; }
    f->fl = kc_malloc((size_t)95 * h * sizeof(char *)); for (int i = 0; i < 95 * h; i++) f->fl[i] = NULL;
    for (int g = 0; g < 95; g++) for (int r = 0; r < h; r++) {
        if (q >= end) { f->fl[g * h + r] = line_dup("", 0); continue; }
        const char *e = memchr(q, '\n', (size_t)(end - q)); size_t l = e ? (size_t)(e - q) : (size_t)(end - q);
        if (l && q[l - 1] == '\r') l--;
        if (l) { char mark = q[l - 1]; while (l && q[l - 1] == mark) l--; }
        f->fl[g * h + r] = line_dup(q, l); q = e ? e + 1 : end;
    }
    return 1;
}
static int load_bdf(const char *txt, Font *f, char *err) {
    int fw = 0, fh = 0, fx = 0, fy = 0; const char *p = strstr(txt, "FONTBOUNDINGBOX");
    if (!p || sscanf(p + 15, "%d %d %d %d", &fw, &fh, &fx, &fy) != 4 || fw < 1 || fh < 1) { snprintf(err, 200, "BDF: missing FONTBOUNDINGBOX"); return 0; }
    f->kind = 1; f->gh = fh; int asc = fh + fy;
    p = txt;
    while ((p = strstr(p, "STARTCHAR")) != NULL) {
        const char *e = strstr(p, "ENDCHAR"); if (!e) break;
        int enc = -1, dw = fw, bw = 0, bh = 0, bx = 0, by = 0; const char *t;
        if ((t = strstr(p, "ENCODING")) && t < e) enc = atoi(t + 8);
        if ((t = strstr(p, "DWIDTH")) && t < e) dw = atoi(t + 6);
        if ((t = strstr(p, "BBX")) && t < e) sscanf(t + 3, "%d %d %d %d", &bw, &bh, &bx, &by);
        const char *bm = strstr(p, "BITMAP");
        if (enc >= 0 && bm && bm < e && bw > 0 && bh > 0) {
            int gw = dw > 0 ? dw : fw; if (bx - fx + bw > gw) gw = bx - fx + bw; if (gw < 1) gw = 1; if (gw > 64) gw = 64;
            uint8_t *px = kc_malloc((size_t)gw * fh); memset(px, 0, (size_t)gw * fh);
            const char *ln = strchr(bm, '\n'); if (ln) ln++;
            int top = asc - (by + bh);
            for (int r = 0; r < bh && ln && ln < e; r++) {
                int xx = 0; const char *hx = ln;
                while (*hx && *hx != '\n' && *hx != '\r') { int v = isdigit((unsigned char)*hx) ? *hx - '0' : (tolower((unsigned char)*hx) - 'a' + 10); for (int b = 3; b >= 0; b--, xx++) if (((v >> b) & 1) && xx < bw) { int ox = bx - fx + xx, oy = top + r; if (ox >= 0 && ox < gw && oy >= 0 && oy < fh) px[(size_t)oy * gw + ox] = 1; } hx++; }
                ln = strchr(ln, '\n'); if (ln) ln++;
            }
            bm_add(f, (uint32_t)enc, gw, px);
        }
        p = e + 7;
    }
    if (!f->ng) { snprintf(err, 200, "BDF: no glyphs"); return 0; }
    return 1;
}
int font_load_file(const char *path, const char *name) {
    fonts_init(); size_t n; uint8_t *d = as_read_file(path, &n); if (!d) as_err("font_load: cannot open '%s'", path);
    int s = font_slot(); Font *f = &FONTS[s]; f->used = true; char err[256] = ""; int ok;
    if (n > 5 && !memcmp(d, "flf2a", 5)) ok = load_figlet(d, n, f, err); else if (n > 14 && strstr((char *)d, "STARTFONT")) ok = load_bdf((char *)d, f, err);
    else { snprintf(err, sizeof err, "unsupported font file (FIGlet .flf/.tlf and BDF .bdf are supported)"); ok = 0; }
    free(d);
    if (!ok) { font_release(f); as_err("font_load: '%s': %s", path, err); }
    set_name(f, name, path); return s;
}
int font_define(const char *name, int h, const char *chars, KcValue rows) {
    fonts_init();
    if (h < 1 || h > 64) as_err("font_define: glyph height must be 1..64");
    if (rows.type != VAL_ARRAY) as_err("font_define: glyphs must be an array of row-arrays");
    int s = font_slot(); Font *f = &FONTS[s]; f->used = true; f->kind = 1; f->gh = h; set_name(f, name, name);
    const char *c = chars; int gi = 0;
    while (*c) {
        uint32_t cp = utf8_next(&c); if (gi >= rows.as.arr->count) break;
        KcValue g = rows.as.arr->items[gi++]; if (g.type != VAL_ARRAY) { font_release(f); as_err("font_define: glyph %d must be an array of strings", gi); }
        int w = 0; for (int r = 0; r < g.as.arr->count; r++) if (g.as.arr->items[r].type == VAL_STRING) { int l = (int)strlen(g.as.arr->items[r].as.s->data); if (l > w) w = l; }
        if (w < 1) w = 1; uint8_t *px = kc_malloc((size_t)w * h); memset(px, 0, (size_t)w * h);
        for (int r = 0; r < g.as.arr->count && r < h; r++) if (g.as.arr->items[r].type == VAL_STRING) { const char *ln = g.as.arr->items[r].as.s->data; for (int x = 0; ln[x]; x++) if (ln[x] != ' ' && ln[x] != '.' && ln[x] != '_' && ln[x] != '0') px[(size_t)r * w + x] = 1; }
        bm_add(f, cp, w, px);
    }
    return s;
}

/* ------------------------------------------------------------- FIGlet layout */
static bool is_blank(char c) { return c == ' '; }
static char smush2(char a, char b, char hard, int rules, bool universal) {
    if (is_blank(a)) return b; if (is_blank(b)) return a;
    if (a == hard || b == hard) { if (a == hard && b == hard && (rules & 32)) return hard; return 0; }
    if (universal) return b;
    if ((rules & 1) && a == b) return a;
    if (rules & 2) { if (a == '_' && strchr("|/\\[]{}()<>", b)) return b; if (b == '_' && strchr("|/\\[]{}()<>", a)) return a; }
    if (rules & 4) { static const char *cls[] = { "|", "/\\", "[]", "{}", "()", "<>" }; int ca = -1, cb = -1; for (int i = 0; i < 6; i++) { if (strchr(cls[i], a)) ca = i; if (strchr(cls[i], b)) cb = i; } if (ca >= 0 && cb >= 0 && ca != cb) return ca > cb ? a : b; }
    if (rules & 8) { if ((a == '[' && b == ']') || (a == ']' && b == '[') || (a == '{' && b == '}') || (a == '}' && b == '{') || (a == '(' && b == ')') || (a == ')' && b == '(')) return '|'; }
    if (rules & 16) { if (a == '/' && b == '\\') return '|'; if (a == '\\' && b == '/') return 'Y'; if (a == '>' && b == '<') return 'X'; }
    return 0;
}
/* returns malloc'd array of fl_h strings for one text line */
static char **figlet_line(Font *f, const char *s, int layout /*0 full 1 fitted 2 smush*/) {
    int h = f->fl_h; char **line = kc_malloc((size_t)h * sizeof(char *)); for (int r = 0; r < h; r++) line[r] = line_dup("", 0);
    int rules = 0; bool universal = false;
    if (layout == 2) { if (f->full_layout >= 0) { rules = f->full_layout & 63; universal = !(f->full_layout & 63) && (f->full_layout & 128); } else if (f->old_layout > 0) rules = f->old_layout & 63; else universal = true; }
    for (; *s; s++) {
        unsigned char ch = (unsigned char)*s; if (ch < 32 || ch > 126) continue;
        char **g = &f->fl[(ch - 32) * h]; int amt = 1 << 30;
        if (layout > 0) {
            for (int r = 0; r < h; r++) {
                int cl = (int)strlen(line[r]), gl = (int)strlen(g[r]), lb = 0, cb = 0;
                while (lb < cl && is_blank(line[r][cl - 1 - lb])) lb++; while (cb < gl && is_blank(g[r][cb])) cb++;
                int a = lb + cb;
                if (layout == 2 && lb < cl && cb < gl) { char x = line[r][cl - 1 - lb], y = g[r][cb]; if (smush2(x, y, f->hard, rules, universal)) a++; }
                if (a < amt) amt = a;
            }
        } else amt = 0;
        for (int r = 0; r < h; r++) {
            int cl = (int)strlen(line[r]), gl = (int)strlen(g[r]), a = amt; if (a > gl) a = gl; if (a > cl) a = cl;
            char *nl = kc_malloc((size_t)(cl + gl - a + 1)); memcpy(nl, line[r], (size_t)(cl - a));
            for (int k = 0; k < a; k++) { char x = line[r][cl - a + k], y = g[r][k], m = smush2(x, y, f->hard, rules, universal); if (!m) m = is_blank(y) ? x : y; nl[cl - a + k] = m; }
            memcpy(nl + cl, g[r] + a, (size_t)(gl - a)); nl[cl + gl - a] = 0; free(line[r]); line[r] = nl;
        }
    }
    for (int r = 0; r < h; r++) for (char *c = line[r]; *c; c++) if (*c == f->hard) *c = ' ';
    return line;
}

/* ------------------------------------------------------------- rendering */
static Col grad_color(const Opts *o, int x, int w, Col base) {
    const KcValue *g = opt_find(o, "gradient");
    if (g && g->type == VAL_ARRAY && g->as.arr->count >= 2) {
        int n = g->as.arr->count; float t = w > 1 ? (float)x / (float)(w - 1) : 0; float pos = t * (float)(n - 1); int i = (int)pos; if (i >= n - 1) i = n - 2;
        return col_mix(col_from_value(g->as.arr->items[i], "gradient"), col_from_value(g->as.arr->items[i + 1], "gradient"), pos - (float)i);
    }
    if (opt_bool(o, "rainbow", false)) return col_hsv(w > 1 ? (float)x / (float)w : 0, 0.85f, 1.0f);
    return base;
}
static const uint8_t *glyph_bm(Font *f, uint32_t cp, int *w, uint8_t *tmp35) {
    if (f->kind == 0) { uint8_t rows[7]; glyph_mask_5x7(cp, rows); for (int y = 0; y < 7; y++) for (int x = 0; x < 5; x++) tmp35[y * 5 + x] = (rows[y] >> (4 - x)) & 1; *w = 5; return tmp35; }
    for (int i = 0; i < f->ng; i++) if (f->cps[i] == cp) { *w = f->gw[i]; return f->bm[i]; }
    for (int i = 0; i < f->ng; i++) if (f->cps[i] == '?') { *w = f->gw[i]; return f->bm[i]; }
    *w = 0; return NULL;
}
static void render_one_line(Font *f, const char *s, const Opts *o, Canvas *dst, int ox, int oy, int *ow, int *oh, bool draw, Col fg, uint16_t attr) {
    if (f->kind == 2) {
        const char *lay = opt_str(o, "layout", "smush"); int layout = !strcmp(lay, "full") ? 0 : !strcmp(lay, "fitted") ? 1 : 2;
        char **ln = figlet_line(f, s, layout); int w = 0;
        for (int r = 0; r < f->fl_h; r++) { int l = (int)strlen(ln[r]); if (l > w) w = l; }
        if (draw) for (int r = 0; r < f->fl_h; r++) for (int x = 0; ln[r][x]; x++) if (ln[r][x] != ' ') cv_put(dst, ox + x, oy + r, (uint32_t)(unsigned char)ln[r][x], grad_color(o, x, w, fg), 0, attr);
        for (int r = 0; r < f->fl_h; r++) free(ln[r]); free(ln);
        *ow = w; *oh = f->fl_h; return;
    }
    int spacing = opt_int(o, "spacing", 1), scale = as_clampi(opt_int(o, "scale", 1), 1, 8), gh = f->kind == 0 ? 7 : f->gh;
    /* measure */
    int W = 0, n = 0; { const char *p = s; while (*p) { uint32_t cp = utf8_next(&p); uint8_t t[35]; int gw; glyph_bm(f, cp == ' ' ? ' ' : cp, &gw, t); if (cp == ' ' && f->kind != 0) gw = gh / 2 + 1; W += gw + (n ? spacing : 0); n++; } }
    if (W < 1) { *ow = 0; *oh = 0; return; }
    uint8_t *bm = kc_malloc((size_t)W * gh); memset(bm, 0, (size_t)W * gh); int cx = 0; n = 0;
    { const char *p = s; while (*p) { uint32_t cp = utf8_next(&p); uint8_t t[35]; int gw; const uint8_t *g = glyph_bm(f, cp, &gw, t); if (cp == ' ' && f->kind != 0) gw = gh / 2 + 1; if (n) cx += spacing; if (g && cp != ' ') for (int y = 0; y < gh; y++) for (int x = 0; x < gw; x++) bm[(size_t)y * W + cx + x] = g[(size_t)y * gw + x]; cx += gw; n++; } }
    int style = f->kind == 0 ? f->style : (opt_has(o, "style") ? (strcmp(opt_str(o, "style", "block"), "half") == 0 ? 1 : !strcmp(opt_str(o, "style", ""), "quad") ? 2 : !strcmp(opt_str(o, "style", ""), "braille") ? 3 : !strcmp(opt_str(o, "style", ""), "ascii") ? 4 : 0) : 0);
    uint32_t chr = 0; { const char *cs = opt_str(o, "char", "#"); const char *q = cs; chr = utf8_next(&q); if (!chr) chr = '#'; }
    int PW = W * scale, PH = gh * scale;
    if (style == 0) {
        int cw = PW * 2, chh = PH; *ow = cw; *oh = chh;
        if (draw) for (int y = 0; y < PH; y++) for (int x = 0; x < PW; x++) if (bm[(size_t)(y / scale) * W + x / scale]) { Col c = grad_color(o, x * 2, cw, fg); cv_put(dst, ox + x * 2, oy + y, 0x2588, c, 0, attr); cv_put(dst, ox + x * 2 + 1, oy + y, 0x2588, c, 0, attr); }
    } else if (style == 4) {
        *ow = PW; *oh = PH;
        if (draw) for (int y = 0; y < PH; y++) for (int x = 0; x < PW; x++) if (bm[(size_t)(y / scale) * W + x / scale]) cv_put(dst, ox + x, oy + y, chr, grad_color(o, x, PW, fg), 0, attr);
    } else {
        int sx = style == 1 ? 1 : 2, sy = style == 3 ? 4 : 2, pw = (PW + sx - 1) / sx * sx, ph = (PH + sy - 1) / sy * sy;
        *ow = pw / sx; *oh = ph / sy;
        if (draw) {
            Col *px = kc_malloc((size_t)pw * ph * sizeof(Col)); memset(px, 0, (size_t)pw * ph * sizeof(Col));
            for (int y = 0; y < PH; y++) for (int x = 0; x < PW; x++) if (bm[(size_t)(y / scale) * W + x / scale]) px[(size_t)y * pw + x] = grad_color(o, x, PW, fg) | COL_SET;
            hr_to_cells(px, pw, ph, style == 1 ? HR_HALF : style == 2 ? HR_QUAD : HR_BRAILLE, dst, ox, oy, 0, attr); free(px);
        }
    }
    free(bm);
}
void big_text_render(int fid, const char *s, const Opts *o, Canvas *c, int x, int y, bool draw, int *out_w, int *out_h) {
    fonts_init(); Font *f = &FONTS[fid];
    Col fg = opt_col(o, "fg", S_style.fg); uint16_t attr = S_style.attr; if (opt_bool(o, "bold", false)) attr |= AT_BOLD;
    int gap = opt_int(o, "line_gap", 1), cw = 0, total_h = 0;
    /* split lines */
    char *dup = kc_malloc(strlen(s) + 1); strcpy(dup, s);
    typedef struct { char *s; int w, h; } L; L ls[64]; int nl = 0; char *p = dup;
    while (nl < 64) { char *e = strchr(p, '\n'); if (e) *e = 0; ls[nl].s = p; render_one_line(f, p, o, NULL, 0, 0, &ls[nl].w, &ls[nl].h, false, fg, attr); nl++; if (!e) break; p = e + 1; }
    for (int i = 0; i < nl; i++) { if (ls[i].w > cw) cw = ls[i].w; total_h += ls[i].h + (i ? gap : 0); }
    if (out_w) *out_w = cw; if (out_h) *out_h = total_h;
    if (draw) {
        const char *al = opt_str(o, "align", "left"); int ax = !strcmp(al, "center") ? -cw / 2 : !strcmp(al, "right") ? -cw : 0; int yy = y;
        Col sh = opt_col(o, "shadow", 0);
        for (int pass = sh ? 0 : 1; pass < 2; pass++) {
            yy = y;
            for (int i = 0; i < nl; i++) {
                int lw = ls[i].w, off = !strcmp(al, "center") ? (cw - lw) / 2 : !strcmp(al, "right") ? cw - lw : 0; int w2, h2;
                if (pass == 0) { int tid = cv_new(lw + 1, ls[i].h + 1); Canvas *tmp = cv_get(tid); render_one_line(f, ls[i].s, o, tmp, 0, 0, &w2, &h2, true, sh, attr); cv_blit(c, tmp, 0, 0, 0, 0, x + ax + off + 1, yy + 1, true, true); cv_free(tid); }
                else render_one_line(f, ls[i].s, o, c, x + ax + off, yy, &w2, &h2, true, fg, attr);
                yy += ls[i].h + gap;
            }
        }
    }
    free(dup);
}
KcValue big_text_lines(int fid, const char *s, const Opts *o) {
    int w, h; big_text_render(fid, s, o, NULL, 0, 0, false, &w, &h);
    int id = cv_new(w > 0 ? w : 1, h > 0 ? h : 1); Canvas *c = cv_get(id);
    big_text_render(fid, s, o, c, 0, 0, true, &w, &h);
    KcValue arr = kc_array_new();
    for (int y = 0; y < h; y++) { size_t len; char *t = cv_to_text(c, 0, y, w, 1, 0, true, &len); kc_array_push(arr, ret_str(t, len)); free(t); }
    cv_free(id); return arr;
}
