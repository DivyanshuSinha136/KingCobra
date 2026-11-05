/* KingCobra std.mmio — native memory-mapped I/O and port I/O.
 *
 * A "region" is an opaque handle over a window of device (or
 * device-like) memory. Every access goes through one choke point that
 * enforces, in this order: the handle is a live region, the call is
 * allowed (`unsafe { }` for anything that can touch real hardware),
 * the region is writable if we are writing, the access lies fully
 * inside the region (overflow-safe), and the *absolute* address is
 * naturally aligned for the access width. The access itself is then a
 * single volatile load/store of exactly that width, bracketed by the
 * same ordering a kernel's readl()/writel() provides. Nothing here
 * ever uses memcpy on device memory.
 *
 * Error model (there is no try/except in the language yet):
 *   - programming errors (wrong argument types, out-of-bounds, bad
 *     alignment, use after unmap, write to a read-only region, missing
 *     `unsafe`) are fatal, exactly like std.memory / std.rhex;
 *   - environmental failures (no permission, no such device, mapping
 *     refused by the kernel, safety policy refusal) make the call
 *     return null and set mmio.last_error().
 *
 * Software ("virtual") regions are plain bounds-checked anonymous
 * memory, as safe as std.memory, so they work without `unsafe`.
 *
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com>
 */
#include "interp.h"
#include "kc_hw.h"
#include <stdarg.h>
#include <stdlib.h>
#include <ctype.h>
#if !defined(KC_HW_WINDOWS)
#include <dirent.h>
#endif

#define KC_H_MMIO 0x4D4D /* handle-kind tag for regions ('MM') */

#define MAX_BLOCK_ELEMS   (1u << 20)
#define MAX_VIRTUAL_BYTES ((size_t)1 << 28)
#define MAX_MAP_BYTES     ((int64_t)1 << 30)

static const char *SRC_PHYS = "phys", *SRC_PCI = "pci", *SRC_UIO = "uio",
                  *SRC_FILE = "file", *SRC_VIRT = "virtual";

typedef struct {
    KcHwMap     map;
    uint64_t    phys;
    bool        has_phys;
    char        label[64];
    const char *src;
    bool        writable, mapped, trace;
    uint64_t    reads, writes;
} Region;

/* ----------------------------- errors ----------------------------- */

static char g_err[256];

static void set_err(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(g_err, sizeof g_err, fmt, ap);
    va_end(ap);
}

static KcValue mm_last_error(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; (void)a;
    if (n != 0) kc_error_at(pos, "mmio.last_error() -> string");
    return kc_string(g_err);
}

static void need_unsafe(KcInterp *ip, KcPos pos, const char *who) {
    if (ip->in_unsafe <= 0)
        kc_error_at(pos, "%s: raw hardware access requires an `unsafe { }` block", who);
}

/* ----------------------------- tracing ---------------------------- */

#define TRACE_CAP 4096
typedef struct { char label[24]; char op; uint8_t width; uint64_t off, value; } TraceEnt;
static TraceEnt g_trace[TRACE_CAP];
static uint64_t g_trace_total;

static void trace_push(const Region *r, char op, int w, uint64_t off, uint64_t value) {
    if (!r->trace) return;
    TraceEnt *e = &g_trace[g_trace_total % TRACE_CAP];
    snprintf(e->label, sizeof e->label, "%.23s", r->label);
    e->op = op; e->width = (uint8_t)w; e->off = off; e->value = value;
    g_trace_total++;
}

/* --------------------------- arg helpers -------------------------- */

static Region *get_region(KcValue v, KcPos pos, const char *who) {
    if (v.type != VAL_HANDLE || v.as.handle->kind != KC_H_MMIO || !v.as.handle->ptr)
        kc_error_at(pos, "%s: expected an mmio region handle", who);
    return (Region *)v.as.handle->ptr;
}

static Region *live_region(KcInterp *ip, KcValue v, KcPos pos, const char *who) {
    Region *r = get_region(v, pos, who);
    if (!r->mapped) kc_error_at(pos, "%s: use after unmap", who);
    if (r->src != SRC_VIRT) need_unsafe(ip, pos, who);
    return r;
}

static int64_t want_int(KcValue *a, int i, KcPos pos, const char *usage) {
    if (a[i].type != VAL_INT) kc_error_at(pos, "%s", usage);
    return a[i].as.i;
}

static int want_width(int64_t w, KcPos pos, const char *who) {
    if (w != 1 && w != 2 && w != 4 && w != 8)
        kc_error_at(pos, "%s: width must be 1, 2, 4 or 8 bytes (got %lld)", who, (long long)w);
#if !KC_HW_HAS_64BIT_ACCESS
    if (w == 8) kc_error_at(pos, "%s: 64-bit accesses need a 64-bit host", who);
#endif
    return (int)w;
}

/* Accept a value representable in `w` bytes as either unsigned or
 * two's-complement signed; refuse silent truncation. */
static uint64_t fit_value(int64_t v, int w, KcPos pos, const char *who) {
    if (w == 8) return (uint64_t)v;
    int bits = w * 8;
    int64_t umax = ((int64_t)1 << bits) - 1, smin = -((int64_t)1 << (bits - 1));
    if (v < smin || v > umax)
        kc_error_at(pos, "%s: value %lld does not fit in %d bits", who, (long long)v, bits);
    return (uint64_t)v & (uint64_t)umax;
}

static KcValue to_kc(uint64_t v) { return kc_int((int64_t)v); }

/* The single choke point for every register access. */
static volatile uint8_t *access_ptr(Region *r, int64_t off, int w, bool write, KcPos pos, const char *who) {
    if (write && !r->writable) kc_error_at(pos, "%s: region '%s' is read-only", who, r->label);
    if (off < 0 || r->map.len < (size_t)w || (uint64_t)off > (uint64_t)(r->map.len - (size_t)w))
        kc_error_at(pos, "%s: %d-byte access at offset %lld out of bounds (region '%s' is %zu bytes)",
                    who, w, (long long)off, r->label, r->map.len);
    volatile uint8_t *p = r->map.ptr + off;
    if ((uintptr_t)p & (uintptr_t)(w - 1))
        kc_error_at(pos, "%s: misaligned %d-byte access at offset %lld (address is not %d-byte aligned)",
                    who, w, (long long)off, w);
    return p;
}

static uint64_t reg_read(Region *r, volatile uint8_t *p, int w, bool be, int64_t off) {
    uint64_t v = kc_hw_rd(p, w);
    kc_hw_post_read();
    if (be) v = kc_hw_bswap(v, w);
    r->reads++;
    trace_push(r, 'r', w, (uint64_t)off, v);
    return v;
}

static void reg_write(Region *r, volatile uint8_t *p, int w, bool be, int64_t off, uint64_t v) {
    uint64_t raw = be ? kc_hw_bswap(v, w) : v;
    kc_hw_pre_write();
    kc_hw_wr(p, w, raw);
    r->writes++;
    trace_push(r, 'w', w, (uint64_t)off, v);
}

/* --------------------------- region setup ------------------------- */

static KcValue region_value(Region *r) {
    KcHandle *h = kc_malloc(sizeof(KcHandle));
    memset(h, 0, sizeof *h);
    h->kind = KC_H_MMIO;
    h->ptr = r;
    h->size = r->map.len;
    KcValue v; v.type = VAL_HANDLE; v.as.handle = h;
    return v;
}

static Region *region_new(const char *src, const char *label) {
    Region *r = kc_malloc(sizeof(Region));
    memset(r, 0, sizeof *r);
    r->src = src;
    snprintf(r->label, sizeof r->label, "%s", label);
    return r;
}

typedef struct { bool ro, wc, ram; } MapFlags;

static MapFlags parse_flags(KcValue *a, int n, int idx, KcPos pos, const char *who) {
    MapFlags f = {0};
    if (n <= idx) return f;
    if (a[idx].type != VAL_STRING) kc_error_at(pos, "%s: flags must be a string such as \"ro\" or \"rw,wc\"", who);
    char buf[64];
    snprintf(buf, sizeof buf, "%s", a[idx].as.s->data);
    for (char *t = strtok(buf, ", |+"); t; t = strtok(NULL, ", |+")) {
        if (!strcmp(t, "ro")) f.ro = true;
        else if (!strcmp(t, "rw")) f.ro = false;
        else if (!strcmp(t, "wc")) f.wc = true;
        else if (!strcmp(t, "ram")) f.ram = true;
        else kc_error_at(pos, "%s: unknown flag '%s' (valid: ro, rw, wc, ram)", who, t);
    }
    return f;
}

static void fmt_label(char *out, size_t n, const char *prefix, uint64_t v) {
    snprintf(out, n, "%s@0x%llx", prefix, (unsigned long long)v);
}

/* ------------------------ /proc/iomem (Linux) ---------------------- */

typedef struct { uint64_t start, end; int depth; char name[80]; } IoMemEnt;

static int iomem_load(IoMemEnt **out) {
    *out = NULL;
#if defined(KC_HW_WINDOWS)
    return 0;
#else
    FILE *f = fopen("/proc/iomem", "r");
    if (!f) return 0;
    int count = 0, cap = 0;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        const char *p = line; int sp = 0;
        while (*p == ' ') { p++; sp++; }
        unsigned long long s, e; char name[80]; int used = 0;
        if (sscanf(p, "%llx-%llx : %79[^\n]%n", &s, &e, name, &used) < 3) continue;
        if (count == cap) { cap = cap ? cap * 2 : 64; *out = kc_realloc(*out, sizeof(IoMemEnt) * (size_t)cap); }
        IoMemEnt *en = &(*out)[count++];
        en->start = s; en->end = e; en->depth = sp / 2;
        snprintf(en->name, sizeof en->name, "%s", name);
    }
    fclose(f);
    return count;
#endif
}

/* True only when /proc/iomem is readable AND shows real addresses
 * (unprivileged readers see all-zero ranges) AND the range overlaps
 * System RAM. Unknown => false: the kernel's own STRICT_DEVMEM policy
 * is the backstop in that case. */
static bool overlaps_system_ram(uint64_t phys, uint64_t len) {
    IoMemEnt *ents; int n = iomem_load(&ents);
    bool reliable = false, hit = false;
    uint64_t last = phys + len - 1;
    for (int i = 0; i < n; i++) {
        if (ents[i].end > 0) reliable = true;
        if (!strcmp(ents[i].name, "System RAM") && ents[i].start <= last && phys <= ents[i].end) hit = true;
    }
    free(ents);
    return reliable && hit;
}

static KcValue mm_iomem(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; (void)a;
    if (n != 0) kc_error_at(pos, "mmio.iomem() -> [[start, end, name, depth], ...]");
    IoMemEnt *ents; int cnt = iomem_load(&ents);
    KcValue out = kc_array_new();
    for (int i = 0; i < cnt; i++) {
        KcValue row = kc_array_new();
        kc_array_push(row, to_kc(ents[i].start)); kc_array_push(row, to_kc(ents[i].end));
        kc_array_push(row, kc_string(ents[i].name)); kc_array_push(row, kc_int(ents[i].depth));
        kc_array_push(out, row);
    }
    free(ents);
    return out;
}

/* ------------------------------ small fs -------------------------- */

#if !defined(KC_HW_WINDOWS)
static int read_small(const char *path, char *buf, size_t n) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    size_t got = fread(buf, 1, n - 1, f);
    fclose(f);
    buf[got] = 0;
    while (got && (buf[got - 1] == '\n' || buf[got - 1] == '\r')) buf[--got] = 0;
    return (int)got;
}

static bool read_hex_file(const char *path, uint64_t *out) {
    char b[64];
    if (read_small(path, b, sizeof b) <= 0) return false;
    char *end; errno = 0;
    unsigned long long v = strtoull(b, &end, 16);
    if (end == b || errno) return false;
    *out = v; return true;
}
#endif

static bool norm_bdf(const char *s, char *out, size_t n) {
    unsigned d = 0, b, dev, f; int used = 0;
    if (sscanf(s, "%x:%x:%x.%x%n", &d, &b, &dev, &f, &used) == 4 && s[used] == 0) {}
    else if (used = 0, sscanf(s, "%x:%x.%x%n", &b, &dev, &f, &used) == 3 && s[used] == 0) d = 0;
    else return false;
    if (d > 0xFFFF || b > 0xFF || dev > 0x1F || f > 7) return false;
    snprintf(out, n, "%04x:%02x:%02x.%x", d, b, dev, f);
    return true;
}

/* ------------------------------ mapping --------------------------- */

static KcValue mm_map(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need_unsafe(ip, pos, "mmio.map");
    const char *u = "mmio.map(phys: int, len: int [, flags: string]) -> region | null";
    if (n < 2 || n > 3) kc_error_at(pos, "%s", u);
    int64_t phys = want_int(a, 0, pos, u), len = want_int(a, 1, pos, u);
    if (phys < 0 || len <= 0) kc_error_at(pos, "mmio.map: phys must be >= 0 and len > 0");
    if ((uint64_t)len > (uint64_t)INT64_MAX - (uint64_t)phys) kc_error_at(pos, "mmio.map: range wraps the address space");
    if (len > MAX_MAP_BYTES) kc_error_at(pos, "mmio.map: len exceeds the 1 GiB per-mapping limit");
    MapFlags f = parse_flags(a, n, 2, pos, "mmio.map");
    if (f.wc) kc_error_at(pos, "mmio.map: \"wc\" is only available through mmio.map_pci");
    if (!f.ram && overlaps_system_ram((uint64_t)phys, (uint64_t)len)) {
        set_err("refusing to map a range overlapping System RAM (see /proc/iomem); "
                "pass the \"ram\" flag if you really mean it");
        return kc_null();
    }
    char label[64]; fmt_label(label, sizeof label, "phys", (uint64_t)phys);
    Region *r = region_new(SRC_PHYS, label);
    char err[200];
    if (kc_hw_map_phys(&r->map, (uint64_t)phys, (size_t)len, !f.ro, err, sizeof err) != 0) { set_err("%s", err); free(r); return kc_null(); }
    r->phys = (uint64_t)phys; r->has_phys = true; r->writable = !f.ro; r->mapped = true;
    return region_value(r);
}

static KcValue mm_map_pci(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need_unsafe(ip, pos, "mmio.map_pci");
    const char *u = "mmio.map_pci(bdf: string, bar: int [, flags: string]) -> region | null";
    if (n < 2 || n > 3 || a[0].type != VAL_STRING || a[1].type != VAL_INT) kc_error_at(pos, "%s", u);
    int64_t bar = a[1].as.i;
    if (bar < 0 || bar > 5) kc_error_at(pos, "mmio.map_pci: bar must be 0..5");
    MapFlags f = parse_flags(a, n, 2, pos, "mmio.map_pci");
    char bdf[32];
    if (!norm_bdf(a[0].as.s->data, bdf, sizeof bdf)) kc_error_at(pos, "mmio.map_pci: bad PCI address '%s' (expected [dddd:]bb:dd.f)", a[0].as.s->data);
#if defined(KC_HW_WINDOWS)
    (void)f; set_err("PCI BAR mapping is Linux-only (sysfs); not available on Windows");
    return kc_null();
#else
    char path[160];
    snprintf(path, sizeof path, "/sys/bus/pci/devices/%s/resource%d%s", bdf, (int)bar, f.wc ? "_wc" : "");
    struct stat st;
    if (stat(path, &st) != 0) { set_err("%s: %s", path, strerror(errno)); return kc_null(); }
    if (st.st_size <= 0) { set_err("%s: BAR %d is not implemented on this device", bdf, (int)bar); return kc_null(); }
    char label[64]; snprintf(label, sizeof label, "pci:%s/bar%d", bdf, (int)bar);
    Region *r = region_new(SRC_PCI, label);
    char err[200];
    if (kc_hw_map_file(&r->map, path, 0, (size_t)st.st_size, !f.ro, false, err, sizeof err) != 0) { set_err("%s", err); free(r); return kc_null(); }
    r->writable = !f.ro; r->mapped = true;
    char rp[160]; snprintf(rp, sizeof rp, "/sys/bus/pci/devices/%s/resource", bdf);
    FILE *rf = fopen(rp, "r");
    if (rf) {
        char line[128]; int idx = 0;
        while (fgets(line, sizeof line, rf) && idx <= (int)bar) {
            unsigned long long s, e, fl;
            if (idx == (int)bar && sscanf(line, "%llx %llx %llx", &s, &e, &fl) == 3) { r->phys = s; r->has_phys = true; }
            idx++;
        }
        fclose(rf);
    }
    return region_value(r);
#endif
}

static KcValue mm_map_uio(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need_unsafe(ip, pos, "mmio.map_uio");
    const char *u = "mmio.map_uio(device: int, map_index: int [, flags: string]) -> region | null";
    if (n < 2 || n > 3 || a[0].type != VAL_INT || a[1].type != VAL_INT) kc_error_at(pos, "%s", u);
    int64_t dev = a[0].as.i, idx = a[1].as.i;
    if (dev < 0 || dev > 1023 || idx < 0 || idx > 4) kc_error_at(pos, "mmio.map_uio: device must be 0..1023 and map_index 0..4");
    MapFlags f = parse_flags(a, n, 2, pos, "mmio.map_uio");
    if (f.wc) kc_error_at(pos, "mmio.map_uio: \"wc\" is only available through mmio.map_pci");
#if defined(KC_HW_WINDOWS)
    (void)f; set_err("UIO is Linux-only; not available on Windows");
    return kc_null();
#else
    char p[160]; uint64_t size, off = 0, addr = 0;
    snprintf(p, sizeof p, "/sys/class/uio/uio%d/maps/map%d/size", (int)dev, (int)idx);
    if (!read_hex_file(p, &size) || size == 0) { set_err("uio%d map%d: no such map (%s)", (int)dev, (int)idx, p); return kc_null(); }
    snprintf(p, sizeof p, "/sys/class/uio/uio%d/maps/map%d/offset", (int)dev, (int)idx);
    read_hex_file(p, &off);
    snprintf(p, sizeof p, "/sys/class/uio/uio%d/maps/map%d/addr", (int)dev, (int)idx);
    read_hex_file(p, &addr);
    size_t ps = kc_hw_page_size();
    if (off >= ps || size > (uint64_t)MAX_MAP_BYTES) { set_err("uio%d map%d: implausible size/offset", (int)dev, (int)idx); return kc_null(); }
    snprintf(p, sizeof p, "/dev/uio%d", (int)dev);
    char label[64]; snprintf(label, sizeof label, "uio%d/map%d", (int)dev, (int)idx);
    Region *r = region_new(SRC_UIO, label);
    char err[200];
    /* UIO selects map N with mmap offset N * PAGE_SIZE; the device
     * region begins `off` bytes into the first page of that mapping. */
    if (kc_hw_map_file(&r->map, p, (uint64_t)idx * ps, (size_t)(size + off), !f.ro, false, err, sizeof err) != 0) { set_err("%s", err); free(r); return kc_null(); }
    r->map.ptr += off; r->map.len = (size_t)size;
    r->phys = addr; r->has_phys = addr != 0; r->writable = !f.ro; r->mapped = true;
    return region_value(r);
#endif
}

static KcValue mm_map_file(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need_unsafe(ip, pos, "mmio.map_file");
    const char *u = "mmio.map_file(path: string, offset: int, len: int [, flags: string]) -> region | null";
    if (n < 3 || n > 4 || a[0].type != VAL_STRING || a[1].type != VAL_INT || a[2].type != VAL_INT) kc_error_at(pos, "%s", u);
    int64_t off = a[1].as.i, len = a[2].as.i;
    if (off < 0 || len <= 0 || len > MAX_MAP_BYTES) kc_error_at(pos, "mmio.map_file: offset must be >= 0 and 0 < len <= 1 GiB");
    MapFlags f = parse_flags(a, n, 3, pos, "mmio.map_file");
    if (f.wc) kc_error_at(pos, "mmio.map_file: \"wc\" is only available through mmio.map_pci");
    Region *r = region_new(SRC_FILE, a[0].as.s->data);
    char err[200];
    if (kc_hw_map_file(&r->map, a[0].as.s->data, (uint64_t)off, (size_t)len, !f.ro, false, err, sizeof err) != 0) { set_err("%s", err); free(r); return kc_null(); }
    r->writable = !f.ro; r->mapped = true;
    return region_value(r);
}

static KcValue mm_virtual(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    const char *u = "mmio.virtual(len: int [, label: string]) -> region | null";
    if (n < 1 || n > 2 || a[0].type != VAL_INT || (n == 2 && a[1].type != VAL_STRING)) kc_error_at(pos, "%s", u);
    if (a[0].as.i <= 0 || (uint64_t)a[0].as.i > MAX_VIRTUAL_BYTES) kc_error_at(pos, "mmio.virtual: len must be 1..%zu", MAX_VIRTUAL_BYTES);
    Region *r = region_new(SRC_VIRT, n == 2 ? a[1].as.s->data : "virtual");
    char err[200];
    if (kc_hw_map_anon(&r->map, (size_t)a[0].as.i, err, sizeof err) != 0) { set_err("%s", err); free(r); return kc_null(); }
    r->writable = true; r->mapped = true;
    return region_value(r);
}

static KcValue mm_unmap(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    if (n != 1) kc_error_at(pos, "mmio.unmap(region) -> bool");
    Region *r = get_region(a[0], pos, "mmio.unmap");
    if (!r->mapped) return kc_bool(false);
    kc_hw_unmap(&r->map);
    r->mapped = false; r->trace = false;
    return kc_bool(true);
}

/* ------------------------------- info ----------------------------- */

static KcValue mm_size(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; if (n != 1) kc_error_at(pos, "mmio.size(region) -> int");
    Region *r = get_region(a[0], pos, "mmio.size");
    return kc_int((int64_t)(r->mapped ? r->map.len : 0));
}
static KcValue mm_base(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; if (n != 1) kc_error_at(pos, "mmio.base(region) -> int | null  (physical base, when known)");
    Region *r = get_region(a[0], pos, "mmio.base");
    return r->has_phys ? to_kc(r->phys) : kc_null();
}
static KcValue mm_label(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; if (n != 1) kc_error_at(pos, "mmio.label(region) -> string");
    return kc_string(get_region(a[0], pos, "mmio.label")->label);
}
static KcValue mm_source(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; if (n != 1) kc_error_at(pos, "mmio.source(region) -> string");
    return kc_string(get_region(a[0], pos, "mmio.source")->src);
}
static KcValue mm_is_mapped(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; if (n != 1) kc_error_at(pos, "mmio.is_mapped(region) -> bool");
    return kc_bool(get_region(a[0], pos, "mmio.is_mapped")->mapped);
}
static KcValue mm_info(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; if (n != 1) kc_error_at(pos, "mmio.info(region) -> [label, source, phys, len, writable, mapped, reads, writes]");
    Region *r = get_region(a[0], pos, "mmio.info");
    KcValue o = kc_array_new();
    kc_array_push(o, kc_string(r->label)); kc_array_push(o, kc_string(r->src));
    kc_array_push(o, r->has_phys ? to_kc(r->phys) : kc_null());
    kc_array_push(o, kc_int((int64_t)(r->mapped ? r->map.len : 0)));
    kc_array_push(o, kc_bool(r->writable)); kc_array_push(o, kc_bool(r->mapped));
    kc_array_push(o, to_kc(r->reads)); kc_array_push(o, to_kc(r->writes));
    return o;
}

static KcValue mm_capabilities(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; (void)a;
    if (n != 0) kc_error_at(pos, "mmio.capabilities() -> string[]");
    KcValue o = kc_array_new();
    kc_array_push(o, kc_string("virtual")); kc_array_push(o, kc_string("file"));
#if !defined(KC_HW_WINDOWS)
    if (access("/dev/mem", R_OK) == 0) kc_array_push(o, kc_string("phys"));
    if (access("/sys/bus/pci/devices", R_OK) == 0) kc_array_push(o, kc_string("pci"));
    if (access("/sys/class/uio", R_OK) == 0) kc_array_push(o, kc_string("uio"));
#endif
    if (kc_hw_port_supported()) kc_array_push(o, kc_string("port"));
    return o;
}

/* --------------------------- register access ---------------------- */

static KcValue rw_read(KcInterp *ip, KcValue *a, int n, KcPos pos, int w, bool be, const char *who) {
    if (n != 2 || a[1].type != VAL_INT) kc_error_at(pos, "%s(region, offset: int) -> int", who);
    Region *r = live_region(ip, a[0], pos, who);
    volatile uint8_t *p = access_ptr(r, a[1].as.i, w, false, pos, who);
    return to_kc(reg_read(r, p, w, be, a[1].as.i));
}
static KcValue rw_write(KcInterp *ip, KcValue *a, int n, KcPos pos, int w, bool be, const char *who) {
    if (n != 3 || a[1].type != VAL_INT || a[2].type != VAL_INT) kc_error_at(pos, "%s(region, offset: int, value: int)", who);
    Region *r = live_region(ip, a[0], pos, who);
    uint64_t v = fit_value(a[2].as.i, w, pos, who);
    volatile uint8_t *p = access_ptr(r, a[1].as.i, w, true, pos, who);
    reg_write(r, p, w, be, a[1].as.i, v);
    return kc_null();
}

#define DEF_RW(BITS, BYTES) \
    static KcValue mm_read##BITS(KcInterp *ip, KcValue *a, int n, KcPos pos)  { return rw_read(ip, a, n, pos, BYTES, false, "mmio.read" #BITS); } \
    static KcValue mm_write##BITS(KcInterp *ip, KcValue *a, int n, KcPos pos) { return rw_write(ip, a, n, pos, BYTES, false, "mmio.write" #BITS); }
DEF_RW(8, 1) DEF_RW(16, 2) DEF_RW(32, 4) DEF_RW(64, 8)

static KcValue mm_read(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    if (n != 3 || a[2].type != VAL_INT) kc_error_at(pos, "mmio.read(region, offset: int, width: int) -> int");
    return rw_read(ip, a, 2, pos, want_width(a[2].as.i, pos, "mmio.read"), false, "mmio.read");
}
static KcValue mm_write(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    if (n != 4 || a[2].type != VAL_INT) kc_error_at(pos, "mmio.write(region, offset: int, width: int, value: int)");
    KcValue t[3] = { a[0], a[1], a[3] };
    return rw_write(ip, t, 3, pos, want_width(a[2].as.i, pos, "mmio.write"), false, "mmio.write");
}
static KcValue mm_read_be(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    if (n != 3 || a[2].type != VAL_INT) kc_error_at(pos, "mmio.read_be(region, offset: int, width: int) -> int");
    return rw_read(ip, a, 2, pos, want_width(a[2].as.i, pos, "mmio.read_be"), true, "mmio.read_be");
}
static KcValue mm_write_be(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    if (n != 4 || a[2].type != VAL_INT) kc_error_at(pos, "mmio.write_be(region, offset: int, width: int, value: int)");
    KcValue t[3] = { a[0], a[1], a[3] };
    return rw_write(ip, t, 3, pos, want_width(a[2].as.i, pos, "mmio.write_be"), true, "mmio.write_be");
}

/* Read-modify-write. NOT atomic with respect to the device or other
 * CPUs/threads: it is a read followed by a write. Returns the old value. */
static KcValue rmw(KcInterp *ip, KcValue *a, int n, KcPos pos, const char *who, int mode) {
    int need = mode == 2 ? 5 : 4; /* 0 set, 1 clear, 2 update */
    if (n != need) kc_error_at(pos, mode == 2 ? "mmio.update(region, offset, width, mask, value) -> old" : "%s(region, offset, width, mask) -> old", who);
    for (int i = 1; i < n; i++) if (a[i].type != VAL_INT) kc_error_at(pos, "%s: offset, width, mask and value must be ints", who);
    Region *r = live_region(ip, a[0], pos, who);
    int w = want_width(a[2].as.i, pos, who);
    uint64_t mask = fit_value(a[3].as.i, w, pos, who);
    uint64_t val  = mode == 0 ? mask : mode == 1 ? 0 : fit_value(a[4].as.i, w, pos, who);
    volatile uint8_t *p = access_ptr(r, a[1].as.i, w, true, pos, who);
    uint64_t old = reg_read(r, p, w, false, a[1].as.i);
    uint64_t wmask = w == 8 ? ~0ull : ((1ull << (w * 8)) - 1);
    uint64_t nv = ((old & ~mask) | (val & mask)) & wmask;
    reg_write(r, p, w, false, a[1].as.i, nv);
    return to_kc(old);
}
static KcValue mm_set_bits(KcInterp *ip, KcValue *a, int n, KcPos pos)   { return rmw(ip, a, n, pos, "mmio.set_bits", 0); }
static KcValue mm_clear_bits(KcInterp *ip, KcValue *a, int n, KcPos pos) { return rmw(ip, a, n, pos, "mmio.clear_bits", 1); }
static KcValue mm_update(KcInterp *ip, KcValue *a, int n, KcPos pos)     { return rmw(ip, a, n, pos, "mmio.update", 2); }

/* Poll until (reg & mask) == (expected & mask) or the timeout (µs)
 * elapses. Returns true on success. Only the final read is traced. */
static KcValue mm_wait_bits(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    const char *u = "mmio.wait_bits(region, offset, width, mask, expected, timeout_us) -> bool";
    if (n != 6) kc_error_at(pos, "%s", u);
    for (int i = 1; i < 6; i++) if (a[i].type != VAL_INT) kc_error_at(pos, "%s", u);
    Region *r = live_region(ip, a[0], pos, "mmio.wait_bits");
    int w = want_width(a[2].as.i, pos, "mmio.wait_bits");
    uint64_t mask = fit_value(a[3].as.i, w, pos, "mmio.wait_bits");
    uint64_t want = fit_value(a[4].as.i, w, pos, "mmio.wait_bits") & mask;
    int64_t tmo = a[5].as.i;
    if (tmo < 0 || tmo > 3600ll * 1000000ll) kc_error_at(pos, "mmio.wait_bits: timeout_us must be 0..3600000000");
    volatile uint8_t *p = access_ptr(r, a[1].as.i, w, false, pos, "mmio.wait_bits");
    uint64_t deadline = kc_hw_now_ns() + (uint64_t)tmo * 1000u;
    uint64_t v = 0; unsigned spins = 0;
    for (;;) {
        v = kc_hw_rd(p, w); kc_hw_post_read();
        if ((v & mask) == want) break;
        if (tmo == 0) break;
        if ((++spins & 15u) == 0 && kc_hw_now_ns() >= deadline) break;
        kc_hw_cpu_relax();
        if ((spins & 0x3FFu) == 0) kc_hw_yield();
    }
    r->reads++;
    trace_push(r, 'r', w, (uint64_t)a[1].as.i, v);
    return kc_bool((v & mask) == want);
}

/* ------------------------------- bulk ----------------------------- */

static KcValue mm_read_block(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    const char *u = "mmio.read_block(region, offset, count, width) -> int[]";
    if (n != 4) kc_error_at(pos, "%s", u);
    for (int i = 1; i < 4; i++) if (a[i].type != VAL_INT) kc_error_at(pos, "%s", u);
    Region *r = live_region(ip, a[0], pos, "mmio.read_block");
    int w = want_width(a[3].as.i, pos, "mmio.read_block");
    int64_t cnt = a[2].as.i;
    if (cnt < 0 || (uint64_t)cnt > MAX_BLOCK_ELEMS) kc_error_at(pos, "mmio.read_block: count must be 0..%u", MAX_BLOCK_ELEMS);
    KcValue out = kc_array_new();
    if (cnt == 0) return out;
    access_ptr(r, a[1].as.i, w, false, pos, "mmio.read_block");
    access_ptr(r, a[1].as.i + (cnt - 1) * w, w, false, pos, "mmio.read_block"); /* last element in range */
    for (int64_t i = 0; i < cnt; i++) {
        int64_t off = a[1].as.i + i * w;
        kc_array_push(out, to_kc(reg_read(r, r->map.ptr + off, w, false, off)));
    }
    return out;
}

static KcValue mm_write_block(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    const char *u = "mmio.write_block(region, offset, width, values: int[])";
    if (n != 4 || a[1].type != VAL_INT || a[2].type != VAL_INT || a[3].type != VAL_ARRAY) kc_error_at(pos, "%s", u);
    Region *r = live_region(ip, a[0], pos, "mmio.write_block");
    int w = want_width(a[2].as.i, pos, "mmio.write_block");
    KcArray *arr = a[3].as.arr;
    if (arr->count > 0 && (uint32_t)arr->count > MAX_BLOCK_ELEMS) kc_error_at(pos, "mmio.write_block: too many elements");
    if (arr->count == 0) return kc_null();
    /* validate everything first so a bad element cannot leave a half-written block */
    access_ptr(r, a[1].as.i, w, true, pos, "mmio.write_block");
    access_ptr(r, a[1].as.i + ((int64_t)arr->count - 1) * w, w, true, pos, "mmio.write_block");
    uint64_t *vals = kc_malloc(sizeof(uint64_t) * (size_t)arr->count);
    for (int i = 0; i < arr->count; i++) {
        if (arr->items[i].type != VAL_INT) { free(vals); kc_error_at(pos, "mmio.write_block: element %d is not an int", i); }
        vals[i] = fit_value(arr->items[i].as.i, w, pos, "mmio.write_block");
    }
    for (int i = 0; i < arr->count; i++) {
        int64_t off = a[1].as.i + (int64_t)i * w;
        reg_write(r, r->map.ptr + off, w, false, off, vals[i]);
    }
    free(vals);
    return kc_null();
}

static KcValue mm_fill(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    const char *u = "mmio.fill(region, offset, width, count, value)";
    if (n != 5) kc_error_at(pos, "%s", u);
    for (int i = 1; i < 5; i++) if (a[i].type != VAL_INT) kc_error_at(pos, "%s", u);
    Region *r = live_region(ip, a[0], pos, "mmio.fill");
    int w = want_width(a[2].as.i, pos, "mmio.fill");
    int64_t cnt = a[3].as.i;
    if (cnt < 0 || (uint64_t)cnt > ((uint64_t)1 << 28)) kc_error_at(pos, "mmio.fill: count out of range");
    uint64_t v = fit_value(a[4].as.i, w, pos, "mmio.fill");
    if (cnt == 0) return kc_null();
    access_ptr(r, a[1].as.i, w, true, pos, "mmio.fill");
    access_ptr(r, a[1].as.i + (cnt - 1) * w, w, true, pos, "mmio.fill");
    for (int64_t i = 0; i < cnt; i++) {
        int64_t off = a[1].as.i + i * w;
        reg_write(r, r->map.ptr + off, w, false, off, v);
    }
    return kc_null();
}

static KcHandle *want_membuf(KcValue v, KcPos pos, const char *who) {
    if (v.type != VAL_HANDLE || v.as.handle->kind != H_MEMBUF)
        kc_error_at(pos, "%s: expected a memory.alloc() buffer", who);
    if (!v.as.handle->ptr) kc_error_at(pos, "%s: buffer was freed", who);
    return v.as.handle;
}

/* Width-preserving transfers between a region and a std.memory buffer. */
static KcValue xfer(KcInterp *ip, KcValue *a, int n, KcPos pos, bool to_region, const char *who) {
    if (n != 6) kc_error_at(pos, "%s(region, region_offset, buffer, buffer_offset, nbytes, width)", who);
    for (int i = 1; i < 6; i++) if (i != 2 && a[i].type != VAL_INT) kc_error_at(pos, "%s: offsets, nbytes and width must be ints", who);
    Region *r = live_region(ip, a[0], pos, who);
    KcHandle *b = want_membuf(a[2], pos, who);
    int w = want_width(a[5].as.i, pos, who);
    int64_t roff = a[1].as.i, boff = a[3].as.i, nb = a[4].as.i;
    if (nb < 0 || nb % w) kc_error_at(pos, "%s: nbytes must be a non-negative multiple of width", who);
    if (nb == 0) return kc_null();
    if (boff < 0 || (uint64_t)boff > b->size || (uint64_t)nb > b->size - (uint64_t)boff)
        kc_error_at(pos, "%s: buffer range [%lld, +%lld) exceeds buffer size %zu", who, (long long)boff, (long long)nb, b->size);
    access_ptr(r, roff, w, to_region, pos, who);
    access_ptr(r, roff + nb - w, w, to_region, pos, who);
    uint8_t *mem = (uint8_t *)b->ptr + boff;
    for (int64_t i = 0; i < nb; i += w) {
        uint64_t v = 0;
        if (to_region) { memcpy(&v, mem + i, (size_t)w); reg_write(r, r->map.ptr + roff + i, w, false, roff + i, v); }
        else { v = reg_read(r, r->map.ptr + roff + i, w, false, roff + i); memcpy(mem + i, &v, (size_t)w); }
    }
    return kc_null();
}
static KcValue mm_read_into(KcInterp *ip, KcValue *a, int n, KcPos pos)  { return xfer(ip, a, n, pos, false, "mmio.read_into"); }
static KcValue mm_write_from(KcInterp *ip, KcValue *a, int n, KcPos pos) { return xfer(ip, a, n, pos, true, "mmio.write_from"); }

/* ------------------------------ barriers -------------------------- */

static KcValue mm_barrier(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    if (n > 1 || (n == 1 && a[0].type != VAL_STRING)) kc_error_at(pos, "mmio.barrier([kind: \"full\"|\"read\"|\"write\"|\"compiler\"])");
    int k = KC_HW_BAR_FULL;
    if (n == 1) {
        const char *s = a[0].as.s->data;
        if (!strcmp(s, "full")) k = KC_HW_BAR_FULL;
        else if (!strcmp(s, "read")) k = KC_HW_BAR_READ;
        else if (!strcmp(s, "write")) k = KC_HW_BAR_WRITE;
        else if (!strcmp(s, "compiler")) k = KC_HW_BAR_COMPILER;
        else kc_error_at(pos, "mmio.barrier: unknown kind '%s'", s);
    }
    kc_hw_barrier(k);
    return kc_null();
}

/* ------------------------------ port I/O -------------------------- */

static KcValue mm_port_in(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need_unsafe(ip, pos, "mmio.port_in");
    if (n != 2 || a[0].type != VAL_INT || a[1].type != VAL_INT) kc_error_at(pos, "mmio.port_in(port: int, width: int) -> int | null");
    int64_t port = a[0].as.i, w = a[1].as.i;
    if (port < 0 || port > 0xFFFF) kc_error_at(pos, "mmio.port_in: port must be 0..65535");
    if (w != 1 && w != 2 && w != 4) kc_error_at(pos, "mmio.port_in: width must be 1, 2 or 4");
    char err[200];
    if (kc_hw_port_access((uint16_t)port, (int)w, err, sizeof err) != 0) { set_err("%s", err); return kc_null(); }
    return kc_int((int64_t)kc_hw_port_in((uint16_t)port, (int)w));
}
static KcValue mm_port_out(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need_unsafe(ip, pos, "mmio.port_out");
    if (n != 3 || a[0].type != VAL_INT || a[1].type != VAL_INT || a[2].type != VAL_INT) kc_error_at(pos, "mmio.port_out(port: int, width: int, value: int) -> bool");
    int64_t port = a[0].as.i, w = a[1].as.i;
    if (port < 0 || port > 0xFFFF) kc_error_at(pos, "mmio.port_out: port must be 0..65535");
    if (w != 1 && w != 2 && w != 4) kc_error_at(pos, "mmio.port_out: width must be 1, 2 or 4");
    uint64_t v = fit_value(a[2].as.i, (int)w, pos, "mmio.port_out");
    char err[200];
    if (kc_hw_port_access((uint16_t)port, (int)w, err, sizeof err) != 0) { set_err("%s", err); return kc_bool(false); }
    kc_hw_port_out((uint16_t)port, (int)w, (uint32_t)v);
    return kc_bool(true);
}

/* ------------------------------- PCI ------------------------------ */

static int cmp_str(const void *x, const void *y) { return strcmp(*(char *const *)x, *(char *const *)y); }

static KcValue mm_pci_list(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; (void)a;
    if (n != 0) kc_error_at(pos, "mmio.pci_list() -> string[]");
    KcValue out = kc_array_new();
#if !defined(KC_HW_WINDOWS)
    DIR *d = opendir("/sys/bus/pci/devices");
    if (!d) { set_err("/sys/bus/pci/devices: %s", strerror(errno)); return out; }
    char **names = NULL; int cnt = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        if (cnt == cap) { cap = cap ? cap * 2 : 32; names = kc_realloc(names, sizeof(char *) * (size_t)cap); }
        names[cnt++] = kc_strdup(e->d_name);
    }
    closedir(d);
    if (cnt) qsort(names, (size_t)cnt, sizeof(char *), cmp_str);
    for (int i = 0; i < cnt; i++) { kc_array_push(out, kc_string(names[i])); free(names[i]); }
    free(names);
#else
    set_err("PCI enumeration is Linux-only; not available on Windows");
#endif
    return out;
}

static KcValue mm_pci_info(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    if (n != 1 || a[0].type != VAL_STRING) kc_error_at(pos, "mmio.pci_info(bdf: string) -> [vendor, device, class] | null");
    char bdf[32];
    if (!norm_bdf(a[0].as.s->data, bdf, sizeof bdf)) kc_error_at(pos, "mmio.pci_info: bad PCI address '%s'", a[0].as.s->data);
#if defined(KC_HW_WINDOWS)
    set_err("PCI enumeration is Linux-only; not available on Windows"); return kc_null();
#else
    static const char *files[3] = { "vendor", "device", "class" };
    uint64_t v[3];
    for (int i = 0; i < 3; i++) {
        char p[128]; snprintf(p, sizeof p, "/sys/bus/pci/devices/%s/%s", bdf, files[i]);
        if (!read_hex_file(p, &v[i])) { set_err("%s: cannot read %s", bdf, files[i]); return kc_null(); }
    }
    KcValue o = kc_array_new();
    for (int i = 0; i < 3; i++) kc_array_push(o, to_kc(v[i]));
    return o;
#endif
}

/* [[index, start, end, size, flags, is_io, prefetchable], ...] for every
 * implemented resource of the device (BARs 0-5, ROM = 6, then bridge windows). */
static KcValue mm_pci_bars(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    if (n != 1 || a[0].type != VAL_STRING) kc_error_at(pos, "mmio.pci_bars(bdf: string) -> [[index, start, end, size, flags, is_io, prefetchable], ...] | null");
    char bdf[32];
    if (!norm_bdf(a[0].as.s->data, bdf, sizeof bdf)) kc_error_at(pos, "mmio.pci_bars: bad PCI address '%s'", a[0].as.s->data);
#if defined(KC_HW_WINDOWS)
    set_err("PCI enumeration is Linux-only; not available on Windows"); return kc_null();
#else
    char p[128]; snprintf(p, sizeof p, "/sys/bus/pci/devices/%s/resource", bdf);
    FILE *f = fopen(p, "r");
    if (!f) { set_err("%s: %s", p, strerror(errno)); return kc_null(); }
    KcValue out = kc_array_new(); char line[160]; int idx = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long long s, e, fl;
        if (sscanf(line, "%llx %llx %llx", &s, &e, &fl) == 3 && !(s == 0 && e == 0)) {
            KcValue row = kc_array_new();
            kc_array_push(row, kc_int(idx)); kc_array_push(row, to_kc(s)); kc_array_push(row, to_kc(e));
            kc_array_push(row, to_kc(e - s + 1)); kc_array_push(row, to_kc(fl));
            kc_array_push(row, kc_bool((fl & 0x100u) != 0)); kc_array_push(row, kc_bool((fl & 0x2000u) != 0));
            kc_array_push(out, row);
        }
        idx++;
    }
    fclose(f);
    return out;
#endif
}

/* ----------------------------- tracing API ------------------------ */

static KcValue mm_trace(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    if (n != 2 || a[1].type != VAL_BOOL) kc_error_at(pos, "mmio.trace(region, enabled: bool)");
    Region *r = get_region(a[0], pos, "mmio.trace");
    if (!r->mapped) kc_error_at(pos, "mmio.trace: use after unmap");
    r->trace = a[1].as.b;
    return kc_null();
}

/* Oldest-to-newest [[label, "r"|"w", width_bytes, offset, value], ...]
 * over a 4096-entry ring shared by all traced regions. */
static KcValue mm_trace_dump(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; (void)a;
    if (n != 0) kc_error_at(pos, "mmio.trace_dump() -> [[label, op, width, offset, value], ...]");
    KcValue out = kc_array_new();
    uint64_t cnt = g_trace_total < TRACE_CAP ? g_trace_total : TRACE_CAP;
    uint64_t start = g_trace_total - cnt;
    for (uint64_t i = 0; i < cnt; i++) {
        const TraceEnt *e = &g_trace[(start + i) % TRACE_CAP];
        char op[2] = { e->op, 0 };
        KcValue row = kc_array_new();
        kc_array_push(row, kc_string(e->label)); kc_array_push(row, kc_string(op));
        kc_array_push(row, kc_int(e->width)); kc_array_push(row, to_kc(e->off)); kc_array_push(row, to_kc(e->value));
        kc_array_push(out, row);
    }
    return out;
}
static KcValue mm_trace_clear(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; (void)a;
    if (n != 0) kc_error_at(pos, "mmio.trace_clear()");
    g_trace_total = 0;
    return kc_null();
}

/* --------------------- bit utilities and formatting --------------- */
/* The language has no bitwise operators yet (only && and ||), so
 * register work needs these. All operate on 64-bit two's complement. */

static int64_t one_int(KcValue *a, int i, KcPos pos, const char *who) {
    if (a[i].type != VAL_INT) kc_error_at(pos, "%s: arguments must be ints", who);
    return a[i].as.i;
}
static KcValue mm_band(KcInterp *ip, KcValue *a, int n, KcPos pos) { (void)ip; if (n != 2) kc_error_at(pos, "mmio.band(a, b) -> int"); return kc_int(one_int(a,0,pos,"mmio.band") & one_int(a,1,pos,"mmio.band")); }
static KcValue mm_bor(KcInterp *ip, KcValue *a, int n, KcPos pos)  { (void)ip; if (n != 2) kc_error_at(pos, "mmio.bor(a, b) -> int");  return kc_int(one_int(a,0,pos,"mmio.bor")  | one_int(a,1,pos,"mmio.bor")); }
static KcValue mm_bxor(KcInterp *ip, KcValue *a, int n, KcPos pos) { (void)ip; if (n != 2) kc_error_at(pos, "mmio.bxor(a, b) -> int"); return kc_int(one_int(a,0,pos,"mmio.bxor") ^ one_int(a,1,pos,"mmio.bxor")); }
static KcValue mm_bnot(KcInterp *ip, KcValue *a, int n, KcPos pos) { (void)ip; if (n != 1) kc_error_at(pos, "mmio.bnot(a) -> int");     return kc_int(~one_int(a,0,pos,"mmio.bnot")); }
static KcValue mm_shl(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; if (n != 2) kc_error_at(pos, "mmio.shl(value, bits) -> int");
    int64_t s = one_int(a, 1, pos, "mmio.shl");
    if (s < 0 || s > 63) kc_error_at(pos, "mmio.shl: shift must be 0..63");
    return to_kc((uint64_t)one_int(a, 0, pos, "mmio.shl") << s);
}
static KcValue mm_shr(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; if (n != 2) kc_error_at(pos, "mmio.shr(value, bits) -> int  (logical shift)");
    int64_t s = one_int(a, 1, pos, "mmio.shr");
    if (s < 0 || s > 63) kc_error_at(pos, "mmio.shr: shift must be 0..63");
    return to_kc((uint64_t)one_int(a, 0, pos, "mmio.shr") >> s);
}
static KcValue mm_bit(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; if (n != 2) kc_error_at(pos, "mmio.bit(value, index) -> bool");
    int64_t s = one_int(a, 1, pos, "mmio.bit");
    if (s < 0 || s > 63) kc_error_at(pos, "mmio.bit: index must be 0..63");
    return kc_bool((((uint64_t)one_int(a, 0, pos, "mmio.bit")) >> s) & 1u);
}
static uint64_t field_mask(int64_t lo, int64_t width, KcPos pos, const char *who) {
    if (lo < 0 || lo > 63 || width < 1 || width > 64 || lo + width > 64)
        kc_error_at(pos, "%s: need 0 <= lo <= 63, 1 <= width <= 64, lo + width <= 64", who);
    return width == 64 ? ~0ull : ((1ull << width) - 1);
}
static KcValue mm_extract(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; if (n != 3) kc_error_at(pos, "mmio.extract(value, lo, width) -> int");
    int64_t lo = one_int(a, 1, pos, "mmio.extract"), wd = one_int(a, 2, pos, "mmio.extract");
    uint64_t m = field_mask(lo, wd, pos, "mmio.extract");
    return to_kc(((uint64_t)one_int(a, 0, pos, "mmio.extract") >> lo) & m);
}
static KcValue mm_insert(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; if (n != 4) kc_error_at(pos, "mmio.insert(value, lo, width, field) -> int");
    int64_t lo = one_int(a, 1, pos, "mmio.insert"), wd = one_int(a, 2, pos, "mmio.insert");
    uint64_t m = field_mask(lo, wd, pos, "mmio.insert");
    uint64_t v = (uint64_t)one_int(a, 0, pos, "mmio.insert"), f = (uint64_t)one_int(a, 3, pos, "mmio.insert");
    if ((f & ~m) != 0) kc_error_at(pos, "mmio.insert: field value does not fit in %lld bits", (long long)wd);
    return to_kc((v & ~(m << lo)) | (f << lo));
}
static KcValue mm_hex(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    if (n < 1 || n > 2) kc_error_at(pos, "mmio.hex(value [, width_bytes]) -> string");
    uint64_t v = (uint64_t)one_int(a, 0, pos, "mmio.hex");
    char buf[24];
    if (n == 2) {
        int w = want_width(one_int(a, 1, pos, "mmio.hex"), pos, "mmio.hex");
        if (w < 8) v &= (1ull << (w * 8)) - 1;
        snprintf(buf, sizeof buf, "0x%0*llx", w * 2, (unsigned long long)v);
    } else snprintf(buf, sizeof buf, "0x%llx", (unsigned long long)v);
    return kc_string(buf);
}

/* ------------------------------- table ---------------------------- */

static KcModuleEntry ENTRIES[] = {
    {"map", mm_map}, {"map_pci", mm_map_pci}, {"map_uio", mm_map_uio}, {"map_file", mm_map_file},
    {"virtual", mm_virtual}, {"unmap", mm_unmap},
    {"size", mm_size}, {"base", mm_base}, {"label", mm_label}, {"source", mm_source},
    {"is_mapped", mm_is_mapped}, {"info", mm_info}, {"capabilities", mm_capabilities},
    {"last_error", mm_last_error},
    {"read8", mm_read8}, {"read16", mm_read16}, {"read32", mm_read32}, {"read64", mm_read64},
    {"write8", mm_write8}, {"write16", mm_write16}, {"write32", mm_write32}, {"write64", mm_write64},
    {"read", mm_read}, {"write", mm_write}, {"read_be", mm_read_be}, {"write_be", mm_write_be},
    {"set_bits", mm_set_bits}, {"clear_bits", mm_clear_bits}, {"update", mm_update},
    {"wait_bits", mm_wait_bits},
    {"read_block", mm_read_block}, {"write_block", mm_write_block}, {"fill", mm_fill},
    {"read_into", mm_read_into}, {"write_from", mm_write_from},
    {"barrier", mm_barrier}, {"port_in", mm_port_in}, {"port_out", mm_port_out},
    {"pci_list", mm_pci_list}, {"pci_info", mm_pci_info}, {"pci_bars", mm_pci_bars}, {"iomem", mm_iomem},
    {"trace", mm_trace}, {"trace_dump", mm_trace_dump}, {"trace_clear", mm_trace_clear},
    {"band", mm_band}, {"bor", mm_bor}, {"bxor", mm_bxor}, {"bnot", mm_bnot},
    {"shl", mm_shl}, {"shr", mm_shr}, {"bit", mm_bit}, {"extract", mm_extract}, {"insert", mm_insert},
    {"hex", mm_hex},
};

KcModule *kc_mod_mmio(void) {
    static KcModule m = { "mmio", ENTRIES, sizeof(ENTRIES)/sizeof(ENTRIES[0]) };
    return &m;
}
