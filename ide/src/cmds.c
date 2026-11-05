/* kcide — commands, key bindings, document management, build/run integration, project index. */
#include "ide.h"
#include <ctype.h>
#ifndef _WIN32
#include <unistd.h>
#endif

const CmdInfo CMDS[] = {
    { C_NEW, "File: New File", "Ctrl+N" }, { C_OPEN, "File: Open File...", "Ctrl+O" }, { C_QUICKOPEN, "File: Quick Open (fuzzy)", "Ctrl+P" },
    { C_SAVE, "File: Save", "Ctrl+S" }, { C_SAVEAS, "File: Save As...", "Ctrl+Alt+S" }, { C_SAVEALL, "File: Save All", "" },
    { C_CLOSE, "File: Close Tab", "Ctrl+W" }, { C_RELOAD, "File: Reload From Disk", "" }, { C_QUIT, "File: Quit", "Ctrl+Q" },
    { C_UNDO, "Edit: Undo", "Ctrl+Z" }, { C_REDO, "Edit: Redo", "Ctrl+Y" }, { C_CUT, "Edit: Cut", "Ctrl+X" }, { C_COPY, "Edit: Copy", "Ctrl+C" }, { C_PASTE, "Edit: Paste", "Ctrl+V" },
    { C_SELALL, "Edit: Select All", "Ctrl+A" }, { C_SELLINE, "Edit: Select Line", "Ctrl+L" }, { C_DUPLINE, "Edit: Duplicate Line", "Ctrl+D" }, { C_DELLINE, "Edit: Delete Line", "Ctrl+K" },
    { C_COMMENT, "Edit: Toggle Line Comment", "Ctrl+/" }, { C_INDENT, "Edit: Indent", "Tab" }, { C_OUTDENT, "Edit: Outdent", "Shift+Tab" },
    { C_MOVEUP, "Edit: Move Line Up", "Alt+Up" }, { C_MOVEDOWN, "Edit: Move Line Down", "Alt+Down" }, { C_FORMAT, "Edit: Format Document (re-indent)", "Ctrl+Alt+L" }, { C_COMPLETE, "Edit: Trigger Completion", "Ctrl+Space" },
    { C_FIND, "Search: Find", "Ctrl+F" }, { C_REPLACE, "Search: Replace", "Ctrl+R" }, { C_FINDNEXT, "Search: Find Next", "F3" }, { C_FINDPREV, "Search: Find Previous", "Shift+F3" },
    { C_GOTOLINE, "Go: Go to Line...", "Ctrl+G" }, { C_GOTOSYM, "Go: Go to Symbol...", "Ctrl+T" }, { C_GOTODEF, "Go: Go to Definition", "F12" },
    { C_FINDPROJ, "Search: Find in Project...", "F4" }, { C_NAVBACK, "Go: Back", "Alt+Left" }, { C_NAVFWD, "Go: Forward", "Alt+Right" },
    { C_SIDEBAR, "View: Toggle Sidebar", "Ctrl+B" }, { C_PANEL, "View: Toggle Output Panel", "Ctrl+J" }, { C_THEME, "View: Color Theme...", "" },
    { C_WHITESPACE, "View: Toggle Whitespace", "" }, { C_LINENUMS, "View: Toggle Line Numbers", "" }, { C_AUTOPAIRS, "View: Toggle Auto-Close Brackets", "" }, { C_CHECKSAVE, "Run: Toggle Check on Save", "" },
    { C_NEXTTAB, "View: Next Tab", "Ctrl+PgDn" }, { C_PREVTAB, "View: Previous Tab", "Ctrl+PgUp" }, { C_PALETTE, "View: Command Palette", "F1 / Alt+P" },
    { C_FOCUS_TREE, "View: Focus Explorer", "Ctrl+E" }, { C_FOCUS_EDITOR, "View: Focus Editor", "Esc" }, { C_FOCUS_PANEL, "View: Focus Output Panel", "Alt+J" },
    { C_NEWFILE, "Explorer: New File...", "n" }, { C_NEWDIR, "Explorer: New Folder...", "N" }, { C_RENAME, "Explorer: Rename...", "r" }, { C_DELETE, "Explorer: Delete...", "Del" }, { C_REFRESH, "Explorer: Refresh", "F5 (in explorer)" },
    { C_RUN, "Run: Run File", "F5" }, { C_RUNTERM, "Run: Run File in Terminal", "Ctrl+F5" }, { C_CHECK, "Run: Check (parse + type-check)", "F6" }, { C_TEST, "Run: Test File", "F7" },
    { C_TESTALL, "Run: Test Project", "" }, { C_LEAKS, "Run: Run with Leak Report", "" }, { C_REPL, "Run: Open REPL (preload file)", "" }, { C_STOP, "Run: Stop", "Shift+F5" },
    { C_NEXTDIAG, "Run: Next Problem", "F8" }, { C_PREVDIAG, "Run: Previous Problem", "Shift+F8" }, { C_CLEAROUT, "Run: Clear Output", "" },
    { C_TERMINAL, "Terminal: Focus / Toggle Terminal", "Alt+T" }, { C_NEWTERM, "Terminal: New Terminal", "" }, { C_KILLTERM, "Terminal: Kill Terminal", "" },
    { C_CONSOLE, "Console: Focus / Toggle Console (REPL)", "Alt+C" }, { C_CONSOLE_CLEAR, "Console: Clear", "" }, { C_CONSOLE_RESTART, "Console: Restart REPL", "" }, { C_CONSOLE_LOAD, "Console: Restart REPL with Current File", "" },
    { C_DEBUGPANEL, "Debug: Show Debug Panel", "Alt+D" }, { C_DEBUG, "Debug: Start Debugging", "Alt+F5" }, { C_DBG_CONT, "Debug: Continue", "F5" }, { C_DBG_OVER, "Debug: Step Over", "F10" },
    { C_DBG_IN, "Debug: Step Into", "F11" }, { C_DBG_OUT, "Debug: Step Out", "Shift+F11" }, { C_DBG_PAUSE, "Debug: Pause", "" }, { C_DBG_STOP, "Debug: Stop Debugging", "Shift+F5" },
    { C_TOGGLE_BP, "Debug: Toggle Breakpoint", "F9" }, { C_CLEAR_BPS, "Debug: Remove All Breakpoints", "" }, { C_ADD_WATCH, "Debug: Add Watch Expression...", "" }, { C_MENU, "View: Open Menu Bar", "Alt+M" },
    { C_KEYS, "Help: Keyboard Shortcuts", "" }, { C_ABOUT, "Help: About KingCobra IDE", "" },
};
const int N_CMDS = (int)(sizeof CMDS / sizeof *CMDS);

static const struct { int key, mods, cmd; } KEYMAP[] = {
    { 'n', MOD_CTRL, C_NEW }, { 'o', MOD_CTRL, C_OPEN }, { 'p', MOD_CTRL, C_QUICKOPEN }, { 's', MOD_CTRL, C_SAVE }, { 's', MOD_CTRL | MOD_ALT, C_SAVEAS },
    { 'w', MOD_CTRL, C_CLOSE }, { 'q', MOD_CTRL, C_QUIT }, { 'z', MOD_CTRL, C_UNDO }, { 'y', MOD_CTRL, C_REDO }, { 'x', MOD_CTRL, C_CUT }, { 'c', MOD_CTRL, C_COPY },
    { 'v', MOD_CTRL, C_PASTE }, { 'a', MOD_CTRL, C_SELALL }, { 'l', MOD_CTRL, C_SELLINE }, { 'd', MOD_CTRL, C_DUPLINE }, { 'k', MOD_CTRL, C_DELLINE }, { '/', MOD_CTRL, C_COMMENT }, { '/', MOD_ALT, C_COMMENT },
    { K_UP, MOD_ALT, C_MOVEUP }, { K_DOWN, MOD_ALT, C_MOVEDOWN }, { 'l', MOD_CTRL | MOD_ALT, C_FORMAT }, { K_FN(9), 0, C_TOGGLE_BP }, { K_FN(10), 0, C_DBG_OVER }, { K_FN(11), 0, C_DBG_IN }, { K_FN(11), MOD_SHIFT, C_DBG_OUT }, { K_FN(5), MOD_ALT, C_DEBUG },
    { 't', MOD_ALT, C_TERMINAL }, { 'c', MOD_ALT, C_CONSOLE }, { 'd', MOD_ALT, C_DEBUGPANEL }, { 'm', MOD_ALT, C_MENU }, { ' ', MOD_CTRL, C_COMPLETE }, { '/', MOD_ALT | MOD_SHIFT, C_COMPLETE },
    { 'f', MOD_CTRL, C_FIND }, { 'r', MOD_CTRL, C_REPLACE }, { K_FN(3), 0, C_FINDNEXT }, { K_FN(3), MOD_SHIFT, C_FINDPREV },
    { 'g', MOD_CTRL, C_GOTOLINE }, { 't', MOD_CTRL, C_GOTOSYM }, { K_FN(12), 0, C_GOTODEF }, { K_FN(4), 0, C_FINDPROJ }, { K_LEFT, MOD_ALT, C_NAVBACK }, { K_RIGHT, MOD_ALT, C_NAVFWD },
    { 'b', MOD_CTRL, C_SIDEBAR }, { 'j', MOD_CTRL, C_PANEL }, { K_FN(1), 0, C_PALETTE }, { 'p', MOD_ALT, C_PALETTE }, { K_PGDN, MOD_CTRL, C_NEXTTAB }, { K_PGUP, MOD_CTRL, C_PREVTAB },
    { 'e', MOD_CTRL, C_FOCUS_TREE }, { 'j', MOD_ALT, C_FOCUS_PANEL },
    { K_FN(5), 0, C_RUN }, { K_FN(5), MOD_CTRL, C_RUNTERM }, { K_FN(5), MOD_SHIFT, C_STOP }, { K_FN(6), 0, C_CHECK }, { K_FN(7), 0, C_TEST }, { K_FN(8), 0, C_NEXTDIAG }, { K_FN(8), MOD_SHIFT, C_PREVDIAG },
};
int keymap_lookup(const Event *e) {
    for (size_t i = 0; i < sizeof KEYMAP / sizeof *KEYMAP; i++) if (KEYMAP[i].key == e->key && KEYMAP[i].mods == e->mods) return KEYMAP[i].cmd;
    return C_NONE;
}
const char *theme_name(int i) { return THEMES[i].name; }

/* ---------------------------------------------------------------- navigation history */
static bool same_loc(NavEntry *a, const char *path, int line) { return a->path && path && path_eq(a->path, path) && a->line == line; }
void nav_push(void) {
    Doc *d = cur_doc(); if (!d || !d->path) return;
    if (G.navpos >= 0 && G.navpos < G.nnav - 1) { for (int i = G.navpos + 1; i < G.nnav; i++) free(G.nav[i].path); G.nnav = G.navpos + 1; }
    if (G.nnav > 0 && same_loc(&G.nav[G.nnav - 1], d->path, d->cur.line)) { G.navpos = G.nnav - 1; return; }
    if (G.nnav == 64) { free(G.nav[0].path); memmove(G.nav, G.nav + 1, 63 * sizeof(NavEntry)); G.nnav--; }
    G.nav[G.nnav++] = (NavEntry){ xstrdup(d->path), d->cur.line, d->cur.col }; G.navpos = G.nnav - 1;
}
static void nav_jump(int i) {
    NavEntry *n = &G.nav[i]; Doc *d = doc_open_path(n->path, true); if (d) ed_goto(d, n->line, n->col, false);
}
void nav_go(int dir) {
    Doc *d = cur_doc(); int target;
    if (G.nnav == 0) { status(0, "No navigation history"); return; }
    if (dir < 0) {
        if (d && d->path && G.navpos == G.nnav - 1 && !same_loc(&G.nav[G.nnav - 1], d->path, d->cur.line)) {
            if (G.nnav == 64) { free(G.nav[0].path); memmove(G.nav, G.nav + 1, 63 * sizeof(NavEntry)); G.nnav--; G.navpos--; }
            G.nav[G.nnav++] = (NavEntry){ xstrdup(d->path), d->cur.line, d->cur.col }; target = G.nnav - 2;
        } else target = G.navpos - 1;
        if (target < 0) { status(0, "Start of history"); return; }
    } else { target = G.navpos + 1; if (target >= G.nnav) { status(0, "End of history"); return; } }
    G.navpos = target; nav_jump(target);
}

/* ---------------------------------------------------------------- documents */
int doc_find_open(const char *path) {
    char *a = path_abs(path); int r = -1;
    for (int i = 0; i < G.ndocs; i++) if (G.docs[i]->path && path_eq(G.docs[i]->path, a)) { r = i; break; }
    free(a); return r;
}
static void doc_add(Doc *d) {
    G.docs = xrealloc(G.docs, (size_t)(G.ndocs + 1) * sizeof(Doc *)); G.docs[G.ndocs++] = d;
    d->readonly = d->readonly;
}
void doc_activate(int idx) {
    if (idx < 0 || idx >= G.ndocs) return;
    G.active = idx; comp_close(&G.comp); Doc *d = G.docs[idx];
    char t[512]; snprintf(t, sizeof t, "%s%s - KingCobra IDE", d->title, d->dirty ? " *" : ""); term_set_title(t);
    ui_layout(); ed_reveal(d); if (G.fs.open) find_recount();
    if (d->path) tree_reveal(d->path);
}
Doc *doc_open_path(const char *path, bool activate) {
    int i = doc_find_open(path);
    if (i >= 0) { if (activate) doc_activate(i); return G.docs[i]; }
    if (is_dir(path)) { status(2, "%s is a directory", path); return NULL; }
    char err[300]; Doc *d;
    if (!is_file(path)) { d = doc_new(path_basename(path)); d->path = path_abs(path); doc_set_lang(d, lang_from_path(path)); status(1, "New file %s (not saved yet)", path_basename(path)); }
    else {
        size_t n; char *probe = read_file(path, &n);
        if (probe && memchr(probe, 0, n < 4096 ? n : 4096)) { free(probe); status(2, "%s looks like a binary file", path_basename(path)); return NULL; }
        free(probe);
        d = doc_open(path, err, sizeof err); if (!d) { status(3, "%s", err); return NULL; }
    }
    doc_add(d); if (activate) doc_activate(G.ndocs - 1);
    return d;
}
bool docs_any_dirty(void) { for (int i = 0; i < G.ndocs; i++) if (G.docs[i]->dirty) return true; return false; }
static void remove_doc(int idx) {
    doc_free(G.docs[idx]); memmove(G.docs + idx, G.docs + idx + 1, (size_t)(G.ndocs - idx - 1) * sizeof(Doc *)); G.ndocs--;
    if (G.active >= G.ndocs) G.active = G.ndocs - 1; if (G.active < 0) G.active = 0;
    comp_close(&G.comp); if (G.ndocs) doc_activate(G.active); else { G.fs.open = false; term_set_title("KingCobra IDE"); ui_layout(); }
}
static bool save_doc(Doc *d);
static void close_confirm(const char *btn, intptr_t id, void *ctx) {
    (void)btn; Doc *d = ctx; int idx = -1; for (int i = 0; i < G.ndocs; i++) if (G.docs[i] == d) idx = i;
    if (idx < 0 || id == 2 || id < 0) return;
    if (id == 0) { if (!save_doc(d)) return; }
    remove_doc(idx);
}
void doc_close_idx(int idx, bool force) {
    if (idx < 0 || idx >= G.ndocs) return; Doc *d = G.docs[idx];
    if (d->dirty && !force) { char m[300]; snprintf(m, sizeof m, "Save changes to %s before closing?", d->title); G.active = idx; ov_confirm(m, "Save", "Don't Save", "Cancel", close_confirm, d); return; }
    remove_doc(idx);
}

/* ---------------------------------------------------------------- save */
static void start_proc(int kind, bool quiet);
void on_saved(Doc *d) {
    status(1, "Saved %s", d->title); doc_activate(G.active);
    G.indexed = false;
    if (G.cfg.check_on_save && d->is_kc && G.kc_found && !G.proc.running) start_proc(C_CHECK, true);
}
static bool save_doc_as(Doc *d, const char *path) {
    char err[300]; if (!doc_save(d, path, G.cfg.trim_ws, err, sizeof err)) { status(3, "%s", err); return false; }
    on_saved(d); tree_build(); return true;
}
static void saveas_cb(const char *text, intptr_t id, void *ctx);
static void overwrite_cb(const char *btn, intptr_t id, void *ctx) { (void)btn; Doc *d = cur_doc(); char *p = ctx; if (d && id == 0) save_doc_as(d, p); free(p); }
static void saveas_cb(const char *text, intptr_t id, void *ctx) {
    (void)id; (void)ctx; Doc *d = cur_doc(); if (!d || !*text) return;
    char *p = path_is_abs(text) ? xstrdup(text) : path_join(G.root, text);
    if (is_file(p) && !(d->path && path_eq(path_abs(p), d->path))) { char m[600]; snprintf(m, sizeof m, "%s exists. Overwrite?", path_basename(p)); ov_confirm(m, "Overwrite", "Cancel", NULL, overwrite_cb, p); return; }
    save_doc_as(d, p); free(p);
}
static bool save_doc(Doc *d) {
    if (!d->path) { char *init = path_join(G.root, d->title[0] && strcmp(d->title, "untitled") ? d->title : "untitled.kc"); ov_prompt("Save As", init, true, saveas_cb, NULL); free(init); return false; }
    char err[300]; if (!doc_save(d, NULL, G.cfg.trim_ws, err, sizeof err)) { status(3, "%s", err); return false; }
    on_saved(d); return true;
}
static void save_all(void) { int n = 0; for (int i = 0; i < G.ndocs; i++) if (G.docs[i]->dirty && G.docs[i]->path) { char err[300]; if (doc_save(G.docs[i], NULL, G.cfg.trim_ws, err, sizeof err)) n++; else status(3, "%s", err); } if (n) status(1, "Saved %d file%s", n, n == 1 ? "" : "s"); }

/* ---------------------------------------------------------------- project index */
static const char *SKIP_DIRS[] = { "node_modules", "target", "__pycache__", "build", "dist", NULL };
static const char *BIN_EXT[] = { ".o", ".obj", ".a", ".so", ".dll", ".exe", ".png", ".jpg", ".jpeg", ".gif", ".zip", ".gz", ".pdf", ".bin", ".ll", ".dylib", ".class", ".pyc", ".ttf", ".woff", ".mp3", ".mp4", ".wav", NULL };
static void walk(const char *dir, int depth) {
    if (depth > 8 || G.nfiles > 20000) return;
    DirList dl; if (!dir_list(dir, &dl)) return;
    for (int i = 0; i < dl.n; i++) {
        const char *n = dl.v[i].name; if (n[0] == '.') continue;
        char *full = path_join(dir, n);
        if (dl.v[i].dir) { bool skip = false; for (int k = 0; SKIP_DIRS[k]; k++) if (!strcmp(n, SKIP_DIRS[k])) skip = true; if (!skip) walk(full, depth + 1); free(full); continue; }
        bool bin = false; const char *e = path_ext(n); for (int k = 0; BIN_EXT[k]; k++) if (!strcmp(e, BIN_EXT[k])) bin = true;
        if (bin) { free(full); continue; }
        G.files = xrealloc(G.files, (size_t)(G.nfiles + 1) * sizeof(char *)); G.files[G.nfiles++] = full;
    }
    dir_list_free(&dl);
}
void index_project(bool force) {
    if (G.indexed && !force) return;
    for (int i = 0; i < G.nfiles; i++) free(G.files[i]); free(G.files); G.files = NULL; G.nfiles = 0;
    for (int i = 0; i < G.nsyms; i++) { free(G.syms[i].name); free(G.syms[i].file); } free(G.syms); G.syms = NULL; G.nsyms = 0;
    walk(G.root, 0);
    int cap = 0;
    for (int i = 0; i < G.nfiles; i++) {
        if (strcmp(path_ext(G.files[i]), ".kc")) continue;
        size_t n; char *t = read_file(G.files[i], &n); if (!t) continue;
        int line = 0; char *p = t;
        while (p < t + n) {
            char *e = memchr(p, '\n', (size_t)(t + n - p)); int len = e ? (int)(e - p) : (int)(t + n - p);
            if (len > 0 && p[len - 1] == '\r') len--;
            SymHit h; if (sym_scan_line(p, len, &h) && h.kind != SYM_IMPL) {
                if (G.nsyms == cap) { cap = cap ? cap * 2 : 256; G.syms = xrealloc(G.syms, (size_t)cap * sizeof(ProjSym)); }
                G.syms[G.nsyms++] = (ProjSym){ xstrdup(h.name), h.kind, xstrdup(G.files[i]), line };
            }
            line++; if (!e) break; p = e + 1;
        }
        free(t);
    }
    G.indexed = true;
}

/* ---------------------------------------------------------------- pickers */
static void pick_open_file(const char *t, intptr_t id, void *ctx) { (void)t; (void)ctx; if (id >= 0 && id < G.nfiles) { Doc *d = doc_open_path(G.files[id], true); (void)d; G.focus = FOCUS_EDITOR; } }
static void quick_open(void) {
    index_project(true); PItem *it = xcalloc((size_t)G.nfiles + 1, sizeof *it);
    for (int i = 0; i < G.nfiles; i++) { it[i].label = path_rel(G.root, G.files[i]); it[i].detail = NULL; it[i].id = i; }
    ov_picker("Quick Open", it, G.nfiles, true, pick_open_file, NULL);
}
static ProjSym *g_pick; static int g_npick;
static void pick_sym(const char *t, intptr_t id, void *ctx) {
    (void)t; (void)ctx; if (id < 0 || id >= g_npick) return; ProjSym s = g_pick[id];
    nav_push(); Doc *d = doc_open_path(s.file, true); if (d) { ed_goto(d, s.line, 0, false); Line *l = &d->lines[d->cur.line]; SymHit h; if (sym_scan_line(l->s, l->len, &h)) { d->cur.col = h.col; d->anchor = d->cur; } ed_reveal(d); } G.focus = FOCUS_EDITOR;
}
static void free_picksyms(void) { for (int i = 0; i < g_npick; i++) { free(g_pick[i].name); free(g_pick[i].file); } free(g_pick); g_pick = NULL; g_npick = 0; }
static void collect_syms(bool project_wide, const char *only_name) {
    free_picksyms(); int cap = 0;
    for (int di = 0; di < G.ndocs; di++) {
        Doc *d = G.docs[di]; if (!d->is_kc) continue;
        if (!project_wide && di != G.active) continue;
        for (int i = 0; i < d->nlines; i++) { SymHit h; if (sym_scan_line(d->lines[i].s, d->lines[i].len, &h) && h.kind != SYM_IMPL && (!only_name || !strcmp(only_name, h.name))) {
            if (g_npick == cap) { cap = cap ? cap * 2 : 128; g_pick = xrealloc(g_pick, (size_t)cap * sizeof(ProjSym)); }
            g_pick[g_npick++] = (ProjSym){ xstrdup(h.name), h.kind, xstrdup(d->path ? d->path : d->title), i }; } }
    }
    if (project_wide) { index_project(false);
        for (int i = 0; i < G.nsyms; i++) { if (only_name && strcmp(only_name, G.syms[i].name)) continue; bool open = false; for (int di = 0; di < G.ndocs; di++) if (G.docs[di]->path && path_eq(G.docs[di]->path, G.syms[i].file)) open = true; if (open) continue;
            if (g_npick == cap) { cap = cap ? cap * 2 : 128; g_pick = xrealloc(g_pick, (size_t)cap * sizeof(ProjSym)); }
            g_pick[g_npick++] = (ProjSym){ xstrdup(G.syms[i].name), G.syms[i].kind, xstrdup(G.syms[i].file), G.syms[i].line }; } }
}
static void goto_symbol(void) {
    collect_syms(true, NULL); if (!g_npick) { status(0, "No symbols found"); return; }
    PItem *it = xcalloc((size_t)g_npick, sizeof *it);
    for (int i = 0; i < g_npick; i++) { it[i].label = xfmt("%-6s %s", sym_kind_name(g_pick[i].kind), g_pick[i].name); char *r = path_rel(G.root, g_pick[i].file); it[i].detail = xfmt("%s:%d", r, g_pick[i].line + 1); free(r); it[i].id = i; }
    ov_picker("Go to Symbol", it, g_npick, true, pick_sym, NULL);
}
static void goto_def(void) {
    Doc *d = cur_doc(); if (!d) return; int a, b; if (!doc_word_at(d, d->cur, &a, &b)) { status(0, "Put the cursor on an identifier"); return; }
    char *w = xstrndup(d->lines[d->cur.line].s + a, (size_t)(b - a)); collect_syms(true, w);
    if (g_npick == 0) status(2, "No definition found for '%s'", w);
    else if (g_npick == 1) { pick_sym("", 0, NULL); }
    else { PItem *it = xcalloc((size_t)g_npick, sizeof *it); for (int i = 0; i < g_npick; i++) { it[i].label = xfmt("%-6s %s", sym_kind_name(g_pick[i].kind), g_pick[i].name); char *r = path_rel(G.root, g_pick[i].file); it[i].detail = xfmt("%s:%d", r, g_pick[i].line + 1); free(r); it[i].id = i; } ov_picker("Definitions", it, g_npick, false, pick_sym, NULL); }
    free(w);
}
static void goto_line_cb(const char *t, intptr_t id, void *ctx) { (void)id; (void)ctx; Doc *d = cur_doc(); if (!d) return; int l = atoi(t), c = 0; const char *p = strchr(t, ':'); if (p) c = atoi(p + 1); ed_goto(d, l - 1, c > 0 ? doc_col_at_vcol(d, l - 1 < d->nlines && l > 0 ? l - 1 : 0, c - 1, 1) : 0, true); G.focus = FOCUS_EDITOR; }

static void proj_search_cb(const char *text, intptr_t id, void *ctx) {
    (void)id; (void)ctx; if (!*text) return; index_project(true);
    out_clear(&G.out); panel_show(true);
    char h[300]; snprintf(h, sizeof h, "Search: \"%s\" in %s", text, G.root); out_puts(&G.out, h, TH.info, ATTR_BOLD);
    int found = 0, tl = (int)strlen(text);
    for (int f = 0; f < G.nfiles && found < 2000; f++) {
        char *t; size_t n; int di = -1; for (int k = 0; k < G.ndocs; k++) if (G.docs[k]->path && path_eq(G.docs[k]->path, G.files[f])) di = k;
        if (di >= 0) { t = doc_text(G.docs[di], (Pos){ 0, 0 }, doc_end(G.docs[di]), (int *)&n); } else { t = read_file(G.files[f], &n); if (!t || n > 2000000 || memchr(t, 0, n < 1024 ? n : 1024)) { free(t); continue; } }
        int line = 0; char *p = t;
        while (p < t + n && found < 2000) {
            char *e = memchr(p, '\n', (size_t)(t + n - p)); int len = e ? (int)(e - p) : (int)(t + n - p);
            for (int i = 0; i + tl <= len; i++) { bool m = true; for (int k = 0; k < tl; k++) if (tolower((unsigned char)p[i + k]) != tolower((unsigned char)text[k])) { m = false; break; }
                if (m) { char *rel = path_rel(G.root, G.files[f]); char *tx = xstrndup(p, (size_t)(len > 160 ? 160 : len)); for (char *q = tx; *q; q++) if (*q == '\t') *q = ' ';
                    char *ln = xfmt("%s:%d:%d  %s", rel, line + 1, i + 1, tx); out_add_link(&G.out, G.files[f], line + 1, i + 1, ln, TH.fg); free(ln); free(tx); free(rel); found++; break; } }
            line++; if (!e) break; p = e + 1;
        }
        free(t);
    }
    char s[100]; snprintf(s, sizeof s, "%d result%s", found, found == 1 ? "" : "s"); out_puts(&G.out, s, TH.ui_dim, ATTR_DIM);
    G.out.scroll = 0; G.out.follow = false;
}

/* ---------------------------------------------------------------- run / check / test */
static void find_kc(void) {
    G.kc_found = false; char tmp[512]; char *c;
    const char *envp = getenv("KINGCOBRA");
    if (G.cfg.kc_path[0] && is_file(G.cfg.kc_path)) { snprintf(G.kc_exe, sizeof G.kc_exe, "%s", G.cfg.kc_path); G.kc_found = true; return; }
    if (envp && is_file(envp)) { snprintf(G.kc_exe, sizeof G.kc_exe, "%s", envp); G.kc_found = true; return; }
    const char *rel[] = { "kingcobra", "../kingcobra", "kingcobra.exe" };
    for (int i = 0; i < 3 && !G.kc_found; i++) { c = path_join(G.root, rel[i]); if (is_file(c)) { char *a = path_abs(c); snprintf(G.kc_exe, sizeof G.kc_exe, "%s", a); free(a); G.kc_found = true; } free(c); }
    if (!G.kc_found && G.exe_dir) for (int i = 0; i < 3 && !G.kc_found; i++) { c = path_join(G.exe_dir, rel[i]); if (is_file(c)) { char *a = path_abs(c); snprintf(G.kc_exe, sizeof G.kc_exe, "%s", a); free(a); G.kc_found = true; } free(c); }
    if (!G.kc_found && find_in_path("kingcobra", tmp, sizeof tmp)) { snprintf(G.kc_exe, sizeof G.kc_exe, "%s", tmp); G.kc_found = true; }
}
void kc_discover(void) {
    find_kc();
    char *root = NULL;
    if (G.kc_found) { char *dir = path_dirname(G.kc_exe); char *s = path_join(dir, "std"); if (is_dir(s)) root = xstrdup(dir); free(s); free(dir); }
    if (!root) { char *s = path_join(G.root, "std"); if (is_dir(s)) root = xstrdup(G.root); free(s); }
    std_api_load(root); free(root);
}
static void finish_proc(void) {
    for (int i = 0; i < 100 && proc_poll(&G.proc); i++) {
#ifndef _WIN32
        usleep(5000);
#endif
    }
}
static int count_diags(int sev, const char *path) { int n = 0; for (int i = G.proc_diag_base; i < G.out.ndiag; i++) if (G.out.diags[i].sev == sev && (!path || path_eq(path, G.out.diags[i].file))) n++; return n; }
static bool g_quiet, g_auto_focus;
static void start_proc(int kind, bool quiet) {
    Doc *d = cur_doc();
    if (!G.kc_found) { status(3, "kingcobra executable not found - set KINGCOBRA or kc_path in the config"); return; }
    bool need_file = kind != C_TESTALL;
    if (need_file && (!d || !d->path)) { if (d && !d->path) { save_doc(d); status(2, "Save the file first"); } else status(2, "No file to run"); return; }
    if (dbg_active() && kind != C_CHECK) { status(2, "Stop the debugger first (Shift+F5)"); return; }
    if (G.proc.running) { if (quiet) return; proc_stop(&G.proc); proc_close(&G.proc); }
    if (!quiet && G.cfg.save_before_run) save_all();
    else if (!quiet && d && d->dirty && d->path) { char e[200]; doc_save(d, NULL, G.cfg.trim_ws, e, sizeof e); }
    char *argv[8]; int n = 0; argv[n++] = G.kc_exe; char *file = d ? d->path : NULL; const char *label = "";
    switch (kind) {
    case C_RUN: argv[n++] = "run"; argv[n++] = file; label = "run"; break;
    case C_LEAKS: argv[n++] = "--leaks"; argv[n++] = "run"; argv[n++] = file; label = "run --leaks"; break;
    case C_CHECK: argv[n++] = "check"; argv[n++] = file; label = "check"; break;
    case C_TEST: argv[n++] = "test"; argv[n++] = file; label = "test"; break;
    case C_TESTALL: argv[n++] = "test"; label = "test (project)"; break;
    default: return;
    }
    argv[n] = NULL;
    out_clear(&G.out); G.proc_diag_base = 0;
    char hdr[700]; snprintf(hdr, sizeof hdr, "$ kingcobra %s%s%s", label, file && kind != C_LEAKS && kind != C_RUN ? " " : "", ""); 
    snprintf(hdr, sizeof hdr, "$ kingcobra %s %s", label, file && kind != C_TESTALL ? path_basename(file) : "");
    out_puts(&G.out, hdr, TH.ui_dim, ATTR_DIM);
    int cols = term_w - 2, rows = G.panel_h > 3 ? G.panel_h - 2 : 8;
    if (!proc_start_ex(&G.proc, argv, G.root, cols, rows, NULL, true)) { out_puts(&G.out, "failed to start process", TH.error, 0); return; }
    G.proc_start_ms = now_ms(); G.proc_is_check = kind == C_CHECK; g_quiet = quiet; snprintf(G.proc_label, sizeof G.proc_label, "%s", label);
    G.proc_ilen = 0;
    if (!quiet) { panel_show(true); if (kind == C_RUN || kind == C_LEAKS) { G.focus = FOCUS_PANEL; g_auto_focus = true; } }
    G.out.follow = true;
}
void run_start(int kind) { start_proc(kind, false); }
bool run_spawn(const char *label, char *const argv[], const char *const *env, bool is_debug) {
    if (G.proc.running) { proc_stop(&G.proc); } proc_close(&G.proc);
    out_clear(&G.out); G.proc_diag_base = 0;
    char hdr[500]; snprintf(hdr, sizeof hdr, "$ kingcobra %s %s", label, argv[2] ? path_basename(argv[2]) : ""); out_puts(&G.out, hdr, TH.ui_dim, ATTR_DIM);
    int cols = term_w - 2, rows = G.panel_h > 3 ? G.panel_h - 2 : 8;
    if (!proc_start_ex(&G.proc, argv, G.root, cols, rows, env, true)) { out_puts(&G.out, "failed to start process", TH.error, 0); return false; }
    G.proc_start_ms = now_ms(); G.proc_is_check = false; g_quiet = false; snprintf(G.proc_label, sizeof G.proc_label, "%s", label); G.proc_ilen = 0; G.proc_is_debug = is_debug; G.out.follow = true;
    return true;
}
void run_poll(void) {
    if (!G.proc.running && G.proc.fd < 0) return;
    char buf[8192]; int total = 0;
    for (;;) {
        int n = proc_read(&G.proc, buf, sizeof buf);
        if (n > 0) { out_feed(&G.out, buf, n); total += n; if (total > 262144) break; continue; }
        if (n < 0) { finish_proc(); proc_close(&G.proc); }
        break;
    }
    if (G.proc.running) { if (!proc_poll(&G.proc)) {} }
    if (!G.proc.running) {
        if (G.proc.fd >= 0) { for (;;) { int n = proc_read(&G.proc, buf, sizeof buf); if (n <= 0) break; out_feed(&G.out, buf, n); } proc_close(&G.proc); }
        if (G.out.lines[G.out.n - 1].n) out_feed(&G.out, "\n", 1);
        uint64_t ms = now_ms() - G.proc_start_ms; int code = G.proc.exit_code;
        int errs = count_diags(0, NULL), warns = count_diags(1, NULL);
        char f[300]; snprintf(f, sizeof f, "[%.60s finished: exit code %d, %llu ms]", G.proc_label, code, (unsigned long long)ms);
        out_puts(&G.out, f, code == 0 ? TH.ok : TH.error, ATTR_BOLD);
        if (G.proc_is_check) {
            if (errs) status(3, "%d error%s, %d warning%s - F8 jumps to the next problem", errs, errs == 1 ? "" : "s", warns, warns == 1 ? "" : "s");
            else if (g_quiet) status(1, warns ? "No errors, %d warning%s" : "No errors", warns, warns == 1 ? "" : "s");
            else status(1, "Check passed%s", warns ? " with warnings" : "");
            if (errs && g_quiet) panel_show(true);
        } else status(code == 0 ? 1 : 3, "%s finished with exit code %d", G.proc_label, code);
        if (!(G.out.ndiag == 0 && code == 0) && !g_quiet) { /* keep panel open */ }
        if (G.focus == FOCUS_PANEL && g_auto_focus) G.focus = FOCUS_EDITOR;
        g_auto_focus = false; g_quiet = false; G.proc.fd = -1; G.proc.running = false;
        if (G.proc_is_debug) { G.proc_is_debug = false; dbg_process_exited(code); }
    }
}
static void run_in_terminal(int kind) {
    Doc *d = cur_doc(); if (!G.kc_found) { status(3, "kingcobra executable not found"); return; }
    if (kind != C_REPL && (!d || !d->path)) { status(2, "Save the file first"); return; }
    if (G.cfg.save_before_run) save_all();
    char *argv[6]; int n = 0; argv[n++] = G.kc_exe;
    if (kind == C_REPL) { argv[n++] = "repl"; if (d && d->path && d->is_kc) argv[n++] = d->path; } else { argv[n++] = "run"; argv[n++] = d->path; }
    argv[n] = NULL;
    proc_stop(&G.proc);
    term_suspend();
    printf("\033[2J\033[H\033[1m$ kingcobra %s %s\033[0m\n\n", argv[1], n > 2 ? path_basename(argv[2]) : ""); fflush(stdout);
    int code = proc_run_inherit(argv, G.root);
    printf("\n\033[1m[exited with code %d - press Enter to return to the IDE]\033[0m ", code); fflush(stdout);
    int ch; while ((ch = getchar()) != '\n' && ch != EOF) {}
    term_resume(); scr_invalidate(); status(code == 0 ? 1 : 3, "Program exited with code %d", code);
}

void focus_diag_index(int i) {
    if (i < 0 || i >= G.out.ndiag) return; Diag *g = &G.out.diags[i]; G.cur_diag = i;
    Doc *d = doc_open_path(g->file, true); if (!d) return;
    nav_push(); ed_goto(d, g->line - 1, g->col > 0 ? g->col - 1 : 0, false); G.focus = FOCUS_EDITOR;
    status(g->sev == 0 ? 3 : g->sev == 1 ? 2 : 0, "%s", g->msg);
}
void diag_goto(int dir) {
    int n = G.out.ndiag; if (!n) { status(0, "No problems"); return; }
    int count = 0; for (int i = 0; i < n; i++) if (G.out.diags[i].sev < 2) count++;
    if (!count) { status(0, "No problems"); return; }
    int i = G.cur_diag;
    for (int step = 0; step < n; step++) { i = ((i + dir) % n + n) % n; if (G.out.diags[i].sev < 2) { focus_diag_index(i); return; } }
}

/* ---------------------------------------------------------------- tree file ops */
static TNode *tree_target(void) { return (G.tree_sel >= 0 && G.tree_sel < G.nflat) ? G.flat[G.tree_sel] : G.tree; }
static char *target_dir(void) { TNode *n = tree_target(); if (!n) return xstrdup(G.root); return n->dir ? xstrdup(n->path) : path_dirname(n->path); }
static void newfile_cb(const char *t, intptr_t id, void *ctx) {
    (void)id; (void)ctx; if (!*t) return; char *dir = target_dir(); char *p = path_join(dir, t); free(dir);
    if (is_file(p) || is_dir(p)) { status(2, "%s already exists", t); free(p); return; }
    FILE *f = fopen(p, "wb"); if (!f) { status(3, "cannot create %s", t); free(p); return; } fclose(f);
    tree_build(); G.indexed = false; Doc *d = doc_open_path(p, true); (void)d; G.focus = FOCUS_EDITOR; free(p);
}
static void newdir_cb(const char *t, intptr_t id, void *ctx) { (void)id; (void)ctx; if (!*t) return; char *dir = target_dir(); char *p = path_join(dir, t); free(dir); if (!make_dir(p)) status(3, "cannot create folder"); tree_build(); free(p); }
static void rename_cb(const char *t, intptr_t id, void *ctx) {
    (void)id; char *old = ctx; if (*t) { char *dir = path_dirname(old); char *np = path_join(dir, t); free(dir);
        if (rename(old, np) == 0) { int i = doc_find_open(old); if (i >= 0) { free(G.docs[i]->path); G.docs[i]->path = path_abs(np); free(G.docs[i]->title); G.docs[i]->title = xstrdup(path_basename(np)); doc_set_lang(G.docs[i], lang_from_path(np)); }
            tree_build(); G.indexed = false; status(1, "Renamed to %s", t); } else status(3, "rename failed"); free(np); }
    free(old);
}
static void delete_cb(const char *btn, intptr_t id, void *ctx) {
    (void)btn; char *p = ctx; if (id == 0) { int i = doc_find_open(p); if (i >= 0) doc_close_idx(i, true); if (remove(p) == 0) { status(1, "Deleted %s", path_basename(p)); tree_build(); G.indexed = false; } else status(3, "cannot delete (folders must be empty)"); } free(p);
}

/* ---------------------------------------------------------------- misc commands */
static void theme_cb(const char *t, intptr_t id, void *ctx) { (void)t; (void)ctx; if (id >= 0 && id < N_THEMES) { G.cfg.theme = (int)id; G.th = (Theme *)&THEMES[id]; scr_invalidate(); cfg_save(); status(1, "Theme: %s", THEMES[id].name); } }
static void palette_cb(const char *t, intptr_t id, void *ctx) { (void)t; (void)ctx; if (id > 0) cmd_run((int)id); }
static void palette(void) {
    PItem *it = xcalloc((size_t)N_CMDS, sizeof *it);
    for (int i = 0; i < N_CMDS; i++) { it[i].label = xstrdup(CMDS[i].label); it[i].detail = CMDS[i].keys[0] ? xstrdup(CMDS[i].keys) : NULL; it[i].id = CMDS[i].id; }
    ov_picker("Command Palette", it, N_CMDS, true, palette_cb, NULL);
}
static void open_cb(const char *t, intptr_t id, void *ctx) { (void)id; (void)ctx; if (!*t) return; char *p = path_is_abs(t) ? xstrdup(t) : path_join(G.root, t); if (doc_open_path(p, true)) G.focus = FOCUS_EDITOR; free(p); }
static void show_text_doc(const char *title, const char *text) {
    int i = -1; for (int k = 0; k < G.ndocs; k++) if (!G.docs[k]->path && !strcmp(G.docs[k]->title, title)) i = k;
    if (i >= 0) remove_doc(i);
    Doc *d = doc_new(title); Pos e; doc_insert(d, (Pos){ 0, 0 }, text, (int)strlen(text), &e); d->dirty = false; d->saved_depth = d->nundo; d->readonly = true; d->cur = d->anchor = (Pos){ 0, 0 };
    doc_add(d); doc_activate(G.ndocs - 1);
}
static void show_keys(void) {
    Str s; str_init(&s); str_adds(&s, "KingCobra IDE - keyboard shortcuts (all commands are also in the palette: F1)\n\n");
    for (int i = 0; i < N_CMDS; i++) str_addf(&s, "  %-44s %s\n", CMDS[i].label, CMDS[i].keys);
    str_adds(&s, "\n  Mouse: click, drag, double-click word, triple-click line, wheel scroll (Shift = horizontal),\n         click tabs / menu / explorer / problems.\n  Find bar: Enter next, Up/Down prev/next, Alt+C case, Alt+W whole word, Alt+A replace all, Tab switch field.\n  Completion: Tab/Enter accept, Esc dismiss. Type a snippet name (fn, if, for, match, test...) + Tab to expand.\n");
    char *t = str_take(&s); show_text_doc("Keyboard Shortcuts", t); free(t);
}
static void about(void) {
    status(1, "KingCobra IDE %s - C11 terminal IDE | %s | std API members indexed: %d", KCIDE_VERSION, G.kc_found ? G.kc_exe : "kingcobra not found", std_api_member_count());
}

static void add_watch_prompt_cb(const char *t, intptr_t id, void *ctx) { (void)id; (void)ctx; dbg_add_watch(t); }
void ui_open_menu(int m);
static void quit_cb(const char *b, intptr_t id, void *ctx) { (void)b; (void)ctx; if (id == 0) { save_all(); if (docs_any_dirty()) status(2, "Some documents have no file name - use Quit again to discard"); else G.quit = true; } else if (id == 1) G.quit = true; }
void cmd_run(int id) {
    Doc *d = cur_doc();
    comp_close(&G.comp);
    switch (id) {
    case C_NEW: { char t[32]; static int n = 1; snprintf(t, sizeof t, "untitled-%d.kc", n++); Doc *nd = doc_new(t); doc_set_lang(nd, LANG_KC); doc_add(nd); doc_activate(G.ndocs - 1); G.focus = FOCUS_EDITOR; break; }
    case C_OPEN: ov_prompt("Open File (path)", G.root, true, open_cb, NULL); break;
    case C_QUICKOPEN: quick_open(); break;
    case C_SAVE: if (d && !d->readonly) save_doc(d); break;
    case C_SAVEAS: if (d && !d->readonly) { char *init = d->path ? xstrdup(d->path) : path_join(G.root, d->title); ov_prompt("Save As", init, true, saveas_cb, NULL); free(init); } break;
    case C_SAVEALL: save_all(); break;
    case C_CLOSE: if (d) doc_close_idx(G.active, false); break;
    case C_RELOAD: if (d && d->path) { char e[200]; if (doc_reload(d, e, sizeof e)) status(1, "Reloaded %s", d->title); else status(3, "%s", e); } break;
    case C_QUIT: if (docs_any_dirty()) ov_confirm("There are unsaved changes. Quit anyway?", "Save All & Quit", "Quit", "Cancel", quit_cb, NULL); else G.quit = true; break;
    case C_UNDO: if (d) ed_undo(d); break; case C_REDO: if (d) ed_redo(d); break;
    case C_CUT: if (d) ed_copy(d, true); break; case C_COPY: if (d) ed_copy(d, false); break;
    case C_PASTE: if (d) ed_paste(d); break; case C_SELALL: if (d) ed_select_all(d); break; case C_SELLINE: if (d) ed_select_line(d); break;
    case C_DUPLINE: if (d && !d->readonly) ed_dup_line(d); break; case C_DELLINE: if (d && !d->readonly) ed_del_line(d); break;
    case C_COMMENT: if (d && !d->readonly) ed_toggle_comment(d); break;
    case C_INDENT: if (d && !d->readonly) ed_indent(d, false); break; case C_OUTDENT: if (d && !d->readonly) ed_indent(d, true); break;
    case C_MOVEUP: if (d && !d->readonly) ed_move_lines(d, -1); break; case C_MOVEDOWN: if (d && !d->readonly) ed_move_lines(d, 1); break;
    case C_FORMAT: if (d && !d->readonly) ed_format(d); break;
    case C_COMPLETE: if (d && d->lang != LANG_PLAIN) ed_trigger_complete(true); break;
    case C_FIND: if (d) { find_open(false); } break; case C_REPLACE: if (d) find_open(true); break;
    case C_FINDNEXT: if (d) { if (!G.fs.find[0]) find_open(false); else find_step(1, false); } break;
    case C_FINDPREV: if (d) { if (!G.fs.find[0]) find_open(false); else find_step(-1, false); } break;
    case C_GOTOLINE: if (d) ov_prompt("Go to Line[:Column]", "", false, goto_line_cb, NULL); break;
    case C_GOTOSYM: goto_symbol(); break; case C_GOTODEF: goto_def(); break;
    case C_FINDPROJ: ov_prompt("Find in Project", d && d->has_sel ? "" : "", false, proj_search_cb, NULL); break;
    case C_NAVBACK: nav_go(-1); break; case C_NAVFWD: nav_go(1); break;
    case C_SIDEBAR: G.cfg.show_sidebar = !G.cfg.show_sidebar; if (!G.cfg.show_sidebar && G.focus == FOCUS_TREE) G.focus = FOCUS_EDITOR; ui_layout(); break;
    case C_PANEL: panel_show(!G.show_panel); break;
    case C_THEME: { PItem *it = xcalloc((size_t)N_THEMES, sizeof *it); for (int i = 0; i < N_THEMES; i++) { it[i].label = xstrdup(THEMES[i].name); it[i].id = i; } ov_picker("Color Theme", it, N_THEMES, false, theme_cb, NULL); G.ov.sel = G.cfg.theme; break; }
    case C_WHITESPACE: G.cfg.show_ws = !G.cfg.show_ws; cfg_save(); break; case C_LINENUMS: G.cfg.line_numbers = !G.cfg.line_numbers; cfg_save(); break;
    case C_AUTOPAIRS: G.cfg.auto_pairs = !G.cfg.auto_pairs; status(0, "Auto-close brackets: %s", G.cfg.auto_pairs ? "on" : "off"); cfg_save(); break;
    case C_CHECKSAVE: G.cfg.check_on_save = !G.cfg.check_on_save; status(0, "Check on save: %s", G.cfg.check_on_save ? "on" : "off"); cfg_save(); break;
    case C_NEXTTAB: if (G.ndocs > 1) doc_activate((G.active + 1) % G.ndocs); break; case C_PREVTAB: if (G.ndocs > 1) doc_activate((G.active + G.ndocs - 1) % G.ndocs); break;
    case C_PALETTE: palette(); break;
    case C_FOCUS_TREE: if (!G.cfg.show_sidebar) { G.cfg.show_sidebar = true; ui_layout(); } G.focus = G.focus == FOCUS_TREE ? FOCUS_EDITOR : FOCUS_TREE; break;
    case C_FOCUS_EDITOR: G.focus = FOCUS_EDITOR; break;
    case C_FOCUS_PANEL: if (G.focus == FOCUS_PANEL) G.focus = FOCUS_EDITOR; else panel_set_tab(G.ptab, true); break;
    case C_TERMINAL: if (G.show_panel && G.ptab == PT_TERMINAL && G.focus == FOCUS_PANEL) G.focus = FOCUS_EDITOR; else panel_set_tab(PT_TERMINAL, true); break;
    case C_NEWTERM: term_new(); break; case C_KILLTERM: term_kill_current(); break;
    case C_CONSOLE: if (G.show_panel && G.ptab == PT_CONSOLE && G.focus == FOCUS_PANEL) G.focus = FOCUS_EDITOR; else panel_set_tab(PT_CONSOLE, true); break;
    case C_CONSOLE_CLEAR: console_clear(); break; case C_CONSOLE_RESTART: console_restart(false); break; case C_CONSOLE_LOAD: panel_set_tab(PT_CONSOLE, true); console_restart(true); break;
    case C_DEBUGPANEL: if (G.show_panel && G.ptab == PT_DEBUG && G.focus == FOCUS_PANEL) G.focus = FOCUS_EDITOR; else panel_set_tab(PT_DEBUG, true); break;
    case C_DEBUG: dbg_start(false); break;
    case C_DBG_CONT: case C_DBG_OVER: case C_DBG_IN: case C_DBG_OUT: case C_DBG_PAUSE: case C_DBG_STOP: dbg_command(id); break;
    case C_TOGGLE_BP: if (d) dbg_toggle_bp(d, d->cur.line); break;
    case C_CLEAR_BPS: { int n = 0; for (int i = 0; i < G.ndocs; i++) for (int k = 0; k < G.docs[i]->nlines; k++) if (G.docs[i]->lines[k].flags & LF_BREAK) { G.docs[i]->lines[k].flags &= (uint8_t)~LF_BREAK; n++; } dbg_clear_breakpoints(); status(0, "Removed %d breakpoint%s", n, n == 1 ? "" : "s"); break; }
    case C_ADD_WATCH: panel_set_tab(PT_DEBUG, false); ov_prompt("Add watch expression", d && d->has_sel ? "" : "", false, add_watch_prompt_cb, NULL); break;
    case C_MENU: ui_open_menu(0); break;
    case C_NEWFILE: ov_prompt("New file (relative to selected folder)", "", false, newfile_cb, NULL); break;
    case C_NEWDIR: ov_prompt("New folder", "", false, newdir_cb, NULL); break;
    case C_RENAME: { TNode *n = tree_target(); if (n && n != G.tree) ov_prompt("Rename to", path_basename(n->path), false, rename_cb, xstrdup(n->path)); break; }
    case C_DELETE: { TNode *n = tree_target(); if (n && n != G.tree) { char m[600]; snprintf(m, sizeof m, "Delete %s?", n->name); ov_confirm(m, "Delete", "Cancel", NULL, delete_cb, xstrdup(n->path)); } break; }
    case C_REFRESH: tree_build(); G.indexed = false; status(0, "Explorer refreshed"); break;
    case C_RUN: if (G.dbg.state == DS_PAUSED) { dbg_command(C_DBG_CONT); break; }
        /* fall through */
    case C_CHECK: case C_TEST: case C_TESTALL: case C_LEAKS: start_proc(id, false); break;
    case C_RUNTERM: run_in_terminal(C_RUN); break; case C_REPL: run_in_terminal(C_REPL); break;
    case C_STOP: if (dbg_active()) dbg_stop(); else if (G.proc.running) { proc_stop(&G.proc); status(2, "Process stopped"); } break;
    case C_NEXTDIAG: diag_goto(1); break; case C_PREVDIAG: diag_goto(-1); break;
    case C_CLEAROUT: out_clear(&G.out); break;
    case C_KEYS: show_keys(); break; case C_ABOUT: about(); break;
    default: break;
    }
}
