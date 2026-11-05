/* KingCobra std.system — running other programs.
 *
 *   one-shot    run(cmd)            through the shell, captures stdout + stderr separately
 *               run_argv([..])      NO shell: argv is passed verbatim, so no quoting/injection bugs
 *               output(cmd)         shell, returns stdout (trailing newline stripped) or null on failure
 *               exec(cmd)           (system.c) shell with inherited stdio, returns the exit code
 *   long-lived  spawn / wait / poll / kill / proc_read / proc_read_err / proc_write /
 *               proc_close_stdin / proc_pid / proc_free
 *
 * Capturing pumps stdout, stderr and stdin together with poll() (POSIX) or
 * pipe polling (Windows), so a child that writes a lot while we feed it input
 * cannot deadlock either side. Timeouts are real: the child is sent SIGTERM,
 * given 0.5 s, then SIGKILL; captured children run in their own process group
 * so grandchildren of `sh -c` are killed with it (POSIX).
 *
 * Exit status: the normal exit code; if the child died from a signal, 128 +
 * signal (the same convention a shell's $? uses) with `signal` set too.
 * A command that cannot even be started (not found, bad cwd) returns null and
 * sets system.last_error().
 *
 * Everything here is refused inside comptime { }.
 *
 * Windows: implemented with CreateProcess + anonymous pipes; killing
 * terminates only the process itself (no job object), stdin input is limited
 * to 512 KiB per write so a full pipe can never block the interpreter.
 *
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com>
 */
#include "system_glue.h"
#if !defined(KC_PLATFORM_WINDOWS)
#include <sys/wait.h>
#include <sys/types.h>
#include <poll.h>
#include <signal.h>
#include <fcntl.h>
#endif

typedef struct {
    bool capture, done, own_group, auto_close_stdin;
    int  code, sig;
    SysBuf out, err, in;
    size_t out_read, err_read, in_off;
#if defined(KC_PLATFORM_WINDOWS)
    HANDLE hproc, hin, hout, herr; DWORD pid;
#else
    pid_t pid; int fd_in, fd_out, fd_err;
#endif
} Proc;

#define WIN_MAX_INPUT (512 * 1024)

/* ---------------------------- argv handling ---------------------------- */

static char **build_argv(KcValue arr, KcPos pos, const char *sig) {
    if (arr.type != VAL_ARRAY) sys_usage(pos, sig);
    int c = arr.as.arr->count;
    if (c < 1) kc_error_at(pos, "%s: argv must contain at least the program name", sig);
    char **v = kc_malloc(sizeof(char *) * (size_t)(c + 1));
    for (int i = 0; i < c; i++) {
        KcValue e = arr.as.arr->items[i];
        if (e.type != VAL_STRING) kc_error_at(pos, "%s: argv[%d] must be a string, got %s", sig, i, kc_type_name(e));
        if (memchr(e.as.s->data, 0, (size_t)e.as.s->len + 0) != NULL && (int)strlen(e.as.s->data) != e.as.s->len)
            kc_error_at(pos, "%s: argv[%d] contains a NUL byte", sig, i);
        v[i] = kc_strdup(e.as.s->data);
    }
    v[c] = NULL;
    if (!v[0][0]) kc_error_at(pos, "%s: argv[0] (the program) is empty", sig);
    return v;
}
static void free_argv(char **v) { for (int i = 0; v[i]; i++) free(v[i]); free(v); }

/* ---------------------- platform launch / pump / kill ---------------------- */

#if defined(KC_PLATFORM_WINDOWS)

static void win_quote(SysBuf *b, const char *a) {
    if (*a && !strpbrk(a, " \t\n\v\"")) { sysbuf_append(b, a, strlen(a)); return; }
    sysbuf_append(b, "\"", 1);
    for (const char *c = a;; c++) {
        size_t bs = 0; while (*c == '\\') { bs++; c++; }
        if (!*c) { for (size_t i = 0; i < bs * 2; i++) sysbuf_append(b, "\\", 1); break; }
        if (*c == '"') { for (size_t i = 0; i < bs * 2 + 1; i++) sysbuf_append(b, "\\", 1); sysbuf_append(b, "\"", 1); }
        else { for (size_t i = 0; i < bs; i++) sysbuf_append(b, "\\", 1); sysbuf_append(b, c, 1); }
    }
    sysbuf_append(b, "\"", 1);
}

static void close_h(HANDLE *h) { if (*h && *h != INVALID_HANDLE_VALUE) CloseHandle(*h); *h = NULL; }

static void pipe_drain(HANDLE *h, SysBuf *b) {
    if (!*h) return;
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(*h, NULL, 0, NULL, &avail, NULL)) { close_h(h); return; } /* broken pipe = EOF */
        if (!avail) return;
        char buf[65536]; DWORD got = 0;
        if (!ReadFile(*h, buf, avail > sizeof buf ? (DWORD)sizeof buf : avail, &got, NULL) || !got) { close_h(h); return; }
        sysbuf_append(b, buf, got);
    }
}

static Proc *launch(char **argv, const char *shell_cmd, bool capture, const char *cwd, const char *who) {
    SysBuf cl = {0};
    if (shell_cmd) { sysbuf_append(&cl, "cmd.exe /d /s /c \"", 18); sysbuf_append(&cl, shell_cmd, strlen(shell_cmd)); sysbuf_append(&cl, "\"", 1); }
    else for (int i = 0; argv[i]; i++) { if (i) sysbuf_append(&cl, " ", 1); win_quote(&cl, argv[i]); }
    Proc *p = kc_malloc(sizeof *p); memset(p, 0, sizeof *p); p->capture = capture;
    HANDLE inR = NULL, outW = NULL, errW = NULL;
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    if (capture) {
        if (!CreatePipe(&inR, &p->hin, &sa, 1 << 20) || !CreatePipe(&p->hout, &outW, &sa, 1 << 20) || !CreatePipe(&p->herr, &errW, &sa, 1 << 20)) {
            sys_set_errno(who); close_h(&inR); close_h(&p->hin); close_h(&p->hout); close_h(&outW); close_h(&p->herr); close_h(&errW);
            sysbuf_free(&cl); free(p); return NULL;
        }
        SetHandleInformation(p->hin, HANDLE_FLAG_INHERIT, 0); SetHandleInformation(p->hout, HANDLE_FLAG_INHERIT, 0); SetHandleInformation(p->herr, HANDLE_FLAG_INHERIT, 0);
    }
    STARTUPINFOA si; memset(&si, 0, sizeof si); si.cb = sizeof si;
    if (capture) { si.dwFlags = STARTF_USESTDHANDLES; si.hStdInput = inR; si.hStdOutput = outW; si.hStdError = errW; }
    PROCESS_INFORMATION pi; memset(&pi, 0, sizeof pi);
    fflush(stdout); fflush(stderr);
    BOOL ok = CreateProcessA(NULL, cl.p, NULL, NULL, capture ? TRUE : FALSE, capture ? CREATE_NO_WINDOW : 0, NULL, cwd, &si, &pi);
    sysbuf_free(&cl);
    close_h(&inR); close_h(&outW); close_h(&errW);
    if (!ok) {
        char what[512]; snprintf(what, sizeof what, "%s: %s", who, shell_cmd ? "cmd.exe" : argv[0]); sys_set_errno(what);
        close_h(&p->hin); close_h(&p->hout); close_h(&p->herr); free(p); return NULL;
    }
    CloseHandle(pi.hThread); p->hproc = pi.hProcess; p->pid = pi.dwProcessId;
    return p;
}

static void proc_finish(Proc *p) {
    DWORD ec = 0; GetExitCodeProcess(p->hproc, &ec);
    p->done = true; p->code = (int)ec; p->sig = 0;
    pipe_drain(&p->hout, &p->out); pipe_drain(&p->herr, &p->err);
    close_h(&p->hout); close_h(&p->herr); close_h(&p->hin); close_h(&p->hproc);
}

static void proc_step(Proc *p, int timeout_ms) {
    if (p->done) return;
    for (int waited = 0;; waited += 5) {
        size_t before = p->out.len + p->err.len;
        pipe_drain(&p->hout, &p->out); pipe_drain(&p->herr, &p->err);
        if (WaitForSingleObject(p->hproc, 0) == WAIT_OBJECT_0) { proc_finish(p); return; }
        if (p->out.len + p->err.len != before || waited >= timeout_ms) return;
        Sleep(5);
    }
}

static bool proc_queue_input(Proc *p, const char *d, size_t n) {
    if (!p->hin) { sys_set_error("system.proc_write: stdin is closed"); return false; }
    if (n > WIN_MAX_INPUT) { sys_set_error("system.proc_write: input larger than %d bytes is not supported on Windows", WIN_MAX_INPUT); return false; }
    DWORD wrote = 0;
    if (!WriteFile(p->hin, d, (DWORD)n, &wrote, NULL) || wrote != n) { sys_set_errno("system.proc_write"); return false; }
    return true;
}
static void proc_close_in(Proc *p) { close_h(&p->hin); }
static bool proc_signal(Proc *p, bool force) { (void)force; return p->hproc && TerminateProcess(p->hproc, 1); }
static void proc_release(Proc *p) { close_h(&p->hin); close_h(&p->hout); close_h(&p->herr); close_h(&p->hproc); }
static long proc_pid_of(Proc *p) { return (long)p->pid; }

#else /* ------------------------------ POSIX ------------------------------ */

static void set_cloexec_nb(int fd) {
    fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
}

static void closefd(int *fd) { if (*fd >= 0) { close(*fd); *fd = -1; } }

static Proc *launch(char **argv, const char *shell_cmd, bool capture, const char *cwd, const char *who) {
    char *sh_argv[4];
    if (shell_cmd) { sh_argv[0] = "/bin/sh"; sh_argv[1] = "-c"; sh_argv[2] = (char *)shell_cmd; sh_argv[3] = NULL; argv = sh_argv; }
    static bool sigpipe_ignored;
    if (!sigpipe_ignored) { signal(SIGPIPE, SIG_IGN); sigpipe_ignored = true; } /* a dead child must give EPIPE, not kill us */

    int in[2] = {-1, -1}, out[2] = {-1, -1}, err[2] = {-1, -1}, ep[2] = {-1, -1};
    if (capture && (pipe(in) || pipe(out) || pipe(err))) {
        sys_set_errno(who); closefd(&in[0]); closefd(&in[1]); closefd(&out[0]); closefd(&out[1]); closefd(&err[0]); closefd(&err[1]);
        return NULL;
    }
    if (pipe(ep)) { sys_set_errno(who); return NULL; }
    fcntl(ep[1], F_SETFD, FD_CLOEXEC);
    fflush(stdout); fflush(stderr); /* don't let the child inherit (and double-flush) our buffered output */
    pid_t pid = fork();
    if (pid < 0) {
        sys_set_errno(who);
        closefd(&ep[0]); closefd(&ep[1]);
        closefd(&in[0]); closefd(&in[1]); closefd(&out[0]); closefd(&out[1]); closefd(&err[0]); closefd(&err[1]);
        return NULL;
    }
    if (pid == 0) {
        struct { int stage, err; } rep;
        if (capture) {
            setpgid(0, 0);
            dup2(in[0], 0); dup2(out[1], 1); dup2(err[1], 2);
            close(in[0]); close(in[1]); close(out[0]); close(out[1]); close(err[0]); close(err[1]);
        }
        signal(SIGPIPE, SIG_DFL);
        if (cwd && chdir(cwd) != 0) { rep.stage = 1; rep.err = errno; (void)!write(ep[1], &rep, sizeof rep); _exit(127); }
        execvp(argv[0], argv);
        rep.stage = 2; rep.err = errno; (void)!write(ep[1], &rep, sizeof rep);
        _exit(127);
    }
    close(ep[1]);
    closefd(&in[0]); closefd(&out[1]); closefd(&err[1]);
    struct { int stage, err; } rep; ssize_t r;
    do { r = read(ep[0], &rep, sizeof rep); } while (r < 0 && errno == EINTR);
    close(ep[0]);
    if (r == (ssize_t)sizeof rep) {                       /* exec/chdir failed in the child */
        int st; waitpid(pid, &st, 0);
        closefd(&in[1]); closefd(&out[0]); closefd(&err[0]);
        char what[600]; errno = rep.err;
        if (rep.stage == 1) snprintf(what, sizeof what, "%s: cannot change directory to '%s'", who, cwd);
        else snprintf(what, sizeof what, "%s: cannot run '%s'", who, argv[0]);
        sys_set_errno(what);
        return NULL;
    }
    Proc *p = kc_malloc(sizeof *p); memset(p, 0, sizeof *p);
    p->pid = pid; p->capture = capture; p->own_group = capture;
    p->fd_in = in[1]; p->fd_out = out[0]; p->fd_err = err[0];
    if (capture) { set_cloexec_nb(p->fd_in); set_cloexec_nb(p->fd_out); set_cloexec_nb(p->fd_err); }
    return p;
}

static void read_some(int *fd, SysBuf *b) {
    if (*fd < 0) return;
    char buf[65536];
    for (;;) {
        ssize_t n = read(*fd, buf, sizeof buf);
        if (n > 0) { sysbuf_append(b, buf, (size_t)n); if ((size_t)n < sizeof buf) return; continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        if (n < 0 && errno == EINTR) continue;
        closefd(fd); return; /* EOF or hard error */
    }
}

static void proc_finish_status(Proc *p, int st) {
    p->done = true;
    if (WIFEXITED(st)) { p->code = WEXITSTATUS(st); p->sig = 0; }
    else if (WIFSIGNALED(st)) { p->sig = WTERMSIG(st); p->code = 128 + p->sig; }
    else p->code = st;
    read_some(&p->fd_out, &p->out); read_some(&p->fd_err, &p->err); /* what the child left in the pipes */
    closefd(&p->fd_out); closefd(&p->fd_err); closefd(&p->fd_in);
}

static void proc_step(Proc *p, int timeout_ms) {
    if (p->done) return;
    struct pollfd pf[3]; int np = 0, ii = -1, io = -1, ie = -1;
    if (p->fd_in >= 0 && p->in_off < p->in.len) { pf[np].fd = p->fd_in; pf[np].events = POLLOUT; ii = np++; }
    if (p->fd_out >= 0) { pf[np].fd = p->fd_out; pf[np].events = POLLIN; io = np++; }
    if (p->fd_err >= 0) { pf[np].fd = p->fd_err; pf[np].events = POLLIN; ie = np++; }
    if (timeout_ms > 50) timeout_ms = 50; /* wake regularly to notice the child exiting */
    if (np) poll(pf, (nfds_t)np, timeout_ms);
    else if (timeout_ms > 0) poll(NULL, 0, timeout_ms);
    if (io >= 0 && pf[io].revents) read_some(&p->fd_out, &p->out);
    if (ie >= 0 && pf[ie].revents) read_some(&p->fd_err, &p->err);
    if (ii >= 0 && p->fd_in >= 0 && pf[ii].revents) {
        ssize_t n = write(p->fd_in, p->in.p + p->in_off, p->in.len - p->in_off);
        if (n > 0) p->in_off += (size_t)n;
        else if (n < 0 && errno != EAGAIN && errno != EINTR) { p->in_off = p->in.len; closefd(&p->fd_in); } /* child closed stdin */
    }
    if (p->fd_in >= 0 && p->auto_close_stdin && p->in_off >= p->in.len) closefd(&p->fd_in);
    int st; pid_t r = waitpid(p->pid, &st, WNOHANG);
    if (r == p->pid) proc_finish_status(p, st);
    else if (r < 0 && errno == ECHILD) { p->done = true; p->code = -1; closefd(&p->fd_out); closefd(&p->fd_err); closefd(&p->fd_in); }
}

static bool proc_queue_input(Proc *p, const char *d, size_t n) {
    if (p->fd_in < 0) { sys_set_error("system.proc_write: stdin is closed"); return false; }
    sysbuf_append(&p->in, d, n);
    return true;
}
static void proc_close_in(Proc *p) { /* flush what is queued, then close */
    p->auto_close_stdin = true;
    if (p->fd_in >= 0 && p->in_off >= p->in.len) closefd(&p->fd_in);
}
static bool proc_signal(Proc *p, bool force) {
    if (p->done) return false;
    int s = force ? SIGKILL : SIGTERM;
    return (p->own_group ? kill(-p->pid, s) : kill(p->pid, s)) == 0;
}
static void proc_release(Proc *p) { closefd(&p->fd_in); closefd(&p->fd_out); closefd(&p->fd_err); }
static long proc_pid_of(Proc *p) { return (long)p->pid; }

#endif

/* ----------------------------- shared logic ----------------------------- */

/* Pump until the process is done or `timeout` seconds pass (<0 = forever). */
static bool proc_wait_done(Proc *p, double timeout) {
    double start = kc_monotonic_seconds();
    for (;;) {
        double left = timeout < 0 ? 1e9 : timeout - (kc_monotonic_seconds() - start);
        if (p->done) return true;
        if (timeout >= 0 && left <= 0) { proc_step(p, 0); return p->done; }
        proc_step(p, left > 0.05 ? 50 : (int)(left * 1000) + 1);
    }
}

static void proc_terminate(Proc *p) { /* SIGTERM, 0.5 s grace, then SIGKILL */
    if (p->done) return;
    proc_signal(p, false);
    if (proc_wait_done(p, 0.5)) return;
    proc_signal(p, true);
    proc_wait_done(p, 2.0);
}

static void free_proc_mem(Proc *p) {
    sysbuf_free(&p->out); sysbuf_free(&p->err); sysbuf_free(&p->in);
    free(p);
}

static KcValue buf_slice(SysBuf *b, size_t from) {
    size_t n = b->len > from ? b->len - from : 0;
    if (!n) return kc_string("");
    char *o = kc_malloc(n + 1); memcpy(o, b->p + from, n);
    return sys_bytes_str(o, n);
}

static KcValue make_handle(Proc *p) {
    KcHandle *h = kc_malloc(sizeof *h);
    h->kind = H_PROC; h->ptr = p; h->size = 0; h->extra = (intptr_t)proc_pid_of(p);
    KcValue v; v.type = VAL_HANDLE; v.as.handle = h; return v;
}

static Proc *get_proc(KcValue v, KcPos pos, const char *who) {
    if (v.type != VAL_HANDLE || v.as.handle->kind != H_PROC) kc_error_at(pos, "%s: expected a process handle from system.spawn()", who);
    if (!v.as.handle->ptr) kc_error_at(pos, "%s: process handle was already freed", who);
    return (Proc *)v.as.handle->ptr;
}

/* Shared body of run / run_argv. */
static KcValue run_common(KcInterp *ip, KcValue *a, int n, KcPos pos, bool use_shell) {
    const char *who = use_shell ? "system.run" : "system.run_argv";
    SYS_IMPURE(ip, pos, who);
    const char *sig = use_shell
        ? "system.run(cmd: string, input: string = null, timeout: float = null, cwd: string = null) -> RunResult"
        : "system.run_argv(argv: array of string, input: string = null, timeout: float = null, cwd: string = null) -> RunResult";
    sys_argc(n, 1, 4, sig, pos);
    char **argv = NULL; const char *cmd = NULL;
    if (use_shell) cmd = sys_str(a, 0, sig, pos); else argv = build_argv(a[0], pos, sig);
    const char *input = NULL; size_t in_len = 0;
    if (!sys_is_nullish(a, n, 1)) { input = sys_str(a, 1, sig, pos); in_len = (size_t)a[1].as.s->len; }
    double timeout = sys_opt_num(a, n, 2, -1.0, sig, pos);
    const char *cwd = sys_is_nullish(a, n, 3) ? NULL : sys_str(a, 3, sig, pos);
    if (!sys_is_nullish(a, n, 2) && timeout < 0) kc_error_at(pos, "%s: timeout must be >= 0", who);

    Proc *p = launch(argv, cmd, true, cwd, who);
    if (argv) free_argv(argv);
    if (!p) return kc_null();
    if (input && in_len) {
        if (!proc_queue_input(p, input, in_len)) { proc_terminate(p); proc_release(p); free_proc_mem(p); return kc_null(); }
    }
    proc_close_in(p); /* run() has no way to send more input later */
    bool done = proc_wait_done(p, timeout);
    bool timed_out = false;
    if (!done) { timed_out = true; proc_terminate(p); }
    proc_release(p);
    static const char *names[] = { "code", "stdout", "stderr", "timed_out", "signal", "ok" };
    KcValue vals[6] = { kc_int(p->code), buf_slice(&p->out, 0), buf_slice(&p->err, 0), kc_bool(timed_out), kc_int(p->sig), kc_bool(!timed_out && p->code == 0) };
    free_proc_mem(p);
    return sys_struct("RunResult", 6, names, vals);
}

static KcValue pr_run(KcInterp *ip, KcValue *a, int n, KcPos pos) { return run_common(ip, a, n, pos, true); }
static KcValue pr_run_argv(KcInterp *ip, KcValue *a, int n, KcPos pos) { return run_common(ip, a, n, pos, false); }

static KcValue pr_output(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    SYS_IMPURE(ip, pos, "system.output");
    const char *sig = "system.output(cmd: string, timeout: float = null) -> string  (stdout minus trailing newline; null if the command fails)";
    sys_argc(n, 1, 2, sig, pos);
    const char *cmd = sys_str(a, 0, sig, pos);
    double timeout = sys_opt_num(a, n, 1, -1.0, sig, pos);
    Proc *p = launch(NULL, cmd, true, NULL, "system.output");
    if (!p) return kc_null();
    proc_close_in(p);
    bool done = proc_wait_done(p, timeout);
    if (!done) proc_terminate(p);
    proc_release(p);
    KcValue res = kc_null();
    if (!done) sys_set_error("system.output: '%s' timed out after %g s", cmd, timeout);
    else if (p->code != 0) {
        size_t el = p->err.len; while (el && (p->err.p[el-1] == '\n' || p->err.p[el-1] == '\r')) el--;
        sys_set_error("system.output: '%s' exited with code %d%s%.*s", cmd, p->code, el ? ": " : "", (int)(el > 200 ? 200 : el), p->err.p ? p->err.p : "");
    } else {
        size_t l = p->out.len;
        while (l && (p->out.p[l-1] == '\n' || p->out.p[l-1] == '\r')) l--;
        char *o = kc_malloc(l + 1); if (l) memcpy(o, p->out.p, l);
        res = sys_bytes_str(o, l);
    }
    free_proc_mem(p);
    return res;
}

/* ------------------------------ spawn family ------------------------------ */

static KcValue pr_spawn(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    SYS_IMPURE(ip, pos, "system.spawn");
    const char *sig = "system.spawn(argv: array of string, capture: bool = false, cwd: string = null) -> process";
    sys_argc(n, 1, 3, sig, pos);
    char **argv = build_argv(a[0], pos, sig);
    bool capture = sys_opt_bool(a, n, 1, false, sig, pos);
    const char *cwd = sys_is_nullish(a, n, 2) ? NULL : sys_str(a, 2, sig, pos);
    Proc *p = launch(argv, NULL, capture, cwd, "system.spawn");
    free_argv(argv);
    return p ? make_handle(p) : kc_null();
}

static KcValue pr_wait(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    SYS_IMPURE(ip, pos, "system.wait");
    const char *sig = "system.wait(proc, timeout: float = null) -> int  (exit code; null if the timeout elapsed first)";
    sys_argc(n, 1, 2, sig, pos);
    Proc *p = get_proc(a[0], pos, "system.wait");
    double timeout = sys_opt_num(a, n, 1, -1.0, sig, pos);
    return proc_wait_done(p, timeout) ? kc_int(p->code) : kc_null();
}

static KcValue pr_poll(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    SYS_IMPURE(ip, pos, "system.poll");
    sys_argc(n, 1, 1, "system.poll(proc) -> int  (exit code, or null while still running)", pos);
    Proc *p = get_proc(a[0], pos, "system.poll");
    proc_step(p, 0);
    return p->done ? kc_int(p->code) : kc_null();
}

static KcValue pr_kill(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    SYS_IMPURE(ip, pos, "system.kill");
    const char *sig = "system.kill(proc, force: bool = false) -> bool  (SIGTERM, or SIGKILL with force; false if already finished)";
    sys_argc(n, 1, 2, sig, pos);
    Proc *p = get_proc(a[0], pos, "system.kill");
    bool force = sys_opt_bool(a, n, 1, false, sig, pos);
    proc_step(p, 0);
    if (p->done) return kc_bool(false);
    bool ok = proc_signal(p, force);
    if (!ok) sys_set_errno("system.kill");
    return kc_bool(ok);
}

static KcValue pr_proc_pid(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    sys_argc(n, 1, 1, "system.proc_pid(proc) -> int", pos);
    return kc_int(proc_pid_of(get_proc(a[0], pos, "system.proc_pid")));
}

static Proc *need_capture(KcValue *a, int n, int lo, int hi, const char *who, const char *sig, KcPos pos) {
    sys_argc(n, lo, hi, sig, pos);
    Proc *p = get_proc(a[0], pos, who);
    if (!p->capture) kc_error_at(pos, "%s: the process was spawned without capture=true", who);
    return p;
}

/* New stdout (or stderr) text produced since the last call; "" if none. Never blocks. */
static KcValue pr_proc_read(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    SYS_IMPURE(ip, pos, "system.proc_read");
    Proc *p = need_capture(a, n, 1, 1, "system.proc_read", "system.proc_read(proc) -> string  (new stdout since last call; never blocks)", pos);
    proc_step(p, 0);
    KcValue v = buf_slice(&p->out, p->out_read); p->out_read = p->out.len; return v;
}
static KcValue pr_proc_read_err(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    SYS_IMPURE(ip, pos, "system.proc_read_err");
    Proc *p = need_capture(a, n, 1, 1, "system.proc_read_err", "system.proc_read_err(proc) -> string  (new stderr since last call; never blocks)", pos);
    proc_step(p, 0);
    KcValue v = buf_slice(&p->err, p->err_read); p->err_read = p->err.len; return v;
}

static KcValue pr_proc_write(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    SYS_IMPURE(ip, pos, "system.proc_write");
    const char *sig = "system.proc_write(proc, data: string) -> bool";
    Proc *p = need_capture(a, n, 2, 2, "system.proc_write", sig, pos);
    sys_str(a, 1, sig, pos);
    if (p->done) { sys_set_error("system.proc_write: process already exited"); return kc_bool(false); }
    bool ok = proc_queue_input(p, a[1].as.s->data, (size_t)a[1].as.s->len);
    if (ok) proc_step(p, 0); /* push as much as the pipe will take right now */
    return kc_bool(ok);
}

static KcValue pr_proc_close_stdin(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    SYS_IMPURE(ip, pos, "system.proc_close_stdin");
    Proc *p = need_capture(a, n, 1, 1, "system.proc_close_stdin", "system.proc_close_stdin(proc)  (the child sees end-of-input once queued data is delivered)", pos);
    proc_close_in(p); proc_step(p, 0);
    return kc_null();
}

/* Releases the handle. A still-running process is killed (SIGKILL) and reaped so it cannot become a zombie. */
static KcValue pr_proc_free(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    SYS_IMPURE(ip, pos, "system.proc_free");
    sys_argc(n, 1, 1, "system.proc_free(proc)  (kills it first if it is still running)", pos);
    Proc *p = get_proc(a[0], pos, "system.proc_free");
    if (!p->done) { proc_signal(p, true); proc_wait_done(p, 2.0); }
    proc_release(p); free_proc_mem(p);
    a[0].as.handle->ptr = NULL;
    return kc_null();
}

const SysEntry sys_proc_entries[] = {
    {"run", pr_run}, {"run_argv", pr_run_argv}, {"output", pr_output},
    {"spawn", pr_spawn}, {"wait", pr_wait}, {"poll", pr_poll}, {"kill", pr_kill}, {"proc_pid", pr_proc_pid},
    {"proc_read", pr_proc_read}, {"proc_read_err", pr_proc_read_err},
    {"proc_write", pr_proc_write}, {"proc_close_stdin", pr_proc_close_stdin}, {"proc_free", pr_proc_free},
};
const int sys_proc_count = (int)(sizeof sys_proc_entries / sizeof sys_proc_entries[0]);
