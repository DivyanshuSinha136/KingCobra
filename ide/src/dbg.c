/* kcide — debugger front end. Talks to `kingcobra debug` over two FIFOs (named pipes on Windows),
 * drives breakpoints / stepping / call stack / variables / watches / expression evaluation, and draws the
 * Debug panel. Protocol: docs/DEBUGGER.md. */
#include "ide.h"
#include <ctype.h>
#include <stdarg.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#define D G.dbg
#ifdef _WIN32
static int g_seq;
#endif

/* ---------------------------------------------------------------- channel */
static void chan_destroy(void) {
#ifdef _WIN32
    if (D.h_evt) CloseHandle(D.h_evt); if (D.h_cmd) CloseHandle(D.h_cmd); D.h_evt = D.h_cmd = NULL;
#else
    if (D.evt_fd >= 0) close(D.evt_fd); if (D.cmd_fd >= 0) close(D.cmd_fd); D.evt_fd = D.cmd_fd = -1;
    if (D.base[0]) { char p[400]; snprintf(p, sizeof p, "%s.cmd", D.base); unlink(p); snprintf(p, sizeof p, "%s.evt", D.base); unlink(p); if (D.dir[0]) rmdir(D.dir); }
#endif
    D.base[0] = D.dir[0] = 0; D.nlb = 0;
}
static bool chan_create(void) {
    D.nlb = 0; D.block = 0;
#ifdef _WIN32
    snprintf(D.base, sizeof D.base, "\\\\.\\pipe\\kcide-dbg-%lu-%d", (unsigned long)GetCurrentProcessId(), ++g_seq);
    char p[400];
    snprintf(p, sizeof p, "%s.cmd", D.base); D.h_cmd = CreateNamedPipeA(p, PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT, 1, 65536, 65536, 0, NULL);
    snprintf(p, sizeof p, "%s.evt", D.base); D.h_evt = CreateNamedPipeA(p, PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT, 1, 65536, 65536, 0, NULL);
    return D.h_cmd != INVALID_HANDLE_VALUE && D.h_evt != INVALID_HANDLE_VALUE && D.h_cmd && D.h_evt;
#else
    const char *tmp = getenv("TMPDIR"); if (!tmp || !*tmp) tmp = "/tmp";
    snprintf(D.dir, sizeof D.dir, "%s/kcide-dbg-XXXXXX", tmp);
    if (!mkdtemp(D.dir)) { D.dir[0] = 0; return false; }
    snprintf(D.base, sizeof D.base, "%s/p", D.dir);
    char c[400], e[400]; snprintf(c, sizeof c, "%s.cmd", D.base); snprintf(e, sizeof e, "%s.evt", D.base);
    if (mkfifo(c, 0600) != 0 || mkfifo(e, 0600) != 0) { chan_destroy(); return false; }
    D.evt_fd = open(e, O_RDONLY | O_NONBLOCK);
    D.cmd_fd = open(c, O_RDWR | O_NONBLOCK);                     /* O_RDWR: never blocks, and stays open until the debuggee connects */
    if (D.evt_fd < 0 || D.cmd_fd < 0) { chan_destroy(); return false; }
    return true;
#endif
}
static void send_cmd(const char *fmt, ...) {
    char b[4096]; va_list ap; va_start(ap, fmt); int n = vsnprintf(b, sizeof b - 2, fmt, ap); va_end(ap);
    if (n < 0) return; if (n > (int)sizeof b - 2) n = (int)sizeof b - 2;
    for (int i = 0; i < n; i++) if (b[i] == '\n' || b[i] == '\r') b[i] = ' ';
    b[n++] = '\n';
#ifdef _WIN32
    DWORD w; if (D.h_cmd) WriteFile(D.h_cmd, b, (DWORD)n, &w, NULL);
#else
    if (D.cmd_fd >= 0) { ssize_t w = write(D.cmd_fd, b, (size_t)n); (void)w; }
#endif
}
int dbg_fd(void) { return D.state != DS_IDLE ? D.evt_fd : -1; }
bool dbg_active(void) { return D.state != DS_IDLE; }

/* ---------------------------------------------------------------- state helpers */
static void free_frames(void) { for (int i = 0; i < D.nframes; i++) { free(D.frames[i].name); free(D.frames[i].file); } free(D.frames); D.frames = NULL; D.nframes = 0; D.fsel = 0; }
static void free_vars(void) { for (int i = 0; i < D.nvars; i++) { free(D.vars[i].name); free(D.vars[i].type); free(D.vars[i].value); } free(D.vars); D.vars = NULL; D.nvars = 0; }
static void clear_watch_values(void) { for (int i = 0; i < D.nwatch; i++) { free(D.wval[i]); D.wval[i] = NULL; } }
void dbg_init(void) { D.evt_fd = D.cmd_fd = -1; D.state = DS_IDLE; }
static void reset_session(void) { free_frames(); free_vars(); clear_watch_values(); free(D.file); D.file = NULL; D.line = 0; D.npend = 0; D.block = 0; D.reason[0] = D.msg[0] = 0; }
void dbg_shutdown(void) { if (D.state != DS_IDLE) send_cmd("quit"); chan_destroy(); reset_session(); }

static void send_eval(int tag, int frame, const char *expr) {
    if (D.npend < 64) D.pend[D.npend++] = tag; else return;
    send_cmd("eval\t%d\t%s", frame, expr);
}
static void refresh_frame_vars(void) { free_vars(); send_cmd("vars\t%d", D.fsel); }
static void request_state(void) {
    send_cmd("stack"); send_cmd("vars\t0");
    for (int i = 0; i < D.nwatch; i++) send_eval(i, 0, D.watch[i]);
}
static void show_location(const char *file, int line) {
    if (!file || !*file || line < 1) return;
    Doc *d = doc_open_path(file, true); if (d) { ed_goto(d, line - 1, 0, false); int a = d->cur.line; (void)a; }
}

/* ---------------------------------------------------------------- incoming events */
static char *fld(char **save) { char *s = *save; if (!s) return ""; char *t = strchr(s, '\t'); if (t) { *t = 0; *save = t + 1; } else *save = NULL; return s; }
static void on_stopped(char *rest) {
    char *r = rest; char *reason = fld(&r), *file = fld(&r), *line = fld(&r), *depth = fld(&r), *msg = r ? r : "";
    reset_session(); D.state = DS_PAUSED;
    snprintf(D.reason, sizeof D.reason, "%s", reason); snprintf(D.msg, sizeof D.msg, "%s", msg); D.file = xstrdup(file); D.line = atoi(line); D.depth = atoi(depth);
    show_location(file, D.line); request_state();
    if (!strcmp(reason, "exception")) { status(3, "Uncaught error: %s", msg); if (G.show_panel == false) panel_show(true); G.ptab = PT_DEBUG; }
    else status(0, "Paused (%s) at %s:%d", reason, path_basename(file), D.line);
    if (!G.show_panel) panel_show(true); if (G.ptab != PT_DEBUG && G.ptab != PT_CONSOLE) G.ptab = PT_DEBUG;
}
static void handle_line(char *l) {
    char *rest = l, *kind = fld(&rest);
    if (!strcmp(kind, "ready")) {
        for (int i = 0; i < G.ndocs; i++) { Doc *d = G.docs[i]; if (!d->path) continue; for (int k = 0; k < d->nlines; k++) if (d->lines[k].flags & LF_BREAK) send_cmd("break\t%s\t%d", d->path, k + 1); }
        send_cmd(D.stop_on_entry ? "entry" : "go"); D.state = DS_RUNNING; D.block = 0;
    } else if (!strcmp(kind, "stopped")) on_stopped(rest ? rest : (char *)"");
    else if (!strcmp(kind, "stack")) { free_frames(); D.block = 1; }
    else if (!strcmp(kind, "vars")) { free_vars(); D.block = 2; }
    else if (!strcmp(kind, "end")) D.block = 0;
    else if (!strcmp(kind, "frame") && D.block == 1) {
        char *r = rest; fld(&r); char *name = fld(&r), *file = fld(&r), *line = fld(&r);
        D.frames = xrealloc(D.frames, (size_t)(D.nframes + 1) * sizeof(DFrame)); D.frames[D.nframes++] = (DFrame){ xstrdup(name), xstrdup(file), atoi(line) };
    } else if (!strcmp(kind, "var") && D.block == 2) {
        char *r = rest; char *scope = fld(&r), *name = fld(&r), *type = fld(&r), *val = r ? r : "";
        D.vars = xrealloc(D.vars, (size_t)(D.nvars + 1) * sizeof(DVar)); DVar *v = &D.vars[D.nvars++]; snprintf(v->scope, sizeof v->scope, "%s", scope); v->name = xstrdup(name); v->type = xstrdup(type); v->value = xstrdup(val);
    } else if (!strcmp(kind, "result")) {
        char *r = rest; char *ok = fld(&r), *type = fld(&r), *val = r ? r : ""; int tag = D.npend ? D.pend[0] : -1; if (D.npend) memmove(D.pend, D.pend + 1, (size_t)(--D.npend) * sizeof(int));
        bool good = !strcmp(ok, "ok");
        if (tag >= 0 && tag < D.nwatch) { free(D.wval[tag]); D.wval[tag] = good ? xfmt("%s", val) : xfmt("<%s>", val); }
        else {
            bool assigned = good && !strcmp(type, "assigned");
            char *m = !good ? xstrdup(val) : assigned ? xstrdup("(assigned)") : xfmt("%s  (%s)", val, type); console_print(m, good ? TH.ok : TH.error, 0); free(m);
            if (assigned && D.state == DS_PAUSED) { refresh_frame_vars(); for (int i = 0; i < D.nwatch; i++) send_eval(i, D.fsel, D.watch[i]); }
        }
    } else if (!strcmp(kind, "exited")) { /* the process exit is handled by dbg_process_exited */ }
}
void dbg_poll(void) {
    if (D.state == DS_IDLE) return;
    char buf[4096]; int n;
#ifdef _WIN32
    if (D.h_evt) { ConnectNamedPipe(D.h_evt, NULL); ConnectNamedPipe(D.h_cmd, NULL); }
    for (;;) { DWORD avail = 0, got = 0; if (!D.h_evt || !PeekNamedPipe(D.h_evt, NULL, 0, NULL, &avail, NULL) || !avail) break; if (!ReadFile(D.h_evt, buf, avail < sizeof buf ? avail : (DWORD)sizeof buf, &got, NULL) || !got) break; n = (int)got;
#else
    while (D.evt_fd >= 0 && (n = (int)read(D.evt_fd, buf, sizeof buf)) > 0) {
#endif
        for (int i = 0; i < n; i++) {
            if (buf[i] == '\n') { D.lb[D.nlb] = 0; if (D.nlb) { char *copy = xstrdup(D.lb); handle_line(copy); free(copy); } D.nlb = 0; if (D.state == DS_IDLE) return; }
            else if (buf[i] != '\r' && D.nlb < (int)sizeof D.lb - 1) D.lb[D.nlb++] = buf[i];
        }
    }
}
void dbg_process_exited(int code) {
    bool was = D.state != DS_IDLE; chan_destroy(); D.state = DS_IDLE; reset_session();
    if (was) { status(code == 0 ? 1 : 2, "Debug session ended (exit code %d)", code); if (G.ptab == PT_DEBUG) { /* keep the panel; it now shows the idle hint */ } }
}

/* ---------------------------------------------------------------- user actions */
void dbg_start(bool entry) {
    Doc *d = cur_doc();
    if (!G.kc_found) { status(3, "kingcobra executable not found - set KINGCOBRA or kc_path"); return; }
    if (!d || !d->path || d->lang != LANG_KC) { status(2, "Open and save a .kc file to debug it"); if (d && !d->path) cmd_run(C_SAVE); return; }
    if (D.state != DS_IDLE) dbg_stop();
    D.stop_on_entry = entry;
    cmd_run(C_SAVEALL);
    reset_session();
    if (!chan_create()) { status(3, "cannot create the debug channel"); return; }
    char env[420]; snprintf(env, sizeof env, "KC_DEBUG_PIPE=%s", D.base); const char *envv[2] = { env, NULL };
    char *argv[4] = { G.kc_exe, "debug", d->path, NULL };
    free(D.target); D.target = xstrdup(d->path);
    if (!run_spawn("debug", argv, envv, true)) { chan_destroy(); status(3, "cannot start the debuggee"); return; }
    D.state = DS_STARTING; panel_set_tab(PT_DEBUG, false); G.focus = FOCUS_EDITOR;
    status(0, "Debugging %s - F10 step over, F11 step into, F5 continue", path_basename(d->path));
}
void dbg_stop(void) {
    if (D.state == DS_IDLE) return;
    send_cmd("quit"); if (G.proc.running) proc_stop(&G.proc);
    dbg_process_exited(G.proc.exit_code);
}
void dbg_command(int cmd) {
    if (D.state == DS_IDLE) {
        if (cmd == C_DBG_OVER || cmd == C_DBG_IN) dbg_start(true); else if (cmd == C_DBG_CONT) dbg_start(false);
        return;
    }
    switch (cmd) {
    case C_DBG_PAUSE: if (D.state == DS_RUNNING) send_cmd("pause"); break;
    case C_DBG_STOP: dbg_stop(); break;
    default:
        if (D.state != DS_PAUSED) { status(2, "The program is %s", D.state == DS_STARTING ? "starting" : "running"); return; }
        send_cmd(cmd == C_DBG_CONT ? "continue" : cmd == C_DBG_OVER ? "stepover" : cmd == C_DBG_IN ? "stepin" : "stepout");
        D.state = DS_RUNNING; free_frames(); free_vars(); free(D.file); D.file = NULL; break;
    }
}
void dbg_toggle_bp(Doc *d, int line) {
    if (!d || line < 0 || line >= d->nlines) return;
    Line *l = &d->lines[line]; l->flags ^= LF_BREAK; bool on = (l->flags & LF_BREAK) != 0;
    if (d->path && D.state != DS_IDLE) send_cmd("%s\t%s\t%d", on ? "break" : "unbreak", d->path, line + 1);
    status(0, "Breakpoint %s at %s:%d", on ? "set" : "removed", d->title, line + 1);
}
void dbg_clear_breakpoints(void) { if (D.state != DS_IDLE) send_cmd("clear"); }
void dbg_add_watch(const char *expr) {
    if (!expr || !*expr) return;
    D.watch = xrealloc(D.watch, (size_t)(D.nwatch + 1) * sizeof(char *)); D.wval = xrealloc(D.wval, (size_t)(D.nwatch + 1) * sizeof(char *));
    D.watch[D.nwatch] = xstrdup(expr); D.wval[D.nwatch] = NULL; D.nwatch++;
    if (D.state == DS_PAUSED) send_eval(D.nwatch - 1, D.fsel, expr);
}
static void del_watch(int i) {
    if (i < 0 || i >= D.nwatch) return; free(D.watch[i]); free(D.wval[i]);
    memmove(D.watch + i, D.watch + i + 1, (size_t)(D.nwatch - i - 1) * sizeof(char *)); memmove(D.wval + i, D.wval + i + 1, (size_t)(D.nwatch - i - 1) * sizeof(char *)); D.nwatch--;
    if (D.sel[2] >= D.nwatch) D.sel[2] = D.nwatch - 1; if (D.sel[2] < 0) D.sel[2] = 0;
}
bool dbg_eval_console(const char *expr) { if (D.state != DS_PAUSED) return false; send_eval(-1, D.fsel, expr); return true; }
bool dbg_exec_line(Doc *d, int *line) {
    if (D.state != DS_PAUSED || !d->path) return false;
    const char *f = D.file; int ln = D.line;
    if (D.fsel > 0 && D.fsel < D.nframes) { f = D.frames[D.fsel].file; ln = D.frames[D.fsel].line; }
    if (!f || !path_eq(f, d->path)) return false;
    *line = ln - 1; return true;
}

/* ---------------------------------------------------------------- panel */
int dbg_header_buttons(const char **lab, int *cmd, int max) {
    int n = 0;
    #define B(l, c) do { if (n < max - 1) { lab[n] = l; cmd[n] = c; n++; } } while (0)
    if (D.state == DS_IDLE) B("\xe2\x96\xb6 Start Debugging  Alt+F5", C_DEBUG);
    else if (D.state == DS_PAUSED) { B("\xe2\x96\xb6 Continue F5", C_DBG_CONT); B("Over F10", C_DBG_OVER); B("Into F11", C_DBG_IN); B("Out S-F11", C_DBG_OUT); B("\xe2\x96\xa0 Stop", C_DBG_STOP); }
    else { B("\xe2\x8f\xb8 Pause", C_DBG_PAUSE); B("\xe2\x96\xa0 Stop", C_DBG_STOP); }
    return n;
}
static int pane_rect(Rect r, int pane, Rect *out) {
    int w0 = r.w * 27 / 100, w1 = r.w * 42 / 100;
    if (pane == 0) *out = (Rect){ r.x, r.y, w0, r.h };
    else if (pane == 1) *out = (Rect){ r.x + w0, r.y, w1, r.h };
    else *out = (Rect){ r.x + w0 + w1, r.y, r.w - w0 - w1, r.h };
    return 0;
}
static void pane_title(Rect p, const char *t, bool focus) {
    scr_fill(p.x, p.y, p.w, 1, ST(focus ? TH.accent : TH.ui_dim, TH.line_bg)); scr_text(p.x + 1, p.y, t, p.w - 2, STA(focus ? TH.accent : TH.ui_dim, TH.line_bg, ATTR_BOLD));
}
static void clamp_scroll(int pane, int n, int rows) {
    if (D.sel[pane] >= n) D.sel[pane] = n - 1; if (D.sel[pane] < 0) D.sel[pane] = 0;
    if (D.sel[pane] < D.scroll[pane]) D.scroll[pane] = D.sel[pane]; if (D.sel[pane] >= D.scroll[pane] + rows) D.scroll[pane] = D.sel[pane] - rows + 1; if (D.scroll[pane] < 0) D.scroll[pane] = 0;
}
void dbg_panel_draw(Rect r) {
    scr_fill(r.x, r.y, r.w, r.h, ST(TH.fg, TH.bg));
    bool foc = G.focus == FOCUS_PANEL; Rect p[3]; for (int i = 0; i < 3; i++) pane_rect(r, i, &p[i]);
    pane_title(p[0], "CALL STACK", foc && D.pane == 0); pane_title(p[1], "VARIABLES", foc && D.pane == 1); pane_title(p[2], "WATCH  (a add, d delete)", foc && D.pane == 2);
    for (int i = 0; i < 2; i++) for (int y = r.y; y < r.y + r.h; y++) scr_putc(p[i].x + p[i].w - 1, y, 0x2502, ST(TH.border, TH.bg));
    int rows = r.h - 1;
    /* stack */
    if (D.state == DS_PAUSED) {
        D.sel[0] = D.fsel; clamp_scroll(0, D.nframes, rows);
        for (int i = 0; i < rows && D.scroll[0] + i < D.nframes; i++) {
            DFrame *f = &D.frames[D.scroll[0] + i]; bool sel = D.scroll[0] + i == D.fsel; Color bg = sel ? (foc && D.pane == 0 ? TH.menu_sel : TH.border) : TH.bg;
            scr_fill(p[0].x, r.y + 1 + i, p[0].w - 1, 1, ST(TH.fg, bg)); char b[300]; snprintf(b, sizeof b, "%s  %s:%d", f->name, path_basename(f->file), f->line);
            int x = p[0].x + 1; x += scr_text(x, r.y + 1 + i, f->name, p[0].w - 3, STA(TH.hl[HC_FUNCDEF], bg, ATTR_BOLD)); char loc[200]; snprintf(loc, sizeof loc, "  %s:%d", path_basename(f->file), f->line); scr_text(x, r.y + 1 + i, loc, p[0].x + p[0].w - 1 - x, ST(TH.ui_dim, bg)); (void)b;
        }
    } else {
        const char *m = D.state == DS_IDLE ? "Not debugging." : D.state == DS_STARTING ? "Starting..." : "Running...";
        scr_text(p[0].x + 1, r.y + 1, m, p[0].w - 2, ST(TH.ui_dim, TH.bg));
        if (D.state == DS_IDLE) { scr_text(p[0].x + 1, r.y + 3, "F9 toggles a", p[0].w - 2, ST(TH.ui_dim, TH.bg)); scr_text(p[0].x + 1, r.y + 4, "breakpoint,", p[0].w - 2, ST(TH.ui_dim, TH.bg)); scr_text(p[0].x + 1, r.y + 5, "Alt+F5 starts.", p[0].w - 2, ST(TH.ui_dim, TH.bg)); }
    }
    /* variables */
    if (D.state == DS_PAUSED) {
        if (D.reason[0]) { char b[600]; snprintf(b, sizeof b, "%s%s%s", D.reason, D.msg[0] ? ": " : "", D.msg); scr_text(p[1].x + 20, r.y, b, p[1].w - 22, ST(!strcmp(D.reason, "exception") ? TH.error : TH.warn, TH.line_bg)); }
        clamp_scroll(1, D.nvars, rows);
        for (int i = 0; i < rows && D.scroll[1] + i < D.nvars; i++) {
            DVar *v = &D.vars[D.scroll[1] + i]; bool sel = D.scroll[1] + i == D.sel[1] && foc && D.pane == 1; Color bg = sel ? TH.menu_sel : TH.bg; int y = r.y + 1 + i, x = p[1].x + 1;
            scr_fill(p[1].x, y, p[1].w - 1, 1, ST(TH.fg, bg)); bool gl = v->scope[0] == 'g';
            x += scr_text(x, y, v->name, p[1].w - 3, STA(gl ? TH.ui_dim : TH.hl[HC_TEXT], bg, ATTR_BOLD)) + 1; x += scr_text(x, y, v->type, 12, ST(TH.hl[HC_TYPE], bg)) + 1;
            if (x < p[1].x + p[1].w - 2) scr_text(x, y, v->value, p[1].x + p[1].w - 1 - x, ST(gl ? TH.ui_dim : TH.hl[HC_STRING], bg));
        }
        if (!D.nvars) scr_text(p[1].x + 1, r.y + 1, "(no variables in scope)", p[1].w - 2, ST(TH.ui_dim, TH.bg));
    } else scr_text(p[1].x + 1, r.y + 1, D.state == DS_RUNNING ? "Variables appear when the program pauses." : "", p[1].w - 2, ST(TH.ui_dim, TH.bg));
    /* watch */
    clamp_scroll(2, D.nwatch, rows);
    for (int i = 0; i < rows && D.scroll[2] + i < D.nwatch; i++) {
        int k = D.scroll[2] + i; bool sel = k == D.sel[2] && foc && D.pane == 2; Color bg = sel ? TH.menu_sel : TH.bg; int y = r.y + 1 + i, x = p[2].x + 1;
        scr_fill(p[2].x, y, p[2].w, 1, ST(TH.fg, bg)); x += scr_text(x, y, D.watch[k], p[2].w - 3, STA(TH.hl[HC_TEXT], bg, ATTR_BOLD)); x += scr_text(x, y, " = ", 3, ST(TH.ui_dim, bg));
        if (x < p[2].x + p[2].w - 1) scr_text(x, y, D.wval[k] ? D.wval[k] : (D.state == DS_PAUSED ? "..." : "n/a"), p[2].x + p[2].w - 1 - x, ST(D.wval[k] && D.wval[k][0] == '<' ? TH.error : TH.hl[HC_STRING], bg));
    }
    if (!D.nwatch) scr_text(p[2].x + 1, r.y + 1, "Press 'a' to add an expression.", p[2].w - 2, ST(TH.ui_dim, TH.bg));
}
static void select_frame(int k) { if (k < 0 || k >= D.nframes) return; D.fsel = k; refresh_frame_vars(); show_location(D.frames[k].file, D.frames[k].line); for (int i = 0; i < D.nwatch; i++) send_eval(i, k, D.watch[i]); }
static void add_watch_cb(const char *t, intptr_t id, void *ctx) { (void)id; (void)ctx; dbg_add_watch(t); G.ptab = PT_DEBUG; D.pane = 2; }
void dbg_panel_key(Event *e) {
    int k = e->key; int rows = G.r_panel.h > 2 ? G.r_panel.h - 2 : 1;
    if (k == K_ESC) { G.focus = FOCUS_EDITOR; return; }
    if (k == K_TAB) { D.pane = (D.pane + ((e->mods & MOD_SHIFT) ? 2 : 1)) % 3; return; }
    if (e->mods & (MOD_CTRL | MOD_ALT)) return;
    int n = D.pane == 0 ? D.nframes : D.pane == 1 ? D.nvars : D.nwatch;
    switch (k) {
    case K_UP: if (D.sel[D.pane] > 0) D.sel[D.pane]--; if (D.pane == 0 && D.state == DS_PAUSED) select_frame(D.sel[0]); break;
    case K_DOWN: if (D.sel[D.pane] + 1 < n) D.sel[D.pane]++; if (D.pane == 0 && D.state == DS_PAUSED) select_frame(D.sel[0]); break;
    case K_PGUP: D.sel[D.pane] = D.sel[D.pane] > rows ? D.sel[D.pane] - rows : 0; break; case K_PGDN: D.sel[D.pane] = D.sel[D.pane] + rows < n ? D.sel[D.pane] + rows : (n ? n - 1 : 0); break;
    case K_ENTER: if (D.pane == 0 && D.state == DS_PAUSED) select_frame(D.sel[0]); else if (D.pane == 2) ov_prompt("Add watch expression", "", false, add_watch_cb, NULL); break;
    case K_DEL: if (D.pane == 2) del_watch(D.sel[2]); break;
    case 'a': ov_prompt("Add watch expression", "", false, add_watch_cb, NULL); break;
    case 'd': if (D.pane == 2) del_watch(D.sel[2]); break;
    default: break;
    }
}
void dbg_panel_mouse(Event *e, Rect r) {
    Rect p[3]; for (int i = 0; i < 3; i++) pane_rect(r, i, &p[i]);
    for (int i = 0; i < 3; i++) if (in_rect(p[i], e->mx, e->my)) {
        if (e->btn == 4) { D.scroll[i] -= 3; if (D.scroll[i] < 0) D.scroll[i] = 0; return; } if (e->btn == 5) { D.scroll[i] += 3; return; }
        if (e->press && e->btn == 0 && !e->drag) {
            D.pane = i; int row = D.scroll[i] + (e->my - r.y - 1); int n = i == 0 ? D.nframes : i == 1 ? D.nvars : D.nwatch;
            if (e->my > r.y && row >= 0 && row < n) { D.sel[i] = row; if (i == 0 && D.state == DS_PAUSED) select_frame(row); }
        }
        return;
    }
}
