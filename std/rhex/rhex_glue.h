/* KingCobra std.rhex — shared glue for the rhex_*.c files.
 *
 * std.rhex is split the same way std.nn is: one file per concern,
 * each exposing a small `const NnEntry`-shaped table that rhex.c
 * (the module's "nn.c equivalent") concatenates into the single
 * KcModule the interpreter binds `import std.rhex;` to.
 *
 *   rhex_buf.c   executable-buffer lifecycle: alloc/write/read/protect/free
 *   rhex_call.c  invoking a buffer as code, with or without typed arguments
 *   rhex_cpu.c   CPU identification/timing/entropy instructions (safe, no `unsafe`)
 *   rhex_hw.c    MSRs, port I/O, cache control (real hardware — `unsafe`)
 *   rhex.c       module table + the require_unsafe/expect_jitbuf helpers used everywhere
 *
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com>
 * License: MIT
 */
#ifndef KC_RHEX_GLUE_H
#define KC_RHEX_GLUE_H
#include "interp.h"
#include "kc_platform.h"
#include "kc_hw.h"
#include "ffi.h"

/* std.rhex runs raw machine code of the HOST architecture (x86-64, ARM64,
 * ARM32, RISC-V, x86): the bytes you give it must be valid for the CPU the
 * program is running on — `rhex.arch()` tells you which that is. On an
 * architecture we have no instruction-cache protocol for, execution is
 * refused. The x86-only instruction wrappers (cpuid, rdtsc, rdrand, clflush,
 * MSRs, port I/O) exist only where KC_ARCH_HAS_X86_INSNS is set and report
 * a clear "x86-64 only" error elsewhere. */
#if !defined(KC_ARCH_CAN_EXEC_RAW)
#define KC_RHEX_UNSUPPORTED 1
#endif
#define KC_RHEX_ARCH_MSG "this architecture (" KC_ARCH_NAME ") has no raw-code execution support"
#define KC_RHEX_X86_MSG  "is an x86-64 instruction and is not available on " KC_ARCH_NAME

/* Every rhex entry that can observably affect the outside world (raw
 * code execution, MSRs, ports, cache control) must run inside an
 * `unsafe { }` block — the language-level admission that what
 * follows is not memory-safe and is the caller's responsibility,
 * mirroring `unsafe` in Rust. CPU-identification/timing/entropy reads
 * (rhex_cpu.c) are pure and bounded, so they deliberately do NOT
 * require it, the same way std.mmio's virtual regions don't. */
void rhex_require_unsafe(KcInterp *ip, KcPos pos, const char *who);

/* A live rhex.alloc() buffer. h->extra: 0 = writable (not executable),
 * 1 = executable (not writable) — W^X is enforced by construction,
 * never both at once. h->ptr is NULL once freed. */
KcHandle *rhex_expect_buf(KcValue v, KcPos pos, const char *who);
KcHandle *rhex_live_buf(KcValue v, KcPos pos, const char *who); /* also rejects a freed buffer */

/* Shared "environmental failure" error string for rhex_hw.c, mirroring
 * mmio.last_error(): programming errors are fatal everywhere in rhex,
 * but "no permission" / "no such device" / "unsupported on this OS"
 * for real-hardware entries return null and set this instead. */
void        rhex_set_error(const char *fmt, ...);
const char *rhex_get_error(void);

typedef struct { const char *name; KcNativeFn fn; } RhexEntry;
extern const RhexEntry rhex_buf_entries[];  extern const int rhex_buf_count;
extern const RhexEntry rhex_call_entries[]; extern const int rhex_call_count;
extern const RhexEntry rhex_cpu_entries[];  extern const int rhex_cpu_count;
extern const RhexEntry rhex_hw_entries[];   extern const int rhex_hw_count;

#endif
