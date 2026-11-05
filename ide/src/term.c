#include "term.h"
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

int term_w = 80, term_h = 24;
bool term_truecolor = false;

static Cell *g_cur = NULL, *g_prev = NULL;
static int g_cw = 0, g_ch = 0;
static int g_curx = 0, g_cury = 0; static bool g_curvis = false;
static bool g_force = true;
static Str g_extra;                  /* escape output queued before the next flush (OSC 52, title) */
static bool g_open = false;
static volatile sig_atomic_t g_winch = 0;

static unsigned char g_in[16384];
static int g_inlen = 0, g_inpos = 0;

/* ---------------------------------------------------------------- platform */
#ifdef _WIN32
static HANDLE g_hin, g_hout; static DWORD g_oldin, g_oldout; static UINT g_oldcp;
static void raw_write(const char *d, size_t n) { DWORD w; while (n) { if (!WriteFile(g_hout, d, (DWORD)(n > 32768 ? 32768 : n), &w, NULL) || !w) return; d += w; n -= w; } }
static void query_size(void) {
    CONSOLE_SCREEN_BUFFER_INFO i;
    if (GetConsoleScreenBufferInfo(g_hout, &i)) { term_w = i.srWindow.Right - i.srWindow.Left + 1; term_h = i.srWindow.Bottom - i.srWindow.Top + 1; }
}
static bool raw_enter(void) {
    g_hin = GetStdHandle(STD_INPUT_HANDLE); g_hout = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!GetConsoleMode(g_hin, &g_oldin) || !GetConsoleMode(g_hout, &g_oldout)) return false;
    g_oldcp = GetConsoleOutputCP(); SetConsoleOutputCP(65001);
    DWORD in = (g_oldin & ~(DWORD)(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT | ENABLE_QUICK_EDIT_MODE)) | ENABLE_VIRTUAL_TERMINAL_INPUT | ENABLE_EXTENDED_FLAGS | ENABLE_WINDOW_INPUT;
    if (!SetConsoleMode(g_hin, in)) return false;
    if (!SetConsoleMode(g_hout, g_oldout | ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN)) return false;
    query_size();
    return true;
}
static void raw_leave(void) { SetConsoleMode(g_hin, g_oldin); SetConsoleMode(g_hout, g_oldout); SetConsoleOutputCP(g_oldcp); }
/* Drains console events into g_in: with VT input every key event carries one char of the VT stream. */
static void fill_input(int timeout_ms) {
    if (g_inpos >= g_inlen) g_inpos = g_inlen = 0;
    if (WaitForSingleObject(g_hin, timeout_ms < 0 ? INFINITE : (DWORD)timeout_ms) != WAIT_OBJECT_0) return;
    DWORD n = 0; GetNumberOfConsoleInputEvents(g_hin, &n);
    static wchar_t hi = 0;
    while (n--) {
        INPUT_RECORD r; DWORD got;
        if (!ReadConsoleInputW(g_hin, &r, 1, &got) || !got) break;
        if (r.EventType == WINDOW_BUFFER_SIZE_EVENT) { g_winch = 1; continue; }
        if (r.EventType != KEY_EVENT || !r.Event.KeyEvent.bKeyDown) continue;
        wchar_t w = r.Event.KeyEvent.uChar.UnicodeChar; if (!w) continue;
        uint32_t cp = w;
        if (w >= 0xD800 && w < 0xDC00) { hi = w; continue; }
        if (w >= 0xDC00 && w < 0xE000 && hi) { cp = 0x10000 + (((uint32_t)hi - 0xD800) << 10) + ((uint32_t)w - 0xDC00); hi = 0; }
        if (g_inlen + 4 < (int)sizeof g_in) g_inlen += utf8_encode(cp, (char *)g_in + g_inlen);
    }
}
#else
static struct termios g_old;
static void raw_write(const char *d, size_t n) {
    while (n) { ssize_t w = write(STDOUT_FILENO, d, n); if (w < 0) { if (errno == EINTR || errno == EAGAIN) continue; return; } d += w; n -= (size_t)w; }
}
static void query_size(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) { term_w = ws.ws_col; term_h = ws.ws_row; }
}
static void on_winch(int sig) { (void)sig; g_winch = 1; }
static void emergency_restore(int sig) {
    static const char seq[] = "\x1b[?1006l\x1b[?1002l\x1b[?1000l\x1b[?2004l\x1b[0m\x1b[?25h\x1b[?1049l";
    if (write(STDOUT_FILENO, seq, sizeof seq - 1) < 0) {}
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_old);
    signal(sig, SIG_DFL); raise(sig);
}
static bool raw_enter(void) {
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return false;
    if (tcgetattr(STDIN_FILENO, &g_old) != 0) return false;
    struct termios t = g_old;
    t.c_iflag &= (tcflag_t) ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    t.c_oflag &= (tcflag_t) ~OPOST;
    t.c_cflag |= CS8;
    t.c_lflag &= (tcflag_t) ~(ECHO | ICANON | IEXTEN | ISIG);
    t.c_cc[VMIN] = 0; t.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &t) != 0) return false;
    query_size();
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = on_winch; sigaction(SIGWINCH, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    int sigs[] = { SIGTERM, SIGHUP, SIGINT, SIGSEGV, SIGABRT };
    for (size_t i = 0; i < sizeof sigs / sizeof *sigs; i++) { memset(&sa, 0, sizeof sa); sa.sa_handler = emergency_restore; sigaction(sigs[i], &sa, NULL); }
    return true;
}
static void raw_leave(void) { tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_old); }
static void fill_input(int timeout_ms) {
    if (g_inpos >= g_inlen) g_inpos = g_inlen = 0;
    struct pollfd p = { STDIN_FILENO, POLLIN, 0 };
    if (poll(&p, 1, timeout_ms) > 0 && (p.revents & POLLIN)) {
        ssize_t n = read(STDIN_FILENO, g_in + g_inlen, sizeof g_in - (size_t)g_inlen);
        if (n > 0) g_inlen += (int)n;
    }
}
#endif

/* ---------------------------------------------------------------- open / close */
static const char *ENTER_SEQ = "\x1b[?1049h\x1b[?25l\x1b[?2004h\x1b[?1000h\x1b[?1002h\x1b[?1006h\x1b[2J";
static const char *LEAVE_SEQ = "\x1b[?1006l\x1b[?1002l\x1b[?1000l\x1b[?2004l\x1b[0m\x1b[?25h\x1b[?1049l";

static void alloc_screen(void) {
    free(g_cur); free(g_prev);
    g_cw = term_w; g_ch = term_h;
    g_cur = xcalloc((size_t)g_cw * (size_t)g_ch, sizeof(Cell));
    g_prev = xcalloc((size_t)g_cw * (size_t)g_ch, sizeof(Cell));
    g_force = true;
}
bool term_open(void) {
    if (!raw_enter()) return false;
    const char *ct = getenv("COLORTERM");
    term_truecolor = (ct && (strstr(ct, "truecolor") || strstr(ct, "24bit")));
#ifdef _WIN32
    term_truecolor = true;
#endif
    if (!term_truecolor) { const char *t = getenv("TERM"); if (t && (strstr(t, "direct") || strstr(t, "kitty") || strstr(t, "alacritty"))) term_truecolor = true; }
    str_init(&g_extra);
    raw_write(ENTER_SEQ, strlen(ENTER_SEQ));
    alloc_screen(); g_open = true;
    return true;
}
void term_close(void) {
    if (!g_open) return;
    raw_write(LEAVE_SEQ, strlen(LEAVE_SEQ));
    raw_leave(); g_open = false;
}
void term_suspend(void) { if (g_open) { raw_write(LEAVE_SEQ, strlen(LEAVE_SEQ)); raw_leave(); } }
void term_resume(void) {
    if (!g_open) return;
    raw_enter(); raw_write(ENTER_SEQ, strlen(ENTER_SEQ));
    g_inlen = g_inpos = 0; g_winch = 1; g_force = true;
}

/* ---------------------------------------------------------------- input */
static int avail(void) { return g_inlen - g_inpos; }
static void need_more(int ms) { if (g_inpos > 0 && g_inpos == g_inlen) g_inpos = g_inlen = 0; fill_input(ms); }

static void set_key(Event *e, int key, int mods) { e->type = EV_KEY; e->key = key; e->mods = mods; }

static bool parse_csi(Event *e) {
    /* g_in[g_inpos] points just after "ESC [" */
    int i = g_inpos; int prm[8] = {0}; int np = 0; bool have = false; bool lt = false;
    if (i < g_inlen && g_in[i] == '<') { lt = true; i++; }
    while (i < g_inlen && ((g_in[i] >= '0' && g_in[i] <= '9') || g_in[i] == ';')) {
        if (g_in[i] == ';') { if (np < 7) np++; have = false; }
        else { prm[np] = prm[np] * 10 + (g_in[i] - '0'); have = true; }
        i++;
    }
    (void)have;
    if (i >= g_inlen) return false; /* incomplete */
    unsigned char f = g_in[i];
    if (f < 0x40 || f > 0x7E) { /* intermediate bytes: skip to final */ while (i < g_inlen && !(g_in[i] >= 0x40 && g_in[i] <= 0x7E)) i++; if (i >= g_inlen) return false; f = g_in[i]; }
    int total = np + 1;
    g_inpos = i + 1;
    int mod = (total >= 2 && prm[1] > 0) ? prm[1] - 1 : 0;
    int mods = (mod & 1 ? MOD_SHIFT : 0) | (mod & 2 ? MOD_ALT : 0) | (mod & 4 ? MOD_CTRL : 0);
    if (lt && (f == 'M' || f == 'm')) {
        int b = prm[0];
        e->type = EV_MOUSE; e->mx = prm[1] - 1; e->my = prm[2] - 1; e->mods = (b & 4 ? MOD_SHIFT : 0) | (b & 8 ? MOD_ALT : 0) | (b & 16 ? MOD_CTRL : 0);
        e->press = f == 'M'; e->release = f == 'm'; e->drag = (b & 32) != 0;
        int base = b & 3;
        if (b & 64) { e->btn = base == 0 ? 4 : 5; e->drag = false; e->release = false; e->press = true; if (base > 1) e->type = EV_NONE; }
        else e->btn = base;
        if (e->drag && base == 3) { e->btn = 3; }
        return true;
    }
    switch (f) {
    case 'A': set_key(e, K_UP, mods); return true;
    case 'B': set_key(e, K_DOWN, mods); return true;
    case 'C': set_key(e, K_RIGHT, mods); return true;
    case 'D': set_key(e, K_LEFT, mods); return true;
    case 'H': set_key(e, K_HOME, mods); return true;
    case 'F': set_key(e, K_END, mods); return true;
    case 'Z': set_key(e, K_TAB, MOD_SHIFT); return true;
    case 'P': set_key(e, K_FN(1), mods); return true;
    case 'Q': set_key(e, K_FN(2), mods); return true;
    case 'R': set_key(e, K_FN(3), mods); return true;
    case 'S': set_key(e, K_FN(4), mods); return true;
    case 'u': { /* CSI-u: code ; mods */
        int code = prm[0];
        if (code == 13) set_key(e, K_ENTER, mods); else if (code == 9) set_key(e, K_TAB, mods); else if (code == 27) set_key(e, K_ESC, mods);
        else if (code == 127) set_key(e, K_BKSP, mods); else set_key(e, (int)cp_lower((uint32_t)code), mods);
        return true;
    }
    case '~':
        switch (prm[0]) {
        case 1: case 7: set_key(e, K_HOME, mods); return true;
        case 2: set_key(e, K_INS, mods); return true;
        case 3: set_key(e, K_DEL, mods); return true;
        case 4: case 8: set_key(e, K_END, mods); return true;
        case 5: set_key(e, K_PGUP, mods); return true;
        case 6: set_key(e, K_PGDN, mods); return true;
        case 11: case 12: case 13: case 14: set_key(e, K_FN(prm[0] - 10), mods); return true;
        case 15: set_key(e, K_FN(5), mods); return true;
        case 17: case 18: case 19: case 20: case 21: set_key(e, K_FN(prm[0] - 11), mods); return true;
        case 23: case 24: set_key(e, K_FN(prm[0] - 12), mods); return true;
        case 200: { /* bracketed paste */
            Str s; str_init(&s); uint64_t t0 = now_ms();
            for (;;) {
                while (avail() > 0) {
                    if (g_in[g_inpos] == 0x1b && avail() >= 6 && memcmp(g_in + g_inpos, "\x1b[201~", 6) == 0) { g_inpos += 6; e->type = EV_PASTE; e->paste = str_take(&s); return true; }
                    if (g_in[g_inpos] == 0x1b && avail() < 6) break;
                    str_addc(&s, (char)g_in[g_inpos++]);
                }
                if (now_ms() - t0 > 1500) { e->type = EV_PASTE; e->paste = str_take(&s); return true; }
                need_more(50);
            }
        }
        default: e->type = EV_NONE; return true;
        }
    default: e->type = EV_NONE; return true;
    }
}

static bool parse_event(Event *e) {
    memset(e, 0, sizeof *e);
    if (avail() <= 0) return false;
    unsigned char c = g_in[g_inpos];
    if (c == 0x1b) {
        if (avail() == 1) { need_more(25); if (avail() == 1) { g_inpos++; set_key(e, K_ESC, 0); return true; } }
        unsigned char d = g_in[g_inpos + 1];
        if (d == '[') {
            g_inpos += 2;
            int save = g_inpos;
            for (int tries = 0; tries < 4; tries++) {
                g_inpos = save;
                if (parse_csi(e)) return true;
                need_more(25);
            }
            g_inpos = g_inlen; e->type = EV_NONE; return true;
        }
        if (d == 'O') {
            if (avail() < 3) need_more(25);
            if (avail() >= 3) {
                unsigned char f = g_in[g_inpos + 2]; g_inpos += 3;
                switch (f) {
                case 'A': set_key(e, K_UP, 0); return true; case 'B': set_key(e, K_DOWN, 0); return true;
                case 'C': set_key(e, K_RIGHT, 0); return true; case 'D': set_key(e, K_LEFT, 0); return true;
                case 'H': set_key(e, K_HOME, 0); return true; case 'F': set_key(e, K_END, 0); return true;
                case 'P': case 'Q': case 'R': case 'S': set_key(e, K_FN(f - 'P' + 1), 0); return true;
                default: e->type = EV_NONE; return true;
                }
            }
        }
        /* Alt + key */
        g_inpos += 1;
        if (!parse_event(e)) { set_key(e, K_ESC, 0); return true; }
        if (e->type == EV_KEY) e->mods |= MOD_ALT;
        return true;
    }
    g_inpos++;
    if (c == 0x7f) { set_key(e, K_BKSP, 0); return true; }
    if (c == 0x08) { set_key(e, K_BKSP, MOD_CTRL); return true; }
    if (c == '\r') { set_key(e, K_ENTER, 0); return true; }
    if (c == '\t') { set_key(e, K_TAB, 0); return true; }
    if (c == 0) { set_key(e, ' ', MOD_CTRL); return true; }
    if (c >= 1 && c <= 26) { set_key(e, 'a' + c - 1, MOD_CTRL); return true; }
    if (c == 0x1c) { set_key(e, '\\', MOD_CTRL); return true; }
    if (c == 0x1d) { set_key(e, ']', MOD_CTRL); return true; }
    if (c == 0x1e) { set_key(e, '6', MOD_CTRL); return true; }
    if (c == 0x1f) { set_key(e, '/', MOD_CTRL); return true; }
    if (c < 0x80) { set_key(e, c, 0); return true; }
    /* UTF-8 sequence */
    g_inpos--;
    int need = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : 2;
    if (avail() < need) need_more(10);
    uint32_t cp; int n = utf8_decode((const char *)g_in + g_inpos, (size_t)avail(), &cp);
    g_inpos += n; set_key(e, (int)cp, 0);
    return true;
}

bool term_next_event(Event *ev, int timeout_ms, const int *fds, int nfds, bool *extra_ready) {
    if (extra_ready) *extra_ready = false;
    if (g_winch) {
        g_winch = 0;
        int ow = term_w, oh = term_h; query_size();
        if (term_w != ow || term_h != oh || g_force) { alloc_screen(); memset(ev, 0, sizeof *ev); ev->type = EV_RESIZE; return true; }
    }
    if (avail() > 0 && parse_event(ev)) { if (ev->type != EV_NONE) return true; }
    if (avail() <= 0) {
#ifndef _WIN32
        struct pollfd p[16]; int np = 1;
        p[0] = (struct pollfd){ STDIN_FILENO, POLLIN, 0 };
        for (int i = 0; i < nfds && np < 16; i++) if (fds[i] >= 0) p[np++] = (struct pollfd){ fds[i], POLLIN | POLLHUP, 0 };
        int r = poll(p, (nfds_t)np, timeout_ms);
        if (r > 0) {
            for (int i = 1; i < np; i++) if (extra_ready && (p[i].revents & (POLLIN | POLLHUP))) *extra_ready = true;
            if (p[0].revents & POLLIN) {
                if (g_inpos >= g_inlen) g_inpos = g_inlen = 0;
                ssize_t n = read(STDIN_FILENO, g_in + g_inlen, sizeof g_in - (size_t)g_inlen);
                if (n > 0) g_inlen += (int)n;
            }
        }
#else
        (void)fds; (void)nfds; fill_input(timeout_ms);
        { static int tick = 0; if (++tick % 4 == 0) { int ow = term_w, oh = term_h; query_size(); if (ow != term_w || oh != term_h) g_winch = 1; } }
#endif
    }
    if (avail() > 0 && parse_event(ev) && ev->type != EV_NONE) return true;
    if (g_winch) { g_winch = 0; int ow = term_w, oh = term_h; query_size(); if (term_w != ow || term_h != oh) { alloc_screen(); memset(ev, 0, sizeof *ev); ev->type = EV_RESIZE; return true; } }
    return false;
}

/* ---------------------------------------------------------------- screen */
Cell *scr_cell(int x, int y) { return (x >= 0 && y >= 0 && x < g_cw && y < g_ch) ? &g_cur[y * g_cw + x] : NULL; }
void scr_clear(Style s) { scr_fill(0, 0, g_cw, g_ch, s); }
void scr_putc(int x, int y, uint32_t ch, Style s) {
    Cell *c = scr_cell(x, y); if (!c) return;
    c->ch = ch; c->fg = s.fg; c->bg = s.bg; c->attr = s.attr;
}
int scr_textn(int x, int y, const char *t, int n, int maxw, Style s) {
    int col = 0, i = 0;
    while (i < n && col < maxw) {
        uint32_t cp; int k = utf8_decode(t + i, (size_t)(n - i), &cp); i += k;
        if (cp == '\t') cp = ' ';
        if (cp < 32) cp = 0xB7;
        int w = cp_width(cp);
        if (w == 0) continue;
        if (col + w > maxw) break;
        scr_putc(x + col, y, cp, s);
        if (w == 2) scr_putc(x + col + 1, y, 0, s);
        col += w;
    }
    return col;
}
int scr_text(int x, int y, const char *t, int maxw, Style s) { return scr_textn(x, y, t, (int)strlen(t), maxw, s); }
void scr_fill(int x, int y, int w, int h, Style s) {
    for (int j = y; j < y + h; j++) for (int i = x; i < x + w; i++) scr_putc(i, j, ' ', s);
}
void scr_restyle(int x, int y, int w, int h, Color fg, Color bg) {
    for (int j = y; j < y + h; j++) for (int i = x; i < x + w; i++) {
        Cell *c = scr_cell(i, j); if (!c) continue;
        if (fg != KEEP) c->fg = fg;
        if (bg != KEEP) c->bg = bg;
    }
}
void scr_cursor(int x, int y, bool visible) { g_curx = x; g_cury = y; g_curvis = visible; }
void scr_invalidate(void) { g_force = true; }

static int to256(Color c) {
    int r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
    if (abs(r - g) < 10 && abs(g - b) < 10) {
        int gray = (r + g + b) / 3;
        if (gray < 8) return 16;
        if (gray > 248) return 231;
        return 232 + (gray - 8) * 24 / 240;
    }
    int rr = r < 48 ? 0 : r < 115 ? 1 : (r - 35) / 40, gg = g < 48 ? 0 : g < 115 ? 1 : (g - 35) / 40, bb = b < 48 ? 0 : b < 115 ? 1 : (b - 35) / 40;
    return 16 + 36 * rr + 6 * gg + bb;
}
static void emit_color(Str *o, bool fg, Color c) {
    if (term_truecolor) str_addf(o, "\x1b[%d;2;%u;%u;%um", fg ? 38 : 48, (c >> 16) & 255, (c >> 8) & 255, c & 255);
    else str_addf(o, "\x1b[%d;5;%dm", fg ? 38 : 48, to256(c));
}
void term_set_title(const char *t) { if (!g_open) return; str_addf(&g_extra, "\x1b]0;%s\x07", t); }
void term_clipboard_set(const char *d, size_t n) {
    if (!g_open) return;
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (n > 100000) return;
    str_adds(&g_extra, "\x1b]52;c;");
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)(unsigned char)d[i] << 16 | (i + 1 < n ? (uint32_t)(unsigned char)d[i + 1] << 8 : 0) | (i + 2 < n ? (uint32_t)(unsigned char)d[i + 2] : 0);
        str_addc(&g_extra, b64[(v >> 18) & 63]); str_addc(&g_extra, b64[(v >> 12) & 63]);
        str_addc(&g_extra, i + 1 < n ? b64[(v >> 6) & 63] : '='); str_addc(&g_extra, i + 2 < n ? b64[v & 63] : '=');
    }
    str_addc(&g_extra, '\x07');
}

void scr_flush(void) {
    if (!g_open) return;
    Str o; str_init(&o);
    str_adds(&o, "\x1b[?25l\x1b[?2026h");
    if (g_extra.n) { str_add(&o, g_extra.p, g_extra.n); str_clear(&g_extra); }
    Color lfg = KEEP, lbg = KEEP; int lattr = -1;
    int cx = -1, cy = -1;
    for (int y = 0; y < g_ch; y++) {
        for (int x = 0; x < g_cw; x++) {
            Cell *c = &g_cur[y * g_cw + x], *p = &g_prev[y * g_cw + x];
            if (c->ch == 0 && x > 0 && g_cur[y * g_cw + x - 1].ch != 0 && cp_width(g_cur[y * g_cw + x - 1].ch) == 2) { *p = *c; continue; }
            if (!g_force && c->ch == p->ch && c->fg == p->fg && c->bg == p->bg && c->attr == p->attr) continue;
            if (cx != x || cy != y) { str_addf(&o, "\x1b[%d;%dH", y + 1, x + 1); cx = x; cy = y; }
            if (c->attr != lattr) {
                str_adds(&o, "\x1b[0m");
                if (c->attr & ATTR_BOLD) str_adds(&o, "\x1b[1m");
                if (c->attr & ATTR_DIM) str_adds(&o, "\x1b[2m");
                if (c->attr & ATTR_ITALIC) str_adds(&o, "\x1b[3m");
                if (c->attr & ATTR_UNDERLINE) str_adds(&o, "\x1b[4m");
                if (c->attr & ATTR_REVERSE) str_adds(&o, "\x1b[7m");
                lattr = c->attr; lfg = lbg = KEEP;
            }
            if (c->fg != lfg) { emit_color(&o, true, c->fg); lfg = c->fg; }
            if (c->bg != lbg) { emit_color(&o, false, c->bg); lbg = c->bg; }
            char tmp[8]; uint32_t ch = c->ch ? c->ch : ' ';
            int n = utf8_encode(ch, tmp); str_add(&o, tmp, (size_t)n);
            cx += cp_width(ch) == 2 ? 2 : 1;
            *p = *c;
        }
    }
    g_force = false;
    str_adds(&o, "\x1b[0m");
    if (g_curvis) str_addf(&o, "\x1b[%d;%dH\x1b[?25h", g_cury + 1, g_curx + 1);
    str_adds(&o, "\x1b[?2026l");
    raw_write(o.p, o.n);
    str_free(&o);
}
