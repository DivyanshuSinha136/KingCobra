/* KingCobra std.rhex — CPU identification, timing and entropy
 * instructions. The x86 instruction wrappers (cpuid, rdtsc, rdrand, ...)
 * are x86-64 only and report a clear error elsewhere; arch(), pause() and
 * fence() work on every architecture.
 *
 * These entries deliberately do NOT require `unsafe { }`: each one is
 * a single, bounded, side-effect-free instruction (read CPU state,
 * read a counter, read a hardware RNG, or a documented ordering/
 * scheduling hint) — the same reasoning std.mmio uses to let its
 * `virtual` regions skip `unsafe` while real-hardware regions require
 * it. Writing to arbitrary MSRs or ports is a different kind of
 * operation entirely and stays gated in rhex_hw.c.
 *
 * CPUID goes through <cpuid.h> (__cpuid_count), which preserves %ebx/%rbx correctly in every build mode, PIC included.
 *
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com>
 * License: MIT
 */
#include "rhex_glue.h"
#include <string.h>
#if defined(KC_ARCH_HAS_X86_INSNS)
#include <cpuid.h>
#endif

#if defined(KC_ARCH_HAS_X86_INSNS)
#define RHEX_CPU_OK 1
#endif

static void need_cpu(KcPos pos, const char *who) {
#ifndef RHEX_CPU_OK
    kc_error_at(pos, "%s " KC_RHEX_X86_MSG, who);
#else
    (void)pos; (void)who;
#endif
}

#ifdef RHEX_CPU_OK
static void do_cpuid(uint32_t leaf, uint32_t subleaf, uint32_t out[4]) {
    /* <cpuid.h> saves and restores %rbx/%ebx correctly (it is callee-saved on x86-64 and the PIC register on i386).
     * A hand-written xchg with an output-only operand clobbered it with an uninitialised value. */
    unsigned int a = 0, b = 0, c = 0, d = 0;
    __cpuid_count(leaf, subleaf, a, b, c, d);
    out[0] = a; out[1] = b; out[2] = c; out[3] = d;
}
static uint32_t max_std_leaf(void)    { uint32_t r[4]; do_cpuid(0, 0, r); return r[0]; }
static uint32_t max_ext_leaf(void)    { uint32_t r[4]; do_cpuid(0x80000000u, 0, r); return r[0]; }
#endif

static KcValue cpu_cpuid(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; need_cpu(pos, "rhex.cpuid");
    if (n < 1 || n > 2 || a[0].type != VAL_INT) kc_error_at(pos, "rhex.cpuid(leaf: int [, subleaf: int]) -> [eax, ebx, ecx, edx]");
    int64_t subleaf = n == 2 ? (n >= 2 && a[1].type == VAL_INT ? a[1].as.i : -1) : 0;
    if (n == 2 && a[1].type != VAL_INT) kc_error_at(pos, "rhex.cpuid: subleaf must be an int");
    (void)subleaf;
#ifdef RHEX_CPU_OK
    uint32_t r[4]; do_cpuid((uint32_t)a[0].as.i, (uint32_t)subleaf, r);
    KcValue out = kc_array_new();
    for (int i = 0; i < 4; i++) kc_array_push(out, kc_int((int64_t)(uint32_t)r[i]));
    return out;
#else
    return kc_null();
#endif
}

static KcValue cpu_vendor(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; (void)a; need_cpu(pos, "rhex.cpu_vendor");
    if (n != 0) kc_error_at(pos, "rhex.cpu_vendor() -> string");
#ifdef RHEX_CPU_OK
    uint32_t r[4]; do_cpuid(0, 0, r);
    char buf[13]; memcpy(buf, &r[1], 4); memcpy(buf + 4, &r[3], 4); memcpy(buf + 8, &r[2], 4); buf[12] = 0;
    return kc_string(buf);
#else
    return kc_string("");
#endif
}

static KcValue cpu_brand(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; (void)a; need_cpu(pos, "rhex.cpu_brand");
    if (n != 0) kc_error_at(pos, "rhex.cpu_brand() -> string");
#ifdef RHEX_CPU_OK
    if (max_ext_leaf() < 0x80000004u) return kc_string("");
    char buf[49]; uint32_t r[4];
    do_cpuid(0x80000002u, 0, r); memcpy(buf,      r, 16);
    do_cpuid(0x80000003u, 0, r); memcpy(buf + 16, r, 16);
    do_cpuid(0x80000004u, 0, r); memcpy(buf + 32, r, 16);
    buf[48] = 0;
    return kc_string(buf);
#else
    return kc_string("");
#endif
}

static KcValue cpu_has_feature(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; need_cpu(pos, "rhex.has_feature");
    if (n != 1 || a[0].type != VAL_STRING) kc_error_at(pos, "rhex.has_feature(name: string) -> bool");
#ifdef RHEX_CPU_OK
    const char *name = a[0].as.s->data;
    uint32_t r1[4] = {0}, r7[4] = {0};
    uint32_t max1 = max_std_leaf();
    if (max1 >= 1) do_cpuid(1, 0, r1);
    if (max1 >= 7) do_cpuid(7, 0, r7);
    /* leaf, word index (0=eax..3=edx), bit */
    struct { const char *name; int leaf_r; int bit; } F[] = {
        {"sse2",    3, 26}, {"sse3",    2, 0},  {"ssse3",   2, 9},  {"sse4_1",  2, 19},
        {"sse4_2",  2, 20}, {"popcnt",  2, 23}, {"aes",     2, 25}, {"avx",     2, 28},
        {"fma",     2, 12}, {"f16c",    2, 29}, {"rdrand",  2, 30},
        {"bmi1", 10 + 1, 3}, {"avx2", 10 + 1, 5}, {"bmi2", 10 + 1, 8}, {"rdseed", 10 + 1, 18},
        {"avx512f", 10 + 1, 16},
    };
    for (size_t i = 0; i < sizeof(F)/sizeof(F[0]); i++) {
        if (strcmp(F[i].name, name) != 0) continue;
        uint32_t word = F[i].leaf_r >= 10 ? r7[F[i].leaf_r - 10] : r1[F[i].leaf_r];
        return kc_bool((word >> F[i].bit) & 1u);
    }
    kc_error_at(pos, "rhex.has_feature: unknown feature name '%s' (known: sse2, sse3, ssse3, sse4_1, sse4_2, popcnt, aes, avx, fma, f16c, rdrand, bmi1, avx2, bmi2, rdseed, avx512f)", name);
    return kc_bool(false);
#else
    return kc_bool(false);
#endif
}

static KcValue cpu_rdtsc(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)a; need_cpu(pos, "rhex.rdtsc");
    kc_forbid_in_comptime(ip, pos, "rhex.rdtsc"); /* a cycle count is never a compile-time constant */
    if (n != 0) kc_error_at(pos, "rhex.rdtsc() -> int");
#ifdef RHEX_CPU_OK
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return kc_int((int64_t)(((uint64_t)hi << 32) | lo));
#else
    return kc_int(0);
#endif
}

static KcValue cpu_rdtscp(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)a; need_cpu(pos, "rhex.rdtscp");
    kc_forbid_in_comptime(ip, pos, "rhex.rdtscp");
    if (n != 0) kc_error_at(pos, "rhex.rdtscp() -> [tsc, cpu_id]");
#ifdef RHEX_CPU_OK
    uint32_t lo, hi, aux;
    __asm__ volatile("rdtscp" : "=a"(lo), "=d"(hi), "=c"(aux));
    KcValue out = kc_array_new();
    kc_array_push(out, kc_int((int64_t)(((uint64_t)hi << 32) | lo)));
    kc_array_push(out, kc_int((int64_t)aux));
    return out;
#else
    return kc_null();
#endif
}

static KcValue cpu_rdrand(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)a; need_cpu(pos, "rhex.rdrand");
    kc_forbid_in_comptime(ip, pos, "rhex.rdrand"); /* a hardware RNG draw is never a compile-time constant */
    if (n != 0) kc_error_at(pos, "rhex.rdrand() -> int | null");
#ifdef RHEX_CPU_OK
    uint64_t val; uint8_t ok;
    __asm__ volatile("rdrand %0\n\t setb %1" : "=r"(val), "=qm"(ok));
    if (!ok) return kc_null(); /* hardware RNG transiently out of entropy, or unsupported */
    return kc_int((int64_t)val);
#else
    return kc_null();
#endif
}

static KcValue cpu_rdseed(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)a; need_cpu(pos, "rhex.rdseed");
    kc_forbid_in_comptime(ip, pos, "rhex.rdseed");
    if (n != 0) kc_error_at(pos, "rhex.rdseed() -> int | null");
#ifdef RHEX_CPU_OK
    uint64_t val; uint8_t ok;
    __asm__ volatile("rdseed %0\n\t setb %1" : "=r"(val), "=qm"(ok));
    if (!ok) return kc_null();
    return kc_int((int64_t)val);
#else
    return kc_null();
#endif
}

static KcValue cpu_pause(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; (void)a;
    if (n != 0) kc_error_at(pos, "rhex.pause()");
    kc_hw_cpu_relax();
    return kc_null();
}

static KcValue cpu_fence(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    if (n > 1 || (n == 1 && a[0].type != VAL_STRING)) kc_error_at(pos, "rhex.fence([\"full\"|\"read\"|\"write\"|\"compiler\"])");
    const char *kind = n == 1 ? a[0].as.s->data : "full";
    int k = !strcmp(kind, "read") ? KC_HW_BAR_READ : !strcmp(kind, "write") ? KC_HW_BAR_WRITE :
            !strcmp(kind, "compiler") ? KC_HW_BAR_COMPILER : !strcmp(kind, "full") ? KC_HW_BAR_FULL : -1;
    if (k < 0) kc_error_at(pos, "rhex.fence: unknown kind '%s' (valid: full, read, write, compiler)", kind);
    kc_hw_barrier(k);
    return kc_null();
}

/* rhex.arch() -> "x86_64" | "aarch64" | "armhf" | "arm" | "riscv64" | "riscv32" | "x86" | "unknown"
 * Which machine code rhex.exec()/call() will actually run. */
static KcValue cpu_arch(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; (void)a;
    if (n != 0) kc_error_at(pos, "rhex.arch() -> string");
    return kc_string(KC_ARCH_NAME);
}

const RhexEntry rhex_cpu_entries[] = {
    {"arch", cpu_arch},
    {"cpuid", cpu_cpuid}, {"cpu_vendor", cpu_vendor}, {"cpu_brand", cpu_brand}, {"has_feature", cpu_has_feature},
    {"rdtsc", cpu_rdtsc}, {"rdtscp", cpu_rdtscp}, {"rdrand", cpu_rdrand}, {"rdseed", cpu_rdseed},
    {"pause", cpu_pause}, {"fence", cpu_fence},
};
const int rhex_cpu_count = sizeof(rhex_cpu_entries) / sizeof(rhex_cpu_entries[0]);
