/* KingCobra std.system — system information: OS, architecture, CPU count,
 * memory, uptime, load, user/host identity, terminal, disk usage.
 *
 * Stable facts (os, arch, hostname, cpu_count, ...) are allowed inside
 * comptime { }. Volatile readings (free memory, uptime, load, disk usage)
 * are not — a "compile-time constant" must not depend on when it ran.
 *
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com>
 */
#include "system_glue.h"
#include "kc_arch.h"
#if defined(KC_PLATFORM_WINDOWS)
#include <io.h>
#else
#include <sys/utsname.h>
#include <sys/statvfs.h>
#include <sys/ioctl.h>
#include <pwd.h>
#include <time.h>
#endif

/* --------------------------- identity strings --------------------------- */

static KcValue si_os(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; (void)a;
    sys_argc(n, 0, 0, "system.os() -> string", pos);
#if defined(_WIN32)
    return kc_string("windows");
#elif defined(__APPLE__)
    return kc_string("macos");
#elif defined(__linux__)
    return kc_string("linux");
#elif defined(__FreeBSD__)
    return kc_string("freebsd");
#else
    return kc_string("unix");
#endif
}

static const char *arch_name(void) { return KC_ARCH_NAME; }

static KcValue si_arch(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; (void)a;
    sys_argc(n, 0, 0, "system.arch() -> string", pos);
    return kc_string(arch_name());
}

static KcValue si_endian(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; (void)a;
    sys_argc(n, 0, 0, "system.endian() -> \"little\" | \"big\"", pos);
    uint16_t probe = 1;
    return kc_string(*(uint8_t *)&probe == 1 ? "little" : "big");
}

static void os_version_str(char *buf, size_t cap) {
#if defined(KC_PLATFORM_WINDOWS)
    typedef LONG (WINAPI *RtlGetVersionFn)(OSVERSIONINFOW *);
    HMODULE nt = GetModuleHandleA("ntdll.dll");
    RtlGetVersionFn fn = nt ? (RtlGetVersionFn)(void *)GetProcAddress(nt, "RtlGetVersion") : NULL;
    OSVERSIONINFOW vi; memset(&vi, 0, sizeof vi); vi.dwOSVersionInfoSize = sizeof vi;
    if (fn && fn(&vi) == 0) snprintf(buf, cap, "%lu.%lu.%lu", (unsigned long)vi.dwMajorVersion, (unsigned long)vi.dwMinorVersion, (unsigned long)vi.dwBuildNumber);
    else snprintf(buf, cap, "unknown");
#else
    struct utsname u;
    if (uname(&u) == 0) snprintf(buf, cap, "%s", u.release); else snprintf(buf, cap, "unknown");
#endif
}

static KcValue si_os_version(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; (void)a;
    sys_argc(n, 0, 0, "system.os_version() -> string", pos);
    char buf[128]; os_version_str(buf, sizeof buf);
    return kc_string(buf);
}

static bool get_hostname(char *buf, size_t cap) {
#if defined(KC_PLATFORM_WINDOWS)
    DWORD sz = (DWORD)cap;
    return GetComputerNameExA(ComputerNameDnsHostname, buf, &sz) || GetComputerNameA(buf, &sz);
#else
    if (gethostname(buf, cap) != 0) return false;
    buf[cap - 1] = 0; return true;
#endif
}

static KcValue si_hostname(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; (void)a;
    sys_argc(n, 0, 0, "system.hostname() -> string", pos);
    char buf[256];
    if (!get_hostname(buf, sizeof buf)) { sys_set_errno("system.hostname"); return kc_null(); }
    return kc_string(buf);
}

static KcValue si_username(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; (void)a;
    sys_argc(n, 0, 0, "system.username() -> string", pos);
#if defined(KC_PLATFORM_WINDOWS)
    char buf[257]; DWORD sz = sizeof buf;
    if (!GetUserNameA(buf, &sz)) { sys_set_errno("system.username"); return kc_null(); }
    return kc_string(buf);
#else
    struct passwd *pw = getpwuid(geteuid());
    if (pw && pw->pw_name) return kc_string(pw->pw_name);
    const char *e = getenv("USER"); if (!e) e = getenv("LOGNAME");
    if (e) return kc_string(e);
    sys_set_error("system.username: no passwd entry for uid %ld", (long)geteuid());
    return kc_null();
#endif
}

static KcValue si_home_dir(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; (void)a;
    sys_argc(n, 0, 0, "system.home_dir() -> string", pos);
#if defined(KC_PLATFORM_WINDOWS)
    const char *e = getenv("USERPROFILE");
    if (e && *e) return kc_string(e);
    const char *d = getenv("HOMEDRIVE"), *p = getenv("HOMEPATH");
    if (d && p) { char buf[1024]; snprintf(buf, sizeof buf, "%s%s", d, p); return kc_string(buf); }
#else
    const char *e = getenv("HOME");
    if (e && *e) return kc_string(e);
    struct passwd *pw = getpwuid(geteuid());
    if (pw && pw->pw_dir) return kc_string(pw->pw_dir);
#endif
    sys_set_error("system.home_dir: could not determine the home directory");
    return kc_null();
}

static KcValue si_temp_dir(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; (void)a;
    sys_argc(n, 0, 0, "system.temp_dir() -> string", pos);
#if defined(KC_PLATFORM_WINDOWS)
    char buf[MAX_PATH + 2]; DWORD len = GetTempPathA(sizeof buf, buf);
    if (len == 0 || len >= sizeof buf) { sys_set_errno("system.temp_dir"); return kc_null(); }
    while (len > 3 && (buf[len-1] == '\\' || buf[len-1] == '/')) buf[--len] = 0; /* keep "C:\" intact */
    return kc_string(buf);
#else
    const char *e = getenv("TMPDIR");
    return kc_string(e && *e ? e : "/tmp");
#endif
}

static KcValue si_exe_path(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; (void)a;
    sys_argc(n, 0, 0, "system.exe_path() -> string  (path of the running kingcobra binary)", pos);
#if defined(KC_PLATFORM_WINDOWS)
    char buf[MAX_PATH]; DWORD len = GetModuleFileNameA(NULL, buf, sizeof buf);
    if (len == 0 || len >= sizeof buf) { sys_set_errno("system.exe_path"); return kc_null(); }
    return kc_string(buf);
#elif defined(__linux__)
    char buf[4096]; ssize_t len = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (len < 0) { sys_set_errno("system.exe_path"); return kc_null(); }
    buf[len] = 0; return kc_string(buf);
#else
    sys_set_error("system.exe_path: not supported on this OS");
    return kc_null();
#endif
}

/* ------------------------------ resources ------------------------------ */

static int64_t cpu_count_now(void) {
#if defined(KC_PLATFORM_WINDOWS)
    SYSTEM_INFO si; GetSystemInfo(&si); return (int64_t)si.dwNumberOfProcessors;
#else
    long c = sysconf(_SC_NPROCESSORS_ONLN); return c > 0 ? c : 1;
#endif
}

static int64_t page_size_now(void) {
#if defined(KC_PLATFORM_WINDOWS)
    SYSTEM_INFO si; GetSystemInfo(&si); return (int64_t)si.dwPageSize;
#else
    long p = sysconf(_SC_PAGESIZE); return p > 0 ? p : 4096;
#endif
}

static KcValue si_cpu_count(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; (void)a;
    sys_argc(n, 0, 0, "system.cpu_count() -> int  (logical CPUs online)", pos);
    return kc_int(cpu_count_now());
}

static KcValue si_page_size(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; (void)a;
    sys_argc(n, 0, 0, "system.page_size() -> int", pos);
    return kc_int(page_size_now());
}

#if defined(__linux__)
/* Value (in bytes) of a "Key:   123 kB" line from /proc/meminfo, or -1. */
static int64_t meminfo_kb(const char *key) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return -1;
    char line[256]; size_t kl = strlen(key); int64_t out = -1;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, key, kl) == 0 && line[kl] == ':') { out = (int64_t)strtoll(line + kl + 1, NULL, 10) * 1024; break; }
    }
    fclose(f);
    return out;
}
#endif

static int64_t mem_total_now(void) {
#if defined(KC_PLATFORM_WINDOWS)
    MEMORYSTATUSEX m; m.dwLength = sizeof m;
    return GlobalMemoryStatusEx(&m) ? (int64_t)m.ullTotalPhys : -1;
#else
#if defined(__linux__)
    int64_t v = meminfo_kb("MemTotal"); if (v >= 0) return v;
#endif
    long pages = sysconf(_SC_PHYS_PAGES);
    return pages > 0 ? (int64_t)pages * page_size_now() : -1;
#endif
}

static int64_t mem_free_now(void) {
#if defined(KC_PLATFORM_WINDOWS)
    MEMORYSTATUSEX m; m.dwLength = sizeof m;
    return GlobalMemoryStatusEx(&m) ? (int64_t)m.ullAvailPhys : -1;
#else
#if defined(__linux__)
    int64_t v = meminfo_kb("MemAvailable"); if (v >= 0) return v; /* what a new program could really use */
#endif
    long pages = sysconf(_SC_AVPHYS_PAGES);
    return pages > 0 ? (int64_t)pages * page_size_now() : -1;
#endif
}

static KcValue si_mem_total(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; (void)a;
    sys_argc(n, 0, 0, "system.mem_total() -> int  (physical memory, bytes)", pos);
    int64_t v = mem_total_now();
    if (v < 0) { sys_set_error("system.mem_total: not available on this OS"); return kc_null(); }
    return kc_int(v);
}

static KcValue si_mem_free(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    SYS_IMPURE(ip, pos, "system.mem_free");
    sys_argc(n, 0, 0, "system.mem_free() -> int  (memory available to new programs, bytes)", pos);
    int64_t v = mem_free_now();
    if (v < 0) { sys_set_error("system.mem_free: not available on this OS"); return kc_null(); }
    return kc_int(v);
}

static KcValue si_uptime(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    SYS_IMPURE(ip, pos, "system.uptime");
    sys_argc(n, 0, 0, "system.uptime() -> float  (seconds since boot)", pos);
#if defined(KC_PLATFORM_WINDOWS)
    return kc_float((double)GetTickCount64() / 1000.0);
#elif defined(__linux__)
    FILE *f = fopen("/proc/uptime", "r"); double up = 0;
    if (f) { int ok = fscanf(f, "%lf", &up); fclose(f); if (ok == 1) return kc_float(up); }
    struct timespec ts;
    if (clock_gettime(CLOCK_BOOTTIME, &ts) == 0) return kc_float((double)ts.tv_sec + ts.tv_nsec / 1e9);
    sys_set_errno("system.uptime"); return kc_null();
#else
    sys_set_error("system.uptime: not supported on this OS");
    return kc_null();
#endif
}

static KcValue si_load_avg(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    SYS_IMPURE(ip, pos, "system.load_avg");
    (void)a;
    sys_argc(n, 0, 0, "system.load_avg() -> [1min, 5min, 15min]", pos);
#if defined(KC_PLATFORM_WINDOWS)
    sys_set_error("system.load_avg: Windows has no load average");
    return kc_null();
#else
    double l[3];
    if (getloadavg(l, 3) != 3) { sys_set_error("system.load_avg: not available on this OS"); return kc_null(); }
    KcValue out = kc_array_new();
    for (int i = 0; i < 3; i++) kc_array_push(out, kc_float(l[i]));
    return out;
#endif
}

static KcValue si_disk_usage(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    SYS_IMPURE(ip, pos, "system.disk_usage");
    const char *sig = "system.disk_usage(path: string = \".\") -> DiskUsage { total, free, used }  (bytes)";
    sys_argc(n, 0, 1, sig, pos);
    const char *path = n ? sys_str(a, 0, sig, pos) : ".";
    int64_t total, freeb;
#if defined(KC_PLATFORM_WINDOWS)
    ULARGE_INTEGER avail, tot, fr;
    if (!GetDiskFreeSpaceExA(path, &avail, &tot, &fr)) { sys_set_errno("system.disk_usage"); return kc_null(); }
    total = (int64_t)tot.QuadPart; freeb = (int64_t)avail.QuadPart;
#else
    struct statvfs sv;
    if (statvfs(path, &sv) != 0) { sys_set_errno("system.disk_usage"); return kc_null(); }
    total = (int64_t)sv.f_blocks * (int64_t)sv.f_frsize;
    freeb = (int64_t)sv.f_bavail * (int64_t)sv.f_frsize; /* space available to unprivileged users */
#endif
    static const char *names[] = { "total", "free", "used" };
    KcValue vals[3] = { kc_int(total), kc_int(freeb), kc_int(total - freeb) };
    return sys_struct("DiskUsage", 3, names, vals);
}

/* ------------------------------- identity ------------------------------- */

static KcValue si_is_admin(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; (void)a;
    sys_argc(n, 0, 0, "system.is_admin() -> bool  (root on Unix, elevated Administrator on Windows)", pos);
#if defined(KC_PLATFORM_WINDOWS)
    BOOL admin = FALSE; PSID grp = NULL;
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &grp)) {
        if (!CheckTokenMembership(NULL, grp, &admin)) admin = FALSE;
        FreeSid(grp);
    }
    return kc_bool(admin != FALSE);
#else
    return kc_bool(geteuid() == 0);
#endif
}

/* -------------------------------- terminal -------------------------------- */

static KcValue si_is_tty(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    const char *sig = "system.is_tty(fd: int = 1) -> bool  (0 = stdin, 1 = stdout, 2 = stderr)";
    sys_argc(n, 0, 1, sig, pos);
    int64_t fd = n ? sys_int(a, 0, sig, pos) : 1;
    if (fd < 0 || fd > 2) kc_error_at(pos, "system.is_tty: fd must be 0, 1 or 2");
#if defined(KC_PLATFORM_WINDOWS)
    return kc_bool(_isatty((int)fd) != 0);
#else
    return kc_bool(isatty((int)fd) != 0);
#endif
}

static KcValue si_term_size(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; (void)a;
    sys_argc(n, 0, 0, "system.term_size() -> [columns, rows]  (null if not a terminal)", pos);
    int cols = 0, rows = 0;
#if defined(KC_PLATFORM_WINDOWS)
    CONSOLE_SCREEN_BUFFER_INFO ci;
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(h, &ci)) {
        cols = ci.srWindow.Right - ci.srWindow.Left + 1; rows = ci.srWindow.Bottom - ci.srWindow.Top + 1;
    }
#else
    struct winsize ws;
    for (int fd = 1; fd >= 0 && !cols; fd--)
        if (ioctl(fd, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) { cols = ws.ws_col; rows = ws.ws_row; }
    if (!cols && isatty(2) && ioctl(2, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) { cols = ws.ws_col; rows = ws.ws_row; }
#endif
    if (cols <= 0) { sys_set_error("system.term_size: not attached to a terminal"); return kc_null(); }
    KcValue out = kc_array_new();
    kc_array_push(out, kc_int(cols)); kc_array_push(out, kc_int(rows));
    return out;
}

/* ---------------------------------- info() ---------------------------------- */

/* One struct with every stable fact, handy for a diagnostics banner:
 *   let i = system.info();  print(i.os, i.arch, i.cpus, i.hostname); */
static KcValue si_info(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; (void)a;
    sys_argc(n, 0, 0, "system.info() -> SystemInfo", pos);
    char host[256] = "", ver[128];
    if (!get_hostname(host, sizeof host)) host[0] = 0;
    os_version_str(ver, sizeof ver);
    KcValue osv = si_os(ip, NULL, 0, pos);
    int64_t mt = mem_total_now();
    static const char *names[] = { "os", "os_version", "arch", "hostname", "cpus", "mem_total", "page_size", "pointer_bits", "kingcobra" };
    KcValue vals[9] = {
        osv, kc_string(ver), kc_string(arch_name()), kc_string(host), kc_int(cpu_count_now()),
        mt >= 0 ? kc_int(mt) : kc_null(), kc_int(page_size_now()), kc_int((int64_t)sizeof(void *) * 8), kc_string(KC_VERSION),
    };
    return sys_struct("SystemInfo", 9, names, vals);
}

const SysEntry sys_info_entries[] = {
    {"os", si_os}, {"arch", si_arch}, {"endian", si_endian}, {"os_version", si_os_version},
    {"hostname", si_hostname}, {"username", si_username}, {"home_dir", si_home_dir},
    {"temp_dir", si_temp_dir}, {"exe_path", si_exe_path},
    {"cpu_count", si_cpu_count}, {"page_size", si_page_size},
    {"mem_total", si_mem_total}, {"mem_free", si_mem_free},
    {"uptime", si_uptime}, {"load_avg", si_load_avg}, {"disk_usage", si_disk_usage},
    {"is_admin", si_is_admin}, {"is_tty", si_is_tty}, {"term_size", si_term_size},
    {"info", si_info},
};
const int sys_info_count = (int)(sizeof sys_info_entries / sizeof sys_info_entries[0]);
