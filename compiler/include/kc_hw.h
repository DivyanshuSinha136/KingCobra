/* KingCobra hardware-access platform layer.
 *
 * Backs std.mmio and std.acpi. Kept separate from kc_platform.h on
 * purpose: only those two modules need raw device mappings, port I/O,
 * CPU barriers and a monotonic clock, and nothing else in the tree
 * should grow a dependency on them.
 *
 * What is (and is not) possible from user space, stated plainly:
 *
 *   Linux    physical memory via /dev/mem (root, subject to the kernel's
 *            STRICT_DEVMEM policy); PCI BARs via sysfs resourceN /
 *            resourceN_wc (the kernel-mediated, preferred route); UIO
 *            device maps; arbitrary file/device mappings; anonymous
 *            ("virtual") regions; x86 port I/O through ioperm/iopl.
 *   Windows  file mappings and anonymous regions. There is NO supported
 *            way to map arbitrary physical memory or touch I/O ports
 *            from user mode without a kernel driver, so those entry
 *            points report that honestly instead of pretending.
 *
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com>
 * License: MIT
 */
#ifndef KC_HW_H
#define KC_HW_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#if defined(_WIN32)
#  define KC_HW_WINDOWS 1
#  include <windows.h>
#else
#  define KC_HW_POSIX 1
#  include <fcntl.h>
#  include <unistd.h>
#  include <sched.h>
#  include <time.h>
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  if (defined(__x86_64__) || defined(__i386__)) && !defined(KC_ARCH_FORCE_GENERIC)
#    if defined(__has_include)
#      if __has_include(<sys/io.h>)
#        include <sys/io.h>
#        define KC_HW_HAVE_PORT_IO 1
#      endif
#    endif
#  endif
/* /dev/mem offsets are physical addresses: off_t must be 64-bit. The
 * Makefile passes -D_FILE_OFFSET_BITS=64; fail loudly if it was lost. */
_Static_assert(sizeof(off_t) >= 8, "build with -D_FILE_OFFSET_BITS=64");
#endif

#include "kc_arch.h"
#if defined(KC_ARCH_X86_64) || defined(KC_ARCH_X86)
#  define KC_HW_X86 1
#elif defined(KC_ARCH_ARM64)
#  define KC_HW_ARM64 1
#elif defined(KC_ARCH_ARM32) && defined(__ARM_ARCH) && __ARM_ARCH >= 7
#  define KC_HW_ARM32 1     /* ARMv7+ (armhf): DMB exists; ARMv6 and older fall to __sync_synchronize */
#elif defined(KC_ARCH_RISCV64) || defined(KC_ARCH_RISCV32)
#  define KC_HW_RISCV 1
#endif

/* Single-copy-atomic 64-bit accesses need a 64-bit host. */
#if UINTPTR_MAX > 0xFFFFFFFFu
#  define KC_HW_HAS_64BIT_ACCESS 1
#else
#  define KC_HW_HAS_64BIT_ACCESS 0
#endif

/* ============================ barriers ============================ */

#define KC_HW_BAR_FULL  0
#define KC_HW_BAR_READ  1
#define KC_HW_BAR_WRITE 2
#define KC_HW_BAR_COMPILER 3

static inline void kc_hw_barrier(int kind) {
    switch (kind) {
    case KC_HW_BAR_COMPILER: __asm__ volatile("" ::: "memory"); return;
#if defined(KC_HW_X86)
    case KC_HW_BAR_READ:  __asm__ volatile("lfence" ::: "memory"); return;
    case KC_HW_BAR_WRITE: __asm__ volatile("sfence" ::: "memory"); return;
    default:              __asm__ volatile("mfence" ::: "memory"); return;
#elif defined(KC_HW_ARM64)
    case KC_HW_BAR_READ:  __asm__ volatile("dmb ld" ::: "memory"); return;
    case KC_HW_BAR_WRITE: __asm__ volatile("dmb st" ::: "memory"); return;
    default:              __asm__ volatile("dmb sy" ::: "memory"); return;
#elif defined(KC_HW_ARM32)
    case KC_HW_BAR_READ:  __asm__ volatile("dmb ish"   ::: "memory"); return;
    case KC_HW_BAR_WRITE: __asm__ volatile("dmb ishst" ::: "memory"); return;
    default:              __asm__ volatile("dmb ish"   ::: "memory"); return;
#elif defined(KC_HW_RISCV)
    case KC_HW_BAR_READ:  __asm__ volatile("fence r,r"   ::: "memory"); return;
    case KC_HW_BAR_WRITE: __asm__ volatile("fence w,w"   ::: "memory"); return;
    default:              __asm__ volatile("fence rw,rw" ::: "memory"); return;
#else
    default:              __sync_synchronize(); return;
#endif
    }
}

/* Ordering applied around every std.mmio register access, mirroring the
 * Linux readl()/writel() contract: a write is ordered after earlier
 * memory operations, a read is ordered before later ones. On x86 the
 * hardware already gives uncached MMIO strong ordering, so a compiler
 * barrier is all that is needed; on ARM64 device memory needs real DMBs. */
static inline void kc_hw_pre_write(void) {
#if defined(KC_HW_ARM64)
    __asm__ volatile("dmb oshst" ::: "memory");
#elif defined(KC_HW_ARM32)
    __asm__ volatile("dmb ishst" ::: "memory");
#elif defined(KC_HW_RISCV)
    __asm__ volatile("fence w,o" ::: "memory");     /* the Linux __io_bw() ordering */
#else
    __asm__ volatile("" ::: "memory");
#endif
}
static inline void kc_hw_post_read(void) {
#if defined(KC_HW_ARM64)
    __asm__ volatile("dmb oshld" ::: "memory");
#elif defined(KC_HW_ARM32)
    __asm__ volatile("dmb ish" ::: "memory");
#elif defined(KC_HW_RISCV)
    __asm__ volatile("fence i,r" ::: "memory");     /* the Linux __io_ar() ordering */
#else
    __asm__ volatile("" ::: "memory");
#endif
}

/* ================== exact-width volatile accessors ================= */
/* One load/store of exactly the requested width: never memcpy, never a
 * compiler-chosen wider or split access (device registers care). */

static inline uint64_t kc_hw_rd(const volatile uint8_t *p, int w) {
    switch (w) {
    case 1: return *(const volatile uint8_t  *)p;
    case 2: return *(const volatile uint16_t *)p;
    case 4: return *(const volatile uint32_t *)p;
#if KC_HW_HAS_64BIT_ACCESS
    case 8: return *(const volatile uint64_t *)p;
#endif
    }
    return 0;
}
static inline void kc_hw_wr(volatile uint8_t *p, int w, uint64_t v) {
    switch (w) {
    case 1: *(volatile uint8_t  *)p = (uint8_t)v;  break;
    case 2: *(volatile uint16_t *)p = (uint16_t)v; break;
    case 4: *(volatile uint32_t *)p = (uint32_t)v; break;
#if KC_HW_HAS_64BIT_ACCESS
    case 8: *(volatile uint64_t *)p = v; break;
#endif
    }
}

static inline uint64_t kc_hw_bswap(uint64_t v, int w) {
    switch (w) {
    case 2: return __builtin_bswap16((uint16_t)v);
    case 4: return __builtin_bswap32((uint32_t)v);
    case 8: return __builtin_bswap64(v);
    default: return v;
    }
}

/* ============================== time =============================== */

static inline uint64_t kc_hw_now_ns(void) {
#if defined(KC_HW_WINDOWS)
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (uint64_t)((double)c.QuadPart * 1e9 / (double)freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

static inline void kc_hw_cpu_relax(void) {
#if defined(KC_HW_X86)
    __asm__ volatile("pause" ::: "memory");
#elif defined(KC_HW_ARM64) || defined(KC_HW_ARM32)
    __asm__ volatile("yield" ::: "memory");
#elif defined(KC_HW_RISCV)
    /* `pause` (Zihintpause) is the hint-encoded fence 0x0100000F; on a core
     * without the extension it executes as a harmless no-op fence hint. */
    __asm__ volatile(".4byte 0x0100000F" ::: "memory");
#else
    __asm__ volatile("" ::: "memory");
#endif
}

static inline void kc_hw_yield(void) {
#if defined(KC_HW_WINDOWS)
    SwitchToThread();
#else
    sched_yield();
#endif
}

/* ============================ mappings ============================= */

typedef struct KcHwMap {
    void             *map_base; /* start of the OS mapping (page aligned) */
    size_t            map_len;  /* length of the OS mapping               */
    volatile uint8_t *ptr;      /* first byte of the requested window     */
    size_t            len;      /* length of the requested window         */
    int               anon;     /* 1 = anonymous / VirtualAlloc region    */
} KcHwMap;

static inline void kc_hw_set_err(char *err, size_t n, const char *what, const char *detail) {
    if (err && n) snprintf(err, n, "%s: %s", what, detail);
}

static inline size_t kc_hw_page_size(void) {
#if defined(KC_HW_WINDOWS)
    SYSTEM_INFO si; GetSystemInfo(&si);
    return (size_t)si.dwAllocationGranularity; /* 64 KiB: the mapping granule */
#else
    long ps = sysconf(_SC_PAGESIZE);
    return ps > 0 ? (size_t)ps : 4096u;
#endif
}

/* Map [offset, offset+len) of a file or device node. `sync` requests
 * O_SYNC, which is what makes /dev/mem mappings uncached. */
static inline int kc_hw_map_file(KcHwMap *m, const char *path, uint64_t offset, size_t len,
                                 bool writable, bool sync, char *err, size_t errn) {
    memset(m, 0, sizeof *m);
    if (len == 0) { kc_hw_set_err(err, errn, path, "zero-length mapping"); return -1; }
    size_t ps = kc_hw_page_size();
    uint64_t aligned = offset & ~((uint64_t)ps - 1);
    size_t delta = (size_t)(offset - aligned);
    if (len > SIZE_MAX - delta) { kc_hw_set_err(err, errn, path, "mapping length overflows"); return -1; }
    size_t total = len + delta;
#if defined(KC_HW_WINDOWS)
    (void)sync;
    HANDLE hf = CreateFileA(path, GENERIC_READ | (writable ? GENERIC_WRITE : 0),
                            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (hf == INVALID_HANDLE_VALUE) {
        char b[64]; snprintf(b, sizeof b, "CreateFile failed (Windows error %lu)", (unsigned long)GetLastError());
        kc_hw_set_err(err, errn, path, b); return -1;
    }
    HANDLE hm = CreateFileMappingA(hf, NULL, writable ? PAGE_READWRITE : PAGE_READONLY, 0, 0, NULL);
    if (!hm) {
        char b[64]; snprintf(b, sizeof b, "CreateFileMapping failed (Windows error %lu)", (unsigned long)GetLastError());
        CloseHandle(hf); kc_hw_set_err(err, errn, path, b); return -1;
    }
    void *p = MapViewOfFile(hm, writable ? FILE_MAP_WRITE : FILE_MAP_READ,
                            (DWORD)(aligned >> 32), (DWORD)(aligned & 0xFFFFFFFFu), total);
    CloseHandle(hm); CloseHandle(hf); /* the view keeps the mapping alive */
    if (!p) {
        char b[64]; snprintf(b, sizeof b, "MapViewOfFile failed (Windows error %lu)", (unsigned long)GetLastError());
        kc_hw_set_err(err, errn, path, b); return -1;
    }
#else
    if (aligned > (uint64_t)INT64_MAX) { kc_hw_set_err(err, errn, path, "offset exceeds off_t range"); return -1; }
    int fd = open(path, (writable ? O_RDWR : O_RDONLY) | O_CLOEXEC | (sync ? O_SYNC : 0));
    if (fd < 0) { kc_hw_set_err(err, errn, path, strerror(errno)); return -1; }
    void *p = mmap(NULL, total, PROT_READ | (writable ? PROT_WRITE : 0), MAP_SHARED, fd, (off_t)aligned);
    int saved = errno;
    close(fd);
    if (p == MAP_FAILED) { kc_hw_set_err(err, errn, path, strerror(saved)); return -1; }
#endif
    m->map_base = p; m->map_len = total;
    m->ptr = (volatile uint8_t *)p + delta; m->len = len; m->anon = 0;
    return 0;
}

/* Zero-filled, page-aligned, read/write memory (software "device"). */
static inline int kc_hw_map_anon(KcHwMap *m, size_t len, char *err, size_t errn) {
    memset(m, 0, sizeof *m);
    if (len == 0) { kc_hw_set_err(err, errn, "virtual region", "zero length"); return -1; }
#if defined(KC_HW_WINDOWS)
    void *p = VirtualAlloc(NULL, len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!p) { kc_hw_set_err(err, errn, "virtual region", "VirtualAlloc failed"); return -1; }
#else
    void *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) { kc_hw_set_err(err, errn, "virtual region", strerror(errno)); return -1; }
#endif
    m->map_base = p; m->map_len = len; m->ptr = (volatile uint8_t *)p; m->len = len; m->anon = 1;
    return 0;
}

static inline void kc_hw_unmap(KcHwMap *m) {
    if (!m->map_base) return;
#if defined(KC_HW_WINDOWS)
    if (m->anon) VirtualFree(m->map_base, 0, MEM_RELEASE); else UnmapViewOfFile(m->map_base);
#else
    munmap(m->map_base, m->map_len);
#endif
    memset(m, 0, sizeof *m);
}

/* Physical memory. Linux: /dev/mem with O_SYNC (uncached). */
static inline int kc_hw_map_phys(KcHwMap *m, uint64_t phys, size_t len, bool writable,
                                 char *err, size_t errn) {
#if defined(KC_HW_WINDOWS)
    (void)phys; (void)len; (void)writable; memset(m, 0, sizeof *m);
    kc_hw_set_err(err, errn, "physical memory",
                  "not available on Windows from user mode (needs a kernel driver); "
                  "use mmio.map_file on a driver-exposed device, or mmio.virtual");
    return -1;
#else
    return kc_hw_map_file(m, "/dev/mem", phys, len, writable, true, err, errn);
#endif
}

/* ============================ port I/O ============================= */
/* x86 only. /dev/port is NOT used: its read/write handlers issue byte
 * accesses to consecutive ports, which is wrong for 16/32-bit registers
 * (e.g. the 32-bit ACPI PM timer). Real inw/inl/outw/outl need ioperm
 * (ports < 0x400) or iopl(3), which require root / CAP_SYS_RAWIO. */

static inline bool kc_hw_port_supported(void) {
#if defined(KC_HW_HAVE_PORT_IO)
    return true;
#else
    return false;
#endif
}

static inline int kc_hw_port_access(uint16_t port, int w, char *err, size_t errn) {
#if defined(KC_HW_HAVE_PORT_IO)
    static int have_iopl = 0;
    if (have_iopl) return 0;
    if (port + (unsigned)w <= 0x400u && ioperm(port, (unsigned long)w, 1) == 0) return 0;
    if (iopl(3) == 0) { have_iopl = 1; return 0; }
    kc_hw_set_err(err, errn, "port I/O", strerror(errno));
    return -1;
#else
    (void)port; (void)w;
    kc_hw_set_err(err, errn, "port I/O", "only supported on x86 Linux builds with <sys/io.h>");
    return -1;
#endif
}

static inline uint32_t kc_hw_port_in(uint16_t port, int w) {
#if defined(KC_HW_HAVE_PORT_IO)
    uint32_t v = 0;
    switch (w) {
    case 1: { uint8_t  b; __asm__ volatile("inb %w1,%b0" : "=a"(b) : "Nd"(port)); v = b; break; }
    case 2: { uint16_t s; __asm__ volatile("inw %w1,%w0" : "=a"(s) : "Nd"(port)); v = s; break; }
    case 4: { __asm__ volatile("inl %w1,%0" : "=a"(v) : "Nd"(port)); break; }
    }
    return v;
#else
    (void)port; (void)w; return 0;
#endif
}

static inline void kc_hw_port_out(uint16_t port, int w, uint32_t v) {
#if defined(KC_HW_HAVE_PORT_IO)
    switch (w) {
    case 1: __asm__ volatile("outb %b0,%w1" : : "a"((uint8_t)v),  "Nd"(port)); break;
    case 2: __asm__ volatile("outw %w0,%w1" : : "a"((uint16_t)v), "Nd"(port)); break;
    case 4: __asm__ volatile("outl %0,%w1"  : : "a"(v),           "Nd"(port)); break;
    }
#else
    (void)port; (void)w; (void)v;
#endif
}

#endif /* KC_HW_H */
