#include "vt.h"
#include <ctype.h>

static const Color ANSI16[16] = { 0x1e222a, 0xe06c75, 0x98c379, 0xe5c07b, 0x61afef, 0xc678dd, 0x56b6c2, 0xc8ccd4,
                                  0x5c6370, 0xff7b86, 0xa9dc76, 0xf2d58a, 0x7ab8ff, 0xd78ae8, 0x6fd0dc, 0xffffff };
static Color color256(int k) {
    if (k < 16) return ANSI16[k];
    if (k >= 232) { int g = 8 + (k - 232) * 10; return (Color)(g << 16 | g << 8 | g); }
    k -= 16; int r = k / 36, g = (k / 6) % 6, b = k % 6;
    return (Color)(((r ? r * 40 + 55 : 0) << 16) | ((g ? g * 40 + 55 : 0) << 8) | (b ? b * 40 + 55 : 0));
}
Color vt_ansi_color(int k) { return color256(k); }

static Cell blank(VTerm *v) { return (Cell){ ' ', VT_DEF, v->bg, 0 }; }
static Cell *row(VTerm *v, int y) { return v->g + (size_t)y * (size_t)v->w; }
static void clear_cells(VTerm *v, int y, int x0, int x1) { Cell b = blank(v); for (int x = x0; x < x1 && x < v->w; x++) row(v, y)[x] = b; }

VTerm *vt_new(int w, int h) {
    VTerm *v = xcalloc(1, sizeof *v); v->w = w; v->h = h; v->main_g = xcalloc((size_t)w * (size_t)h, sizeof(Cell)); v->alt_g = xcalloc((size_t)w * (size_t)h, sizeof(Cell)); v->g = v->main_g;
    v->fg = VT_DEF; v->bg = VT_DEF; v->top = 0; v->bot = h - 1; v->autowrap = true; v->cursor_vis = true; v->sb_cap = 3000; v->sb = xcalloc((size_t)v->sb_cap, sizeof(VRow));
    v->tabs = xcalloc((size_t)w + 1, 1); for (int i = 0; i < w; i += 8) v->tabs[i] = 1;
    for (int y = 0; y < h; y++) clear_cells(v, y, 0, w);
    return v;
}
static void sb_clear(VTerm *v) { for (int i = 0; i < v->sb_cap; i++) { free(v->sb[i].c); v->sb[i] = (VRow){ 0 }; } v->sb_n = v->sb_head = 0; v->view = 0; }
void vt_free(VTerm *v) { if (!v) return; sb_clear(v); free(v->sb); free(v->main_g); free(v->alt_g); free(v->tabs); free(v); }

static void sb_push(VTerm *v, int y) {
    int idx = (v->sb_head + v->sb_n) % v->sb_cap;
    if (v->sb_n == v->sb_cap) { free(v->sb[v->sb_head].c); v->sb_head = (v->sb_head + 1) % v->sb_cap; idx = (v->sb_head + v->sb_n - 1) % v->sb_cap; } else v->sb_n++;
    v->sb[idx].c = xmalloc((size_t)v->w * sizeof(Cell)); memcpy(v->sb[idx].c, row(v, y), (size_t)v->w * sizeof(Cell)); v->sb[idx].w = v->w;
    if (v->view > 0) { v->view++; if (v->view > v->sb_n) v->view = v->sb_n; }
}
static void scroll_up(VTerm *v, int n) {
    if (n > v->bot - v->top + 1) n = v->bot - v->top + 1;
    for (int i = 0; i < n; i++) {
        if (!v->alt_active && v->top == 0) sb_push(v, 0);
        for (int y = v->top; y < v->bot; y++) memcpy(row(v, y), row(v, y + 1), (size_t)v->w * sizeof(Cell));
        clear_cells(v, v->bot, 0, v->w);
    }
}
static void scroll_down(VTerm *v, int n) {
    if (n > v->bot - v->top + 1) n = v->bot - v->top + 1;
    for (int i = 0; i < n; i++) { for (int y = v->bot; y > v->top; y--) memcpy(row(v, y), row(v, y - 1), (size_t)v->w * sizeof(Cell)); clear_cells(v, v->top, 0, v->w); }
}
static void linefeed(VTerm *v) { if (v->cy == v->bot) scroll_up(v, 1); else if (v->cy < v->h - 1) v->cy++; }
static void revindex(VTerm *v) { if (v->cy == v->top) scroll_down(v, 1); else if (v->cy > 0) v->cy--; }

void vt_resize(VTerm *v, int w, int h) {
    if (w < 2) w = 2; if (h < 2) h = 2; if (w == v->w && h == v->h) return;
    Cell *nm = xcalloc((size_t)w * (size_t)h, sizeof(Cell)), *na = xcalloc((size_t)w * (size_t)h, sizeof(Cell));
    Cell b = { ' ', VT_DEF, VT_DEF, 0 }; for (int i = 0; i < w * h; i++) nm[i] = na[i] = b;
    int shift = 0;                                                         /* keep the bottom rows when shrinking vertically */
    if (v->cy >= h) shift = v->cy - h + 1;
    for (int y = 0; y < h; y++) { int sy = y + shift; if (sy >= v->h) break; for (int x = 0; x < w && x < v->w; x++) { nm[y * w + x] = v->main_g[sy * v->w + x]; if (y + shift < v->h) na[y * w + x] = v->alt_g[sy * v->w + x]; } }
    free(v->main_g); free(v->alt_g); v->main_g = nm; v->alt_g = na; v->g = v->alt_active ? na : nm;
    v->cy -= shift; if (v->cy < 0) v->cy = 0; if (v->cx >= w) v->cx = w - 1; if (v->cy >= h) v->cy = h - 1;
    v->w = w; v->h = h; v->top = 0; v->bot = h - 1; v->wrap_pending = false; v->view = 0;
    free(v->tabs); v->tabs = xcalloc((size_t)w + 1, 1); for (int i = 0; i < w; i += 8) v->tabs[i] = 1;
}

const Cell *vt_cell(VTerm *v, int x, int y) {
    static const Cell none = { ' ', VT_DEF, VT_DEF, 0 };
    if (x < 0 || y < 0 || x >= v->w || y >= v->h) return &none;
    if (v->view > 0 && !v->alt_active) {
        int sy = y - v->view;                                              /* negative: inside the scrollback */
        if (sy < 0) { int k = v->sb_n + sy; if (k < 0) return &none; VRow *r = &v->sb[(v->sb_head + k) % v->sb_cap]; return x < r->w ? &r->c[x] : &none; }
        return &row(v, sy)[x];
    }
    return &row(v, y)[x];
}
void vt_scroll_view(VTerm *v, int d) { if (v->alt_active) return; v->view += d; if (v->view < 0) v->view = 0; if (v->view > v->sb_n) v->view = v->sb_n; }

static void put(VTerm *v, uint32_t cp) {
    int w = cp_width(cp); if (w == 0) return;
    if (cp < 32) return;
    if (v->wrap_pending && v->autowrap) { v->cx = 0; linefeed(v); }
    v->wrap_pending = false;
    if (w == 2 && v->cx == v->w - 1) { if (v->autowrap) { row(v, v->cy)[v->cx] = blank(v); v->cx = 0; linefeed(v); } else return; }
    Cell *c = &row(v, v->cy)[v->cx]; *c = (Cell){ cp, v->fg, v->bg, v->attr };
    if (w == 2 && v->cx + 1 < v->w) row(v, v->cy)[v->cx + 1] = (Cell){ 0, v->fg, v->bg, v->attr };
    v->cx += w; if (v->cx >= v->w) { v->cx = v->w - 1; v->wrap_pending = true; }
}

static void sgr(VTerm *v, int *p, int n) {
    if (n == 0) { p[0] = 0; n = 1; }
    for (int i = 0; i < n; i++) {
        int c = p[i];
        if (c == 0) { v->fg = VT_DEF; v->bg = VT_DEF; v->attr = 0; }
        else if (c == 1) v->attr |= ATTR_BOLD; else if (c == 2) v->attr |= ATTR_DIM; else if (c == 3) v->attr |= ATTR_ITALIC; else if (c == 4) v->attr |= ATTR_UNDERLINE; else if (c == 7) v->attr |= ATTR_REVERSE;
        else if (c == 22) v->attr &= (uint8_t) ~(ATTR_BOLD | ATTR_DIM); else if (c == 23) v->attr &= (uint8_t)~ATTR_ITALIC; else if (c == 24) v->attr &= (uint8_t)~ATTR_UNDERLINE; else if (c == 27) v->attr &= (uint8_t)~ATTR_REVERSE;
        else if (c >= 30 && c <= 37) v->fg = color256(c - 30); else if (c == 39) v->fg = VT_DEF; else if (c >= 90 && c <= 97) v->fg = color256(c - 90 + 8);
        else if (c >= 40 && c <= 47) v->bg = color256(c - 40); else if (c == 49) v->bg = VT_DEF; else if (c >= 100 && c <= 107) v->bg = color256(c - 100 + 8);
        else if ((c == 38 || c == 48) && i + 1 < n) {
            Color col = VT_DEF; bool ok = false;
            if (p[i + 1] == 5 && i + 2 < n) { col = color256(p[i + 2] & 255); i += 2; ok = true; }
            else if (p[i + 1] == 2 && i + 4 < n) { col = (Color)(((p[i + 2] & 255) << 16) | ((p[i + 3] & 255) << 8) | (p[i + 4] & 255)); i += 4; ok = true; }
            if (ok) { if (c == 38) v->fg = col; else v->bg = col; }
        }
    }
}
static void reply(VTerm *v, const char *s) { if (v->reply) v->reply(v->rctx, s, (int)strlen(s)); }
static void switch_alt(VTerm *v, bool on, bool save) {
    if (on == v->alt_active) return;
    if (on) { if (save) { v->scx = v->cx; v->scy = v->cy; v->sfg = v->fg; v->sbg = v->bg; v->sattr = v->attr; } v->alt_active = true; v->g = v->alt_g; Cell b = blank(v); for (int i = 0; i < v->w * v->h; i++) v->alt_g[i] = b; v->view = 0; }
    else { v->alt_active = false; v->g = v->main_g; if (save) { v->cx = v->scx; v->cy = v->scy; v->fg = v->sfg; v->bg = v->sbg; v->attr = v->sattr; } }
    v->top = 0; v->bot = v->h - 1;
}
static void reset(VTerm *v) {
    v->fg = VT_DEF; v->bg = VT_DEF; v->attr = 0; v->cx = v->cy = 0; v->top = 0; v->bot = v->h - 1; v->autowrap = true; v->cursor_vis = true; v->appkeys = v->bracketed = false; v->wrap_pending = false;
    switch_alt(v, false, false); for (int y = 0; y < v->h; y++) clear_cells(v, y, 0, v->w);
}

static void csi(VTerm *v, char f) {
    int p[16] = { 0 }; int n = 0; bool priv = false, gt = false; const char *s = v->pbuf; bool have = false;
    if (*s == '?') { priv = true; s++; } else if (*s == '>') { gt = true; s++; } else if (*s == '=' || *s == '<') s++;
    for (; *s; s++) { if (isdigit((unsigned char)*s)) { if (n < 16) p[n] = p[n] * 10 + (*s - '0'); have = true; } else if (*s == ';') { if (n < 15) n++; have = false; } }
    int cnt = (have || n > 0) ? n + 1 : 0; (void)cnt; if (have || n > 0) n++;
    int a = n > 0 && p[0] > 0 ? p[0] : 1, b = n > 1 && p[1] > 0 ? p[1] : 1;
    v->wrap_pending = false;
    switch (f) {
    case 'A': v->cy -= a; if (v->cy < v->top && v->cy + a >= v->top) v->cy = v->top; if (v->cy < 0) v->cy = 0; break;
    case 'B': case 'e': v->cy += a; if (v->cy > v->h - 1) v->cy = v->h - 1; break;
    case 'C': case 'a': v->cx += a; if (v->cx > v->w - 1) v->cx = v->w - 1; break;
    case 'D': v->cx -= a; if (v->cx < 0) v->cx = 0; break;
    case 'E': v->cy += a; if (v->cy > v->h - 1) v->cy = v->h - 1; v->cx = 0; break;
    case 'F': v->cy -= a; if (v->cy < 0) v->cy = 0; v->cx = 0; break;
    case 'G': case '`': v->cx = a - 1; if (v->cx > v->w - 1) v->cx = v->w - 1; break;
    case 'd': v->cy = a - 1; if (v->cy > v->h - 1) v->cy = v->h - 1; break;
    case 'H': case 'f': v->cy = a - 1; v->cx = b - 1; if (v->cy > v->h - 1) v->cy = v->h - 1; if (v->cx > v->w - 1) v->cx = v->w - 1; break;
    case 'J': { int m = n > 0 ? p[0] : 0;
        if (m == 0) { clear_cells(v, v->cy, v->cx, v->w); for (int y = v->cy + 1; y < v->h; y++) clear_cells(v, y, 0, v->w); }
        else if (m == 1) { for (int y = 0; y < v->cy; y++) clear_cells(v, y, 0, v->w); clear_cells(v, v->cy, 0, v->cx + 1); }
        else if (m == 2 || m == 3) { if (m == 3) sb_clear(v); else for (int y = 0; y < v->h; y++) clear_cells(v, y, 0, v->w); }
        break; }
    case 'K': { int m = n > 0 ? p[0] : 0; if (m == 0) clear_cells(v, v->cy, v->cx, v->w); else if (m == 1) clear_cells(v, v->cy, 0, v->cx + 1); else clear_cells(v, v->cy, 0, v->w); break; }
    case 'L': if (v->cy >= v->top && v->cy <= v->bot) { int t = v->top; v->top = v->cy; scroll_down(v, a); v->top = t; } break;
    case 'M': if (v->cy >= v->top && v->cy <= v->bot) { int t = v->top; bool alt = v->alt_active; v->top = v->cy; v->alt_active = true; scroll_up(v, a); v->alt_active = alt; v->top = t; } break;
    case 'P': { Cell *r = row(v, v->cy); int k = a; if (k > v->w - v->cx) k = v->w - v->cx; memmove(r + v->cx, r + v->cx + k, (size_t)(v->w - v->cx - k) * sizeof(Cell)); clear_cells(v, v->cy, v->w - k, v->w); break; }
    case '@': { Cell *r = row(v, v->cy); int k = a; if (k > v->w - v->cx) k = v->w - v->cx; memmove(r + v->cx + k, r + v->cx, (size_t)(v->w - v->cx - k) * sizeof(Cell)); clear_cells(v, v->cy, v->cx, v->cx + k); break; }
    case 'X': clear_cells(v, v->cy, v->cx, v->cx + a); break;
    case 'S': { bool alt = v->alt_active; scroll_up(v, a); (void)alt; break; }
    case 'T': scroll_down(v, a); break;
    case 'r': { int t = n > 0 && p[0] > 0 ? p[0] : 1, bt = n > 1 && p[1] > 0 ? p[1] : v->h; if (t < bt && bt <= v->h) { v->top = t - 1; v->bot = bt - 1; v->cx = 0; v->cy = 0; } break; }
    case 'm': if (!priv && !gt) sgr(v, p, n); break;
    case 's': v->scx = v->cx; v->scy = v->cy; v->sfg = v->fg; v->sbg = v->bg; v->sattr = v->attr; break;
    case 'u': v->cx = v->scx; v->cy = v->scy; break;
    case 'h': case 'l': {
        bool on = f == 'h';
        for (int i = 0; i < n; i++) if (priv) switch (p[i]) {
            case 1: v->appkeys = on; break; case 7: v->autowrap = on; break; case 25: v->cursor_vis = on; break;
            case 47: case 1047: switch_alt(v, on, false); break; case 1049: switch_alt(v, on, true); break; case 2004: v->bracketed = on; break;
            default: break; }
        break; }
    case 'n': if (!priv) { if (n > 0 && p[0] == 5) reply(v, "\x1b[0n"); else if (n > 0 && p[0] == 6) { char b2[32]; snprintf(b2, sizeof b2, "\x1b[%d;%dR", v->cy + 1, v->cx + 1); reply(v, b2); } } break;
    case 'c': if (gt) reply(v, "\x1b[>0;10;1c"); else if (!priv) reply(v, "\x1b[?62;c"); break;
    case 'p': if (!priv && v->pbuf[0] == '!') reset(v); break;
    default: break;
    }
}

void vt_feed(VTerm *v, const char *d, int n) {
    v->gen++;
    if (v->view) v->view = 0;
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)d[i];
        switch (v->st) {
        case 1:                                                                   /* after ESC */
            v->st = 0;
            switch (c) {
            case '[': v->st = 2; v->np = 0; v->pbuf[0] = 0; break;
            case ']': v->st = 3; v->np = 0; v->pbuf[0] = 0; break;
            case 'P': case '^': case '_': case 'X': v->st = 3; v->np = 0; break;     /* DCS/PM/APC/SOS: swallowed like OSC */
            case '(': case ')': case '*': case '+': case '#': case '%': v->st = 4; break;
            case '7': v->scx = v->cx; v->scy = v->cy; v->sfg = v->fg; v->sbg = v->bg; v->sattr = v->attr; break;
            case '8': v->cx = v->scx; v->cy = v->scy; v->wrap_pending = false; break;
            case 'D': linefeed(v); break; case 'E': v->cx = 0; linefeed(v); break; case 'M': revindex(v); break;
            case 'c': reset(v); break;
            case 'H': if (v->cx < v->w) v->tabs[v->cx] = 1; break;
            default: break;
            }
            continue;
        case 2:                                                                   /* CSI */
            if (c >= 0x40 && c <= 0x7e) { v->pbuf[v->np] = 0; csi(v, (char)c); v->st = 0; }
            else if (c == 0x1b) { v->st = 1; }
            else if (c >= 0x20 && v->np < (int)sizeof v->pbuf - 1) v->pbuf[v->np++] = (char)c;
            continue;
        case 3:                                                                   /* OSC / DCS string */
            if (c == 7) { v->pbuf[v->np] = 0; if (!strncmp(v->pbuf, "0;", 2) || !strncmp(v->pbuf, "2;", 2)) snprintf(v->title, sizeof v->title, "%s", v->pbuf + 2); v->st = 0; }
            else if (c == 0x1b) { v->pbuf[v->np] = 0; if (!strncmp(v->pbuf, "0;", 2) || !strncmp(v->pbuf, "2;", 2)) snprintf(v->title, sizeof v->title, "%s", v->pbuf + 2); v->st = 5; }
            else if (v->np < (int)sizeof v->pbuf - 1) v->pbuf[v->np++] = (char)c;
            continue;
        case 5: v->st = (c == '\\') ? 0 : 1; if (c != '\\') i--; continue;           /* ESC inside OSC: ST ends it */
        case 4: v->st = 0; continue;
        default: break;
        }
        if (c == 0x1b) { v->st = 1; v->cpn = 0; continue; }
        if (c < 0x20) {
            v->cpn = 0;
            switch (c) {
            case '\n': case 0x0b: case 0x0c: v->wrap_pending = false; linefeed(v); break;
            case '\r': v->cx = 0; v->wrap_pending = false; break;
            case '\b': if (v->cx > 0) v->cx--; v->wrap_pending = false; break;
            case '\t': { int x = v->cx + 1; while (x < v->w - 1 && !v->tabs[x]) x++; v->cx = x < v->w ? x : v->w - 1; v->wrap_pending = false; break; }
            default: break;
            }
            continue;
        }
        if (c < 0x80) { put(v, c); continue; }
        /* UTF-8 */
        static unsigned char ub[4]; static int un, uneed;
        if (c >= 0xC0) { ub[0] = c; un = 1; uneed = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2; }
        else if (un > 0 && un < 4) { ub[un++] = c; }
        if (un > 0 && un == uneed) { uint32_t cp; utf8_decode((const char *)ub, (size_t)un, &cp); un = 0; put(v, cp); }
    }
}

/* ---------------------------------------------------------------- keys */
int vt_encode_key(const Event *e, bool app, char *o) {
    int k = e->key, m = e->mods; int mod = 1 + ((m & MOD_SHIFT) ? 1 : 0) + ((m & MOD_ALT) ? 2 : 0) + ((m & MOD_CTRL) ? 4 : 0);
    char *p = o;
    #define CSI1(ch) do { if (mod > 1) p += sprintf(p, "\x1b[1;%d%c", mod, ch); else p += sprintf(p, app ? "\x1bO%c" : "\x1b[%c", ch); return (int)(p - o); } while (0)
    #define TILDE(n) do { if (mod > 1) p += sprintf(p, "\x1b[%d;%d~", n, mod); else p += sprintf(p, "\x1b[%d~", n); return (int)(p - o); } while (0)
    switch (k) {
    case K_UP: CSI1('A'); case K_DOWN: CSI1('B'); case K_RIGHT: CSI1('C'); case K_LEFT: CSI1('D'); case K_HOME: CSI1('H'); case K_END: CSI1('F');
    case K_INS: TILDE(2); case K_DEL: TILDE(3); case K_PGUP: TILDE(5); case K_PGDN: TILDE(6);
    case K_ENTER: if (m & MOD_ALT) *p++ = 0x1b; *p++ = '\r'; return (int)(p - o);
    case K_TAB: if (m & MOD_SHIFT) { p += sprintf(p, "\x1b[Z"); } else *p++ = '\t'; return (int)(p - o);
    case K_ESC: *p++ = 0x1b; return 1;
    case K_BKSP: if (m & MOD_ALT) *p++ = 0x1b; *p++ = (m & MOD_CTRL) ? 0x08 : 0x7f; return (int)(p - o);
    default: break;
    }
    if (k >= K_FN(1) && k <= K_FN(12)) {
        int n = k - K_FN(1) + 1;
        if (n <= 4) { if (mod > 1) p += sprintf(p, "\x1b[1;%d%c", mod, 'P' + n - 1); else p += sprintf(p, "\x1bO%c", 'P' + n - 1); return (int)(p - o); }
        static const int code[] = { 15, 17, 18, 19, 20, 21, 23, 24 }; TILDE(code[n - 5]);
    }
    if (k > 0 && k < 0x200000) {
        if (m & MOD_ALT) *p++ = 0x1b;
        if ((m & MOD_CTRL) && k >= 'a' && k <= 'z') { *p++ = (char)(k - 'a' + 1); return (int)(p - o); }
        if ((m & MOD_CTRL) && k == ' ') { *p++ = 0; return (int)(p - o); }
        if ((m & MOD_CTRL) && k == '/') { *p++ = 0x1f; return (int)(p - o); }
        if ((m & MOD_CTRL) && k == '\\') { *p++ = 0x1c; return (int)(p - o); }
        if ((m & MOD_CTRL) && k == ']') { *p++ = 0x1d; return (int)(p - o); }
        p += utf8_encode((uint32_t)k, p); return (int)(p - o);
    }
    return 0;
}
