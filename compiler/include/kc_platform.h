/* KingCobra platform abstraction layer.
 *
 * Everything in compiler/ and std/ that differs between Linux and
 * Windows funnels through here: dynamic library loading (for `extern`
 * FFI), executable memory (for the closure JIT and `rhex`), sockets
 * (for std.network / std.http), and a handful of POSIX calls that
 * don't exist as such on Windows (getcwd, getpid, msec sleep).
 *
 * Linux/macOS/BSD path uses the real POSIX calls. Windows path uses
 * Win32/Winsock equivalents. Nothing outside this header should
 * `#include <dlfcn.h>`, `#include <sys/mman.h>`, or touch BSD socket
 * headers directly — include "kc_platform.h" instead.
 *
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com>
 * License: MIT
 */
#ifndef KC_PLATFORM_H
#define KC_PLATFORM_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#if defined(_WIN32)
#define KC_PLATFORM_WINDOWS 1
#else
#define KC_PLATFORM_LINUX 1
#endif

/* ===================== dynamic library loading ===================== */
/* Backs `extern fn ... from "lib";` (see compiler/src/ffi.c). */

#if defined(KC_PLATFORM_WINDOWS)
#include <windows.h>

typedef HMODULE kc_dl_handle;

static inline kc_dl_handle kc_dlopen(const char *path) {
    if (!path) {
        /* "load from self" — closest Windows equivalent is the main
         * executable's own module, which exposes no exported symbols
         * by default. Kept for API symmetry with the Linux path. */
        return GetModuleHandleA(NULL);
    }
    return LoadLibraryA(path);
}
static inline void *kc_dlsym(kc_dl_handle h, const char *name) {
    return (void *)GetProcAddress(h, name);
}
static inline const char *kc_dlerror(void) {
    static char buf[160];
    snprintf(buf, sizeof buf, "Windows error 0x%lx", (unsigned long)GetLastError());
    return buf;
}
#define KC_DL_NULL ((kc_dl_handle)NULL)

/* Windows has no equivalent of libm.so/libc.so: math and C-runtime
 * functions live inside whichever CRT DLL the process is already
 * linked against (ucrtbase.dll on UCRT toolchains, msvcrt.dll on
 * older/MSVCRT-based MinGW builds — which one depends on how the
 * KingCobra interpreter itself was built, not on the user's script).
 * So that the SAME `extern ... from "libm.so.6"` KingCobra source
 * works unmodified on Linux and Windows, common *nix library names
 * are mapped here to the Windows DLL(s) that actually export those
 * symbols; we try each candidate with GetModuleHandle first (since
 * the CRT DLL our own process depends on is already loaded) and fall
 * back to LoadLibrary. An unrecognized name is passed through as a
 * literal DLL name (e.g. a user who already wrote "msvcrt.dll"). */
static inline kc_dl_handle kc_dlopen_portable(const char *name) {
    static const char *MATH_ALIASES[] = { "ucrtbase.dll", "msvcrt.dll", "api-ms-win-crt-math-l1-1-0.dll", NULL };
    static const char *LIBC_ALIASES[] = { "ucrtbase.dll", "msvcrt.dll", "kernel32.dll", NULL };
    const char **candidates = NULL;
    if (name && (strncmp(name, "libm.so", 7) == 0 || strcmp(name, "libm") == 0))
        candidates = MATH_ALIASES;
    else if (name && (strncmp(name, "libc.so", 7) == 0 || strcmp(name, "libc") == 0))
        candidates = LIBC_ALIASES;
    if (!candidates) return kc_dlopen(name);
    for (int i = 0; candidates[i]; i++) {
        HMODULE h = GetModuleHandleA(candidates[i]);
        if (h) return h;
        h = LoadLibraryA(candidates[i]);
        if (h) return h;
    }
    return KC_DL_NULL;
}

#else /* Linux/macOS/BSD */
#include <dlfcn.h>

typedef void *kc_dl_handle;

static inline kc_dl_handle kc_dlopen(const char *path) {
    return path ? dlopen(path, RTLD_LAZY | RTLD_GLOBAL) : dlopen(NULL, RTLD_LAZY | RTLD_GLOBAL);
}
static inline void *kc_dlsym(kc_dl_handle h, const char *name) { return dlsym(h, name); }
static inline const char *kc_dlerror(void) { return dlerror(); }
#define KC_DL_NULL ((kc_dl_handle)NULL)

/* No alias mapping needed: the real libm.so/libc.so are exactly what
 * was asked for. */
static inline kc_dl_handle kc_dlopen_portable(const char *name) { return kc_dlopen(name); }

#endif

/* ========================= executable memory ========================= */
/* Backs the closure JIT (compiler/src/jit.c) and std.rhex. Both emit
 * raw x86-64 machine code themselves; this layer only owns "give me N
 * writable bytes", "flip them read+exec", and "release them". */

#include "kc_arch.h"

/* Instruction-cache coherence. x86 keeps instruction fetch coherent with
 * data stores, so freshly written code can simply be run. ARM and RISC-V do
 * NOT: after writing code, the data cache must be cleaned and the
 * instruction cache invalidated for that range before the first jump into
 * it, otherwise the CPU can execute stale bytes. kc_exec_seal() does this,
 * so every caller (std.rhex today; any future code generator) gets it. */
static inline void kc_icache_flush(void *p, size_t size) {
#if defined(KC_ARCH_X86_64) || defined(KC_ARCH_X86)
    (void)p; (void)size;
#elif defined(KC_PLATFORM_WINDOWS)
    FlushInstructionCache(GetCurrentProcess(), p, size);
#elif defined(__GNUC__) || defined(__clang__)
    __builtin___clear_cache((char *)p, (char *)p + size);
#else
    (void)p; (void)size;
#endif
}

#if defined(KC_PLATFORM_WINDOWS)

static inline void *kc_exec_alloc(size_t size) {
    return VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
}
static inline int kc_exec_seal(void *p, size_t size) {
    DWORD old;
    if (!VirtualProtect(p, size, PAGE_EXECUTE_READ, &old)) return -1;
    kc_icache_flush(p, size);
    return 0;
}
static inline void kc_exec_free(void *p, size_t size) {
    (void)size;
    VirtualFree(p, 0, MEM_RELEASE);
}
/* Reverse of kc_exec_seal: back to writable (not executable), so a
 * sealed buffer can be patched and re-sealed instead of only ever
 * going writable -> executable once. Kept as its own call (not a
 * general "set any combination" API) so nothing can ever ask for
 * W+X together — std.rhex enforces W^X through this pair. */
static inline int kc_exec_unseal(void *p, size_t size) {
    DWORD old;
    return VirtualProtect(p, size, PAGE_READWRITE, &old) ? 0 : -1;
}

#else
#include <sys/mman.h>

static inline void *kc_exec_alloc(size_t size) {
    void *m = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return m == MAP_FAILED ? NULL : m;
}
static inline int kc_exec_seal(void *p, size_t size) {
    if (mprotect(p, size, PROT_READ | PROT_EXEC) != 0) return -1;
    kc_icache_flush(p, size);
    return 0;
}
static inline int kc_exec_unseal(void *p, size_t size) {
    return mprotect(p, size, PROT_READ | PROT_WRITE);
}
static inline void kc_exec_free(void *p, size_t size) {
    munmap(p, size);
}

#endif

/* The closure JIT is an x86-64 machine code generator: it works on any OS
 * running that CPU, since only the memory-protection call above differs by
 * platform. On every other architecture (ARM64, ARMhf, RISC-V, ...) there
 * is no JIT and programs run in the interpreter — see kc_arch.h. */
#if defined(KC_ARCH_HAS_JIT)
#define KC_NATIVE_CODEGEN_SUPPORTED 1
#endif

/* ================================ sockets ================================ */
/* Backs std.network and std.http. */

#if defined(KC_PLATFORM_WINDOWS)
#include <winsock2.h>
#include <ws2tcpip.h>

typedef SOCKET kc_sock_t;
#define KC_SOCK_INVALID INVALID_SOCKET
#define kc_sock_close closesocket
#define kc_sock_errno() WSAGetLastError()

/* Call once before any socket use; safe to call repeatedly. */
static inline int kc_sockets_init(void) {
    static int done = 0;
    if (done) return 0;
    WSADATA wsa;
    int rc = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (rc == 0) done = 1;
    return rc;
}

#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>  /* TCP_NODELAY — std.network's set_nodelay */
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>

typedef int kc_sock_t;
#define KC_SOCK_INVALID (-1)
#define kc_sock_close close
#define kc_sock_errno() errno

static inline int kc_sockets_init(void) { return 0; } /* nothing to do on POSIX */

#endif

/* ========================== small POSIX gaps ========================== */

#if defined(KC_PLATFORM_WINDOWS)
#include <direct.h>   /* _getcwd */
#include <process.h>  /* _getpid */

static inline char *kc_getcwd(char *buf, size_t size) { return _getcwd(buf, (int)size); }
static inline long kc_getpid(void) { return (long)_getpid(); }
static inline void kc_sleep_seconds(double secs) { Sleep((DWORD)(secs * 1000.0)); }
static inline void kc_setenv(const char *name, const char *value) { SetEnvironmentVariableA(name, value); }

#else
#include <unistd.h>

static inline char *kc_getcwd(char *buf, size_t size) { return getcwd(buf, size); }
static inline long kc_getpid(void) { return (long)getpid(); }
static inline void kc_sleep_seconds(double secs) { usleep((useconds_t)(secs * 1e6)); }
static inline void kc_setenv(const char *name, const char *value) { setenv(name, value, 1); }

#endif

/* ============================ monotonic clock ============================ */
/* Backs the `clock()` builtin (compiler/src/builtins.c). */

#if defined(KC_PLATFORM_WINDOWS)

static inline double kc_monotonic_seconds(void) {
    static LARGE_INTEGER freq = {0};
    LARGE_INTEGER now;
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart / (double)freq.QuadPart;
}

#else
#include <time.h>

static inline double kc_monotonic_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

#endif

/* ============================ console colors ============================ */
/* Backs the red/yellow/bold error and warning output in compiler/src/errors.c.
 * POSIX terminals interpret ANSI escapes by default; Windows consoles
 * (cmd.exe, PowerShell, Windows Terminal) need ENABLE_VIRTUAL_TERMINAL_
 * PROCESSING turned on first, or the raw escape bytes print as visible
 * garbage instead of coloring the text. Call once at startup. */

#if defined(KC_PLATFORM_WINDOWS)
#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif

static inline void kc_enable_ansi_colors(void) {
    /* Source strings (REPL banner, help text, user programs) are UTF-8, but a
     * Windows console defaults to an OEM code page (437/850), which turns the
     * UTF-8 bytes of an em dash into "ΓÇö". Switch the console to UTF-8 so
     * output and typed input round-trip correctly. SetConsole*CP fail
     * harmlessly when there is no console (redirected output). */
    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);
    HANDLE handles[2] = { GetStdHandle(STD_OUTPUT_HANDLE), GetStdHandle(STD_ERROR_HANDLE) };
    for (int i = 0; i < 2; i++) {
        if (handles[i] == INVALID_HANDLE_VALUE || handles[i] == NULL) continue;
        DWORD mode = 0;
        if (GetConsoleMode(handles[i], &mode)) {
            SetConsoleMode(handles[i], mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        }
        /* GetConsoleMode fails harmlessly when stdout/stderr is redirected
         * to a file or pipe rather than a real console — nothing to do
         * in that case, since no escapes are visible there anyway. On
         * Windows versions older than 10 (build 1511), SetConsoleMode
         * with this flag simply fails and we leave colors off rather
         * than guess further. */
    }
}
#else
static inline void kc_enable_ansi_colors(void) { /* POSIX terminals already honor ANSI escapes */ }
#endif

/* terminal detection, shared by the REPL, diagnostics and the CLI */
#if defined(_WIN32)
#  include <io.h>
#  define KC_ISATTY(fd) _isatty(fd)
#  define KC_FILENO(f)  _fileno(f)
#else
#  include <unistd.h>
#  define KC_ISATTY(fd) isatty(fd)
#  define KC_FILENO(f)  fileno(f)
#endif

#endif /* KC_PLATFORM_H */
