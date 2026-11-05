/* KingCobra std.rhex — real-hardware access: model-specific registers,
 * x86 port I/O, and cache-line control.
 *
 * Unlike rhex_cpu.c, every entry here requires `unsafe { }`: these can
 * affect or depend on real hardware state outside the process (MSRs
 * can halt/misconfigure the CPU; ports can hit arbitrary devices).
 *
 * Error model, same split std.mmio already uses:
 *   - programming errors (wrong types, bad width, missing `unsafe`)
 *     are fatal, like the rest of std.rhex;
 *   - environmental failures (no permission, no such device, not
 *     supported on this OS) return null/false and set
 *     rhex.last_error() instead of crashing the whole program.
 *
 * Port I/O reuses the exact same kc_hw_port_* functions in kc_hw.h
 * that std.mmio's port_in/port_out call — one real implementation,
 * shared, not two copies that could drift.
 *
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com>
 * License: MIT
 */
#include "rhex_glue.h"

#if defined(KC_HW_POSIX)
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#endif

static KcValue hw_last_error(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; (void)a;
    if (n != 0) kc_error_at(pos, "rhex.last_error() -> string");
    return kc_string(rhex_get_error());
}

static int want_width(KcValue v, KcPos pos, const char *who) {
    if (v.type != VAL_INT || (v.as.i != 1 && v.as.i != 2 && v.as.i != 4))
        kc_error_at(pos, "%s: width must be 1, 2 or 4 bytes", who);
    return (int)v.as.i;
}

/* ------------------------------------------------------- MSRs (Linux only) */

static KcValue hw_msr_read(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    rhex_require_unsafe(ip, pos, "rhex.msr_read");
    if (n < 1 || n > 2 || a[0].type != VAL_INT) kc_error_at(pos, "rhex.msr_read(msr: int [, cpu: int]) -> int | null");
    int64_t cpu = n == 2 ? a[1].as.i : 0;
    if (n == 2 && a[1].type != VAL_INT) kc_error_at(pos, "rhex.msr_read: cpu must be an int");
#if defined(KC_HW_WINDOWS)
    (void)cpu; rhex_set_error("msr_read: not available on Windows from user mode (needs a kernel driver)");
    return kc_null();
#else
    char path[64]; snprintf(path, sizeof path, "/dev/cpu/%lld/msr", (long long)cpu);
    int fd = open(path, O_RDONLY);
    if (fd < 0) { rhex_set_error("msr_read: %s: %s", path, strerror(errno)); return kc_null(); }
    uint64_t val = 0;
    ssize_t r = pread(fd, &val, sizeof val, (off_t)a[0].as.i);
    int saved = errno; close(fd);
    if (r != (ssize_t)sizeof val) { rhex_set_error("msr_read: pread failed: %s", strerror(saved)); return kc_null(); }
    return kc_int((int64_t)val);
#endif
}

static KcValue hw_msr_write(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    rhex_require_unsafe(ip, pos, "rhex.msr_write");
    if (n < 2 || n > 3 || a[0].type != VAL_INT) kc_error_at(pos, "rhex.msr_write(msr: int, value: int [, cpu: int]) -> bool");
    int64_t cpu = n == 3 ? a[2].as.i : 0;
    if (n == 3 && a[2].type != VAL_INT) kc_error_at(pos, "rhex.msr_write: cpu must be an int");
#if defined(KC_HW_WINDOWS)
    (void)cpu; rhex_set_error("msr_write: not available on Windows from user mode (needs a kernel driver)");
    return kc_bool(false);
#else
    char path[64]; snprintf(path, sizeof path, "/dev/cpu/%lld/msr", (long long)cpu);
    int fd = open(path, O_WRONLY);
    if (fd < 0) { rhex_set_error("msr_write: %s: %s", path, strerror(errno)); return kc_bool(false); }
    uint64_t val = (uint64_t)a[1].as.i;
    ssize_t w = pwrite(fd, &val, sizeof val, (off_t)a[0].as.i);
    int saved = errno; close(fd);
    if (w != (ssize_t)sizeof val) { rhex_set_error("msr_write: pwrite failed: %s", strerror(saved)); return kc_bool(false); }
    return kc_bool(true);
#endif
}

/* ------------------------------------------------------- port I/O (x86 Linux) */

static KcValue hw_port_in(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    rhex_require_unsafe(ip, pos, "rhex.port_in");
    if (n != 2 || a[0].type != VAL_INT) kc_error_at(pos, "rhex.port_in(port: int, width: int) -> int | null");
    if (a[0].as.i < 0 || a[0].as.i > 0xFFFF) kc_error_at(pos, "rhex.port_in: port must be 0..65535");
    int w = want_width(a[1], pos, "rhex.port_in");
    if (!kc_hw_port_supported()) { rhex_set_error("port_in: only supported on x86 Linux builds with <sys/io.h>"); return kc_null(); }
    char err[160];
    if (kc_hw_port_access((uint16_t)a[0].as.i, w, err, sizeof err) != 0) { rhex_set_error("port_in: %s", err); return kc_null(); }
    return kc_int((int64_t)kc_hw_port_in((uint16_t)a[0].as.i, w));
}

static KcValue hw_port_out(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    rhex_require_unsafe(ip, pos, "rhex.port_out");
    if (n != 3 || a[0].type != VAL_INT || a[2].type != VAL_INT) kc_error_at(pos, "rhex.port_out(port: int, width: int, value: int) -> bool");
    if (a[0].as.i < 0 || a[0].as.i > 0xFFFF) kc_error_at(pos, "rhex.port_out: port must be 0..65535");
    int w = want_width(a[1], pos, "rhex.port_out");
    if (!kc_hw_port_supported()) { rhex_set_error("port_out: only supported on x86 Linux builds with <sys/io.h>"); return kc_bool(false); }
    char err[160];
    if (kc_hw_port_access((uint16_t)a[0].as.i, w, err, sizeof err) != 0) { rhex_set_error("port_out: %s", err); return kc_bool(false); }
    kc_hw_port_out((uint16_t)a[0].as.i, w, (uint32_t)a[2].as.i);
    return kc_bool(true);
}

/* ------------------------------------------------------- cache control */

static KcValue hw_clflush(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    rhex_require_unsafe(ip, pos, "rhex.clflush");
    if (n != 2 || a[1].type != VAL_INT || a[1].as.i < 0) kc_error_at(pos, "rhex.clflush(handle, offset: int)");
    KcHandle *h = rhex_live_buf(a[0], pos, "rhex.clflush");
    if ((size_t)a[1].as.i >= h->size) kc_error_at(pos, "rhex.clflush: offset out of bounds (size %zu)", h->size);
#ifdef KC_ARCH_HAS_X86_INSNS
    volatile uint8_t *p = (uint8_t *)h->ptr + a[1].as.i;
    __asm__ volatile("clflush (%0)" :: "r"(p) : "memory");
#else
    kc_error_at(pos, "rhex.clflush " KC_RHEX_X86_MSG);
#endif
    return kc_null();
}

const RhexEntry rhex_hw_entries[] = {
    {"msr_read", hw_msr_read}, {"msr_write", hw_msr_write},
    {"port_in", hw_port_in}, {"port_out", hw_port_out},
    {"clflush", hw_clflush}, {"last_error", hw_last_error},
};
const int rhex_hw_count = sizeof(rhex_hw_entries) / sizeof(rhex_hw_entries[0]);
