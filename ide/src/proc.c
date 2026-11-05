#ifdef _WIN32
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#include "proc.h"
#include <windows.h>
#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE 0x00020016
#endif
typedef HRESULT(WINAPI *CreatePCFn)(COORD, HANDLE, HANDLE, DWORD, void **);
typedef HRESULT(WINAPI *ResizePCFn)(void *, COORD);
typedef void(WINAPI *ClosePCFn)(void *);
static CreatePCFn pCreatePC; static ResizePCFn pResizePC; static ClosePCFn pClosePC;
static bool conpty_ok(void) {
    static int st = -1;
    if (st < 0) { HMODULE k = GetModuleHandleA("kernel32.dll"); pCreatePC = (CreatePCFn)(void *)GetProcAddress(k, "CreatePseudoConsole"); pResizePC = (ResizePCFn)(void *)GetProcAddress(k, "ResizePseudoConsole"); pClosePC = (ClosePCFn)(void *)GetProcAddress(k, "ClosePseudoConsole"); st = pCreatePC && pResizePC && pClosePC; }
    return st == 1;
}
static void build_cmdline(Str *cl, char *const argv[]) { str_init(cl); for (int i = 0; argv[i]; i++) { if (i) str_addc(cl, ' '); str_addc(cl, '"'); str_adds(cl, argv[i]); str_addc(cl, '"'); } }
bool proc_start_ex(Proc *p, char *const argv[], const char *cwd, int cols, int rows, const char *const *envkv, bool use_pty) {
    memset(p, 0, sizeof *p); p->fd = p->wfd = -1;
    if (envkv) for (int i = 0; envkv[i]; i++) { char *k = xstrdup(envkv[i]); char *eq = strchr(k, '='); if (eq) { *eq = 0; SetEnvironmentVariableA(k, eq + 1); } free(k); }
    Str cl; build_cmdline(&cl, argv);
    if (use_pty && conpty_ok()) {
        HANDLE inr, inw, outr, outw; SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, FALSE };
        if (!CreatePipe(&inr, &inw, &sa, 0) || !CreatePipe(&outr, &outw, &sa, 0)) { str_free(&cl); return false; }
        COORD sz = { (SHORT)cols, (SHORT)rows }; void *hpc = NULL;
        if (FAILED(pCreatePC(sz, inr, outw, 0, &hpc))) { str_free(&cl); return false; }
        CloseHandle(inr); CloseHandle(outw);
        STARTUPINFOEXA si; memset(&si, 0, sizeof si); si.StartupInfo.cb = sizeof si;
        SIZE_T asz = 0; InitializeProcThreadAttributeList(NULL, 1, 0, &asz); si.lpAttributeList = HeapAlloc(GetProcessHeap(), 0, asz);
        InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &asz);
        UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, hpc, sizeof hpc, NULL, NULL);
        PROCESS_INFORMATION pi; BOOL ok = CreateProcessA(NULL, cl.p, NULL, NULL, FALSE, EXTENDED_STARTUPINFO_PRESENT, NULL, cwd, &si.StartupInfo, &pi);
        DeleteProcThreadAttributeList(si.lpAttributeList); HeapFree(GetProcessHeap(), 0, si.lpAttributeList); str_free(&cl);
        if (!ok) { pClosePC(hpc); CloseHandle(inw); CloseHandle(outr); return false; }
        CloseHandle(pi.hThread); p->hproc = pi.hProcess; p->hin = inw; p->hout = outr; p->hpc = hpc; p->running = true; return true;
    }
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE }; HANDLE inr, inw, outr, outw;
    if (!CreatePipe(&outr, &outw, &sa, 0) || !CreatePipe(&inr, &inw, &sa, 0)) { str_free(&cl); return false; }
    SetHandleInformation(outr, HANDLE_FLAG_INHERIT, 0); SetHandleInformation(inw, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si; memset(&si, 0, sizeof si); si.cb = sizeof si; si.dwFlags = STARTF_USESTDHANDLES; si.hStdInput = inr; si.hStdOutput = outw; si.hStdError = outw;
    PROCESS_INFORMATION pi; BOOL ok = CreateProcessA(NULL, cl.p, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, cwd, &si, &pi);
    str_free(&cl); CloseHandle(inr); CloseHandle(outw);
    if (!ok) { CloseHandle(outr); CloseHandle(inw); return false; }
    CloseHandle(pi.hThread); p->hproc = pi.hProcess; p->hin = inw; p->hout = outr; p->running = true; return true;
}
int proc_read(Proc *p, char *buf, int cap) {
    DWORD avail = 0; if (!p->hout) return -1;
    if (!PeekNamedPipe(p->hout, NULL, 0, NULL, &avail, NULL)) return -1;
    if (!avail) return 0;
    DWORD got; if (!ReadFile(p->hout, buf, avail < (DWORD)cap ? avail : (DWORD)cap, &got, NULL)) return -1;
    return (int)got;
}
void proc_write(Proc *p, const char *d, int n) { DWORD w; if (p->hin) WriteFile(p->hin, d, (DWORD)n, &w, NULL); }
bool proc_poll(Proc *p) {
    if (!p->running) return false;
    DWORD c; if (WaitForSingleObject(p->hproc, 0) == WAIT_OBJECT_0) { GetExitCodeProcess(p->hproc, &c); p->exit_code = (int)c; p->running = false; }
    return p->running;
}
void proc_stop(Proc *p) { if (p->running) { if (p->hpc) { char c = 3; proc_write(p, &c, 1); Sleep(50); } if (proc_poll(p)) TerminateProcess(p->hproc, 1); } }
void proc_resize(Proc *p, int c, int r) { if (p->hpc && pResizePC) { COORD sz = { (SHORT)c, (SHORT)r }; pResizePC(p->hpc, sz); } }
void proc_close(Proc *p) {
    if (p->hpc && pClosePC) pClosePC(p->hpc);
    if (p->hproc) CloseHandle(p->hproc); if (p->hin) CloseHandle(p->hin); if (p->hout) CloseHandle(p->hout);
    p->hproc = p->hin = p->hout = p->hpc = NULL; p->running = false;
}
int proc_run_inherit(char *const argv[], const char *cwd) {
    Str cl; build_cmdline(&cl, argv);
    STARTUPINFOA si; memset(&si, 0, sizeof si); si.cb = sizeof si; PROCESS_INFORMATION pi;
    BOOL ok = CreateProcessA(NULL, cl.p, NULL, NULL, TRUE, 0, NULL, cwd, &si, &pi); str_free(&cl);
    if (!ok) return -1;
    WaitForSingleObject(pi.hProcess, INFINITE); DWORD c = 0; GetExitCodeProcess(pi.hProcess, &c);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread); return (int)c;
}
#else
#include "proc.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

bool proc_start_ex(Proc *p, char *const argv[], const char *cwd, int cols, int rows, const char *const *envkv, bool use_pty) {
    memset(p, 0, sizeof *p); p->fd = p->wfd = -1;
    int m = -1, rd = -1, wr = -1, cin = -1, cout = -1; char slave[128] = "";
    if (use_pty) {
        m = posix_openpt(O_RDWR | O_NOCTTY);
        if (m < 0 || grantpt(m) < 0 || unlockpt(m) < 0) { if (m >= 0) close(m); return false; }
        char *sn = ptsname(m); if (!sn) { close(m); return false; }
        snprintf(slave, sizeof slave, "%s", sn);
        struct winsize ws = { (unsigned short)rows, (unsigned short)cols, 0, 0 }; ioctl(m, TIOCSWINSZ, &ws);
    } else {
        int a[2], b[2]; if (pipe(a) < 0) return false; if (pipe(b) < 0) { close(a[0]); close(a[1]); return false; }
        cin = a[0]; wr = a[1]; rd = b[0]; cout = b[1];
    }
    pid_t pid = fork();
    if (pid < 0) { if (m >= 0) close(m); if (rd >= 0) { close(rd); close(wr); close(cin); close(cout); } return false; }
    if (pid == 0) {
        setsid();
        if (use_pty) {
            int s = open(slave, O_RDWR); if (s < 0) _exit(127);
            ioctl(s, TIOCSCTTY, 0); dup2(s, 0); dup2(s, 1); dup2(s, 2); if (s > 2) close(s); close(m);
            setenv("TERM", "xterm-256color", 1); setenv("COLORTERM", "truecolor", 1);
        } else { dup2(cin, 0); dup2(cout, 1); dup2(cout, 2); close(cin); close(cout); close(rd); close(wr); }
        if (cwd && chdir(cwd) != 0) {}
        if (envkv) for (int i = 0; envkv[i]; i++) { char *k = strdup(envkv[i]); char *eq = strchr(k, '='); if (eq) { *eq = 0; setenv(k, eq + 1, 1); } free(k); }
        signal(SIGPIPE, SIG_DFL); signal(SIGINT, SIG_DFL); signal(SIGHUP, SIG_DFL); signal(SIGTERM, SIG_DFL);
        execvp(argv[0], argv);
        dprintf(2, "kcide: cannot execute %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    if (use_pty) { fcntl(m, F_SETFL, fcntl(m, F_GETFL) | O_NONBLOCK); p->fd = p->wfd = m; }
    else { close(cin); close(cout); fcntl(rd, F_SETFL, fcntl(rd, F_GETFL) | O_NONBLOCK); p->fd = rd; p->wfd = wr; }
    p->pid = pid; p->running = true; p->exit_code = 0; p->signaled = false;
    return true;
}
bool proc_start(Proc *p, char *const argv[], const char *cwd, int cols, int rows) { return proc_start_ex(p, argv, cwd, cols, rows, NULL, true); }
int proc_read(Proc *p, char *buf, int cap) {
    if (p->fd < 0) return -1;
    ssize_t n = read(p->fd, buf, (size_t)cap);
    if (n > 0) return (int)n;
    if (n < 0 && (errno == EAGAIN || errno == EINTR)) return 0;
    return -1;                                  /* EIO once the slave side is closed / EOF on a pipe */
}
void proc_write(Proc *p, const char *d, int n) { if (p->wfd >= 0) { ssize_t w = write(p->wfd, d, (size_t)n); (void)w; } }
bool proc_poll(Proc *p) {
    if (!p->running) return false;
    int st; pid_t r = waitpid(p->pid, &st, WNOHANG);
    if (r == p->pid) {
        p->running = false;
        if (WIFEXITED(st)) p->exit_code = WEXITSTATUS(st); else if (WIFSIGNALED(st)) { p->signaled = true; p->exit_code = 128 + WTERMSIG(st); }
    }
    return p->running;
}
void proc_stop(Proc *p) {
    if (!p->running) return;
    kill(-p->pid, SIGINT); kill(p->pid, SIGINT);
    for (int i = 0; i < 20; i++) { usleep(10000); if (!proc_poll(p)) return; }
    kill(-p->pid, SIGKILL); kill(p->pid, SIGKILL); waitpid(p->pid, NULL, 0); p->running = false; p->signaled = true; p->exit_code = 137;
}
void proc_resize(Proc *p, int cols, int rows) {
    if (p->fd < 0 || p->wfd != p->fd) return;
    struct winsize ws = { (unsigned short)rows, (unsigned short)cols, 0, 0 }; ioctl(p->fd, TIOCSWINSZ, &ws);
    if (p->running) kill(p->pid, SIGWINCH);
}
void proc_close(Proc *p) { if (p->wfd > 2 && p->wfd != p->fd) close(p->wfd); if (p->fd > 2) close(p->fd); p->fd = p->wfd = -1; }   /* never touch stdio descriptors */
int proc_run_inherit(char *const argv[], const char *cwd) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) { if (cwd && chdir(cwd) != 0) {} execvp(argv[0], argv); _exit(127); }
    int st = 0; while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128;
}
#endif
