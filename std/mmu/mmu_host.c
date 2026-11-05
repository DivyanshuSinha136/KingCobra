/* KingCobra std.mmu — real host virtual memory.
 *
 * Where the rest of std.mmu simulates an MMU, this part drives the real one: anonymous mappings
 * with per-page protection, guard pages, huge-page and locking advice, residency, and — on Linux —
 * /proc/self/pagemap and /proc/self/maps introspection.
 *
 * Safety model: a script never gets a raw pointer. Every access is bounds-checked against the
 * region *and* against the protection the module itself tracks, so touching a PROT_NONE page is a
 * clean error return rather than a SIGSEGV. Requesting execute permission needs an `unsafe { }`
 * block, and writable+executable is refused outright (map writable, write the code, then
 * host_protect to "r,x" — the W^X discipline).
 *
 * Linux and Windows are implemented; elsewhere (and for Linux-only features on Windows) the calls
 * return null with mmu.last_error() explaining why, instead of pretending.
 *
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com>
 */
#include "kc_arch.h"
#include "mmu_internal.h"
#include "kc_hw.h"
#include <ctype.h>
#if defined(KC_HW_POSIX)
#include <sys/resource.h>
#endif

#define HOST_MAX_REGION ((size_t)1 << 30)

typedef struct {
    uint8_t *base; size_t len;
    void *map_base; size_t map_len;
    size_t page, npages;
    uint8_t *prot;               /* PROT_R/W/X per page */
    bool guard;
    uint64_t reads, writes;
} HostRegion;

#define OI(o, k, v) mm_oset(o, k, kc_int((int64_t)(v)))
#define OB(o, k, v) mm_oset(o, k, kc_bool(v))
#define OS(o, k, v) mm_oset(o, k, kc_string(v))

static size_t host_page(void) {
#if defined(KC_HW_WINDOWS)
    SYSTEM_INFO si; GetSystemInfo(&si); return (size_t)si.dwPageSize;
#else
    long p = sysconf(_SC_PAGESIZE); return p > 0 ? (size_t)p : 4096;
#endif
}

static void forbid(KcInterp *ip, KcPos pos, const char *who) { kc_forbid_in_comptime(ip, pos, who); }

static HostRegion *need_host(KcValue *a, int n, int i, KcPos pos, const char *who) {
    return mm_need_handle(a, n, i, MH_HOST, pos, who);
}

#if defined(KC_HW_WINDOWS)
static DWORD win_prot(uint32_t p) {
    bool r = p & PROT_R, w = p & PROT_W, x = p & PROT_X;
    if (x) return w ? PAGE_EXECUTE_READWRITE : r ? PAGE_EXECUTE_READ : PAGE_EXECUTE;
    if (w) return PAGE_READWRITE;
    if (r) return PAGE_READONLY;
    return PAGE_NOACCESS;
}
#else
static int posix_prot(uint32_t p) { return ((p & PROT_R) ? PROT_READ : 0) | ((p & PROT_W) ? PROT_WRITE : 0) | ((p & PROT_X) ? PROT_EXEC : 0); }
#endif

static void need_unsafe_exec(KcInterp *ip, KcPos pos, const char *who, uint32_t prot) {
    if ((prot & PROT_X) && ip->in_unsafe <= 0) kc_error_at(pos, "%s: executable memory requires an `unsafe { }` block", who);
    if ((prot & PROT_X) && (prot & PROT_W)) kc_error_at(pos, "%s: writable+executable (W^X) mappings are refused; map \"r,w\", fill it, then host_protect(r, 0, len, \"r,x\")", who);
}

static void os_error(const char *what) {
#if defined(KC_HW_WINDOWS)
    mm_set_error("%s failed (Windows error %lu)", what, (unsigned long)GetLastError());
#else
    mm_set_error("%s failed: %s", what, strerror(errno));
#endif
}

static bool os_protect(void *addr, size_t len, uint32_t prot) {
#if defined(KC_HW_WINDOWS)
    DWORD old; return VirtualProtect(addr, len, win_prot(prot), &old) != 0;
#else
    return mprotect(addr, len, posix_prot(prot)) == 0;
#endif
}

/* ------------------------------------------------------------------ alloc / free */

static KcValue f_host_alloc(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    MM_ENTER(); const char *who = "mmu.host_alloc"; forbid(ip, pos, who);
    mm_need(n, 1, 2, pos, "mmu.host_alloc(len [, flags]) -> host region | null   (flags: r w x none guard huge lock populate)");
    uint64_t want = mm_arg_size(a, n, 0, pos, who);
    MFlags F; mm_parse_flags(n == 2 ? mm_arg_str(a, n, 1, pos, who) : "r,w", &F, pos, who);
    if (F.rel) MM_ERR("%s: +/- flags are not valid here", who);
    uint32_t prot = F.none ? 0 : (F.prot ? F.prot : (PROT_R | PROT_W));
    prot &= (PROT_R | PROT_W | PROT_X);
    need_unsafe_exec(ip, pos, who, prot);
    size_t pg = host_page();
    if (want == 0 || want > HOST_MAX_REGION) MM_ERR("%s: length must be 1 byte .. 1 GiB", who);
    size_t len = ((size_t)want + pg - 1) / pg * pg;
    size_t total = len + (F.guard ? 2 * pg : 0);
    void *m;
#if defined(KC_HW_WINDOWS)
    m = VirtualAlloc(NULL, total, MEM_COMMIT | MEM_RESERVE, F.guard ? PAGE_NOACCESS : win_prot(prot));
    if (!m) { os_error("VirtualAlloc"); return kc_null(); }
    if (F.guard && !os_protect((uint8_t *)m + pg, len, prot)) { os_error("VirtualProtect"); VirtualFree(m, 0, MEM_RELEASE); return kc_null(); }
#else
    int fl = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef MAP_POPULATE
    if (F.populate && !F.guard && prot) fl |= MAP_POPULATE;
#endif
    m = mmap(NULL, total, F.guard ? PROT_NONE : posix_prot(prot), fl, -1, 0);
    if (m == MAP_FAILED) { os_error("mmap"); return kc_null(); }
    if (F.guard && !os_protect((uint8_t *)m + pg, len, prot)) { os_error("mprotect"); munmap(m, total); return kc_null(); }
#ifdef MADV_HUGEPAGE
    if (F.huge) madvise((uint8_t *)m + (F.guard ? pg : 0), len, MADV_HUGEPAGE);        /* advisory: failure is not an error */
#endif
#endif
    HostRegion *r = kc_malloc(sizeof *r); memset(r, 0, sizeof *r);
    r->map_base = m; r->map_len = total; r->base = (uint8_t *)m + (F.guard ? pg : 0); r->len = len; r->page = pg; r->npages = len / pg; r->guard = F.guard;
    r->prot = kc_malloc(r->npages); memset(r->prot, (int)prot, r->npages);
    if (F.locked) {
#if defined(KC_HW_WINDOWS)
        if (!VirtualLock(r->base, len)) { os_error("VirtualLock"); }
#else
        if (mlock(r->base, len) != 0) { os_error("mlock"); }
#endif
    }
    KcValue v = mmuh_wrap(MH_HOST, r);
    v.as.handle->size = len;
    return v;
}

bool mmu_host_destroy(KcHandle *h) {
    HostRegion *r = h->ptr;
    if (!r) return false;
#if defined(KC_HW_WINDOWS)
    VirtualFree(r->map_base, 0, MEM_RELEASE);
#else
    munmap(r->map_base, r->map_len);
#endif
    free(r->prot); free(r); h->ptr = NULL;
    return true;
}

static KcValue f_host_free(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    MM_ENTER(); const char *who = "mmu.host_free"; forbid(ip, pos, who); mm_need(n, 1, 1, pos, "mmu.host_free(region) -> bool");
    need_host(a, n, 0, pos, who);
    return kc_bool(mmu_host_destroy(a[0].as.handle));
}

/* ------------------------------------------------------------------ range helpers */

static bool range_ok(HostRegion *r, uint64_t off, uint64_t len, uint32_t need, const char *what) {
    if (len > r->len || off > r->len - len) { mm_set_error("%s: range [%llu, +%llu) is outside the %zu-byte region", what, (unsigned long long)off, (unsigned long long)len, r->len); return false; }
    if (!need || !len) return true;
    for (uint64_t p = off / r->page; p <= (off + len - 1) / r->page; p++)
        if ((r->prot[p] & need) != need) { mm_set_error("%s: access violation at offset %llu (page protection \"%s%s%s\")", what, (unsigned long long)(p * r->page), (r->prot[p] & PROT_R) ? "r" : "", (r->prot[p] & PROT_W) ? "w" : "", (r->prot[p] & PROT_X) ? "x" : ""); return false; }
    return true;
}

static void page_range(HostRegion *r, KcValue *a, int n, int i, KcPos pos, const char *who, uint64_t *off, uint64_t *len) {
    *off = (uint64_t)mm_arg_int(a, n, i, pos, who); *len = mm_arg_size(a, n, i + 1, pos, who);
    if ((int64_t)*off < 0 || (*off % r->page)) MM_ERR("%s: offset must be a multiple of the page size (%zu)", who, r->page);
    *len = (*len + r->page - 1) / r->page * r->page;
    if (*len == 0 || *len > r->len || *off > r->len - *len) MM_ERR("%s: range is outside the region (%zu bytes)", who, r->len);
}

/* ------------------------------------------------------------------ protect / advise / lock */

static KcValue f_host_protect(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    MM_ENTER(); const char *who = "mmu.host_protect"; forbid(ip, pos, who);
    mm_need(n, 4, 4, pos, "mmu.host_protect(region, offset, len, flags) -> bool   (flags: r,w,x or \"none\")");
    HostRegion *r = need_host(a, n, 0, pos, who); uint64_t off, len; page_range(r, a, n, 1, pos, who, &off, &len);
    MFlags F; mm_parse_flags(mm_arg_str(a, n, 3, pos, who), &F, pos, who);
    if (F.rel) MM_ERR("%s: pass absolute flags such as \"r,w\"", who);
    uint32_t prot = F.none ? 0 : F.prot; prot &= (PROT_R | PROT_W | PROT_X);
    if (!prot && !F.none) MM_ERR("%s: flags need r, w, x or \"none\"", who);
    need_unsafe_exec(ip, pos, who, prot);
    if (!os_protect(r->base + off, (size_t)len, prot)) { os_error("mprotect"); return kc_bool(false); }
    memset(r->prot + off / r->page, (int)prot, (size_t)(len / r->page));
    return kc_bool(true);
}

static KcValue f_host_advise(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    MM_ENTER(); const char *who = "mmu.host_advise"; forbid(ip, pos, who);
    mm_need(n, 4, 4, pos, "mmu.host_advise(region, offset, len, advice) -> bool   (dontneed, free, willneed, hugepage, nohugepage, normal, sequential, random)");
    HostRegion *r = need_host(a, n, 0, pos, who); uint64_t off, len; page_range(r, a, n, 1, pos, who, &off, &len);
    const char *adv = mm_arg_str(a, n, 3, pos, who);
    void *p = r->base + off;
#if defined(KC_HW_WINDOWS)
    if (!strcmp(adv, "dontneed") || !strcmp(adv, "free")) {
        /* decommit + recommit with the tracked protections: contents become zero, like MADV_DONTNEED */
        if (!VirtualFree(p, (size_t)len, MEM_DECOMMIT)) { os_error("VirtualFree(MEM_DECOMMIT)"); return kc_bool(false); }
        for (uint64_t i = 0; i < len / r->page; i++) {
            if (!VirtualAlloc((uint8_t *)p + i * r->page, r->page, MEM_COMMIT, win_prot(r->prot[off / r->page + i]))) { os_error("VirtualAlloc(MEM_COMMIT)"); return kc_bool(false); }
        }
        return kc_bool(true);
    }
    if (!strcmp(adv, "willneed") || !strcmp(adv, "normal") || !strcmp(adv, "sequential") || !strcmp(adv, "random") || !strcmp(adv, "hugepage") || !strcmp(adv, "nohugepage")) return kc_bool(true);
#else
    int m = -1;
    if (!strcmp(adv, "dontneed")) m = MADV_DONTNEED;
    else if (!strcmp(adv, "free")) {
#ifdef MADV_FREE
        m = MADV_FREE;
#else
        m = MADV_DONTNEED;
#endif
    }
    else if (!strcmp(adv, "willneed")) m = MADV_WILLNEED;
    else if (!strcmp(adv, "normal")) m = MADV_NORMAL;
    else if (!strcmp(adv, "sequential")) m = MADV_SEQUENTIAL;
    else if (!strcmp(adv, "random")) m = MADV_RANDOM;
#ifdef MADV_HUGEPAGE
    else if (!strcmp(adv, "hugepage")) m = MADV_HUGEPAGE;
    else if (!strcmp(adv, "nohugepage")) m = MADV_NOHUGEPAGE;
#else
    else if (!strcmp(adv, "hugepage") || !strcmp(adv, "nohugepage")) { mm_set_error("transparent huge pages are not available on this platform"); return kc_bool(false); }
#endif
    if (m >= 0) { if (madvise(p, (size_t)len, m) != 0) { os_error("madvise"); return kc_bool(false); } return kc_bool(true); }
#endif
    MM_ERR("%s: unknown advice '%s'", who, adv);
    return kc_null();
}

static KcValue lock_op(KcInterp *ip, KcValue *a, int n, KcPos pos, const char *who, bool lock) {
    MM_ENTER(); forbid(ip, pos, who); mm_need(n, 3, 3, pos, lock ? "mmu.host_lock(region, offset, len) -> bool" : "mmu.host_unlock(region, offset, len) -> bool");
    HostRegion *r = need_host(a, n, 0, pos, who); uint64_t off, len; page_range(r, a, n, 1, pos, who, &off, &len);
#if defined(KC_HW_WINDOWS)
    BOOL ok = lock ? VirtualLock(r->base + off, (size_t)len) : VirtualUnlock(r->base + off, (size_t)len);
    if (!ok) { os_error(lock ? "VirtualLock" : "VirtualUnlock"); return kc_bool(false); }
#else
    int rc = lock ? mlock(r->base + off, (size_t)len) : munlock(r->base + off, (size_t)len);
    if (rc != 0) { os_error(lock ? "mlock" : "munlock"); return kc_bool(false); }
#endif
    return kc_bool(true);
}
static KcValue f_host_lock(KcInterp *ip, KcValue *a, int n, KcPos pos) { return lock_op(ip, a, n, pos, "mmu.host_lock", true); }
static KcValue f_host_unlock(KcInterp *ip, KcValue *a, int n, KcPos pos) { return lock_op(ip, a, n, pos, "mmu.host_unlock", false); }

/* ------------------------------------------------------------------ data access */

static KcValue host_read_w(KcInterp *ip, KcValue *a, int n, KcPos pos, const char *who, int w) {
    MM_ENTER(); forbid(ip, pos, who);
    HostRegion *r = need_host(a, n, 0, pos, who); uint64_t off = (uint64_t)mm_arg_int(a, n, 1, pos, who);
    if (!range_ok(r, off, (uint64_t)w, PROT_R, who)) return kc_null();
    uint64_t v = 0; memcpy(&v, r->base + off, (size_t)w);          /* little-endian hosts only: all supported targets */
    r->reads++;
    return kc_int((int64_t)v);
}
static KcValue host_write_w(KcInterp *ip, KcValue *a, int n, KcPos pos, const char *who, int w) {
    MM_ENTER(); forbid(ip, pos, who);
    HostRegion *r = need_host(a, n, 0, pos, who); uint64_t off = (uint64_t)mm_arg_int(a, n, 1, pos, who); int64_t val = mm_arg_int(a, n, 2, pos, who);
    if (w < 8) { int bits = w * 8; int64_t umax = ((int64_t)1 << bits) - 1, smin = -((int64_t)1 << (bits - 1)); if (val < smin || val > umax) MM_ERR("%s: value %lld does not fit in %d bits", who, (long long)val, bits); }
    if (!range_ok(r, off, (uint64_t)w, PROT_W, who)) return kc_bool(false);
    memcpy(r->base + off, &val, (size_t)w);
    r->writes++;
    return kc_bool(true);
}
#define HR(NAME, W) static KcValue f_host_read##NAME(KcInterp *ip, KcValue *a, int n, KcPos pos) { mm_need(n, 2, 2, pos, "mmu.host_read" #NAME "(region, offset) -> int | null"); return host_read_w(ip, a, n, pos, "mmu.host_read" #NAME, W); }
#define HW(NAME, W) static KcValue f_host_write##NAME(KcInterp *ip, KcValue *a, int n, KcPos pos) { mm_need(n, 3, 3, pos, "mmu.host_write" #NAME "(region, offset, value) -> bool"); return host_write_w(ip, a, n, pos, "mmu.host_write" #NAME, W); }
HR(8, 1) HR(16, 2) HR(32, 4) HR(64, 8)
HW(8, 1) HW(16, 2) HW(32, 4) HW(64, 8)

static KcValue f_host_read_bytes(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    MM_ENTER(); const char *who = "mmu.host_read_bytes"; forbid(ip, pos, who); mm_need(n, 3, 3, pos, "mmu.host_read_bytes(region, offset, len) -> string | null");
    HostRegion *r = need_host(a, n, 0, pos, who); uint64_t off = (uint64_t)mm_arg_int(a, n, 1, pos, who), len = (uint64_t)mm_arg_int(a, n, 2, pos, who);
    if ((int64_t)len < 0) MM_ERR("%s: negative length", who);
    if (!range_ok(r, off, len, PROT_R, who)) return kc_null();
    r->reads++;
    return mm_str_bytes(r->base + off, (size_t)len);
}
static KcValue f_host_write_bytes(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    MM_ENTER(); const char *who = "mmu.host_write_bytes"; forbid(ip, pos, who); mm_need(n, 3, 3, pos, "mmu.host_write_bytes(region, offset, string) -> bool");
    HostRegion *r = need_host(a, n, 0, pos, who); uint64_t off = (uint64_t)mm_arg_int(a, n, 1, pos, who);
    if (a[2].type != VAL_STRING) MM_ERR("%s: data must be a string", who);
    if (!range_ok(r, off, (uint64_t)a[2].as.s->len, PROT_W, who)) return kc_bool(false);
    memcpy(r->base + off, a[2].as.s->data, (size_t)a[2].as.s->len); r->writes++;
    return kc_bool(true);
}
static KcValue f_host_fill(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    MM_ENTER(); const char *who = "mmu.host_fill"; forbid(ip, pos, who); mm_need(n, 4, 4, pos, "mmu.host_fill(region, offset, len, byte) -> bool");
    HostRegion *r = need_host(a, n, 0, pos, who); uint64_t off = (uint64_t)mm_arg_int(a, n, 1, pos, who), len = (uint64_t)mm_arg_int(a, n, 2, pos, who); int64_t b = mm_arg_int(a, n, 3, pos, who);
    if ((int64_t)len < 0 || b < -128 || b > 255) MM_ERR("%s: invalid length or byte", who);
    if (!range_ok(r, off, len, PROT_W, who)) return kc_bool(false);
    memset(r->base + off, (int)(b & 0xFF), (size_t)len); r->writes++;
    return kc_bool(true);
}
static KcValue f_host_copy(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    MM_ENTER(); const char *who = "mmu.host_copy"; forbid(ip, pos, who); mm_need(n, 5, 5, pos, "mmu.host_copy(dst, dst_off, src, src_off, len) -> bool");
    HostRegion *d = need_host(a, n, 0, pos, who), *s = need_host(a, n, 2, pos, who);
    uint64_t doff = (uint64_t)mm_arg_int(a, n, 1, pos, who), soff = (uint64_t)mm_arg_int(a, n, 3, pos, who), len = (uint64_t)mm_arg_int(a, n, 4, pos, who);
    if ((int64_t)len < 0) MM_ERR("%s: negative length", who);
    if (!range_ok(s, soff, len, PROT_R, who) || !range_ok(d, doff, len, PROT_W, who)) return kc_bool(false);
    memmove(d->base + doff, s->base + soff, (size_t)len);
    return kc_bool(true);
}

/* ------------------------------------------------------------------ introspection */

static KcValue f_host_info(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; (void)a; mm_need(n, 0, 0, pos, "mmu.host_info() -> object");
    KcValue o = kc_object_new();
    size_t pg = host_page();
    OI(o, "page_size", pg); OI(o, "pointer_bits", sizeof(void *) * 8);
#if defined(KC_HW_WINDOWS)
    SYSTEM_INFO si; GetSystemInfo(&si);
    OS(o, "os", "windows"); OI(o, "alloc_granularity", si.dwAllocationGranularity);
    OI(o, "huge_page_size", GetLargePageMinimum());
    OI(o, "min_address", (uint64_t)(uintptr_t)si.lpMinimumApplicationAddress); OI(o, "max_address", (uint64_t)(uintptr_t)si.lpMaximumApplicationAddress);
    OB(o, "pagemap", false); OB(o, "mincore", false);
#else
#if defined(__linux__)
    OS(o, "os", "linux");
    uint64_t huge = 0; FILE *f = fopen("/proc/meminfo", "r");
    if (f) { char l[160]; while (fgets(l, sizeof l, f)) { unsigned long long kb; if (sscanf(l, "Hugepagesize: %llu kB", &kb) == 1) { huge = kb * 1024; break; } } fclose(f); }
    OI(o, "huge_page_size", huge);
    OB(o, "pagemap", access("/proc/self/pagemap", R_OK) == 0); OB(o, "mincore", true);
#else
    OS(o, "os", "posix"); OI(o, "huge_page_size", 0); OB(o, "pagemap", false); OB(o, "mincore", true);
#endif
    OI(o, "alloc_granularity", pg);
#endif
#if defined(KC_ARCH_X86_64)
    OS(o, "arch", "x86_64");
#elif defined(KC_ARCH_X86)
    OS(o, "arch", "x86");
#elif defined(KC_ARCH_ARM64)
    OS(o, "arch", "arm64");
#elif defined(KC_ARCH_ARM32)
    OS(o, "arch", "arm");
#elif defined(KC_ARCH_RISCV64) || defined(KC_ARCH_RISCV32)
    OS(o, "arch", "riscv");
#else
    OS(o, "arch", "unknown");
#endif
    return o;
}

static KcValue f_host_address(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    MM_ENTER(); const char *who = "mmu.host_address"; forbid(ip, pos, who); mm_need(n, 1, 1, pos, "mmu.host_address(region) -> int (the region's real virtual address; informational only)");
    return kc_int((int64_t)(uintptr_t)need_host(a, n, 0, pos, who)->base);
}

static KcValue f_host_pages(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    MM_ENTER(); const char *who = "mmu.host_pages"; forbid(ip, pos, who); mm_need(n, 1, 1, pos, "mmu.host_pages(region) -> [ { offset, len, flags } ] (runs of equal protection)");
    HostRegion *r = need_host(a, n, 0, pos, who);
    KcValue arr = kc_array_new();
    size_t i = 0;
    while (i < r->npages) {
        size_t j = i; while (j < r->npages && r->prot[j] == r->prot[i]) j++;
        KcValue o = kc_object_new(); char fl[8]; snprintf(fl, sizeof fl, "%s%s%s", (r->prot[i] & PROT_R) ? "r" : "", (r->prot[i] & PROT_W) ? "w" : "", (r->prot[i] & PROT_X) ? "x" : "");
        if (!fl[0]) snprintf(fl, sizeof fl, "none");
        OI(o, "offset", i * r->page); OI(o, "len", (j - i) * r->page); OS(o, "flags", fl);
        kc_array_push(arr, o); i = j;
    }
    return arr;
}

/* residency string: one '0'/'1' per page */
static bool residency(HostRegion *r, uint64_t off, uint64_t len, unsigned char **vec_out, size_t *np) {
#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)
    size_t pages = (size_t)(len / r->page);
    unsigned char *vec = kc_malloc(pages ? pages : 1);
    if (mincore(r->base + off, (size_t)len, (void *)vec) != 0) { os_error("mincore"); free(vec); return false; }
    *vec_out = vec; *np = pages; return true;
#else
    (void)r; (void)off; (void)len; (void)vec_out; (void)np;
    mm_set_error("residency queries are not implemented on this platform");
    return false;
#endif
}

static KcValue f_host_resident(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    MM_ENTER(); const char *who = "mmu.host_resident"; forbid(ip, pos, who); mm_need(n, 1, 3, pos, "mmu.host_resident(region [, offset, len]) -> number of resident pages | null");
    HostRegion *r = need_host(a, n, 0, pos, who); uint64_t off = 0, len = r->len;
    if (n == 3) page_range(r, a, n, 1, pos, who, &off, &len); else if (n == 2) MM_ERR("%s: give both offset and len, or neither", who);
    unsigned char *v; size_t np; if (!residency(r, off, len, &v, &np)) return kc_null();
    int64_t c = 0; for (size_t i = 0; i < np; i++) c += v[i] & 1;
    free(v); return kc_int(c);
}
static KcValue f_host_residency(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    MM_ENTER(); const char *who = "mmu.host_residency"; forbid(ip, pos, who); mm_need(n, 1, 1, pos, "mmu.host_residency(region) -> string of '1' (resident) / '0' per page | null");
    HostRegion *r = need_host(a, n, 0, pos, who);
    unsigned char *v; size_t np; if (!residency(r, 0, r->len, &v, &np)) return kc_null();
    char *s = kc_malloc(np + 1); for (size_t i = 0; i < np; i++) s[i] = (v[i] & 1) ? '1' : '0'; s[np] = 0; free(v);
    return kc_string_take(s, (int)np);
}

static KcValue f_host_translate(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    MM_ENTER(); const char *who = "mmu.host_translate"; forbid(ip, pos, who);
    mm_need(n, 2, 3, pos, "mmu.host_translate(region, offset [, touch]) -> { present, swapped, pfn, ... } | null   (Linux /proc/self/pagemap)");
    HostRegion *r = need_host(a, n, 0, pos, who); uint64_t off = (uint64_t)mm_arg_int(a, n, 1, pos, who);
    if (off >= r->len) MM_ERR("%s: offset is outside the region", who);
#if defined(__linux__)
    if (n == 3 && kc_truthy(a[2]) && (r->prot[off / r->page] & PROT_R)) { volatile uint8_t t = r->base[off]; (void)t; }
    int fd = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
    if (fd < 0) { os_error("open /proc/self/pagemap"); return kc_null(); }
    uint64_t va = (uint64_t)(uintptr_t)(r->base + off), e = 0;
    ssize_t got = pread(fd, &e, sizeof e, (off_t)((va / r->page) * 8));
    close(fd);
    if (got != (ssize_t)sizeof e) { os_error("read pagemap"); return kc_null(); }
    KcValue o = kc_object_new();
    bool present = (e >> 63) & 1; uint64_t pfn = present ? (e & (((uint64_t)1 << 55) - 1)) : 0;
    OI(o, "vaddr", va & ~(uint64_t)(r->page - 1)); OB(o, "present", present); OB(o, "swapped", (e >> 62) & 1);
    OB(o, "file_or_shared", (e >> 61) & 1); OB(o, "exclusive", (e >> 56) & 1); OB(o, "soft_dirty", (e >> 55) & 1);
    OI(o, "pfn", pfn); OB(o, "pfn_visible", pfn != 0);
    OI(o, "phys", pfn * r->page);
    return o;
#else
    mm_set_error("page-table introspection of the host needs Linux (/proc/self/pagemap)");
    return kc_null();
#endif
}

static KcValue f_host_maps(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    MM_ENTER(); const char *who = "mmu.host_maps"; forbid(ip, pos, who); mm_need(n, 0, 0, pos, "mmu.host_maps() -> [ { start, end, perms, ... } ]  (the process's real memory map)");
    (void)a;
    KcValue arr = kc_array_new();
#if defined(__linux__)
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) { os_error("open /proc/self/maps"); return kc_null(); }
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        unsigned long long s, e, off, inode; char perms[8], dev[16]; int used = 0;
        if (sscanf(line, "%llx-%llx %7s %llx %15s %llu %n", &s, &e, perms, &off, dev, &inode, &used) < 6) continue;
        char *path = line + used; size_t pl = strlen(path); while (pl && (path[pl - 1] == '\n' || path[pl - 1] == ' ')) path[--pl] = 0;
        KcValue o = kc_object_new();
        OI(o, "start", s); OI(o, "end", e); OI(o, "size", e - s); OS(o, "perms", perms); OI(o, "offset", off); OS(o, "dev", dev); OI(o, "inode", inode); OS(o, "path", path);
        kc_array_push(arr, o);
    }
    fclose(f);
#elif defined(KC_HW_WINDOWS)
    SYSTEM_INFO si; GetSystemInfo(&si);
    uint8_t *p = si.lpMinimumApplicationAddress; MEMORY_BASIC_INFORMATION mbi;
    while (p < (uint8_t *)si.lpMaximumApplicationAddress && VirtualQuery(p, &mbi, sizeof mbi) == sizeof mbi) {
        if (mbi.State != MEM_FREE) {
            KcValue o = kc_object_new(); char perms[8];
            DWORD pr = mbi.Protect & 0xFF; bool x = pr == PAGE_EXECUTE || pr == PAGE_EXECUTE_READ || pr == PAGE_EXECUTE_READWRITE || pr == PAGE_EXECUTE_WRITECOPY;
            bool w = pr == PAGE_READWRITE || pr == PAGE_WRITECOPY || pr == PAGE_EXECUTE_READWRITE || pr == PAGE_EXECUTE_WRITECOPY;
            bool rd = pr != PAGE_NOACCESS && pr != 0;
            snprintf(perms, sizeof perms, "%c%c%c%c", rd ? 'r' : '-', w ? 'w' : '-', x ? 'x' : '-', mbi.Type == MEM_PRIVATE ? 'p' : 's');
            OI(o, "start", (uint64_t)(uintptr_t)mbi.BaseAddress); OI(o, "end", (uint64_t)(uintptr_t)mbi.BaseAddress + mbi.RegionSize); OI(o, "size", mbi.RegionSize);
            OS(o, "perms", perms); OS(o, "state", mbi.State == MEM_COMMIT ? "commit" : "reserve");
            kc_array_push(arr, o);
        }
        p = (uint8_t *)mbi.BaseAddress + mbi.RegionSize;
    }
#else
    mm_set_error("memory-map listing is not implemented on this platform");
    return kc_null();
#endif
    return arr;
}

static KcValue f_host_query(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    MM_ENTER(); const char *who = "mmu.host_query"; forbid(ip, pos, who); mm_need(n, 1, 1, pos, "mmu.host_query(address) -> map entry containing the real address | null");
    uint64_t addr = mm_arg_addr(a, n, 0, pos, who);
    KcValue none[1]; KcValue maps = f_host_maps(ip, none, 0, pos);
    if (maps.type != VAL_ARRAY) return kc_null();
    for (int i = 0; i < maps.as.arr->count; i++) {
        KcValue e = maps.as.arr->items[i];
        KcValue *s = kc_object_get(e, "start", 5), *en = kc_object_get(e, "end", 3);
        if (s && en && (uint64_t)s->as.i <= addr && addr < (uint64_t)en->as.i) return e;
    }
    mm_set_error("address 0x%llx is not mapped in this process", (unsigned long long)addr);
    return kc_null();
}

static KcValue f_host_stats(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    MM_ENTER(); const char *who = "mmu.host_stats"; forbid(ip, pos, who); mm_need(n, 0, 0, pos, "mmu.host_stats() -> object (process memory counters)");
    (void)a;
    KcValue o = kc_object_new();
#if defined(KC_HW_POSIX)
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) == 0) { OI(o, "minor_faults", ru.ru_minflt); OI(o, "major_faults", ru.ru_majflt); OI(o, "max_rss", (int64_t)ru.ru_maxrss * 1024); }
#if defined(__linux__)
    FILE *f = fopen("/proc/self/status", "r");
    if (f) {
        char l[200];
        static const struct { const char *k, *name; } want[] = { {"VmSize:", "vm_size"}, {"VmRSS:", "rss"}, {"VmData:", "data"}, {"VmStk:", "stack"}, {"VmLck:", "locked"}, {"VmPin:", "pinned"}, {"VmSwap:", "swap"}, {"VmHWM:", "rss_peak"} };
        while (fgets(l, sizeof l, f)) {
            for (size_t i = 0; i < sizeof want / sizeof want[0]; i++) {
                size_t kl = strlen(want[i].k);
                if (!strncmp(l, want[i].k, kl)) { unsigned long long kb = strtoull(l + kl, NULL, 10); OI(o, want[i].name, kb * 1024); }
            }
        }
        fclose(f);
    }
#endif
#else
    mm_set_error("process memory counters are not implemented on this platform");
    return kc_null();
#endif
    return o;
}

#define E(n, f) { n, f }
const MmuEntry mmu_host_entries[] = {
    E("host_info", f_host_info), E("host_alloc", f_host_alloc), E("host_free", f_host_free), E("host_protect", f_host_protect),
    E("host_advise", f_host_advise), E("host_lock", f_host_lock), E("host_unlock", f_host_unlock),
    E("host_read8", f_host_read8), E("host_read16", f_host_read16), E("host_read32", f_host_read32), E("host_read64", f_host_read64),
    E("host_write8", f_host_write8), E("host_write16", f_host_write16), E("host_write32", f_host_write32), E("host_write64", f_host_write64),
    E("host_read_bytes", f_host_read_bytes), E("host_write_bytes", f_host_write_bytes), E("host_fill", f_host_fill), E("host_copy", f_host_copy),
    E("host_address", f_host_address), E("host_pages", f_host_pages), E("host_resident", f_host_resident), E("host_residency", f_host_residency),
    E("host_translate", f_host_translate), E("host_maps", f_host_maps), E("host_query", f_host_query), E("host_stats", f_host_stats),
};
const int mmu_host_count = (int)(sizeof mmu_host_entries / sizeof mmu_host_entries[0]);
