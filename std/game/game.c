/* std.game — window/input backend, main loop, and the KingCobra bindings.
 *
 * Backends: X11 (Linux, when built with KC_GUI_X11), Win32 (Windows), and a headless backend that
 * needs no display at all (CI, servers, tests): time is virtual, input can be injected, frames can
 * be saved with game.screenshot().
 */
#include "game_internal.h"
#include <ctype.h>

#if defined(_WIN32)
#  define KC_GAME_WIN32 1
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#elif defined(KC_GUI_X11)
#  define KC_GAME_X11 1
#  include <X11/Xlib.h>
#  include <X11/Xutil.h>
#  include <X11/keysym.h>
#  include <X11/XKBlib.h>
#endif

/* ================================================================== keys */
static const struct { const char *name; int code; } KEYS[] = {
    {"space", 32}, {"enter", 128}, {"return", 128}, {"escape", 129}, {"esc", 129}, {"tab", 130}, {"backspace", 131}, {"delete", 132}, {"insert", 133},
    {"left", 134}, {"right", 135}, {"up", 136}, {"down", 137}, {"home", 138}, {"end", 139}, {"pageup", 140}, {"pagedown", 141},
    {"shift", 142}, {"ctrl", 143}, {"alt", 144}, {"f1", 145}, {"f2", 146}, {"f3", 147}, {"f4", 148}, {"f5", 149}, {"f6", 150}, {"f7", 151}, {"f8", 152}, {"f9", 153}, {"f10", 154}, {"f11", 155}, {"f12", 156},
    {"comma", ','}, {"period", '.'}, {"slash", '/'}, {"minus", '-'}, {"equals", '='}, {"semicolon", ';'}, {"quote", '\''}, {"lbracket", '['}, {"rbracket", ']'}, {"backslash", '\\'}, {"grave", '`'},
};
int key_code(const char *name) {
    if (!name || !*name) return -1;
    if (name[1] == 0 && isalnum((unsigned char)name[0])) return tolower((unsigned char)name[0]);
    char low[16]; size_t i = 0; for (; name[i] && i < 15; i++) low[i] = (char)tolower((unsigned char)name[i]); low[i] = 0;
    for (size_t k = 0; k < sizeof KEYS / sizeof KEYS[0]; k++) if (!strcmp(KEYS[k].name, low)) return KEYS[k].code;
    return -1;
}
const char *key_name(int code) { for (size_t k = 0; k < sizeof KEYS / sizeof KEYS[0]; k++) if (KEYS[k].code == code) return KEYS[k].name; return NULL; }

static void gm_key_event(int code, bool down) {
    if (code < 0 || code > 255) return;
    if (down) { if (!G.key[code]) G.kp[code] = 1; G.key[code] = 1; } else { if (G.key[code]) G.kr[code] = 1; G.key[code] = 0; }
}
static void gm_mouse_event(int b, bool down) {
    if (b < 1 || b > 7) return;
    if (down) { if (!G.mb[b]) G.mbp[b] = 1; G.mb[b] = 1; } else { if (G.mb[b]) G.mbr[b] = 1; G.mb[b] = 0; }
}

/* ================================================================ backends */
static uint32_t *present_buf; static size_t present_cap;
static void build_present(void) {                                    /* upscale fb -> window-sized buffer */
    size_t need = (size_t)G.win_w * G.win_h; if (need > present_cap) { present_buf = kc_realloc(present_buf, need * 4); present_cap = need; }
    int S = G.scale;
    if (S == 1 && G.w == G.win_w && G.h == G.win_h) { memcpy(present_buf, G.fb, need * 4); return; }
    for (int y = 0; y < G.win_h; y++) { int sy = imini(y / S, G.h - 1); const uint32_t *src = G.fb + (size_t)sy * G.w; uint32_t *dst = present_buf + (size_t)y * G.win_w; for (int x = 0; x < G.win_w; x++) dst[x] = src[imini(x / S, G.w - 1)]; }
}

#if defined(KC_GAME_X11)
static Display *dpy; static Window win; static GC gc; static XImage *ximg; static Atom wm_del; static Cursor blank_cur; static bool cursor_hidden; static int warp_cx, warp_cy;
static int x_key(KeySym ks) {
    if (ks >= XK_a && ks <= XK_z) return (int)ks; if (ks >= XK_A && ks <= XK_Z) return (int)(ks - XK_A + XK_a); if (ks >= XK_0 && ks <= XK_9) return (int)ks;
    switch (ks) {
        case XK_space: return 32; case XK_Return: case XK_KP_Enter: return 128; case XK_Escape: return 129; case XK_Tab: return 130; case XK_BackSpace: return 131; case XK_Delete: return 132; case XK_Insert: return 133;
        case XK_Left: return 134; case XK_Right: return 135; case XK_Up: return 136; case XK_Down: return 137; case XK_Home: return 138; case XK_End: return 139; case XK_Prior: return 140; case XK_Next: return 141;
        case XK_Shift_L: case XK_Shift_R: return 142; case XK_Control_L: case XK_Control_R: return 143; case XK_Alt_L: case XK_Alt_R: return 144;
        case XK_comma: return ','; case XK_period: return '.'; case XK_slash: return '/'; case XK_minus: return '-'; case XK_equal: return '='; case XK_semicolon: return ';'; case XK_apostrophe: return '\'';
        case XK_bracketleft: return '['; case XK_bracketright: return ']'; case XK_backslash: return '\\'; case XK_grave: return '`';
    }
    if (ks >= XK_F1 && ks <= XK_F12) return 145 + (int)(ks - XK_F1);
    return -1;
}
static void x_make_image(void) {
    if (ximg) { ximg->data = NULL; XDestroyImage(ximg); ximg = NULL; }
    size_t need = (size_t)G.win_w * G.win_h; if (need > present_cap) { present_buf = kc_realloc(present_buf, need * 4); present_cap = need; }
    ximg = XCreateImage(dpy, DefaultVisual(dpy, DefaultScreen(dpy)), (unsigned)DefaultDepth(dpy, DefaultScreen(dpy)), ZPixmap, 0, (char *)present_buf, (unsigned)G.win_w, (unsigned)G.win_h, 32, 0);
}
static bool backend_open(void) {
    dpy = XOpenDisplay(NULL); if (!dpy) return false;
    int scr = DefaultScreen(dpy);
    win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), 0, 0, (unsigned)G.win_w, (unsigned)G.win_h, 0, BlackPixel(dpy, scr), BlackPixel(dpy, scr));
    XStoreName(dpy, win, G.title); wm_del = XInternAtom(dpy, "WM_DELETE_WINDOW", False); XSetWMProtocols(dpy, win, &wm_del, 1);
    XSelectInput(dpy, win, KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | StructureNotifyMask | FocusChangeMask);
    gc = XCreateGC(dpy, win, 0, NULL); XkbSetDetectableAutoRepeat(dpy, True, NULL);
    present_buf = NULL; present_cap = 0; x_make_image(); XMapWindow(dpy, win); XFlush(dpy);
    Pixmap bm = XCreatePixmap(dpy, win, 1, 1, 1); XColor c; memset(&c, 0, sizeof c); blank_cur = XCreatePixmapCursor(dpy, bm, bm, &c, &c, 0, 0); XFreePixmap(dpy, bm);
    return true;
}
static void backend_close(void) {
    if (!dpy) return; if (ximg) { ximg->data = NULL; XDestroyImage(ximg); ximg = NULL; } XFreeCursor(dpy, blank_cur); XFreeGC(dpy, gc); XDestroyWindow(dpy, win); XCloseDisplay(dpy); dpy = NULL;
}
static void backend_capture(bool on) {
    if (!dpy) return;
    if (on) { warp_cx = G.win_w / 2; warp_cy = G.win_h / 2; XDefineCursor(dpy, win, blank_cur); XWarpPointer(dpy, None, win, 0, 0, 0, 0, warp_cx, warp_cy); cursor_hidden = true; }
    else { XUndefineCursor(dpy, win); cursor_hidden = false; }
    XFlush(dpy);
}
static void backend_poll(void) {
    if (!dpy) return;
    while (XPending(dpy)) {
        XEvent ev; XNextEvent(dpy, &ev);
        switch (ev.type) {
            case KeyPress: { KeySym ks = XLookupKeysym(&ev.xkey, 0); gm_key_event(x_key(ks), true);
                char buf[8]; int l = XLookupString(&ev.xkey, buf, sizeof buf, NULL, NULL); size_t cur = strlen(G.typed);
                for (int i = 0; i < l && cur + 1 < sizeof G.typed; i++) if ((unsigned char)buf[i] >= 32 && buf[i] != 127) G.typed[cur++] = buf[i]; G.typed[cur] = 0; break; }
            case KeyRelease: gm_key_event(x_key(XLookupKeysym(&ev.xkey, 0)), false); break;
            case ButtonPress: if (ev.xbutton.button == 4) G.wheel++; else if (ev.xbutton.button == 5) G.wheel--; else gm_mouse_event((int)ev.xbutton.button, true); break;
            case ButtonRelease: if (ev.xbutton.button <= 3) gm_mouse_event((int)ev.xbutton.button, false); break;
            case MotionNotify:
                if (G.captured) { if (ev.xmotion.x == warp_cx && ev.xmotion.y == warp_cy) break; G.dmx += ev.xmotion.x - warp_cx; G.dmy += ev.xmotion.y - warp_cy; }
                else { G.dmx += ev.xmotion.x / G.scale - G.mx; G.dmy += ev.xmotion.y / G.scale - G.my; }
                G.mx = ev.xmotion.x / G.scale; G.my = ev.xmotion.y / G.scale; break;
            case ConfigureNotify: if (ev.xconfigure.width != G.win_w || ev.xconfigure.height != G.win_h) {
                G.win_w = imaxi(64, ev.xconfigure.width); G.win_h = imaxi(64, ev.xconfigure.height); gfx_resize(G.win_w / G.scale, G.win_h / G.scale); x_make_image(); warp_cx = G.win_w / 2; warp_cy = G.win_h / 2; } break;
            case FocusOut: memset(G.key, 0, sizeof G.key); memset(G.mb, 0, sizeof G.mb); break;
            case ClientMessage: if ((Atom)ev.xclient.data.l[0] == wm_del) G.quit = true; break;
        }
    }
    if (G.captured) XWarpPointer(dpy, None, win, 0, 0, 0, 0, warp_cx, warp_cy);
}
static void backend_present(void) { if (!dpy) return; build_present(); XPutImage(dpy, win, gc, ximg, 0, 0, 0, 0, (unsigned)G.win_w, (unsigned)G.win_h); XFlush(dpy); }
static void backend_title(void) { if (dpy) { XStoreName(dpy, win, G.title); XFlush(dpy); } }

#elif defined(KC_GAME_WIN32)
static HWND hwnd; static bool cursor_hidden_w; static int cap_cx, cap_cy;
static int w_key(WPARAM vk) {
    if (vk >= 'A' && vk <= 'Z') return (int)(vk - 'A' + 'a'); if (vk >= '0' && vk <= '9') return (int)vk;
    if (vk >= VK_F1 && vk <= VK_F12) return 145 + (int)(vk - VK_F1);
    switch (vk) { case VK_SPACE: return 32; case VK_RETURN: return 128; case VK_ESCAPE: return 129; case VK_TAB: return 130; case VK_BACK: return 131; case VK_DELETE: return 132; case VK_INSERT: return 133;
        case VK_LEFT: return 134; case VK_RIGHT: return 135; case VK_UP: return 136; case VK_DOWN: return 137; case VK_HOME: return 138; case VK_END: return 139; case VK_PRIOR: return 140; case VK_NEXT: return 141;
        case VK_SHIFT: return 142; case VK_CONTROL: return 143; case VK_MENU: return 144; case VK_OEM_COMMA: return ','; case VK_OEM_PERIOD: return '.'; case VK_OEM_2: return '/'; case VK_OEM_MINUS: return '-'; case VK_OEM_PLUS: return '=';
        case VK_OEM_1: return ';'; case VK_OEM_7: return '\''; case VK_OEM_4: return '['; case VK_OEM_6: return ']'; case VK_OEM_5: return '\\'; case VK_OEM_3: return '`'; }
    return -1;
}
static LRESULT CALLBACK wproc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
        case WM_CLOSE: G.quit = true; return 0;
        case WM_KEYDOWN: case WM_SYSKEYDOWN: gm_key_event(w_key(wp), true); return 0;
        case WM_KEYUP: case WM_SYSKEYUP: gm_key_event(w_key(wp), false); return 0;
        case WM_CHAR: if (wp >= 32 && wp < 127) { size_t c = strlen(G.typed); if (c + 1 < sizeof G.typed) { G.typed[c] = (char)wp; G.typed[c + 1] = 0; } } return 0;
        case WM_LBUTTONDOWN: gm_mouse_event(1, true); return 0; case WM_LBUTTONUP: gm_mouse_event(1, false); return 0;
        case WM_MBUTTONDOWN: gm_mouse_event(2, true); return 0; case WM_MBUTTONUP: gm_mouse_event(2, false); return 0;
        case WM_RBUTTONDOWN: gm_mouse_event(3, true); return 0; case WM_RBUTTONUP: gm_mouse_event(3, false); return 0;
        case WM_MOUSEWHEEL: G.wheel += GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 1 : -1; return 0;
        case WM_MOUSEMOVE: { int x = (short)LOWORD(lp), y = (short)HIWORD(lp); if (!G.captured) { G.dmx += x / G.scale - G.mx; G.dmy += y / G.scale - G.my; G.mx = x / G.scale; G.my = y / G.scale; } return 0; }
        case WM_SIZE: if (wp != SIZE_MINIMIZED) { int w = LOWORD(lp), hh = HIWORD(lp); if (w > 0 && hh > 0 && (w != G.win_w || hh != G.win_h)) { G.win_w = w; G.win_h = hh; gfx_resize(w / G.scale, hh / G.scale); } } return 0;
        case WM_KILLFOCUS: memset(G.key, 0, sizeof G.key); memset(G.mb, 0, sizeof G.mb); return 0;
    }
    return DefWindowProcA(h, m, wp, lp);
}
static bool backend_open(void) {
    WNDCLASSA wc; memset(&wc, 0, sizeof wc); wc.lpfnWndProc = wproc; wc.hInstance = GetModuleHandleA(NULL); wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.lpszClassName = "KCGameWindow"; RegisterClassA(&wc);
    RECT r = { 0, 0, G.win_w, G.win_h }; AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    hwnd = CreateWindowA("KCGameWindow", G.title, WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    return hwnd != NULL;
}
static void backend_close(void) { if (hwnd) { DestroyWindow(hwnd); hwnd = NULL; } }
static void backend_capture(bool on) {
    if (!hwnd) return; if (on) { RECT r; GetClientRect(hwnd, &r); POINT p = { r.right / 2, r.bottom / 2 }; cap_cx = p.x; cap_cy = p.y; ClientToScreen(hwnd, &p); SetCursorPos(p.x, p.y); if (!cursor_hidden_w) { ShowCursor(FALSE); cursor_hidden_w = true; } }
    else if (cursor_hidden_w) { ShowCursor(TRUE); cursor_hidden_w = false; }
}
static void backend_poll(void) {
    if (!hwnd) return; MSG m; while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageA(&m); }
    if (G.captured && GetForegroundWindow() == hwnd) { POINT p; GetCursorPos(&p); ScreenToClient(hwnd, &p); G.dmx += p.x - cap_cx; G.dmy += p.y - cap_cy; POINT c = { cap_cx, cap_cy }; ClientToScreen(hwnd, &c); SetCursorPos(c.x, c.y); }
}
static void backend_present(void) {
    if (!hwnd) return; build_present(); BITMAPINFO bi; memset(&bi, 0, sizeof bi); bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = G.win_w; bi.bmiHeader.biHeight = -G.win_h; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    HDC dc = GetDC(hwnd); StretchDIBits(dc, 0, 0, G.win_w, G.win_h, 0, 0, G.win_w, G.win_h, present_buf, &bi, DIB_RGB_COLORS, SRCCOPY); ReleaseDC(hwnd, dc);
}
static void backend_title(void) { if (hwnd) SetWindowTextA(hwnd, G.title); }
#else
static bool backend_open(void) { return false; }
static void backend_close(void) {}
static void backend_capture(bool on) { (void)on; }
static void backend_poll(void) {}
static void backend_present(void) {}
static void backend_title(void) {}
#endif

/* ================================================================= frame loop */
static float dt_override = -1.0f;
static uint8_t inj_kp[256], inj_kr[256], inj_mbp[8], inj_mbr[8]; static int inj_dx, inj_dy;      /* injected input waits for the next poll */

static void game_defaults(void) {
    G.target_fps = 60; G.fov = 70; G.znear = 0.1f; G.zfar = 600; G.cam = v3(0, 20, 0); G.c2zoom = 1; G.mode3d = true; G.hud_auto = true;
    G.mem_limit_models = 256u << 20; G.mem_limit_tex = 256u << 20; rnd_seed(12345);
}
static void ensure_window(void) {
    if (G.created) return;
    G.created = true; if (!G.win_w) { G.win_w = 640; G.win_h = 360; } if (G.scale < 1) G.scale = 1;
    if (!G.title[0]) snprintf(G.title, sizeof G.title, "KingCobra Game");
    gfx_resize(G.win_w / G.scale, G.win_h / G.scale);
    if (!G.headless) { if (backend_open()) G.backend_open = true; else { G.headless = true; gm_warn("no display available — running headless (use game.screenshot to see frames)"); } }
    G.last_real = clock_s(); G.vclock = 0;
}
static void ensure_begun(void) {
    ensure_window();
    if (G.begun) return;
    if (G.mode3d) r3d_begin(); else gfx_clear(0xFF14161Cu);
    G.begun = true;
}
void game_begin_frame(void) { ensure_begun(); }

static bool game_poll(void) {
    ensure_window();
    memset(G.kp, 0, sizeof G.kp); memset(G.kr, 0, sizeof G.kr); memset(G.mbp, 0, sizeof G.mbp); memset(G.mbr, 0, sizeof G.mbr); G.dmx = G.dmy = 0; G.wheel = 0; G.typed[0] = 0;
    if (!G.headless) backend_poll();
    for (int i = 0; i < 256; i++) { if (inj_kp[i]) G.kp[i] = 1; if (inj_kr[i]) G.kr[i] = 1; } for (int i = 0; i < 8; i++) { if (inj_mbp[i]) G.mbp[i] = 1; if (inj_mbr[i]) G.mbr[i] = 1; }
    G.dmx += inj_dx; G.dmy += inj_dy; memset(inj_kp, 0, sizeof inj_kp); memset(inj_kr, 0, sizeof inj_kr); memset(inj_mbp, 0, sizeof inj_mbp); memset(inj_mbr, 0, sizeof inj_mbr); inj_dx = inj_dy = 0;
    double now = clock_s(), dt;
    if (G.headless || dt_override > 0) { dt = dt_override > 0 ? dt_override : 1.0 / (G.target_fps > 0 ? G.target_fps : 60); }
    else { dt = now - G.last_real; if (dt > 0.1) dt = 0.1; if (dt < 1e-4) dt = 1e-4; }
    G.last_real = now; G.dt = dt; G.time += dt; G.frame++;
    if (G.dt > 0) G.fps = G.fps <= 0 ? 1.0 / dt : G.fps * 0.9 + (1.0 / dt) * 0.1;
    G.st_tris = G.st_tris_drawn = G.st_quads = G.st_chunks_drawn = G.st_chunks_culled = G.st_draws = G.st_ents_drawn = G.st_particles = 0;
    /* HUD input routing: dialog keys / clicks are consumed so scripts do not also react */
    if (hud_dialog_active()) {
        if (G.kp[32] || G.kp[128]) { hud_dialog_advance(); G.kp[32] = G.kp[128] = 0; }
        for (int d = 1; d <= 9; d++) if (G.kp['0' + d]) { hud_key(d - 1); G.kp['0' + d] = 0; }
    }
    if (G.mbp[1] && hud_click(G.mx, G.my)) G.mbp[1] = 0;
    scene_update((float)dt); hud_update((float)dt);
    return !G.quit;
}
void game_end_frame(void) {
    ensure_begun(); part_draw2d();
    if (G.hud_auto) hud_draw();
    if (G.backend_open && !G.headless) backend_present();
    G.begun = false;
    if ((G.frame & 15) == 0) { tex_evict(G.mem_limit_tex); model_evict(G.mem_limit_models); }
    if (!G.headless && G.target_fps > 0 && dt_override <= 0) { double spent = clock_s() - G.last_real, want = 1.0 / G.target_fps; if (spent < want) sleep_ms_((int)((want - spent) * 1000.0)); }
    G.st_frame_ms = (clock_s() - G.last_real) * 1000.0;
}
static void render3d(void) {
    ensure_begun(); if (!G.mode3d) return;
    double t0 = clock_s(); world_update((float)G.dt); world_draw(); scene_draw3d(); G.st_render_ms = (clock_s() - t0) * 1000.0;
}

/* ============================================================== bindings */
#define NAT(nm) static KcValue nm(KcInterp *ip, KcValue *a, int n, KcPos pos)
#define BEGIN(lo, hi, sig) do { ENTER(); A_argc(n, lo, hi, sig); } while (0)
#define NIL kc_null()

static bool is_fn(KcValue v) { return v.type == VAL_CLOSURE || v.type == VAL_NATIVE; }
static int res_model(KcValue v, const char *who) {
    if (V_isnum(v)) { int id = (int)V_num(v, who); if (!model_info(id)) gm_err("%s: invalid model %d", who, id); return id; }
    if (v.type != VAL_STRING) gm_err("%s: expected a model id or name", who);
    const char *s = v.as.s->data; int id = model_builtin(s); if (id >= 0) return id;
    if (file_exists(s)) { Opts o = opts_make(NULL, 0, 0); return model_load(s, &o); }
    gm_err("%s: unknown model \"%s\" (not a built-in or custom name, and no such file). Built-ins: use game.models()", who, s);
}
static int res_tex(KcValue v, const char *who) {
    if (V_isnum(v)) { int id = (int)V_num(v, who); if (!tex_peek(id)) gm_err("%s: invalid texture %d", who, id); return id; }
    if (v.type == VAL_STRING) { if (!file_exists(v.as.s->data)) gm_err("%s: texture file not found: %s", who, v.as.s->data); return tex_file(v.as.s->data); }
    gm_err("%s: expected a texture id or file path", who);
}
static int res_btn(KcValue *a, int n, int i) {
    if (i >= n) return 1; if (a[i].type == VAL_STRING) { const char *s = a[i].as.s->data; return !strcmp(s, "left") ? 1 : !strcmp(s, "middle") ? 2 : !strcmp(s, "right") ? 3 : 1; } return iclampi((int)V_num(a[i], "button"), 1, 7);
}
static int need_key(KcValue *a, int n, int i, const char *who) {
    const char *nm = A_str(a, n, i, who); int c = key_code(nm); if (c < 0) gm_err("%s: unknown key \"%s\" (letters, digits, space enter escape tab backspace left right up down shift ctrl alt f1..f12 comma period ...)", who, nm); return c;
}

/* ---- window + loop ---- */
NAT(g_window) {
    BEGIN(1, 4, "window(title, [width, height], [scale])");
    if (G.created) gm_err("window: already created");
    snprintf(G.title, sizeof G.title, "%s", A_str(a, n, 0, "window")); if (n > 1) { G.win_w = iclampi(A_int(a, n, 1, "window"), 64, 8192); G.win_h = iclampi(n > 2 ? A_int(a, n, 2, "window") : G.win_w * 9 / 16, 64, 8192); }
    G.scale = n > 3 ? iclampi(A_int(a, n, 3, "window"), 1, 8) : 1; ensure_window(); return kc_bool(!G.headless);
}
NAT(g_title) { BEGIN(1, 1, "title(text)"); snprintf(G.title, sizeof G.title, "%s", A_str(a, n, 0, "title")); backend_title(); return NIL; }
NAT(g_size) { BEGIN(0, 0, "size() -> [width, height]  (render resolution)"); ensure_window(); return arr2(G.w, G.h); }
NAT(g_headless) { BEGIN(0, 1, "headless([on])"); bool on = n ? A_bool(a, n, 0, "headless") : true; if (G.created && on != G.headless) { if (on) { backend_close(); G.backend_open = false; G.headless = true; } else gm_err("headless: call game.headless(false) before the window is created"); } else G.headless = on; return NIL; }
NAT(g_is_headless) { BEGIN(0, 0, "is_headless()"); ensure_window(); return kc_bool(G.headless); }
NAT(g_run) {
    BEGIN(1, 3, "run(update(dt), [draw()], [max_frames])"); ensure_window();
    bool hu = a[0].type != VAL_NULL; if (hu && !is_fn(a[0])) gm_err("run: update must be a function"); bool hd = n > 1 && a[1].type != VAL_NULL; if (hd && !is_fn(a[1])) gm_err("run: draw must be a function");
    int maxf = n > 2 ? A_int(a, n, 2, "run") : 0, cnt = 0; G.quit = false;
    while (game_poll()) {
        if (hu) { KcValue d = kc_float(G.dt); kc_call_value(ip, a[0], &d, 1, pos); }
        ensure_begun(); render3d();
        if (hd) kc_call_value(ip, a[1], NULL, 0, pos);
        game_end_frame(); if (maxf > 0 && ++cnt >= maxf) break;
    }
    return NIL;
}
NAT(g_poll) { BEGIN(0, 0, "poll() -> false once the window is closed"); return kc_bool(game_poll()); }
NAT(g_begin) { BEGIN(0, 0, "begin()"); ensure_begun(); return NIL; }
NAT(g_render3d) { BEGIN(0, 0, "render3d()  (streams + draws the world, entities, particles)"); ensure_begun(); render3d(); return NIL; }
NAT(g_present) { BEGIN(0, 0, "present()  (HUD + show the frame)"); game_end_frame(); return NIL; }
NAT(g_quit) { BEGIN(0, 0, "quit()"); G.quit = true; return NIL; }
NAT(g_dt) { BEGIN(0, 0, "dt()"); return kc_float(G.dt); }
NAT(g_time) { BEGIN(0, 0, "time()"); return kc_float(G.time); }
NAT(g_fps) { BEGIN(0, 0, "fps()"); return kc_float(G.fps); }
NAT(g_frame) { BEGIN(0, 0, "frame()"); return kc_int((int64_t)G.frame); }
NAT(g_target_fps) { BEGIN(1, 1, "target_fps(n)  (0 = unlimited)"); G.target_fps = imaxi(0, A_int(a, n, 0, "target_fps")); return NIL; }
NAT(g_advance) {
    BEGIN(1, 2, "advance(frames, [dt])  (run frames without script callbacks: tests, baking)"); ensure_window(); int f = A_int(a, n, 0, "advance"); if (n > 1) dt_override = (float)A_num(a, n, 1, "advance");
    for (int i = 0; i < f; i++) { if (!game_poll()) break; ensure_begun(); render3d(); game_end_frame(); } dt_override = -1; return kc_int(f);
}
NAT(g_screenshot) {
    BEGIN(1, 1, "screenshot(path)  (.png, .bmp or .ppm)"); ensure_window(); const char *p = A_str(a, n, 0, "screenshot"); size_t cnt = (size_t)G.w * G.h; uint32_t *t = kc_malloc(cnt * 4);
    for (size_t i = 0; i < cnt; i++) t[i] = 0xFF000000u | G.fb[i]; char err[256]; bool ok = img_save(p, t, G.w, G.h, err); free(t); if (!ok) gm_err("screenshot: %s", err); return kc_bool(true);
}
NAT(g_pixel) { BEGIN(2, 2, "pixel(x, y) -> colour int"); ensure_window(); return kc_int(gfx_get(A_int(a, n, 0, "pixel"), A_int(a, n, 1, "pixel"))); }
NAT(g_mode) { BEGIN(1, 1, "mode(\"2d\" | \"3d\")"); const char *m = A_str(a, n, 0, "mode"); if (!strcmp(m, "2d")) G.mode3d = false; else if (!strcmp(m, "3d")) G.mode3d = true; else gm_err("mode: use \"2d\" or \"3d\""); return NIL; }
NAT(g_hud_auto) { BEGIN(1, 1, "hud_auto(on)"); G.hud_auto = A_bool(a, n, 0, "hud_auto"); return NIL; }
NAT(g_budget) { BEGIN(2, 2, "budget(model_mb, texture_mb)  (LRU eviction of file-backed assets above these)"); G.mem_limit_models = (size_t)(A_num(a, n, 0, "budget") * 1048576.0); G.mem_limit_tex = (size_t)(A_num(a, n, 1, "budget") * 1048576.0); return NIL; }
NAT(g_stats) {
    BEGIN(0, 0, "stats() -> object"); KcValue o = obj_new();
    obj_num(o, "fps", G.fps); obj_num(o, "frame_ms", G.st_frame_ms); obj_num(o, "render_ms", G.st_render_ms); obj_num(o, "stream_ms", G.st_stream_ms);
    obj_int(o, "triangles", G.st_tris); obj_int(o, "triangles_drawn", G.st_tris_drawn); obj_int(o, "quads", G.st_quads); obj_int(o, "draw_calls", G.st_draws); obj_int(o, "entities_drawn", G.st_ents_drawn); obj_int(o, "particles", G.st_particles);
    obj_int(o, "chunks_drawn", G.st_chunks_drawn); obj_int(o, "chunks_culled", G.st_chunks_culled); obj_int(o, "textures", tex_count()); obj_num(o, "texture_mb", (double)tex_resident_bytes() / 1048576.0);
    obj_int(o, "models", model_count()); obj_num(o, "model_mb", (double)model_resident_bytes() / 1048576.0); obj_int(o, "entities", ent_count()); obj_num(o, "fog_end", R_fog.autofog ? R_fog.cur_end : R_fog.end);
    world_stats(o); return o;
}
NAT(g_reset) { BEGIN(0, 0, "reset()"); world_reset(); scene_reset(); hud_reset(); G.cam = v3(0, 20, 0); G.yaw = G.pitch = G.roll = 0; return NIL; }

/* ---- input ---- */
NAT(g_key) { BEGIN(1, 1, "key(name) -> held"); return kc_bool(G.key[need_key(a, n, 0, "key")] != 0); }
NAT(g_key_pressed) { BEGIN(1, 1, "key_pressed(name) -> went down this frame"); return kc_bool(G.kp[need_key(a, n, 0, "key_pressed")] != 0); }
NAT(g_key_released) { BEGIN(1, 1, "key_released(name)"); return kc_bool(G.kr[need_key(a, n, 0, "key_released")] != 0); }
NAT(g_mouse) { BEGIN(0, 0, "mouse() -> [x, y]"); return arr2(G.mx, G.my); }
NAT(g_mouse_delta) { BEGIN(0, 0, "mouse_delta() -> [dx, dy]  (render pixels; relative when captured)"); float k = G.captured ? 1.0f / (float)G.scale : 1.0f; return arr2(G.dmx * k, G.dmy * k); }
NAT(g_mouse_down) { BEGIN(0, 1, "mouse_down([button])"); return kc_bool(G.mb[res_btn(a, n, 0)] != 0); }
NAT(g_mouse_pressed) { BEGIN(0, 1, "mouse_pressed([button])"); return kc_bool(G.mbp[res_btn(a, n, 0)] != 0); }
NAT(g_mouse_released) { BEGIN(0, 1, "mouse_released([button])"); return kc_bool(G.mbr[res_btn(a, n, 0)] != 0); }
NAT(g_wheel) { BEGIN(0, 0, "wheel() -> notches scrolled this frame"); return kc_int(G.wheel); }
NAT(g_capture_mouse) { BEGIN(1, 1, "capture_mouse(on)  (hide cursor, relative mouse look)"); ensure_window(); G.captured = A_bool(a, n, 0, "capture_mouse") && !G.headless; backend_capture(G.captured); return NIL; }
NAT(g_typed) { BEGIN(0, 0, "typed() -> text typed this frame"); return kc_string(G.typed); }
NAT(g_send_key) { BEGIN(2, 2, "send_key(name, down)  (inject input — tests / headless)"); { int c = need_key(a, n, 0, "send_key"); bool d = A_bool(a, n, 1, "send_key"); if (d && !G.key[c]) inj_kp[c] = 1; if (!d && G.key[c]) inj_kr[c] = 1; G.key[c] = d; } return NIL; }
NAT(g_send_mouse) {
    BEGIN(2, 4, "send_mouse(x, y, [button, down])"); int x = A_int(a, n, 0, "send_mouse"), y = A_int(a, n, 1, "send_mouse"); inj_dx += x - G.mx; inj_dy += y - G.my; G.mx = x; G.my = y;
    if (n > 3) { int b = res_btn(a, n, 2); bool d = A_bool(a, n, 3, "send_mouse"); if (d && !G.mb[b]) inj_mbp[b] = 1; if (!d && G.mb[b]) inj_mbr[b] = 1; G.mb[b] = d; } return NIL;
}

/* ---- colours / maths ---- */
NAT(g_rgb) { BEGIN(3, 4, "rgb(r, g, b, [a])"); uint32_t c = col_pack(A_int(a, n, 0, "rgb"), A_int(a, n, 1, "rgb"), A_int(a, n, 2, "rgb"), n > 3 ? A_int(a, n, 3, "rgb") : 255); return kc_int(CA(c) == 0 ? -1 : (int64_t)c); }
NAT(g_hsv) { BEGIN(3, 4, "hsv(h 0..1, s, v, [a])"); return kc_int(col_hsv((float)A_num(a, n, 0, "hsv"), (float)A_num(a, n, 1, "hsv"), (float)A_num(a, n, 2, "hsv"), n > 3 ? A_int(a, n, 3, "hsv") : 255)); }
NAT(g_mix) { BEGIN(3, 3, "mix(colour_a, colour_b, t)"); return kc_int(col_mix(A_col(a, n, 0, "mix"), A_col(a, n, 1, "mix"), (float)A_num(a, n, 2, "mix"))); }
NAT(g_noise2) { BEGIN(2, 3, "noise2(x, y, [seed]) -> 0..1"); return kc_float(vnoise2((float)A_num(a, n, 0, "noise2"), (float)A_num(a, n, 1, "noise2"), n > 2 ? (uint32_t)A_int(a, n, 2, "noise2") : 0)); }
NAT(g_noise3) { BEGIN(3, 4, "noise3(x, y, z, [seed]) -> 0..1"); return kc_float(vnoise3((float)A_num(a, n, 0, "noise3"), (float)A_num(a, n, 1, "noise3"), (float)A_num(a, n, 2, "noise3"), n > 3 ? (uint32_t)A_int(a, n, 3, "noise3") : 0)); }
NAT(g_fbm) { BEGIN(2, 5, "fbm(x, y, [octaves=4], [seed], [gain=0.5]) -> 0..1"); return kc_float(fbm2((float)A_num(a, n, 0, "fbm"), (float)A_num(a, n, 1, "fbm"), n > 2 ? A_int(a, n, 2, "fbm") : 4, 2.0f, n > 4 ? (float)A_num(a, n, 4, "fbm") : 0.5f, n > 3 ? (uint32_t)A_int(a, n, 3, "fbm") : 0)); }
NAT(g_seed) { BEGIN(1, 1, "seed(n)"); rnd_seed((uint32_t)A_int(a, n, 0, "seed")); return NIL; }
NAT(g_random) { BEGIN(0, 2, "random([lo, hi]) -> float"); return kc_float(n >= 2 ? rnd_range((float)A_num(a, n, 0, "random"), (float)A_num(a, n, 1, "random")) : rnd_f()); }
NAT(g_overlap) {
    BEGIN(8, 8, "overlap(ax, ay, aw, ah, bx, by, bw, bh)"); double v[8]; for (int i = 0; i < 8; i++) v[i] = A_num(a, n, i, "overlap");
    return kc_bool(v[0] < v[4] + v[6] && v[0] + v[2] > v[4] && v[1] < v[5] + v[7] && v[1] + v[3] > v[5]);
}

/* ---- 2D drawing ---- */
NAT(g_clear) { BEGIN(0, 1, "clear([colour])"); ensure_window(); gfx_clear(n ? A_col(a, n, 0, "clear") : 0xFF14161Cu); return NIL; }
NAT(g_rect) { BEGIN(5, 6, "rect(x, y, w, h, colour, [fill=true])"); ensure_begun(); gfx_rect(A_int(a, n, 0, "rect"), A_int(a, n, 1, "rect"), A_int(a, n, 2, "rect"), A_int(a, n, 3, "rect"), A_col(a, n, 4, "rect"), n > 5 ? A_bool(a, n, 5, "rect") : true); return NIL; }
NAT(g_rect_grad) { BEGIN(6, 6, "rect_grad(x, y, w, h, top, bottom)"); ensure_begun(); gfx_rect_grad(A_int(a, n, 0, "rect_grad"), A_int(a, n, 1, "rect_grad"), A_int(a, n, 2, "rect_grad"), A_int(a, n, 3, "rect_grad"), A_col(a, n, 4, "rect_grad"), A_col(a, n, 5, "rect_grad")); return NIL; }
NAT(g_line) { BEGIN(5, 6, "line(x0, y0, x1, y1, colour, [thickness])"); ensure_begun(); gfx_line(A_int(a, n, 0, "line"), A_int(a, n, 1, "line"), A_int(a, n, 2, "line"), A_int(a, n, 3, "line"), A_col(a, n, 4, "line"), n > 5 ? A_int(a, n, 5, "line") : 1); return NIL; }
NAT(g_circle) { BEGIN(4, 5, "circle(x, y, r, colour, [fill=true])"); ensure_begun(); gfx_circle(A_int(a, n, 0, "circle"), A_int(a, n, 1, "circle"), A_int(a, n, 2, "circle"), A_col(a, n, 3, "circle"), n > 4 ? A_bool(a, n, 4, "circle") : true); return NIL; }
NAT(g_triangle) { BEGIN(7, 8, "triangle(x0, y0, x1, y1, x2, y2, colour, [fill=true])"); ensure_begun(); gfx_tri(A_int(a, n, 0, "triangle"), A_int(a, n, 1, "triangle"), A_int(a, n, 2, "triangle"), A_int(a, n, 3, "triangle"), A_int(a, n, 4, "triangle"), A_int(a, n, 5, "triangle"), A_col(a, n, 6, "triangle"), n > 7 ? A_bool(a, n, 7, "triangle") : true); return NIL; }
NAT(g_pixel_set) { BEGIN(3, 3, "pixel_set(x, y, colour)"); ensure_begun(); gfx_px(A_int(a, n, 0, "pixel_set"), A_int(a, n, 1, "pixel_set"), A_col(a, n, 2, "pixel_set")); return NIL; }
NAT(g_text) {
    BEGIN(4, 6, "text(x, y, text, colour, [scale], [shadow=false])"); ensure_begun(); char *s = kc_to_display_string(a[2]); int sc = n > 4 ? imaxi(1, A_int(a, n, 4, "text")) : 1;
    if (n > 5 && A_bool(a, n, 5, "text")) gfx_text_shadow(A_int(a, n, 0, "text"), A_int(a, n, 1, "text"), s, A_col(a, n, 3, "text"), sc, 0xFF000000u); else gfx_text(A_int(a, n, 0, "text"), A_int(a, n, 1, "text"), s, A_col(a, n, 3, "text"), sc); free(s); return NIL;
}
NAT(g_text_width) { BEGIN(1, 2, "text_width(text, [scale])"); char *s = kc_to_display_string(a[0]); int w = gfx_text_w(s, n > 1 ? imaxi(1, A_int(a, n, 1, "text_width")) : 1); free(s); return kc_int(w); }
NAT(g_text_height) { BEGIN(1, 2, "text_height(text, [scale])"); char *s = kc_to_display_string(a[0]); int w = gfx_text_h(s, n > 1 ? imaxi(1, A_int(a, n, 1, "text_height")) : 1); free(s); return kc_int(w); }
NAT(g_clip) { BEGIN(0, 4, "clip([x, y, w, h])  (no arguments resets)"); ensure_window(); if (!n) gfx_clip_reset(); else { int x = A_int(a, n, 0, "clip"), y = A_int(a, n, 1, "clip"); gfx_clip(x, y, x + A_int(a, n, 2, "clip"), y + A_int(a, n, 3, "clip")); } return NIL; }
NAT(g_cam2d) { BEGIN(2, 3, "cam2d(x, y, [zoom])  (centre of the 2D view in world pixels)"); G.c2x = (float)A_num(a, n, 0, "cam2d"); G.c2y = (float)A_num(a, n, 1, "cam2d"); if (n > 2) G.c2zoom = fmaxf(0.05f, (float)A_num(a, n, 2, "cam2d")); return NIL; }
NAT(g_cam2d_get) { BEGIN(0, 0, "cam2d_get() -> [x, y, zoom]"); return arr3(G.c2x, G.c2y, G.c2zoom); }
NAT(g_screen_to_world2d) { BEGIN(2, 2, "screen_to_world2d(sx, sy) -> [x, y]"); ensure_window(); return arr2((A_num(a, n, 0, "x") - G.w * 0.5) / G.c2zoom + G.c2x, (A_num(a, n, 1, "y") - G.h * 0.5) / G.c2zoom + G.c2y); }

/* ---- textures / sprites ---- */
NAT(g_texture) { BEGIN(1, 1, "texture(path) -> id  (PNG, BMP, TGA, PPM; read lazily, evicted when unused)"); ensure_window(); const char *p = A_str(a, n, 0, "texture"); if (!file_exists(p)) gm_err("texture: file not found: %s", p); return kc_int(tex_file(p)); }
NAT(g_texture_new) { BEGIN(2, 3, "texture_new(w, h, [colour])"); ensure_window(); return kc_int(tex_new(A_int(a, n, 0, "texture_new"), A_int(a, n, 1, "texture_new"), n > 2 ? A_col(a, n, 2, "texture_new") : 0)); }
NAT(g_texture_proc) { BEGIN(2, 5, "texture_proc(kind, size, [seed], [colour_a], [colour_b])  kinds: checker noise grass dirt stone sand brick wood planks water metal snow leaves gradient stripes dots"); ensure_window(); return kc_int(tex_proc(A_str(a, n, 0, "texture_proc"), A_int(a, n, 1, "texture_proc"), n > 2 ? (uint32_t)A_int(a, n, 2, "texture_proc") : 0, n > 3 ? A_col(a, n, 3, "texture_proc") : 0, n > 4 ? A_col(a, n, 4, "texture_proc") : 0)); }
NAT(g_texture_size) { BEGIN(1, 1, "texture_size(id) -> [w, h]"); Tex *t = tex_get(res_tex(a[0], "texture_size")); return arr2(t->w, t->h); }
NAT(g_texture_get) { BEGIN(3, 3, "texture_get(id, x, y) -> colour"); Tex *t = tex_get(res_tex(a[0], "texture_get")); int x = A_int(a, n, 1, "texture_get"), y = A_int(a, n, 2, "texture_get"); if (x < 0 || y < 0 || x >= t->w || y >= t->h) return NIL; return kc_int(t->px[(size_t)y * t->w + x]); }
NAT(g_texture_set) { BEGIN(4, 4, "texture_set(id, x, y, colour)"); Tex *t = tex_get(res_tex(a[0], "texture_set")); int x = A_int(a, n, 1, "texture_set"), y = A_int(a, n, 2, "texture_set"); if (x >= 0 && y >= 0 && x < t->w && y < t->h) { t->px[(size_t)y * t->w + x] = A_col(a, n, 3, "texture_set"); t->protect = true; } return NIL; }
NAT(g_texture_free) { BEGIN(1, 1, "texture_free(id)"); tex_free(res_tex(a[0], "texture_free")); return NIL; }
NAT(g_texture_save) { BEGIN(2, 2, "texture_save(id, path)"); Tex *t = tex_get(res_tex(a[0], "texture_save")); char err[256]; if (!img_save(A_str(a, n, 1, "texture_save"), t->px, t->w, t->h, err)) gm_err("texture_save: %s", err); return kc_bool(true); }
NAT(g_sprite) { BEGIN(3, 40, "sprite(texture, x, y, [\"scale\", 2, \"rot\", 45, \"flip_x\", true, \"tint\", c, \"world\", true, \"center\", true, \"sx\",.. \"sw\",..])"); ensure_begun(); Opts o = opts_make(a, n, 3); sprite_draw(res_tex(a[0], "sprite"), (float)A_num(a, n, 1, "sprite"), (float)A_num(a, n, 2, "sprite"), &o); return NIL; }
NAT(g_sheet) { BEGIN(3, 5, "sheet(texture, cell_w, cell_h, [margin], [spacing]) -> sheet id"); ensure_window(); return kc_int(sheet_new(res_tex(a[0], "sheet"), A_int(a, n, 1, "sheet"), A_int(a, n, 2, "sheet"), n > 3 ? A_int(a, n, 3, "sheet") : 0, n > 4 ? A_int(a, n, 4, "sheet") : 0)); }
NAT(g_sheet_draw) { BEGIN(4, 40, "sheet_draw(sheet, index, x, y, [options])"); ensure_begun(); Opts o = opts_make(a, n, 4); return kc_bool(sheet_draw(A_int(a, n, 0, "sheet_draw"), A_int(a, n, 1, "sheet_draw"), (float)A_num(a, n, 2, "sheet_draw"), (float)A_num(a, n, 3, "sheet_draw"), &o)); }
NAT(g_anim) {
    BEGIN(3, 4, "anim(sheet, [frames], fps, [loop=true]) -> id"); ensure_window(); if (a[1].type != VAL_ARRAY || a[1].as.arr->count < 1) gm_err("anim: frames must be a non-empty array of sheet indices");
    int nf = a[1].as.arr->count; int *f = kc_malloc(sizeof(int) * (size_t)nf); for (int i = 0; i < nf; i++) f[i] = (int)V_num(a[1].as.arr->items[i], "anim");
    int id = anim_new(A_int(a, n, 0, "anim"), f, nf, (float)A_num(a, n, 2, "anim"), n > 3 ? A_bool(a, n, 3, "anim") : true); free(f); return kc_int(id);
}
NAT(g_anim_draw) { BEGIN(3, 40, "anim_draw(anim, x, y, [options])"); ensure_begun(); Opts o = opts_make(a, n, 3); return kc_bool(anim_draw(A_int(a, n, 0, "anim_draw"), (float)A_num(a, n, 1, "anim_draw"), (float)A_num(a, n, 2, "anim_draw"), &o)); }
NAT(g_anim_ctl) { BEGIN(2, 2, "anim_ctl(anim, \"play\"|\"pause\"|\"stop\"|\"reset\")"); anim_ctl(A_int(a, n, 0, "anim_ctl"), A_str(a, n, 1, "anim_ctl")); return NIL; }

/* ---- tilemaps ---- */
NAT(g_tilemap) { BEGIN(3, 4, "tilemap(tile_w, tile_h, sheet, [stream_dir]) -> id  (chunks of 32x32 tiles, saved/loaded on demand when a directory is given)"); ensure_window(); return kc_int(tmap_new(A_int(a, n, 0, "tilemap"), A_int(a, n, 1, "tilemap"), A_int(a, n, 2, "tilemap"), n > 3 ? A_str(a, n, 3, "tilemap") : NULL)); }
NAT(g_tile_set) { BEGIN(4, 4, "tile_set(map, tx, ty, tile)  (tile 0 = empty, 1.. = sheet index + 1)"); tmap_set(A_int(a, n, 0, "tile_set"), A_int(a, n, 1, "tile_set"), A_int(a, n, 2, "tile_set"), A_int(a, n, 3, "tile_set")); return NIL; }
NAT(g_tile_get) { BEGIN(3, 3, "tile_get(map, tx, ty)"); return kc_int(tmap_get(A_int(a, n, 0, "tile_get"), A_int(a, n, 1, "tile_get"), A_int(a, n, 2, "tile_get"))); }
NAT(g_tile_fill) { BEGIN(6, 6, "tile_fill(map, tx, ty, w, h, tile)"); int m = A_int(a, n, 0, "tile_fill"), x = A_int(a, n, 1, "tile_fill"), y = A_int(a, n, 2, "tile_fill"), w = A_int(a, n, 3, "tile_fill"), h = A_int(a, n, 4, "tile_fill"), t = A_int(a, n, 5, "tile_fill"); for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) tmap_set(m, x + i, y + j, t); return NIL; }
NAT(g_tilemap_draw) { BEGIN(1, 1, "tilemap_draw(map)  (only visible chunks are touched)"); ensure_begun(); Opts o = opts_make(a, n, n); tmap_draw(A_int(a, n, 0, "tilemap_draw"), &o); return NIL; }
NAT(g_tile_solid) { BEGIN(3, 3, "tile_solid(map, tile, solid)"); tmap_solid(A_int(a, n, 0, "tile_solid"), A_int(a, n, 1, "tile_solid"), A_bool(a, n, 2, "tile_solid")); return NIL; }
NAT(g_tile_hit) { BEGIN(5, 5, "tile_hit(map, x, y, w, h) -> touching a solid tile?"); return kc_bool(tmap_hit(A_int(a, n, 0, "tile_hit"), (float)A_num(a, n, 1, "tile_hit"), (float)A_num(a, n, 2, "tile_hit"), (float)A_num(a, n, 3, "tile_hit"), (float)A_num(a, n, 4, "tile_hit"))); }
NAT(g_tile_move) {
    BEGIN(7, 7, "tile_move(map, x, y, w, h, dx, dy) -> [x, y, hit_x, hit_y]"); float x = (float)A_num(a, n, 1, "tile_move"), y = (float)A_num(a, n, 2, "tile_move"); bool hx, hy;
    tmap_move(A_int(a, n, 0, "tile_move"), &x, &y, (float)A_num(a, n, 3, "tile_move"), (float)A_num(a, n, 4, "tile_move"), (float)A_num(a, n, 5, "tile_move"), (float)A_num(a, n, 6, "tile_move"), &hx, &hy);
    KcValue r = arr2(x, y); kc_array_push(r, kc_bool(hx)); kc_array_push(r, kc_bool(hy)); return r;
}
NAT(g_tile_import) { BEGIN(2, 4, "tile_import(map, csv_path, [ox], [oy]) -> tiles set  (streams the file line by line)"); return kc_int(tmap_import_csv(A_int(a, n, 0, "tile_import"), A_str(a, n, 1, "tile_import"), n > 2 ? A_int(a, n, 2, "tile_import") : 0, n > 3 ? A_int(a, n, 3, "tile_import") : 0)); }
NAT(g_tilemap_flush) { BEGIN(1, 1, "tilemap_flush(map) -> chunks saved"); return kc_int(tmap_flush(A_int(a, n, 0, "tilemap_flush"))); }

/* ---- 3D camera + environment ---- */
NAT(g_camera) {
    BEGIN(3, 6, "camera(x, y, z, [yaw_deg], [pitch_deg], [roll_deg])"); ensure_window(); G.cam = v3((float)A_num(a, n, 0, "camera"), (float)A_num(a, n, 1, "camera"), (float)A_num(a, n, 2, "camera"));
    if (n > 3) G.yaw = DEG2RAD((float)A_num(a, n, 3, "camera")); if (n > 4) G.pitch = fclampf(DEG2RAD((float)A_num(a, n, 4, "camera")), -1.553f, 1.553f); if (n > 5) G.roll = DEG2RAD((float)A_num(a, n, 5, "camera")); return NIL;
}
NAT(g_camera_get) { BEGIN(0, 0, "camera_get() -> [x, y, z, yaw, pitch]"); KcValue r = arr3(G.cam.x, G.cam.y, G.cam.z); kc_array_push(r, kc_float(RAD2DEG(G.yaw))); kc_array_push(r, kc_float(RAD2DEG(G.pitch))); return r; }
NAT(g_look_at) {
    BEGIN(6, 6, "look_at(eye_x, eye_y, eye_z, target_x, target_y, target_z)"); ensure_window(); V3 e = v3((float)A_num(a, n, 0, "look_at"), (float)A_num(a, n, 1, "look_at"), (float)A_num(a, n, 2, "look_at")), t = v3((float)A_num(a, n, 3, "look_at"), (float)A_num(a, n, 4, "look_at"), (float)A_num(a, n, 5, "look_at"));
    V3 d = v3sub(t, e); float l = v3len(d); if (l < 1e-6f) return NIL; G.cam = e; G.yaw = atan2f(-d.x, -d.z); G.pitch = asinf(fclampf(d.y / l, -1, 1)); return NIL;
}
NAT(g_camera_forward) { BEGIN(0, 0, "camera_forward() -> [x, y, z]"); ensure_window(); r3d_matrices(); V3 f = r3d_forward(); return arr3(f.x, f.y, f.z); }
NAT(g_camera_right) { BEGIN(0, 0, "camera_right() -> [x, y, z]"); ensure_window(); r3d_matrices(); V3 f = r3d_right(); return arr3(f.x, f.y, f.z); }
NAT(g_fov) { BEGIN(1, 1, "fov(degrees)"); G.fov = fclampf((float)A_num(a, n, 0, "fov"), 20, 140); return NIL; }
NAT(g_clip_planes) { BEGIN(2, 2, "clip_planes(near, far)"); G.znear = fmaxf(0.01f, (float)A_num(a, n, 0, "clip_planes")); G.zfar = fmaxf(G.znear + 1, (float)A_num(a, n, 1, "clip_planes")); return NIL; }
NAT(g_sky) { BEGIN(1, 2, "sky(top_colour, [horizon_colour])"); R_sky_top = A_col(a, n, 0, "sky"); R_sky_bot = n > 1 ? A_col(a, n, 1, "sky") : col_mix(R_sky_top, 0xFFFFFFFFu, 0.45f); return NIL; }
NAT(g_sky_clouds) { BEGIN(1, 1, "sky_clouds(on)"); R_sky_clouds = A_bool(a, n, 0, "sky_clouds"); return NIL; }
NAT(g_sun) { BEGIN(1, 1, "sun(on)"); R_sun = A_bool(a, n, 0, "sun"); return NIL; }
NAT(g_light) {
    BEGIN(3, 5, "light(dir_x, dir_y, dir_z, [colour], [ambient 0..1])  (direction the light travels)"); V3 d = v3norm(v3((float)A_num(a, n, 0, "light"), (float)A_num(a, n, 1, "light"), (float)A_num(a, n, 2, "light"))); if (v3len(d) > 0) R_light_dir = d;
    if (n > 3) R_light_col = A_col(a, n, 3, "light"); if (n > 4) R_ambient = fclampf((float)A_num(a, n, 4, "light"), 0, 1); return NIL;
}
NAT(g_fog) {
    BEGIN(1, 5, "fog(\"off\"|\"linear\"|\"exp\"|\"cloud\", [start], [end], [colour | \"auto\"])  — with fog_auto on (default) the end is clamped to the streaming horizon");
    const char *m = A_str(a, n, 0, "fog"); R_fog.mode = !strcmp(m, "off") ? 0 : (!strcmp(m, "linear") || !strcmp(m, "fog")) ? 1 : (!strcmp(m, "exp") || !strcmp(m, "exponential")) ? 2 : !strcmp(m, "cloud") ? 3 : -1;
    if (R_fog.mode < 0) gm_err("fog: unknown mode \"%s\" (off, linear, exp, cloud)", m);
    if (n > 1) R_fog.start = (float)A_num(a, n, 1, "fog"); if (n > 2) { R_fog.end = fmaxf(8.0f, (float)A_num(a, n, 2, "fog")); R_fog.density = 2.2f / R_fog.end; }
    if (n > 3) { if (a[3].type == VAL_STRING && !strcmp(a[3].as.s->data, "auto")) R_fog.auto_color = true; else { R_fog.color = A_col(a, n, 3, "fog"); R_fog.auto_color = false; } }
    R_fog.cur_end = 0; return NIL;
}
NAT(g_fog_auto) { BEGIN(1, 1, "fog_auto(on)  (hide not-yet-streamed chunks behind fog/cloud)"); R_fog.autofog = A_bool(a, n, 0, "fog_auto"); R_fog.cur_end = 0; return NIL; }
NAT(g_world_to_screen) { BEGIN(3, 3, "world_to_screen(x, y, z) -> [sx, sy, depth] or null if behind the camera"); ensure_window(); r3d_matrices(); float sx, sy, d; if (!r3d_project(v3((float)A_num(a, n, 0, "w2s"), (float)A_num(a, n, 1, "w2s"), (float)A_num(a, n, 2, "w2s")), &sx, &sy, &d)) return NIL; return arr3(sx, sy, d); }
NAT(g_screen_ray) {
    BEGIN(0, 2, "screen_ray([sx, sy]) -> [ox, oy, oz, dx, dy, dz]  (defaults to the screen centre)"); ensure_window(); r3d_matrices(); V3 o, d; r3d_ray(n > 0 ? (float)A_num(a, n, 0, "screen_ray") : G.w * 0.5f, n > 1 ? (float)A_num(a, n, 1, "screen_ray") : G.h * 0.5f, &o, &d);
    KcValue r = arr3(o.x, o.y, o.z); kc_array_push(r, kc_float(d.x)); kc_array_push(r, kc_float(d.y)); kc_array_push(r, kc_float(d.z)); return r;
}

/* ---- immediate 3D drawing ---- */
NAT(g_draw_model) {
    BEGIN(4, 40, "draw_model(model, x, y, z, [\"rot\", [rx,ry,rz], \"yaw\", deg, \"scale\", s | [sx,sy,sz], \"tint\", c, \"tex\", t, \"wire\", true])");
    ensure_begun(); int id = res_model(a[0], "draw_model"); Mesh *m = model_mesh(id); if (!m) return NIL; Model *mi = model_info(id); Opts o = opts_make(a, n, 4);
    V3 p = v3((float)A_num(a, n, 1, "draw_model"), (float)A_num(a, n, 2, "draw_model"), (float)A_num(a, n, 3, "draw_model")), rot = opt_v3(&o, "rot", v3(0, 0, 0)), sc = opt_v3(&o, "scale", v3(1, 1, 1));
    rot.y += (float)opt_num(&o, "yaw", 0); rot.x += (float)opt_num(&o, "pitch", 0); rot.z += (float)opt_num(&o, "roll", 0);
    r3d_matrices(); float rad = v3len(v3sub(m->bmax, m->bmin)) * fmaxf(sc.x, fmaxf(sc.y, sc.z)); if (!r3d_aabb_visible(v3sub(p, v3(rad, rad, rad)), v3add(p, v3(rad, rad, rad)))) return NIL;
    M4 mm = m4_trs(p, rot, sc); int tex = opt_has(&o, "tex") ? res_tex(*opt_find(&o, "tex"), "draw_model") : (mi ? mi->tex_override : -1);
    r3d_draw_mesh(m, &mm, col_mul(opt_col(&o, "tint", 0xFFFFFFFFu), mi ? mi->tint : 0xFFFFFFFFu), tex, opt_bool(&o, "wire", false) ? RF_WIRE : 0); return NIL;
}
NAT(g_billboard) { BEGIN(6, 40, "billboard(texture, x, y, z, w, h, [\"tint\", c])  (camera-facing quad, alpha cut-out)"); ensure_begun(); r3d_matrices(); Opts o = opts_make(a, n, 6); r3d_billboard(res_tex(a[0], "billboard"), v3((float)A_num(a, n, 1, "billboard"), (float)A_num(a, n, 2, "billboard"), (float)A_num(a, n, 3, "billboard")), (float)A_num(a, n, 4, "billboard"), (float)A_num(a, n, 5, "billboard"), opt_col(&o, "tint", 0xFFFFFFFFu), 0); return NIL; }
NAT(g_line3d) { BEGIN(7, 7, "line3d(x0, y0, z0, x1, y1, z1, colour)"); ensure_begun(); r3d_matrices(); r3d_line(v3((float)A_num(a, n, 0, "line3d"), (float)A_num(a, n, 1, "line3d"), (float)A_num(a, n, 2, "line3d")), v3((float)A_num(a, n, 3, "line3d"), (float)A_num(a, n, 4, "line3d"), (float)A_num(a, n, 5, "line3d")), A_col(a, n, 6, "line3d")); return NIL; }
NAT(g_box3d) { BEGIN(7, 7, "box3d(min_x, min_y, min_z, max_x, max_y, max_z, colour)  (wireframe)"); ensure_begun(); r3d_matrices(); r3d_box(v3((float)A_num(a, n, 0, "box3d"), (float)A_num(a, n, 1, "box3d"), (float)A_num(a, n, 2, "box3d")), v3((float)A_num(a, n, 3, "box3d"), (float)A_num(a, n, 4, "box3d"), (float)A_num(a, n, 5, "box3d")), A_col(a, n, 6, "box3d")); return NIL; }

/* ---- models ---- */
NAT(g_model) { BEGIN(1, 40, "model(name_or_path, [options]) -> id.  Built-in names: see game.models(). Options: scale, unreal, meters, center, ground, flip_uv, flip_winding, tint, lazy, name"); ensure_window();
    if (a[0].type == VAL_STRING) { int id = model_builtin(a[0].as.s->data); if (id >= 0) return kc_int(id); Opts o = opts_make(a, n, 1); return kc_int(model_load(a[0].as.s->data, &o)); } return kc_int(res_model(a[0], "model")); }
NAT(g_model_load) { BEGIN(1, 40, "model_load(path, [\"scale\", s, \"unreal\", true, \"center\", true, \"lazy\", false ...]) -> id"); ensure_window(); Opts o = opts_make(a, n, 1); return kc_int(model_load(A_str(a, n, 0, "model_load"), &o)); }
NAT(g_models) { BEGIN(0, 0, "models() -> names of the built-in default models"); return model_builtin_names(); }
NAT(g_model_formats) { BEGIN(0, 0, "model_formats()"); return model_formats(); }
NAT(g_model_info) {
    BEGIN(1, 1, "model_info(model) -> object"); int id = res_model(a[0], "model_info"); Model *mi = model_info(id); Mesh *m = model_mesh(id); KcValue o = obj_new();
    obj_int(o, "id", id); obj_str(o, "name", mi->name ? mi->name : ""); obj_str(o, "path", mi->path ? mi->path : ""); obj_bool(o, "builtin", mi->builtin); obj_bool(o, "loaded", m != NULL); obj_bool(o, "failed", mi->failed);
    if (m) { obj_str(o, "format", m->fmt); obj_int(o, "vertices", m->nv); obj_int(o, "triangles", m->ni / 3); obj_int(o, "parts", m->np); obj_int(o, "bytes", (int64_t)m->bytes);
        obj_val(o, "min", arr3(m->bmin.x, m->bmin.y, m->bmin.z)); obj_val(o, "max", arr3(m->bmax.x, m->bmax.y, m->bmax.z)); obj_val(o, "size", arr3(m->bmax.x - m->bmin.x, m->bmax.y - m->bmin.y, m->bmax.z - m->bmin.z)); }
    return o;
}
NAT(g_model_from_text) { BEGIN(1, 2, "model_from_text(kcm_text, [name]) -> id  (see docs: part / v / f / box / cyl / sphere / cone / plane / torus)"); ensure_window(); char err[256] = ""; int id = model_from_kcm(A_str(a, n, 0, "model_from_text"), n > 1 ? A_str(a, n, 1, "model_from_text") : "custom", err); if (id < 0) gm_err("model_from_text: %s", err); return kc_int(id); }
NAT(g_model_begin) { BEGIN(0, 0, "model_begin()  (start building a custom mesh)"); mb_begin(); return NIL; }
NAT(g_model_vertex) { BEGIN(3, 9, "model_vertex(x, y, z, [u, v], [colour], [nx, ny, nz]) -> index"); uint32_t c = n > 5 ? A_col(a, n, 5, "model_vertex") : 0xFFFFFFFFu; bool hn = n >= 9; return kc_int(mb_vertex(v3((float)A_num(a, n, 0, "model_vertex"), (float)A_num(a, n, 1, "model_vertex"), (float)A_num(a, n, 2, "model_vertex")), hn ? v3((float)A_num(a, n, 6, "model_vertex"), (float)A_num(a, n, 7, "model_vertex"), (float)A_num(a, n, 8, "model_vertex")) : v3(0, 0, 0), n > 3 ? (float)A_num(a, n, 3, "model_vertex") : 0, n > 4 ? (float)A_num(a, n, 4, "model_vertex") : 0, c, hn)); }
NAT(g_model_tri) { BEGIN(3, 3, "model_tri(a, b, c)  (counter-clockwise = front)"); int v[3]; for (int i = 0; i < 3; i++) { v[i] = A_int(a, n, i, "model_tri"); if (v[i] < 0 || v[i] >= mb_vcount()) gm_err("model_tri: vertex index %d does not exist", v[i]); } mb_tri(v[0], v[1], v[2]); return NIL; }
NAT(g_model_quad) { BEGIN(4, 4, "model_quad(a, b, c, d)"); int v[4]; for (int i = 0; i < 4; i++) { v[i] = A_int(a, n, i, "model_quad"); if (v[i] < 0 || v[i] >= mb_vcount()) gm_err("model_quad: vertex index %d does not exist", v[i]); } mb_tri(v[0], v[1], v[2]); mb_tri(v[0], v[2], v[3]); return NIL; }
NAT(g_model_part) { BEGIN(0, 40, "model_part([\"tex\", t, \"color\", c, \"twosided\", true, \"translucent\", true, \"unlit\", true])  (material for the triangles that follow)"); Opts o = opts_make(a, n, 0); mb_part(opt_has(&o, "tex") ? res_tex(*opt_find(&o, "tex"), "model_part") : -1, opt_col(&o, "color", 0xFFFFFFFFu), opt_bool(&o, "twosided", false), opt_bool(&o, "translucent", false), opt_bool(&o, "unlit", false)); return NIL; }
NAT(g_model_end) { BEGIN(0, 1, "model_end([name]) -> id"); Mesh *m = mb_end(); if (!m->nv) { mesh_free(m); gm_err("model_end: the model has no vertices"); } return kc_int(model_from_mesh(m, n ? A_str(a, n, 0, "model_end") : "custom")); }
NAT(g_model_texture) { BEGIN(2, 2, "model_texture(model, texture)  (override every part's texture)"); Model *mi = model_info(res_model(a[0], "model_texture")); mi->tex_override = a[1].type == VAL_NULL ? -1 : res_tex(a[1], "model_texture"); return NIL; }
NAT(g_model_tint) { BEGIN(2, 2, "model_tint(model, colour)"); model_info(res_model(a[0], "model_tint"))->tint = A_col(a, n, 1, "model_tint"); return NIL; }
NAT(g_model_free) { BEGIN(1, 1, "model_free(model)"); model_free(res_model(a[0], "model_free")); return NIL; }
NAT(g_model_save) { BEGIN(2, 2, "model_save(model, path.kcm)"); Mesh *m = model_mesh(res_model(a[0], "model_save")); if (!m) gm_err("model_save: model could not be loaded"); if (!model_save_kcm(m, A_str(a, n, 1, "model_save"))) gm_err("model_save: cannot write file"); return kc_bool(true); }

/* ---- world ---- */
NAT(g_world) { BEGIN(0, 40, "world([directory], [\"gen\", \"nature\"|\"hills\"|\"mountains\"|\"islands\"|\"flat\"|\"empty\", \"seed\", n, \"radius\", chunks, \"height\", 64, \"trees\", 1.0])  — open a streamed world"); ensure_window(); const char *dir = (n % 2 == 1) ? A_str(a, n, 0, "world") : ""; Opts o = opts_make(a, n, n % 2 == 1 ? 1 : 0); world_open(dir, &o); return kc_bool(true); }
NAT(g_world_close) { BEGIN(0, 0, "world_close()  (saves edited chunks, frees everything)"); world_reset(); return NIL; }
NAT(g_world_config) { BEGIN(2, 2, "world_config(\"radius\"|\"keep\"|\"max_chunks\"|\"loads_per_frame\"|\"meshes_per_frame\"|\"trees\", value)"); world_config_num(A_str(a, n, 0, "world_config"), A_num(a, n, 1, "world_config")); return NIL; }
NAT(g_world_flush) { BEGIN(0, 0, "world_flush()  (save edited chunks now)"); world_flush(); return NIL; }
NAT(g_world_bake) { BEGIN(1, 1, "world_bake(radius_chunks) -> chunks written  (write chunk files around the camera so the world is pure disk-streamed data)"); return kc_int(world_bake(A_int(a, n, 0, "world_bake"))); }
NAT(g_block) { BEGIN(3, 3, "block(x, y, z) -> block id, or -1 if that chunk is not loaded"); bool ld; int b = world_get(A_int(a, n, 0, "block"), A_int(a, n, 1, "block"), A_int(a, n, 2, "block"), &ld); return kc_int(ld ? b : -1); }
NAT(g_set_block) {
    BEGIN(4, 4, "set_block(x, y, z, block_id_or_name) -> ok (false if the chunk is not loaded)"); int b = a[3].type == VAL_STRING ? block_by_name(a[3].as.s->data) : A_int(a, n, 3, "set_block"); if (b < 0) gm_err("set_block: unknown block \"%s\"", a[3].as.s->data);
    return kc_bool(world_set(A_int(a, n, 0, "set_block"), A_int(a, n, 1, "set_block"), A_int(a, n, 2, "set_block"), b));
}
NAT(g_block_define) {
    BEGIN(2, 6, "block_define(name, top_tex, [side_tex], [bottom_tex], [solid=true], [translucent=false]) -> id"); ensure_window(); (void)world_active();
    int t = res_tex(a[1], "block_define"), s = n > 2 && a[2].type != VAL_NULL ? res_tex(a[2], "block_define") : -1, b = n > 3 && a[3].type != VAL_NULL ? res_tex(a[3], "block_define") : -1;
    extern void game_world_defaults(void); game_world_defaults();
    return kc_int(block_define(A_str(a, n, 0, "block_define"), t, s, b, n > 4 ? A_bool(a, n, 4, "block_define") : true, n > 5 ? A_bool(a, n, 5, "block_define") : false, 0));
}
NAT(g_block_id) { BEGIN(1, 1, "block_id(name) -> id or -1"); extern void game_world_defaults(void); game_world_defaults(); return kc_int(block_by_name(A_str(a, n, 0, "block_id"))); }
NAT(g_block_name) { BEGIN(1, 1, "block_name(id)"); const char *s = block_name(A_int(a, n, 0, "block_name")); return s ? kc_string(s) : NIL; }
NAT(g_surface) { BEGIN(2, 2, "surface(x, z) -> top y of the column, or -1 if not loaded"); return kc_int(world_surface(A_int(a, n, 0, "surface"), A_int(a, n, 1, "surface"), NULL)); }
NAT(g_raycast) {
    BEGIN(6, 7, "raycast(ox, oy, oz, dx, dy, dz, [max_dist=8]) -> null | {x,y,z, nx,ny,nz, block, name, dist}  (voxel DDA through loaded chunks)");
    V3 o = v3((float)A_num(a, n, 0, "raycast"), (float)A_num(a, n, 1, "raycast"), (float)A_num(a, n, 2, "raycast")), d = v3((float)A_num(a, n, 3, "raycast"), (float)A_num(a, n, 4, "raycast"), (float)A_num(a, n, 5, "raycast"));
    int hx, hy, hz, nx, ny, nz, blk; float dist; bool unl; if (!world_raycast(o, d, n > 6 ? (float)A_num(a, n, 6, "raycast") : 8.0f, &hx, &hy, &hz, &nx, &ny, &nz, &blk, &dist, &unl)) return NIL;
    KcValue r = obj_new(); obj_int(r, "x", hx); obj_int(r, "y", hy); obj_int(r, "z", hz); obj_int(r, "nx", nx); obj_int(r, "ny", ny); obj_int(r, "nz", nz); obj_int(r, "block", blk); obj_str(r, "name", block_name(blk) ? block_name(blk) : ""); obj_num(r, "dist", dist); return r;
}
NAT(g_collide_move) {
    BEGIN(8, 9, "collide_move(x, y, z, half_width, height, dx, dy, dz, [step_up=0]) -> [x, y, z, grounded, hit_x, hit_y, hit_z]  (AABB vs the voxel world; unloaded chunks are solid)");
    V3 p = v3((float)A_num(a, n, 0, "collide_move"), (float)A_num(a, n, 1, "collide_move"), (float)A_num(a, n, 2, "collide_move")); bool fl[4];
    world_move(&p, (float)A_num(a, n, 3, "collide_move"), (float)A_num(a, n, 4, "collide_move"), v3((float)A_num(a, n, 5, "collide_move"), (float)A_num(a, n, 6, "collide_move"), (float)A_num(a, n, 7, "collide_move")), n > 8 ? (float)A_num(a, n, 8, "collide_move") : 0, fl);
    KcValue r = arr3(p.x, p.y, p.z); kc_array_push(r, kc_bool(fl[3])); kc_array_push(r, kc_bool(fl[0])); kc_array_push(r, kc_bool(fl[1])); kc_array_push(r, kc_bool(fl[2])); return r;
}

/* ---- entities / particles ---- */
NAT(g_spawn) { BEGIN(4, 40, "spawn(model, x, y, z, [\"scale\", s, \"rot\", [..], \"tint\", c, \"gravity\", true, \"collide\", true, \"radius\", .3, \"height\", 1.8, \"spin\", deg_per_s, \"tag\", n]) -> id"); ensure_window(); int id = ent_spawn(res_model(a[0], "spawn"), v3((float)A_num(a, n, 1, "spawn"), (float)A_num(a, n, 2, "spawn"), (float)A_num(a, n, 3, "spawn"))); Opts o = opts_make(a, n, 4); ent_apply_opts(id, &o); return kc_int(id); }
NAT(g_despawn) { BEGIN(1, 1, "despawn(entity)"); ent_despawn(A_int(a, n, 0, "despawn")); return NIL; }
NAT(g_set) { BEGIN(3, 5, "set(entity, property, value...)  properties: pos x y z rot yaw pitch roll scale vel vx vy vz tint visible gravity collide wire radius height spin tex tag model"); if (!ent_set(A_int(a, n, 0, "set"), A_str(a, n, 1, "set"), a + 2, n - 2)) gm_err("set: entity %d does not exist", A_int(a, n, 0, "set")); return NIL; }
NAT(g_get) { BEGIN(2, 2, "get(entity, property)"); return ent_get(A_int(a, n, 0, "get"), A_str(a, n, 1, "get")); }
NAT(g_alive) { BEGIN(1, 1, "alive(entity)"); return kc_bool(ent_alive(A_int(a, n, 0, "alive"))); }
NAT(g_entity_count) { BEGIN(0, 0, "entity_count()"); return kc_int(ent_count()); }
NAT(g_move) { BEGIN(4, 4, "move(entity, dx, dy, dz) -> grounded  (collides with the world when the entity has collide = true)"); return kc_bool(ent_move(A_int(a, n, 0, "move"), v3((float)A_num(a, n, 1, "move"), (float)A_num(a, n, 2, "move"), (float)A_num(a, n, 3, "move")))); }
NAT(g_pick) { BEGIN(0, 3, "pick([sx, sy], [max_dist]) -> entity id under the screen point (default centre), or -1"); ensure_window(); r3d_matrices(); V3 o, d; r3d_ray(n > 1 ? (float)A_num(a, n, 0, "pick") : G.w * 0.5f, n > 1 ? (float)A_num(a, n, 1, "pick") : G.h * 0.5f, &o, &d); float dist; return kc_int(ent_pick(o, d, n > 2 ? (float)A_num(a, n, 2, "pick") : 100.0f, &dist)); }
NAT(g_gravity) { BEGIN(1, 1, "gravity(units_per_s2)  (default 26)"); G_gravity = (float)A_num(a, n, 0, "gravity"); return NIL; }
NAT(g_emit) { BEGIN(4, 40, "emit(x, y, z, count, [\"color\", c, \"color_end\", c, \"speed\", s, \"life\", secs, \"size\", s, \"gravity\", g, \"spread\", s])  (3D particles)"); ensure_begun(); Opts o = opts_make(a, n, 4); part_emit(true, v3((float)A_num(a, n, 0, "emit"), (float)A_num(a, n, 1, "emit"), (float)A_num(a, n, 2, "emit")), A_int(a, n, 3, "emit"), &o); return NIL; }
NAT(g_emit2d) { BEGIN(3, 40, "emit2d(x, y, count, [options])  (2D particles, world coordinates)"); Opts o = opts_make(a, n, 3); part_emit(false, v3((float)A_num(a, n, 0, "emit2d"), (float)A_num(a, n, 1, "emit2d"), 0), A_int(a, n, 2, "emit2d"), &o); return NIL; }

/* ---- HUD ---- */
NAT(g_hud) { BEGIN(1, 40, "hud(kind, ...) -> id.  kinds: text bar icon panel button crosshair hotbar minimap compass label3d (see docs/STD_GAME.md)"); ensure_window(); return kc_int(hud_add(A_str(a, n, 0, "hud"), a + 1, n - 1)); }
NAT(g_hud_set) { BEGIN(3, 3, "hud_set(id, property, value)"); if (!hud_set(A_int(a, n, 0, "hud_set"), A_str(a, n, 1, "hud_set"), a[2])) gm_err("hud_set: element %d does not exist", A_int(a, n, 0, "hud_set")); return NIL; }
NAT(g_hud_get) { BEGIN(2, 2, "hud_get(id, property)"); return hud_get(A_int(a, n, 0, "hud_get"), A_str(a, n, 1, "hud_get")); }
NAT(g_hud_remove) { BEGIN(1, 1, "hud_remove(id)"); hud_remove(A_int(a, n, 0, "hud_remove")); return NIL; }
NAT(g_hud_clear) { BEGIN(0, 0, "hud_clear()"); hud_clear(); return NIL; }
NAT(g_hud_tween) { BEGIN(4, 5, "hud_tween(id, property, target, seconds, [\"linear\"|\"in\"|\"out\"|\"inout\"|\"back\"])"); hud_tween(A_int(a, n, 0, "hud_tween"), A_str(a, n, 1, "hud_tween"), (float)A_num(a, n, 2, "hud_tween"), (float)A_num(a, n, 3, "hud_tween"), n > 4 ? A_str(a, n, 4, "hud_tween") : "linear"); return NIL; }
NAT(g_hud_notify) { BEGIN(1, 3, "hud_notify(text, [colour], [seconds])  (toast)"); char *s = kc_to_display_string(a[0]); hud_notify(s, n > 1 ? A_col(a, n, 1, "hud_notify") : 0xFF6AA8FFu, n > 2 ? (float)A_num(a, n, 2, "hud_notify") : 3.0f); free(s); return NIL; }
NAT(g_hud_float) { BEGIN(4, 6, "hud_float(x, y, z, text, [colour], [seconds])  (rising world-anchored number, e.g. damage)"); char *s = kc_to_display_string(a[3]); hud_float_text(v3((float)A_num(a, n, 0, "hud_float"), (float)A_num(a, n, 1, "hud_float"), (float)A_num(a, n, 2, "hud_float")), s, n > 4 ? A_col(a, n, 4, "hud_float") : 0xFFFFD040u, n > 5 ? (float)A_num(a, n, 5, "hud_float") : 1.2f); free(s); return NIL; }
NAT(g_hud_dialog) { BEGIN(2, 3, "hud_dialog(speaker, text, [choices])  (typewriter box; space/enter/click advance, 1-9 pick a choice)"); ensure_window(); char *sp = kc_to_display_string(a[0]), *tx = kc_to_display_string(a[1]); if (n > 2 && a[2].type != VAL_ARRAY) gm_err("hud_dialog: choices must be an array"); hud_dialog_open(sp, tx, n > 2 ? a[2].as.arr->items : NULL, n > 2 ? a[2].as.arr->count : 0); free(sp); free(tx); return NIL; }
NAT(g_dialog_result) { BEGIN(0, 0, "dialog_result() -> -2 while open, -1 closed without choice, else the chosen index (-3 = never opened)"); return kc_int(hud_dialog_result()); }
NAT(g_dialog_active) { BEGIN(0, 0, "dialog_active()"); return kc_bool(hud_dialog_active()); }
NAT(g_hud_debug) { BEGIN(0, 1, "hud_debug([on])  (F3-style overlay: fps, position, chunks, triangles, memory)"); hud_debug(n ? A_bool(a, n, 0, "hud_debug") : true); return NIL; }
NAT(g_hud_scale) { BEGIN(1, 1, "hud_scale(n)  (integer UI scale)"); hud_scale(A_int(a, n, 0, "hud_scale")); return NIL; }
NAT(g_hud_at) { BEGIN(2, 2, "hud_at(x, y) -> id of the clickable element there, or -1"); return kc_int(hud_hit(A_int(a, n, 0, "hud_at"), A_int(a, n, 1, "hud_at"))); }


/* ---- maths (KingCobra has no math builtins besides sqrt) ---- */
#define M1(nm, expr, doc) NAT(nm) { BEGIN(1, 1, doc); double x = A_num(a, n, 0, "math"); (void)x; return kc_float(expr); }
M1(m_sin, sin(x), "sin(radians)") M1(m_cos, cos(x), "cos(radians)") M1(m_tan, tan(x), "tan(radians)") M1(m_asin, asin(fmax(-1, fmin(1, x))), "asin(x)") M1(m_acos, acos(fmax(-1, fmin(1, x))), "acos(x)")
M1(m_dsin, sin(x * 0.017453292519943295), "dsin(degrees)") M1(m_dcos, cos(x * 0.017453292519943295), "dcos(degrees)")
M1(m_sqrt, sqrt(x), "sqrt(x)") M1(m_abs, fabs(x), "abs(x)") M1(m_floor, floor(x), "floor(x)") M1(m_ceil, ceil(x), "ceil(x)") M1(m_round, floor(x + 0.5), "round(x)")
M1(m_rad, x * 0.017453292519943295, "rad(degrees)") M1(m_deg, x * 57.29577951308232, "deg(radians)") M1(m_sign, x > 0 ? 1.0 : (x < 0 ? -1.0 : 0.0), "sign(x)")
NAT(m_atan2) { BEGIN(2, 2, "atan2(y, x) -> radians"); return kc_float(atan2(A_num(a, n, 0, "atan2"), A_num(a, n, 1, "atan2"))); }
NAT(m_pow) { BEGIN(2, 2, "pow(x, y)"); return kc_float(pow(A_num(a, n, 0, "pow"), A_num(a, n, 1, "pow"))); }
NAT(m_min) { BEGIN(2, 2, "min(a, b)"); double x = A_num(a, n, 0, "min"), y = A_num(a, n, 1, "min"); return (a[0].type == VAL_INT && a[1].type == VAL_INT) ? kc_int((int64_t)(x < y ? x : y)) : kc_float(x < y ? x : y); }
NAT(m_max) { BEGIN(2, 2, "max(a, b)"); double x = A_num(a, n, 0, "max"), y = A_num(a, n, 1, "max"); return (a[0].type == VAL_INT && a[1].type == VAL_INT) ? kc_int((int64_t)(x > y ? x : y)) : kc_float(x > y ? x : y); }
NAT(m_clamp) { BEGIN(3, 3, "clamp(x, lo, hi)"); double x = A_num(a, n, 0, "clamp"), lo = A_num(a, n, 1, "clamp"), hi = A_num(a, n, 2, "clamp"); return kc_float(x < lo ? lo : (x > hi ? hi : x)); }
NAT(m_lerp) { BEGIN(3, 3, "lerp(a, b, t)"); double x = A_num(a, n, 0, "lerp"), y = A_num(a, n, 1, "lerp"); return kc_float(x + (y - x) * A_num(a, n, 2, "lerp")); }
NAT(m_smoothstep) { BEGIN(3, 3, "smoothstep(edge0, edge1, x)"); double e0 = A_num(a, n, 0, "smoothstep"), e1 = A_num(a, n, 1, "smoothstep"), x = A_num(a, n, 2, "smoothstep"); double t = e1 != e0 ? (x - e0) / (e1 - e0) : 0; t = t < 0 ? 0 : (t > 1 ? 1 : t); return kc_float(t * t * (3 - 2 * t)); }

static KcModuleEntry ENTRIES[] = {
    {"sin", m_sin}, {"cos", m_cos}, {"tan", m_tan}, {"asin", m_asin}, {"acos", m_acos}, {"atan2", m_atan2}, {"dsin", m_dsin}, {"dcos", m_dcos}, {"sqrt", m_sqrt}, {"abs", m_abs}, {"floor", m_floor}, {"ceil", m_ceil}, {"round", m_round},
    {"rad", m_rad}, {"deg", m_deg}, {"sign", m_sign}, {"pow", m_pow}, {"min", m_min}, {"max", m_max}, {"clamp", m_clamp}, {"lerp", m_lerp}, {"smoothstep", m_smoothstep},
    {"window", g_window}, {"title", g_title}, {"size", g_size}, {"headless", g_headless}, {"is_headless", g_is_headless}, {"run", g_run}, {"poll", g_poll}, {"begin", g_begin}, {"render3d", g_render3d}, {"present", g_present},
    {"quit", g_quit}, {"dt", g_dt}, {"time", g_time}, {"fps", g_fps}, {"frame", g_frame}, {"target_fps", g_target_fps}, {"advance", g_advance}, {"screenshot", g_screenshot}, {"pixel", g_pixel}, {"mode", g_mode},
    {"hud_auto", g_hud_auto}, {"budget", g_budget}, {"stats", g_stats}, {"reset", g_reset},
    {"key", g_key}, {"key_pressed", g_key_pressed}, {"key_released", g_key_released}, {"mouse", g_mouse}, {"mouse_delta", g_mouse_delta}, {"mouse_down", g_mouse_down}, {"mouse_pressed", g_mouse_pressed}, {"mouse_released", g_mouse_released},
    {"wheel", g_wheel}, {"capture_mouse", g_capture_mouse}, {"typed", g_typed}, {"send_key", g_send_key}, {"send_mouse", g_send_mouse},
    {"rgb", g_rgb}, {"hsv", g_hsv}, {"mix", g_mix}, {"noise2", g_noise2}, {"noise3", g_noise3}, {"fbm", g_fbm}, {"seed", g_seed}, {"random", g_random}, {"overlap", g_overlap},
    {"clear", g_clear}, {"rect", g_rect}, {"rect_grad", g_rect_grad}, {"line", g_line}, {"circle", g_circle}, {"triangle", g_triangle}, {"pixel_set", g_pixel_set}, {"text", g_text}, {"text_width", g_text_width}, {"text_height", g_text_height}, {"clip", g_clip},
    {"cam2d", g_cam2d}, {"cam2d_get", g_cam2d_get}, {"screen_to_world2d", g_screen_to_world2d},
    {"texture", g_texture}, {"texture_new", g_texture_new}, {"texture_proc", g_texture_proc}, {"texture_size", g_texture_size}, {"texture_get", g_texture_get}, {"texture_set", g_texture_set}, {"texture_free", g_texture_free}, {"texture_save", g_texture_save},
    {"sprite", g_sprite}, {"sheet", g_sheet}, {"sheet_draw", g_sheet_draw}, {"anim", g_anim}, {"anim_draw", g_anim_draw}, {"anim_ctl", g_anim_ctl},
    {"tilemap", g_tilemap}, {"tile_set", g_tile_set}, {"tile_get", g_tile_get}, {"tile_fill", g_tile_fill}, {"tilemap_draw", g_tilemap_draw}, {"tile_solid", g_tile_solid}, {"tile_hit", g_tile_hit}, {"tile_move", g_tile_move}, {"tile_import", g_tile_import}, {"tilemap_flush", g_tilemap_flush},
    {"camera", g_camera}, {"camera_get", g_camera_get}, {"look_at", g_look_at}, {"camera_forward", g_camera_forward}, {"camera_right", g_camera_right}, {"fov", g_fov}, {"clip_planes", g_clip_planes},
    {"sky", g_sky}, {"sky_clouds", g_sky_clouds}, {"sun", g_sun}, {"light", g_light}, {"fog", g_fog}, {"fog_auto", g_fog_auto}, {"world_to_screen", g_world_to_screen}, {"screen_ray", g_screen_ray},
    {"draw_model", g_draw_model}, {"billboard", g_billboard}, {"line3d", g_line3d}, {"box3d", g_box3d},
    {"model", g_model}, {"model_load", g_model_load}, {"models", g_models}, {"model_formats", g_model_formats}, {"model_info", g_model_info}, {"model_from_text", g_model_from_text},
    {"model_begin", g_model_begin}, {"model_vertex", g_model_vertex}, {"model_tri", g_model_tri}, {"model_quad", g_model_quad}, {"model_part", g_model_part}, {"model_end", g_model_end},
    {"model_texture", g_model_texture}, {"model_tint", g_model_tint}, {"model_free", g_model_free}, {"model_save", g_model_save},
    {"world", g_world}, {"world_close", g_world_close}, {"world_config", g_world_config}, {"world_flush", g_world_flush}, {"world_bake", g_world_bake}, {"block", g_block}, {"set_block", g_set_block},
    {"block_define", g_block_define}, {"block_id", g_block_id}, {"block_name", g_block_name}, {"surface", g_surface}, {"raycast", g_raycast}, {"collide_move", g_collide_move},
    {"spawn", g_spawn}, {"despawn", g_despawn}, {"set", g_set}, {"get", g_get}, {"alive", g_alive}, {"entity_count", g_entity_count}, {"move", g_move}, {"pick", g_pick}, {"gravity", g_gravity}, {"emit", g_emit}, {"emit2d", g_emit2d},
    {"hud", g_hud}, {"hud_set", g_hud_set}, {"hud_get", g_hud_get}, {"hud_remove", g_hud_remove}, {"hud_clear", g_hud_clear}, {"hud_tween", g_hud_tween}, {"hud_notify", g_hud_notify}, {"hud_float", g_hud_float},
    {"hud_dialog", g_hud_dialog}, {"dialog_result", g_dialog_result}, {"dialog_active", g_dialog_active}, {"hud_debug", g_hud_debug}, {"hud_scale", g_hud_scale}, {"hud_at", g_hud_at},
};

KcModule *kc_mod_game(void) {
    static KcModule m = { "game", ENTRIES, sizeof(ENTRIES) / sizeof(ENTRIES[0]) };
    static bool init = false;
    if (!init) { init = true; game_defaults(); }
    return &m;
}
