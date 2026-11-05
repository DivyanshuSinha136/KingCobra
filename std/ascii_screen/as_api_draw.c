/* std.ascii_screen — bindings: canvases, text, shapes, boxes, hi-res pixel layers, export/import. */
#include "as_api.h"

Sty sty_from(const Opts *o) {
    Sty s = { S_style.fg, S_style.bg, S_style.attr };
    if (!o) return s;
    if (opt_has(o, "fg")) s.fg = opt_col(o, "fg", 0); if (opt_has(o, "bg")) s.bg = opt_col(o, "bg", 0);
    static const struct { const char *n; uint16_t b; } AT[] = { { "bold", AT_BOLD }, { "dim", AT_DIM }, { "italic", AT_ITALIC }, { "underline", AT_UNDER }, { "blink", AT_BLINK }, { "reverse", AT_REVERSE }, { "strike", AT_STRIKE } };
    for (size_t i = 0; i < sizeof AT / sizeof AT[0]; i++) if (opt_has(o, AT[i].n)) { if (opt_bool(o, AT[i].n, false)) s.attr |= AT[i].b; else s.attr &= (uint16_t)~AT[i].b; }
    return s;
}
static uint32_t ch_arg(KcValue *a, int n, int i, const char *who) {
    if (i >= n) as_err("%s: missing character argument", who);
    if (a[i].type == VAL_STRING) { const char *s = a[i].as.s->data; if (!*s) return ' '; return utf8_next(&s); }
    int c = (int)V_num(a[i], who); if (c < 0 || c > 0x10FFFF) as_err("%s: bad code point %d", who, c); return (uint32_t)c;
}
static Canvas *CT(void) { return cv_target(); }

/* ------------------------------------------------------------ canvases */
NAT(d_canvas) { BEGIN(2, 2, "canvas(w, h)"); return kc_int(cv_new(A_int(a, n, 0, "canvas"), A_int(a, n, 1, "canvas"))); }
NAT(d_canvas_free) { BEGIN(1, 1, "canvas_free(id)"); cv_free(A_int(a, n, 0, "canvas_free")); return OKV; }
NAT(d_canvas_size) { BEGIN(1, 1, "canvas_size(id)"); Canvas *c = cv_must(A_int(a, n, 0, "canvas_size"), "canvas_size"); KcValue r = kc_array_new(); kc_array_push(r, kc_int(c->w)); kc_array_push(r, kc_int(c->h)); return r; }
NAT(d_canvas_resize) { BEGIN(3, 3, "canvas_resize(id, w, h)"); int id = A_int(a, n, 0, "canvas_resize"); if (id == 0) as_err("canvas_resize: the screen follows the terminal size"); cv_resize(cv_must(id, "canvas_resize"), A_int(a, n, 1, "canvas_resize"), A_int(a, n, 2, "canvas_resize")); return OKV; }
NAT(d_canvas_copy) { BEGIN(1, 1, "canvas_copy(id)"); Canvas *s = cv_must(A_int(a, n, 0, "canvas_copy"), "canvas_copy"); int id = cv_new(s->w, s->h); memcpy(cv_get(id)->c, s->c, (size_t)s->w * s->h * sizeof(Cell)); return kc_int(id); }
NAT(d_target) { BEGIN(0, 1, "target([id])"); term_ensure(); if (n == 0) return kc_int(S_target); int id = A_int(a, n, 0, "target"); cv_must(id, "target"); S_target = id; return OKV; }
NAT(d_style) {
    BEGIN(0, 20, "style(\"fg\", color, \"bg\", color, \"bold\", true, ...)  -- no arguments resets"); if (n == 0) { S_style.fg = S_style.bg = 0; S_style.attr = 0; return OKV; }
    Opts o = opts_make(a, n, 0); Sty s = sty_from(&o); S_style.fg = s.fg; S_style.bg = s.bg; S_style.attr = s.attr; return OKV;
}
NAT(d_clear) { BEGIN(0, 11, "clear([char], options...)"); Canvas *c = CT(); uint32_t ch = ' '; int st = 0; if (n >= 1 && (n % 2 == 1 || a[0].type != VAL_STRING)) { ch = ch_arg(a, n, 0, "clear"); st = 1; } Opts o = opts_make(a, n, st); Sty s = sty_from(&o); cv_clear(c, ch, s.fg, s.bg, s.attr); return OKV; }
NAT(d_put) { BEGIN(3, 11, "put(x, y, char, options...)"); Opts o = opts_make(a, n, 3); Sty s = sty_from(&o); cv_put(CT(), A_int(a, n, 0, "put"), A_int(a, n, 1, "put"), ch_arg(a, n, 2, "put"), s.fg, s.bg, s.attr); return OKV; }
NAT(d_text) {
    BEGIN(3, 20, "text(x, y, string, options...)"); int x = A_int(a, n, 0, "text"), y = A_int(a, n, 1, "text"); const char *str; char *tmp = NULL;
    if (a[2].type == VAL_STRING) str = a[2].as.s->data; else { tmp = kc_to_display_string(a[2]); str = tmp; }
    Opts o = opts_make(a, n, 3); Sty s = sty_from(&o); int width = opt_int(&o, "width", 0); const char *al = opt_str(&o, "align", "left"); Canvas *c = CT(); int maxw = 0;
    char **lines = NULL; int nl; if (width > 0) nl = cv_wrap_lines(str, width, &lines);
    else { int cap = 4; nl = 0; lines = kc_malloc((size_t)cap * sizeof(char *)); const char *p = str; while (1) { const char *e = strchr(p, '\n'); size_t l = e ? (size_t)(e - p) : strlen(p); if (nl >= cap) { cap *= 2; lines = kc_realloc(lines, (size_t)cap * sizeof(char *)); } lines[nl] = kc_malloc(l + 1); memcpy(lines[nl], p, l); lines[nl][l] = 0; nl++; if (!e) break; p = e + 1; } }
    if (width > 0 && opt_bool(&o, "pad", opt_has(&o, "bg"))) cv_fill(c, x, y, width, nl, ' ', s.fg, s.bg, s.attr);
    for (int i = 0; i < nl; i++) { int tw = str_width(lines[i]); int off = width > 0 ? (!strcmp(al, "center") ? (width - tw) / 2 : !strcmp(al, "right") ? width - tw : 0) : 0; if (off < 0) off = 0; cv_text(c, x + off, y + i, lines[i], strlen(lines[i]), s.fg, s.bg, s.attr); if (tw > maxw) maxw = tw; free(lines[i]); }
    free(lines); free(tmp); return kc_int(maxw);
}
NAT(d_text_width) { BEGIN(1, 1, "text_width(string)"); return kc_int(str_width(A_str(a, n, 0, "text_width"))); }
NAT(d_text_wrap) { BEGIN(2, 2, "text_wrap(string, width)"); char **L; int nl = cv_wrap_lines(A_str(a, n, 0, "text_wrap"), A_int(a, n, 1, "text_wrap"), &L); KcValue r = kc_array_new(); for (int i = 0; i < nl; i++) { kc_array_push(r, kc_string(L[i])); free(L[i]); } free(L); return r; }
NAT(d_fill) { BEGIN(4, 14, "fill(x, y, w, h [, char], options...)"); int st = 4; uint32_t ch = ' '; if (n > 4 && ((n - 4) % 2 == 1)) { ch = ch_arg(a, n, 4, "fill"); st = 5; } Opts o = opts_make(a, n, st); Sty s = sty_from(&o); cv_fill(CT(), A_int(a, n, 0, "fill"), A_int(a, n, 1, "fill"), A_int(a, n, 2, "fill"), A_int(a, n, 3, "fill"), ch, s.fg, s.bg, s.attr); return OKV; }
NAT(d_hline) { BEGIN(3, 14, "hline(x, y, length [, char], options...)"); int st = 3; uint32_t ch = 0x2500; if (n > 3 && ((n - 3) % 2 == 1)) { ch = ch_arg(a, n, 3, "hline"); st = 4; } Opts o = opts_make(a, n, st); Sty s = sty_from(&o); cv_fill(CT(), A_int(a, n, 0, "hline"), A_int(a, n, 1, "hline"), A_int(a, n, 2, "hline"), 1, ch, s.fg, s.bg, s.attr); return OKV; }
NAT(d_vline) { BEGIN(3, 14, "vline(x, y, length [, char], options...)"); int st = 3; uint32_t ch = 0x2502; if (n > 3 && ((n - 3) % 2 == 1)) { ch = ch_arg(a, n, 3, "vline"); st = 4; } Opts o = opts_make(a, n, st); Sty s = sty_from(&o); cv_fill(CT(), A_int(a, n, 0, "vline"), A_int(a, n, 1, "vline"), 1, A_int(a, n, 2, "vline"), ch, s.fg, s.bg, s.attr); return OKV; }
NAT(d_box) {
    BEGIN(4, 24, "box(x, y, w, h, options...)  options: style, title, fill, fg, bg, title_fg"); Opts o = opts_make(a, n, 4); Sty s = sty_from(&o); const char *st = opt_str(&o, "style", "single");
    if (!strcmp(st, "none")) { if (opt_bool(&o, "fill", false)) cv_fill(CT(), A_int(a, n, 0, "box"), A_int(a, n, 1, "box"), A_int(a, n, 2, "box"), A_int(a, n, 3, "box"), ' ', s.fg, s.bg, s.attr); return OKV; }
    draw_box(CT(), A_int(a, n, 0, "box"), A_int(a, n, 1, "box"), A_int(a, n, 2, "box"), A_int(a, n, 3, "box"), st, s.fg, s.bg, s.attr, opt_bool(&o, "fill", false), opt_has(&o, "title") ? opt_str(&o, "title", "") : NULL, opt_col(&o, "title_fg", 0)); return OKV;
}
NAT(d_line) { BEGIN(4, 14, "line(x0, y0, x1, y1 [, char], options...)"); int st = 4; uint32_t ch = 0; if (n > 4 && ((n - 4) % 2 == 1)) { ch = ch_arg(a, n, 4, "line"); st = 5; } Opts o = opts_make(a, n, st); Sty s = sty_from(&o); draw_line(CT(), A_int(a, n, 0, "line"), A_int(a, n, 1, "line"), A_int(a, n, 2, "line"), A_int(a, n, 3, "line"), ch, s.fg, s.bg, s.attr); return OKV; }
NAT(d_circle) { BEGIN(3, 16, "circle(cx, cy, r, options...)  options: fill, char, aspect(2), fg, bg"); Opts o = opts_make(a, n, 3); Sty s = sty_from(&o); int r = A_int(a, n, 2, "circle"); double asp = opt_num(&o, "aspect", 2.0); const char *g = opt_str(&o, "char", NULL); uint32_t ch = 0; if (g && *g) ch = utf8_next(&g);
    draw_ellipse(CT(), A_int(a, n, 0, "circle"), A_int(a, n, 1, "circle"), (int)floor(r * asp + 0.5), r, ch, s.fg, s.bg, s.attr, opt_bool(&o, "fill", false)); return OKV; }
NAT(d_ellipse) { BEGIN(4, 16, "ellipse(cx, cy, rx, ry, options...)"); Opts o = opts_make(a, n, 4); Sty s = sty_from(&o); const char *g = opt_str(&o, "char", NULL); uint32_t ch = 0; if (g && *g) ch = utf8_next(&g);
    draw_ellipse(CT(), A_int(a, n, 0, "ellipse"), A_int(a, n, 1, "ellipse"), A_int(a, n, 2, "ellipse"), A_int(a, n, 3, "ellipse"), ch, s.fg, s.bg, s.attr, opt_bool(&o, "fill", false)); return OKV; }
NAT(d_triangle) { BEGIN(6, 16, "triangle(x0, y0, x1, y1, x2, y2, options...)"); Opts o = opts_make(a, n, 6); Sty s = sty_from(&o); const char *g = opt_str(&o, "char", NULL); uint32_t ch = 0; if (g && *g) ch = utf8_next(&g);
    draw_triangle(CT(), A_int(a, n, 0, "triangle"), A_int(a, n, 1, "triangle"), A_int(a, n, 2, "triangle"), A_int(a, n, 3, "triangle"), A_int(a, n, 4, "triangle"), A_int(a, n, 5, "triangle"), ch, s.fg, s.bg, s.attr); return OKV; }
NAT(d_flood) { BEGIN(3, 13, "flood(x, y, char, options...)"); Opts o = opts_make(a, n, 3); Sty s = sty_from(&o); draw_flood(CT(), A_int(a, n, 0, "flood"), A_int(a, n, 1, "flood"), ch_arg(a, n, 2, "flood"), s.fg, s.bg, s.attr); return OKV; }
NAT(d_clip) { BEGIN(4, 4, "clip(x, y, w, h)"); cv_set_clip(CT(), A_int(a, n, 0, "clip"), A_int(a, n, 1, "clip"), A_int(a, n, 2, "clip"), A_int(a, n, 3, "clip")); return OKV; }
NAT(d_unclip) { BEGIN(0, 0, "unclip()"); cv_reset_clip(CT()); return OKV; }
NAT(d_blit) {
    BEGIN(3, 14, "blit(src_canvas, dx, dy [, sx, sy, sw, sh] [, \"transparent\", true])"); Canvas *src = cv_must(A_int(a, n, 0, "blit"), "blit"); int dx = A_int(a, n, 1, "blit"), dy = A_int(a, n, 2, "blit"), sx = 0, sy = 0, sw = 0, sh = 0, st = 3;
    if (n >= 7 && V_isnum(a[3])) { sx = A_int(a, n, 3, "blit"); sy = A_int(a, n, 4, "blit"); sw = A_int(a, n, 5, "blit"); sh = A_int(a, n, 6, "blit"); st = 7; }
    Opts o = opts_make(a, n, st); bool tr = opt_bool(&o, "transparent", false); Canvas *d = CT(); if (src == d) as_err("blit: source and target are the same canvas"); cv_blit(d, src, sx, sy, sw, sh, dx, dy, tr, tr); return OKV;
}
NAT(d_cell) { BEGIN(2, 3, "cell(x, y [, canvas])"); Canvas *c = n > 2 ? cv_must(A_int(a, n, 2, "cell"), "cell") : CT(); Cell *q = cv_at(c, A_int(a, n, 0, "cell"), A_int(a, n, 1, "cell")); if (!q) return kc_null(); char u[5]; int l = utf8_put(u, q->ch == CH_CONT ? ' ' : q->ch ? q->ch : ' '); u[l] = 0;
    KcValue r = kc_array_new(); kc_array_push(r, kc_string(u)); kc_array_push(r, col_ret(q->fg)); kc_array_push(r, col_ret(q->bg)); return r; }

/* ------------------------------------------------------------ export / import */
static int cmode_of(const char *s) { if (!s || !strcmp(s, "none") || !strcmp(s, "plain")) return 0; if (!strcmp(s, "16") || !strcmp(s, "ansi16")) return 1; if (!strcmp(s, "256") || !strcmp(s, "ansi256")) return 2; if (!strcmp(s, "truecolor") || !strcmp(s, "24bit") || !strcmp(s, "ansi")) return 3; as_err("unknown colour mode \"%s\" (none, ansi16, ansi256, truecolor)", s); }
static Canvas *opt_canvas(KcValue *a, int n, int i, const char *who) { if (i < n && V_isnum(a[i]) && a[i].type != VAL_BOOL) return cv_must(A_int(a, n, i, who), who); term_ensure(); return S_screen; }
NAT(d_dump) { BEGIN(0, 1, "dump([canvas])  -> plain text of the screen/canvas"); Canvas *c = opt_canvas(a, n, 0, "dump"); size_t l; char *t = cv_to_text(c, 0, 0, 0, 0, 0, true, &l); KcValue r = ret_str(t, l); free(t); return r; }
NAT(d_dump_ansi) { BEGIN(0, 2, "dump_ansi([canvas [, mode]])"); Canvas *c = opt_canvas(a, n, 0, "dump_ansi"); int cm = n > 1 ? cmode_of(A_str(a, n, 1, "dump_ansi")) : 3; size_t l; char *t = cv_to_text(c, 0, 0, 0, 0, cm, false, &l); KcValue r = ret_str(t, l); free(t); return r; }
NAT(d_canvas_text) { BEGIN(1, 6, "canvas_text(canvas [, x, y, w, h])"); Canvas *c = cv_must(A_int(a, n, 0, "canvas_text"), "canvas_text"); size_t l; char *t = cv_to_text(c, n > 1 ? A_int(a, n, 1, "canvas_text") : 0, n > 2 ? A_int(a, n, 2, "canvas_text") : 0, n > 3 ? A_int(a, n, 3, "canvas_text") : 0, n > 4 ? A_int(a, n, 4, "canvas_text") : 0, 0, true, &l); KcValue r = ret_str(t, l); free(t); return r; }
static void save_s(const char *who, const char *path, const char *d, size_t l) { if (!as_write_file(path, d, l)) as_err("%s: cannot write '%s'", who, path); }
NAT(d_save_text) { BEGIN(1, 2, "save_text(path [, canvas])"); Canvas *c = opt_canvas(a, n, 1, "save_text"); size_t l; char *t = cv_to_text(c, 0, 0, 0, 0, 0, true, &l); save_s("save_text", A_str(a, n, 0, "save_text"), t, l); free(t); return OKV; }
NAT(d_save_ansi) { BEGIN(1, 3, "save_ansi(path [, canvas [, mode]])"); Canvas *c = opt_canvas(a, n, 1, "save_ansi"); int cm = n > 2 ? cmode_of(A_str(a, n, 2, "save_ansi")) : 3; size_t l; char *t = cv_to_text(c, 0, 0, 0, 0, cm, false, &l); save_s("save_ansi", A_str(a, n, 0, "save_ansi"), t, l); free(t); return OKV; }
NAT(d_save_html) { BEGIN(1, 2, "save_html(path [, canvas])"); Canvas *c = opt_canvas(a, n, 1, "save_html"); size_t l; char *t = cv_to_html(c, &l); save_s("save_html", A_str(a, n, 0, "save_html"), t, l); free(t); return OKV; }
NAT(d_canvas_load) { BEGIN(1, 5, "canvas_load(path [, \"cp437\", true, \"wrap\", 80])  -- plain text / ANSI art"); const char *p = A_str(a, n, 0, "canvas_load"); size_t l; uint8_t *d = as_read_file(p, &l); if (!d) as_err("canvas_load: cannot open '%s'", p); Opts o = opts_make(a, n, 1);
    size_t pl = strlen(p); bool cp = opt_bool(&o, "cp437", pl > 4 && !strcasecmp(p + pl - 4, ".ans")); int id = cv_from_text((const char *)d, l, cp, opt_int(&o, "wrap", cp ? 80 : 0)); free(d); return kc_int(id); }
NAT(d_canvas_from_text) { BEGIN(1, 5, "canvas_from_text(string [, \"cp437\", false, \"wrap\", 0])"); const char *s = A_str(a, n, 0, "canvas_from_text"); Opts o = opts_make(a, n, 1); return kc_int(cv_from_text(s, strlen(s), opt_bool(&o, "cp437", false), opt_int(&o, "wrap", 0))); }

/* ------------------------------------------------------------ hi-res layers */
static Hires *H(KcValue *a, int n, int i, const char *who) { Hires *h = hr_get(A_int(a, n, i, who)); if (!h) as_err("%s: invalid hires layer %d", who, A_int(a, n, i, who)); return h; }
static void hset(Hires *h, int x, int y, Col c) { if (x >= 0 && y >= 0 && x < h->w && y < h->h) h->px[(size_t)y * h->w + x] = c; }
NAT(h_hires) { BEGIN(2, 3, "hires(cell_w, cell_h [, mode])  modes: half (1x2), quad (2x2), braille (2x4)"); HrMode m = HR_BRAILLE; if (n > 2 && !hr_mode_parse(A_str(a, n, 2, "hires"), &m)) as_err("hires: mode must be half, quad or braille"); return kc_int(hr_new(A_int(a, n, 0, "hires"), A_int(a, n, 1, "hires"), m)); }
NAT(h_free) { BEGIN(1, 1, "hires_free(id)"); hr_free(A_int(a, n, 0, "hires_free")); return OKV; }
NAT(h_size) { BEGIN(1, 1, "hires_size(id)  -> [pixel_w, pixel_h]"); Hires *h = H(a, n, 0, "hires_size"); KcValue r = kc_array_new(); kc_array_push(r, kc_int(h->w)); kc_array_push(r, kc_int(h->h)); return r; }
NAT(h_clear) { BEGIN(1, 1, "hires_clear(id)"); Hires *h = H(a, n, 0, "hires_clear"); memset(h->px, 0, (size_t)h->w * h->h * sizeof(Col)); return OKV; }
static Col pcol(KcValue *a, int n, int i, const char *who) { if (i >= n) return col_rgb(235, 235, 235); Col c = col_from_value(a[i], who); return c; }
NAT(h_set) { BEGIN(3, 4, "hires_set(id, x, y [, color])  (color -1 erases)"); hset(H(a, n, 0, "hires_set"), A_int(a, n, 1, "hires_set"), A_int(a, n, 2, "hires_set"), pcol(a, n, 3, "hires_set")); return OKV; }
NAT(h_get) { BEGIN(3, 3, "hires_get(id, x, y)"); Hires *h = H(a, n, 0, "hires_get"); int x = A_int(a, n, 1, "hires_get"), y = A_int(a, n, 2, "hires_get"); if (x < 0 || y < 0 || x >= h->w || y >= h->h) return kc_int(-1); return col_ret(h->px[(size_t)y * h->w + x]); }
NAT(h_line) { BEGIN(5, 6, "hires_line(id, x0, y0, x1, y1 [, color])"); Hires *h = H(a, n, 0, "hires_line"); int x0 = A_int(a, n, 1, "hires_line"), y0 = A_int(a, n, 2, "hires_line"), x1 = A_int(a, n, 3, "hires_line"), y1 = A_int(a, n, 4, "hires_line"); Col c = pcol(a, n, 5, "hires_line");
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1, err = dx + dy; for (int g = 0; g < 200000; g++) { hset(h, x0, y0, c); if (x0 == x1 && y0 == y1) break; int e2 = 2 * err; if (e2 >= dy) { err += dy; x0 += sx; } if (e2 <= dx) { err += dx; y0 += sy; } } return OKV; }
NAT(h_rect) { BEGIN(5, 7, "hires_rect(id, x, y, w, h [, color [, fill]])"); Hires *h = H(a, n, 0, "hires_rect"); int x = A_int(a, n, 1, "hires_rect"), y = A_int(a, n, 2, "hires_rect"), w = A_int(a, n, 3, "hires_rect"), hh = A_int(a, n, 4, "hires_rect"); Col c = pcol(a, n, 5, "hires_rect"); bool fill = n > 6 ? A_bool(a, n, 6, "hires_rect") : false;
    for (int j = y; j < y + hh; j++) for (int i = x; i < x + w; i++) if (fill || i == x || j == y || i == x + w - 1 || j == y + hh - 1) hset(h, i, j, c); return OKV; }
NAT(h_circle) { BEGIN(4, 6, "hires_circle(id, cx, cy, r [, color [, fill]])"); Hires *h = H(a, n, 0, "hires_circle"); int cx = A_int(a, n, 1, "hires_circle"), cy = A_int(a, n, 2, "hires_circle"), r = A_int(a, n, 3, "hires_circle"); Col c = pcol(a, n, 4, "hires_circle"); bool fill = n > 5 ? A_bool(a, n, 5, "hires_circle") : false;
    for (int j = -r; j <= r; j++) for (int i = -r; i <= r; i++) { double d = sqrt((double)(i * i + j * j)); if (fill ? d <= r + 0.3 : fabs(d - r) < 0.7) hset(h, cx + i, cy + j, c); } return OKV; }
NAT(h_draw) { BEGIN(3, 7, "hires_draw(id, x, y [, options])"); Hires *h = H(a, n, 0, "hires_draw"); Opts o = opts_make(a, n, 3); Sty s = sty_from(&o); hr_draw(h, CT(), A_int(a, n, 1, "hires_draw"), A_int(a, n, 2, "hires_draw"), s.bg, s.attr); return OKV; }
NAT(h_plot) { BEGIN(2, 12, "plot(id, values [, \"color\", c, \"min\", lo, \"max\", hi])  draws a line chart of numbers into a hires layer"); Hires *h = H(a, n, 0, "plot"); if (a[1].type != VAL_ARRAY || a[1].as.arr->count < 2) as_err("plot: values must be an array of 2+ numbers"); Opts o = opts_make(a, n, 2);
    int cnt = a[1].as.arr->count; double lo = 1e300, hi = -1e300; for (int i = 0; i < cnt; i++) { double v = V_num(a[1].as.arr->items[i], "plot"); if (v < lo) lo = v; if (v > hi) hi = v; } lo = opt_num(&o, "min", lo); hi = opt_num(&o, "max", hi); if (hi - lo < 1e-12) hi = lo + 1; Col c = opt_col(&o, "color", col_rgb(120, 210, 255)); int px = -1, py = -1;
    for (int i = 0; i < cnt; i++) { double v = V_num(a[1].as.arr->items[i], "plot"); int x = (int)lround((double)i / (cnt - 1) * (h->w - 1)), y = (int)lround((1 - (v - lo) / (hi - lo)) * (h->h - 1)); if (px >= 0) { int dx = abs(x - px), sx = px < x ? 1 : -1, dy = -abs(y - py), sy = py < y ? 1 : -1, err = dx + dy, x0 = px, y0 = py; for (int g = 0; g < 100000; g++) { hset(h, x0, y0, c); if (x0 == x && y0 == y) break; int e2 = 2 * err; if (e2 >= dy) { err += dy; x0 += sx; } if (e2 <= dx) { err += dx; y0 += sy; } } } else hset(h, x, y, c); px = x; py = y; } return OKV; }

KcModuleEntry AS_T_DRAW[] = {
    E("canvas", d_canvas), E("canvas_free", d_canvas_free), E("canvas_size", d_canvas_size), E("canvas_resize", d_canvas_resize), E("canvas_copy", d_canvas_copy), E("target", d_target), E("style", d_style), E("clear", d_clear),
    E("put", d_put), E("text", d_text), E("text_width", d_text_width), E("text_wrap", d_text_wrap), E("fill", d_fill), E("hline", d_hline), E("vline", d_vline), E("box", d_box), E("line", d_line), E("circle", d_circle), E("ellipse", d_ellipse),
    E("triangle", d_triangle), E("flood", d_flood), E("clip", d_clip), E("unclip", d_unclip), E("blit", d_blit), E("cell", d_cell),
    E("dump", d_dump), E("dump_ansi", d_dump_ansi), E("canvas_text", d_canvas_text), E("save_text", d_save_text), E("save_ansi", d_save_ansi), E("save_html", d_save_html), E("canvas_load", d_canvas_load), E("canvas_from_text", d_canvas_from_text),
    E("hires", h_hires), E("hires_free", h_free), E("hires_size", h_size), E("hires_clear", h_clear), E("hires_set", h_set), E("hires_get", h_get), E("hires_line", h_line), E("hires_rect", h_rect), E("hires_circle", h_circle), E("hires_draw", h_draw), E("plot", h_plot),
};
const int AS_N_DRAW = sizeof(AS_T_DRAW) / sizeof(AS_T_DRAW[0]);
