/* std.ascii_screen — terminal backends, live keyboard/mouse input, diff-based frame output, loop timing.
 *
 * Backends: POSIX termios (Linux/macOS/BSD), Windows console (VT output + ReadConsoleInput), and a
 * headless backend (no terminal needed: CI, tests, piping) with virtual time and injectable input.
 *
 * Live keys: terminals normally only report key *presses*. We use, in order of preference:
 *   "kitty"   the kitty keyboard protocol (kitty, foot, WezTerm, Ghostty, Alacritty>=0.13, iTerm2, ...):
 *             real press / repeat / release events;
 *   "windows" the console input API: real key-down / key-up events;
 *   "timed"   fallback: a key counts as held for a short window after its last (auto-repeat) event.
 */
#include "as_internal.h"
#include <signal.h>
#include <time.h>
#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <io.h>
#  include <fcntl.h>
#else
#  include <unistd.h>
#  include <termios.h>
#  include <sys/ioctl.h>
#  include <sys/select.h>
#  include <fcntl.h>
#endif

Term T;
Canvas *S_screen = NULL;
static Canvas *S_front = NULL;
static bool need_full = true, atexit_set = false;

/* ===================================================================== keys */
static const struct { const char *name; int code; } KEYS[] = {
    {"space", 32}, {"enter", 128}, {"return", 128}, {"escape", 129}, {"esc", 129}, {"tab", 130}, {"backspace", 131}, {"delete", 132}, {"del", 132}, {"insert", 133},
    {"left", 134}, {"right", 135}, {"up", 136}, {"down", 137}, {"home", 138}, {"end", 139}, {"pageup", 140}, {"pagedown", 141},
    {"shift", 142}, {"ctrl", 143}, {"alt", 144}, {"super", 157}, {"f1", 145}, {"f2", 146}, {"f3", 147}, {"f4", 148}, {"f5", 149}, {"f6", 150}, {"f7", 151}, {"f8", 152}, {"f9", 153}, {"f10", 154}, {"f11", 155}, {"f12", 156},
    {"comma", ','}, {"period", '.'}, {"slash", '/'}, {"minus", '-'}, {"equals", '='}, {"semicolon", ';'}, {"quote", '\''}, {"lbracket", '['}, {"rbracket", ']'}, {"backslash", '\\'}, {"grave", '`'},
};
int key_code(const char *name) {
    if (!name || !*name) return -1;
    if (name[1] == 0) { unsigned char c = (unsigned char)name[0]; if (c > 32 && c < 127) return tolower(c); }
    char low[16]; size_t i = 0; for (; name[i] && i < 15; i++) low[i] = (char)tolower((unsigned char)name[i]); low[i] = 0;
    for (size_t k = 0; k < sizeof KEYS / sizeof KEYS[0]; k++) if (!strcmp(KEYS[k].name, low)) return KEYS[k].code;
    return -1;
}
const char *key_name(int code) {
    static char one[2];
    for (size_t k = 0; k < sizeof KEYS / sizeof KEYS[0]; k++) if (KEYS[k].code == code && strcmp(KEYS[k].name, "return") && strcmp(KEYS[k].name, "esc") && strcmp(KEYS[k].name, "del")) return KEYS[k].name;
    if (code > 32 && code < 127) { one[0] = (char)code; one[1] = 0; return one; }
    return NULL;
}

static uint8_t K_down[KEYN], K_kp[KEYN], K_kr[KEYN], K_rep[KEYN];
static double  K_until[KEYN];
static uint8_t M_down[8], M_p[8], M_r[8];
enum { EV_PRESS = 1, EV_REPEAT = 2, EV_RELEASE = 3 };

static void ev_key(int code, int kind) {
    if (code < 0 || code >= KEYN) return;
    double now = as_clock_s();
    if (kind == EV_RELEASE) { if (K_down[code]) { K_down[code] = 0; K_kr[code] = 1; } return; }
    if (!K_down[code]) { K_down[code] = 1; K_kp[code] = 1; K_rep[code] = 1; if (kind == EV_REPEAT) K_kp[code] = 0; K_until[code] = now + T.hold_first; }
    else { K_rep[code] = 1; K_until[code] = now + T.hold_repeat; }
    if (kind == EV_PRESS || kind == EV_REPEAT) T.last_key = code;
}
static void ev_text(const char *s, size_t n) {
    size_t cur = strlen(T.typed);
    for (size_t i = 0; i < n && cur + 1 < sizeof T.typed; i++) T.typed[cur++] = s[i];
    T.typed[cur] = 0;
}
static void ev_mouse_btn(int b, bool down) {
    if (b < 1 || b > 7) return;
    if (down) { if (!M_down[b]) M_p[b] = 1; M_down[b] = 1; } else { if (M_down[b]) M_r[b] = 1; M_down[b] = 0; }
}
static void ev_mouse_move(int x, int y) {
    x = as_clampi(x, 0, T.w > 0 ? T.w - 1 : 0); y = as_clampi(y, 0, T.h > 0 ? T.h - 1 : 0);
    T.dmx += x - T.mx; T.dmy += y - T.my; T.mx = x; T.my = y;
}
static void clear_all_keys(void) {
    for (int i = 0; i < KEYN; i++) if (K_down[i]) { K_down[i] = 0; K_kr[i] = 1; }
    for (int i = 0; i < 8; i++) if (M_down[i]) { M_down[i] = 0; M_r[i] = 1; }
}

bool term_key(int c) { return c >= 0 && c < KEYN && K_down[c]; }
bool term_key_pressed(int c) { return c >= 0 && c < KEYN && K_kp[c]; }
bool term_key_released(int c) { return c >= 0 && c < KEYN && K_kr[c]; }
bool term_key_repeat(int c) { return c >= 0 && c < KEYN && K_rep[c]; }
bool term_mouse_down(int b) { return b >= 1 && b < 8 && M_down[b]; }
bool term_mouse_pressed(int b) { return b >= 1 && b < 8 && M_p[b]; }
bool term_mouse_released(int b) { return b >= 1 && b < 8 && M_r[b]; }
int  term_mods(void) { return T.mods; }

/* ---- injected events (headless tests, scripted demos) ---- */
typedef struct { int kind; int a, b, c, d, defer; } InjEv;   /* kind: 1 key(a=code,b=press/repeat/release,c=mods) 2 mouse btn(a=btn,b=down) 3 move(a=x,b=y) 4 wheel(a=dy) 5 text */
static InjEv *inj; static int inj_n, inj_cap; static char *inj_text; static size_t inj_text_n, inj_text_cap;
static void inj_push(InjEv e) { if (inj_n >= inj_cap) { inj_cap = inj_cap ? inj_cap * 2 : 32; inj = kc_realloc(inj, (size_t)inj_cap * sizeof *inj); } inj[inj_n++] = e; }
void term_send_key(int code, bool down, int mods) {
    InjEv e = { 1, code, down ? EV_PRESS : EV_RELEASE, mods, 0, 0 }; inj_push(e);
}
void term_send_key_tap(int code, int mods) {
    InjEv e = { 1, code, EV_PRESS, mods, 0, 0 }; inj_push(e);
    InjEv r = { 1, code, EV_RELEASE, mods, 0, 1 }; inj_push(r);
}
void term_send_mouse(int x, int y, int button, int kind) {
    if (kind == 3) { InjEv e = { 4, button, 0, 0, 0, 0 }; inj_push(e); return; }
    InjEv m = { 3, x, y, 0, 0, 0 }; inj_push(m);
    if (kind == 1) { InjEv e = { 2, button, 1, 0, 0, 0 }; inj_push(e); }
    else if (kind == 2) { InjEv e = { 2, button, 0, 0, 0, 0 }; inj_push(e); }
}
void term_send_text(const char *s) {
    size_t n = strlen(s); if (inj_text_n + n + 1 > inj_text_cap) { inj_text_cap = (inj_text_n + n + 1) * 2; inj_text = kc_realloc(inj_text, inj_text_cap); }
    memcpy(inj_text + inj_text_n, s, n); inj_text_n += n; inj_text[inj_text_n] = 0;
}
static void apply_injected(void) {
    int keep = 0;
    for (int i = 0; i < inj_n; i++) {
        InjEv e = inj[i];
        if (e.defer > 0) { e.defer--; inj[keep++] = e; continue; }
        if (e.kind == 1) { T.mods = e.c; ev_key(e.a, e.b); }
        else if (e.kind == 2) ev_mouse_btn(e.a, e.b != 0);
        else if (e.kind == 3) ev_mouse_move(e.a, e.b);
        else if (e.kind == 4) T.wheel_y += e.a;
    }
    inj_n = keep;
    if (inj_text_n) { ev_text(inj_text, inj_text_n); inj_text_n = 0; }
}

/* ============================================================== resizing */
static void screen_alloc(int w, int h) {
    if (w < 1) w = 1; if (h < 1) h = 1;
    if (!S_screen) { S_screen = kc_malloc(sizeof(Canvas)); memset(S_screen, 0, sizeof(Canvas)); S_screen->used = true; }
    if (!S_screen->c) { S_screen->c = kc_malloc((size_t)w * h * sizeof(Cell)); S_screen->w = w; S_screen->h = h; cv_clear(S_screen, ' ', 0, 0, 0); cv_reset_clip(S_screen); }
    else cv_resize(S_screen, w, h);
    if (!S_front) { S_front = kc_malloc(sizeof(Canvas)); memset(S_front, 0, sizeof(Canvas)); }
    S_front->c = kc_realloc(S_front->c, (size_t)w * h * sizeof(Cell)); S_front->w = w; S_front->h = h;
    for (size_t i = 0; i < (size_t)w * h; i++) { S_front->c[i].ch = 0xFFFFFFFEu; S_front->c[i].fg = S_front->c[i].bg = 0xFFFFFFFFu; S_front->c[i].attr = 0xFFFF; }
    need_full = true;
}
void term_resize_virtual(int w, int h) {
    T.hw = as_clampi(w, 1, 1000); T.hh = as_clampi(h, 1, 500);
    if (T.open && T.headless && (T.w != T.hw || T.h != T.hh)) { T.w = T.hw; T.h = T.hh; screen_alloc(T.w, T.h); T.resized = true; }
}
void term_refresh(void) { need_full = true; }

/* ============================================================== POSIX */
#if !defined(_WIN32)
static struct termios orig_tio; static bool have_orig = false;
static volatile sig_atomic_t sig_int = 0, sig_cont = 0, sig_winch = 0;
static char init_seq[400], fini_seq[300];
static size_t init_len, fini_len;
static int flags_saved = -1;

static void build_seqs(void) {
    Buf b; buf_init(&b);
    if (T.alt_screen) buf_adds(&b, "\x1b[?1049h");
    buf_adds(&b, "\x1b[?25l\x1b[?7l\x1b[22;2t");
    if (T.mouse_on) buf_adds(&b, "\x1b[?1003h\x1b[?1006h");
    buf_adds(&b, "\x1b[?1004h\x1b[?2004h");
    if (T.kitty) buf_adds(&b, "\x1b[>31u");
    buf_adds(&b, "\x1b[0m\x1b[2J\x1b[H");
    init_len = b.n < sizeof init_seq ? b.n : sizeof init_seq - 1; memcpy(init_seq, b.d, init_len); buf_free(&b);
    buf_init(&b);
    if (T.kitty) buf_adds(&b, "\x1b[<u");
    buf_adds(&b, "\x1b[?2004l\x1b[?1004l");
    if (T.mouse_on) buf_adds(&b, "\x1b[?1006l\x1b[?1003l");
    buf_adds(&b, "\x1b[0m\x1b[?7h\x1b[?25h\x1b[23;2t");
    if (T.alt_screen) buf_adds(&b, "\x1b[?1049l");
    fini_len = b.n < sizeof fini_seq ? b.n : sizeof fini_seq - 1; memcpy(fini_seq, b.d, fini_len); buf_free(&b);
}
static void tty_raw(void) {
    struct termios t = orig_tio;
    t.c_iflag &= (tcflag_t)~(IXON | ICRNL | INLCR | IGNCR | BRKINT | ISTRIP | INPCK);
    t.c_lflag &= (tcflag_t)~(ICANON | ECHO | ECHONL | IEXTEN);
    if (!T.ctrl_c_quits) t.c_lflag &= (tcflag_t)~ISIG;
    t.c_cc[VMIN] = 0; t.c_cc[VTIME] = 0;
    tcsetattr(0, TCSANOW, &t);
}
static void sig_restore_fast(void) {
    if (!T.raw) return;
    ssize_t r = write(1, fini_seq, fini_len); (void)r;
    if (have_orig) tcsetattr(0, TCSANOW, &orig_tio);
}
static void on_sigint(int s) { (void)s; if (sig_int) { sig_restore_fast(); _exit(130); } sig_int = 1; }
static void on_sigfatal(int s) { sig_restore_fast(); signal(s, SIG_DFL); raise(s); }
static void on_sigtstp(int s) { (void)s; sig_restore_fast(); signal(SIGTSTP, SIG_DFL); raise(SIGTSTP); }
static void on_sigcont(int s) {
    (void)s;
    if (T.raw) { tty_raw(); ssize_t r = write(1, init_seq, init_len); (void)r; sig_cont = 1; }
    signal(SIGTSTP, on_sigtstp);
}
static void on_sigwinch(int s) { (void)s; sig_winch = 1; }
static void install_signals(void) {
    struct sigaction sa; memset(&sa, 0, sizeof sa); sigemptyset(&sa.sa_mask);
    sa.sa_handler = on_sigint; sigaction(SIGINT, &sa, NULL);
    sa.sa_handler = on_sigfatal; sigaction(SIGTERM, &sa, NULL); sigaction(SIGHUP, &sa, NULL); sigaction(SIGQUIT, &sa, NULL);
    sa.sa_handler = on_sigtstp; sigaction(SIGTSTP, &sa, NULL);
    sa.sa_handler = on_sigcont; sigaction(SIGCONT, &sa, NULL);
    sa.sa_handler = on_sigwinch; sigaction(SIGWINCH, &sa, NULL);
}
static void restore_signals(void) {
    struct sigaction sa; memset(&sa, 0, sizeof sa); sigemptyset(&sa.sa_mask); sa.sa_handler = SIG_DFL;
    sigaction(SIGINT, &sa, NULL); sigaction(SIGTERM, &sa, NULL); sigaction(SIGHUP, &sa, NULL); sigaction(SIGQUIT, &sa, NULL);
    sigaction(SIGTSTP, &sa, NULL); sigaction(SIGCONT, &sa, NULL); sigaction(SIGWINCH, &sa, NULL);
}
static bool tty_size(int *w, int *h) {
    struct winsize ws;
    if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) { *w = ws.ws_col; *h = ws.ws_row; return true; }
    return false;
}
static void out_write(const char *d, size_t n) {
    if (!n) return;
    fwrite(d, 1, n, stdout); fflush(stdout); T.bytes_out += n;
}
static size_t in_read(uint8_t *buf, size_t cap) {
    ssize_t r = read(0, buf, cap);
    return r > 0 ? (size_t)r : 0;
}
static bool in_wait(int ms) {
    fd_set fs; FD_ZERO(&fs); FD_SET(0, &fs); struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    return select(1, &fs, NULL, NULL, &tv) > 0;
}
#else
/* ============================================================== Windows */
static HANDLE hIn = INVALID_HANDLE_VALUE, hOut = INVALID_HANDLE_VALUE;
static DWORD mode_in0, mode_out0; static UINT cp0; static bool have_modes = false;
static volatile LONG sig_int = 0; static wchar_t hi_surr = 0;
static BOOL WINAPI on_ctrl(DWORD t) { if (t == CTRL_C_EVENT || t == CTRL_BREAK_EVENT) { if (sig_int) return FALSE; sig_int = 1; return TRUE; } return FALSE; }
static void out_write(const char *d, size_t n) {
    if (!n) return;
    fflush(stdout); T.bytes_out += n;
    size_t off = 0;
    while (off < n) { DWORD w = 0; DWORD chunk = (DWORD)((n - off) > 32768 ? 32768 : (n - off)); if (!WriteFile(hOut, d + off, chunk, &w, NULL) || w == 0) break; off += w; }
}
static bool tty_size(int *w, int *h) {
    CONSOLE_SCREEN_BUFFER_INFO i;
    if (!GetConsoleScreenBufferInfo(hOut, &i)) return false;
    *w = i.srWindow.Right - i.srWindow.Left + 1; *h = i.srWindow.Bottom - i.srWindow.Top + 1; return *w > 0 && *h > 0;
}
static int vk_to_code(WORD vk) {
    if (vk >= 'A' && vk <= 'Z') return vk - 'A' + 'a';
    if (vk >= '0' && vk <= '9') return vk;
    if (vk >= 0x60 && vk <= 0x69) return '0' + (vk - 0x60);
    if (vk >= 0x70 && vk <= 0x7B) return 145 + (vk - 0x70);
    switch (vk) {
        case VK_SPACE: return 32; case VK_RETURN: return 128; case VK_ESCAPE: return 129; case VK_TAB: return 130; case VK_BACK: return 131; case VK_DELETE: return 132; case VK_INSERT: return 133;
        case VK_LEFT: return 134; case VK_RIGHT: return 135; case VK_UP: return 136; case VK_DOWN: return 137; case VK_HOME: return 138; case VK_END: return 139; case VK_PRIOR: return 140; case VK_NEXT: return 141;
        case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: return 142; case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: return 143; case VK_MENU: case VK_LMENU: case VK_RMENU: return 144;
        case VK_LWIN: case VK_RWIN: return 157;
        case VK_OEM_COMMA: return ','; case VK_OEM_PERIOD: return '.'; case VK_OEM_2: return '/'; case VK_OEM_MINUS: return '-'; case VK_OEM_PLUS: return '='; case VK_OEM_1: return ';';
        case VK_OEM_7: return '\''; case VK_OEM_4: return '['; case VK_OEM_6: return ']'; case VK_OEM_5: return '\\'; case VK_OEM_3: return '`';
        case VK_ADD: return '='; case VK_SUBTRACT: return '-'; case VK_DECIMAL: return '.'; case VK_DIVIDE: return '/';
    }
    return -1;
}
#endif

/* ============================================================ input parser */
#if !defined(_WIN32)
static uint8_t inbuf[8192]; static size_t inlen = 0; static double esc_t = 0; static bool in_paste = false;

static int csi_num(const char *s, int idx, int sub, int def) {          /* idx-th ';' param, sub-th ':' value */
    int i = 0; const char *p = s;
    while (i < idx) { const char *q = strchr(p, ';'); if (!q) return def; p = q + 1; i++; }
    int j = 0; while (j < sub) { const char *q = p; while (*q && *q != ';' && *q != ':') q++; if (*q != ':') return def; p = q + 1; j++; }
    if (*p < '0' || *p > '9') return def;
    return atoi(p);
}
static int mods_from(int m) { return m > 0 ? m - 1 : 0; }
static void key_event_mods(int code, int kind, int mods) {
    T.mods = mods;
    if (code < 0) return;
    ev_key(code, kind);
}
static int kitty_code(int cp) {
    switch (cp) {
        case 27: return 129; case 13: return 128; case 9: return 130; case 127: return 131; case 57414: return 128;
        case 57441: case 57447: return 142; case 57442: case 57448: return 143; case 57443: case 57449: return 144; case 57444: case 57450: return 157;
        case 57409: return '.'; case 57410: return '/'; case 57411: return '*'; case 57412: return '-'; case 57413: return '='; case 57415: return '=';
    }
    if (cp >= 57399 && cp <= 57408) return '0' + (cp - 57399);
    if (cp == 32) return 32;
    if (cp > 32 && cp < 127) return tolower(cp);
    return -1;
}
static void handle_csi(const char *pre, const char *par, char fin) {
    int kind = EV_PRESS;
    int ev = csi_num(par, 1, 1, 1); if (ev == 2) kind = EV_REPEAT; else if (ev == 3) kind = EV_RELEASE;
    int mods = mods_from(csi_num(par, 1, 0, 1));
    if (pre[0] == '<' && (fin == 'M' || fin == 'm')) {                               /* SGR mouse */
        int b = csi_num(par, 0, 0, 0), x = csi_num(par, 1, 0, 1) - 1, y = csi_num(par, 2, 0, 1) - 1;
        int m = ((b & 4) ? 1 : 0) | ((b & 8) ? 2 : 0) | ((b & 16) ? 4 : 0); T.mods = m;
        if (b & 64) { int w = b & 3; if (w == 0) T.wheel_y++; else if (w == 1) T.wheel_y--; else if (w == 2) T.wheel_x--; else T.wheel_x++; ev_mouse_move(x, y); return; }
        ev_mouse_move(x, y);
        if (b & 32) return;                                                         /* motion */
        int btn = (b & 3) + 1; if (btn > 3) return;
        ev_mouse_btn(btn, fin == 'M');
        return;
    }
    if (pre[0] == '?') return;                                                      /* kitty query / DA replies */
    switch (fin) {
        case 'A': key_event_mods(136, kind, mods); return; case 'B': key_event_mods(137, kind, mods); return;
        case 'C': key_event_mods(135, kind, mods); return; case 'D': key_event_mods(134, kind, mods); return;
        case 'H': key_event_mods(138, kind, mods); return; case 'F': key_event_mods(139, kind, mods); return;
        case 'E': return;
        case 'Z': key_event_mods(130, kind, mods | 1); return;
        case 'P': key_event_mods(145, kind, mods); return; case 'Q': key_event_mods(146, kind, mods); return;
        case 'R': if (csi_num(par, 0, 0, 1) == 1) key_event_mods(147, kind, mods); return; case 'S': key_event_mods(148, kind, mods); return;
        case 'I': T.focused = true; return;
        case 'O': T.focused = false; clear_all_keys(); return;
        case '~': {
            int n = csi_num(par, 0, 0, 0); int code = -1;
            switch (n) {
                case 1: case 7: code = 138; break; case 2: code = 133; break; case 3: code = 132; break; case 4: case 8: code = 139; break; case 5: code = 140; break; case 6: code = 141; break;
                case 11: case 12: case 13: case 14: case 15: code = 145 + (n - 11); break;
                case 17: case 18: case 19: case 20: case 21: code = 150 + (n - 17); break;
                case 23: code = 155; break; case 24: code = 156; break;
                case 200: in_paste = true; return; case 201: in_paste = false; return;
            }
            key_event_mods(code, kind, mods); return;
        }
        case 'u': {
            int cp = csi_num(par, 0, 0, 0), shifted = csi_num(par, 0, 1, 0);
            int code = kitty_code(cp);
            if (!T.kitty_supported) T.kitty_supported = true;
            key_event_mods(code, kind, mods);
            if (kind != EV_RELEASE && !(mods & (2 | 4 | 8 | 32))) {                  /* alt/ctrl/super combos are not text */
                const char *tp = par; int idx = 0; while (idx < 2 && (tp = strchr(tp, ';'))) { tp++; idx++; }
                char u[8];
                if (idx == 2 && tp && *tp) { const char *q = tp; while (*q) { int c2 = atoi(q); int l = utf8_put(u, (uint32_t)c2); ev_text(u, (size_t)l); while (*q && *q != ':') q++; if (*q == ':') q++; } }
                else if (cp >= 32 && cp != 127 && !(cp >= 57344 && cp <= 63743)) { int c2 = (mods & 1) && shifted ? shifted : (mods & 1) && cp < 128 && isalpha(cp) ? toupper(cp) : cp; int l = utf8_put(u, (uint32_t)c2); ev_text(u, (size_t)l); }
            }
            return;
        }
    }
}
static void process_input(bool flush) {
    size_t i = 0;
    while (i < inlen) {
        if (in_paste) {                                                              /* bracketed paste: text only */
            const uint8_t *e = NULL; for (size_t k = i; k + 6 <= inlen; k++) if (inbuf[k] == 0x1b && !memcmp(inbuf + k, "\x1b[201~", 6)) { e = inbuf + k; break; }
            size_t stop = e ? (size_t)(e - inbuf) : inlen;
            if (!e && !flush && inlen - i >= 6) stop = inlen - 5;
            if (stop > i) ev_text((const char *)inbuf + i, stop - i);
            if (e) { in_paste = false; i = stop + 6; } else { i = stop; if (flush) i = inlen; break; }
            continue;
        }
        uint8_t b = inbuf[i];
        if (b == 0x1b) {
            if (i + 1 >= inlen) { if (flush) { ev_key(129, EV_PRESS); ev_key(129, EV_RELEASE); i++; continue; } break; }
            uint8_t c = inbuf[i + 1];
            if (c == '[') {
                size_t j = i + 2; char pre[4] = { 0 }, par[64]; size_t pn = 0, pr = 0;
                while (j < inlen && inbuf[j] >= 0x3C && inbuf[j] <= 0x3F && pr < 2) pre[pr++] = (char)inbuf[j++];
                while (j < inlen && ((inbuf[j] >= '0' && inbuf[j] <= '9') || inbuf[j] == ';' || inbuf[j] == ':')) { if (pn + 1 < sizeof par) par[pn++] = (char)inbuf[j]; j++; }
                par[pn] = 0;
                while (j < inlen && inbuf[j] >= 0x20 && inbuf[j] <= 0x2F) j++;
                if (j >= inlen) { if (flush || inlen - i > 80) { i = inlen; break; } break; }
                if (inbuf[j] < 0x40 || inbuf[j] > 0x7E) { i = j + 1; continue; }
                handle_csi(pre, par, (char)inbuf[j]); i = j + 1; continue;
            }
            if (c == 'O') {
                if (i + 2 >= inlen) { if (flush) { i = inlen; break; } break; }
                int code = -1; switch (inbuf[i + 2]) { case 'A': code = 136; break; case 'B': code = 137; break; case 'C': code = 135; break; case 'D': code = 134; break; case 'H': code = 138; break; case 'F': code = 139; break;
                                                      case 'P': code = 145; break; case 'Q': code = 146; break; case 'R': code = 147; break; case 'S': code = 148; break; }
                if (code >= 0) { T.mods = 0; ev_key(code, EV_PRESS); }
                i += 3; continue;
            }
            if (c == 0x1b) { ev_key(129, EV_PRESS); i++; continue; }
            /* Alt + key */
            if (c >= 0x20 && c < 0x7f) { T.mods = 2; int code = tolower(c); ev_key(code, EV_PRESS); i += 2; continue; }
            ev_key(129, EV_PRESS); i++; continue;
        }
        if (b == '\r' || b == '\n') { T.mods = 0; ev_key(128, EV_PRESS); i++; continue; }
        if (b == '\t') { T.mods = 0; ev_key(130, EV_PRESS); i++; continue; }
        if (b == 0x7f || b == 0x08) { T.mods = 0; ev_key(131, EV_PRESS); i++; continue; }
        if (b == 0) { T.mods = 4; ev_key(32, EV_PRESS); i++; continue; }
        if (b < 0x20) { T.mods = 4; ev_key('a' + b - 1, EV_PRESS); i++; continue; }
        if (b < 0x80) {
            char ch = (char)b; ev_text(&ch, 1);
            T.mods = (b >= 'A' && b <= 'Z') ? 1 : 0; ev_key(tolower(b), EV_PRESS); i++; continue;
        }
        int len = b >= 0xF0 ? 4 : b >= 0xE0 ? 3 : b >= 0xC0 ? 2 : 1;
        if (i + (size_t)len > inlen) { if (flush) { i = inlen; break; } break; }
        ev_text((const char *)inbuf + i, (size_t)len); i += (size_t)len;
    }
    if (i > 0) { memmove(inbuf, inbuf + i, inlen - i); inlen -= i; }
}
static void read_input(void) {
    uint8_t tmp[2048]; size_t got;
    while ((got = in_read(tmp, sizeof tmp)) > 0) {
        if (inlen + got > sizeof inbuf) { inlen = 0; }                              /* flood: drop rather than overflow */
        memcpy(inbuf + inlen, tmp, got); inlen += got;
    }
    if (!inlen) return;
    double now = as_clock_s();
    bool has_lone_esc = (inbuf[inlen - 1] == 0x1b) || (inlen >= 2 && inbuf[inlen - 2] == 0x1b && (inbuf[inlen - 1] == '[' || inbuf[inlen - 1] == 'O'));
    if (has_lone_esc && esc_t == 0) esc_t = now;
    bool flush = has_lone_esc && now - esc_t > 0.04;
    process_input(flush);
    if (!inlen || !has_lone_esc) esc_t = 0;
    if (flush) esc_t = 0;
}
#else
static void read_input(void) {
    DWORD n = 0; INPUT_RECORD rec[64];
    while (GetNumberOfConsoleInputEvents(hIn, &n) && n > 0) {
        DWORD got = 0; if (!ReadConsoleInputW(hIn, rec, 64, &got) || got == 0) break;
        for (DWORD i = 0; i < got; i++) {
            INPUT_RECORD *r = &rec[i];
            if (r->EventType == KEY_EVENT) {
                KEY_EVENT_RECORD *k = &r->Event.KeyEvent;
                int code = vk_to_code(k->wVirtualKeyCode); DWORD cs = k->dwControlKeyState;
                T.mods = ((cs & SHIFT_PRESSED) ? 1 : 0) | ((cs & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) ? 2 : 0) | ((cs & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) ? 4 : 0);
                if (code >= 0) { if (k->bKeyDown) ev_key(code, K_down[code] ? EV_REPEAT : EV_PRESS); else ev_key(code, EV_RELEASE); }
                if (k->bKeyDown) {
                    wchar_t wc = k->uChar.UnicodeChar;
                    if (wc >= 0xD800 && wc < 0xDC00) hi_surr = wc;
                    else if (wc >= 32 && wc != 127 && (!(T.mods & 4) || (cs & RIGHT_ALT_PRESSED))) {
                        uint32_t cp = wc; if (wc >= 0xDC00 && wc < 0xE000 && hi_surr) { cp = 0x10000 + (((uint32_t)hi_surr - 0xD800) << 10) + ((uint32_t)wc - 0xDC00); hi_surr = 0; }
                        char u[4]; int l = utf8_put(u, cp); ev_text(u, (size_t)l);
                    }
                }
            } else if (r->EventType == MOUSE_EVENT) {
                MOUSE_EVENT_RECORD *m = &r->Event.MouseEvent; CONSOLE_SCREEN_BUFFER_INFO bi; int ox = 0, oy = 0;
                if (GetConsoleScreenBufferInfo(hOut, &bi)) { ox = bi.srWindow.Left; oy = bi.srWindow.Top; }
                ev_mouse_move(m->dwMousePosition.X - ox, m->dwMousePosition.Y - oy);
                if (m->dwEventFlags & MOUSE_WHEELED) { short d = (short)HIWORD(m->dwButtonState); T.wheel_y += d > 0 ? 1 : -1; }
                else if (m->dwEventFlags & MOUSE_HWHEELED) { short d = (short)HIWORD(m->dwButtonState); T.wheel_x += d > 0 ? 1 : -1; }
                else { ev_mouse_btn(1, (m->dwButtonState & FROM_LEFT_1ST_BUTTON_PRESSED) != 0); ev_mouse_btn(3, (m->dwButtonState & RIGHTMOST_BUTTON_PRESSED) != 0); ev_mouse_btn(2, (m->dwButtonState & FROM_LEFT_2ND_BUTTON_PRESSED) != 0); }
            } else if (r->EventType == FOCUS_EVENT) { if (!r->Event.FocusEvent.bSetFocus) { T.focused = false; clear_all_keys(); } else T.focused = true; }
        }
    }
}
#endif

/* ========================================================= open / close */
static void restore_terminal(void) {
    if (!T.open || T.headless || !T.raw) return;
#if defined(_WIN32)
    Buf b; buf_init(&b);
    buf_adds(&b, "\x1b[?1004l\x1b[?7h\x1b[0m\x1b[?25h\x1b[23;2t"); if (T.alt_screen) buf_adds(&b, "\x1b[?1049l");
    out_write(b.d, b.n); buf_free(&b);
    if (have_modes) { SetConsoleMode(hIn, mode_in0); SetConsoleMode(hOut, mode_out0); SetConsoleOutputCP(cp0); }
    SetConsoleCtrlHandler(on_ctrl, FALSE);
#else
    out_write(fini_seq, fini_len);
    if (have_orig) tcsetattr(0, TCSANOW, &orig_tio);
    if (flags_saved >= 0) fcntl(0, F_SETFL, flags_saved);
    restore_signals();
#endif
    T.raw = false;
}
void term_restore_for_error(void) {
    if (T.open && !T.headless && T.raw) { restore_terminal(); T.open = false; }
}
static void at_exit_restore(void) { if (T.open && !T.headless && T.raw) restore_terminal(); }

void term_close(void) {
    if (!T.open) return;
    restore_terminal();
    T.open = false; T.cur_show = false;
    clear_all_keys();
    for (int i = 0; i < KEYN; i++) K_kr[i] = 0; for (int i = 0; i < 8; i++) M_r[i] = 0;
}

static int detect_colors(void) {
    const char *nc = getenv("NO_COLOR"); if (nc && *nc) return 0;
    const char *ct = getenv("COLORTERM");
#if !defined(_WIN32)
    const char *tm = getenv("TERM");
#endif
    if (ct && (strstr(ct, "truecolor") || strstr(ct, "24bit"))) return 24;
#if defined(_WIN32)
    return 24;
#else
    if (tm && (strstr(tm, "truecolor") || strstr(tm, "direct") || !strcmp(tm, "xterm-kitty") || !strcmp(tm, "alacritty") || !strcmp(tm, "wezterm"))) return 24;
    if (getenv("WT_SESSION") || getenv("KITTY_WINDOW_ID") || getenv("ITERM_SESSION_ID")) return 24;
    if (tm && strstr(tm, "256")) return 256;
    if (tm && (!strcmp(tm, "dumb"))) return 0;
    if (tm && (!strcmp(tm, "linux") || !strcmp(tm, "vt100") || !strcmp(tm, "ansi") || !strncmp(tm, "xterm", 5) || !strncmp(tm, "screen", 6) || !strncmp(tm, "tmux", 4))) return tm[0] == 'x' || tm[0] == 's' || tm[0] == 't' ? 256 : 16;
    return 16;
#endif
}
static int parse_color_opt(const char *s, int def) {
    if (!s || !strcmp(s, "auto")) return def;
    if (!strcmp(s, "none") || !strcmp(s, "mono") || !strcmp(s, "off")) return 0;
    if (!strcmp(s, "16") || !strcmp(s, "ansi16")) return 16;
    if (!strcmp(s, "256") || !strcmp(s, "ansi256")) return 256;
    if (!strcmp(s, "truecolor") || !strcmp(s, "24bit") || !strcmp(s, "rgb")) return 24;
    as_err("open: unknown color mode \"%s\" (auto, none, ansi16, ansi256, truecolor)", s);
}

bool term_open(KcInterp *ip, Opts *o) {
    (void)ip;
    if (T.open) return true;
    bool want_headless = opt_bool(o, "headless", false);
    const char *env = getenv("KC_ASCII_HEADLESS"); if (env && *env && *env != '0') want_headless = true;
    bool force = opt_bool(o, "force", false); bool requested_headless = want_headless;
    T.target_fps = opt_num(o, "fps", T.target_fps > 0 ? T.target_fps : 30);
    T.hold_first = opt_num(o, "hold_first", T.hold_first > 0 ? T.hold_first : 0.40);
    T.hold_repeat = opt_num(o, "hold_repeat", T.hold_repeat > 0 ? T.hold_repeat : 0.12);
    T.cell_aspect = opt_num(o, "cell_aspect", T.cell_aspect > 0 ? T.cell_aspect : 2.0);
    T.ctrl_c_quits = !(opt_has(o, "ctrl_c") && strcmp(opt_str(o, "ctrl_c", "quit"), "quit") != 0);
    T.alt_screen = opt_bool(o, "alt_screen", true);
    T.mouse_on = opt_bool(o, "mouse", true);
    T.sigint_quit = false; T.quit = false; T.focused = true;
    if (!T.hw) { T.hw = 80; T.hh = 24; }
    if (opt_has(o, "width")) T.hw = as_clampi(opt_int(o, "width", 80), 1, 1000);
    if (opt_has(o, "height")) T.hh = as_clampi(opt_int(o, "height", 24), 1, 500);
    memset(K_down, 0, sizeof K_down); memset(K_kp, 0, sizeof K_kp); memset(K_kr, 0, sizeof K_kr); memset(K_rep, 0, sizeof K_rep);
    memset(M_down, 0, sizeof M_down); memset(M_p, 0, sizeof M_p); memset(M_r, 0, sizeof M_r);
    T.mx = T.my = T.dmx = T.dmy = 0; T.wheel_y = T.wheel_x = 0; T.mods = 0; T.typed[0] = 0; T.last_key = -1; T.frame = 0; T.time = 0; T.fps = 0; T.dt = 1.0 / T.target_fps;
    const char *cm = opt_str(o, "color", "auto");

#if defined(_WIN32)
    hIn = GetStdHandle(STD_INPUT_HANDLE); hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD m1, m2; bool tty = GetConsoleMode(hIn, &m1) && GetConsoleMode(hOut, &m2);
    if (!tty && !force) want_headless = true;
#else
    bool tty = isatty(0) && isatty(1);
    const char *tm = getenv("TERM"); if (tm && !strcmp(tm, "dumb") && !force) tty = false;
    if (!tty && !force) want_headless = true;
#endif
    if (want_headless || !tty) {
        if (!requested_headless && !force && !getenv("KC_ASCII_QUIET"))
            as_warn("no interactive terminal detected (stdin/stdout is not a console), so nothing will be drawn and no keys can be read - running headless.\n"
                    "  Run from a real console: Windows Terminal, PowerShell or cmd.exe (not Git Bash/mintty, an IDE output pane or a pipe; in Git Bash prefix the command with 'winpty'),\n"
                    "  or pass \"force\", true to open() to use the terminal anyway. Set KC_ASCII_QUIET=1 to silence this note.");
        T.headless = true; T.backend = "headless"; T.keymode = "virtual"; T.raw = false;
        T.colors = parse_color_opt(cm, 24); T.w = T.hw; T.h = T.hh;
        T.open = true; screen_alloc(T.w, T.h); T.last_real = as_clock_s();
        T.kitty = false;
        return true;
    }
    T.headless = false;
    T.colors = parse_color_opt(cm, detect_colors());
#if defined(_WIN32)
    T.backend = "windows"; T.keymode = "windows";
    if (!have_modes) { GetConsoleMode(hIn, &mode_in0); GetConsoleMode(hOut, &mode_out0); cp0 = GetConsoleOutputCP(); have_modes = true; }
    DWORD mo = mode_out0 | 0x0004 /*ENABLE_VIRTUAL_TERMINAL_PROCESSING*/ | 0x0008 /*DISABLE_NEWLINE_AUTO_RETURN*/ | ENABLE_PROCESSED_OUTPUT;
    if (!SetConsoleMode(hOut, mo)) {
        if (!force) { T.headless = true; T.backend = "headless"; T.keymode = "virtual"; T.colors = parse_color_opt(cm, 0); T.w = T.hw; T.h = T.hh; T.open = true; screen_alloc(T.w, T.h); T.last_real = as_clock_s(); as_warn("this console has no ANSI/VT support; running headless (use Windows Terminal or Windows 10+ conhost)"); return true; }
    }
    DWORD mi = ENABLE_EXTENDED_FLAGS | ENABLE_WINDOW_INPUT | (T.mouse_on ? ENABLE_MOUSE_INPUT : 0) | (T.ctrl_c_quits ? ENABLE_PROCESSED_INPUT : 0);
    SetConsoleMode(hIn, mi); SetConsoleOutputCP(CP_UTF8); SetConsoleCtrlHandler(on_ctrl, TRUE);
    T.kitty = false; T.kitty_supported = false;
    { Buf b; buf_init(&b); if (T.alt_screen) buf_adds(&b, "\x1b[?1049h"); buf_adds(&b, "\x1b[?25l\x1b[?7l\x1b[22;2t\x1b[?1004h\x1b[0m\x1b[2J\x1b[H"); out_write(b.d, b.n); buf_free(&b); }
    FlushConsoleInputBuffer(hIn);
#else
    T.backend = "posix";
    if (tcgetattr(0, &orig_tio) == 0) have_orig = true;
    flags_saved = fcntl(0, F_GETFL);
    T.kitty = false; T.kitty_supported = false;
    const char *kopt = opt_str(o, "kitty", "auto");
    if (opt_has(o, "kitty") && opt_find(o, "kitty")->type != VAL_STRING) kopt = opt_bool(o, "kitty", true) ? "on" : "off";
    tty_raw(); T.raw = true; inlen = 0; esc_t = 0; in_paste = false;
    if (strcmp(kopt, "off") != 0) {                                                   /* probe the kitty keyboard protocol: reply to ESC[?u arrives before the DA1 reply */
        out_write("\x1b[?u\x1b[c", 7);
        double t0 = as_clock_s(); bool got_da = false;
        while (as_clock_s() - t0 < 0.20 && !got_da) {
            if (in_wait(30)) { uint8_t tmp[512]; size_t g = in_read(tmp, sizeof tmp); if (g && inlen + g < sizeof inbuf) { memcpy(inbuf + inlen, tmp, g); inlen += g; } }
            for (size_t i = 0; i + 2 < inlen; i++) if (inbuf[i] == 0x1b && inbuf[i + 1] == '[' && inbuf[i + 2] == '?') {
                size_t j = i + 3; while (j < inlen && ((inbuf[j] >= '0' && inbuf[j] <= '9') || inbuf[j] == ';')) j++;
                if (j < inlen && inbuf[j] == 'u') T.kitty_supported = true; else if (j < inlen && inbuf[j] == 'c') got_da = true;
            }
        }
        if (T.kitty_supported || !strcmp(kopt, "on")) T.kitty = true;
    }
    T.keymode = T.kitty ? "kitty" : "timed";
    build_seqs(); out_write(init_seq, init_len);
    install_signals();
    process_input(false);
#endif
    T.raw = true;
    int w = 80, h = 24; if (!tty_size(&w, &h)) { w = T.hw; h = T.hh; }
    T.w = w; T.h = h; T.open = true; screen_alloc(w, h);
    T.cursor_visible = false; T.last_real = as_clock_s();
    if (!atexit_set) { atexit(at_exit_restore); atexit_set = true; }
    if (opt_has(o, "title")) term_set_title(opt_str(o, "title", ""));
    return true;
}
void term_ensure(void) {
    if (T.open) return;
    Opts none = opts_make(NULL, 0, 0);
    term_open(AC.ip, &none);
}
void term_set_title(const char *s) {
    if (!T.open || T.headless) return;
    Buf b; buf_init(&b); buf_adds(&b, "\x1b]2;");
    for (const char *p = s; *p && b.n < 200; p++) if ((unsigned char)*p >= 32) buf_addc(&b, *p);
    buf_adds(&b, "\x07"); out_write(b.d, b.n); buf_free(&b);
}
void term_bell(void) { if (T.open && !T.headless) out_write("\x07", 1); }

/* ================================================================ poll */
static bool check_resize(void) {
    if (T.headless) return false;
    int w, h;
#if !defined(_WIN32)
    if (!sig_winch && T.frame % 30 != 0) return false;
    sig_winch = 0;
#endif
    if (!tty_size(&w, &h)) return false;
    if (w == T.w && h == T.h) return false;
    T.w = w; T.h = h; screen_alloc(w, h);
    out_write("\x1b[2J", 4); T.resized = true; return true;
}
void term_advance(double dt) { T.dt_override = dt; }

bool term_poll(void) {
    term_ensure();
    memset(K_kp, 0, sizeof K_kp); memset(K_kr, 0, sizeof K_kr); memset(K_rep, 0, sizeof K_rep); memset(M_p, 0, sizeof M_p); memset(M_r, 0, sizeof M_r);
    T.dmx = T.dmy = 0; T.wheel_y = T.wheel_x = 0; T.typed[0] = 0; T.resized = false;
    if (!T.headless) {
        read_input();
#if !defined(_WIN32)
        if (sig_cont) { sig_cont = 0; need_full = true; }
        if (sig_int) { sig_int = 0; T.quit = true; }
#else
        if (sig_int) { sig_int = 0; T.quit = true; }
#endif
        check_resize();
        if (!T.kitty && !T.headless && !(T.backend && !strcmp(T.backend, "windows"))) {     /* timed hold fallback: release keys whose repeat window has lapsed */
            double now = as_clock_s();
            for (int i = 0; i < KEYN; i++) if (K_down[i] && now >= K_until[i]) { K_down[i] = 0; K_kr[i] = 1; }
        }
    } else {
        if (T.w != T.hw || T.h != T.hh) { T.w = T.hw; T.h = T.hh; screen_alloc(T.w, T.h); T.resized = true; }
    }
    apply_injected();
    if (T.ctrl_c_quits == false) { /* ctrl+c arrives as a key */ }
    else if (K_kp['c'] && (T.mods & 4)) T.quit = true;
    double now = as_clock_s(), dt;
    if (T.dt_override > 0) { dt = T.dt_override; T.dt_override = 0; }
    else if (T.headless) dt = 1.0 / (T.target_fps > 0 ? T.target_fps : 30);
    else { dt = now - T.last_real; if (dt > 0.25) dt = 0.25; if (dt < 1e-4) dt = 1e-4; }
    T.last_real = now; T.dt = dt; T.time += dt; T.frame++;
    T.fps = T.fps <= 0 ? 1.0 / dt : T.fps * 0.9 + (1.0 / dt) * 0.1;
    if (T.headless) { double sum = 0; (void)sum; }
    return !T.quit;
}

/* ============================================================== present */
static bool cell_eq(const Cell *a, const Cell *b) { return a->ch == b->ch && a->fg == b->fg && a->bg == b->bg && a->attr == b->attr; }
void term_present(void) {
    if (!T.open) return;
    T.cells_changed = 0;
    if (T.headless) { T.cells_changed = S_screen->w * S_screen->h; return; }
    int W = S_screen->w, H = S_screen->h;
    int cmode = T.colors == 0 ? 0 : T.colors == 16 ? 1 : T.colors == 256 ? 2 : 3;
    Buf b; buf_init(&b);
    buf_adds(&b, "\x1b[?2026h");
    Col cf = 0, cb = 0; uint16_t ca = 0; bool style_known = false; int cx = -1, cy = -1;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            Cell *n = &S_screen->c[(size_t)y * W + x], *o = &S_front->c[(size_t)y * W + x];
            if (n->ch == CH_CONT) { if (!need_full && cell_eq(n, o)) continue; if (x > 0 && cell_eq(&S_screen->c[(size_t)y * W + x - 1], &S_front->c[(size_t)y * W + x - 1])) { /* head unchanged: tail changed means head changed too */ } continue; }
            if (cell_eq(n, o)) continue;
            T.cells_changed++;
            if (cy != y || cx != x) {
                if (cy == y && x > cx && x - cx < 8) buf_printf(&b, "\x1b[%dC", x - cx);
                else buf_printf(&b, "\x1b[%d;%dH", y + 1, x + 1);
                cx = x; cy = y;
            }
            if (!style_known) { buf_adds(&b, "\x1b[0m"); cf = cb = 0; ca = 0; style_known = true; }
            Col fg = n->fg, bg = n->bg; uint16_t at = n->attr;
            if (cmode == 0) { fg = bg = 0; at &= (uint16_t)(AT_BOLD | AT_DIM | AT_ITALIC | AT_UNDER | AT_REVERSE | AT_STRIKE); }
            sgr_apply(&b, cmode ? cmode : 1, fg, bg, at, &cf, &cb, &ca);
            char u[4]; int l = utf8_put(u, n->ch ? n->ch : ' '); buf_add(&b, u, (size_t)l);
            int w = cp_width(n->ch); if (w < 1) w = 1; cx += w;
            if (cx >= W) { cx = -1; cy = -1; }
        }
    }
    if (style_known) buf_adds(&b, "\x1b[0m");
    if (T.cur_show) buf_printf(&b, "\x1b[%d;%dH\x1b[?25h", as_clampi(T.cur_y, 0, H - 1) + 1, as_clampi(T.cur_x, 0, W - 1) + 1), T.cursor_visible = true;
    else if (T.cursor_visible) { buf_adds(&b, "\x1b[?25l"); T.cursor_visible = false; }
    buf_adds(&b, "\x1b[?2026l");
    if (T.cells_changed || need_full || T.cur_show) out_write(b.d, b.n);
    buf_free(&b);
    memcpy(S_front->c, S_screen->c, (size_t)W * H * sizeof(Cell));
    need_full = false;
}
void term_frame_wait(void) {
    if (T.headless || T.target_fps <= 0) return;
    double want = 1.0 / T.target_fps, spent = as_clock_s() - T.last_real;
    if (spent < want) as_sleep_ms((int)((want - spent) * 1000.0));
}
