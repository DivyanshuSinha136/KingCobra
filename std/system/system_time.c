/* KingCobra std.system — time and randomness: wall clock, monotonic clock,
 * sleep, calendar breakdown/formatting, timezone offset, OS random bytes.
 *
 * Everything here that observes the clock or randomness is refused inside
 * comptime { } (a compile-time constant must not depend on when it ran).
 * The pure calendar functions — date(ts), format_time(ts, ...) with an
 * explicit timestamp, utc_offset(ts) — are allowed.
 *
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com>
 */
#include "system_glue.h"
#include <time.h>
#if !defined(KC_PLATFORM_WINDOWS)
#include <sys/time.h>
#endif

static double wall_seconds(void) {
#if defined(KC_PLATFORM_WINDOWS)
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime; /* 100ns since 1601 */
    return (double)(t - 116444736000000000ULL) / 1e7;
#else
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#endif
}

static KcValue st_time(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)a; SYS_IMPURE(ip, pos, "system.time");
    sys_argc(n, 0, 0, "system.time() -> float  (seconds since the Unix epoch)", pos);
    return kc_float(wall_seconds());
}

static KcValue st_time_ms(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)a; SYS_IMPURE(ip, pos, "system.time_ms");
    sys_argc(n, 0, 0, "system.time_ms() -> int  (milliseconds since the Unix epoch)", pos);
    return kc_int((int64_t)(wall_seconds() * 1000.0));
}

static KcValue st_monotonic(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)a; SYS_IMPURE(ip, pos, "system.monotonic");
    sys_argc(n, 0, 0, "system.monotonic() -> float  (seconds; never goes backwards — use it to time things)", pos);
    return kc_float(kc_monotonic_seconds());
}

/* nanosleep resumed across signal interruptions (the v0.1 usleep call could
 * return early on a signal, and was undefined for >= 1s on some systems). */
static KcValue st_sleep(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    SYS_IMPURE(ip, pos, "system.sleep");
    const char *sig = "system.sleep(seconds: float)";
    sys_argc(n, 1, 1, sig, pos);
    double secs = sys_num(a, 0, sig, pos);
    if (!(secs >= 0.0)) kc_error_at(pos, "system.sleep: duration must be >= 0 (got a negative number or NaN)");
    fflush(stdout);
#if defined(KC_PLATFORM_WINDOWS)
    double ms = secs * 1000.0;
    while (ms > 0) { DWORD chunk = ms > 1e9 ? 1000000000UL : (DWORD)(ms + 0.5); Sleep(chunk); ms -= chunk; if (chunk == 0) break; }
#else
    struct timespec req, rem;
    req.tv_sec = (time_t)secs; req.tv_nsec = (long)((secs - (double)req.tv_sec) * 1e9);
    while (nanosleep(&req, &rem) != 0 && errno == EINTR) req = rem;
#endif
    return kc_null();
}

/* ---------------------------- calendar helpers ---------------------------- */

static bool break_down(double ts, bool utc, struct tm *out) {
    if (!(ts > -62135596800.0 && ts < 253402300800.0)) return false; /* years 1..9999 */
    time_t t = (time_t)(ts < 0 ? ts - 0.0 : ts);
    if ((double)t > ts) t--; /* floor toward -infinity for negative fractional values */
#if defined(KC_PLATFORM_WINDOWS)
    return (utc ? gmtime_s(out, &t) : localtime_s(out, &t)) == 0;
#else
    return (utc ? gmtime_r(&t, out) : localtime_r(&t, out)) != NULL;
#endif
}

/* Days from 1970-01-01 to y-m-d (proleptic Gregorian). */
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

/* Seconds east of UTC at instant `ts` in the local zone (DST included). */
static int64_t offset_at(double ts) {
    struct tm lt;
    if (!break_down(ts, false, &lt)) return 0;
    int64_t local_as_utc = days_from_civil(lt.tm_year + 1900, (unsigned)lt.tm_mon + 1, (unsigned)lt.tm_mday) * 86400
                         + lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec;
    time_t t = (time_t)ts; if ((double)t > ts) t--;
    return local_as_utc - (int64_t)t;
}

static KcValue st_utc_offset(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    const char *sig = "system.utc_offset(timestamp: float = now) -> int  (seconds east of UTC, DST included)";
    sys_argc(n, 0, 1, sig, pos);
    double ts;
    if (n && a[0].type != VAL_NULL) ts = sys_num(a, 0, sig, pos);
    else { SYS_IMPURE(ip, pos, "system.utc_offset"); ts = wall_seconds(); }
    return kc_int(offset_at(ts));
}

static KcValue st_date(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    const char *sig = "system.date(timestamp: float = now, utc: bool = false) -> DateTime";
    sys_argc(n, 0, 2, sig, pos);
    double ts;
    if (!sys_is_nullish(a, n, 0)) ts = sys_num(a, 0, sig, pos);
    else { SYS_IMPURE(ip, pos, "system.date"); ts = wall_seconds(); }
    bool utc = sys_opt_bool(a, n, 1, false, sig, pos);
    struct tm tm;
    if (!break_down(ts, utc, &tm)) { sys_set_error("system.date: timestamp out of range"); return kc_null(); }
    static const char *names[] = { "year", "month", "day", "hour", "minute", "second", "weekday", "yearday", "is_dst", "utc_offset" };
    KcValue vals[10] = {
        kc_int(tm.tm_year + 1900), kc_int(tm.tm_mon + 1), kc_int(tm.tm_mday),
        kc_int(tm.tm_hour), kc_int(tm.tm_min), kc_int(tm.tm_sec),
        kc_int(tm.tm_wday),          /* 0 = Sunday */
        kc_int(tm.tm_yday + 1),      /* 1..366 */
        kc_bool(!utc && tm.tm_isdst > 0),
        kc_int(utc ? 0 : offset_at(ts)),
    };
    return sys_struct("DateTime", 10, names, vals);
}

static KcValue st_format_time(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    const char *sig = "system.format_time(timestamp: float, format: string = \"%Y-%m-%d %H:%M:%S\", utc: bool = false) -> string";
    sys_argc(n, 1, 3, sig, pos);
    double ts = sys_num(a, 0, sig, pos);
    const char *fmt = sys_is_nullish(a, n, 1) ? "%Y-%m-%d %H:%M:%S" : sys_str(a, 1, sig, pos);
    bool utc = sys_opt_bool(a, n, 2, false, sig, pos);
    struct tm tm;
    if (!break_down(ts, utc, &tm)) { sys_set_error("system.format_time: timestamp out of range"); return kc_null(); }
    if (!*fmt) return kc_string("");
    char buf[1024];
    size_t len = strftime(buf, sizeof buf, fmt, &tm);
    if (len == 0) { sys_set_error("system.format_time: format produced no output or exceeded %zu bytes", sizeof buf - 1); return kc_null(); }
    return kc_string(buf);
}

/* ------------------------------- randomness ------------------------------- */

#if defined(KC_PLATFORM_WINDOWS)
typedef BOOLEAN (WINAPI *RtlGenRandomFn)(PVOID, ULONG);
#endif

static bool os_random(uint8_t *buf, size_t len) {
#if defined(KC_PLATFORM_WINDOWS)
    static RtlGenRandomFn fn; /* advapi32!SystemFunction036 == RtlGenRandom */
    if (!fn) { HMODULE m = LoadLibraryA("advapi32.dll"); if (m) fn = (RtlGenRandomFn)(void *)GetProcAddress(m, "SystemFunction036"); }
    if (!fn) { sys_set_error("system.random: RtlGenRandom unavailable"); return false; }
    while (len) { ULONG c = len > 0x10000000u ? 0x10000000u : (ULONG)len; if (!fn(buf, c)) { sys_set_error("system.random: RtlGenRandom failed"); return false; } buf += c; len -= c; }
    return true;
#else
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f) { sys_set_errno("system.random: /dev/urandom"); return false; }
    size_t got = fread(buf, 1, len, f); fclose(f);
    if (got != len) { sys_set_error("system.random: short read from /dev/urandom"); return false; }
    return true;
#endif
}

#define MAX_RANDOM_BYTES (1 << 20)

static KcValue st_random_bytes(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    SYS_IMPURE(ip, pos, "system.random_bytes");
    const char *sig = "system.random_bytes(count: int) -> array of int 0..255  (OS cryptographic randomness)";
    sys_argc(n, 1, 1, sig, pos);
    int64_t c = sys_int(a, 0, sig, pos);
    if (c < 0 || c > MAX_RANDOM_BYTES) kc_error_at(pos, "system.random_bytes: count must be 0..%d", MAX_RANDOM_BYTES);
    uint8_t *buf = kc_malloc((size_t)c + 1);
    if (!os_random(buf, (size_t)c)) { free(buf); return kc_null(); }
    KcValue out = kc_array_new();
    for (int64_t i = 0; i < c; i++) kc_array_push(out, kc_int(buf[i]));
    free(buf);
    return out;
}

static KcValue st_random_hex(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    SYS_IMPURE(ip, pos, "system.random_hex");
    const char *sig = "system.random_hex(bytes: int) -> string  (2*bytes hex digits, OS cryptographic randomness)";
    sys_argc(n, 1, 1, sig, pos);
    int64_t c = sys_int(a, 0, sig, pos);
    if (c < 0 || c > MAX_RANDOM_BYTES) kc_error_at(pos, "system.random_hex: bytes must be 0..%d", MAX_RANDOM_BYTES);
    uint8_t *buf = kc_malloc((size_t)c + 1);
    if (!os_random(buf, (size_t)c)) { free(buf); return kc_null(); }
    char *hex = kc_malloc((size_t)c * 2 + 1);
    static const char D[] = "0123456789abcdef";
    for (int64_t i = 0; i < c; i++) { hex[2*i] = D[buf[i] >> 4]; hex[2*i+1] = D[buf[i] & 15]; }
    free(buf);
    return sys_bytes_str(hex, (size_t)c * 2);
}

/* Uniform integer in [lo, hi] (inclusive) — rejection sampling, so no modulo bias. */
static KcValue st_random_int(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    SYS_IMPURE(ip, pos, "system.random_int");
    const char *sig = "system.random_int(lo: int, hi: int) -> int  (inclusive; OS cryptographic randomness)";
    sys_argc(n, 2, 2, sig, pos);
    int64_t lo = sys_int(a, 0, sig, pos), hi = sys_int(a, 1, sig, pos);
    if (lo > hi) kc_error_at(pos, "system.random_int: lo (%lld) is greater than hi (%lld)", (long long)lo, (long long)hi);
    uint64_t range = (uint64_t)hi - (uint64_t)lo + 1; /* 0 means the full 2^64 span */
    uint64_t r;
    if (range == 0) { if (!os_random((uint8_t *)&r, 8)) return kc_null(); return kc_int((int64_t)r); }
    uint64_t limit = UINT64_MAX - (UINT64_MAX - range + 1) % range; /* largest accepted value */
    do { if (!os_random((uint8_t *)&r, 8)) return kc_null(); } while (r > limit);
    return kc_int((int64_t)((uint64_t)lo + r % range));
}

const SysEntry sys_time_entries[] = {
    {"time", st_time}, {"time_ms", st_time_ms}, {"monotonic", st_monotonic}, {"sleep", st_sleep},
    {"utc_offset", st_utc_offset}, {"date", st_date}, {"format_time", st_format_time},
    {"random_bytes", st_random_bytes}, {"random_hex", st_random_hex}, {"random_int", st_random_int},
};
const int sys_time_count = (int)(sizeof sys_time_entries / sizeof sys_time_entries[0]);
