#include "kc_common.h"
#include "kc_platform.h"
#include <stdarg.h>
#ifndef _WIN32
#include <signal.h>
#include <unistd.h>
#endif

KcErrCatch *kc_err_top = NULL;

/* Diagnostics colour: --color=always|never, else NO_COLOR / FORCE_COLOR, else "is stderr a terminal?". */
int kc_color_mode = -1;                                  /* -1 auto, 0 never, 1 always */
bool kc_stderr_color(void) {
    if (kc_color_mode >= 0) return kc_color_mode == 1;
    static int cached = -1;
    if (cached < 0) {
        const char *nc = getenv("NO_COLOR"), *fc = getenv("FORCE_COLOR");
        if (nc && *nc) cached = 0;
        else if (fc && *fc && strcmp(fc, "0") != 0) cached = 1;
        else cached = KC_ISATTY(KC_FILENO(stderr)) ? 1 : 0;
    }
    return cached == 1;
}
#define CC(code) (kc_stderr_color() ? (code) : "")
void (*kc_error_hook)(void) = NULL;
void (*kc_error_debug_hook)(KcPos pos, const char *msg) = NULL;   /* debugger: stop on an uncaught runtime error */

void kc_try_begin(KcErrCatch *c) {
    c->prev = kc_err_top; c->code = 0; c->msg[0] = 0; c->pos = (KcPos){ NULL, 0, 0 };
    kc_err_top = c;
}
void kc_try_end(KcErrCatch *c) { if (kc_err_top == c) kc_err_top = c->prev; }

void kc_fatal(int code) {
    if (kc_error_hook) kc_error_hook();
    if (kc_err_top) { kc_err_top->code = code; longjmp(kc_err_top->jb, 1); }
    exit(code);
}

/* Prints the offending source line with a caret under the column, e.g.
 *     3 |     print(cnt);
 *       |           ^
 * The file is read only now, on the error path, so correct programs never pay for this. Skipped
 * for pseudo-files (<repl>, <input>) and when the file can't be read. */
bool (*kc_source_line_hook)(const char *file, int line, char *buf, size_t cap) = NULL;   /* REPL: text of a "<repl:N>" input */

void kc_print_excerpt(KcPos pos) {
    if (!pos.file || pos.line <= 0 || pos.col <= 0) return;
    char buf[512];
    if (pos.file[0] == '<') {
        if (!kc_source_line_hook || !kc_source_line_hook(pos.file, pos.line, buf, sizeof buf)) return;
    } else {
        FILE *f = fopen(pos.file, "rb");
        if (!f) return;
        int ln = 0;
        bool found = false;
        while (fgets(buf, sizeof buf, f)) {
            size_t n = strlen(buf);
            bool complete = n > 0 && buf[n - 1] == '\n';
            if (++ln == pos.line) { found = true; break; }
            while (!complete && fgets(buf, sizeof buf, f)) { n = strlen(buf); complete = n > 0 && buf[n - 1] == '\n'; }   /* skip the rest of a long line */
        }
        fclose(f);
        if (!found) return;
    }
    size_t n = strlen(buf);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
    if (pos.col > (int)n + 1 || n == 0) return;                      /* column is not on this line: don't guess */
    fprintf(stderr, "%s%5d |%s %s\n%s      |%s ", CC("\x1b[2m"), pos.line, CC("\x1b[0m"), buf, CC("\x1b[2m"), CC("\x1b[0m"));
    for (int i = 0; i < pos.col - 1; i++) fputc(buf[i] == '\t' ? '\t' : ' ', stderr);
    fprintf(stderr, "%s^%s\n", CC("\x1b[31m"), CC("\x1b[0m"));
}
static void print_excerpt(KcPos pos) { kc_print_excerpt(pos); }

void kc_error_at(KcPos pos, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char msg[600];
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    KcErrCatch *c = kc_err_top;
    if (c) { snprintf(c->msg, sizeof c->msg, "%s", msg); c->pos = pos; }
    if (kc_error_hook) kc_error_hook();       /* a module owning the terminal restores it first, so the diagnostic is visible */
    if (!c || !c->quiet) {
        fflush(stdout);
        fprintf(stderr, "%s%s:%d:%d: %serror:%s %s\n", CC("\x1b[1m"), pos.file ? pos.file : "<input>", pos.line, pos.col, CC("\x1b[31m"), CC("\x1b[0m"), msg);
        print_excerpt(pos);
    }
    if (!c && kc_error_debug_hook) kc_error_debug_hook(pos, msg);
    kc_fatal(65);
}

void kc_warn_at(KcPos pos, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%s%s:%d:%d: %swarning:%s ", CC("\x1b[1m"), pos.file ? pos.file : "<input>", pos.line, pos.col, CC("\x1b[33m"), CC("\x1b[0m"));
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

#ifndef _WIN32
static char *g_so_hi = NULL;
static size_t g_so_lim = 0;
static char g_so_altstack[64 * 1024];

/* Runs on the alternate stack. A fault within a megabyte of the stack's edge is a stack overflow:
 * unwind into the innermost `try` as an ordinary runtime error, or report and exit. Any other
 * fault is a genuine crash and keeps the default action. */
static void on_stack_overflow(int sig, siginfo_t *si, void *uc) {
    (void)uc;
    char *a = (char *)si->si_addr, *lo = g_so_hi - g_so_lim;
    if (g_so_hi && a < lo + (1 << 20) && a > lo - (1 << 20)) {
        static const char msg[] = "stack overflow (recursion too deep)";
        KcErrCatch *c = kc_err_top;
        if (c) {
            memcpy(c->msg, msg, sizeof msg);
            c->pos = (KcPos){ NULL, 0, 0 };
            c->code = 65;
            longjmp(c->jb, 1);          /* SA_NODEFER: SIGSEGV is not left blocked, so the next overflow is handled too */
        }
        static const char out[] = "error: stack overflow (recursion too deep)\n";
        ssize_t w = write(2, out, sizeof out - 1); (void)w;
        _exit(65);
    }
    signal(sig, SIG_DFL);               /* unrelated fault: re-execute it with the default action */
}

void kc_install_stack_overflow_handler(char *stack_hi, size_t limit) {
    g_so_hi = stack_hi; g_so_lim = limit;
    stack_t ss; ss.ss_sp = g_so_altstack; ss.ss_size = sizeof g_so_altstack; ss.ss_flags = 0;
    if (sigaltstack(&ss, NULL) != 0) return;
    struct sigaction sv;
    memset(&sv, 0, sizeof sv);
    sv.sa_sigaction = on_stack_overflow;
    sv.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigemptyset(&sv.sa_mask);
    sigaction(SIGSEGV, &sv, NULL);
}
#else
void kc_install_stack_overflow_handler(char *stack_hi, size_t limit) { (void)stack_hi; (void)limit; }
#endif
