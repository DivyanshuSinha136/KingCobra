/* kcide — bottom panel: tab strip (Output | Terminal | Console | Debug), integrated terminals (full VT
 * emulation on a pty / ConPTY) and the KingCobra console (REPL, or debugger evaluation while paused). */
#include "ide.h"
#include <ctype.h>
#ifndef _WIN32
#include <unistd.h>
#endif

extern const int N_CMDS;
static const char *TAB_NAMES[PT_COUNT] = { "OUTPUT", "TERMINAL", "CONSOLE", "DEBUG" };
static Rect content(void) { return (Rect){ G.r_panel.x, G.r_panel.y + 1, G.r_panel.w, G.r_panel.h > 1 ? G.r_panel.h - 1 : 0 }; }

/* ---------------------------------------------------------------- terminals */
static void term_reply(void *ctx, const char *s, int n) { Term *t = ctx; proc_write(&t->proc, s, n); }
static bool term_spawn(Term *t) {
    Rect c = content(); int w = c.w > 2 ? c.w : 80, h = c.h > 1 ? c.h : 10;
    if (t->vt) vt_free(t->vt);
    t->vt = vt_new(w, h); t->vt->reply = term_reply; t->vt->rctx = t; t->exited = false; t->sel = false;
#ifdef _WIN32
    const char *shell = getenv("COMSPEC"); if (!shell) shell = "cmd.exe";
#else
    const char *shell = getenv("SHELL"); if (!shell || !*shell) shell = "/bin/sh";
#endif
    char *argv[2] = { (char *)shell, NULL };
    if (!proc_start_ex(&t->proc, argv, G.root, w, h, NULL, true)) { t->exited = true; vt_feed(t->vt, "cannot start shell\r\n", 20); return false; }
    return true;
}
void term_new(void) {
    if (G.nterms >= MAX_TERMS) { status(2, "At most %d terminals", MAX_TERMS); panel_set_tab(PT_TERMINAL, true); return; }
    panel_set_tab(PT_TERMINAL, false);
    Term *t = &G.terms[G.nterms]; memset(t, 0, sizeof *t); t->proc.fd = t->proc.wfd = -1; snprintf(t->name, sizeof t->name, "%d", G.nterms + 1);
    G.nterms++; G.cterm = G.nterms - 1; term_spawn(t); panel_set_tab(PT_TERMINAL, true);
}
void term_kill_current(void) {
    if (!G.nterms) return; Term *t = &G.terms[G.cterm];
    proc_stop(&t->proc); proc_close(&t->proc); vt_free(t->vt);
    memmove(t, t + 1, (size_t)(G.nterms - G.cterm - 1) * sizeof *t); G.nterms--;
    if (G.cterm >= G.nterms) G.cterm = G.nterms - 1; if (G.cterm < 0) G.cterm = 0;
    for (int i = 0; i < G.nterms; i++) { G.terms[i].vt->rctx = &G.terms[i]; snprintf(G.terms[i].name, sizeof G.terms[i].name, "%d", i + 1); }
    if (!G.nterms) G.focus = FOCUS_EDITOR;
}
static Term *cur_term(void) { return G.nterms ? &G.terms[G.cterm] : NULL; }
static void term_key(Event *e) {
    Term *t = cur_term(); if (!t) { if (e->key == K_ENTER) term_new(); return; }
    if (t->exited) { if (e->key == K_ENTER) { proc_close(&t->proc); term_spawn(t); } return; }
    if ((e->key == K_PGUP || e->key == K_PGDN) && (e->mods & MOD_SHIFT)) { vt_scroll_view(t->vt, (e->key == K_PGUP ? 1 : -1) * (t->vt->h - 1)); return; }
    char b[32]; int n = vt_encode_key(e, t->vt->appkeys, b);
    if (n) { t->vt->view = 0; t->sel = false; proc_write(&t->proc, b, n); }
}
static void term_copy_selection(Term *t) {
    int y0 = t->sy, x0 = t->sx, y1 = t->ey, x1 = t->ex;
    if (y1 < y0 || (y1 == y0 && x1 < x0)) { int a = y0, b = x0; y0 = y1; x0 = x1; y1 = a; x1 = b; }
    Str s; str_init(&s);
    for (int y = y0; y <= y1; y++) {
        int a = y == y0 ? x0 : 0, b = y == y1 ? x1 : t->vt->w - 1; int last = a - 1; Str ln; str_init(&ln);
        for (int x = a; x <= b; x++) { const Cell *c = vt_cell(t->vt, x, y); if (c->ch == 0) continue; char u[8]; int k = utf8_encode(c->ch, u); str_add(&ln, u, (size_t)k); if (c->ch != ' ') last = (int)ln.n; }
        (void)last; size_t e = ln.n; while (e > 0 && ln.p[e - 1] == ' ') e--; str_add(&s, ln.p, e); str_free(&ln); if (y < y1) str_addc(&s, '\n');
    }
    if (s.n) { free(G.clip); G.clip = xstrndup(s.p, s.n); G.cliplen = s.n; term_clipboard_set(s.p, s.n); status(0, "Copied %d characters", (int)s.n); }
    str_free(&s);
}
static void draw_terminal(Rect r) {
    Term *t = cur_term();
    scr_fill(r.x, r.y, r.w, r.h, ST(TH.fg, TH.bg));
    if (!t) { scr_text(r.x + 2, r.y + 1, "No terminal is open. Press Enter, or use the [+ New] button.", r.w - 4, ST(TH.ui_dim, TH.bg)); return; }
    VTerm *v = t->vt; Color sy0 = 0; (void)sy0;
    int y0 = t->sy, x0 = t->sx, y1 = t->ey, x1 = t->ex;
    if (y1 < y0 || (y1 == y0 && x1 < x0)) { int a = y0, b = x0; y0 = y1; x0 = x1; y1 = a; x1 = b; }
    for (int y = 0; y < r.h && y < v->h; y++) for (int x = 0; x < r.w && x < v->w; x++) {
        const Cell *c = vt_cell(v, x, y);
        Color fg = c->fg == VT_DEF ? TH.fg : c->fg, bg = c->bg == VT_DEF ? TH.bg : c->bg;
        if (t->sel && (y > y0 || (y == y0 && x >= x0)) && (y < y1 || (y == y1 && x <= x1))) bg = TH.sel_bg;
        scr_putc(r.x + x, r.y + y, c->ch ? c->ch : (x > 0 ? 0 : ' '), STA(fg, bg, c->attr));
    }
    if (v->view > 0) { char b[40]; snprintf(b, sizeof b, " scrollback +%d ", v->view); scr_text(r.x + r.w - (int)strlen(b), r.y, b, 40, ST(TH.bg, TH.warn)); }
    if (G.focus == FOCUS_PANEL && G.ptab == PT_TERMINAL && v->cursor_vis && v->view == 0 && !t->exited && G.ov.kind == OV_NONE) scr_cursor(r.x + v->cx, r.y + v->cy, true);
}

/* ---------------------------------------------------------------- console */
static void con_hist_add(const char *l) {
    if (!*l) return; if (G.con.nhist && !strcmp(G.con.hist[G.con.nhist - 1], l)) { G.con.hpos = G.con.nhist; return; }
    G.con.hist = xrealloc(G.con.hist, (size_t)(G.con.nhist + 1) * sizeof(char *)); G.con.hist[G.con.nhist++] = xstrdup(l); G.con.hpos = G.con.nhist;
}
void console_print(const char *text, Color fg, uint8_t attr) { out_puts(&G.con.log, text, fg, attr); }
void console_clear(void) { out_clear(&G.con.log); }
void console_prompt_label(char *o, size_t cap) { snprintf(o, cap, "%s", G.dbg.state == DS_PAUSED ? "dbg> " : "kc> "); }
static void console_start(const char *preload) {
    if (!G.kc_found) { console_print("kingcobra executable not found (set KINGCOBRA)", TH.error, 0); return; }
    char *argv[4]; int n = 0; argv[n++] = G.kc_exe; argv[n++] = "repl"; if (preload) argv[n++] = (char *)preload; argv[n] = NULL;
    if (G.con.proc.running) { proc_stop(&G.con.proc); } proc_close(&G.con.proc);
    if (!proc_start_ex(&G.con.proc, argv, G.root, 80, 24, NULL, false)) { console_print("cannot start the REPL", TH.error, 0); return; }
    G.con.started = true;
    char m[400]; snprintf(m, sizeof m, preload ? "KingCobra REPL (loaded %s)" : "KingCobra REPL - type expressions or statements", preload ? path_basename(preload) : "");
    console_print(m, TH.ui_dim, ATTR_DIM);
}
void console_restart(bool preload_current) {
    Doc *d = cur_doc(); const char *pre = (preload_current && d && d->path && d->lang == LANG_KC) ? d->path : NULL;
    if (pre && d->dirty) { char e[200]; doc_save(d, NULL, G.cfg.trim_ws, e, sizeof e); }
    console_start(pre);
}
void console_submit(const char *line) {
    char echo[600]; char lab[16]; console_prompt_label(lab, sizeof lab); snprintf(echo, sizeof echo, "%s%s", lab, line); console_print(echo, TH.ui_dim, 0);
    con_hist_add(line);
    if (G.dbg.state != DS_IDLE) {
        if (G.dbg.state == DS_PAUSED) { if (!dbg_eval_console(line)) console_print("cannot evaluate right now", TH.warn, 0); }
        else console_print("the program is running - pause it or hit a breakpoint to evaluate expressions", TH.warn, 0);
        return;
    }
    if (!G.con.started || !G.con.proc.running) console_start(NULL);
    if (G.con.started) { proc_write(&G.con.proc, line, (int)strlen(line)); proc_write(&G.con.proc, "\n", 1); }
}
static void console_key(Event *e) {
    Console *c = &G.con; int k = e->key, m = e->mods;
    if (k == K_ESC) { G.focus = FOCUS_EDITOR; return; }
    if (m & MOD_CTRL) {
        if (k == 'u') { c->input[0] = 0; c->ilen = c->icur = 0; } else if (k == 'l') console_clear(); else if (k == 'c') { c->input[0] = 0; c->ilen = c->icur = 0; }
        else if (k == 'a') c->icur = 0; else if (k == 'e') c->icur = c->ilen;
        else if (k == 'k') { c->input[c->icur] = 0; c->ilen = c->icur; }
        else if (k == 'w') { int p = c->icur; while (p > 0 && c->input[p - 1] == ' ') p--; while (p > 0 && c->input[p - 1] != ' ') p--; memmove(c->input + p, c->input + c->icur, (size_t)(c->ilen - c->icur + 1)); c->ilen -= c->icur - p; c->icur = p; }
        return;
    }
    switch (k) {
    case K_ENTER: { char *l = xstrdup(c->input); c->input[0] = 0; c->ilen = c->icur = 0; console_submit(l); free(l); c->log.follow = true; return; }
    case K_BKSP: if (c->icur > 0) { int p = utf8_prev(c->input, c->icur); memmove(c->input + p, c->input + c->icur, (size_t)(c->ilen - c->icur + 1)); c->ilen -= c->icur - p; c->icur = p; } return;
    case K_DEL: if (c->icur < c->ilen) { uint32_t cp; int n = utf8_decode(c->input + c->icur, (size_t)(c->ilen - c->icur), &cp); memmove(c->input + c->icur, c->input + c->icur + n, (size_t)(c->ilen - c->icur - n + 1)); c->ilen -= n; } return;
    case K_LEFT: if (c->icur > 0) c->icur = utf8_prev(c->input, c->icur); return;
    case K_RIGHT: if (c->icur < c->ilen) { uint32_t cp; c->icur += utf8_decode(c->input + c->icur, (size_t)(c->ilen - c->icur), &cp); } return;
    case K_HOME: c->icur = 0; return; case K_END: c->icur = c->ilen; return;
    case K_UP: if (c->nhist && c->hpos > 0) { c->hpos--; snprintf(c->input, sizeof c->input, "%s", c->hist[c->hpos]); c->ilen = c->icur = (int)strlen(c->input); } return;
    case K_DOWN: if (c->hpos < c->nhist - 1) { c->hpos++; snprintf(c->input, sizeof c->input, "%s", c->hist[c->hpos]); } else { c->hpos = c->nhist; c->input[0] = 0; } c->ilen = c->icur = (int)strlen(c->input); return;
    case K_PGUP: c->log.follow = false; c->log.scroll -= G.r_panel.h - 3; if (c->log.scroll < 0) c->log.scroll = 0; return;
    case K_PGDN: c->log.scroll += G.r_panel.h - 3; if (c->log.scroll >= c->log.n - (G.r_panel.h - 2)) c->log.follow = true; return;
    default: break;
    }
    if (k >= 32 && k < 0x200000 && !(m & MOD_ALT)) { char b[8]; int n = utf8_encode((uint32_t)k, b); if (c->ilen + n < (int)sizeof c->input - 1) { memmove(c->input + c->icur + n, c->input + c->icur, (size_t)(c->ilen - c->icur + 1)); memcpy(c->input + c->icur, b, (size_t)n); c->ilen += n; c->icur += n; } }
}
static void draw_console(Rect r) {
    Console *c = &G.con; int rows = r.h - 1; if (rows < 1) return;
    scr_fill(r.x, r.y, r.w, r.h, ST(TH.fg, TH.bg));
    if (c->log.follow) { c->log.scroll = c->log.n - rows; if (c->log.scroll < 0) c->log.scroll = 0; }
    if (c->log.scroll > c->log.n - 1) c->log.scroll = c->log.n - 1; if (c->log.scroll < 0) c->log.scroll = 0;
    for (int i = 0; i < rows && c->log.scroll + i < c->log.n; i++) {
        OLine *l = &c->log.lines[c->log.scroll + i]; int vx = 0;
        for (int k = 0; k < l->n && vx < r.w - 2; k++) { uint32_t ch = l->c[k].ch; int w = cp_width(ch); if (!w) continue; scr_putc(r.x + 1 + vx, r.y + i, ch, STA(l->c[k].fg, TH.bg, l->c[k].attr)); if (w == 2) scr_putc(r.x + 2 + vx, r.y + i, 0, ST(l->c[k].fg, TH.bg)); vx += w; }
    }
    int y = r.y + r.h - 1; char lab[16]; console_prompt_label(lab, sizeof lab);
    scr_fill(r.x, y, r.w, 1, ST(TH.fg, TH.line_bg)); int px = r.x + 1 + scr_text(r.x + 1, y, lab, 10, STA(G.dbg.state == DS_PAUSED ? TH.warn : TH.accent, TH.line_bg, ATTR_BOLD));
    int cols = 0; for (int i = 0; i < c->icur;) { uint32_t cp; i += utf8_decode(c->input + i, (size_t)(c->ilen - i), &cp); cols += cp_width(cp); }
    int avail = r.w - (px - r.x) - 1, start = 0; while (cols >= avail - 1 && start < c->icur) { uint32_t cp; int n = utf8_decode(c->input + start, (size_t)(c->ilen - start), &cp); start += n; cols -= cp_width(cp); }
    scr_textn(px, y, c->input + start, c->ilen - start, avail, ST(TH.fg, TH.line_bg));
    if (G.focus == FOCUS_PANEL && G.ptab == PT_CONSOLE && G.ov.kind == OV_NONE) scr_cursor(px + cols, y, true);
}

/* ---------------------------------------------------------------- tab strip */
typedef struct { int x0, x1, kind, arg; } Hit;
static Hit hits[40]; static int nhits;
static void add_hit(int x0, int x1, int kind, int arg) { if (nhits < 40) hits[nhits++] = (Hit){ x0, x1, kind, arg }; }
static int buttons(const char **lab, int *cmd, int max) {
    int n = 0;
    #define B(l, c) do { if (n < max) { lab[n] = l; cmd[n] = c; n++; } } while (0)
    switch (G.ptab) {
    case PT_OUTPUT: B("Clear", C_CLEAROUT); break;
    case PT_TERMINAL: B("+ New", C_NEWTERM); B("Kill", C_KILLTERM); break;
    case PT_CONSOLE: B("Clear", C_CONSOLE_CLEAR); B("Restart", C_CONSOLE_RESTART); B("Load file", C_CONSOLE_LOAD); break;
    case PT_DEBUG: n = dbg_header_buttons(lab, cmd, max); break;
    default: break;
    }
    B("Hide", C_PANEL);
    return n;
}
static void draw_header(void) {
    Rect r = G.r_panel; nhits = 0; Color bg = TH.menu_bg;
    scr_fill(r.x, r.y, r.w, 1, ST(TH.ui_fg, bg));
    int x = r.x + 1;
    for (int t = 0; t < PT_COUNT; t++) {
        bool act = G.ptab == t, foc = act && G.focus == FOCUS_PANEL;
        char lab[48]; const char *ind = "";
        if (t == PT_OUTPUT && G.proc.running) ind = " \xe2\x97\x8f"; if (t == PT_DEBUG && G.dbg.state != DS_IDLE) ind = G.dbg.state == DS_PAUSED ? " \xe2\x8f\xb8" : " \xe2\x97\x8f";
        snprintf(lab, sizeof lab, " %s%s ", TAB_NAMES[t], ind);
        Style s = act ? STA(foc ? TH.accent : TH.fg, TH.bg, ATTR_BOLD | ATTR_UNDERLINE) : ST(TH.ui_dim, bg);
        int w = scr_text(x, r.y, lab, 30, s); add_hit(x, x + w, 0, t); x += w;
        if (t == PT_TERMINAL && act && G.nterms > 1) for (int i = 0; i < G.nterms; i++) { char n[16]; snprintf(n, sizeof n, "[%d]", i + 1); int ww = scr_text(x, r.y, n, 6, STA(i == G.cterm ? TH.accent : TH.ui_dim, bg, i == G.cterm ? ATTR_BOLD : 0)); add_hit(x, x + ww, 2, i); x += ww + 1; }
        x += 1;
    }
    const char *lab[8]; int cmd[8]; int n = buttons(lab, cmd, 8); int rx = r.x + r.w - 1;
    for (int i = n - 1; i >= 0; i--) { char b[40]; snprintf(b, sizeof b, "[%s]", lab[i]); int w = (int)strlen(b) + (int)(strlen(lab[i]) == strlen(lab[i]) ? 0 : 0); for (const char *p = b; *p; p++) if (((unsigned char)*p & 0xC0) == 0x80) w--; rx -= w; scr_text(rx, r.y, b, 40, ST(i == n - 1 ? TH.ui_dim : TH.info, bg)); add_hit(rx, rx + w, 1, cmd[i]); rx -= 1; }
    if (G.focus == FOCUS_PANEL && (G.ptab == PT_TERMINAL) && rx - x > 26) scr_text(x + 1, r.y, "Alt+J: back to editor", 24, ST(TH.ui_dim, bg));
}

void panel_draw(void) {
    if (!G.show_panel || G.r_panel.h < 2) return;
    draw_header(); Rect c = content();
    switch (G.ptab) {
    case PT_OUTPUT: output_body_draw(c); break;
    case PT_TERMINAL: draw_terminal(c); break;
    case PT_CONSOLE: draw_console(c); break;
    case PT_DEBUG: dbg_panel_draw(c); break;
    default: break;
    }
}
void panel_set_tab(int tab, bool focus) {
    G.ptab = tab; if (!G.show_panel) panel_show(true); else ui_layout();
    if (focus) G.focus = FOCUS_PANEL;
    if (tab == PT_TERMINAL && !G.nterms && focus) term_new();
}
bool panel_reserved_key(const Event *e) {
    int k = e->key, m = e->mods;
    if (k >= K_FN(1) && k <= K_FN(12)) return true;
    if (m == MOD_ALT && (k == 'j' || k == 't' || k == 'c' || k == 'd' || k == 'm')) return true;
    if ((m & MOD_CTRL) && (k == K_PGUP || k == K_PGDN)) return true;
    return false;
}
/* true when the focused panel consumes this key itself (before the global key map) */
bool panel_wants_key(const Event *e) {
    if (G.focus != FOCUS_PANEL || !G.show_panel) return false;
    int k = e->key, m = e->mods;
    if (panel_reserved_key(e)) return false;
    switch (G.ptab) {
    case PT_TERMINAL: return true;
    case PT_CONSOLE: return !(m & MOD_ALT) && (!(m & MOD_CTRL) || k == 'u' || k == 'l' || k == 'c' || k == 'a' || k == 'e' || k == 'k' || k == 'w');
    case PT_OUTPUT: return G.proc.running && !(m & MOD_ALT) && !(m == MOD_CTRL && (k == 'q' || k == 'b' || k == 'j' || k == 'w' || k == 'n' || k == 'o' || k == 'p' || k == 's' || k == 'e'));
    default: return false;
    }
}
void panel_key(Event *e) {
    switch (G.ptab) {
    case PT_OUTPUT: output_key(e); break;
    case PT_TERMINAL: term_key(e); break;
    case PT_CONSOLE: console_key(e); break;
    case PT_DEBUG: dbg_panel_key(e); break;
    default: break;
    }
}
void panel_paste(const char *t, int n) {
    if (G.ptab == PT_TERMINAL) { Term *tm = cur_term(); if (!tm || tm->exited) return; if (tm->vt->bracketed) proc_write(&tm->proc, "\x1b[200~", 6); proc_write(&tm->proc, t, n); if (tm->vt->bracketed) proc_write(&tm->proc, "\x1b[201~", 6); }
    else if (G.ptab == PT_CONSOLE) { Console *c = &G.con; for (int i = 0; i < n && t[i] != '\n' && t[i] != '\r' && c->ilen < (int)sizeof c->input - 2; i++) { c->input[c->ilen++] = t[i]; } c->input[c->ilen] = 0; c->icur = c->ilen; }
    else if (G.ptab == PT_OUTPUT && G.proc.running) proc_write(&G.proc, t, n);
}
void panel_mouse(Event *e) {
    Rect r = G.r_panel, c = content(); int x = e->mx, y = e->my;
    if (y == r.y) {
        if (e->press && e->btn == 0 && !e->drag) for (int i = 0; i < nhits; i++) if (x >= hits[i].x0 && x < hits[i].x1) {
            if (hits[i].kind == 0) { G.ptab = hits[i].arg; G.focus = FOCUS_PANEL; if (G.ptab == PT_TERMINAL && !G.nterms) term_new(); }
            else if (hits[i].kind == 1) cmd_run(hits[i].arg);
            else if (hits[i].kind == 2) { G.cterm = hits[i].arg; G.focus = FOCUS_PANEL; }
            return;
        }
        if (e->press && e->btn == 0 && !e->drag) G.mouse_down = 10;        /* empty header space: drag to resize the panel */
        return;
    }
    if (e->press && e->btn == 0 && !e->drag) G.focus = FOCUS_PANEL;
    if (G.ptab == PT_TERMINAL) {
        Term *t = cur_term(); if (!t) return;
        if (e->btn == 4) { vt_scroll_view(t->vt, 3); return; } if (e->btn == 5) { vt_scroll_view(t->vt, -3); return; }
        int cx = x - c.x, cy = y - c.y; if (cx < 0) cx = 0; if (cx >= t->vt->w) cx = t->vt->w - 1; if (cy < 0) cy = 0; if (cy >= t->vt->h) cy = t->vt->h - 1;
        if (e->press && e->btn == 0 && !e->drag) { t->sel = false; t->sx = t->ex = cx; t->sy = t->ey = cy; G.mouse_down = 20; }
        else if (e->drag && G.mouse_down == 20) { t->ex = cx; t->ey = cy; t->sel = true; }
        else if (e->release && G.mouse_down == 20) { G.mouse_down = 0; if (t->sel && (t->sx != t->ex || t->sy != t->ey)) term_copy_selection(t); else t->sel = false; }
    } else if (G.ptab == PT_OUTPUT) { if (e->btn == 4) { G.out.follow = false; G.out.scroll -= 3; if (G.out.scroll < 0) G.out.scroll = 0; } else if (e->btn == 5) { G.out.scroll += 3; if (G.out.scroll >= G.out.n - (r.h - 1)) G.out.follow = true; } else if (e->press && e->btn == 0 && !e->drag) output_click(e); }
    else if (G.ptab == PT_CONSOLE) { if (e->btn == 4) { G.con.log.follow = false; G.con.log.scroll -= 3; if (G.con.log.scroll < 0) G.con.log.scroll = 0; } else if (e->btn == 5) { G.con.log.scroll += 3; if (G.con.log.scroll >= G.con.log.n - (r.h - 2)) G.con.log.follow = true; } }
    else if (G.ptab == PT_DEBUG) dbg_panel_mouse(e, c);
}

/* ---------------------------------------------------------------- lifecycle / polling */
void panel_init(void) {
    out_init(&G.con.log, G.th->fg); G.con.proc.fd = G.con.proc.wfd = -1; G.con.hpos = 0;
}
void panel_free(void) {
    for (int i = 0; i < G.nterms; i++) { proc_stop(&G.terms[i].proc); proc_close(&G.terms[i].proc); vt_free(G.terms[i].vt); }
    G.nterms = 0; if (G.con.proc.running) proc_stop(&G.con.proc); proc_close(&G.con.proc);
}
void panel_sync_sizes(void) {
    Rect c = content(); if (c.w < 2 || c.h < 1) return;
    for (int i = 0; i < G.nterms; i++) { Term *t = &G.terms[i]; if (t->vt->w != c.w || t->vt->h != c.h) { vt_resize(t->vt, c.w, c.h); proc_resize(&t->proc, c.w, c.h); } }
}
void panel_poll(void) {
    char buf[16384];
    for (int i = 0; i < G.nterms; i++) {
        Term *t = &G.terms[i]; if (t->exited) continue; int total = 0;
        for (;;) { int n = proc_read(&t->proc, buf, sizeof buf); if (n > 0) { vt_feed(t->vt, buf, n); total += n; if (total > 262144) break; continue; } if (n < 0) { proc_poll(&t->proc); } break; }
        if (!proc_poll(&t->proc) && t->proc.pid != 0) {
            for (;;) { int n = proc_read(&t->proc, buf, sizeof buf); if (n <= 0) break; vt_feed(t->vt, buf, n); }
            t->exited = true; t->code = t->proc.exit_code; char m[120]; snprintf(m, sizeof m, "\r\n\x1b[2m[process exited with code %d - press Enter to restart]\x1b[0m\r\n", t->code); vt_feed(t->vt, m, (int)strlen(m)); proc_close(&t->proc);
        }
    }
    if (G.con.started && (G.con.proc.running || G.con.proc.fd >= 0)) {
        int total = 0;
        for (;;) { int n = proc_read(&G.con.proc, buf, sizeof buf); if (n > 0) { out_feed(&G.con.log, buf, n); total += n; if (total > 262144) break; continue; } if (n < 0) { proc_poll(&G.con.proc); } break; }
        if (!proc_poll(&G.con.proc)) { for (;;) { int n = proc_read(&G.con.proc, buf, sizeof buf); if (n <= 0) break; out_feed(&G.con.log, buf, n); } proc_close(&G.con.proc); G.con.started = false; if (G.con.log.lines[G.con.log.n - 1].n) out_feed(&G.con.log, "\n", 1); console_print("[REPL ended - the next line starts a fresh session]", TH.ui_dim, ATTR_DIM); }
    }
}
int panel_fds(int *fds, int max) {
    int n = 0;
    for (int i = 0; i < G.nterms && n < max; i++) if (!G.terms[i].exited && G.terms[i].proc.fd >= 0) fds[n++] = G.terms[i].proc.fd;
    if (G.con.proc.fd >= 0 && n < max) fds[n++] = G.con.proc.fd;
    return n;
}
