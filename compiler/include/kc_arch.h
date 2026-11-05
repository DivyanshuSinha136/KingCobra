/* KingCobra — CPU architecture detection, in ONE place.
 *
 * Every "which CPU is this?" decision in the tree goes through the macros
 * below instead of testing __x86_64__ / __aarch64__ / ... ad hoc, so there
 * is exactly one table to keep correct when an architecture is added.
 *
 * Support levels (stated plainly; see docs/PLATFORMS.md):
 *
 *   x86_64            Full: interpreter + closure JIT + every std.rhex instruction.
 *   aarch64 (ARM64)   Interpreter. JIT off. std.rhex runs raw aarch64 machine code.
 *   arm (ARMhf/ARMel) Interpreter (32-bit). JIT off. std.rhex runs raw ARM machine code.
 *   riscv64 / riscv32 Interpreter. JIT off. std.rhex runs raw RISC-V machine code.
 *   x86 (i386)        Interpreter (32-bit). JIT off.
 *   anything else     Interpreter only; std.rhex refuses to execute raw code
 *                     (no known instruction-cache protocol for it).
 *
 * The closure JIT emits x86-64 machine code only. On every other CPU the
 * program runs in the tree-walking interpreter, which is the reference
 * semantics — results are identical, only slower.
 *
 * Test hook: -DKC_ARCH_FORCE_GENERIC makes the build behave as an unknown
 * architecture even on x86-64 (no JIT, no x86 asm, no raw-code execution),
 * so the portable fallbacks can be exercised on any machine.
 *
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com>
 * License: MIT
 */
#ifndef KC_ARCH_H
#define KC_ARCH_H

#include <stdint.h>

#if defined(KC_ARCH_FORCE_GENERIC)
#  define KC_ARCH_OTHER 1
#  define KC_ARCH_NAME "generic"
#elif defined(__x86_64__) || defined(_M_X64)
#  define KC_ARCH_X86_64 1
#  define KC_ARCH_NAME "x86_64"
#elif defined(__i386__) || defined(_M_IX86)
#  define KC_ARCH_X86 1
#  define KC_ARCH_NAME "x86"
#elif defined(__aarch64__) || defined(_M_ARM64)
#  define KC_ARCH_ARM64 1
#  define KC_ARCH_NAME "aarch64"
#elif defined(__arm__) || defined(_M_ARM)
#  define KC_ARCH_ARM32 1
#  if defined(__ARM_PCS_VFP)
#    define KC_ARCH_ARM_HARDFLOAT 1
#    define KC_ARCH_NAME "armhf"
#  else
#    define KC_ARCH_NAME "arm"
#  endif
#elif defined(__riscv) && defined(__riscv_xlen) && __riscv_xlen == 64
#  define KC_ARCH_RISCV64 1
#  define KC_ARCH_NAME "riscv64"
#elif defined(__riscv) && defined(__riscv_xlen) && __riscv_xlen == 32
#  define KC_ARCH_RISCV32 1
#  define KC_ARCH_NAME "riscv32"
#else
#  define KC_ARCH_OTHER 1
#  define KC_ARCH_NAME "unknown"
#endif

/* Pointer / machine-word width. 32-bit hosts (armhf, riscv32, x86) are
 * ILP32: `long`, `size_t` and pointers are 32 bits, `int64_t` is not a
 * machine word. FFI argument passing depends on this (see ffi.c). */
#if UINTPTR_MAX > 0xFFFFFFFFu
#  define KC_ARCH_WORD_BITS 64
#else
#  define KC_ARCH_WORD_BITS 32
#endif

/* x86-only instructions (cpuid, rdtsc, rdrand, clflush, port I/O, MSRs). */
#if defined(KC_ARCH_X86_64)
#  define KC_ARCH_HAS_X86_INSNS 1
#endif

/* Architectures where std.rhex may run raw machine code: we know how to
 * make freshly written bytes visible to the instruction stream (a no-op on
 * x86, an explicit flush on ARM / RISC-V — see kc_exec_seal). */
#if defined(KC_ARCH_X86_64) || defined(KC_ARCH_X86) || defined(KC_ARCH_ARM64) || \
    defined(KC_ARCH_ARM32) || defined(KC_ARCH_RISCV64) || defined(KC_ARCH_RISCV32)
#  define KC_ARCH_CAN_EXEC_RAW 1
#endif

/* The closure JIT generates x86-64 only. Everything else interprets. */
#if defined(KC_ARCH_X86_64)
#  define KC_ARCH_HAS_JIT 1
#endif

#endif /* KC_ARCH_H */
