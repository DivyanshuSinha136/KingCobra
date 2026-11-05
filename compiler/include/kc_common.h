/* KingCobra — common types, error handling, memory helpers
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com>
 * License: MIT
 */
#ifndef KC_COMMON_H
#define KC_COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <setjmp.h>

#define KC_VERSION "1.0.0"

/* ---- Allocation helpers (abort on OOM — a language impl must never
 * silently corrupt memory because malloc failed). ---- */
static inline void *kc_malloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) { fprintf(stderr, "kingcobra: out of memory (%zu bytes)\n", n); exit(70); }
    return p;
}
static inline void *kc_realloc(void *ptr, size_t n) {
    void *p = realloc(ptr, n ? n : 1);
    if (!p) { fprintf(stderr, "kingcobra: out of memory (realloc %zu bytes)\n", n); exit(70); }
    return p;
}
static inline char *kc_strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = kc_malloc(n);
    memcpy(p, s, n);
    return p;
}

/* ---- Source position, used for diagnostics ---- */
typedef struct {
    const char *file;
    int line;
    int col;
} KcPos;

/* ---- Fatal, positioned error. Exits with a compiler-style message. ---- */
extern int kc_color_mode;                         /* -1 auto, 0 never, 1 always (set by --color) */
bool kc_stderr_color(void);                       /* should diagnostics use ANSI colour? */
void kc_print_excerpt(KcPos pos);                 /* the offending source line + caret, as in an error report */
extern bool (*kc_source_line_hook)(const char *file, int line, char *buf, size_t cap);   /* lets the REPL serve "<repl:N>" lines */
void kc_error_at(KcPos pos, const char *fmt, ...);
void kc_warn_at(KcPos pos, const char *fmt, ...);

/* ---- Recoverable errors (used by the REPL) ----
 * Normally an error prints a diagnostic and exits. While a catcher is
 * active, kc_error_at / kc_fatal instead record the message and longjmp back
 * to it, so a long-lived session survives a bad input:
 *
 *     KcErrCatch c; c.quiet = false;
 *     if (KC_TRY(&c) == 0) { ...may error...; kc_try_end(&c); }
 *     else { kc_try_end(&c); ...c.msg / c.pos describe it... }
 *
 * quiet = true suppresses the stderr report (the caller prints c.msg).
 * Locals modified between KC_TRY and the error must be `volatile`. */
typedef struct KcErrCatch {
    jmp_buf jb;
    struct KcErrCatch *prev;
    bool quiet;
    int code;
    KcPos pos;
    char msg[600];
} KcErrCatch;
extern KcErrCatch *kc_err_top;
void kc_try_begin(KcErrCatch *c);
void kc_try_end(KcErrCatch *c);
#define KC_TRY(c) (kc_try_begin(c), setjmp((c)->jb))
/* Terminate the program with `code`, or unwind to the active catcher. */
void kc_fatal(int code) __attribute__((noreturn));
/* Native-code stack overflow (JIT self-recursion has no depth counter) becomes a catchable
 * "stack overflow" runtime error instead of a segfault. `stack_hi` is any address near the top of
 * the stack, `limit` the stack size in bytes. No-op where signals are unavailable. */
void kc_install_stack_overflow_handler(char *stack_hi, size_t limit);

/* Optional hook called before an error is reported or the process is torn down, so a module that
 * owns the terminal (std.ascii_screen: raw mode / alternate screen) can restore it first and the
 * diagnostic is visible. Must be idempotent and must not itself raise errors. */
extern void (*kc_error_hook)(void);

/* float -> int64 with ONE defined result on every CPU. C leaves out-of-range conversions undefined and the
 * hardware disagrees: x86-64 yields INT64_MIN, while ARM64 and RISC-V saturate and map NaN to 0. A language whose
 * `int(1e30)` printed different numbers per CPU would not be portable, so KingCobra pins the saturating rule
 * (the same as Rust's `as`): truncate toward zero, clamp to the int64 range, NaN -> 0. */
#include <stdint.h>
static inline int64_t kc_f2i(double d) {
    if (d != d) return 0;
    if (d >= 9223372036854775808.0) return INT64_MAX;
    if (d <= -9223372036854775808.0) return INT64_MIN;
    return (int64_t)d;
}

#endif
