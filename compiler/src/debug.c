/* Source-level debugger: breakpoints, stepping, call stack, variables, expression evaluation.
 * Author: Divyanshu Sinha. See include/debug.h and docs/DEBUGGER.md. */
#include "debug.h"
#include "parser.h"
#include <stdarg.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#endif

int kc_dbg_on = 0;
extern void (*kc_error_debug_hook)(KcPos pos, const char *msg);           /* errors.c */
KcValue kc_interp_eval_node(KcInterp *ip, KcEnv *env, KcNode *expr);       /* interp.c */

/* ---------------------------------------------------------------- channel */
#ifdef _WIN32
static HANDLE h_in = INVALID_HANDLE_VALUE, h_out = INVALID_HANDLE_VALUE;
static bool chan_open(const char *base) {
    char p[600];
    snprintf(p, sizeof p, "%s.cmd", base); h_in = CreateFileA(p, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    snprintf(p, sizeof p, "%s.evt", base); h_out = CreateFileA(p, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    return h_in != INVALID_HANDLE_VALUE && h_out != INVALID_HANDLE_VALUE;
}
static int rd_byte(bool block) {
    unsigned char c; DWORD n = 0;
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(h_in, NULL, 0, NULL, &avail, NULL)) return -2;
        if (avail) { if (!ReadFile(h_in, &c, 1, &n, NULL) || n != 1) return -2; return c; }
        if (!block) return -1;
        Sleep(5);
    }
}
static void wr(const char *d, size_t n) { DWORD w; while (n) { if (!WriteFile(h_out, d, (DWORD)n, &w, NULL) || !w) { kc_dbg_on = 0; return; } d += w; n -= w; } }
#else
static int fd_in = -1, fd_out = -1;
static bool chan_open(const char *base) {
    char p[600];
    snprintf(p, sizeof p, "%s.cmd", base); fd_in = open(p, O_RDONLY);
    snprintf(p, sizeof p, "%s.evt", base); fd_out = open(p, O_WRONLY);
    signal(SIGPIPE, SIG_IGN);
    return fd_in >= 0 && fd_out >= 0;
}
static int rd_byte(bool block) {
    unsigned char c; struct pollfd pf = { fd_in, POLLIN, 0 };
    int r = poll(&pf, 1, block ? -1 : 0);
    if (r <= 0) return -1;
    ssize_t n = read(fd_in, &c, 1);
    if (n == 1) return c;
    return -2;                                                         /* writer closed */
}
static void wr(const char *d, size_t n) { while (n) { ssize_t w = write(fd_out, d, n); if (w <= 0) { kc_dbg_on = 0; return; } d += w; n -= (size_t)w; } }
#endif

static char lb[8192]; static int ln_;
/* returns 1 = a full line is in `out`, 0 = nothing yet, -1 = channel closed */
static int rd_line(char *out, size_t cap, bool block) {
    for (;;) {
        int c = rd_byte(block);
        if (c == -2) return -1;
        if (c == -1) return 0;
        if (c == '\n') { lb[ln_] = 0; snprintf(out, cap, "%s", lb); ln_ = 0; return 1; }
        if (c != '\r' && ln_ < (int)sizeof lb - 1) lb[ln_++] = (char)c;
    }
}
static void send_line(const char *fmt, ...) {
    char b[4096]; va_list ap; va_start(ap, fmt); int n = vsnprintf(b, sizeof b - 1, fmt, ap); va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof b - 2) n = (int)sizeof b - 2;
    b[n++] = '\n'; wr(b, (size_t)n);
}

/* ---------------------------------------------------------------- state */
typedef enum { M_RUN, M_STEP_IN, M_STEP_OVER, M_STEP_OUT } Mode;
typedef struct { const char *file; int line; KcEnv *env; KcEnv *fenv; const char *name; } Frame;
#define MAXF 4100
static Frame fr[MAXF];
static KcInterp *g_ip;
static Mode mode = M_RUN;
static int step_depth, step_line; static const char *step_file;
static int last_depth = -1, last_line = -1; static const char *last_file;
static int pause_depth; static bool in_pause, pause_req; static unsigned tick;
typedef struct { char *file; int line; } Bp;
static Bp bps[1024]; static int nbp;

static const char *real_of(const char *f) {                           /* path normalisation, cached by pointer */
    static struct { const char *p; char r[1024]; } cache[8]; static int next;
    if (!f) return "";
    for (int i = 0; i < 8; i++) if (cache[i].p == f) return cache[i].r;
    int k = next++ & 7; cache[k].p = f;
#ifdef _WIN32
    if (!_fullpath(cache[k].r, f, sizeof cache[k].r)) snprintf(cache[k].r, sizeof cache[k].r, "%.1000s", f);
#else
    char tmp[PATH_MAX]; if (f[0] != '<' && realpath(f, tmp)) { size_t n = strlen(tmp); if (n > sizeof cache[k].r - 1) n = sizeof cache[k].r - 1; memcpy(cache[k].r, tmp, n); cache[k].r[n] = 0; } else snprintf(cache[k].r, sizeof cache[k].r, "%.1000s", f);
#endif
    return cache[k].r;
}
static bool path_same(const char *a, const char *b) {
#ifdef _WIN32
    return _stricmp(a, b) == 0;
#else
    return strcmp(a, b) == 0;
#endif
}
static void bp_set(const char *file, int line, bool on) {
    char tmp[1024];
#ifdef _WIN32
    if (!_fullpath(tmp, file, sizeof tmp)) snprintf(tmp, sizeof tmp, "%s", file);
#else
    char rp[PATH_MAX]; const char *src = realpath(file, rp) ? rp : file; size_t sl = strlen(src); if (sl > sizeof tmp - 1) sl = sizeof tmp - 1; memcpy(tmp, src, sl); tmp[sl] = 0;
#endif
    for (int i = 0; i < nbp; i++) if (bps[i].line == line && path_same(bps[i].file, tmp)) { if (!on) { free(bps[i].file); bps[i] = bps[--nbp]; } return; }
    if (on && nbp < 1024) { bps[nbp].file = kc_strdup(tmp); bps[nbp].line = line; nbp++; }
}

/* ---------------------------------------------------------------- formatting */
static char *clean(const char *s, int max) {
    size_t n = strlen(s); char *o = kc_malloc(n * 2 + 8); size_t k = 0; int cols = 0;
    for (size_t i = 0; i < n && cols < max; i++, cols++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\n') { o[k++] = '\\'; o[k++] = 'n'; } else if (c == '\t') { o[k++] = '\\'; o[k++] = 't'; } else if (c == '\r') { o[k++] = '\\'; o[k++] = 'r'; }
        else if (c < 32) o[k++] = '?'; else o[k++] = (char)c;
    }
    if (cols >= max && n > (size_t)max) { memcpy(o + k, "...", 3); k += 3; }
    o[k] = 0; return o;
}
static char *val_text(KcValue v) {
    char *d = kc_to_display_string(v), *c;
    if (v.type == VAL_STRING) { char *q = kc_malloc(strlen(d) + 3); sprintf(q, "\"%s\"", d); c = clean(q, 300); free(q); }
    else c = clean(d, 300);
    free(d); return c;
}

/* ---------------------------------------------------------------- commands */
static void send_stack(void) {
    int n = pause_depth + 1; if (n > 200) n = 200;
    send_line("stack\t%d", n);
    for (int k = 0; k < n; k++) {
        int dd = pause_depth - k; if (dd < 0 || dd >= MAXF) break;
        send_line("frame\t%d\t%s\t%s\t%d", k, dd == 0 ? "<module>" : (fr[dd].name ? fr[dd].name : "<fn>"), real_of(fr[dd].file), fr[dd].line);
    }
    send_line("end");
}
static bool is_callable(KcValType t) { return t == VAL_CLOSURE || t == VAL_NATIVE || t == VAL_MODULE || t == VAL_EXTERN || t == VAL_NATIVE_LIB; }
static void send_vars(int k) {
    int dd = pause_depth - k; send_line("vars\t%d", k);
    if (dd >= 0 && dd < MAXF) {
        KcEnv *e = fr[dd].env, *boundary = dd > 0 ? fr[dd].fenv : NULL; bool local = true; int sent = 0;
        while (e && e != g_ip->builtins_env && sent < 600) {
            if (dd == 0 && e == g_ip->globals) local = false;
            for (int i = 0; i < e->count && sent < 600; i++) {
                KcEnvEntry *en = &e->entries[i];
                if (!local && is_callable(en->value.type)) continue;
                char *vt = val_text(en->value);
                send_line("var\t%s\t%s\t%s\t%s", local ? "local" : "global", en->name, kc_type_name(en->value), vt); free(vt); sent++;
            }
            if (e == boundary) local = false;
            e = e->parent;
        }
    }
    send_line("end");
}
static void do_eval(int k, const char *expr) {
    int dd = pause_depth - k; if (dd < 0 || dd >= MAXF) dd = pause_depth;
    KcErrCatch c; c.quiet = true; volatile bool ok = false; KcValue res = kc_null(); volatile bool assigned = false;
    int saved = g_ip->call_depth, sunsafe = g_ip->in_unsafe, scomp = g_ip->in_comptime;
    kc_dbg_on = 1;
    if (KC_TRY(&c) == 0) {
        size_t el = strlen(expr); char *ex2 = kc_malloc(el + 3); memcpy(ex2, expr, el); ex2[el] = 0;
        while (el > 0 && (ex2[el - 1] == ' ' || ex2[el - 1] == ';')) ex2[--el] = 0;
        strcat(ex2, ";");
        KcNode *prog = kc_parse_program(ex2, "<debug>");
        if (prog->as.program.stmts.count < 1) kc_error_at((KcPos){ "<debug>", 1, 1 }, "nothing to evaluate");
        KcNode *st = prog->as.program.stmts.items[0];
        if (st->kind != N_EXPRSTMT) kc_error_at(st->pos, "only expressions (and assignments) can be evaluated here");
        assigned = st->as.exprstmt.expr->kind == N_ASSIGN;
        res = kc_interp_eval_node(g_ip, fr[dd].env, st->as.exprstmt.expr);
        kc_try_end(&c); ok = true;
    } else { kc_try_end(&c); g_ip->call_depth = saved; g_ip->in_unsafe = sunsafe; g_ip->in_comptime = scomp; g_ip->has_thrown = false; }
    if (ok) { char *t = val_text(res); send_line("result\tok\t%s\t%s", assigned ? "assigned" : kc_type_name(res), assigned ? "(assigned)" : t); free(t); }
    else { char *m = clean(c.msg[0] ? c.msg : "evaluation failed", 400); send_line("result\terr\terror\t%s", m); free(m); }
}
/* handles one command line; returns true when it resumes execution */
static bool handle_cmd(char *line, bool paused) {
    char *save = NULL, *cmd = strtok_r(line, "\t", &save);
    if (!cmd) return false;
    if (!strcmp(cmd, "break") || !strcmp(cmd, "unbreak")) {
        char *f = strtok_r(NULL, "\t", &save), *l = strtok_r(NULL, "\t", &save); if (f && l) bp_set(f, atoi(l), cmd[0] == 'b');
    } else if (!strcmp(cmd, "clear")) { for (int i = 0; i < nbp; i++) free(bps[i].file); nbp = 0; }
    else if (!strcmp(cmd, "pause")) pause_req = true;
    else if (!strcmp(cmd, "quit")) { kc_dbg_on = 0; exit(0); }
    else if (!strcmp(cmd, "go") || !strcmp(cmd, "continue")) { mode = M_RUN; return true; }
    else if (!strcmp(cmd, "entry") || !strcmp(cmd, "stepin")) { mode = M_STEP_IN; return true; }
    else if (!strcmp(cmd, "stepover") || !strcmp(cmd, "stepout")) {
        mode = cmd[4] == 'o' && cmd[5] == 'v' ? M_STEP_OVER : M_STEP_OUT;
        step_depth = paused ? pause_depth : last_depth; step_line = last_line; step_file = last_file; return true;
    } else if (paused && !strcmp(cmd, "stack")) send_stack();
    else if (paused && !strcmp(cmd, "vars")) { char *k = strtok_r(NULL, "\t", &save); send_vars(k ? atoi(k) : 0); }
    else if (paused && !strcmp(cmd, "eval")) { char *k = strtok_r(NULL, "\t", &save), *e = strtok_r(NULL, "", &save); do_eval(k ? atoi(k) : 0, e ? e : ""); }
    return false;
}
static void poll_cmds(void) {
    char b[4096];
    while (kc_dbg_on && rd_line(b, sizeof b, false) == 1) handle_cmd(b, false);
}
static void pause_here(const char *reason, const char *msg, const char *file, int line) {
    fflush(stdout); fflush(stderr);
    in_pause = true; pause_depth = g_ip->call_depth; if (pause_depth >= MAXF) pause_depth = MAXF - 1; pause_req = false;
    char *m = clean(msg ? msg : "", 400);
    send_line("stopped\t%s\t%s\t%d\t%d\t%s", reason, real_of(file), line, pause_depth, m); free(m);
    char b[8192];
    while (kc_dbg_on) {
        int r = rd_line(b, sizeof b, true);
        if (r < 0) { kc_dbg_on = 0; break; }
        if (r == 1 && handle_cmd(b, true)) break;
    }
    in_pause = false;
}

/* ---------------------------------------------------------------- hooks */
void kc_dbg_enter(KcInterp *ip, KcEnv *fenv, KcClosure *cl, KcPos callpos) {
    int d = ip->call_depth; if (d <= 0 || d >= MAXF) return;
    fr[d] = (Frame){ callpos.file, callpos.line, fenv, fenv, cl && cl->name ? cl->name : "<fn>" };
}
void kc_dbg_stmt(KcInterp *ip, KcEnv *env, KcNode *n) {
    if (in_pause) return;
    switch (n->kind) { case N_BLOCK: case N_PROGRAM: case N_FN_DECL: case N_STRUCT_DECL: case N_IMPORT: case N_EXTERN_DECL: case N_TYPE_ALIAS:
    case N_LABEL: case N_MULTI: case N_ENUM_DECL: case N_IMPL: return;     /* declarations and containers: their inner statements are hooked on their own */ default: break; }
    int d = ip->call_depth; if (d >= MAXF) d = MAXF - 1;
    Frame *f = &fr[d]; f->file = n->pos.file; f->line = n->pos.line; f->env = env;
    if ((++tick & 511) == 0) poll_cmds();
    if (!kc_dbg_on) return;
    int line = n->pos.line; const char *file = n->pos.file;
    bool changed = d != last_depth || line != last_line || file != last_file;
    const char *reason = NULL;
    if (pause_req) reason = "pause";
    else if (mode == M_STEP_IN && changed) reason = "step";
    else if (mode == M_STEP_OVER && d <= step_depth && (d < step_depth || line != step_line || file != step_file)) reason = "step";
    else if (mode == M_STEP_OUT && d < step_depth) reason = "step";
    if (!reason && nbp && changed) { const char *rf = real_of(file); for (int i = 0; i < nbp; i++) if (bps[i].line == line && path_same(bps[i].file, rf)) { reason = "breakpoint"; break; } }
    last_depth = d; last_line = line; last_file = file;
    if (reason) pause_here(reason, "", file, line);
}
static void on_fatal(KcPos pos, const char *msg) {
    if (!kc_dbg_on || in_pause) return;
    kc_error_debug_hook = NULL;
    int d = g_ip->call_depth; if (d < MAXF) { fr[d].file = pos.file; fr[d].line = pos.line; }
    pause_here("exception", msg, pos.file, pos.line);
}
void kc_dbg_finish(void) { if (kc_dbg_on) { fflush(stdout); send_line("exited"); } }
bool kc_dbg_init(KcInterp *ip, const char *base, char *err, size_t cap) {
    if (!base || !*base) { snprintf(err, cap, "`kingcobra debug` is driven by an IDE: set KC_DEBUG_PIPE (see docs/DEBUGGER.md)"); return false; }
    if (!chan_open(base)) { snprintf(err, cap, "cannot open debug pipes at %s.cmd / %s.evt", base, base); return false; }
    g_ip = ip; kc_dbg_on = 1; fr[0] = (Frame){ ip->filename, 1, ip->globals, ip->globals, "<module>" };
    kc_error_debug_hook = on_fatal; atexit(kc_dbg_finish);
    send_line("ready\t%d", 1);
    char b[8192];
    for (;;) {                                                                  /* setup phase: breakpoints until `go` / `entry` */
        int r = rd_line(b, sizeof b, true);
        if (r < 0) { kc_dbg_on = 0; snprintf(err, cap, "debug channel closed"); return false; }
        if (r == 1 && handle_cmd(b, false)) break;
    }
    return true;
}
