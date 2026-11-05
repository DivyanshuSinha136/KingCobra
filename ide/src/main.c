/* kcide — KingCobra IDE entry point: configuration, status messages and the main event loop. */
#include "ide.h"
#include <stdarg.h>
#ifdef _WIN32
#include <direct.h>
#define getcwd _getcwd
#define chdir _chdir
#else
#include <unistd.h>
#endif
extern void kc_discover(void);
Ide G;

void status(int kind, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vsnprintf(G.msg, sizeof G.msg, fmt, ap); va_end(ap);
    G.msg_kind = kind; G.msg_until = now_ms() + (kind >= 2 ? 6000 : 3500);
}

static char *cfg_path(void) { char *d = home_config_dir(); if (!d) return NULL; char *p = path_join(d, "config"); free(d); return p; }
static void cfg_defaults(void) {
    memset(&G.cfg, 0, sizeof G.cfg);
    G.cfg.tab_width = 4; G.cfg.use_spaces = true; G.cfg.check_on_save = true; G.cfg.save_before_run = true; G.cfg.auto_complete = true; G.cfg.trim_ws = false;
    G.cfg.show_ws = false; G.cfg.show_sidebar = true; G.cfg.line_numbers = true; G.cfg.auto_pairs = true; G.cfg.theme = 0;
}
void cfg_load(void) {
    cfg_defaults(); char *p = cfg_path(); if (!p) return; char *t = read_file(p, NULL); free(p); if (!t) return;
    for (char *line = strtok(t, "\n"); line; line = strtok(NULL, "\n")) {
        char *eq = strchr(line, '='); if (!eq || line[0] == '#') continue; *eq = 0; char *k = line, *v = eq + 1;
        while (*k == ' ') k++; char *ke = k + strlen(k); while (ke > k && ke[-1] == ' ') *--ke = 0; while (*v == ' ') v++; char *ve = v + strlen(v); while (ve > v && (ve[-1] == ' ' || ve[-1] == '\r')) *--ve = 0;
        bool b = !strcmp(v, "true") || !strcmp(v, "1") || !strcmp(v, "yes");
        if (!strcmp(k, "tab_width")) { int n = atoi(v); if (n >= 1 && n <= 16) G.cfg.tab_width = n; }
        else if (!strcmp(k, "use_spaces")) G.cfg.use_spaces = b; else if (!strcmp(k, "check_on_save")) G.cfg.check_on_save = b;
        else if (!strcmp(k, "save_before_run")) G.cfg.save_before_run = b; else if (!strcmp(k, "auto_complete")) G.cfg.auto_complete = b;
        else if (!strcmp(k, "trim_trailing_whitespace")) G.cfg.trim_ws = b; else if (!strcmp(k, "show_whitespace")) G.cfg.show_ws = b;
        else if (!strcmp(k, "show_sidebar")) G.cfg.show_sidebar = b; else if (!strcmp(k, "line_numbers")) G.cfg.line_numbers = b; else if (!strcmp(k, "auto_pairs")) G.cfg.auto_pairs = b;
        else if (!strcmp(k, "kc_path")) snprintf(G.cfg.kc_path, sizeof G.cfg.kc_path, "%s", v);
        else if (!strcmp(k, "theme")) { for (int i = 0; i < N_THEMES; i++) if (!strcmp(THEMES[i].name, v)) G.cfg.theme = i; }
    }
    free(t);
}
void cfg_save(void) {
    char *p = cfg_path(); if (!p) return; Str s; str_init(&s);
    str_adds(&s, "# KingCobra IDE configuration\n");
    str_addf(&s, "theme = %s\ntab_width = %d\nuse_spaces = %s\ncheck_on_save = %s\nsave_before_run = %s\nauto_complete = %s\ntrim_trailing_whitespace = %s\nshow_whitespace = %s\nshow_sidebar = %s\nline_numbers = %s\nauto_pairs = %s\n",
        THEMES[G.cfg.theme].name, G.cfg.tab_width, G.cfg.use_spaces ? "true" : "false", G.cfg.check_on_save ? "true" : "false", G.cfg.save_before_run ? "true" : "false", G.cfg.auto_complete ? "true" : "false",
        G.cfg.trim_ws ? "true" : "false", G.cfg.show_ws ? "true" : "false", G.cfg.show_sidebar ? "true" : "false", G.cfg.line_numbers ? "true" : "false", G.cfg.auto_pairs ? "true" : "false");
    if (G.cfg.kc_path[0]) str_addf(&s, "kc_path = %s\n", G.cfg.kc_path);
    write_file_atomic(p, s.p, s.n); str_free(&s); free(p);
}

static void usage(void) {
    puts("kcide " KCIDE_VERSION " - KingCobra IDE (terminal)\n\nUsage: kcide [options] [file|dir ...]\n\n  -h, --help      show this help\n  -v, --version   print version\n  --kc PATH       path to the kingcobra executable (or set KINGCOBRA)\n\n"
         "A directory argument becomes the project root. Press F1 inside the IDE for the command palette.");
}
static char *exe_dir_of(const char *argv0) {
    char buf[1024];
#ifndef _WIN32
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1); if (n > 0) { buf[n] = 0; return path_dirname(buf); }
#endif
    if (strchr(argv0, '/') || strchr(argv0, '\\')) { char *a = path_abs(argv0); char *d = path_dirname(a); free(a); return d; }
    if (find_in_path(argv0, buf, sizeof buf)) return path_dirname(buf);
    return xstrdup(".");
}

int main(int argc, char **argv) {
    char **files = xcalloc((size_t)argc + 1, sizeof *files); int nfiles = 0; const char *root_arg = NULL; const char *kc_arg = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(); return 0; }
        if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--version")) { puts("kcide " KCIDE_VERSION); return 0; }
        if (!strcmp(argv[i], "--kc") && i + 1 < argc) { kc_arg = argv[++i]; continue; }
        if (is_dir(argv[i])) root_arg = argv[i]; else files[nfiles++] = argv[i];
    }
    memset(&G, 0, sizeof G); cfg_load();
    G.th = (Theme *)&THEMES[G.cfg.theme]; G.proc.fd = G.proc.wfd = -1; G.sb_w = 28; G.panel_h = 11; G.show_panel = false; G.focus = FOCUS_EDITOR; G.fs.field = -1; G.menu_open = -1; G.navpos = -1;
    G.exe_dir = exe_dir_of(argv[0]);
    if (kc_arg) snprintf(G.cfg.kc_path, sizeof G.cfg.kc_path, "%s", kc_arg);
    if (root_arg) G.root = path_abs(root_arg);
    else if (nfiles) { char *a = path_abs(files[0]); char cwd[1024]; G.root = (getcwd(cwd, sizeof cwd) && !strncmp(a, cwd, strlen(cwd)) && a[strlen(cwd)] == PATH_SEP) ? xstrdup(cwd) : path_dirname(a); free(a); }
    else { char cwd[1024]; G.root = getcwd(cwd, sizeof cwd) ? xstrdup(cwd) : xstrdup("."); }
    if (chdir(G.root) != 0) {}
    out_init(&G.out, G.th->fg); panel_init(); dbg_init();
    kc_discover();
    if (!term_open()) { fprintf(stderr, "kcide: needs an interactive terminal (stdin/stdout must be a TTY)\n"); return 1; }
    tree_build(); ui_layout();
    for (int i = 0; i < nfiles; i++) doc_open_path(files[i], true);
    if (!G.ndocs) { status(0, "Welcome to KingCobra IDE %s - Ctrl+O opens a file, F1 shows all commands", KCIDE_VERSION); }
    ui_layout();
    while (!G.quit) {
        ui_draw(); scr_flush();
        Event ev; bool pr = false;
        int fds[16], nf = 0; if (G.proc.running && G.proc.fd >= 0) fds[nf++] = G.proc.fd; if (dbg_fd() >= 0) fds[nf++] = dbg_fd(); nf += panel_fds(fds + nf, 16 - nf);
        bool busy = G.proc.running || nf > 0 || G.nterms > 0 || G.con.started || dbg_active();
        int wait = busy ? (G.proc.running ? 40 : 60) : 250;
#ifdef _WIN32
        if (busy) wait = 30;
#endif
        bool got = term_next_event(&ev, wait, fds, nf, &pr);
        if (G.proc.running || G.proc.fd >= 0) run_poll();
        dbg_poll(); panel_poll();
        if (got) {
            ui_event(&ev);
            while (term_next_event(&ev, 0, NULL, 0, NULL)) { ui_event(&ev); if (G.quit) break; }
        }
        /* drop stale diagnostics for a closed doc is unnecessary; keep file mtime check cheap */
        Doc *d = cur_doc();
        if (d && d->path && !d->dirty && d->mtime) { static uint64_t last; uint64_t t = now_ms(); if (t - last > 1500) { last = t; long m = file_mtime(d->path); if (m && m != d->mtime) { char e[200]; if (doc_reload(d, e, sizeof e)) status(2, "%s changed on disk - reloaded", d->title); } } }
    }
    dbg_shutdown(); panel_free();
    term_close();
    if (G.proc.running) proc_stop(&G.proc);
    return 0;
}
