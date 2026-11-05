/* KingCobra std.rhex — run raw machine code directly from an
 * executable memory buffer, plus the CPU-/hardware-access entry
 * points that live at the same "I am touching the real machine"
 * level (CPU feature detection, timestamp/entropy instructions,
 * MSRs, port I/O, cache control).
 *
 * This is the sharpest tool in the standard library. Runs the HOST CPU's machine code (x86-64, ARM64, ARM32, RISC-V); see rhex.arch()
 * (Linux and Windows both supported via compiler/include/kc_platform.h
 * and compiler/include/kc_hw.h) — other architectures refuse rather
 * than guess at machine-code or instruction semantics they weren't
 * written for.
 *
 * v0.1 was a single file with one call shape (zero-argument code,
 * one-way writable->executable). This module aggregates four focused
 * files (see rhex_glue.h) the way std.nn aggregates its own files,
 * and adds: read-back and re-patching of buffers, typed/argumented
 * calls (reusing the same calling-convention inference the native-
 * import FFI already relies on), and a real hardware-access surface
 * (cpuid/rdtsc/rdrand/rdseed/msr/port I/O/clflush) instead of only
 * "execute whatever bytes you hand me".
 *
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com>
 * License: MIT
 */
#include "rhex_glue.h"
#include <stdarg.h>

void rhex_require_unsafe(KcInterp *ip, KcPos pos, const char *who) {
    if (ip->in_unsafe <= 0)
        kc_error_at(pos, "%s: raw hardware access requires an `unsafe { }` block", who);
    kc_forbid_in_comptime(ip, pos, who); /* raw machine code / real hardware is never a "compile-time constant" */
}

KcHandle *rhex_expect_buf(KcValue v, KcPos pos, const char *who) {
    if (v.type != VAL_HANDLE || v.as.handle->kind != H_JITBUF)
        kc_error_at(pos, "%s: expected an rhex.alloc() handle", who);
    return v.as.handle;
}

KcHandle *rhex_live_buf(KcValue v, KcPos pos, const char *who) {
    KcHandle *h = rhex_expect_buf(v, pos, who);
    if (!h->ptr) kc_error_at(pos, "%s: buffer was already freed", who);
    return h;
}

static char g_rhex_err[256];

void rhex_set_error(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(g_rhex_err, sizeof g_rhex_err, fmt, ap);
    va_end(ap);
}
const char *rhex_get_error(void) { return g_rhex_err; }

KcModule *kc_mod_rhex(void) {
    static KcModule m; static bool ready = false;
    if (!ready) {
        int total = rhex_buf_count + rhex_call_count + rhex_cpu_count + rhex_hw_count;
        KcModuleEntry *e = kc_malloc(sizeof(KcModuleEntry) * (size_t)total);
        int k = 0;
        for (int i = 0; i < rhex_buf_count; i++)  { e[k].name = (char *)rhex_buf_entries[i].name;  e[k].fn = rhex_buf_entries[i].fn;  k++; }
        for (int i = 0; i < rhex_call_count; i++) { e[k].name = (char *)rhex_call_entries[i].name; e[k].fn = rhex_call_entries[i].fn; k++; }
        for (int i = 0; i < rhex_cpu_count; i++)  { e[k].name = (char *)rhex_cpu_entries[i].name;  e[k].fn = rhex_cpu_entries[i].fn;  k++; }
        for (int i = 0; i < rhex_hw_count; i++)   { e[k].name = (char *)rhex_hw_entries[i].name;   e[k].fn = rhex_hw_entries[i].fn;   k++; }
        m.name = "rhex"; m.entries = e; m.count = total; ready = true;
    }
    return &m;
}
