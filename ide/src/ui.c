/* kcide — layout, menu bar, tabs, explorer, output panel, status bar, overlays and event dispatch. */
#include "ide.h"
#include <ctype.h>

extern int keymap_lookup(const Event *e);
extern const int N_CMDS;
static int g_psel = -1;                 /* selected output line */

/* ---------------------------------------------------------------- layout */
void panel_show(bool show) { G.show_panel = show; if (!show && G.focus == FOCUS_PANEL) G.focus = FOCUS_EDITOR; ui_layout(); }
void ui_layout(void) {
    int w = term_w, h = term_h;
    G.r_menu = (Rect){ 0, 0, w, 1 }; G.r_tabs = (Rect){ 0, 1, w, 1 }; G.r_status = (Rect){ 0, h - 1, w, 1 };
    int body_y = 2, body_h = h - 3; if (body_h < 3) body_h = 3;
    int ph = 0;
    if (G.show_panel) { ph = G.panel_h; if (ph < 4) ph = 4; if (ph > body_h - 3) ph = body_h - 3 > 4 ? body_h - 3 : 4; G.panel_h = ph; }
    G.r_panel = (Rect){ 0, body_y + body_h - ph, w, ph };
    int top_h = body_h - ph; if (top_h < 1) top_h = 1;
    int sbw = G.cfg.show_sidebar ? G.sb_w : 0; if (sbw > w / 2) sbw = w / 2; if (G.cfg.show_sidebar && sbw < 12) sbw = 12;
    G.r_tree = (Rect){ 0, body_y, sbw, top_h };
    int ex = sbw + (sbw ? 1 : 0); int fr = G.fs.open ? (G.fs.replace_mode ? 2 : 1) : 0; if (fr >= top_h) fr = 0;
    G.r_edit = (Rect){ ex, body_y, w - ex, top_h - fr };
    G.r_find = (Rect){ ex, body_y + top_h - fr, w - ex, fr };
    G.text_h = G.r_edit.h; G.text_w = G.r_edit.w - G.gutter_w - 1;
    G.need_redraw = true;
    panel_sync_sizes();
}

/* ---------------------------------------------------------------- explorer */
static void tnode_free(TNode *n) { if (!n) return; for (int i = 0; i < n->nkids; i++) tnode_free(n->kids[i]); free(n->kids); free(n->name); free(n->path); free(n); }
static TNode *find_old(TNode *n, const char *path) { if (!n) return NULL; if (path_eq(n->path, path)) return n; for (int i = 0; i < n->nkids; i++) { TNode *r = find_old(n->kids[i], path); if (r) return r; } return NULL; }
static void load_kids(TNode *n, TNode *old) {
    DirList dl; n->loaded = true; if (!dir_list(n->path, &dl)) return;
    for (int i = 0; i < dl.n; i++) {
        if (dl.v[i].name[0] == '.') continue;
        TNode *k = xcalloc(1, sizeof *k); k->name = xstrdup(dl.v[i].name); k->path = path_join(n->path, dl.v[i].name); k->dir = dl.v[i].dir; k->parent = n; k->depth = n->depth + 1;
        n->kids = xrealloc(n->kids, (size_t)(n->nkids + 1) * sizeof(TNode *)); n->kids[n->nkids++] = k;
        TNode *o = old ? find_old(old, k->path) : NULL;
        if (k->dir && o && o->open) { k->open = true; load_kids(k, old); }
    }
    dir_list_free(&dl);
}
static void flatten(TNode *n) { for (int i = 0; i < n->nkids; i++) { TNode *k = n->kids[i]; G.flat = xrealloc(G.flat, (size_t)(G.nflat + 1) * sizeof(TNode *)); G.flat[G.nflat++] = k; if (k->dir && k->open) flatten(k); } }
static void reflat(void) { free(G.flat); G.flat = NULL; G.nflat = 0; if (G.tree) flatten(G.tree); if (G.tree_sel >= G.nflat) G.tree_sel = G.nflat - 1; if (G.tree_sel < 0) G.tree_sel = 0; }
void tree_build(void) {
    TNode *old = G.tree; TNode *root = xcalloc(1, sizeof *root); root->name = xstrdup(path_basename(G.root)); root->path = xstrdup(G.root); root->dir = root->open = true; root->depth = -1;
    load_kids(root, old); G.tree = root; tnode_free(old); reflat();
}
void tree_reveal(const char *path) {
    if (!G.tree) return; size_t rl = strlen(G.root); if (strncmp(path, G.root, rl) != 0 || (path[rl] != '/' && path[rl] != '\\')) return;
    TNode *n = G.tree; const char *p = path + rl + 1; bool changed = false;
    while (*p) {
        const char *e = p; while (*e && *e != '/' && *e != '\\') e++;
        TNode *next = NULL; for (int i = 0; i < n->nkids; i++) if ((int)strlen(n->kids[i]->name) == (int)(e - p) && !strncmp(n->kids[i]->name, p, (size_t)(e - p))) next = n->kids[i];
        if (!next) break;
        if (next->dir && *e) { if (!next->open) { next->open = true; changed = true; } if (!next->loaded) load_kids(next, NULL); }
        n = next; p = *e ? e + 1 : e;
    }
    if (changed) reflat();
    for (int i = 0; i < G.nflat; i++) if (G.flat[i] == n) { G.tree_sel = i; if (i < G.tree_scroll) G.tree_scroll = i; if (i >= G.tree_scroll + G.r_tree.h - 1) G.tree_scroll = i - G.r_tree.h + 2; if (G.tree_scroll < 0) G.tree_scroll = 0; }
}
static void tree_toggle(TNode *n) { n->open = !n->open; if (n->open && !n->loaded) load_kids(n, NULL); reflat(); }
static bool file_has_diag(const char *p, int *sev) { int best = 9; for (int i = 0; i < G.out.ndiag; i++) if (G.out.diags[i].sev < 2 && path_eq(G.out.diags[i].file, p) && G.out.diags[i].sev < best) best = G.out.diags[i].sev; if (best == 9) return false; *sev = best; return true; }
static void draw_tree(void) {
    Rect r = G.r_tree; if (r.w <= 0) return;
    bool foc = G.focus == FOCUS_TREE;
    scr_fill(r.x, r.y, r.w, r.h, ST(TH.ui_fg, TH.ui_bg));
    char hd[200]; snprintf(hd, sizeof hd, " EXPLORER  %s", G.tree ? G.tree->name : "");
    scr_fill(r.x, r.y, r.w, 1, ST(foc ? TH.accent : TH.ui_dim, TH.ui_bg)); scr_text(r.x, r.y, hd, r.w, STA(foc ? TH.accent : TH.ui_dim, TH.ui_bg, ATTR_BOLD));
    int rows = r.h - 1; if (G.tree_scroll > G.nflat - rows) G.tree_scroll = G.nflat - rows; if (G.tree_scroll < 0) G.tree_scroll = 0;
    Doc *cd = cur_doc();
    for (int i = 0; i < rows && G.tree_scroll + i < G.nflat; i++) {
        TNode *n = G.flat[G.tree_scroll + i]; int y = r.y + 1 + i; bool sel = G.tree_scroll + i == G.tree_sel;
        Color bg = sel ? (foc ? TH.menu_sel : TH.border) : TH.ui_bg; scr_fill(r.x, y, r.w, 1, ST(TH.ui_fg, bg));
        int x = r.x + 1 + n->depth * 2; int sev;
        Color fg = n->dir ? TH.info : TH.ui_fg; if (!n->dir && !strcmp(path_ext(n->name), ".kc")) fg = TH.hl[HC_MODULE];
        if (!n->dir && file_has_diag(n->path, &sev)) fg = sev == 0 ? TH.error : TH.warn;
        if (cd && cd->path && path_eq(cd->path, n->path) && !sel) bg = TH.tab_off_bg;
        scr_fill(r.x, y, r.w, 1, ST(fg, bg));
        if (n->dir) scr_putc(x, y, n->open ? 0x25BE : 0x25B8, ST(TH.ui_dim, bg)); x += 2;
        scr_text(x, y, n->name, r.x + r.w - x - 1, STA(fg, bg, n->dir ? ATTR_BOLD : 0));
        if (!n->dir) { int ix = doc_find_open(n->path); if (ix >= 0 && G.docs[ix]->dirty) scr_putc(r.x + r.w - 2, y, 0x25CF, ST(TH.warn, bg)); }
    }
    for (int y = r.y; y < r.y + r.h; y++) scr_putc(r.x + r.w, y, 0x2502, ST(TH.border, TH.bg));
}
static void tree_key(Event *e) {
    int k = e->key; TNode *n = G.nflat ? G.flat[G.tree_sel] : NULL; int page = G.r_tree.h > 2 ? G.r_tree.h - 2 : 1;
    if (e->mods & (MOD_CTRL | MOD_ALT)) return;
    switch (k) {
    case K_UP: if (G.tree_sel > 0) G.tree_sel--; break; case K_DOWN: if (G.tree_sel + 1 < G.nflat) G.tree_sel++; break;
    case K_PGUP: G.tree_sel = G.tree_sel > page ? G.tree_sel - page : 0; break; case K_PGDN: G.tree_sel = G.tree_sel + page < G.nflat ? G.tree_sel + page : G.nflat - 1; break;
    case K_HOME: G.tree_sel = 0; break; case K_END: G.tree_sel = G.nflat - 1; break;
    case K_ENTER: case K_RIGHT: if (n) { if (n->dir) tree_toggle(n); else { if (doc_open_path(n->path, true)) G.focus = k == K_ENTER ? FOCUS_EDITOR : FOCUS_TREE; } } break;
    case K_LEFT: if (n) { if (n->dir && n->open) tree_toggle(n); else if (n->parent && n->parent != G.tree) for (int i = 0; i < G.nflat; i++) if (G.flat[i] == n->parent) G.tree_sel = i; } break;
    case K_ESC: case K_TAB: G.focus = FOCUS_EDITOR; break;
    case K_DEL: cmd_run(C_DELETE); break; case K_FN(5): cmd_run(C_REFRESH); break; case K_FN(2): cmd_run(C_RENAME); break;
    case 'n': cmd_run(C_NEWFILE); break; case 'N': cmd_run(C_NEWDIR); break; case 'r': cmd_run(C_RENAME); break;
    default: break;
    }
    if (G.tree_sel < G.tree_scroll) G.tree_scroll = G.tree_sel; if (G.tree_sel >= G.tree_scroll + G.r_tree.h - 1) G.tree_scroll = G.tree_sel - G.r_tree.h + 2;
}

/* ---------------------------------------------------------------- menus */
static const char *MENU_TITLES[] = { "File", "Edit", "Search", "View", "Run", "Debug", "Help" };
static const int MENU_ITEMS[][20] = {
    { C_NEW, C_OPEN, C_QUICKOPEN, 0, C_SAVE, C_SAVEAS, C_SAVEALL, 0, C_CLOSE, C_RELOAD, 0, C_QUIT, -1 },
    { C_UNDO, C_REDO, 0, C_CUT, C_COPY, C_PASTE, C_SELALL, 0, C_DUPLINE, C_DELLINE, C_COMMENT, C_INDENT, C_OUTDENT, C_MOVEUP, C_MOVEDOWN, C_FORMAT, C_COMPLETE, -1 },
    { C_FIND, C_REPLACE, C_FINDNEXT, C_FINDPREV, C_FINDPROJ, 0, C_GOTOLINE, C_GOTOSYM, C_GOTODEF, C_NAVBACK, C_NAVFWD, -1 },
    { C_PALETTE, 0, C_SIDEBAR, C_PANEL, C_TERMINAL, C_NEWTERM, C_CONSOLE, C_DEBUGPANEL, 0, C_THEME, C_WHITESPACE, C_LINENUMS, C_AUTOPAIRS, 0, C_NEXTTAB, C_PREVTAB, -1 },
    { C_RUN, C_RUNTERM, C_CHECK, C_TEST, C_TESTALL, C_LEAKS, C_REPL, C_STOP, 0, C_NEXTDIAG, C_PREVDIAG, C_CLEAROUT, C_CHECKSAVE, -1 },
    { C_DEBUG, C_DBG_CONT, C_DBG_OVER, C_DBG_IN, C_DBG_OUT, C_DBG_PAUSE, C_DBG_STOP, 0, C_TOGGLE_BP, C_CLEAR_BPS, C_ADD_WATCH, -1 },
    { C_KEYS, C_ABOUT, -1 },
};
#define N_MENUS 7
static const CmdInfo *cmd_info(int id) { for (int i = 0; i < N_CMDS; i++) if (CMDS[i].id == id) return &CMDS[i]; return NULL; }
static const char *short_label(const char *l) { const char *c = strstr(l, ": "); return c ? c + 2 : l; }
static int menu_x(int m) { int x = 1; for (int i = 0; i < m; i++) x += (int)strlen(MENU_TITLES[i]) + 3; return x; }
static int menu_count(int m) { int n = 0; while (MENU_ITEMS[m][n] != -1) n++; return n; }
static Rect menu_rect(int m) {
    int n = menu_count(m), w = 20; for (int i = 0; i < n; i++) { const CmdInfo *c = cmd_info(MENU_ITEMS[m][i]); if (c) { int l = (int)strlen(short_label(c->label)) + (int)strlen(c->keys) + 6; if (l > w) w = l; } }
    return (Rect){ menu_x(m) - 1, 1, w, n + 2 };
}
static void draw_menubar(void) {
    scr_fill(0, 0, term_w, 1, ST(TH.ui_fg, TH.ui_bg));
    for (int m = 0; m < N_MENUS; m++) {
        bool open = G.ov.kind == OV_MENU && G.ov.menu == m; int x = menu_x(m) - 1;
        Style s = open ? STA(TH.fg, TH.menu_sel, ATTR_BOLD) : ST(TH.ui_fg, TH.ui_bg);
        scr_text(x, 0, " ", 1, s); scr_putc(x + 1, 0, (uint32_t)MENU_TITLES[m][0], STA(open ? TH.fg : TH.accent, open ? TH.menu_sel : TH.ui_bg, ATTR_UNDERLINE | ATTR_BOLD));
        scr_text(x + 2, 0, MENU_TITLES[m] + 1, 20, s); scr_text(x + 2 + (int)strlen(MENU_TITLES[m]) - 1, 0, " ", 1, s);
    }
    char r[200]; snprintf(r, sizeof r, "KingCobra IDE %s ", KCIDE_VERSION); scr_text(term_w - (int)strlen(r), 0, r, 60, STA(TH.accent, TH.ui_bg, ATTR_BOLD));
}
static void draw_menu_dropdown(void) {
    if (G.ov.kind != OV_MENU) return; int m = G.ov.menu; Rect r = menu_rect(m);
    scr_fill(r.x, r.y, r.w, r.h, ST(TH.ui_fg, TH.menu_bg));
    for (int x = r.x; x < r.x + r.w; x++) { scr_putc(x, r.y, 0x2500, ST(TH.border, TH.menu_bg)); scr_putc(x, r.y + r.h - 1, 0x2500, ST(TH.border, TH.menu_bg)); }
    for (int y = r.y; y < r.y + r.h; y++) { scr_putc(r.x, y, y == r.y ? 0x250C : y == r.y + r.h - 1 ? 0x2514 : 0x2502, ST(TH.border, TH.menu_bg)); scr_putc(r.x + r.w - 1, y, y == r.y ? 0x2510 : y == r.y + r.h - 1 ? 0x2518 : 0x2502, ST(TH.border, TH.menu_bg)); }
    int n = menu_count(m);
    for (int i = 0; i < n; i++) {
        int id = MENU_ITEMS[m][i], y = r.y + 1 + i;
        if (id == 0) { for (int x = r.x + 1; x < r.x + r.w - 1; x++) scr_putc(x, y, 0x2500, ST(TH.border, TH.menu_bg)); continue; }
        const CmdInfo *c = cmd_info(id); bool sel = G.ov.mitem == i; Color bg = sel ? TH.menu_sel : TH.menu_bg;
        scr_fill(r.x + 1, y, r.w - 2, 1, ST(TH.ui_fg, bg));
        scr_text(r.x + 2, y, short_label(c->label), r.w - 4, STA(TH.fg, bg, sel ? ATTR_BOLD : 0));
        int kl = (int)strlen(c->keys); scr_text(r.x + r.w - 2 - kl, y, c->keys, kl, ST(TH.ui_dim, bg));
    }
}
void ui_open_menu(int m);
static void open_menu(int m) { ov_close(); G.ov.kind = OV_MENU; G.ov.menu = m; G.ov.mitem = 0; while (MENU_ITEMS[m][G.ov.mitem] == 0) G.ov.mitem++; }

/* ---------------------------------------------------------------- tabs */
static int tab_width(Doc *d) { char b[80]; snprintf(b, sizeof b, "%.26s", d->title); int w = 0; for (const char *p = b; *p;) { uint32_t cp; p += utf8_decode(p, strlen(p), &cp); w += cp_width(cp); } return w + 5; }
static void tab_layout(void) {
    if (G.active < G.tab_scroll) G.tab_scroll = G.active;
    for (;;) { int w = 0; for (int i = G.tab_scroll; i <= G.active && i < G.ndocs; i++) w += tab_width(G.docs[i]); if (w > G.r_tabs.w - 2 && G.tab_scroll < G.active) G.tab_scroll++; else break; }
    if (G.tab_scroll < 0) G.tab_scroll = 0;
}
static void draw_tabs(void) {
    scr_fill(0, 1, term_w, 1, ST(TH.ui_fg, TH.tab_off_bg)); tab_layout(); int x = 0;
    if (G.tab_scroll > 0) { scr_putc(0, 1, 0x25C0, ST(TH.ui_dim, TH.tab_off_bg)); x = 1; }
    for (int i = G.tab_scroll; i < G.ndocs; i++) {
        Doc *d = G.docs[i]; int w = tab_width(d); bool act = i == G.active; if (x + w > term_w) { scr_putc(term_w - 1, 1, 0x25B6, ST(TH.ui_dim, TH.tab_off_bg)); break; }
        Color bg = act ? TH.tab_on_bg : TH.tab_off_bg; scr_fill(x, 1, w, 1, ST(TH.ui_fg, bg));
        char b[80]; snprintf(b, sizeof b, "%.26s", d->title);
        scr_text(x + 1, 1, b, w - 4, STA(act ? TH.fg : TH.ui_dim, bg, act ? ATTR_BOLD : 0));
        scr_putc(x + w - 2, 1, d->dirty ? 0x25CF : 0xD7, ST(d->dirty ? TH.warn : TH.ui_dim, bg));
        if (act) for (int k = 0; k < w; k++) { Cell *c = scr_cell(x + k, 1); if (c) c->attr |= ATTR_UNDERLINE; }
        if (act) scr_restyle(x, 1, 0, 0, KEEP, KEEP);
        x += w;
    }
}
static int tab_hit(int mx, bool *on_close) {
    int x = G.tab_scroll > 0 ? 1 : 0; *on_close = false;
    for (int i = G.tab_scroll; i < G.ndocs; i++) { int w = tab_width(G.docs[i]); if (mx >= x && mx < x + w) { *on_close = mx >= x + w - 3; return i; } x += w; }
    return -1;
}

/* ---------------------------------------------------------------- output panel */
void output_body_draw(Rect r) {
    bool foc = G.focus == FOCUS_PANEL; int rows = r.h; if (rows < 1) return;
    if (G.out.follow) { G.out.scroll = G.out.n - rows; if (G.out.scroll < 0) G.out.scroll = 0; }
    if (G.out.scroll > G.out.n - 1) G.out.scroll = G.out.n - 1; if (G.out.scroll < 0) G.out.scroll = 0;
    scr_fill(r.x, r.y, r.w, rows, ST(TH.ui_fg, TH.bg));
    int cur_line_y = -1, cur_line_x = 0;
    for (int i = 0; i < rows && G.out.scroll + i < G.out.n; i++) {
        OLine *l = &G.out.lines[G.out.scroll + i]; int y = r.y + i; bool sel = foc && G.out.scroll + i == g_psel && !G.proc.running;
        if (sel) scr_fill(r.x, y, r.w, 1, ST(TH.fg, TH.menu_sel));
        Color bg = sel ? TH.menu_sel : TH.bg;
        int vx = 0; for (int k = 0; k < l->n && vx < r.w - 1; k++) { uint32_t ch = l->c[k].ch; int w = cp_width(ch); if (!w) continue; scr_putc(r.x + 1 + vx, y, ch, STA(l->c[k].fg, bg, l->c[k].attr)); if (w == 2) scr_putc(r.x + 2 + vx, y, 0, ST(l->c[k].fg, bg)); vx += w; }
        if (l->diag >= 0 && G.out.diags[l->diag].sev < 2) scr_putc(r.x, y, 0x25B8, ST(G.out.diags[l->diag].sev == 0 ? TH.error : TH.warn, bg));
        if (G.out.scroll + i == G.out.n - 1) { cur_line_y = y; cur_line_x = r.x + 1 + vx; }
    }
    if (foc && G.proc.running && cur_line_y >= 0 && G.ov.kind == OV_NONE) scr_cursor(cur_line_x, cur_line_y, true);
}
void output_key(Event *e) {
    int k = e->key, rows = G.r_panel.h - 1;
    if (G.proc.running) {
        if (k == K_ESC) { G.focus = FOCUS_EDITOR; return; }
        char b[8]; int n = 0;
        if (e->mods & MOD_CTRL) { if (k >= 'a' && k <= 'z') { b[0] = (char)(k - 'a' + 1); n = 1; } }
        else if (e->mods & MOD_ALT) return;
        else if (k == K_ENTER) {
#ifdef _WIN32
            b[0] = '\r'; n = 1;
#else
            b[0] = '\n'; n = 1;
#endif
        }
        else if (k == K_BKSP) { b[0] = 0x7f; n = 1; }
        else if (k == K_TAB) { b[0] = '\t'; n = 1; }
        else if (k >= 32 && k < 0x200000) n = utf8_encode((uint32_t)k, b);
        if (n) proc_write(&G.proc, b, n);
        return;
    }
    switch (k) {
    case K_ESC: G.focus = FOCUS_EDITOR; break;
    case K_UP: if (g_psel < 0) g_psel = G.out.n - 1; else if (g_psel > 0) g_psel--; G.out.follow = false; break;
    case K_DOWN: if (g_psel + 1 < G.out.n) g_psel++; break;
    case K_PGUP: g_psel = g_psel > rows ? g_psel - rows : 0; G.out.follow = false; break; case K_PGDN: g_psel = g_psel + rows < G.out.n ? g_psel + rows : G.out.n - 1; break;
    case K_HOME: g_psel = 0; G.out.follow = false; break; case K_END: g_psel = G.out.n - 1; G.out.follow = true; break;
    case K_ENTER: if (g_psel >= 0 && g_psel < G.out.n && G.out.lines[g_psel].diag >= 0) focus_diag_index(G.out.lines[g_psel].diag); break;
    default: break;
    }
    if (g_psel >= 0) { if (g_psel < G.out.scroll) { G.out.scroll = g_psel; G.out.follow = false; } if (g_psel >= G.out.scroll + rows) G.out.scroll = g_psel - rows + 1; }
}
void output_click(Event *e) {
    G.focus = FOCUS_PANEL; int row = G.out.scroll + (e->my - G.r_panel.y - 1);
    if (row >= 0 && row < G.out.n) { g_psel = row; if (G.out.lines[row].diag >= 0) focus_diag_index(G.out.lines[row].diag); }
}

/* ---------------------------------------------------------------- status bar */
static void draw_status(void) {
    Rect r = G.r_status; Doc *d = cur_doc();
    scr_fill(r.x, r.y, r.w, 1, ST(TH.status_fg, TH.status_bg));
    bool msg = now_ms() < G.msg_until && G.msg[0];
    Color mc = G.msg_kind == 3 ? TH.error : G.msg_kind == 2 ? TH.warn : TH.status_fg;
    char right[256] = "";
    int errs = 0, warns = 0; for (int i = 0; i < G.out.ndiag; i++) { if (G.out.diags[i].sev == 0) errs++; else if (G.out.diags[i].sev == 1) warns++; }
    if (d) snprintf(right, sizeof right, "Ln %d, Col %d  %s%s  %s  %s  %s  \xe2\x9c\x96 %d \xe2\x9a\xa0 %d ", d->cur.line + 1, doc_vcol(d, d->cur.line, d->cur.col, G.cfg.tab_width) + 1,
        d->has_sel && pos_cmp(d->anchor, d->cur) ? "(sel) " : "", G.cfg.use_spaces ? "Spaces" : "Tabs", d->crlf ? "CRLF" : "LF", "UTF-8", lang_name(d->lang), errs, warns);
    else snprintf(right, sizeof right, "\xe2\x9c\x96 %d \xe2\x9a\xa0 %d ", errs, warns);
    int rl = 0; for (const char *p = right; *p;) { uint32_t cp; p += utf8_decode(p, strlen(p), &cp); rl += cp_width(cp); }
    scr_text(r.w - rl, r.y, right, rl, ST(TH.status_fg, TH.status_bg));
    if (msg) { Color bg = G.msg_kind == 3 ? 0x7a1f1f : G.msg_kind == 2 ? 0x6b5200 : TH.status_bg; scr_fill(0, r.y, r.w - rl, 1, ST(TH.status_fg, bg)); scr_text(1, r.y, G.msg, r.w - rl - 2, STA(0xffffff, bg, ATTR_BOLD)); (void)mc; }
    else { char l[300]; snprintf(l, sizeof l, " %s%s", d ? (d->path ? d->path : d->title) : "No file open", !G.kc_found ? "   (kingcobra not found: set KINGCOBRA)" : ""); scr_text(0, r.y, l, r.w - rl - 1, ST(TH.status_fg, TH.status_bg)); }
}

/* ---------------------------------------------------------------- overlays */
static void ov_free(void) {
    for (int i = 0; i < G.ov.nitems; i++) { free(G.ov.items[i].label); free(G.ov.items[i].detail); }
    free(G.ov.items); free(G.ov.view); memset(&G.ov, 0, sizeof G.ov);
}
void ov_close(void) { ov_free(); }
static void ov_cancel(void) { void (*cb)(const char *, intptr_t, void *) = G.ov.on_accept; void *ctx = G.ov.ctx; int kind = G.ov.kind; ov_free(); if (kind == OV_CONFIRM && cb) cb("", -1, ctx); else if (kind == OV_PROMPT && cb == NULL) {} }
static void picker_refilter(void) {
    Overlay *o = &G.ov; o->nview = 0; o->view = xrealloc(o->view, (size_t)(o->nitems + 1) * sizeof(int));
    for (int i = 0; i < o->nitems; i++) {
        if (!o->input[0]) { o->items[i].score = 0; o->view[o->nview++] = i; continue; }
        char *hay = o->items[i].detail ? xfmt("%s %s", o->items[i].label, o->items[i].detail) : xstrdup(o->items[i].label);
        int sc = fuzzy_score(o->input, hay); free(hay); o->items[i].score = sc; if (sc >= 0) o->view[o->nview++] = i;
    }
    if (o->input[0]) { for (int i = 1; i < o->nview; i++) { int v = o->view[i], j = i - 1; while (j >= 0 && o->items[o->view[j]].score < o->items[v].score) { o->view[j + 1] = o->view[j]; j--; } o->view[j + 1] = v; } }
    o->sel = 0; o->scroll = 0;
}
void ov_prompt(const char *title, const char *init, bool path_mode, void (*cb)(const char *, intptr_t, void *), void *ctx) {
    ov_free(); G.ov.kind = OV_PROMPT; snprintf(G.ov.title, sizeof G.ov.title, "%s", title); snprintf(G.ov.input, sizeof G.ov.input, "%s", init); G.ov.ilen = (int)strlen(G.ov.input); G.ov.icur = G.ov.ilen;
    G.ov.on_accept = cb; G.ov.ctx = ctx; G.ov.path_mode = path_mode;
}
void ov_picker(const char *title, PItem *items, int n, bool live, void (*cb)(const char *, intptr_t, void *), void *ctx) {
    ov_free(); G.ov.kind = OV_PICKER; snprintf(G.ov.title, sizeof G.ov.title, "%s", title); G.ov.items = items; G.ov.nitems = n; G.ov.on_accept = cb; G.ov.ctx = ctx; G.ov.live_filter = live; picker_refilter();
}
void ov_confirm(const char *msg, const char *b0, const char *b1, const char *b2, void (*cb)(const char *, intptr_t, void *), void *ctx) {
    ov_free(); G.ov.kind = OV_CONFIRM; snprintf(G.ov.msg, sizeof G.ov.msg, "%s", msg); G.ov.btns[0] = b0; G.ov.btns[1] = b1; G.ov.btns[2] = b2; G.ov.nbtns = b2 ? 3 : b1 ? 2 : 1; G.ov.btn = 0; G.ov.on_accept = cb; G.ov.ctx = ctx;
}
static Rect ov_box(void) {
    Overlay *o = &G.ov; int w = term_w * 3 / 4; if (w > 90) w = 90; if (w < 30) w = term_w - 2;
    if (o->kind == OV_PROMPT) { Rect r = { (term_w - w) / 2, term_h / 4, w, 4 }; return r; }
    if (o->kind == OV_PICKER) { int rows = o->nview < 14 ? o->nview : 14; if (rows < 1) rows = 1; Rect r = { (term_w - w) / 2, term_h / 6, w, rows + 3 }; return r; }
    int mw = (int)strlen(o->msg) + 6; if (mw > term_w - 4) mw = term_w - 4; if (mw < 36) mw = 36; Rect r = { (term_w - mw) / 2, term_h / 3, mw, 5 }; return r;
}
static void draw_box(Rect r, const char *title) {
    scr_fill(r.x, r.y, r.w, r.h, ST(TH.ui_fg, TH.menu_bg));
    for (int x = r.x; x < r.x + r.w; x++) { scr_putc(x, r.y, 0x2500, ST(TH.accent, TH.menu_bg)); scr_putc(x, r.y + r.h - 1, 0x2500, ST(TH.accent, TH.menu_bg)); }
    for (int y = r.y; y < r.y + r.h; y++) { scr_putc(r.x, y, y == r.y ? 0x250C : y == r.y + r.h - 1 ? 0x2514 : 0x2502, ST(TH.accent, TH.menu_bg)); scr_putc(r.x + r.w - 1, y, y == r.y ? 0x2510 : y == r.y + r.h - 1 ? 0x2518 : 0x2502, ST(TH.accent, TH.menu_bg)); }
    char t[200]; snprintf(t, sizeof t, " %s ", title); scr_text(r.x + 2, r.y, t, r.w - 4, STA(TH.accent, TH.menu_bg, ATTR_BOLD));
}
static void draw_input(int x, int y, int w) {
    Overlay *o = &G.ov; scr_fill(x, y, w, 1, ST(TH.fg, TH.bg));
    int start = 0, cols = 0; for (int i = 0; i < o->icur;) { uint32_t cp; i += utf8_decode(o->input + i, (size_t)(o->ilen - i), &cp); cols += cp_width(cp); }
    while (cols >= w - 1 && start < o->icur) { uint32_t cp; int k = utf8_decode(o->input + start, (size_t)(o->ilen - start), &cp); start += k; cols -= cp_width(cp); }
    scr_textn(x, y, o->input + start, o->ilen - start, w - 1, ST(TH.fg, TH.bg)); scr_cursor(x + cols, y, true);
}
static void draw_overlay(void) {
    Overlay *o = &G.ov; if (o->kind == OV_NONE || o->kind == OV_MENU) return;
    Rect b = ov_box();
    if (o->kind == OV_PROMPT) { draw_box(b, o->title); draw_input(b.x + 2, b.y + 2, b.w - 4); if (o->path_mode) scr_text(b.x + 2, b.y + 1, "Tab completes paths", b.w - 4, ST(TH.ui_dim, TH.menu_bg)); }
    else if (o->kind == OV_PICKER) {
        char t[220]; snprintf(t, sizeof t, "%s  (%d/%d)", o->title, o->nview, o->nitems); draw_box(b, t); draw_input(b.x + 2, b.y + 1, b.w - 4);
        int rows = b.h - 3; if (o->sel < o->scroll) o->scroll = o->sel; if (o->sel >= o->scroll + rows) o->scroll = o->sel - rows + 1;
        for (int i = 0; i < rows && o->scroll + i < o->nview; i++) {
            PItem *it = &o->items[o->view[o->scroll + i]]; bool sel = o->scroll + i == o->sel; Color bg = sel ? TH.popup_sel : TH.menu_bg; int y = b.y + 2 + i;
            scr_fill(b.x + 1, y, b.w - 2, 1, ST(TH.fg, bg));
            int dl = it->detail ? (int)strlen(it->detail) : 0; if (dl > b.w / 2) dl = b.w / 2;
            scr_text(b.x + 2, y, it->label, b.w - 6 - dl, STA(TH.fg, bg, sel ? ATTR_BOLD : 0));
            if (it->detail) scr_text(b.x + b.w - 2 - dl, y, it->detail, dl, ST(TH.ui_dim, bg));
        }
        if (!o->nview) scr_text(b.x + 2, b.y + 2, "no matches", b.w - 4, ST(TH.ui_dim, TH.menu_bg));
    } else if (o->kind == OV_CONFIRM) {
        draw_box(b, "Confirm"); scr_text(b.x + 3, b.y + 1, o->msg, b.w - 6, ST(TH.fg, TH.menu_bg));
        int tot = 0; for (int i = 0; i < o->nbtns; i++) tot += (int)strlen(o->btns[i]) + 4; int x = b.x + (b.w - tot) / 2;
        for (int i = 0; i < o->nbtns; i++) { char l[80]; snprintf(l, sizeof l, "[ %s ]", o->btns[i]); bool sel = i == o->btn; x += scr_text(x, b.y + 3, l, 60, STA(sel ? TH.fg : TH.ui_fg, sel ? TH.menu_sel : TH.menu_bg, sel ? ATTR_BOLD : 0)) + 1; }
    }
}
static void inp_insert(const char *t, int n) { Overlay *o = &G.ov; if (o->ilen + n >= (int)sizeof o->input - 1) return; memmove(o->input + o->icur + n, o->input + o->icur, (size_t)(o->ilen - o->icur + 1)); memcpy(o->input + o->icur, t, (size_t)n); o->ilen += n; o->icur += n; }
static bool inp_key(Event *e) {
    Overlay *o = &G.ov; int k = e->key;
    if (e->mods & MOD_CTRL) {
        if (k == 'u') { o->input[0] = 0; o->ilen = o->icur = 0; return true; }
        if (k == 'a') { o->icur = 0; return true; } if (k == 'e') { o->icur = o->ilen; return true; }
        if (k == 'w') { int p = o->icur; while (p > 0 && o->input[p - 1] == ' ') p--; while (p > 0 && o->input[p - 1] != ' ' && o->input[p - 1] != '/') p--; memmove(o->input + p, o->input + o->icur, (size_t)(o->ilen - o->icur + 1)); o->ilen -= o->icur - p; o->icur = p; return true; }
        return false;
    }
    if (e->mods & MOD_ALT) return false;
    if (k == K_BKSP) { if (o->icur > 0) { int p = utf8_prev(o->input, o->icur); memmove(o->input + p, o->input + o->icur, (size_t)(o->ilen - o->icur + 1)); o->ilen -= o->icur - p; o->icur = p; } return true; }
    if (k == K_DEL) { if (o->icur < o->ilen) { uint32_t cp; int n = utf8_decode(o->input + o->icur, (size_t)(o->ilen - o->icur), &cp); memmove(o->input + o->icur, o->input + o->icur + n, (size_t)(o->ilen - o->icur - n + 1)); o->ilen -= n; } return true; }
    if (k == K_LEFT) { if (o->icur > 0) o->icur = utf8_prev(o->input, o->icur); return true; }
    if (k == K_RIGHT) { if (o->icur < o->ilen) { uint32_t cp; o->icur += utf8_decode(o->input + o->icur, (size_t)(o->ilen - o->icur), &cp); } return true; }
    if (k == K_HOME) { o->icur = 0; return true; } if (k == K_END) { o->icur = o->ilen; return true; }
    if (k >= 32 && k < 0x200000) { char b[8]; int n = utf8_encode((uint32_t)k, b); inp_insert(b, n); return true; }
    return false;
}
static void path_complete(void) {
    Overlay *o = &G.ov; char *in = xstrdup(o->input); char *slash = NULL; for (char *p = in; *p; p++) if (*p == '/' || *p == '\\') slash = p;
    char *dir = slash ? xstrndup(in, (size_t)(slash - in + 1)) : xstrdup(""); const char *pre = slash ? slash + 1 : in;
    char *scan = dir[0] ? xstrdup(dir) : xstrdup(G.root); DirList dl; char common[256] = ""; int nm = 0; bool one_dir = false;
    if (dir_list(scan, &dl)) {
        for (int i = 0; i < dl.n; i++) { if (strncmp(dl.v[i].name, pre, strlen(pre))) continue;
            if (!nm) { snprintf(common, sizeof common, "%s", dl.v[i].name); one_dir = dl.v[i].dir; } else { int k = 0; while (common[k] && common[k] == dl.v[i].name[k]) k++; common[k] = 0; one_dir = false; } nm++; }
        dir_list_free(&dl);
    }
    if (nm) { char *res = xfmt("%s%s%s", dir, common, nm == 1 && one_dir ? (const char[]){ PATH_SEP, 0 } : ""); snprintf(o->input, sizeof o->input, "%s", res); o->ilen = o->icur = (int)strlen(o->input); free(res); }
    free(in); free(dir); free(scan);
}
static void ov_key(Event *e) {
    Overlay *o = &G.ov; int k = e->key;
    if (o->kind == OV_MENU) {
        int n = menu_count(o->menu);
        if (k == K_ESC || k == K_FN(10)) { ov_free(); return; }
        if (k == K_LEFT) { open_menu((o->menu + N_MENUS - 1) % N_MENUS); return; } if (k == K_RIGHT) { open_menu((o->menu + 1) % N_MENUS); return; }
        if (k == K_UP || k == K_DOWN) { int d = k == K_UP ? -1 : 1; do { o->mitem = (o->mitem + d + n) % n; } while (MENU_ITEMS[o->menu][o->mitem] == 0); return; }
        if (k == K_ENTER) { int id = MENU_ITEMS[o->menu][o->mitem]; ov_free(); if (id) cmd_run(id); return; }
        return;
    }
    if (o->kind == OV_CONFIRM) {
        if (k == K_ESC) { ov_cancel(); return; }
        if (k == K_LEFT || (k == K_TAB && (e->mods & MOD_SHIFT))) { o->btn = (o->btn + o->nbtns - 1) % o->nbtns; return; }
        if (k == K_RIGHT || k == K_TAB) { o->btn = (o->btn + 1) % o->nbtns; return; }
        if (k == K_ENTER) { void (*cb)(const char *, intptr_t, void *) = o->on_accept; void *ctx = o->ctx; int b = o->btn; const char *l = o->btns[b]; ov_free(); if (cb) cb(l, b, ctx); return; }
        if (k < 128 && isalpha(k)) for (int i = 0; i < o->nbtns; i++) if (tolower((unsigned char)o->btns[i][0]) == k) { o->btn = i; Event en = { .type = EV_KEY, .key = K_ENTER }; ov_key(&en); return; }
        return;
    }
    if (k == K_ESC) { ov_cancel(); return; }
    if (o->kind == OV_PROMPT) {
        if (k == K_ENTER) { void (*cb)(const char *, intptr_t, void *) = o->on_accept; void *ctx = o->ctx; char *t = xstrdup(o->input); ov_free(); if (cb) cb(t, 0, ctx); free(t); return; }
        if (k == K_TAB && o->path_mode) { path_complete(); return; }
        inp_key(e); return;
    }
    if (o->kind == OV_PICKER) {
        int rows = 14;
        if (k == K_ENTER) { if (!o->nview) return; void (*cb)(const char *, intptr_t, void *) = o->on_accept; void *ctx = o->ctx; intptr_t id = o->items[o->view[o->sel]].id; char *t = xstrdup(o->input); ov_free(); if (cb) cb(t, id, ctx); free(t); return; }
        if (k == K_DOWN || (k == K_TAB && !(e->mods & MOD_SHIFT))) { if (o->nview) o->sel = (o->sel + 1) % o->nview; return; }
        if (k == K_UP || k == K_TAB) { if (o->nview) o->sel = (o->sel + o->nview - 1) % o->nview; return; }
        if (k == K_PGDN) { o->sel = o->sel + rows < o->nview ? o->sel + rows : o->nview - 1; if (o->sel < 0) o->sel = 0; return; }
        if (k == K_PGUP) { o->sel = o->sel > rows ? o->sel - rows : 0; return; }
        if (inp_key(e)) picker_refilter();
    }
}

/* ---------------------------------------------------------------- drawing & events */
void ui_draw(void) {
    scr_clear(ST(TH.fg, TH.bg)); scr_cursor(0, 0, false);
    draw_tree(); ed_draw(cur_doc()); panel_draw(); draw_tabs(); draw_menubar(); draw_status();
    draw_menu_dropdown(); draw_overlay();
}
static void paste_event(Event *e) {
    if (!e->paste) return;
    int n = (int)strlen(e->paste);
    if (G.ov.kind == OV_PROMPT || G.ov.kind == OV_PICKER) { for (int i = 0; i < n; i++) if (e->paste[i] == '\n' || e->paste[i] == '\r') n = i; inp_insert(e->paste, n); if (G.ov.kind == OV_PICKER) picker_refilter(); }
    else if (G.fs.open && G.fs.field >= 0) { char *buf = G.fs.field == 0 ? G.fs.find : G.fs.repl; int len = (int)strlen(buf); for (int i = 0; i < n; i++) if (e->paste[i] == '\n') n = i; if (len + n < 255) { memcpy(buf + len, e->paste, (size_t)n); buf[len + n] = 0; if (G.fs.field == 0) { find_recount(); find_step(1, true); } } }
    else if (G.focus == FOCUS_PANEL) panel_paste(e->paste, n);
    else { Doc *d = cur_doc(); if (d) ed_paste_text(d, e->paste, n); }
    free(e->paste); e->paste = NULL;
}
static void handle_key(Event *e) {
    Doc *d = cur_doc(); int k = e->key, m = e->mods;
    if (G.ov.kind != OV_NONE) { ov_key(e); return; }
    if (panel_wants_key(e)) { panel_key(e); return; }
    if (G.focus == FOCUS_EDITOR && d && G.fs.open && G.fs.field >= 0 && find_key(e)) return;
    int cmd = keymap_lookup(e);
    if (cmd != C_NONE) {
        if (cmd == C_FIND && G.fs.open) { G.fs.field = 0; G.fs.replace_mode = false; ui_layout(); }
        if (cmd == C_REPLACE && G.fs.open) { G.fs.field = 1; G.fs.replace_mode = true; ui_layout(); }
        if ((cmd == C_FIND || cmd == C_REPLACE) && !G.fs.open) { G.focus = FOCUS_EDITOR; }
        if (!((cmd == C_FIND || cmd == C_REPLACE) && G.fs.open)) cmd_run(cmd);
        if (G.focus == FOCUS_EDITOR && G.fs.open && (cmd == C_FIND || cmd == C_REPLACE)) G.fs.field = cmd == C_REPLACE ? 1 : 0;
        return;
    }
    if ((m & MOD_ALT) && !(m & ~MOD_ALT)) {
        if (k >= '1' && k <= '9') { if (k - '1' < G.ndocs) doc_activate(k - '1'); return; }
        const char *mn = "fesvrbh"; for (int i = 0; i < 7; i++) if (k == mn[i]) { open_menu(i); return; }
    }
    if (G.focus == FOCUS_TREE) { tree_key(e); return; }
    if (G.focus == FOCUS_PANEL) { panel_key(e); return; }
    if (d) { if (k == K_ESC && G.fs.open) { find_close(); return; } ed_try_key(d, e); }
}
static void handle_mouse(Event *e) {
    int x = e->mx, y = e->my;
    if (G.ov.kind != OV_NONE) {
        Overlay *o = &G.ov;
        if (o->kind == OV_MENU) {
            if (e->press && e->btn == 0 && !e->drag) {
                Rect r = menu_rect(o->menu);
                if (y == 0) { for (int m = 0; m < N_MENUS; m++) if (x >= menu_x(m) - 1 && x < menu_x(m) + (int)strlen(MENU_TITLES[m]) + 2) { open_menu(m); return; } ov_free(); return; }
                if (in_rect(r, x, y)) { int i = y - r.y - 1; if (i >= 0 && i < menu_count(o->menu) && MENU_ITEMS[o->menu][i]) { int id = MENU_ITEMS[o->menu][i]; ov_free(); cmd_run(id); } return; }
                ov_free();
            } else if (e->btn == 3 || e->drag) { Rect r = menu_rect(o->menu); if (in_rect(r, x, y)) { int i = y - r.y - 1; if (i >= 0 && i < menu_count(o->menu) && MENU_ITEMS[o->menu][i]) o->mitem = i; } else if (y == 0) for (int m = 0; m < N_MENUS; m++) if (x >= menu_x(m) - 1 && x < menu_x(m) + (int)strlen(MENU_TITLES[m]) + 2 && m != o->menu) open_menu(m); }
            return;
        }
        Rect b = ov_box();
        if (o->kind == OV_PICKER && (e->btn == 4 || e->btn == 5)) { if (o->nview) o->sel = e->btn == 4 ? (o->sel > 0 ? o->sel - 1 : 0) : (o->sel + 1 < o->nview ? o->sel + 1 : o->sel); return; }
        if (!(e->press && e->btn == 0 && !e->drag)) return;
        if (!in_rect(b, x, y)) { ov_cancel(); return; }
        if (o->kind == OV_PICKER) { int i = y - b.y - 2; if (i >= 0 && o->scroll + i < o->nview) { o->sel = o->scroll + i; Event en = { .type = EV_KEY, .key = K_ENTER }; ov_key(&en); } }
        else if (o->kind == OV_CONFIRM && y == b.y + 3) { int tot = 0; for (int i = 0; i < o->nbtns; i++) tot += (int)strlen(o->btns[i]) + 4; int bx = b.x + (b.w - tot) / 2; for (int i = 0; i < o->nbtns; i++) { int w = (int)strlen(o->btns[i]) + 4; if (x >= bx && x < bx + w) { o->btn = i; Event en = { .type = EV_KEY, .key = K_ENTER }; ov_key(&en); return; } bx += w + 1; } }
        return;
    }
    if (e->press && e->btn == 0 && !e->drag) G.mouse_down = 0;
    if (G.mouse_down == 10 && e->drag) { int ph = G.r_panel.y + G.r_panel.h - y; G.panel_h = ph < 4 ? 4 : ph; ui_layout(); return; }
    if (G.mouse_down == 11 && e->drag) { G.sb_w = x < 12 ? 12 : x; ui_layout(); return; }
    if (e->release && (G.mouse_down == 10 || G.mouse_down == 11)) { G.mouse_down = 0; return; }
    if (e->press && e->btn == 0 && !e->drag) {
        if (G.cfg.show_sidebar && x == G.r_tree.w && y >= G.r_tree.y && y < G.r_tree.y + G.r_tree.h) { G.mouse_down = 11; return; }
    }
    if (y == 0) { if (e->press && e->btn == 0 && !e->drag) for (int m = 0; m < N_MENUS; m++) if (x >= menu_x(m) - 1 && x < menu_x(m) + (int)strlen(MENU_TITLES[m]) + 2) { open_menu(m); return; } return; }
    if (in_rect(G.r_tabs, x, y)) {
        if (e->press && !e->drag && (e->btn == 0 || e->btn == 1)) { bool cl; int i = tab_hit(x, &cl); if (i >= 0) { if (cl || e->btn == 1) doc_close_idx(i, false); else { doc_activate(i); G.focus = FOCUS_EDITOR; } } }
        else if (e->btn == 4 && G.ndocs > 1) doc_activate((G.active + G.ndocs - 1) % G.ndocs); else if (e->btn == 5 && G.ndocs > 1) doc_activate((G.active + 1) % G.ndocs);
        return;
    }
    if (G.cfg.show_sidebar && in_rect(G.r_tree, x, y)) {
        if (e->btn == 4) { G.tree_scroll -= 3; if (G.tree_scroll < 0) G.tree_scroll = 0; return; } if (e->btn == 5) { G.tree_scroll += 3; return; }
        if (e->press && e->btn == 0 && !e->drag) { G.focus = FOCUS_TREE; int i = G.tree_scroll + (y - G.r_tree.y - 1); if (i >= 0 && i < G.nflat) { G.tree_sel = i; TNode *n = G.flat[i]; if (n->dir) tree_toggle(n); else if (doc_open_path(n->path, true)) G.focus = FOCUS_EDITOR; } }
        return;
    }
    if (G.show_panel && (in_rect(G.r_panel, x, y) || G.mouse_down == 20)) { panel_mouse(e); return; }
    if (in_rect(G.r_edit, x, y) || G.mouse_down || G.drag_scroll) { Doc *d = cur_doc(); if (d) { if (e->press && !e->drag) G.focus = FOCUS_EDITOR; ed_mouse(d, e); } }
    else if (in_rect(G.r_find, x, y) && e->press && !e->drag) { G.focus = FOCUS_EDITOR; G.fs.field = (y - G.r_find.y == 1) ? 1 : 0; }
}
void ui_open_menu(int m) { open_menu(m); }
void ui_event(Event *e) {
    switch (e->type) {
    case EV_RESIZE: ui_layout(); proc_resize(&G.proc, term_w - 2, G.panel_h > 3 ? G.panel_h - 2 : 8); break;
    case EV_PASTE: paste_event(e); break;
    case EV_MOUSE: handle_mouse(e); break;
    case EV_KEY: handle_key(e); break;
    default: break;
    }
}
