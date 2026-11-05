/* KingCobra std.gui — a retained-mode GUI toolkit with a software renderer.
 *
 * Every widget (labels, buttons, text boxes, sliders, lists, tabs, canvases,
 * dialogs...) is laid out and rasterised by this module into a plain pixel
 * buffer using a built-in bitmap font, so look and behaviour are identical
 * on every platform. Only the last step — showing the pixel buffer in a
 * window and receiving input — is platform-specific:
 *
 *   Linux   : Xlib (compiled in when <X11/Xlib.h> is available)
 *   Windows : Win32/GDI  (written against the documented API; not yet
 *             exercised on a real Windows machine — see docs/STD_GUI.md)
 *   Headless: no window; renders to memory. Used automatically when no
 *             display exists (or KC_GUI_HEADLESS=1) and powers
 *             gui.screenshot() + the gui.click/send_key test helpers.
 *
 * Model: one window, a tree of widgets referenced by integer ids (0 is the
 * window's root panel). Layout is automatic (vbox / hbox / grid) with
 * absolute placement available. Callbacks are ordinary KingCobra functions.
 *
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com>
 */
#include "interp.h"
#include "gui_font.h"
#include <math.h>
#include <time.h>
#include <ctype.h>

/* Several one-line `if (a) x; if (b) y;` helpers are deliberate compact style. */
#if defined(__GNUC__)
#  pragma GCC diagnostic ignored "-Wmisleading-indentation"
#endif

#if defined(_WIN32)
#  define KC_GUI_WIN32 1
#  include <windows.h>
#elif defined(KC_GUI_X11)
#  include <X11/Xlib.h>
#  include <X11/Xutil.h>
#  include <X11/keysym.h>
#  include <unistd.h>
#  include <sys/select.h>
#else
#  include <unistd.h>
#endif

/* ================================================================ types */

typedef enum { W_DEAD, W_PANEL, W_LABEL, W_BUTTON, W_CHECK, W_RADIO, W_TEXT, W_SLIDER, W_PROGRESS,
               W_LIST, W_DROPDOWN, W_TABS, W_CANVAS, W_SEP, W_SPACER } WKind;
enum { L_VBOX, L_HBOX, L_GRID, L_ABS };
enum { EV_CLICK, EV_CHANGE, EV_SELECT, EV_SUBMIT, EV_ACTIVATE, EV_DRAW, EV_KEY, EV_MOUSE_DOWN,
       EV_MOUSE_UP, EV_MOUSE_MOVE, EV_WHEEL, EV_RESIZE, EV_CLOSE, EV_COUNT };
static const char *EV_NAMES[EV_COUNT] = { "click", "change", "select", "submit", "activate", "draw", "key",
                                          "mouse_down", "mouse_up", "mouse_move", "wheel", "resize", "close" };
enum { CROSS_AUTO = -1, CROSS_START = 0, CROSS_CENTER = 1, CROSS_END = 2, CROSS_FILL = 3 };
enum { M_SHIFT = 1, M_CTRL = 2, M_ALT = 4 };

typedef struct Widget {
    int id, kind, parent;
    char *text, *placeholder, *group;
    int x, y, w, h;               /* absolute rect, set by layout */
    int pw, ph;                   /* measured preferred size */
    int req_w, req_h;             /* explicit size (0 = automatic) */
    int ax, ay;                   /* position for absolute layout */
    int weight;                   /* share of leftover space on the main axis */
    int layout, pad, spacing, cols;
    int fs;                       /* font-scale override (0 = global) */
    int cross;                    /* cross-axis alignment override */
    int talign;                   /* text alignment: 0 left 1 center 2 right */
    bool visible, enabled, checked, password, readonly, hover, pressed;
    double val, min, max, step;
    char **items; int nitems; int sel, scroll;
    int cur, anchor, hscroll;     /* text box caret / selection anchor / pixel scroll */
    bool has_fg, has_bg; uint32_t fg, bg;
    KcValue cb[EV_COUNT]; bool has_cb[EV_COUNT];
} Widget;

typedef struct { uint32_t bg, panel, text, dim, accent, accent_text, border, field, hover, press, track, sel, shade; } Theme;
static const Theme THEME_DARK  = { 0x1E1F26, 0x262833, 0xE6E8EE, 0x8A8FA3, 0x4C8DFF, 0xFFFFFF, 0x3A3D4D, 0x16171D, 0x323650, 0x232538, 0x3A3D4D, 0x2F5FB8, 0x000000 };
static const Theme THEME_LIGHT = { 0xF2F3F7, 0xFFFFFF, 0x1C1D24, 0x777B8C, 0x2563EB, 0xFFFFFF, 0xC6C9D6, 0xFFFFFF, 0xE4E7F2, 0xD2D6E8, 0xCFD3E1, 0xA9C4F5, 0x000000 };

typedef struct { int type, x, y, button, key, mods; char text[8]; } Ev;
enum { E_NONE, E_MOVE, E_DOWN, E_UP, E_WHEEL, E_KEY, E_CHAR, E_RESIZE, E_CLOSE, E_EXPOSE };
enum { K_NONE, K_ENTER, K_ESC, K_TAB, K_BACKSPACE, K_DELETE, K_LEFT, K_RIGHT, K_UP, K_DOWN, K_HOME, K_END,
       K_PGUP, K_PGDN, K_F1 = 20, K_CTRL_BASE = 256 };

typedef struct { int id; long interval, next; bool repeat, active; KcValue cb; } Timer;

typedef struct {
    bool active; char *title, *text; char **btn; int nbtn, sel; KcValue cb; bool has_cb, as_bool;
    int x, y, w, h, bx0, by, bw, bh, tx, ty, tw; char **lines; int nlines;
} Dialog;

static struct {
    KcInterp *ip; KcPos pos;
    Widget **ws; int nws, cap;
    char *title; int win_w, win_h; bool created;
    uint32_t *fb; int fb_w, fb_h;
    Theme th; int fs;
    bool headless, backend_open, quit, dirty;
    int focus, hover, capture, popup;
    Dialog dlg;
    Timer *timers; int ntimers, timer_cap, next_timer;
    long vclock;                              /* virtual clock for headless mode (ms) */
    long last_click_ms; int last_click_id, last_click_idx;
    char *clip;
    int mx, my;
    long blink_epoch; bool blink_on;
    int cx0, cy0, cx1, cy1;                   /* current clip rectangle */
    int ox, oy; int draw_id;                  /* canvas drawing context (draw_id < 0 outside draw callbacks) */
} G = { .th = {0}, .fs = 2, .focus = -1, .hover = -1, .capture = -1, .popup = -1, .draw_id = -1, .next_timer = 1 };

/* ======================================================== small helpers */

static char *sdup(const char *s) { return kc_strdup(s ? s : ""); }
static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }
static int iclamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static int u8_next(const char *s, int i) { int n = (int)strlen(s); if (i >= n) return n; i++; while (i < n && ((unsigned char)s[i] & 0xC0) == 0x80) i++; return i; }
static int u8_prev(const char *s, int i) { if (i <= 0) return 0; i--; while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) i--; return i; }
static int u8_decode(const char *s, int *adv) {
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80) { *adv = 1; return c; }
    int l = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1; int cp = l == 4 ? c & 7 : l == 3 ? c & 15 : l == 2 ? c & 31 : c;
    int i = 1; for (; i < l && ((unsigned char)s[i] & 0xC0) == 0x80; i++) cp = (cp << 6) | ((unsigned char)s[i] & 63);
    *adv = i; return cp;
}
static int u8_count(const char *s, int bytes) { int n = 0; for (int i = 0; i < bytes && s[i]; i = u8_next(s, i)) n++; return n; }

static long now_ms(void) {
    if (G.headless) return G.vclock;
#if defined(KC_GUI_WIN32)
    return (long)GetTickCount64();
#else
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (long)(ts.tv_sec * 1000L + ts.tv_nsec / 1000000L);
#endif
}
static void sleep_ms(int ms) {
#if defined(KC_GUI_WIN32)
    Sleep((DWORD)ms);
#else
    struct timespec ts = { 0, (long)ms * 1000000L }; nanosleep(&ts, NULL);
#endif
}

static Widget *wget(int id) { return (id >= 0 && id < G.nws && G.ws[id]->kind != W_DEAD) ? G.ws[id] : NULL; }
static int wfs(const Widget *w) { return w->fs > 0 ? w->fs : G.fs; }
static void mark_dirty(void) { G.dirty = true; }

/* ================================================== pixel / text drawing */

static void fb_clip_reset(void) { G.cx0 = 0; G.cy0 = 0; G.cx1 = G.fb_w; G.cy1 = G.fb_h; }

static void fb_rect(int x, int y, int w, int h, uint32_t c) {
    int x0 = imax(x, G.cx0), y0 = imax(y, G.cy0), x1 = imin(x + w, G.cx1), y1 = imin(y + h, G.cy1);
    if (!G.fb || x0 >= x1 || y0 >= y1) return;
    for (int yy = y0; yy < y1; yy++) { uint32_t *row = G.fb + (size_t)yy * (size_t)G.fb_w; for (int xx = x0; xx < x1; xx++) row[xx] = c; }
}
static void fb_px(int x, int y, uint32_t c) {
    if (G.fb && x >= G.cx0 && x < G.cx1 && y >= G.cy0 && y < G.cy1) G.fb[(size_t)y * (size_t)G.fb_w + (size_t)x] = c;
}
static uint32_t blend(uint32_t dst, uint32_t src, int a /*0..255*/) {
    uint32_t r = (((dst >> 16) & 255) * (uint32_t)(255 - a) + ((src >> 16) & 255) * (uint32_t)a) / 255;
    uint32_t g = (((dst >> 8) & 255) * (uint32_t)(255 - a) + ((src >> 8) & 255) * (uint32_t)a) / 255;
    uint32_t b = ((dst & 255) * (uint32_t)(255 - a) + (src & 255) * (uint32_t)a) / 255;
    return (r << 16) | (g << 8) | b;
}
static void fb_rect_alpha(int x, int y, int w, int h, uint32_t c, int a) {
    int x0 = imax(x, G.cx0), y0 = imax(y, G.cy0), x1 = imin(x + w, G.cx1), y1 = imin(y + h, G.cy1);
    if (!G.fb) return;
    for (int yy = y0; yy < y1; yy++) for (int xx = x0; xx < x1; xx++) {
        uint32_t *p = &G.fb[(size_t)yy * (size_t)G.fb_w + (size_t)xx]; *p = blend(*p, c, a);
    }
}
static void fb_frame(int x, int y, int w, int h, uint32_t c) {
    fb_rect(x, y, w, 1, c); fb_rect(x, y + h - 1, w, 1, c); fb_rect(x, y, 1, h, c); fb_rect(x + w - 1, y, 1, h, c);
}
/* filled rounded rectangle (r in pixels) */
static void fb_round(int x, int y, int w, int h, int r, uint32_t c) {
    if (r * 2 > w) r = w / 2; if (r * 2 > h) r = h / 2;
    if (r <= 0) { fb_rect(x, y, w, h, c); return; }
    for (int dy = 0; dy < h; dy++) {
        int d = dy < r ? r - dy : (dy >= h - r ? dy - (h - r - 1) : 0);
        int inset = 0;
        if (d > 0) { double t = (double)r * r - (double)(d - 0.5) * (d - 0.5); inset = r - (int)(sqrt(t > 0 ? t : 0) + 0.5); if (inset < 0) inset = 0; }
        fb_rect(x + inset, y + dy, w - 2 * inset, 1, c);
    }
}
static void fb_round_border(int x, int y, int w, int h, int r, uint32_t fill, uint32_t border) {
    fb_round(x, y, w, h, r, border); fb_round(x + 1, y + 1, w - 2, h - 2, imax(r - 1, 0), fill);
}
static void fb_line(int x0, int y0, int x1, int y1, uint32_t c, int th) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1, err = dx + dy;
    if (th < 1) th = 1;
    for (;;) {
        if (th == 1) fb_px(x0, y0, c); else fb_rect(x0 - th / 2, y0 - th / 2, th, th, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}
static void fb_circle(int cx, int cy, int r, uint32_t c, bool fill) {
    if (r < 0) return;
    for (int y = -r; y <= r; y++) for (int x = -r; x <= r; x++) {
        int d2 = x * x + y * y;
        if (fill ? d2 <= r * r + r : (d2 <= r * r + r && d2 >= (r - 1) * (r - 1) + (r - 1))) fb_px(cx + x, cy + y, c);
    }
}
static void fb_triangle(int x0, int y0, int x1, int y1, int x2, int y2, uint32_t c, bool fill) {
    if (!fill) { fb_line(x0, y0, x1, y1, c, 1); fb_line(x1, y1, x2, y2, c, 1); fb_line(x2, y2, x0, y0, c, 1); return; }
    int minx = imin(x0, imin(x1, x2)), maxx = imax(x0, imax(x1, x2)), miny = imin(y0, imin(y1, y2)), maxy = imax(y0, imax(y1, y2));
    long area = (long)(x1 - x0) * (y2 - y0) - (long)(x2 - x0) * (y1 - y0);
    if (area == 0) return;
    for (int y = miny; y <= maxy; y++) for (int x = minx; x <= maxx; x++) {
        long w0 = (long)(x1 - x0) * (y - y0) - (long)(x - x0) * (y1 - y0);
        long w1 = (long)(x2 - x1) * (y - y1) - (long)(x - x1) * (y2 - y1);
        long w2 = (long)(x0 - x2) * (y - y2) - (long)(x - x2) * (y0 - y2);
        if (area > 0 ? (w0 >= 0 && w1 >= 0 && w2 >= 0) : (w0 <= 0 && w1 <= 0 && w2 <= 0)) fb_px(x, y, c);
    }
}
static void fb_glyph(int x, int y, int cp, uint32_t c, int s) {
    if (cp < 32 || cp > 126) cp = '?';
    const uint8_t *g = KC_FONT8[cp - 32];
    for (int r = 0; r < 8; r++) for (int col = 0; col < 8; col++) if ((g[r] >> col) & 1) fb_rect(x + col * s, y + r * s, s, s, c);
}
static int text_w(const char *s, int fs) {            /* width of the widest line */
    int best = 0, cur = 0;
    for (const char *p = s; *p; p = p + u8_next(p, 0)) { if (*p == '\n') { best = imax(best, cur); cur = 0; } else cur++; }
    return imax(best, cur) * 8 * fs;
}
static int text_lines(const char *s) { int n = 1; for (; *s; s++) if (*s == '\n') n++; return n; }
static int line_h(int fs) { return 8 * fs + 2 * fs; }
static void fb_text(int x, int y, const char *s, uint32_t c, int fs) {
    int cx = x, cy = y;
    for (int i = 0; s[i];) {
        if (s[i] == '\n') { cx = x; cy += line_h(fs); i++; continue; }
        int adv; int cp = u8_decode(s + i, &adv); i += adv;
        fb_glyph(cx, cy, cp, c, fs); cx += 8 * fs;
    }
}
/* draws text clipped to [x,x+w) horizontally */
static void fb_text_clip(int x, int y, int w, int h, const char *s, uint32_t c, int fs) {
    int s0x = G.cx0, s0y = G.cy0, s1x = G.cx1, s1y = G.cy1;
    G.cx0 = imax(G.cx0, x); G.cy0 = imax(G.cy0, y); G.cx1 = imin(G.cx1, x + w); G.cy1 = imin(G.cy1, y + h);
    fb_text(x, y, s, c, fs);
    G.cx0 = s0x; G.cy0 = s0y; G.cx1 = s1x; G.cy1 = s1y;
}

/* ============================================================== backends */
/* Contract: be_open / be_close / be_present / be_poll (non-blocking, one
 * event per call) / be_set_title / be_idle_wait. Headless implements all of
 * them as no-ops. */

#if defined(KC_GUI_X11)

static Display *x_dpy; static Window x_win; static GC x_gc; static Atom x_wm_delete; static int x_scr;
static int x_last_w, x_last_h;

static bool be_open(const char *title, int w, int h) {
    x_dpy = XOpenDisplay(NULL);
    if (!x_dpy) return false;
    x_scr = DefaultScreen(x_dpy);
    x_win = XCreateSimpleWindow(x_dpy, RootWindow(x_dpy, x_scr), 0, 0, (unsigned)w, (unsigned)h, 0, 0, G.th.bg);
    XStoreName(x_dpy, x_win, title);
    XSelectInput(x_dpy, x_win, ExposureMask | KeyPressMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | StructureNotifyMask);
    x_wm_delete = XInternAtom(x_dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(x_dpy, x_win, &x_wm_delete, 1);
    XSizeHints *sh = XAllocSizeHints();
    if (sh) { sh->flags = PMinSize; sh->min_width = 240; sh->min_height = 160; XSetWMNormalHints(x_dpy, x_win, sh); XFree(sh); }
    x_gc = XCreateGC(x_dpy, x_win, 0, NULL);
    XMapWindow(x_dpy, x_win);
    XFlush(x_dpy);
    x_last_w = w; x_last_h = h;
    return true;
}
static void be_close(void) { if (x_dpy) { XFreeGC(x_dpy, x_gc); XDestroyWindow(x_dpy, x_win); XCloseDisplay(x_dpy); x_dpy = NULL; } }
static void be_set_title(const char *t) { if (x_dpy) { XStoreName(x_dpy, x_win, t); XFlush(x_dpy); } }
static void be_present(void) {
    if (!x_dpy || !G.fb) return;
    XImage *img = XCreateImage(x_dpy, DefaultVisual(x_dpy, x_scr), (unsigned)DefaultDepth(x_dpy, x_scr), ZPixmap, 0,
                               (char *)G.fb, (unsigned)G.fb_w, (unsigned)G.fb_h, 32, 0);
    if (!img) return;
    XPutImage(x_dpy, x_win, x_gc, img, 0, 0, 0, 0, (unsigned)G.fb_w, (unsigned)G.fb_h);
    img->data = NULL; XDestroyImage(img);
    XFlush(x_dpy);
}
static int x_mods(unsigned st) { return ((st & ShiftMask) ? M_SHIFT : 0) | ((st & ControlMask) ? M_CTRL : 0) | ((st & Mod1Mask) ? M_ALT : 0); }
static bool be_poll(Ev *e) {
    memset(e, 0, sizeof *e);
    while (x_dpy && XPending(x_dpy)) {
        XEvent xe; XNextEvent(x_dpy, &xe);
        switch (xe.type) {
            case Expose: e->type = E_EXPOSE; return true;
            case ConfigureNotify:
                if (xe.xconfigure.width != x_last_w || xe.xconfigure.height != x_last_h) {
                    x_last_w = xe.xconfigure.width; x_last_h = xe.xconfigure.height;
                    e->type = E_RESIZE; e->x = x_last_w; e->y = x_last_h; return true;
                }
                break;
            case ClientMessage: if ((Atom)xe.xclient.data.l[0] == x_wm_delete) { e->type = E_CLOSE; return true; } break;
            case ButtonPress:
                e->x = xe.xbutton.x; e->y = xe.xbutton.y; e->mods = x_mods(xe.xbutton.state);
                if (xe.xbutton.button == 4 || xe.xbutton.button == 5) { e->type = E_WHEEL; e->button = xe.xbutton.button == 4 ? 1 : -1; }
                else if (xe.xbutton.button <= 3) { e->type = E_DOWN; e->button = (int)xe.xbutton.button; }
                else break;
                return true;
            case ButtonRelease:
                if (xe.xbutton.button > 3) break;
                e->type = E_UP; e->button = (int)xe.xbutton.button; e->x = xe.xbutton.x; e->y = xe.xbutton.y; e->mods = x_mods(xe.xbutton.state); return true;
            case MotionNotify: e->type = E_MOVE; e->x = xe.xmotion.x; e->y = xe.xmotion.y; e->mods = x_mods(xe.xmotion.state); return true;
            case KeyPress: {
                char buf[32]; KeySym ks = NoSymbol;
                int n = XLookupString(&xe.xkey, buf, (int)sizeof buf - 1, &ks, NULL);
                e->mods = x_mods(xe.xkey.state);
                int k = K_NONE;
                switch (ks) {
                    case XK_Return: case XK_KP_Enter: k = K_ENTER; break; case XK_Escape: k = K_ESC; break;
                    case XK_Tab: case XK_ISO_Left_Tab: k = K_TAB; if (ks == XK_ISO_Left_Tab) e->mods |= M_SHIFT; break;
                    case XK_BackSpace: k = K_BACKSPACE; break; case XK_Delete: case XK_KP_Delete: k = K_DELETE; break;
                    case XK_Left: k = K_LEFT; break; case XK_Right: k = K_RIGHT; break; case XK_Up: k = K_UP; break; case XK_Down: k = K_DOWN; break;
                    case XK_Home: k = K_HOME; break; case XK_End: k = K_END; break; case XK_Page_Up: k = K_PGUP; break; case XK_Page_Down: k = K_PGDN; break;
                    default: if (ks >= XK_F1 && ks <= XK_F12) k = K_F1 + (int)(ks - XK_F1); break;
                }
                if (k != K_NONE) { e->type = E_KEY; e->key = k; return true; }
                if ((e->mods & (M_CTRL | M_ALT)) && n == 1 && ks >= XK_a && ks <= XK_z) { e->type = E_KEY; e->key = K_CTRL_BASE + (int)ks; return true; }
                if ((e->mods & M_CTRL) && ks >= XK_A && ks <= XK_Z) { e->type = E_KEY; e->key = K_CTRL_BASE + (int)ks - 'A' + 'a'; return true; }
                if (n >= 1 && (unsigned char)buf[0] >= 32 && buf[0] != 127) {
                    e->type = E_CHAR; int l = n < 7 ? n : 7; memcpy(e->text, buf, (size_t)l); e->text[l] = 0;
                    if ((unsigned char)buf[0] >= 0x80) { int cp = (unsigned char)buf[0]; e->text[0] = (char)(0xC0 | (cp >> 6)); e->text[1] = (char)(0x80 | (cp & 63)); e->text[2] = 0; }  /* Latin-1 -> UTF-8 */
                    return true;
                }
                break;
            }
            default: break;
        }
    }
    return false;
}
static void be_idle_wait(int ms) {
    if (!x_dpy) { sleep_ms(ms); return; }
    int fd = ConnectionNumber(x_dpy);
    fd_set fds; FD_ZERO(&fds); FD_SET(fd, &fds);
    struct timeval tv = { 0, ms * 1000 };
    select(fd + 1, &fds, NULL, NULL, &tv);
}

#elif defined(KC_GUI_WIN32)

static HWND w_hwnd; static Ev w_q[256]; static int w_qh, w_qt; static bool w_class_done;
static void w_push(Ev e) { int n = (w_qt + 1) % 256; if (n != w_qh) { w_q[w_qt] = e; w_qt = n; } }
static int w_mods(void) { return ((GetKeyState(VK_SHIFT) & 0x8000) ? M_SHIFT : 0) | ((GetKeyState(VK_CONTROL) & 0x8000) ? M_CTRL : 0) | ((GetKeyState(VK_MENU) & 0x8000) ? M_ALT : 0); }
static LRESULT CALLBACK w_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    Ev e; memset(&e, 0, sizeof e);
    switch (m) {
        case WM_CLOSE: e.type = E_CLOSE; w_push(e); return 0;
        case WM_SIZE: if (wp != SIZE_MINIMIZED) { e.type = E_RESIZE; e.x = LOWORD(lp); e.y = HIWORD(lp); w_push(e); } return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
            if (G.fb) { BITMAPINFO bi; memset(&bi, 0, sizeof bi); bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth = G.fb_w; bi.bmiHeader.biHeight = -G.fb_h; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
                        SetDIBitsToDevice(dc, 0, 0, (DWORD)G.fb_w, (DWORD)G.fb_h, 0, 0, 0, (UINT)G.fb_h, G.fb, &bi, DIB_RGB_COLORS); }
            EndPaint(h, &ps); return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_MOUSEMOVE: e.type = E_MOVE; e.x = (short)LOWORD(lp); e.y = (short)HIWORD(lp); e.mods = w_mods(); w_push(e); return 0;
        case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: SetCapture(h); e.type = E_DOWN; e.button = m == WM_LBUTTONDOWN ? 1 : m == WM_MBUTTONDOWN ? 2 : 3; e.x = (short)LOWORD(lp); e.y = (short)HIWORD(lp); e.mods = w_mods(); w_push(e); return 0;
        case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: ReleaseCapture(); e.type = E_UP; e.button = m == WM_LBUTTONUP ? 1 : m == WM_MBUTTONUP ? 2 : 3; e.x = (short)LOWORD(lp); e.y = (short)HIWORD(lp); e.mods = w_mods(); w_push(e); return 0;
        case WM_MOUSEWHEEL: { POINT p = { (short)LOWORD(lp), (short)HIWORD(lp) }; ScreenToClient(h, &p); e.type = E_WHEEL; e.button = (short)HIWORD(wp) > 0 ? 1 : -1; e.x = p.x; e.y = p.y; w_push(e); return 0; }
        case WM_KEYDOWN: case WM_SYSKEYDOWN: {
            int k = K_NONE; e.mods = w_mods();
            switch (wp) {
                case VK_RETURN: k = K_ENTER; break; case VK_ESCAPE: k = K_ESC; break; case VK_TAB: k = K_TAB; break; case VK_BACK: k = K_BACKSPACE; break;
                case VK_DELETE: k = K_DELETE; break; case VK_LEFT: k = K_LEFT; break; case VK_RIGHT: k = K_RIGHT; break; case VK_UP: k = K_UP; break; case VK_DOWN: k = K_DOWN; break;
                case VK_HOME: k = K_HOME; break; case VK_END: k = K_END; break; case VK_PRIOR: k = K_PGUP; break; case VK_NEXT: k = K_PGDN; break;
                default: if (wp >= VK_F1 && wp <= VK_F12) k = K_F1 + (int)(wp - VK_F1);
                         else if ((e.mods & (M_CTRL | M_ALT)) && wp >= 'A' && wp <= 'Z') k = K_CTRL_BASE + (int)wp - 'A' + 'a';
            }
            if (k != K_NONE) { e.type = E_KEY; e.key = k; w_push(e); return 0; }
            break;
        }
        case WM_CHAR: {
            if (wp < 32 || wp == 127 || (GetKeyState(VK_CONTROL) & 0x8000)) return 0;
            unsigned cp = (unsigned)wp; e.type = E_CHAR;
            if (cp < 0x80) e.text[0] = (char)cp;
            else if (cp < 0x800) { e.text[0] = (char)(0xC0 | (cp >> 6)); e.text[1] = (char)(0x80 | (cp & 63)); }
            else { e.text[0] = (char)(0xE0 | (cp >> 12)); e.text[1] = (char)(0x80 | ((cp >> 6) & 63)); e.text[2] = (char)(0x80 | (cp & 63)); }
            w_push(e); return 0;
        }
    }
    return DefWindowProcA(h, m, wp, lp);
}
static bool be_open(const char *title, int w, int h) {
    HINSTANCE hi = GetModuleHandleA(NULL);
    if (!w_class_done) {
        WNDCLASSA wc; memset(&wc, 0, sizeof wc); wc.lpfnWndProc = w_proc; wc.hInstance = hi; wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.lpszClassName = "KingCobraGui";
        if (!RegisterClassA(&wc)) return false; w_class_done = true;
    }
    RECT r = { 0, 0, w, h }; AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    w_hwnd = CreateWindowA("KingCobraGui", title, WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL, hi, NULL);
    return w_hwnd != NULL;
}
static void be_close(void) { if (w_hwnd) { DestroyWindow(w_hwnd); w_hwnd = NULL; } }
static void be_set_title(const char *t) { if (w_hwnd) SetWindowTextA(w_hwnd, t); }
static void be_present(void) { if (w_hwnd) { InvalidateRect(w_hwnd, NULL, FALSE); UpdateWindow(w_hwnd); } }
static bool be_poll(Ev *e) {
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    if (w_qh != w_qt) { *e = w_q[w_qh]; w_qh = (w_qh + 1) % 256; return true; }
    return false;
}
static void be_idle_wait(int ms) { MsgWaitForMultipleObjects(0, NULL, FALSE, (DWORD)ms, QS_ALLINPUT); }

#else   /* no windowing system compiled in: always headless */

static bool be_open(const char *t, int w, int h) { (void)t; (void)w; (void)h; return false; }
static void be_close(void) {}
static void be_set_title(const char *t) { (void)t; }
static void be_present(void) {}
static bool be_poll(Ev *e) { (void)e; return false; }
static void be_idle_wait(int ms) { sleep_ms(ms); }

#endif

/* ======================================================= widget storage */

static void free_items(Widget *w) { for (int i = 0; i < w->nitems; i++) free(w->items[i]); free(w->items); w->items = NULL; w->nitems = 0; }
static void items_add(Widget *w, const char *s) { w->items = kc_realloc(w->items, sizeof(char *) * (size_t)(w->nitems + 1)); w->items[w->nitems++] = sdup(s); }

static Widget *widget_new(int kind, int parent) {
    Widget *w = kc_malloc(sizeof(Widget)); memset(w, 0, sizeof *w);
    w->id = G.nws; w->kind = kind; w->parent = parent; w->visible = true; w->enabled = true; w->cross = CROSS_AUTO;
    w->text = sdup(""); w->placeholder = sdup(""); w->group = sdup("");
    w->pad = 0; w->spacing = 8; w->cols = 2; w->layout = L_VBOX; w->max = 1; w->sel = -1;
    if (G.nws == G.cap) { G.cap = G.cap ? G.cap * 2 : 64; G.ws = kc_realloc(G.ws, sizeof(Widget *) * (size_t)G.cap); }
    G.ws[G.nws++] = w; mark_dirty();
    return w;
}

static void kill_widget(int id) {
    Widget *w = wget(id); if (!w) return;
    for (int i = 1; i < G.nws; i++) if (G.ws[i]->kind != W_DEAD && G.ws[i]->parent == id) kill_widget(i);
    free(w->text); free(w->placeholder); free(w->group); free_items(w);
    w->text = sdup(""); w->placeholder = sdup(""); w->group = sdup("");
    w->kind = W_DEAD; w->visible = false;
    if (G.focus == id) G.focus = -1; if (G.hover == id) G.hover = -1; if (G.capture == id) G.capture = -1; if (G.popup == id) G.popup = -1;
    mark_dirty();
}

static bool default_fillx(int k) { return k == W_PANEL || k == W_TEXT || k == W_SLIDER || k == W_PROGRESS || k == W_LIST || k == W_DROPDOWN || k == W_TABS || k == W_CANVAS || k == W_SEP; }
static bool default_filly(int k) { return k == W_PANEL || k == W_LIST || k == W_TABS || k == W_CANVAS || k == W_SEP; }
static bool focusable(const Widget *w) {
    if (!w->visible || !w->enabled) return false;
    return w->kind == W_BUTTON || w->kind == W_CHECK || w->kind == W_RADIO || w->kind == W_TEXT || w->kind == W_SLIDER ||
           w->kind == W_LIST || w->kind == W_DROPDOWN || w->kind == W_TABS;
}
static bool tree_visible(const Widget *w) {
    while (w) { if (!w->visible) return false; if (w->parent < 0 || w->id == 0) return true; Widget *p = wget(w->parent); if (!p) return false; w = p; }
    return true;
}
static int title_h(const Widget *w) { return (w->kind == W_PANEL && w->text[0]) ? 8 * wfs(w) + 12 : 0; }
static int row_h(const Widget *w) { return 8 * wfs(w) + 10; }
static int tab_bar_h(const Widget *w) { return 8 * wfs(w) + 16; }
static int box_size(const Widget *w) { return 8 * wfs(w) + 2; }

/* ================================================================ layout */

static void measure(Widget *w);

static int visible_children(int id, int *out, int cap) {
    int n = 0;
    for (int i = 1; i < G.nws && n < cap; i++) if (G.ws[i]->kind != W_DEAD && G.ws[i]->parent == id && G.ws[i]->visible) out[n++] = i;
    return n;
}
#define MAXCH 4096

static void measure_children(Widget *w, int *ids, int n) { for (int i = 0; i < n; i++) measure(G.ws[ids[i]]); }

static void measure(Widget *w) {
    int fs = wfs(w), cw = 8 * fs, ch = 8 * fs, pw = 0, ph = 0;
    switch (w->kind) {
        case W_LABEL: pw = text_w(w->text, fs); ph = text_lines(w->text) * line_h(fs) - 2 * fs; break;
        case W_BUTTON: pw = text_w(w->text, fs) + 28; ph = ch + 16; break;
        case W_CHECK: case W_RADIO: pw = box_size(w) + 10 + text_w(w->text, fs); ph = imax(box_size(w), ch) + 8; break;
        case W_TEXT: pw = 20 * cw; ph = ch + 14; break;
        case W_SLIDER: pw = 160; ph = ch + 10; break;
        case W_PROGRESS: pw = 160; ph = ch + 6; break;
        case W_LIST: { int mw = 0; for (int i = 0; i < w->nitems; i++) mw = imax(mw, text_w(w->items[i], fs)); pw = imax(mw + 28, 140); ph = row_h(w) * 6 + 2; break; }
        case W_DROPDOWN: { int mw = 0; for (int i = 0; i < w->nitems; i++) mw = imax(mw, text_w(w->items[i], fs)); pw = mw + 20 + ch + 8; ph = ch + 14; break; }
        case W_CANVAS: pw = 200; ph = 150; break;
        case W_SEP: pw = 9; ph = 9; break;
        case W_SPACER: pw = 0; ph = 0; break;
        case W_TABS: {
            int ids[MAXCH]; int n = visible_children(w->id, ids, MAXCH); measure_children(w, ids, n);
            int bar = 0; for (int i = 0; i < w->nitems; i++) bar += text_w(w->items[i], fs) + 28;
            int mw = 0, mh = 0; for (int i = 0; i < n; i++) { mw = imax(mw, G.ws[ids[i]]->pw); mh = imax(mh, G.ws[ids[i]]->ph); }
            pw = imax(bar, mw); ph = tab_bar_h(w) + mh; break;
        }
        case W_PANEL: {
            int *ids = kc_malloc(sizeof(int) * MAXCH); int n = visible_children(w->id, ids, MAXCH); measure_children(w, ids, n);
            int p2 = 2 * w->pad, th = title_h(w);
            if (w->layout == L_VBOX || w->layout == L_HBOX) {
                bool v = w->layout == L_VBOX; int main = 0, cross = 0;
                for (int i = 0; i < n; i++) { Widget *c = G.ws[ids[i]]; main += v ? c->ph : c->pw; cross = imax(cross, v ? c->pw : c->ph); }
                if (n > 1) main += w->spacing * (n - 1);
                pw = (v ? cross : main) + p2; ph = (v ? main : cross) + p2 + th;
            } else if (w->layout == L_GRID) {
                int cols = imax(w->cols, 1), rows = (n + cols - 1) / cols;
                int *cwid = kc_malloc(sizeof(int) * (size_t)cols), *rh = kc_malloc(sizeof(int) * (size_t)(rows ? rows : 1));
                memset(cwid, 0, sizeof(int) * (size_t)cols); memset(rh, 0, sizeof(int) * (size_t)(rows ? rows : 1));
                for (int i = 0; i < n; i++) { Widget *c = G.ws[ids[i]]; cwid[i % cols] = imax(cwid[i % cols], c->pw); rh[i / cols] = imax(rh[i / cols], c->ph); }
                int tw = 0, thh = 0; for (int c = 0; c < cols; c++) tw += cwid[c]; for (int r = 0; r < rows; r++) thh += rh[r];
                pw = tw + w->spacing * (cols - 1) + p2; ph = thh + (rows > 0 ? w->spacing * (rows - 1) : 0) + p2 + th;
                free(cwid); free(rh);
            } else {
                for (int i = 0; i < n; i++) { Widget *c = G.ws[ids[i]]; int cw2 = c->req_w ? c->req_w : c->pw, ch2 = c->req_h ? c->req_h : c->ph; pw = imax(pw, c->ax + cw2); ph = imax(ph, c->ay + ch2); }
                pw += p2; ph += p2 + th;
            }
            if (w->text[0]) pw = imax(pw, text_w(w->text, fs) + 24);
            free(ids); break;
        }
        default: break;
    }
    if (w->req_w > 0) pw = w->req_w;
    if (w->req_h > 0) ph = w->req_h;
    w->pw = pw; w->ph = ph;
}

/* Places a child inside the cell (cx,cy,cw,chh) according to its cross-axis / fill rules. */
static void place_in_cell(Widget *c, int cx, int cy, int cw, int chh, bool horizontal_flow, bool is_grid);
static void layout(Widget *w, int x, int y, int ww, int hh);

static void place_in_cell(Widget *c, int cx, int cy, int cw, int chh, bool hflow, bool grid) {
    int w = c->pw, h = c->ph;
    bool fillx = default_fillx(c->kind), filly = default_filly(c->kind);
    int ax = c->cross, ay = c->cross;
    if (c->req_w > 0) fillx = false; if (c->req_h > 0) filly = false;
    if (c->cross == CROSS_FILL) { fillx = filly = true; }
    if (grid) { if (c->cross == CROSS_AUTO) { ax = CROSS_START; ay = CROSS_CENTER; } }
    else if (hflow) { if (c->cross == CROSS_AUTO) ay = CROSS_CENTER; ax = CROSS_START; }
    else { if (c->cross == CROSS_AUTO) ax = CROSS_START; ay = CROSS_START; }
    if (c->kind == W_SPACER) { w = cw; h = chh; }
    else {
        if (fillx || (!hflow && !grid && c->cross == CROSS_FILL)) w = cw;
        if (filly) h = chh;
    }
    if (hflow && !grid) { /* main axis handled by caller: cw is the allotted width */ w = cw; }
    int ox = cx, oy = cy;
    if (!fillx && !hflow) { if (ax == CROSS_CENTER) ox = cx + (cw - w) / 2; else if (ax == CROSS_END) ox = cx + cw - w; }
    if (!filly && hflow)  { if (ay == CROSS_CENTER) oy = cy + (chh - h) / 2; else if (ay == CROSS_END) oy = cy + chh - h; }
    if (grid) { if (!fillx && ax == CROSS_CENTER) ox = cx + (cw - w) / 2; if (!filly) { if (ay == CROSS_CENTER) oy = cy + (chh - h) / 2; else if (ay == CROSS_END) oy = cy + chh - h; } }
    layout(c, ox, oy, w, h);
}

static void layout(Widget *w, int x, int y, int ww, int hh) {
    w->x = x; w->y = y; w->w = imax(ww, 0); w->h = imax(hh, 0);
    if (w->kind != W_PANEL && w->kind != W_TABS) return;
    int *ids = kc_malloc(sizeof(int) * MAXCH); int n = visible_children(w->id, ids, MAXCH);
    if (w->kind == W_TABS) {
        int bar = tab_bar_h(w);
        int idx = 0;
        for (int i = 1; i < G.nws; i++) if (G.ws[i]->kind != W_DEAD && G.ws[i]->parent == w->id) {
            Widget *c = G.ws[i];
            if (c->visible) { if (idx == w->sel) layout(c, x, y + bar, ww, hh - bar); }
            idx++;
        }
        free(ids); return;
    }
    int p = w->pad, th = title_h(w);
    int ix = x + p, iy = y + p + th, iw = ww - 2 * p, ih = hh - 2 * p - th;
    if (w->layout == L_VBOX || w->layout == L_HBOX) {
        bool v = w->layout == L_VBOX; int avail = (v ? ih : iw) - (n > 1 ? w->spacing * (n - 1) : 0), pref = 0, wsum = 0;
        for (int i = 0; i < n; i++) { Widget *c = G.ws[ids[i]]; pref += v ? c->ph : c->pw; wsum += c->weight > 0 ? c->weight : 0; }
        int extra = avail - pref, pos = v ? iy : ix, given = 0, seen = 0;
        for (int i = 0; i < n; i++) {
            Widget *c = G.ws[ids[i]]; int main = v ? c->ph : c->pw;
            if (c->weight > 0 && wsum > 0) {
                seen += c->weight;
                int share = (int)((long)extra * seen / wsum) - given; given += share; main += share; if (main < 0) main = 0;
            }
            if (v) place_in_cell(c, ix, pos, iw, main, false, false); else place_in_cell(c, pos, iy, main, ih, true, false);
            pos += main + w->spacing;
        }
    } else if (w->layout == L_GRID) {
        int cols = imax(w->cols, 1), rows = (n + cols - 1) / cols;
        int *cwid = kc_malloc(sizeof(int) * (size_t)cols), *rh = kc_malloc(sizeof(int) * (size_t)(rows ? rows : 1)), *cwt = kc_malloc(sizeof(int) * (size_t)cols), *rwt = kc_malloc(sizeof(int) * (size_t)(rows ? rows : 1));
        memset(cwid, 0, sizeof(int) * (size_t)cols); memset(cwt, 0, sizeof(int) * (size_t)cols);
        memset(rh, 0, sizeof(int) * (size_t)(rows ? rows : 1)); memset(rwt, 0, sizeof(int) * (size_t)(rows ? rows : 1));
        for (int i = 0; i < n; i++) { Widget *c = G.ws[ids[i]]; int cc = i % cols, rr = i / cols; cwid[cc] = imax(cwid[cc], c->pw); rh[rr] = imax(rh[rr], c->ph); cwt[cc] = imax(cwt[cc], c->weight); rwt[rr] = imax(rwt[rr], c->weight); }
        int tw = 0, thh = 0, cws = 0, rws = 0;
        for (int c = 0; c < cols; c++) { tw += cwid[c]; cws += cwt[c]; } for (int r = 0; r < rows; r++) { thh += rh[r]; rws += rwt[r]; }
        int ex = iw - tw - w->spacing * (cols - 1), ey = ih - thh - (rows > 0 ? w->spacing * (rows - 1) : 0);
        if (cws > 0 && ex > 0) { int g = 0, s = 0; for (int c = 0; c < cols; c++) { s += cwt[c]; int sh = ex * s / cws - g; g += sh; cwid[c] += sh; } }
        if (rws > 0 && ey > 0) { int g = 0, s = 0; for (int r = 0; r < rows; r++) { s += rwt[r]; int sh = ey * s / rws - g; g += sh; rh[r] += sh; } }
        int yy = iy;
        for (int r = 0; r < rows; r++) {
            int xx = ix;
            for (int c = 0; c < cols; c++) { int i = r * cols + c; if (i < n) place_in_cell(G.ws[ids[i]], xx, yy, cwid[c], rh[r], false, true); xx += cwid[c] + w->spacing; }
            yy += rh[r] + w->spacing;
        }
        free(cwid); free(rh); free(cwt); free(rwt);
    } else {
        for (int i = 0; i < n; i++) { Widget *c = G.ws[ids[i]]; layout(c, ix + c->ax, iy + c->ay, c->req_w ? c->req_w : c->pw, c->req_h ? c->req_h : c->ph); }
    }
    free(ids);
}

static void relayout(void) {
    Widget *r = G.ws[0];
    measure(r);
    layout(r, 0, 0, G.win_w, G.win_h);
}

/* ================================================================ paint */

static uint32_t wfg(const Widget *w) { return w->has_fg ? w->fg : G.th.text; }
static int caret_px(const Widget *w, int bytepos) { return u8_count(w->text, bytepos) * 8 * wfs(w); }

static void paint_widget(Widget *w);

static void paint_children(Widget *w) {
    for (int i = 1; i < G.nws; i++) {
        Widget *c = G.ws[i];
        if (c->kind == W_DEAD || c->parent != w->id || !c->visible) continue;
        if (w->kind == W_TABS) { int idx = 0; for (int j = 1; j < i; j++) if (G.ws[j]->kind != W_DEAD && G.ws[j]->parent == w->id) idx++; if (idx != w->sel) continue; }
        paint_widget(c);
    }
}

static void sel_range(const Widget *w, int *a, int *b) { *a = imin(w->cur, w->anchor); *b = imax(w->cur, w->anchor); }

static void paint_widget(Widget *w) {
    const Theme *t = &G.th; int fs = wfs(w), cw = 8 * fs, ch = 8 * fs;
    bool dis = !w->enabled; uint32_t tc = dis ? t->dim : wfg(w);
    bool focused = G.focus == w->id;
    switch (w->kind) {
        case W_PANEL:
            if (w->has_bg) fb_rect(w->x, w->y, w->w, w->h, w->bg);
            if (w->text[0]) {
                int th = title_h(w);
                fb_round_border(w->x, w->y + th / 2, w->w, w->h - th / 2, 6, w->has_bg ? w->bg : t->panel, t->border);
                int tx = w->x + 12, tw2 = text_w(w->text, fs) + 10;
                fb_rect(tx - 5, w->y + th / 2 - 1, tw2, 3, w->has_bg ? w->bg : t->panel);
                fb_text(tx, w->y + (th - ch) / 2 - 1, w->text, t->dim, fs);
            }
            paint_children(w); break;
        case W_TABS: {
            int bar = tab_bar_h(w), x = w->x;
            fb_rect(w->x, w->y + bar - 1, w->w, 1, t->border);
            for (int i = 0; i < w->nitems; i++) {
                int tw2 = text_w(w->items[i], fs) + 28; bool act = i == w->sel;
                fb_round(x, w->y + 2, tw2, bar - 2, 6, act ? t->panel : t->bg);
                fb_rect(x, w->y + bar - 8, tw2, 8, act ? t->panel : t->bg);
                if (act) fb_rect(x + 6, w->y + bar - 3, tw2 - 12, 3, t->accent);
                fb_text(x + 14, w->y + (bar - ch) / 2, w->items[i], act ? t->text : t->dim, fs);
                if (focused && act) fb_frame(x + 2, w->y + 4, tw2 - 4, bar - 8, t->accent);
                x += tw2;
            }
            fb_rect(w->x, w->y + bar, w->w, w->h - bar, t->panel);
            paint_children(w); break;
        }
        case W_LABEL: {
            int th = text_lines(w->text) * line_h(fs);
            (void)th;
            if (w->talign == 0) fb_text(w->x, w->y, w->text, tc, fs);
            else {                                  /* align each line within the widget width */
                int y = w->y; const char *s = w->text;
                for (;;) {
                    const char *e = strchr(s, '\n'); int len = e ? (int)(e - s) : (int)strlen(s);
                    char *line = kc_malloc((size_t)len + 1); memcpy(line, s, (size_t)len); line[len] = 0;
                    int lw = text_w(line, fs); int x = w->talign == 1 ? w->x + (w->w - lw) / 2 : w->x + w->w - lw;
                    fb_text(x, y, line, tc, fs); free(line); y += line_h(fs);
                    if (!e) break; s = e + 1;
                }
            }
            break;
        }
        case W_BUTTON: {
            uint32_t face = dis ? t->track : (w->pressed ? t->press : (w->hover ? t->hover : t->border));
            uint32_t fg = dis ? t->dim : t->text;
            if (w->has_bg && !dis) face = w->pressed ? t->press : w->bg;
            fb_round(w->x, w->y, w->w, w->h, 6, face);
            if (focused) fb_round_border(w->x, w->y, w->w, w->h, 6, face, t->accent);
            int tw2 = text_w(w->text, fs); int off = w->pressed ? 1 : 0;
            fb_text(w->x + (w->w - tw2) / 2 + off, w->y + (w->h - ch) / 2 + off, w->text, w->has_fg ? w->fg : fg, fs);
            break;
        }
        case W_CHECK: case W_RADIO: {
            int bs = box_size(w), by = w->y + (w->h - bs) / 2;
            uint32_t edge = focused ? t->accent : (w->hover ? t->dim : t->border);
            if (w->kind == W_CHECK) {
                fb_round_border(w->x, by, bs, bs, 4, w->checked ? t->accent : t->field, w->checked ? t->accent : edge);
                if (w->checked) { fb_line(w->x + bs / 5 + 1, by + bs / 2, w->x + bs / 2 - 1, by + bs - bs / 4 - 1, t->accent_text, 2); fb_line(w->x + bs / 2 - 1, by + bs - bs / 4 - 1, w->x + bs - bs / 5 - 1, by + bs / 4, t->accent_text, 2); }
            } else {
                fb_circle(w->x + bs / 2, by + bs / 2, bs / 2, edge, true); fb_circle(w->x + bs / 2, by + bs / 2, bs / 2 - 1, t->field, true);
                if (w->checked) fb_circle(w->x + bs / 2, by + bs / 2, bs / 4 + 1, t->accent, true);
            }
            fb_text(w->x + bs + 10, w->y + (w->h - ch) / 2, w->text, tc, fs);
            break;
        }
        case W_TEXT: {
            fb_round_border(w->x, w->y, w->w, w->h, 5, t->field, focused ? t->accent : t->border);
            int tx = w->x + 8, ty = w->y + (w->h - ch) / 2, inner = w->w - 16;
            int s0x = G.cx0, s0y = G.cy0, s1x = G.cx1, s1y = G.cy1;
            G.cx0 = imax(G.cx0, tx); G.cx1 = imin(G.cx1, tx + inner); G.cy0 = imax(G.cy0, w->y + 2); G.cy1 = imin(G.cy1, w->y + w->h - 2);
            const char *shown = w->text; char *mask = NULL;
            if (w->password) { int n = u8_count(w->text, (int)strlen(w->text)); mask = kc_malloc((size_t)n + 1); memset(mask, '*', (size_t)n); mask[n] = 0; shown = mask; }
            if (!w->text[0] && !focused && w->placeholder[0]) fb_text(tx, ty, w->placeholder, t->dim, fs);
            int a, b; sel_range(w, &a, &b);
            if (focused && a != b) { int pa = w->password ? u8_count(w->text, a) * cw : caret_px(w, a), pb = w->password ? u8_count(w->text, b) * cw : caret_px(w, b); fb_rect(tx - w->hscroll + pa, ty - 2, pb - pa, ch + 4, t->sel); }
            fb_text(tx - w->hscroll, ty, shown, dis ? t->dim : wfg(w), fs);
            if (focused && G.blink_on && a == b) { int cp = (w->password ? u8_count(w->text, w->cur) * cw : caret_px(w, w->cur)); fb_rect(tx - w->hscroll + cp, ty - 2, 2, ch + 4, t->text); }
            free(mask);
            G.cx0 = s0x; G.cy0 = s0y; G.cx1 = s1x; G.cy1 = s1y;
            break;
        }
        case W_SLIDER: {
            int th = 6, ty = w->y + (w->h - th) / 2, knob = ch + 2;
            int tx0 = w->x + knob / 2, tx1 = w->x + w->w - knob / 2;
            double f = w->max > w->min ? (w->val - w->min) / (w->max - w->min) : 0; if (f < 0) f = 0; if (f > 1) f = 1;
            int kx = tx0 + (int)((tx1 - tx0) * f);
            fb_round(tx0, ty, tx1 - tx0, th, 3, t->track); fb_round(tx0, ty, kx - tx0, th, 3, dis ? t->dim : t->accent);
            fb_circle(kx, w->y + w->h / 2, knob / 2, focused ? t->accent : t->text, true);
            fb_circle(kx, w->y + w->h / 2, knob / 2 - 2, dis ? t->dim : (w->pressed ? t->press : t->panel), true);
            break;
        }
        case W_PROGRESS: {
            double f = w->max > w->min ? (w->val - w->min) / (w->max - w->min) : 0; if (f < 0) f = 0; if (f > 1) f = 1;
            fb_round(w->x, w->y, w->w, w->h, w->h / 2, t->track);
            int fw = (int)(w->w * f); if (fw > 0) fb_round(w->x, w->y, imax(fw, w->h), w->h, w->h / 2, t->accent);
            break;
        }
        case W_LIST: {
            fb_round_border(w->x, w->y, w->w, w->h, 5, t->field, focused ? t->accent : t->border);
            int rh = row_h(w), vis = imax((w->h - 2) / rh, 1);
            int s0x = G.cx0, s0y = G.cy0, s1x = G.cx1, s1y = G.cy1;
            G.cx0 = imax(G.cx0, w->x + 1); G.cx1 = imin(G.cx1, w->x + w->w - 1); G.cy0 = imax(G.cy0, w->y + 1); G.cy1 = imin(G.cy1, w->y + w->h - 1);
            for (int i = 0; i < vis && w->scroll + i < w->nitems; i++) {
                int idx = w->scroll + i, ry = w->y + 1 + i * rh;
                if (idx == w->sel) fb_rect(w->x + 1, ry, w->w - 2, rh, focused ? t->accent : t->sel);
                fb_text(w->x + 8, ry + (rh - ch) / 2, w->items[idx], idx == w->sel ? t->accent_text : tc, fs);
            }
            if (w->nitems > vis) {                                   /* scrollbar indicator */
                int bh = imax(w->h * vis / w->nitems, 14), by = w->y + 1 + (w->h - 2 - bh) * w->scroll / imax(w->nitems - vis, 1);
                fb_round(w->x + w->w - 7, by, 5, bh, 2, t->track);
            }
            G.cx0 = s0x; G.cy0 = s0y; G.cx1 = s1x; G.cy1 = s1y;
            break;
        }
        case W_DROPDOWN: {
            fb_round_border(w->x, w->y, w->w, w->h, 5, w->hover ? t->hover : t->field, focused || G.popup == w->id ? t->accent : t->border);
            const char *cur = (w->sel >= 0 && w->sel < w->nitems) ? w->items[w->sel] : "";
            fb_text_clip(w->x + 8, w->y + (w->h - ch) / 2, w->w - 16 - ch, ch + 2, cur, tc, fs);
            int ax = w->x + w->w - ch - 2, ay = w->y + w->h / 2;
            fb_triangle(ax - ch / 3, ay - ch / 6, ax + ch / 3, ay - ch / 6, ax, ay + ch / 4, t->dim, true);
            break;
        }
        case W_CANVAS: {
            fb_round_border(w->x, w->y, w->w, w->h, 0, w->has_bg ? w->bg : t->field, focused ? t->accent : t->border);
            if (w->has_cb[EV_DRAW]) {
                int a = G.cx0, b = G.cy0, c = G.cx1, d = G.cy1;
                G.cx0 = imax(G.cx0, w->x + 1); G.cy0 = imax(G.cy0, w->y + 1); G.cx1 = imin(G.cx1, w->x + w->w - 1); G.cy1 = imin(G.cy1, w->y + w->h - 1);
                G.ox = w->x; G.oy = w->y; G.draw_id = w->id;
                KcValue args[3] = { kc_int(w->id), kc_int(w->w), kc_int(w->h) };
                KcValue cb = w->cb[EV_DRAW];
                kc_call_value(G.ip, cb, args, 3, G.pos);
                G.draw_id = -1;
                G.cx0 = a; G.cy0 = b; G.cx1 = c; G.cy1 = d;
            }
            break;
        }
        case W_SEP:
            if (w->parent >= 0 && wget(w->parent) && wget(w->parent)->layout == L_HBOX) fb_rect(w->x + w->w / 2, w->y, 1, w->h, t->border);
            else fb_rect(w->x, w->y + w->h / 2, w->w, 1, t->border);
            break;
        default: break;
    }
}

/* ---- modal dialog ---- */

static void dlg_free(void) {
    free(G.dlg.title); free(G.dlg.text); for (int i = 0; i < G.dlg.nbtn; i++) free(G.dlg.btn[i]); free(G.dlg.btn);
    for (int i = 0; i < G.dlg.nlines; i++) free(G.dlg.lines[i]); free(G.dlg.lines);
    memset(&G.dlg, 0, sizeof G.dlg);
}
static void dlg_geometry(void) {
    Dialog *d = &G.dlg; int fs = G.fs, cw = 8 * fs, ch = 8 * fs;
    int maxw = imin(G.win_w - 40, 56 * cw); if (maxw < 160) maxw = 160;
    int cols = imax((maxw - 40) / cw, 8);
    for (int i = 0; i < d->nlines; i++) free(d->lines[i]); free(d->lines); d->lines = NULL; d->nlines = 0;
    const char *s = d->text; int cur = 0;
    char *line = kc_malloc(strlen(s) + 2); int ll = 0, lastsp = -1;
    for (;; s++) {
        char c = *s;
        if (c == '\n' || c == 0 || (ll >= cols && c)) {
            int cut = ll;
            if (c != '\n' && c != 0 && lastsp > 0) { cut = lastsp; }
            char *l = kc_malloc((size_t)cut + 1); memcpy(l, line, (size_t)cut); l[cut] = 0;
            d->lines = kc_realloc(d->lines, sizeof(char *) * (size_t)(d->nlines + 1)); d->lines[d->nlines++] = l;
            if (c == 0) break;
            if (c == '\n') { ll = 0; lastsp = -1; continue; }
            int rest = ll - cut; while (rest > 0 && line[cut] == ' ') { cut++; rest--; }
            memmove(line, line + cut, (size_t)rest); ll = rest; lastsp = -1;
            if (c != ' ') line[ll++] = c; else lastsp = -1;
            continue;
        }
        if (c == ' ') lastsp = ll;
        line[ll++] = c;
    }
    free(line); (void)cur;
    int tw = 0; for (int i = 0; i < d->nlines; i++) tw = imax(tw, text_w(d->lines[i], fs));
    int bwid = 0; for (int i = 0; i < d->nbtn; i++) bwid = imax(bwid, text_w(d->btn[i], fs));
    d->bw = bwid + 36; d->bh = ch + 18;
    int btnrow = d->nbtn * d->bw + (d->nbtn - 1) * 10;
    d->w = imax(imax(tw, btnrow), text_w(d->title, fs)) + 48; d->w = imax(d->w, 260);
    d->h = 20 + ch + 16 + d->nlines * line_h(fs) + 24 + d->bh + 20;
    d->x = (G.win_w - d->w) / 2; d->y = (G.win_h - d->h) / 2;
    d->tx = d->x + 24; d->ty = d->y + 20 + ch + 16;
    d->by = d->y + d->h - 20 - d->bh; d->bx0 = d->x + d->w - 24 - btnrow;
}
static void paint_dialog(void) {
    Dialog *d = &G.dlg; if (!d->active) return;
    const Theme *t = &G.th; int fs = G.fs, ch = 8 * fs;
    fb_clip_reset(); fb_rect_alpha(0, 0, G.win_w, G.win_h, 0x000000, 130);
    dlg_geometry();
    fb_round(d->x + 3, d->y + 5, d->w, d->h, 10, 0x000000);   /* hard shadow */
    fb_rect_alpha(d->x - 2, d->y - 2, d->w + 8, d->h + 10, 0x000000, 0);
    fb_round_border(d->x, d->y, d->w, d->h, 10, t->panel, t->border);
    fb_text(d->x + 24, d->y + 20, d->title, t->text, fs);
    fb_rect(d->x + 24, d->y + 20 + ch + 7, d->w - 48, 1, t->border);
    for (int i = 0; i < d->nlines; i++) fb_text(d->tx, d->ty + i * line_h(fs), d->lines[i], t->text, fs);
    for (int i = 0; i < d->nbtn; i++) {
        int bx = d->bx0 + i * (d->bw + 10); bool sel = i == d->sel;
        fb_round(bx, d->by, d->bw, d->bh, 6, sel ? t->accent : t->border);
        fb_text(bx + (d->bw - text_w(d->btn[i], fs)) / 2, d->by + (d->bh - ch) / 2, d->btn[i], sel ? t->accent_text : t->text, fs);
    }
}

static void popup_geometry(const Widget *w, int *x, int *y, int *pw, int *ph) {
    int rh = row_h(w), vis = imin(w->nitems, 8);
    *x = w->x; *y = w->y + w->h + 2; *pw = w->w; *ph = vis * rh + 2;
    if (*y + *ph > G.win_h && w->y - 2 - *ph >= 0) *y = w->y - 2 - *ph;    /* flip up when there is no room below */
}
static void paint_popup(void) {
    Widget *w = wget(G.popup); if (!w || w->kind != W_DROPDOWN) return;
    int x, y, pw, ph; popup_geometry(w, &x, &y, &pw, &ph);
    const Theme *t = &G.th; int fs = wfs(w), rh = row_h(w);
    fb_clip_reset();
    fb_round_border(x, y, pw, ph, 5, t->field, t->accent);
    for (int i = 0; i < imin(w->nitems, 8); i++) {
        int ry = y + 1 + i * rh; bool hov = G.my >= ry && G.my < ry + rh && G.mx >= x && G.mx < x + pw;
        if (i == w->sel || hov) fb_rect(x + 1, ry, pw - 2, rh, i == w->sel ? t->accent : t->hover);
        fb_text(x + 8, ry + (rh - 8 * fs) / 2, w->items[i], i == w->sel ? t->accent_text : t->text, fs);
    }
}

static void paint_all(void) {
    if (!G.fb) return;
    fb_clip_reset();
    fb_rect(0, 0, G.fb_w, G.fb_h, G.th.bg);
    paint_widget(G.ws[0]);
    if (G.popup >= 0) paint_popup();
    paint_dialog();
}

/* ============================================================== events */

static KcValue fire(int id, int ev, KcValue *args, int n) {
    Widget *w = wget(id);
    if (!w || !w->has_cb[ev] || !G.ip) return kc_null();
    KcValue cb = w->cb[ev];                       /* copy: the callback may rebind or kill the widget */
    return kc_call_value(G.ip, cb, args, n, G.pos);
}
static void fire_id(int id, int ev) { KcValue a[1] = { kc_int(id) }; fire(id, ev, a, 1); }

static void set_focus(int id) {
    if (G.focus == id) return;
    G.focus = id; G.blink_on = true; G.blink_epoch = now_ms(); mark_dirty();
}
static void reveal_caret(Widget *w) {
    int cw = 8 * wfs(w), px = (w->password ? u8_count(w->text, w->cur) : u8_count(w->text, w->cur)) * cw, inner = w->w - 16;
    if (px - w->hscroll > inner - cw) w->hscroll = px - (inner - cw);
    if (px - w->hscroll < 0) w->hscroll = px;
    if (w->hscroll < 0) w->hscroll = 0;
}
static void list_reveal(Widget *w) {
    int vis = imax((w->h - 2) / row_h(w), 1);
    if (w->sel < w->scroll) w->scroll = w->sel;
    if (w->sel >= w->scroll + vis) w->scroll = w->sel - vis + 1;
    w->scroll = iclamp(w->scroll, 0, imax(w->nitems - vis, 0));
}
static void tb_changed(Widget *w) {
    reveal_caret(w); G.blink_on = true; G.blink_epoch = now_ms(); mark_dirty();
    KcValue a[2] = { kc_int(w->id), kc_string(w->text) }; fire(w->id, EV_CHANGE, a, 2);
}
static void tb_replace_sel(Widget *w, const char *ins) {
    int a, b; sel_range(w, &a, &b); int il = (int)strlen(ins), tl = (int)strlen(w->text);
    char *nt = kc_malloc((size_t)(tl - (b - a) + il + 1));
    memcpy(nt, w->text, (size_t)a); memcpy(nt + a, ins, (size_t)il); memcpy(nt + a + il, w->text + b, (size_t)(tl - b + 1));
    free(w->text); w->text = nt; w->cur = w->anchor = a + il;
}
static void tb_move(Widget *w, int to, bool extend) { w->cur = to; if (!extend) w->anchor = to; G.blink_on = true; G.blink_epoch = now_ms(); reveal_caret(w); mark_dirty(); }
static int tb_byte_at_x(Widget *w, int px) {
    int cw = 8 * wfs(w); int col = (px - (w->x + 8) + w->hscroll + cw / 2) / cw; if (col < 0) col = 0;
    int i = 0; while (col > 0 && w->text[i]) { i = u8_next(w->text, i); col--; }
    return i;
}

static void set_checked_radio(Widget *w) {
    for (int i = 1; i < G.nws; i++) { Widget *o = G.ws[i]; if (o->kind == W_RADIO && o->parent == w->parent && strcmp(o->group, w->group) == 0 && o != w) o->checked = false; }
    if (!w->checked) { w->checked = true; KcValue a[2] = { kc_int(w->id), kc_bool(true) }; fire(w->id, EV_CHANGE, a, 2); }
    mark_dirty();
}
static void activate(Widget *w) {                      /* click-equivalent for buttons / checkboxes / radios */
    if (!w->enabled) return;
    if (w->kind == W_BUTTON) fire_id(w->id, EV_CLICK);
    else if (w->kind == W_CHECK) { w->checked = !w->checked; mark_dirty(); KcValue a[2] = { kc_int(w->id), kc_bool(w->checked) }; fire(w->id, EV_CHANGE, a, 2); fire_id(w->id, EV_CLICK); }
    else if (w->kind == W_RADIO) { set_checked_radio(w); fire_id(w->id, EV_CLICK); }
}
static void slider_set(Widget *w, double v, bool fire_cb) {
    if (w->step > 0) v = w->min + floor((v - w->min) / w->step + 0.5) * w->step;
    if (v < w->min) v = w->min; if (v > w->max) v = w->max;
    if (v == w->val) return;
    w->val = v; mark_dirty();
    if (fire_cb) { KcValue a[2] = { kc_int(w->id), kc_float(v) }; fire(w->id, EV_CHANGE, a, 2); }
}
static void slider_from_x(Widget *w, int x) {
    int knob = 8 * wfs(w) + 2, x0 = w->x + knob / 2, x1 = w->x + w->w - knob / 2;
    double f = x1 > x0 ? (double)(x - x0) / (double)(x1 - x0) : 0; f = f < 0 ? 0 : (f > 1 ? 1 : f);
    slider_set(w, w->min + (w->max - w->min) * f, true);
}
static void list_select(Widget *w, int idx, bool user) {
    if (idx < 0 || idx >= w->nitems) return;
    bool changed = idx != w->sel; w->sel = idx; list_reveal(w); mark_dirty();
    if (changed || user) { KcValue a[2] = { kc_int(w->id), kc_int(idx) }; fire(w->id, EV_SELECT, a, 2); }
}
static void dropdown_select(Widget *w, int idx) {
    if (idx < 0 || idx >= w->nitems) return;
    bool changed = idx != w->sel; w->sel = idx; mark_dirty();
    if (changed) { KcValue a[2] = { kc_int(w->id), kc_int(idx) }; fire(w->id, EV_CHANGE, a, 2); fire(w->id, EV_SELECT, a, 2); }
}
static int active_page(const Widget *tabs) {
    int idx = 0;
    for (int i = 1; i < G.nws; i++) if (G.ws[i]->kind != W_DEAD && G.ws[i]->parent == tabs->id) { if (idx == tabs->sel) return i; idx++; }
    return -1;
}
static void tabs_select(Widget *w, int idx) {
    if (idx < 0 || idx >= w->nitems || idx == w->sel) return;
    w->sel = idx; mark_dirty();
    KcValue a[2] = { kc_int(w->id), kc_int(idx) }; fire(w->id, EV_CHANGE, a, 2); fire(w->id, EV_SELECT, a, 2);
}

static int hit(Widget *w, int x, int y) {
    if (!w->visible || x < w->x || x >= w->x + w->w || y < w->y || y >= w->y + w->h) return -1;
    int page = w->kind == W_TABS ? active_page(w) : -1;
    if (w->kind == W_TABS && y < w->y + tab_bar_h(w)) return w->id;
    for (int i = G.nws - 1; i >= 1; i--) {
        Widget *c = G.ws[i]; if (c->kind == W_DEAD || c->parent != w->id) continue;
        if (w->kind == W_TABS && i != page) continue;
        int r = hit(c, x, y); if (r >= 0) return r;
    }
    return w->id;
}

static void focus_step(bool back) {
    int start = G.focus, n = G.nws;
    for (int k = 1; k <= n; k++) {
        int i = ((start < 0 ? (back ? n : -1) : start) + (back ? -k : k)) % n; if (i < 0) i += n;
        Widget *w = G.ws[i];
        if (w->kind != W_DEAD && focusable(w) && tree_visible(w) && (w->kind != W_TABS || w->nitems > 0)) { set_focus(i); return; }
    }
}

static const char *key_name(int k) {
    static char buf[16];
    switch (k) {
        case K_ENTER: return "Enter"; case K_ESC: return "Escape"; case K_TAB: return "Tab"; case K_BACKSPACE: return "Backspace";
        case K_DELETE: return "Delete"; case K_LEFT: return "Left"; case K_RIGHT: return "Right"; case K_UP: return "Up"; case K_DOWN: return "Down";
        case K_HOME: return "Home"; case K_END: return "End"; case K_PGUP: return "PageUp"; case K_PGDN: return "PageDown";
    }
    if (k >= K_F1 && k < K_F1 + 12) { snprintf(buf, sizeof buf, "F%d", k - K_F1 + 1); return buf; }
    if (k >= K_CTRL_BASE) { buf[0] = (char)(k - K_CTRL_BASE); buf[1] = 0; return buf; }
    return "";
}
static bool is_key_name_valid(const char *n, int *k) {
    for (int c = K_ENTER; c <= K_PGDN; c++) if (strcmp(key_name(c), n) == 0) { *k = c; return true; }
    for (int c = 0; c < 12; c++) if (strcmp(key_name(K_F1 + c), n) == 0) { *k = K_F1 + c; return true; }
    return false;
}

static void dlg_close(int index) {
    KcValue cb = G.dlg.cb; bool has = G.dlg.has_cb, as_bool = G.dlg.as_bool;
    dlg_free(); mark_dirty();
    if (has) { KcValue a[1] = { as_bool ? kc_bool(index == 1) : kc_int(index) }; kc_call_value(G.ip, cb, a, 1, G.pos); }
}

static void fire_key(Widget *foc, const char *name, int mods) {
    KcValue a[3];
    if (foc && foc->has_cb[EV_KEY]) { a[0] = kc_int(foc->id); a[1] = kc_string(name); a[2] = kc_int(mods); fire(foc->id, EV_KEY, a, 3); }
    a[0] = kc_int(0); a[1] = kc_string(name); a[2] = kc_int(mods); fire(0, EV_KEY, a, 3);
}

static void on_key(int k, int mods) {
    if (G.dlg.active) {
        Dialog *d = &G.dlg;
        if (k == K_LEFT || (k == K_TAB && (mods & M_SHIFT))) d->sel = (d->sel + d->nbtn - 1) % d->nbtn;
        else if (k == K_RIGHT || k == K_TAB) d->sel = (d->sel + 1) % d->nbtn;
        else if (k == K_ENTER) dlg_close(d->sel);
        else if (k == K_ESC) dlg_close(-1);
        mark_dirty(); return;
    }
    if (G.popup >= 0) {
        Widget *p = wget(G.popup);
        if (p) { if (k == K_UP) dropdown_select(p, p->sel - 1); else if (k == K_DOWN) dropdown_select(p, p->sel + 1); }
        if (k == K_ESC || k == K_ENTER) { G.popup = -1; mark_dirty(); }
        return;
    }
    Widget *f = wget(G.focus);
    G.blink_on = true; G.blink_epoch = now_ms();
    bool shift = mods & M_SHIFT, ctrl = mods & M_CTRL;
    if (k == K_TAB && !ctrl) { focus_step(shift); return; }
    if (f && f->enabled) {
        switch (f->kind) {
            case W_BUTTON: if (k == K_ENTER || k == K_CTRL_BASE + ' ') activate(f); break;
            case W_CHECK: case W_RADIO: if (k == K_ENTER) activate(f); break;
            case W_SLIDER: {
                double span = f->max - f->min, st = f->step > 0 ? f->step : span / 20.0; if (shift) st = f->step > 0 ? f->step : span / 100.0;
                if (k == K_LEFT || k == K_DOWN) slider_set(f, f->val - st, true); else if (k == K_RIGHT || k == K_UP) slider_set(f, f->val + st, true);
                else if (k == K_HOME) slider_set(f, f->min, true); else if (k == K_END) slider_set(f, f->max, true);
                break;
            }
            case W_LIST:
                if (k == K_UP) list_select(f, f->sel < 0 ? 0 : f->sel - 1, true); else if (k == K_DOWN) list_select(f, f->sel + 1, true);
                else if (k == K_HOME) list_select(f, 0, true); else if (k == K_END) list_select(f, f->nitems - 1, true);
                else if (k == K_PGUP) list_select(f, f->sel - imax((f->h - 2) / row_h(f) - 1, 1), true);
                else if (k == K_PGDN) list_select(f, imin(f->sel + imax((f->h - 2) / row_h(f) - 1, 1), f->nitems - 1), true);
                else if (k == K_ENTER && f->sel >= 0) { KcValue a[2] = { kc_int(f->id), kc_int(f->sel) }; fire(f->id, EV_ACTIVATE, a, 2); }
                break;
            case W_DROPDOWN:
                if (k == K_UP) dropdown_select(f, f->sel - 1); else if (k == K_DOWN) dropdown_select(f, f->sel + 1);
                else if (k == K_ENTER) { G.popup = f->id; mark_dirty(); }
                break;
            case W_TABS: if (k == K_LEFT) tabs_select(f, f->sel - 1); else if (k == K_RIGHT) tabs_select(f, f->sel + 1); break;
            case W_TEXT: {
                int len = (int)strlen(f->text), a, b; sel_range(f, &a, &b);
                if (k == K_LEFT) tb_move(f, (a != b && !shift) ? a : u8_prev(f->text, f->cur), shift);
                else if (k == K_RIGHT) tb_move(f, (a != b && !shift) ? b : u8_next(f->text, f->cur), shift);
                else if (k == K_HOME) tb_move(f, 0, shift); else if (k == K_END) tb_move(f, len, shift);
                else if (k == K_BACKSPACE && !f->readonly) { if (a == b && f->cur > 0) f->anchor = u8_prev(f->text, f->cur); tb_replace_sel(f, ""); tb_changed(f); }
                else if (k == K_DELETE && !f->readonly) { if (a == b && f->cur < len) f->anchor = u8_next(f->text, f->cur); tb_replace_sel(f, ""); tb_changed(f); }
                else if (k == K_ENTER) { KcValue ar[2] = { kc_int(f->id), kc_string(f->text) }; fire(f->id, EV_SUBMIT, ar, 2); }
                else if (ctrl && k == K_CTRL_BASE + 'a') { f->anchor = 0; f->cur = len; mark_dirty(); }
                else if (ctrl && (k == K_CTRL_BASE + 'c' || k == K_CTRL_BASE + 'x') && a != b && !f->password) {
                    free(G.clip); G.clip = kc_malloc((size_t)(b - a) + 1); memcpy(G.clip, f->text + a, (size_t)(b - a)); G.clip[b - a] = 0;
                    if (k == K_CTRL_BASE + 'x' && !f->readonly) { tb_replace_sel(f, ""); tb_changed(f); }
                }
                else if (ctrl && k == K_CTRL_BASE + 'v' && G.clip && !f->readonly) { tb_replace_sel(f, G.clip); tb_changed(f); }
                break;
            }
            default: break;
        }
    }
    fire_key(f, key_name(k), mods);
    mark_dirty();
}

static void on_char(const char *text) {
    if (G.dlg.active || G.popup >= 0) return;
    Widget *f = wget(G.focus);
    if (f && f->enabled && f->kind == W_TEXT && !f->readonly) { tb_replace_sel(f, text); tb_changed(f); }
    else if (f && f->enabled && (f->kind == W_BUTTON || f->kind == W_CHECK || f->kind == W_RADIO) && text[0] == ' ' && !text[1]) activate(f);
    fire_key(f, text[0] == ' ' && !text[1] ? "Space" : text, 0);
}

static void on_mouse_down(int x, int y, int button, int mods) {
    G.mx = x; G.my = y; G.blink_on = true; G.blink_epoch = now_ms();
    if (G.dlg.active) {
        Dialog *d = &G.dlg;
        for (int i = 0; i < d->nbtn; i++) {
            int bx = d->bx0 + i * (d->bw + 10);
            if (x >= bx && x < bx + d->bw && y >= d->by && y < d->by + d->bh) { dlg_close(i); return; }
        }
        return;
    }
    if (G.popup >= 0) {
        Widget *p = wget(G.popup); int px, py, pw, ph;
        if (p) {
            popup_geometry(p, &px, &py, &pw, &ph);
            if (x >= px && x < px + pw && y >= py && y < py + ph) { int idx = (y - py - 1) / row_h(p); G.popup = -1; dropdown_select(p, idx); mark_dirty(); return; }
        }
        G.popup = -1; mark_dirty();
        if (p && x >= p->x && x < p->x + p->w && y >= p->y && y < p->y + p->h) return;   /* click on the closed-over header just closes it */
    }
    int id = hit(G.ws[0], x, y); Widget *w = wget(id);
    if (w && !w->enabled) w = NULL;
    if (w && (focusable(w) || (w->kind == W_CANVAS && (w->has_cb[EV_KEY])))) set_focus(w->id); else if (!w || w->kind == W_PANEL || w->kind == W_LABEL) set_focus(-1);
    if (w && button == 1) {
        switch (w->kind) {
            case W_BUTTON: case W_CHECK: case W_RADIO: w->pressed = true; G.capture = w->id; break;
            case W_SLIDER: w->pressed = true; G.capture = w->id; slider_from_x(w, x); break;
            case W_TEXT: {
                int pos = tb_byte_at_x(w, x); w->cur = pos; if (!(mods & M_SHIFT)) w->anchor = pos; G.capture = w->id; reveal_caret(w); break;
            }
            case W_LIST: {
                int idx = w->scroll + (y - w->y - 1) / row_h(w); long t = now_ms();
                if (idx >= 0 && idx < w->nitems) {
                    bool dbl = G.last_click_id == w->id && G.last_click_idx == idx && t - G.last_click_ms < 400;
                    list_select(w, idx, true);
                    if (dbl) { KcValue a[2] = { kc_int(w->id), kc_int(idx) }; fire(w->id, EV_ACTIVATE, a, 2); G.last_click_id = -1; }
                    else { G.last_click_id = w->id; G.last_click_idx = idx; G.last_click_ms = t; }
                }
                break;
            }
            case W_DROPDOWN: G.popup = w->id; mark_dirty(); break;
            case W_TABS: {
                if (y < w->y + tab_bar_h(w)) {
                    int tx = w->x, fs = wfs(w);
                    for (int i = 0; i < w->nitems; i++) { int tw = text_w(w->items[i], fs) + 28; if (x >= tx && x < tx + tw) { tabs_select(w, i); break; } tx += tw; }
                }
                break;
            }
            case W_CANVAS: G.capture = w->id; break;
            default: break;
        }
    }
    if (w) {
        KcValue a[4] = { kc_int(w->id), kc_int(x - w->x), kc_int(y - w->y), kc_int(button) };
        if (w->kind == W_CANVAS || w->has_cb[EV_MOUSE_DOWN]) fire(w->id, EV_MOUSE_DOWN, a, 4);
    }
    KcValue r[4] = { kc_int(0), kc_int(x), kc_int(y), kc_int(button) }; fire(0, EV_MOUSE_DOWN, r, 4);
    mark_dirty();
}

static void on_mouse_up(int x, int y, int button) {
    G.mx = x; G.my = y;
    if (G.dlg.active) return;
    int cap = G.capture; G.capture = -1;
    Widget *w = wget(cap);
    if (w) {
        bool inside = x >= w->x && x < w->x + w->w && y >= w->y && y < w->y + w->h;
        bool was = w->pressed; w->pressed = false; mark_dirty();
        if (was && inside && (w->kind == W_BUTTON || w->kind == W_CHECK || w->kind == W_RADIO)) activate(w);
        KcValue a[4] = { kc_int(w->id), kc_int(x - w->x), kc_int(y - w->y), kc_int(button) };
        if (w->kind == W_CANVAS || w->has_cb[EV_MOUSE_UP]) fire(w->id, EV_MOUSE_UP, a, 4);
    }
    KcValue r[4] = { kc_int(0), kc_int(x), kc_int(y), kc_int(button) }; fire(0, EV_MOUSE_UP, r, 4);
}

static void on_mouse_move(int x, int y) {
    G.mx = x; G.my = y;
    if (G.dlg.active) { mark_dirty(); return; }
    Widget *c = wget(G.capture);
    if (c) {
        if (c->kind == W_SLIDER) slider_from_x(c, x);
        else if (c->kind == W_TEXT) { c->cur = tb_byte_at_x(c, x); reveal_caret(c); mark_dirty(); }
        else if (c->kind == W_BUTTON) { bool inside = x >= c->x && x < c->x + c->w && y >= c->y && y < c->y + c->h; if (c->pressed != inside) { c->pressed = inside; mark_dirty(); } }
    }
    int id = G.popup >= 0 ? -2 : hit(G.ws[0], x, y);
    if (id != G.hover) {
        Widget *o = wget(G.hover); if (o) o->hover = false;
        G.hover = id; Widget *n = wget(id); if (n) n->hover = true; mark_dirty();
    }
    if (G.popup >= 0) mark_dirty();
    Widget *w = c ? c : wget(id);
    if (w && (w->kind == W_CANVAS || w->has_cb[EV_MOUSE_MOVE])) { KcValue a[3] = { kc_int(w->id), kc_int(x - w->x), kc_int(y - w->y) }; fire(w->id, EV_MOUSE_MOVE, a, 3); }
    KcValue r[3] = { kc_int(0), kc_int(x), kc_int(y) }; fire(0, EV_MOUSE_MOVE, r, 3);
}

static void on_wheel(int x, int y, int dir) {
    if (G.dlg.active || G.popup >= 0) return;
    int id = hit(G.ws[0], x, y); Widget *w = wget(id);
    if (w && w->kind == W_LIST) { int vis = imax((w->h - 2) / row_h(w), 1); w->scroll = iclamp(w->scroll - dir * 3, 0, imax(w->nitems - vis, 0)); mark_dirty(); }
    else if (w && w->kind == W_SLIDER && w->enabled) { slider_set(w, w->val + dir * (w->step > 0 ? w->step : (w->max - w->min) / 20.0), true); }
    if (w && w->has_cb[EV_WHEEL]) { KcValue a[4] = { kc_int(w->id), kc_int(dir), kc_int(x - w->x), kc_int(y - w->y) }; fire(w->id, EV_WHEEL, a, 4); }
    KcValue r[4] = { kc_int(0), kc_int(dir), kc_int(x), kc_int(y) }; fire(0, EV_WHEEL, r, 4);
}

/* ============================================================ frame/loop */

static void fb_alloc(int w, int h) {
    free(G.fb); G.fb_w = imax(w, 1); G.fb_h = imax(h, 1);
    G.fb = kc_malloc(sizeof(uint32_t) * (size_t)G.fb_w * (size_t)G.fb_h);
}

static void ensure_backend(void) {
    if (G.backend_open || G.headless) return;
    const char *hl = getenv("KC_GUI_HEADLESS");
    if (hl && hl[0] && hl[0] != '0') { G.headless = true; return; }
    if (be_open(G.title, G.win_w, G.win_h)) { G.backend_open = true; return; }
    fprintf(stderr, "gui: no display available - running headless (gui.screenshot() still works)\n");
    G.headless = true;
}
static void frame(bool force) {
    if (!G.created) return;
    if (!G.dirty && !force) return;
    relayout();
    G.dirty = false;
    paint_all();
    if (G.backend_open) be_present();
}

static void do_resize(int w, int h) {
    G.win_w = imax(w, 1); G.win_h = imax(h, 1); fb_alloc(G.win_w, G.win_h);
    KcValue a[3] = { kc_int(0), kc_int(G.win_w), kc_int(G.win_h) }; fire(0, EV_RESIZE, a, 3);
    mark_dirty();
}

static void process_event(const Ev *e) {
    switch (e->type) {
        case E_MOVE: on_mouse_move(e->x, e->y); break;
        case E_DOWN: on_mouse_down(e->x, e->y, e->button, e->mods); break;
        case E_UP: on_mouse_up(e->x, e->y, e->button); break;
        case E_WHEEL: on_wheel(e->x, e->y, e->button); break;
        case E_KEY: on_key(e->key, e->mods); break;
        case E_CHAR: on_char(e->text); break;
        case E_RESIZE: do_resize(e->x, e->y); break;
        case E_EXPOSE: mark_dirty(); break;
        case E_CLOSE: {
            Widget *r = G.ws[0]; bool allow = true;
            if (r->has_cb[EV_CLOSE]) { KcValue a[1] = { kc_int(0) }; KcValue v = fire(0, EV_CLOSE, a, 1); if (v.type == VAL_BOOL && !v.as.b) allow = false; }
            if (allow) G.quit = true;
            break;
        }
    }
}

static void run_timers(void) {
    long t = now_ms();
    for (int i = 0; i < G.ntimers; i++) {
        if (!G.timers[i].active || t < G.timers[i].next) continue;
        int id = G.timers[i].id; KcValue cb = G.timers[i].cb;
        if (G.timers[i].repeat) { G.timers[i].next += G.timers[i].interval; if (G.timers[i].next <= t) G.timers[i].next = t + G.timers[i].interval; }
        else G.timers[i].active = false;
        KcValue a[1] = { kc_int(id) };
        kc_call_value(G.ip, cb, a, 1, G.pos);
        mark_dirty();
        if (G.quit) return;
    }
}
static void blink_tick(void) {
    long t = now_ms(); bool on = ((t - G.blink_epoch) / 530) % 2 == 0;
    if (on != G.blink_on) { G.blink_on = on; Widget *f = wget(G.focus); if (f && f->kind == W_TEXT) mark_dirty(); }
}
static int next_timer_delay(void) {
    long t = now_ms(); int best = 100;
    for (int i = 0; i < G.ntimers; i++) if (G.timers[i].active) { long d = G.timers[i].next - t; if (d < 0) d = 0; if (d < best) best = (int)d; }
    return best;
}
static void pump_events(void) {
    Ev e;
    while (!G.quit && be_poll(&e)) process_event(&e);
}

/* ================================================================== API */

#define ENTER() do { G.ip = ip; G.pos = pos; } while (0)

static void usage(KcPos pos, const char *sig) { kc_error_at(pos, "usage: gui.%s", sig); }
static void argc_check(int n, int lo, int hi, KcPos pos, const char *sig) { if (n < lo || n > hi) usage(pos, sig); }
static int arg_int(KcValue v, KcPos pos, const char *who) {
    if (v.type == VAL_INT) return (int)v.as.i;
    if (v.type == VAL_FLOAT) return (int)floor(v.as.f + 0.5);
    kc_error_at(pos, "gui.%s: expected a number, got %s", who, kc_type_name(v)); return 0;
}
static double arg_num(KcValue v, KcPos pos, const char *who) {
    if (v.type == VAL_FLOAT) return v.as.f; if (v.type == VAL_INT) return (double)v.as.i;
    kc_error_at(pos, "gui.%s: expected a number, got %s", who, kc_type_name(v)); return 0;
}
static const char *arg_str(KcValue v, KcPos pos, const char *who) {
    if (v.type != VAL_STRING) kc_error_at(pos, "gui.%s: expected a string, got %s", who, kc_type_name(v));
    return v.as.s->data;
}
static bool arg_bool(KcValue v, KcPos pos, const char *who) {
    if (v.type == VAL_BOOL) return v.as.b; if (v.type == VAL_INT) return v.as.i != 0;
    kc_error_at(pos, "gui.%s: expected true/false, got %s", who, kc_type_name(v)); return false;
}
static bool is_callable(KcValue v) { return v.type == VAL_CLOSURE || v.type == VAL_NATIVE || v.type == VAL_EXTERN; }
static uint32_t arg_color(KcValue v, KcPos pos, const char *who) {
    if (v.type == VAL_INT) return (uint32_t)v.as.i & 0xFFFFFF;
    if (v.type == VAL_STRING) {
        const char *s = v.as.s->data; if (*s == '#') s++;
        char *end; unsigned long c = strtoul(s, &end, 16);
        if (*end == 0 && strlen(s) == 6) return (uint32_t)c;
        if (*end == 0 && strlen(s) == 3) return (uint32_t)(((c >> 8 & 15) * 17) << 16 | ((c >> 4 & 15) * 17) << 8 | (c & 15) * 17);
    }
    kc_error_at(pos, "gui.%s: color must be an int like 0xFF8800 or a string like \"#ff8800\"", who); return 0;
}
static void need_window(KcPos pos) { if (!G.created) kc_error_at(pos, "gui: no window yet - call gui.window(title, width, height) first"); }
static Widget *arg_widget(KcValue v, KcPos pos, const char *who) {
    need_window(pos);
    if (v.type != VAL_INT) kc_error_at(pos, "gui.%s: expected a widget id, got %s", who, kc_type_name(v));
    Widget *w = wget((int)v.as.i);
    if (!w) kc_error_at(pos, "gui.%s: no widget with id %lld (removed or never created)", who, (long long)v.as.i);
    return w;
}
static Widget *arg_parent(KcValue v, KcPos pos, const char *who) {
    Widget *w = arg_widget(v, pos, who);
    if (w->kind != W_PANEL) kc_error_at(pos, "gui.%s: parent %d is not a container (use gui.vbox/hbox/grid/absolute/panel ids; 0 is the window)", who, w->id);
    return w;
}
static void set_cb(Widget *w, int ev, KcValue cb, KcPos pos, const char *who) {
    if (cb.type == VAL_NULL) { w->has_cb[ev] = false; return; }
    if (!is_callable(cb)) kc_error_at(pos, "gui.%s: callback must be a function, got %s", who, kc_type_name(cb));
    w->cb[ev] = cb; w->has_cb[ev] = true;
}
static void set_items_from(Widget *w, KcValue arr, KcPos pos, const char *who) {
    if (arr.type != VAL_ARRAY) kc_error_at(pos, "gui.%s: items must be an array", who);
    free_items(w);
    for (int i = 0; i < arr.as.arr->count; i++) { char *s = kc_to_display_string(arr.as.arr->items[i]); items_add(w, s); free(s); }
    if (w->kind == W_LIST || w->kind == W_DROPDOWN) { if (w->sel >= w->nitems) w->sel = w->nitems - 1; }
    mark_dirty();
}
static void set_text(Widget *w, const char *s) {
    free(w->text); w->text = sdup(s);
    if (w->kind == W_TEXT) { w->cur = w->anchor = (int)strlen(s); w->hscroll = 0; }
    mark_dirty();
}

/* ---- window ---- */

static KcValue g_window(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    ENTER(); argc_check(n, 3, 3, pos, "window(title, width, height)");
    const char *t = arg_str(a[0], pos, "window"); int w = arg_int(a[1], pos, "window"), h = arg_int(a[2], pos, "window");
    if (w < 80 || h < 60 || w > 8192 || h > 8192) kc_error_at(pos, "gui.window: size must be between 80x60 and 8192x8192");
    if (G.created) { for (int i = G.nws - 1; i >= 1; i--) kill_widget(i); dlg_free(); G.popup = -1; G.ntimers = 0; }
    else {
        G.th = THEME_DARK; G.created = true;
        Widget *r = widget_new(W_PANEL, -1); r->pad = 12; r->spacing = 8;
    }
    free(G.title); G.title = sdup(t); G.win_w = w; G.win_h = h; G.focus = G.hover = G.capture = -1;
    fb_alloc(w, h); if (G.backend_open) be_set_title(G.title);
    mark_dirty(); return kc_int(0);
}
static KcValue g_title(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    ENTER(); argc_check(n, 1, 1, pos, "title(text)"); need_window(pos);
    free(G.title); G.title = sdup(arg_str(a[0], pos, "title")); if (G.backend_open) be_set_title(G.title); return kc_null();
}
static KcValue g_size(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    ENTER(); need_window(pos); KcValue r = kc_array_new(); kc_array_push(r, kc_int(G.win_w)); kc_array_push(r, kc_int(G.win_h)); return r;
}
static KcValue g_theme(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    ENTER(); argc_check(n, 1, 1, pos, "theme(\"dark\" | \"light\")"); need_window(pos);
    const char *s = arg_str(a[0], pos, "theme");
    if (strcmp(s, "dark") == 0) G.th = THEME_DARK; else if (strcmp(s, "light") == 0) G.th = THEME_LIGHT;
    else kc_error_at(pos, "gui.theme: unknown theme '%s' (use \"dark\" or \"light\")", s);
    mark_dirty(); return kc_null();
}
static KcValue g_scale(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    ENTER(); argc_check(n, 1, 1, pos, "scale(1..4)  (font scale: 8px glyphs x n)");
    int s = arg_int(a[0], pos, "scale"); if (s < 1 || s > 4) kc_error_at(pos, "gui.scale: must be 1..4");
    G.fs = s; mark_dirty(); return kc_null();
}
static KcValue g_headless(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    ENTER(); argc_check(n, 1, 1, pos, "headless(on)");
    bool on = arg_bool(a[0], pos, "headless");
    if (on && G.backend_open) { be_close(); G.backend_open = false; }
    G.headless = on; return kc_null();
}
static KcValue g_is_headless(KcInterp *ip, KcValue *a, int n, KcPos pos) { ENTER(); ensure_backend(); return kc_bool(G.headless); }

/* ---- running ---- */

static void shutdown_backend(void) { if (G.backend_open) { be_close(); G.backend_open = false; } }

static KcValue g_run(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    ENTER(); argc_check(n, 0, 0, pos, "run()"); need_window(pos); kc_forbid_in_comptime(ip, pos, "gui.run");
    ensure_backend(); frame(true);
    if (G.headless) { run_timers(); frame(false); G.quit = false; return kc_null(); }
    G.blink_epoch = now_ms();
    while (!G.quit) {
        pump_events(); if (G.quit) break;
        run_timers(); if (G.quit) break;
        blink_tick();
        if (G.dirty) frame(false); else be_idle_wait(imin(next_timer_delay(), 50));
    }
    G.quit = false; shutdown_backend();
    return kc_null();
}
static KcValue g_poll(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    ENTER(); argc_check(n, 0, 0, pos, "poll() -> bool (false once the window is closed)"); need_window(pos); kc_forbid_in_comptime(ip, pos, "gui.poll");
    ensure_backend();
    if (!G.headless) { pump_events(); if (!G.quit) { run_timers(); blink_tick(); } }
    if (G.quit) { G.quit = false; shutdown_backend(); return kc_bool(false); }
    frame(false);
    if (!G.headless && !G.dirty) be_idle_wait(4);
    return kc_bool(true);
}
static KcValue g_update(KcInterp *ip, KcValue *a, int n, KcPos pos) { ENTER(); need_window(pos); frame(true); return kc_null(); }
static KcValue g_quit(KcInterp *ip, KcValue *a, int n, KcPos pos) { ENTER(); G.quit = true; return kc_null(); }
static KcValue g_invalidate(KcInterp *ip, KcValue *a, int n, KcPos pos) { ENTER(); mark_dirty(); return kc_null(); }
static KcValue g_now(KcInterp *ip, KcValue *a, int n, KcPos pos) { ENTER(); return kc_int(now_ms()); }

static int timer_add(KcValue cb, long ms, bool repeat) {
    if (G.ntimers == G.timer_cap) { G.timer_cap = G.timer_cap ? G.timer_cap * 2 : 8; G.timers = kc_realloc(G.timers, sizeof(Timer) * (size_t)G.timer_cap); }
    Timer *t = &G.timers[G.ntimers++]; t->id = G.next_timer++; t->interval = ms; t->next = now_ms() + ms; t->repeat = repeat; t->active = true; t->cb = cb;
    return t->id;
}
static KcValue g_timer(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    ENTER(); argc_check(n, 2, 3, pos, "timer(ms, fn[, repeat=true]) -> timer id"); need_window(pos);
    int ms = arg_int(a[0], pos, "timer"); if (ms < 1) kc_error_at(pos, "gui.timer: interval must be >= 1 ms");
    if (!is_callable(a[1])) kc_error_at(pos, "gui.timer: second argument must be a function");
    return kc_int(timer_add(a[1], ms, n == 3 ? arg_bool(a[2], pos, "timer") : true));
}
static KcValue g_after(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    ENTER(); argc_check(n, 2, 2, pos, "after(ms, fn) -> timer id  (one-shot)"); need_window(pos);
    int ms = arg_int(a[0], pos, "after"); if (ms < 0) ms = 0;
    if (!is_callable(a[1])) kc_error_at(pos, "gui.after: second argument must be a function");
    return kc_int(timer_add(a[1], ms < 1 ? 1 : ms, false));
}
static KcValue g_cancel_timer(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    ENTER(); argc_check(n, 1, 1, pos, "cancel_timer(id)"); int id = arg_int(a[0], pos, "cancel_timer");
    for (int i = 0; i < G.ntimers; i++) if (G.timers[i].id == id) G.timers[i].active = false;
    return kc_null();
}
/* Headless time travel: advance the virtual clock, firing due timers along the way. */
static KcValue g_advance(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    ENTER(); argc_check(n, 1, 1, pos, "advance(ms)  (headless/test: moves the virtual clock and fires timers)"); need_window(pos);
    ensure_backend();
    if (!G.headless) kc_error_at(pos, "gui.advance: only available in headless mode (gui.headless(true))");
    int ms = arg_int(a[0], pos, "advance"); long end = G.vclock + ms;
    while (G.vclock < end) { G.vclock++; run_timers(); if (G.quit) break; }
    frame(true); return kc_null();
}

/* ---- widget creation ---- */

static KcValue made(Widget *w) { return kc_int(w->id); }

static KcValue g_panel_common(KcInterp *ip, KcValue *a, int n, KcPos pos, int layout, const char *sig, int title_idx, int cols) {
    ENTER(); argc_check(n, title_idx, title_idx + 1, pos, sig);
    Widget *par = arg_parent(a[0], pos, "panel"); Widget *w = widget_new(W_PANEL, par->id);
    w->layout = layout; w->cols = cols;
    if (n > title_idx && a[title_idx].type == VAL_STRING) { set_text(w, a[title_idx].as.s->data); w->pad = 10; }
    return made(w);
}
static KcValue g_vbox(KcInterp *ip, KcValue *a, int n, KcPos pos) { return g_panel_common(ip, a, n, pos, L_VBOX, "vbox(parent[, title])", 1, 2); }
static KcValue g_hbox(KcInterp *ip, KcValue *a, int n, KcPos pos) { return g_panel_common(ip, a, n, pos, L_HBOX, "hbox(parent[, title])", 1, 2); }
static KcValue g_absolute(KcInterp *ip, KcValue *a, int n, KcPos pos) { return g_panel_common(ip, a, n, pos, L_ABS, "absolute(parent[, title])", 1, 2); }
static KcValue g_grid(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    ENTER(); argc_check(n, 2, 3, pos, "grid(parent, columns[, title])");
    int cols = arg_int(a[1], pos, "grid"); if (cols < 1 || cols > 64) kc_error_at(pos, "gui.grid: columns must be 1..64");
    Widget *par = arg_parent(a[0], pos, "grid"); Widget *w = widget_new(W_PANEL, par->id); w->layout = L_GRID; w->cols = cols;
    if (n == 3 && a[2].type == VAL_STRING) { set_text(w, a[2].as.s->data); w->pad = 10; }
    return made(w);
}
static KcValue g_label(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    ENTER(); argc_check(n, 2, 2, pos, "label(parent, text)");
    Widget *par = arg_parent(a[0], pos, "label"); Widget *w = widget_new(W_LABEL, par->id);
    char *s = kc_to_display_string(a[1]); set_text(w, s); free(s); return made(w);
}
static KcValue g_button(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    ENTER(); argc_check(n, 2, 3, pos, "button(parent, text[, on_click])");
    Widget *par = arg_parent(a[0], pos, "button"); Widget *w = widget_new(W_BUTTON, par->id);
    set_text(w, arg_str(a[1], pos, "button")); if (n == 3) set_cb(w, EV_CLICK, a[2], pos, "button"); return made(w);
}
static KcValue g_checkbox(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    ENTER(); argc_check(n, 2, 4, pos, "checkbox(parent, text[, checked[, on_change(id, checked)]])");
    Widget *par = arg_parent(a[0], pos, "checkbox"); Widget *w = widget_new(W_CHECK, par->id);
    set_text(w, arg_str(a[1], pos, "checkbox")); if (n >= 3) w->checked = arg_bool(a[2], pos, "checkbox"); if (n == 4) set_cb(w, EV_CHANGE, a[3], pos, "checkbox");
    return made(w);
}
static KcValue g_radio(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    ENTER(); argc_check(n, 3, 5, pos, "radio(parent, group, text[, checked[, on_change(id, checked)]])");
    Widget *par = arg_parent(a[0], pos, "radio"); Widget *w = widget_new(W_RADIO, par->id);
    free(w->group); w->group = sdup(arg_str(a[1], pos, "radio")); set_text(w, arg_str(a[2], pos, "radio"));
    if (n >= 4 && arg_bool(a[3], pos, "radio")) set_checked_radio(w);
    if (n == 5) set_cb(w, EV_CHANGE, a[4], pos, "radio"); return made(w);
}
static KcValue g_textbox(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    ENTER(); argc_check(n, 1, 3, pos, "textbox(parent[, text[, on_change(id, text)]])");
    Widget *par = arg_parent(a[0], pos, "textbox"); Widget *w = widget_new(W_TEXT, par->id);
    if (n >= 2 && a[1].type != VAL_NULL) { char *s = kc_to_display_string(a[1]); set_text(w, s); free(s); }
    if (n == 3) set_cb(w, EV_CHANGE, a[2], pos, "textbox"); return made(w);
}
static KcValue g_slider(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    ENTER(); argc_check(n, 4, 5, pos, "slider(parent, min, max, value[, on_change(id, value)])");
    Widget *par = arg_parent(a[0], pos, "slider"); Widget *w = widget_new(W_SLIDER, par->id);
    w->min = arg_num(a[1], pos, "slider"); w->max = arg_num(a[2], pos, "slider"); if (w->max <= w->min) kc_error_at(pos, "gui.slider: max must be greater than min");
    double v = arg_num(a[3], pos, "slider"); w->val = v < w->min ? w->min : (v > w->max ? w->max : v);
    if (n == 5) set_cb(w, EV_CHANGE, a[4], pos, "slider"); return made(w);
}
static KcValue g_progress(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    ENTER(); argc_check(n, 2, 2, pos, "progress(parent, fraction 0..1)");
    Widget *par = arg_parent(a[0], pos, "progress"); Widget *w = widget_new(W_PROGRESS, par->id); w->val = arg_num(a[1], pos, "progress"); return made(w);
}
static KcValue g_list(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    ENTER(); argc_check(n, 2, 3, pos, "list(parent, items[, on_select(id, index)])");
    Widget *par = arg_parent(a[0], pos, "list"); Widget *w = widget_new(W_LIST, par->id);
    set_items_from(w, a[1], pos, "list"); if (n == 3) set_cb(w, EV_SELECT, a[2], pos, "list"); return made(w);
}
static KcValue g_dropdown(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    ENTER(); argc_check(n, 2, 4, pos, "dropdown(parent, items[, selected[, on_change(id, index)]])");
    Widget *par = arg_parent(a[0], pos, "dropdown"); Widget *w = widget_new(W_DROPDOWN, par->id);
    set_items_from(w, a[1], pos, "dropdown"); w->sel = w->nitems ? 0 : -1;
    if (n >= 3 && a[2].type != VAL_NULL) { int s = arg_int(a[2], pos, "dropdown"); if (s >= 0 && s < w->nitems) w->sel = s; }
    if (n == 4) set_cb(w, EV_CHANGE, a[3], pos, "dropdown"); return made(w);
}
static KcValue g_tabs(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    ENTER(); argc_check(n, 2, 3, pos, "tabs(parent, names[, on_change(id, index)])  - then gui.page(tabs, i) for each page");
    Widget *par = arg_parent(a[0], pos, "tabs"); Widget *w = widget_new(W_TABS, par->id);
    set_items_from(w, a[1], pos, "tabs"); w->sel = 0; int id = w->id;
    for (int i = 0; i < w->nitems; i++) { Widget *pg = widget_new(W_PANEL, id); pg->pad = 12; }
    w = wget(id); if (n == 3) set_cb(w, EV_CHANGE, a[2], pos, "tabs"); return kc_int(id);
}
static KcValue g_page(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    ENTER(); argc_check(n, 2, 2, pos, "page(tabs, index) -> container id");
    Widget *t = arg_widget(a[0], pos, "page"); if (t->kind != W_TABS) kc_error_at(pos, "gui.page: %d is not a tabs widget", t->id);
    int idx = arg_int(a[1], pos, "page"), k = 0;
    for (int i = 1; i < G.nws; i++) if (G.ws[i]->kind != W_DEAD && G.ws[i]->parent == t->id) { if (k == idx) return kc_int(i); k++; }
    kc_error_at(pos, "gui.page: tabs has %d page(s), asked for %d", k, idx); return kc_null();
}
static KcValue g_canvas(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    ENTER(); argc_check(n, 3, 4, pos, "canvas(parent, width, height[, on_draw(id, w, h)])");
    Widget *par = arg_parent(a[0], pos, "canvas"); Widget *w = widget_new(W_CANVAS, par->id);
    w->req_w = arg_int(a[1], pos, "canvas"); w->req_h = arg_int(a[2], pos, "canvas"); if (n == 4) set_cb(w, EV_DRAW, a[3], pos, "canvas"); return made(w);
}
static KcValue g_separator(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    ENTER(); argc_check(n, 1, 1, pos, "separator(parent)"); Widget *par = arg_parent(a[0], pos, "separator"); return made(widget_new(W_SEP, par->id));
}
static KcValue g_spacer(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    ENTER(); argc_check(n, 1, 1, pos, "spacer(parent)  (flexible empty space)"); Widget *par = arg_parent(a[0], pos, "spacer");
    Widget *w = widget_new(W_SPACER, par->id); w->weight = 1; return made(w);
}

/* ---- properties ---- */

static KcValue arr_of_items(const Widget *w) { KcValue r = kc_array_new(); for (int i = 0; i < w->nitems; i++) kc_array_push(r, kc_string(w->items[i])); return r; }

static KcValue g_set(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    ENTER(); argc_check(n, 3, 3, pos, "set(id, property, value)");
    Widget *w = arg_widget(a[0], pos, "set"); const char *p = arg_str(a[1], pos, "set"); KcValue v = a[2];
    if (!strcmp(p, "text") || !strcmp(p, "title")) { char *s = kc_to_display_string(v); set_text(w, s); free(s); if (w->kind == W_PANEL && w->text[0] && w->pad < 10) w->pad = 10; }
    else if (!strcmp(p, "value")) {
        if (w->kind == W_SLIDER) slider_set(w, arg_num(v, pos, "set"), false);
        else if (w->kind == W_PROGRESS) { w->val = arg_num(v, pos, "set"); mark_dirty(); }
        else if (w->kind == W_TEXT) { char *s = kc_to_display_string(v); set_text(w, s); free(s); }
        else if (w->kind == W_CHECK) { w->checked = arg_bool(v, pos, "set"); mark_dirty(); }
        else if (w->kind == W_RADIO) { if (arg_bool(v, pos, "set")) { for (int i = 1; i < G.nws; i++) { Widget *o = G.ws[i]; if (o->kind == W_RADIO && o->parent == w->parent && !strcmp(o->group, w->group)) o->checked = false; } w->checked = true; } else w->checked = false; mark_dirty(); }
        else if (w->kind == W_LIST || w->kind == W_DROPDOWN || w->kind == W_TABS) goto selected;
        else kc_error_at(pos, "gui.set: 'value' is not valid for this widget");
    }
    else if (!strcmp(p, "checked")) { w->checked = arg_bool(v, pos, "set"); mark_dirty(); }
    else if (!strcmp(p, "selected")) {
        selected: { int i = arg_int(v, pos, "set");
        if (w->kind == W_TABS) { if (i >= 0 && i < w->nitems) { w->sel = i; mark_dirty(); } }
        else if (w->kind == W_LIST) { if (i >= -1 && i < w->nitems) { w->sel = i; if (i >= 0) list_reveal(w); mark_dirty(); } }
        else if (w->kind == W_DROPDOWN) { if (i >= -1 && i < w->nitems) { w->sel = i; mark_dirty(); } }
        else kc_error_at(pos, "gui.set: 'selected' needs a list, dropdown or tabs widget"); }
    }
    else if (!strcmp(p, "enabled")) { w->enabled = arg_bool(v, pos, "set"); mark_dirty(); }
    else if (!strcmp(p, "visible")) { w->visible = arg_bool(v, pos, "set"); mark_dirty(); }
    else if (!strcmp(p, "min")) { w->min = arg_num(v, pos, "set"); mark_dirty(); }
    else if (!strcmp(p, "max")) { w->max = arg_num(v, pos, "set"); mark_dirty(); }
    else if (!strcmp(p, "step")) { w->step = arg_num(v, pos, "set"); if (w->step < 0) w->step = 0; }
    else if (!strcmp(p, "items")) { set_items_from(w, v, pos, "set"); }
    else if (!strcmp(p, "weight")) { w->weight = imax(arg_int(v, pos, "set"), 0); mark_dirty(); }
    else if (!strcmp(p, "width")) { w->req_w = imax(arg_int(v, pos, "set"), 0); mark_dirty(); }
    else if (!strcmp(p, "height")) { w->req_h = imax(arg_int(v, pos, "set"), 0); mark_dirty(); }
    else if (!strcmp(p, "x")) { w->ax = arg_int(v, pos, "set"); mark_dirty(); }
    else if (!strcmp(p, "y")) { w->ay = arg_int(v, pos, "set"); mark_dirty(); }
    else if (!strcmp(p, "padding")) { w->pad = imax(arg_int(v, pos, "set"), 0); mark_dirty(); }
    else if (!strcmp(p, "spacing")) { w->spacing = imax(arg_int(v, pos, "set"), 0); mark_dirty(); }
    else if (!strcmp(p, "cols") || !strcmp(p, "columns")) { w->cols = iclamp(arg_int(v, pos, "set"), 1, 64); mark_dirty(); }
    else if (!strcmp(p, "scale")) { w->fs = iclamp(arg_int(v, pos, "set"), 0, 4); mark_dirty(); }
    else if (!strcmp(p, "color")) { w->fg = arg_color(v, pos, "set"); w->has_fg = true; mark_dirty(); }
    else if (!strcmp(p, "bg")) { w->bg = arg_color(v, pos, "set"); w->has_bg = true; mark_dirty(); }
    else if (!strcmp(p, "password")) { w->password = arg_bool(v, pos, "set"); mark_dirty(); }
    else if (!strcmp(p, "readonly")) { w->readonly = arg_bool(v, pos, "set"); }
    else if (!strcmp(p, "placeholder")) { free(w->placeholder); w->placeholder = sdup(arg_str(v, pos, "set")); mark_dirty(); }
    else if (!strcmp(p, "group")) { free(w->group); w->group = sdup(arg_str(v, pos, "set")); }
    else if (!strcmp(p, "align")) {
        const char *s = arg_str(v, pos, "set");
        w->cross = !strcmp(s, "start") ? CROSS_START : !strcmp(s, "center") ? CROSS_CENTER : !strcmp(s, "end") ? CROSS_END : !strcmp(s, "fill") ? CROSS_FILL :
                   !strcmp(s, "auto") ? CROSS_AUTO : (kc_error_at(pos, "gui.set: align must be start|center|end|fill|auto"), 0);
        mark_dirty();
    }
    else if (!strcmp(p, "text_align")) {
        const char *s = arg_str(v, pos, "set");
        w->talign = !strcmp(s, "left") ? 0 : !strcmp(s, "center") ? 1 : !strcmp(s, "right") ? 2 : (kc_error_at(pos, "gui.set: text_align must be left|center|right"), 0);
        mark_dirty();
    }
    else kc_error_at(pos, "gui.set: unknown property '%s'", p);
    return kc_null();
}
static KcValue g_get(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    ENTER(); argc_check(n, 2, 2, pos, "get(id, property)");
    Widget *w = arg_widget(a[0], pos, "get"); const char *p = arg_str(a[1], pos, "get");
    if (!strcmp(p, "text") || !strcmp(p, "title")) return kc_string(w->text);
    if (!strcmp(p, "value")) {
        if (w->kind == W_SLIDER || w->kind == W_PROGRESS) return kc_float(w->val);
        if (w->kind == W_TEXT) return kc_string(w->text);
        if (w->kind == W_CHECK || w->kind == W_RADIO) return kc_bool(w->checked);
        if (w->kind == W_LIST || w->kind == W_DROPDOWN || w->kind == W_TABS) return kc_int(w->sel);
        return kc_null();
    }
    if (!strcmp(p, "checked")) return kc_bool(w->checked);
    if (!strcmp(p, "selected")) return kc_int(w->sel);
    if (!strcmp(p, "selected_text")) return (w->sel >= 0 && w->sel < w->nitems) ? kc_string(w->items[w->sel]) : kc_null();
    if (!strcmp(p, "items")) return arr_of_items(w);
    if (!strcmp(p, "count")) return kc_int(w->nitems);
    if (!strcmp(p, "enabled")) return kc_bool(w->enabled);
    if (!strcmp(p, "visible")) return kc_bool(w->visible);
    if (!strcmp(p, "focused")) return kc_bool(G.focus == w->id);
    if (!strcmp(p, "min")) return kc_float(w->min);
    if (!strcmp(p, "max")) return kc_float(w->max);
    if (!strcmp(p, "x")) return kc_int(w->x); if (!strcmp(p, "y")) return kc_int(w->y);
    if (!strcmp(p, "width")) return kc_int(w->w); if (!strcmp(p, "height")) return kc_int(w->h);
    if (!strcmp(p, "caret")) return kc_int(u8_count(w->text, w->cur));
    if (!strcmp(p, "selection")) { int s0, s1; sel_range(w, &s0, &s1); KcValue r = kc_array_new(); kc_array_push(r, kc_int(u8_count(w->text, s0))); kc_array_push(r, kc_int(u8_count(w->text, s1))); return r; }
    kc_error_at(pos, "gui.get: unknown property '%s'", p); return kc_null();
}
static KcValue g_on(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    ENTER(); argc_check(n, 3, 3, pos, "on(id, event, fn)  events: click change select submit activate draw key mouse_down mouse_up mouse_move wheel resize close");
    Widget *w = arg_widget(a[0], pos, "on"); const char *e = arg_str(a[1], pos, "on");
    for (int i = 0; i < EV_COUNT; i++) if (!strcmp(e, EV_NAMES[i])) { set_cb(w, i, a[2], pos, "on"); mark_dirty(); return kc_null(); }
    kc_error_at(pos, "gui.on: unknown event '%s'", e); return kc_null();
}
static KcValue g_remove(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    ENTER(); argc_check(n, 1, 1, pos, "remove(id)"); Widget *w = arg_widget(a[0], pos, "remove");
    if (w->id == 0) kc_error_at(pos, "gui.remove: the window root cannot be removed"); kill_widget(w->id); return kc_null();
}
static KcValue g_clear(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    ENTER();
    if (n == 1 && G.draw_id < 0) {                       /* gui.clear(container): remove all children */
        Widget *w = arg_widget(a[0], pos, "clear");
        for (int i = G.nws - 1; i >= 1; i--) if (G.ws[i]->kind != W_DEAD && G.ws[i]->parent == w->id) kill_widget(i);
        return kc_null();
    }
    if (G.draw_id < 0) kc_error_at(pos, "gui.clear(color) can only be used inside a canvas draw callback (gui.clear(container) empties a container)");
    argc_check(n, 1, 1, pos, "clear(color)"); Widget *c = wget(G.draw_id);
    if (c) fb_rect(c->x, c->y, c->w, c->h, arg_color(a[0], pos, "clear")); return kc_null();
}
static KcValue g_add_item(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    ENTER(); argc_check(n, 2, 2, pos, "add_item(id, text)"); Widget *w = arg_widget(a[0], pos, "add_item");
    if (w->kind != W_LIST && w->kind != W_DROPDOWN) kc_error_at(pos, "gui.add_item: needs a list or dropdown");
    char *s = kc_to_display_string(a[1]); items_add(w, s); free(s); if (w->kind == W_DROPDOWN && w->sel < 0) w->sel = 0;
    mark_dirty(); return kc_int(w->nitems - 1);
}
static KcValue g_remove_item(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    ENTER(); argc_check(n, 2, 2, pos, "remove_item(id, index)"); Widget *w = arg_widget(a[0], pos, "remove_item");
    int i = arg_int(a[1], pos, "remove_item"); if (i < 0 || i >= w->nitems) kc_error_at(pos, "gui.remove_item: index %d out of range", i);
    free(w->items[i]); memmove(w->items + i, w->items + i + 1, sizeof(char *) * (size_t)(w->nitems - i - 1)); w->nitems--;
    if (w->sel >= w->nitems) w->sel = w->nitems - 1; mark_dirty(); return kc_null();
}
static KcValue g_place(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    ENTER(); argc_check(n, 3, 5, pos, "place(id, x, y[, width, height])  (for absolute() containers)");
    Widget *w = arg_widget(a[0], pos, "place"); w->ax = arg_int(a[1], pos, "place"); w->ay = arg_int(a[2], pos, "place");
    if (n >= 4) w->req_w = imax(arg_int(a[3], pos, "place"), 0); if (n == 5) w->req_h = imax(arg_int(a[4], pos, "place"), 0);
    mark_dirty(); return kc_null();
}
static KcValue g_focus(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    ENTER(); argc_check(n, 1, 1, pos, "focus(id)"); Widget *w = arg_widget(a[0], pos, "focus"); if (focusable(w) || w->kind == W_CANVAS) set_focus(w->id); return kc_null();
}
static KcValue g_bounds(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    ENTER(); argc_check(n, 1, 1, pos, "bounds(id) -> [x, y, w, h]  (as of the last layout)"); Widget *w = arg_widget(a[0], pos, "bounds");
    relayout(); KcValue r = kc_array_new(); kc_array_push(r, kc_int(w->x)); kc_array_push(r, kc_int(w->y)); kc_array_push(r, kc_int(w->w)); kc_array_push(r, kc_int(w->h)); return r;
}
static KcValue g_children(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    ENTER(); argc_check(n, 1, 1, pos, "children(id) -> [ids]"); Widget *w = arg_widget(a[0], pos, "children");
    KcValue r = kc_array_new(); for (int i = 1; i < G.nws; i++) if (G.ws[i]->kind != W_DEAD && G.ws[i]->parent == w->id) kc_array_push(r, kc_int(i)); return r;
}
static KcValue g_clipboard(KcInterp *ip, KcValue *a, int n, KcPos pos) { ENTER(); return kc_string(G.clip ? G.clip : ""); }
static KcValue g_set_clipboard(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    ENTER(); argc_check(n, 1, 1, pos, "set_clipboard(text)"); free(G.clip); G.clip = sdup(arg_str(a[0], pos, "set_clipboard")); return kc_null();
}

/* ---- dialogs ---- */

static void dialog_open(KcPos pos, const char *title, const char *text, KcValue btns, KcValue cb, bool as_bool) {
    need_window(pos); dlg_free();
    Dialog *d = &G.dlg; d->active = true; d->title = sdup(title); d->text = sdup(text);
    if (btns.type == VAL_ARRAY && btns.as.arr->count > 0) {
        for (int i = 0; i < btns.as.arr->count && i < 6; i++) { char *s = kc_to_display_string(btns.as.arr->items[i]); d->btn = kc_realloc(d->btn, sizeof(char *) * (size_t)(d->nbtn + 1)); d->btn[d->nbtn++] = s; }
    } else { d->btn = kc_malloc(sizeof(char *)); d->btn[0] = sdup("OK"); d->nbtn = 1; }
    d->sel = d->nbtn - 1; d->as_bool = as_bool;
    if (cb.type != VAL_NULL) { if (!is_callable(cb)) kc_error_at(pos, "gui: dialog callback must be a function"); d->cb = cb; d->has_cb = true; }
    G.popup = -1; mark_dirty();
}
static KcValue g_dialog(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    ENTER(); argc_check(n, 3, 4, pos, "dialog(title, text, buttons[, on_close(index)])  (index is -1 when dismissed with Escape)");
    dialog_open(pos, arg_str(a[0], pos, "dialog"), arg_str(a[1], pos, "dialog"), a[2], n == 4 ? a[3] : kc_null(), false); return kc_null();
}
static KcValue g_alert(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    ENTER(); argc_check(n, 2, 3, pos, "alert(title, text[, on_close])");
    dialog_open(pos, arg_str(a[0], pos, "alert"), arg_str(a[1], pos, "alert"), kc_null(), n == 3 ? a[2] : kc_null(), false); return kc_null();
}
static KcValue g_confirm(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    ENTER(); argc_check(n, 3, 3, pos, "confirm(title, text, on_answer(bool))");
    KcValue b = kc_array_new(); kc_array_push(b, kc_string("Cancel")); kc_array_push(b, kc_string("OK"));
    dialog_open(pos, arg_str(a[0], pos, "confirm"), arg_str(a[1], pos, "confirm"), b, a[2], true); return kc_null();
}

/* ---- canvas drawing (valid only inside a draw callback) ---- */

static void need_draw(KcPos pos, const char *who) { if (G.draw_id < 0) kc_error_at(pos, "gui.%s can only be called inside a canvas draw callback", who); }
static bool fill_arg(KcValue *a, int n, int idx, KcPos pos, const char *who) { return n > idx ? arg_bool(a[idx], pos, who) : true; }

static KcValue g_rect(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    ENTER(); argc_check(n, 5, 6, pos, "rect(x, y, w, h, color[, fill=true])"); need_draw(pos, "rect");
    int x = G.ox + arg_int(a[0], pos, "rect"), y = G.oy + arg_int(a[1], pos, "rect"), w = arg_int(a[2], pos, "rect"), h = arg_int(a[3], pos, "rect"); uint32_t c = arg_color(a[4], pos, "rect");
    if (fill_arg(a, n, 5, pos, "rect")) fb_rect(x, y, w, h, c); else fb_frame(x, y, w, h, c); return kc_null();
}
static KcValue g_round_rect(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    ENTER(); argc_check(n, 6, 7, pos, "round_rect(x, y, w, h, radius, color[, fill=true])"); need_draw(pos, "round_rect");
    int x = G.ox + arg_int(a[0], pos, "round_rect"), y = G.oy + arg_int(a[1], pos, "round_rect"), w = arg_int(a[2], pos, "round_rect"), h = arg_int(a[3], pos, "round_rect"), r = arg_int(a[4], pos, "round_rect");
    uint32_t c = arg_color(a[5], pos, "round_rect");
    if (fill_arg(a, n, 6, pos, "round_rect")) fb_round(x, y, w, h, r, c); else { fb_round(x, y, w, h, r, c); /* outline: carve interior with the canvas background */ Widget *cv = wget(G.draw_id); fb_round(x + 1, y + 1, w - 2, h - 2, imax(r - 1, 0), cv && cv->has_bg ? cv->bg : G.th.field); }
    return kc_null();
}
static KcValue g_line(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    ENTER(); argc_check(n, 5, 6, pos, "line(x1, y1, x2, y2, color[, thickness=1])"); need_draw(pos, "line");
    fb_line(G.ox + arg_int(a[0], pos, "line"), G.oy + arg_int(a[1], pos, "line"), G.ox + arg_int(a[2], pos, "line"), G.oy + arg_int(a[3], pos, "line"), arg_color(a[4], pos, "line"), n == 6 ? arg_int(a[5], pos, "line") : 1);
    return kc_null();
}
static KcValue g_circle(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    ENTER(); argc_check(n, 4, 5, pos, "circle(cx, cy, radius, color[, fill=true])"); need_draw(pos, "circle");
    fb_circle(G.ox + arg_int(a[0], pos, "circle"), G.oy + arg_int(a[1], pos, "circle"), arg_int(a[2], pos, "circle"), arg_color(a[3], pos, "circle"), fill_arg(a, n, 4, pos, "circle")); return kc_null();
}
static KcValue g_triangle(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    ENTER(); argc_check(n, 7, 8, pos, "triangle(x1, y1, x2, y2, x3, y3, color[, fill=true])"); need_draw(pos, "triangle");
    fb_triangle(G.ox + arg_int(a[0], pos, "triangle"), G.oy + arg_int(a[1], pos, "triangle"), G.ox + arg_int(a[2], pos, "triangle"), G.oy + arg_int(a[3], pos, "triangle"),
                G.ox + arg_int(a[4], pos, "triangle"), G.oy + arg_int(a[5], pos, "triangle"), arg_color(a[6], pos, "triangle"), fill_arg(a, n, 7, pos, "triangle")); return kc_null();
}
static KcValue g_pixel(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    ENTER(); argc_check(n, 3, 3, pos, "pixel(x, y, color)"); need_draw(pos, "pixel");
    fb_px(G.ox + arg_int(a[0], pos, "pixel"), G.oy + arg_int(a[1], pos, "pixel"), arg_color(a[2], pos, "pixel")); return kc_null();
}
static KcValue g_text(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    ENTER(); argc_check(n, 4, 5, pos, "text(x, y, string, color[, scale])"); need_draw(pos, "text");
    char *s = kc_to_display_string(a[2]); int sc = n == 5 ? iclamp(arg_int(a[4], pos, "text"), 1, 8) : G.fs;
    fb_text(G.ox + arg_int(a[0], pos, "text"), G.oy + arg_int(a[1], pos, "text"), s, arg_color(a[3], pos, "text"), sc); free(s); return kc_null();
}
static KcValue g_text_size(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    ENTER(); argc_check(n, 1, 2, pos, "text_size(string[, scale]) -> [w, h]");
    char *s = kc_to_display_string(a[0]); int sc = n == 2 ? iclamp(arg_int(a[1], pos, "text_size"), 1, 8) : G.fs;
    KcValue r = kc_array_new(); kc_array_push(r, kc_int(text_w(s, sc))); kc_array_push(r, kc_int(text_lines(s) * line_h(sc) - 2 * sc)); free(s); return r;
}

/* ---- screenshots + simulated input (headless testing / automation) ---- */

static void put_le(FILE *f, uint32_t v, int bytes) { for (int i = 0; i < bytes; i++) fputc((int)((v >> (8 * i)) & 255), f); }
static KcValue g_screenshot(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    ENTER(); argc_check(n, 1, 1, pos, "screenshot(path.bmp)"); need_window(pos); kc_forbid_in_comptime(ip, pos, "gui.screenshot");
    const char *path = arg_str(a[0], pos, "screenshot"); frame(true);
    FILE *f = fopen(path, "wb"); if (!f) kc_error_at(pos, "gui.screenshot: cannot open '%s' for writing", path);
    int w = G.fb_w, h = G.fb_h, rowb = (w * 3 + 3) & ~3; uint32_t size = 54 + (uint32_t)rowb * (uint32_t)h;
    fputc('B', f); fputc('M', f); put_le(f, size, 4); put_le(f, 0, 4); put_le(f, 54, 4); put_le(f, 40, 4); put_le(f, (uint32_t)w, 4); put_le(f, (uint32_t)h, 4);
    put_le(f, 1, 2); put_le(f, 24, 2); put_le(f, 0, 4); put_le(f, (uint32_t)rowb * (uint32_t)h, 4); put_le(f, 2835, 4); put_le(f, 2835, 4); put_le(f, 0, 4); put_le(f, 0, 4);
    for (int y = h - 1; y >= 0; y--) {
        for (int x = 0; x < w; x++) { uint32_t p = G.fb[(size_t)y * (size_t)w + (size_t)x]; fputc((int)(p & 255), f); fputc((int)((p >> 8) & 255), f); fputc((int)((p >> 16) & 255), f); }
        for (int k = w * 3; k < rowb; k++) fputc(0, f);
    }
    fclose(f); return kc_int(size);
}
static KcValue g_click(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    ENTER(); argc_check(n, 1, 3, pos, "click(id[, dx, dy])  (simulates a left click; offsets are from the widget's top-left)");
    Widget *w = arg_widget(a[0], pos, "click"); relayout();
    int x = w->x + (n >= 2 ? arg_int(a[1], pos, "click") : w->w / 2), y = w->y + (n == 3 ? arg_int(a[2], pos, "click") : w->h / 2);
    on_mouse_move(x, y); on_mouse_down(x, y, 1, 0); on_mouse_up(x, y, 1); return kc_null();
}
static KcValue g_mouse_click(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    ENTER(); argc_check(n, 2, 2, pos, "mouse_click(x, y)"); need_window(pos); relayout();
    int x = arg_int(a[0], pos, "mouse_click"), y = arg_int(a[1], pos, "mouse_click");
    on_mouse_move(x, y); on_mouse_down(x, y, 1, 0); on_mouse_up(x, y, 1); return kc_null();
}
static KcValue g_send_text(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    ENTER(); argc_check(n, 1, 2, pos, "send_text([id,] text)  (types into the focused or given widget)"); need_window(pos);
    const char *t; if (n == 2) { Widget *w = arg_widget(a[0], pos, "send_text"); set_focus(w->id); t = arg_str(a[1], pos, "send_text"); } else t = arg_str(a[0], pos, "send_text");
    relayout();
    for (int i = 0; t[i];) { int adv = u8_next(t, i) - i; char buf[8]; memcpy(buf, t + i, (size_t)adv); buf[adv] = 0; on_char(buf); i += adv; }
    return kc_null();
}
static KcValue g_send_key(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    ENTER(); argc_check(n, 1, 2, pos, "send_key(name[, mods])  names: Enter Escape Tab Backspace Delete Left Right Up Down Home End PageUp PageDown F1.. or a letter; mods bits: 1 shift 2 ctrl 4 alt"); need_window(pos);
    const char *nm = arg_str(a[0], pos, "send_key"); int mods = n == 2 ? arg_int(a[1], pos, "send_key") : 0, k; relayout();
    if (is_key_name_valid(nm, &k)) on_key(k, mods);
    else if (strlen(nm) == 1 && (mods & (M_CTRL | M_ALT))) on_key(K_CTRL_BASE + tolower((unsigned char)nm[0]), mods);
    else on_char(strcmp(nm, "Space") == 0 ? " " : nm);
    return kc_null();
}
static KcValue g_widget_at(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    ENTER(); argc_check(n, 2, 2, pos, "widget_at(x, y) -> id or -1"); need_window(pos); relayout();
    return kc_int(hit(G.ws[0], arg_int(a[0], pos, "widget_at"), arg_int(a[1], pos, "widget_at")));
}
static KcValue g_dialog_open(KcInterp *ip, KcValue *a, int n, KcPos pos) { ENTER(); return kc_bool(G.dlg.active); }
static KcValue g_dialog_click(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    ENTER(); argc_check(n, 1, 1, pos, "dialog_click(button_index)  (simulates clicking a dialog button)");
    if (!G.dlg.active) kc_error_at(pos, "gui.dialog_click: no dialog is open");
    int i = arg_int(a[0], pos, "dialog_click"); if (i < 0 || i >= G.dlg.nbtn) kc_error_at(pos, "gui.dialog_click: no such button");
    dlg_close(i); return kc_null();
}

static KcModuleEntry ENTRIES[] = {
    {"window", g_window}, {"title", g_title}, {"size", g_size}, {"theme", g_theme}, {"scale", g_scale},
    {"headless", g_headless}, {"is_headless", g_is_headless},
    {"run", g_run}, {"poll", g_poll}, {"update", g_update}, {"quit", g_quit}, {"invalidate", g_invalidate}, {"now", g_now},
    {"timer", g_timer}, {"after", g_after}, {"cancel_timer", g_cancel_timer}, {"advance", g_advance},
    {"vbox", g_vbox}, {"panel", g_vbox}, {"hbox", g_hbox}, {"grid", g_grid}, {"absolute", g_absolute},
    {"label", g_label}, {"button", g_button}, {"checkbox", g_checkbox}, {"radio", g_radio}, {"textbox", g_textbox},
    {"slider", g_slider}, {"progress", g_progress}, {"list", g_list}, {"dropdown", g_dropdown},
    {"tabs", g_tabs}, {"page", g_page}, {"canvas", g_canvas}, {"separator", g_separator}, {"spacer", g_spacer},
    {"set", g_set}, {"get", g_get}, {"on", g_on}, {"remove", g_remove}, {"clear", g_clear},
    {"add_item", g_add_item}, {"remove_item", g_remove_item}, {"place", g_place}, {"focus", g_focus},
    {"bounds", g_bounds}, {"children", g_children}, {"clipboard", g_clipboard}, {"set_clipboard", g_set_clipboard},
    {"dialog", g_dialog}, {"alert", g_alert}, {"confirm", g_confirm},
    {"rect", g_rect}, {"round_rect", g_round_rect}, {"line", g_line}, {"circle", g_circle}, {"triangle", g_triangle},
    {"pixel", g_pixel}, {"text", g_text}, {"text_size", g_text_size},
    {"screenshot", g_screenshot}, {"click", g_click}, {"mouse_click", g_mouse_click}, {"send_text", g_send_text},
    {"send_key", g_send_key}, {"widget_at", g_widget_at}, {"dialog_open", g_dialog_open}, {"dialog_click", g_dialog_click},
};

KcModule *kc_mod_gui(void) {
    static KcModule m = { "gui", ENTRIES, sizeof(ENTRIES) / sizeof(ENTRIES[0]) };
    return &m;
}
