#ifndef KC_JIT_H
#define KC_JIT_H
#include "value.h"

/* KingCobra's closure JIT (v0.3 scope, stated honestly):
 *
 * The JIT is an optimisation, never a requirement: kc_jit_available()
 * (below) probes the platform once and, if generated code can't be run
 * here (or the user passed --no-jit / KC_JIT=0), every closure simply
 * stays in the tree-walking interpreter.
 *
 * When a closure has been called KC_JIT_THRESHOLD times, the VM makes
 * ONE attempt to compile it straight to x86-64 machine code (see
 * compiler/src/jit.c, which comments every correctness-sensitive
 * decision at the point it matters — this header gives the boundary,
 * that file explains the "why"). The attempt succeeds for:
 *   - up to 4 int parameters, int locals (`let`/`var` with an
 *     initializer), and int arithmetic (+ - * unary-)
 *   - comparisons (< <= > >= == !=) and logical &&/||/! , matching the
 *     interpreter's short-circuit and truthiness-normalization
 *     semantics exactly (both sides of && /|| always end up a real
 *     0/1, never the raw operand)
 *   - if/else, while with break/continue, multiple statements, and
 *     compound assignment (+= -=)
 *   - self-recursive calls (a function calling itself by name) with
 *     the correct System V or Microsoft x64 calling convention for
 *     the host platform
 * provided the function's last top-level statement is a `return` (so
 * "falling off the end" with no explicit return, which would otherwise
 * leave an arbitrary value in rax, can't happen).
 *
 * Deliberately still NOT attempted, each for a specific reason:
 *   - floats: would need real per-variable type inference inside the
 *     JIT (int vs. double determines which registers/instructions to
 *     emit), not just "doesn't fit, bail" — a correctness-sensitive
 *     scope expansion big enough to warrant its own pass
 *   - calls to any function OTHER than the one being compiled: needs
 *     either compiling callees in dependency order or a trampoline/
 *     patch-later scheme; self-calls sidestep this because the
 *     function's own address is already known (it's allocated before
 *     compiling) — see compiler/src/jit.c
 *   - division/modulo: no div-by-zero check has been written for the
 *     generated code yet, so these simply aren't compiled rather than
 *     risk a native SIGFPE in place of KingCobra's own clean runtime
 *     error
 *   - deoptimization: nothing currently re-validates a specialization's
 *     assumptions after the fact, because nothing it currently compiles
 *     can have its assumptions invalidated post-compilation (no type
 *     feedback is used) — this becomes relevant once float/polymorphic
 *     support is added
 * A true general bytecode IR between the AST and native code (tracked
 * in docs/ROADMAP.md) would make all of the above far more tractable
 * at once; this pass deliberately extended the existing AST-direct
 * compiler instead; a larger architectural change, scoped separately.
 * Everything outside the compilable shape simply keeps running in the
 * tree-walking interpreter, correctly, just without the native
 * speedup — bailing out is always safe; guessing wrong is not.
 *
 * On success, kc_call_value() calls the machine code directly instead
 * of walking the AST — a real tier-up, not a simulated one.
 */
#define KC_JIT_THRESHOLD 50

/* Runtime capability check. The JIT is an optimisation, never a requirement:
 * this returns false when the host CPU has no native code generator, when the
 * OS will not give us (or will not execute) generated code pages, or when the
 * user turned it off (`--no-jit`, or KC_JIT=0 in the environment). In that
 * case every closure keeps running in the tree-walking interpreter with
 * identical results. The check runs once, lazily, and is cached.
 * kc_jit_status() returns "available" or a one-line human reason. */
bool kc_jit_available(void);
const char *kc_jit_status(void);
/* Force the JIT off (false) or back to "decide by probing" (true). Call
 * before running code; used by `--no-jit` and tests. */
void kc_jit_set_enabled(bool on);

/* Tries to compile once; sets cl->jit_code/jit_size on success.
 * Always sets cl->jit_tried = 1 so we never retry a function whose
 * shape doesn't fit. Safe to call on any closure. */
void kc_jit_try_compile(KcClosure *cl);

/* Calls a successfully-JIT'd closure with up to 4 int64 args. */
int64_t kc_jit_invoke(KcClosure *cl, int64_t *args, int nargs);

#endif
