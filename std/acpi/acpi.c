/* KingCobra std.acpi — native ACPI table access, parsing and platform
 * power/thermal state.
 *
 * What this module is:
 *   - A table loader with three interchangeable sources: the OS firmware
 *     table interface (Linux sysfs / Windows GetSystemFirmwareTable),
 *     raw physical memory (RSDP -> RSDT/XSDT walk, Linux, `unsafe`), and
 *     offline sources (dump files, byte arrays).
 *   - Validating parsers for the tables a program actually needs: the
 *     SDT header, FADT/FACP, MADT/APIC, MCFG, HPET, SRAT, SLIT, FACS,
 *     RSDT/XSDT, plus Generic Address Structure (GAS) decoding.
 *   - Read-only register access through GAS (system memory and x86
 *     I/O space), including the ACPI PM timer.
 *   - OS-reported AC/battery state, thermal zones and sleep states.
 *
 * What it deliberately is NOT: an AML interpreter. DSDT/SSDT are
 * exposed as validated raw tables, but no namespace is evaluated, and
 * nothing here writes ACPI registers (no sleep/reset/enable calls): a
 * stray write to PM1_CNT or RESET_REG powers the machine off.
 *
 * Every parser treats the table as hostile input: all offsets are
 * range-checked against the table length, repeating structures stop at
 * the first malformed record, and fields that postdate the table's
 * revision read as null rather than as out-of-bounds garbage.
 *
 * Error model matches std.mmio: misuse is fatal; environmental failure
 * (no such table, no permission) returns null and sets acpi.last_error().
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

#define KC_H_ACPI 0x4143 /* handle-kind tag for tables ('AC') */
#define MAX_TABLE_BYTES (64u << 20)
#define SDT_HEADER_LEN 36u

typedef struct {
    uint8_t *data;
    size_t   len;
    char     sig[5];
    char     origin[128];
    bool     freed;
} Table;

static char g_err[256];
static void set_err(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(g_err, sizeof g_err, fmt, ap);
    va_end(ap);
}
static KcValue ac_last_error(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; (void)a;
    if (n != 0) kc_error_at(pos, "acpi.last_error() -> string");
    return kc_string(g_err);
}
static void need_unsafe(KcInterp *ip, KcPos pos, const char *who) {
    if (ip->in_unsafe <= 0)
        kc_error_at(pos, "%s: raw hardware access requires an `unsafe { }` block", who);
}

/* ------------------------- little-endian reads ---------------------- */

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t le64(const uint8_t *p) { return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32); }
static KcValue u(uint64_t v) { return kc_int((int64_t)v); }

static uint8_t sum8(const uint8_t *p, size_t n) { uint8_t s = 0; while (n--) s = (uint8_t)(s + *p++); return s; }

/* Fixed-width ASCII field -> string: cut at NUL, trim trailing spaces,
 * replace anything unprintable. */
static KcValue ascii_field(const uint8_t *p, size_t n) {
    char buf[64]; size_t k = 0;
    for (size_t i = 0; i < n && i < sizeof buf - 1 && p[i]; i++) buf[k++] = (p[i] >= 0x20 && p[i] < 0x7F) ? (char)p[i] : '?';
    while (k && buf[k - 1] == ' ') k--;
    buf[k] = 0;
    return kc_string(buf);
}

/* ---------------------------- table objects ------------------------ */

static bool sig_ok(const uint8_t *p) {
    for (int i = 0; i < 4; i++) if (p[i] < 0x20 || p[i] > 0x7E) return false;
    return true;
}

/* Validate and copy. A checksum mismatch does NOT fail the load — a
 * firmware-debugging tool needs to be able to inspect a bad table —
 * it is reported by acpi.checksum_ok(). Structural damage does fail. */
static Table *table_from_buffer(const uint8_t *buf, size_t n, const char *origin) {
    if (n < SDT_HEADER_LEN) { set_err("%s: %zu bytes is too short for a 36-byte ACPI table header", origin, n); return NULL; }
    if (!sig_ok(buf)) { set_err("%s: signature is not 4 printable ASCII characters", origin); return NULL; }
    uint32_t len = le32(buf + 4);
    if (len < SDT_HEADER_LEN) { set_err("%s: header length field %u is smaller than the 36-byte header", origin, len); return NULL; }
    if (len > MAX_TABLE_BYTES) { set_err("%s: header length %u exceeds the %u-byte sanity limit", origin, len, MAX_TABLE_BYTES); return NULL; }
    if (len > n) { set_err("%s: header says %u bytes but only %zu are available (truncated?)", origin, len, n); return NULL; }
    Table *t = kc_malloc(sizeof *t);
    memset(t, 0, sizeof *t);
    t->data = kc_malloc(len);
    memcpy(t->data, buf, len);
    t->len = len;
    memcpy(t->sig, buf, 4); t->sig[4] = 0;
    snprintf(t->origin, sizeof t->origin, "%s", origin);
    return t;
}

static KcValue table_value(Table *t) {
    KcHandle *h = kc_malloc(sizeof(KcHandle));
    memset(h, 0, sizeof *h);
    h->kind = KC_H_ACPI;
    h->ptr = t;
    h->size = t->len;
    KcValue v; v.type = VAL_HANDLE; v.as.handle = h;
    return v;
}

static Table *get_table(KcValue v, KcPos pos, const char *who) {
    if (v.type != VAL_HANDLE || v.as.handle->kind != KC_H_ACPI || !v.as.handle->ptr)
        kc_error_at(pos, "%s: expected an ACPI table handle", who);
    Table *t = (Table *)v.as.handle->ptr;
    if (t->freed) kc_error_at(pos, "%s: use after acpi.free", who);
    return t;
}

/* Read a whole file in chunks (sysfs files report unreliable sizes). */
static uint8_t *read_all(const char *path, size_t *out_n, char *err, size_t errn) {
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err, errn, "%s: %s", path, strerror(errno)); return NULL; }
    size_t cap = 8192, n = 0;
    uint8_t *buf = kc_malloc(cap);
    for (;;) {
        if (n == cap) {
            if (cap >= MAX_TABLE_BYTES) { snprintf(err, errn, "%s: larger than the %u-byte limit", path, MAX_TABLE_BYTES); free(buf); fclose(f); return NULL; }
            cap *= 2; buf = kc_realloc(buf, cap);
        }
        size_t got = fread(buf + n, 1, cap - n, f);
        if (got == 0) break;
        n += got;
    }
    if (ferror(f)) { snprintf(err, errn, "%s: read error: %s", path, strerror(errno)); free(buf); fclose(f); return NULL; }
    fclose(f);
    *out_n = n;
    return buf;
}

/* ------------------------- OS firmware interface -------------------- */

static bool name_ok(const char *s) {
    size_t n = strlen(s);
    if (n == 0 || n > 16) return false;
    for (size_t i = 0; i < n; i++) if (!isalnum((unsigned char)s[i]) && s[i] != '_') return false;
    return true;
}

#if !defined(KC_HW_WINDOWS)
static const char *SYSFS_DIRS[] = {
    "/sys/firmware/acpi/tables/", "/sys/firmware/acpi/tables/data/", "/sys/firmware/acpi/tables/dynamic/",
};
#define SYSFS_DIR_COUNT 3
#else
#define WIN_ACPI_PROVIDER 0x41435049u /* 'ACPI' */
#endif

static Table *os_load(const char *name) {
    if (!name_ok(name)) { set_err("'%s' is not a valid table name", name); return NULL; }
#if defined(KC_HW_WINDOWS)
    if (strlen(name) != 4) { set_err("Windows exposes ACPI tables by 4-character signature only (got '%s')", name); return NULL; }
    DWORD id = (DWORD)(uint8_t)name[0] | ((DWORD)(uint8_t)name[1] << 8) | ((DWORD)(uint8_t)name[2] << 16) | ((DWORD)(uint8_t)name[3] << 24);
    UINT need = GetSystemFirmwareTable(WIN_ACPI_PROVIDER, id, NULL, 0);
    if (need == 0) { set_err("table '%s' not present (Windows error %lu)", name, (unsigned long)GetLastError()); return NULL; }
    uint8_t *buf = kc_malloc(need);
    UINT got = GetSystemFirmwareTable(WIN_ACPI_PROVIDER, id, buf, need);
    if (got == 0 || got > need) { set_err("GetSystemFirmwareTable('%s') failed", name); free(buf); return NULL; }
    Table *t = table_from_buffer(buf, got, "windows-api");
    free(buf);
    return t;
#else
    char last[200] = "";
    for (int d = 0; d < SYSFS_DIR_COUNT; d++) {
        char path[160]; snprintf(path, sizeof path, "%s%s", SYSFS_DIRS[d], name);
        size_t n; char err[200];
        uint8_t *buf = read_all(path, &n, err, sizeof err);
        if (!buf) { if (d == 0) snprintf(last, sizeof last, "%s", err); continue; }
        char origin[200]; snprintf(origin, sizeof origin, "sysfs:%s", path);
        Table *t = table_from_buffer(buf, n, origin);
        free(buf);
        return t;
    }
    set_err("%s", last[0] ? last : "table not found");
    return NULL;
#endif
}

static int cmp_str(const void *x, const void *y) { return strcmp(*(char *const *)x, *(char *const *)y); }

static KcValue ac_available(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; (void)a;
    if (n != 0) kc_error_at(pos, "acpi.available() -> bool");
#if defined(KC_HW_WINDOWS)
    return kc_bool(EnumSystemFirmwareTables(WIN_ACPI_PROVIDER, NULL, 0) > 0);
#else
    static const char *probe[] = { "FACP", "DSDT", "APIC", "MCFG" };
    for (size_t i = 0; i < sizeof probe / sizeof *probe; i++) {
        char p[96]; snprintf(p, sizeof p, "%s%s", SYSFS_DIRS[0], probe[i]);
        FILE *f = fopen(p, "rb");
        if (f) { fclose(f); return kc_bool(true); }
    }
    return kc_bool(false);
#endif
}

static KcValue ac_source(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; (void)a;
    if (n != 0) kc_error_at(pos, "acpi.source() -> string");
#if defined(KC_HW_WINDOWS)
    return kc_string(EnumSystemFirmwareTables(WIN_ACPI_PROVIDER, NULL, 0) > 0 ? "windows-api" : "none");
#else
    return kc_string(access("/sys/firmware/acpi/tables", R_OK) == 0 ? "sysfs" : "none");
#endif
}

static KcValue ac_list(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; (void)a;
    if (n != 0) kc_error_at(pos, "acpi.list() -> string[]");
    KcValue out = kc_array_new();
#if defined(KC_HW_WINDOWS)
    UINT need = EnumSystemFirmwareTables(WIN_ACPI_PROVIDER, NULL, 0);
    if (need == 0) { set_err("EnumSystemFirmwareTables failed (Windows error %lu)", (unsigned long)GetLastError()); return out; }
    uint8_t *buf = kc_malloc(need);
    UINT got = EnumSystemFirmwareTables(WIN_ACPI_PROVIDER, buf, need);
    char **names = kc_malloc(sizeof(char *) * (got / 4 + 1)); int cnt = 0;
    for (UINT i = 0; i + 4 <= got; i += 4) {
        char s[5]; memcpy(s, buf + i, 4); s[4] = 0;
        bool dup = false;
        for (int k = 0; k < cnt; k++) if (!strcmp(names[k], s)) dup = true;
        if (!dup) names[cnt++] = kc_strdup(s);
    }
    qsort(names, (size_t)cnt, sizeof(char *), cmp_str);
    for (int i = 0; i < cnt; i++) { kc_array_push(out, kc_string(names[i])); free(names[i]); }
    free(names); free(buf);
#else
    char **names = NULL; int cnt = 0, cap = 0;
    for (int d = 0; d < SYSFS_DIR_COUNT; d++) {
        DIR *dir = opendir(SYSFS_DIRS[d]);
        if (!dir) { if (d == 0) { set_err("%s: %s", SYSFS_DIRS[d], strerror(errno)); return out; } continue; }
        struct dirent *e;
        while ((e = readdir(dir))) {
            if (e->d_name[0] == '.' || !name_ok(e->d_name)) continue;
            char p[320]; snprintf(p, sizeof p, "%s%s", SYSFS_DIRS[d], e->d_name);
            struct stat st;
            if (stat(p, &st) != 0 || !S_ISREG(st.st_mode)) continue; /* skips the data/ and dynamic/ dirs */
            bool dup = false;
            for (int k = 0; k < cnt; k++) if (!strcmp(names[k], e->d_name)) dup = true;
            if (dup) continue;
            if (cnt == cap) { cap = cap ? cap * 2 : 32; names = kc_realloc(names, sizeof(char *) * (size_t)cap); }
            names[cnt++] = kc_strdup(e->d_name);
        }
        closedir(dir);
    }
    if (cnt) qsort(names, (size_t)cnt, sizeof(char *), cmp_str);
    for (int i = 0; i < cnt; i++) { kc_array_push(out, kc_string(names[i])); free(names[i]); }
    free(names);
#endif
    return out;
}

static KcValue ac_load(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    if (n != 1 || a[0].type != VAL_STRING) kc_error_at(pos, "acpi.load(name: string) -> table | null   (e.g. \"FACP\", \"APIC\", \"SSDT2\")");
    Table *t = os_load(a[0].as.s->data);
    return t ? table_value(t) : kc_null();
}

static KcValue ac_load_file(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    if (n != 1 || a[0].type != VAL_STRING) kc_error_at(pos, "acpi.load_file(path: string) -> table | null   (raw binary table, as written by acpidump -b)");
    size_t len; char err[200];
    uint8_t *buf = read_all(a[0].as.s->data, &len, err, sizeof err);
    if (!buf) { set_err("%s", err); return kc_null(); }
    char origin[128]; snprintf(origin, sizeof origin, "file:%.120s", a[0].as.s->data);
    Table *t = table_from_buffer(buf, len, origin);
    free(buf);
    return t ? table_value(t) : kc_null();
}

static KcValue ac_from_bytes(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    if (n != 1 || a[0].type != VAL_ARRAY) kc_error_at(pos, "acpi.from_bytes(bytes: int[]) -> table | null");
    KcArray *arr = a[0].as.arr;
    if (arr->count < 0 || (uint32_t)arr->count > MAX_TABLE_BYTES) kc_error_at(pos, "acpi.from_bytes: array too large");
    uint8_t *buf = kc_malloc((size_t)arr->count + 1);
    for (int i = 0; i < arr->count; i++) {
        if (arr->items[i].type != VAL_INT || arr->items[i].as.i < 0 || arr->items[i].as.i > 255) { free(buf); kc_error_at(pos, "acpi.from_bytes: element %d is not a byte (0..255)", i); }
        buf[i] = (uint8_t)arr->items[i].as.i;
    }
    Table *t = table_from_buffer(buf, (size_t)arr->count, "bytes");
    free(buf);
    return t ? table_value(t) : kc_null();
}

static KcValue ac_free(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    if (n != 1) kc_error_at(pos, "acpi.free(table) -> bool");
    if (a[0].type != VAL_HANDLE || a[0].as.handle->kind != KC_H_ACPI || !a[0].as.handle->ptr) kc_error_at(pos, "acpi.free: expected an ACPI table handle");
    Table *t = (Table *)a[0].as.handle->ptr;
    if (t->freed) return kc_bool(false);
    free(t->data); t->data = NULL; t->len = 0; t->freed = true;
    return kc_bool(true);
}

/* --------------------------- header accessors ----------------------- */

static KcValue ac_signature(KcInterp *ip, KcValue *a, int n, KcPos pos) { (void)ip; if (n != 1) kc_error_at(pos, "acpi.signature(table) -> string"); return kc_string(get_table(a[0], pos, "acpi.signature")->sig); }
static KcValue ac_origin(KcInterp *ip, KcValue *a, int n, KcPos pos)    { (void)ip; if (n != 1) kc_error_at(pos, "acpi.origin(table) -> string");    return kc_string(get_table(a[0], pos, "acpi.origin")->origin); }
static KcValue ac_length(KcInterp *ip, KcValue *a, int n, KcPos pos)    { (void)ip; if (n != 1) kc_error_at(pos, "acpi.length(table) -> int");      return kc_int((int64_t)get_table(a[0], pos, "acpi.length")->len); }
static KcValue ac_revision(KcInterp *ip, KcValue *a, int n, KcPos pos)  { (void)ip; if (n != 1) kc_error_at(pos, "acpi.revision(table) -> int");    return kc_int(get_table(a[0], pos, "acpi.revision")->data[8]); }
static KcValue ac_oem_id(KcInterp *ip, KcValue *a, int n, KcPos pos)    { (void)ip; if (n != 1) kc_error_at(pos, "acpi.oem_id(table) -> string");   return ascii_field(get_table(a[0], pos, "acpi.oem_id")->data + 10, 6); }
static KcValue ac_oem_table_id(KcInterp *ip, KcValue *a, int n, KcPos pos) { (void)ip; if (n != 1) kc_error_at(pos, "acpi.oem_table_id(table) -> string"); return ascii_field(get_table(a[0], pos, "acpi.oem_table_id")->data + 16, 8); }
static KcValue ac_oem_revision(KcInterp *ip, KcValue *a, int n, KcPos pos) { (void)ip; if (n != 1) kc_error_at(pos, "acpi.oem_revision(table) -> int"); return u(le32(get_table(a[0], pos, "acpi.oem_revision")->data + 24)); }
static KcValue ac_creator_id(KcInterp *ip, KcValue *a, int n, KcPos pos)   { (void)ip; if (n != 1) kc_error_at(pos, "acpi.creator_id(table) -> string");   return ascii_field(get_table(a[0], pos, "acpi.creator_id")->data + 28, 4); }
static KcValue ac_creator_revision(KcInterp *ip, KcValue *a, int n, KcPos pos) { (void)ip; if (n != 1) kc_error_at(pos, "acpi.creator_revision(table) -> int"); return u(le32(get_table(a[0], pos, "acpi.creator_revision")->data + 32)); }

/* true/false for ordinary SDTs; null for FACS, which has no checksum. */
static KcValue ac_checksum_ok(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; if (n != 1) kc_error_at(pos, "acpi.checksum_ok(table) -> bool | null");
    Table *t = get_table(a[0], pos, "acpi.checksum_ok");
    if (!memcmp(t->sig, "FACS", 4)) return kc_null();
    return kc_bool(sum8(t->data, t->len) == 0);
}

static KcValue ac_header(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; if (n != 1) kc_error_at(pos, "acpi.header(table) -> [sig, length, revision, checksum, oem_id, oem_table_id, oem_revision, creator_id, creator_revision]");
    Table *t = get_table(a[0], pos, "acpi.header");
    const uint8_t *d = t->data;
    KcValue o = kc_array_new();
    kc_array_push(o, kc_string(t->sig)); kc_array_push(o, u(le32(d + 4))); kc_array_push(o, kc_int(d[8])); kc_array_push(o, kc_int(d[9]));
    kc_array_push(o, ascii_field(d + 10, 6)); kc_array_push(o, ascii_field(d + 16, 8)); kc_array_push(o, u(le32(d + 24)));
    kc_array_push(o, ascii_field(d + 28, 4)); kc_array_push(o, u(le32(d + 32)));
    return o;
}

/* ------------------------------ raw access -------------------------- */

static bool in_range(const Table *t, int64_t off, uint64_t n) {
    return off >= 0 && (uint64_t)off <= t->len && n <= t->len - (uint64_t)off;
}

static KcValue raw_u(KcValue *a, int n, KcPos pos, int w, const char *who) {
    if (n != 2 || a[1].type != VAL_INT) kc_error_at(pos, "%s(table, offset: int) -> int", who);
    Table *t = get_table(a[0], pos, who);
    if (!in_range(t, a[1].as.i, (uint64_t)w)) kc_error_at(pos, "%s: %d-byte read at offset %lld is outside the %zu-byte table", who, w, (long long)a[1].as.i, t->len);
    const uint8_t *p = t->data + a[1].as.i;
    return u(w == 1 ? p[0] : w == 2 ? le16(p) : w == 4 ? le32(p) : le64(p));
}
static KcValue ac_u8(KcInterp *ip, KcValue *a, int n, KcPos pos)  { (void)ip; return raw_u(a, n, pos, 1, "acpi.u8"); }
static KcValue ac_u16(KcInterp *ip, KcValue *a, int n, KcPos pos) { (void)ip; return raw_u(a, n, pos, 2, "acpi.u16"); }
static KcValue ac_u32(KcInterp *ip, KcValue *a, int n, KcPos pos) { (void)ip; return raw_u(a, n, pos, 4, "acpi.u32"); }
static KcValue ac_u64(KcInterp *ip, KcValue *a, int n, KcPos pos) { (void)ip; return raw_u(a, n, pos, 8, "acpi.u64"); }

static KcValue ac_bytes(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    if (n != 3 || a[1].type != VAL_INT || a[2].type != VAL_INT) kc_error_at(pos, "acpi.bytes(table, offset: int, count: int) -> int[]");
    Table *t = get_table(a[0], pos, "acpi.bytes");
    if (a[2].as.i < 0 || a[2].as.i > (1 << 20)) kc_error_at(pos, "acpi.bytes: count must be 0..1048576");
    if (!in_range(t, a[1].as.i, (uint64_t)a[2].as.i)) kc_error_at(pos, "acpi.bytes: range [%lld, +%lld) is outside the %zu-byte table", (long long)a[1].as.i, (long long)a[2].as.i, t->len);
    KcValue o = kc_array_new();
    for (int64_t i = 0; i < a[2].as.i; i++) kc_array_push(o, kc_int(t->data[a[1].as.i + i]));
    return o;
}

static KcValue ac_str(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    if (n != 3 || a[1].type != VAL_INT || a[2].type != VAL_INT) kc_error_at(pos, "acpi.str(table, offset: int, count: int) -> string");
    Table *t = get_table(a[0], pos, "acpi.str");
    if (a[2].as.i < 0 || a[2].as.i > (1 << 20)) kc_error_at(pos, "acpi.str: count must be 0..1048576");
    if (!in_range(t, a[1].as.i, (uint64_t)a[2].as.i)) kc_error_at(pos, "acpi.str: range is outside the %zu-byte table", t->len);
    size_t cnt = (size_t)a[2].as.i;
    char *buf = kc_malloc(cnt + 1); size_t k = 0;
    for (size_t i = 0; i < cnt && t->data[a[1].as.i + (int64_t)i]; i++) { uint8_t c = t->data[a[1].as.i + (int64_t)i]; buf[k++] = (c >= 0x20 && c < 0x7F) ? (char)c : '?'; }
    buf[k] = 0;
    return kc_string_take(buf, (int)k);
}

static KcValue ac_save(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    if (n != 2 || a[1].type != VAL_STRING) kc_error_at(pos, "acpi.save(table, path: string) -> bool   (raw binary, same as acpidump -b)");
    Table *t = get_table(a[0], pos, "acpi.save");
    FILE *f = fopen(a[1].as.s->data, "wb");
    if (!f) { set_err("%s: %s", a[1].as.s->data, strerror(errno)); return kc_bool(false); }
    bool ok = fwrite(t->data, 1, t->len, f) == t->len;
    if (fclose(f) != 0) ok = false;
    if (!ok) set_err("%s: write failed", a[1].as.s->data);
    return kc_bool(ok);
}

/* --------------------- data-driven named fields -------------------- */

enum { F_INT, F_GAS };
typedef struct { const char *name; uint16_t off; uint8_t size; uint8_t kind; } Field;

static const Field FADT_FIELDS[] = {
    {"firmware_ctrl",36,4,F_INT},{"dsdt",40,4,F_INT},{"preferred_pm_profile",45,1,F_INT},{"sci_int",46,2,F_INT},
    {"smi_cmd",48,4,F_INT},{"acpi_enable",52,1,F_INT},{"acpi_disable",53,1,F_INT},{"s4bios_req",54,1,F_INT},
    {"pstate_cnt",55,1,F_INT},{"pm1a_evt_blk",56,4,F_INT},{"pm1b_evt_blk",60,4,F_INT},{"pm1a_cnt_blk",64,4,F_INT},
    {"pm1b_cnt_blk",68,4,F_INT},{"pm2_cnt_blk",72,4,F_INT},{"pm_tmr_blk",76,4,F_INT},{"gpe0_blk",80,4,F_INT},
    {"gpe1_blk",84,4,F_INT},{"pm1_evt_len",88,1,F_INT},{"pm1_cnt_len",89,1,F_INT},{"pm2_cnt_len",90,1,F_INT},
    {"pm_tmr_len",91,1,F_INT},{"gpe0_blk_len",92,1,F_INT},{"gpe1_blk_len",93,1,F_INT},{"gpe1_base",94,1,F_INT},
    {"cst_cnt",95,1,F_INT},{"p_lvl2_lat",96,2,F_INT},{"p_lvl3_lat",98,2,F_INT},{"flush_size",100,2,F_INT},
    {"flush_stride",102,2,F_INT},{"duty_offset",104,1,F_INT},{"duty_width",105,1,F_INT},{"day_alrm",106,1,F_INT},
    {"mon_alrm",107,1,F_INT},{"century",108,1,F_INT},{"iapc_boot_arch",109,2,F_INT},{"flags",112,4,F_INT},
    {"reset_reg",116,12,F_GAS},{"reset_value",128,1,F_INT},{"arm_boot_arch",129,2,F_INT},{"fadt_minor_version",131,1,F_INT},
    {"x_firmware_ctrl",132,8,F_INT},{"x_dsdt",140,8,F_INT},
    {"x_pm1a_evt_blk",148,12,F_GAS},{"x_pm1b_evt_blk",160,12,F_GAS},{"x_pm1a_cnt_blk",172,12,F_GAS},
    {"x_pm1b_cnt_blk",184,12,F_GAS},{"x_pm2_cnt_blk",196,12,F_GAS},{"x_pm_tmr_blk",208,12,F_GAS},
    {"x_gpe0_blk",220,12,F_GAS},{"x_gpe1_blk",232,12,F_GAS},{"sleep_control_reg",244,12,F_GAS},
    {"sleep_status_reg",256,12,F_GAS},{"hypervisor_vendor_id",268,8,F_INT},
};
static const Field HPET_FIELDS[] = {
    {"event_timer_block_id",36,4,F_INT},{"base_address",40,12,F_GAS},{"hpet_number",52,1,F_INT},
    {"min_clock_tick",53,2,F_INT},{"page_protection",55,1,F_INT},
};
static const Field MADT_FIELDS[] = { {"lapic_address",36,4,F_INT},{"flags",40,4,F_INT} };
static const Field FACS_FIELDS[] = {
    {"hardware_signature",8,4,F_INT},{"firmware_waking_vector",12,4,F_INT},{"global_lock",16,4,F_INT},
    {"flags",20,4,F_INT},{"x_firmware_waking_vector",24,8,F_INT},{"version",32,1,F_INT},
};
static const Field SLIT_FIELDS[] = { {"localities",36,8,F_INT} };

typedef struct { const char *sig; const Field *f; size_t n; } FieldSet;
static const FieldSet FIELD_SETS[] = {
    { "FACP", FADT_FIELDS, sizeof FADT_FIELDS / sizeof *FADT_FIELDS },
    { "HPET", HPET_FIELDS, sizeof HPET_FIELDS / sizeof *HPET_FIELDS },
    { "APIC", MADT_FIELDS, sizeof MADT_FIELDS / sizeof *MADT_FIELDS },
    { "FACS", FACS_FIELDS, sizeof FACS_FIELDS / sizeof *FACS_FIELDS },
    { "SLIT", SLIT_FIELDS, sizeof SLIT_FIELDS / sizeof *SLIT_FIELDS },
};

static const FieldSet *field_set(const Table *t) {
    for (size_t i = 0; i < sizeof FIELD_SETS / sizeof *FIELD_SETS; i++) if (!memcmp(t->sig, FIELD_SETS[i].sig, 4)) return &FIELD_SETS[i];
    return NULL;
}
static bool field_present(const Table *t, const Field *f) { return (size_t)f->off + f->size <= t->len; }

static const char *gas_space_name(unsigned id) {
    switch (id) {
    case 0: return "memory"; case 1: return "io"; case 2: return "pci_config";
    case 3: return "embedded_controller"; case 4: return "smbus"; case 5: return "cmos";
    case 6: return "pci_bar_target"; case 7: return "ipmi"; case 8: return "gpio";
    case 9: return "generic_serial_bus"; case 10: return "pcc"; case 0x7F: return "functional_fixed_hw";
    default: return "unknown";
    }
}

static KcValue gas_value(const uint8_t *p) {
    KcValue o = kc_array_new();
    kc_array_push(o, kc_int(p[0])); kc_array_push(o, kc_string(gas_space_name(p[0])));
    kc_array_push(o, kc_int(p[1])); kc_array_push(o, kc_int(p[2])); kc_array_push(o, kc_int(p[3]));
    kc_array_push(o, u(le64(p + 4)));
    return o;
}

static const char *pm_profile_name(unsigned v) {
    static const char *n[] = { "unspecified", "desktop", "mobile", "workstation", "enterprise_server",
                               "soho_server", "appliance_pc", "performance_server", "tablet" };
    return v < sizeof n / sizeof *n ? n[v] : "reserved";
}

static const Field *find_field(const Table *t, const char *name) {
    const FieldSet *fs = field_set(t);
    if (!fs) return NULL;
    for (size_t i = 0; i < fs->n; i++) if (!strcmp(fs->f[i].name, name)) return &fs->f[i];
    return NULL;
}

static KcValue ac_get(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    if (n != 2 || a[1].type != VAL_STRING) kc_error_at(pos, "acpi.get(table, field: string) -> int | string | null");
    Table *t = get_table(a[0], pos, "acpi.get");
    const char *name = a[1].as.s->data;
    const Field *f = find_field(t, name);
    if (f) {
        if (f->kind == F_GAS) kc_error_at(pos, "acpi.get: '%s' is an address structure; use acpi.gas(table, \"%s\")", name, name);
        if (!field_present(t, f)) return kc_null(); /* postdates this table's revision */
        const uint8_t *p = t->data + f->off;
        return u(f->size == 1 ? p[0] : f->size == 2 ? le16(p) : f->size == 4 ? le32(p) : le64(p));
    }
    /* derived (pseudo) fields */
    if (!memcmp(t->sig, "FACP", 4) && !strcmp(name, "pm_profile_name") && t->len > 45) return kc_string(pm_profile_name(t->data[45]));
    if (!memcmp(t->sig, "HPET", 4) && t->len >= 40) {
        uint32_t id = le32(t->data + 36);
        if (!strcmp(name, "vendor_id")) return kc_int((id >> 16) & 0xFFFF);
        if (!strcmp(name, "comparators")) return kc_int(((id >> 8) & 0x1F) + 1);
        if (!strcmp(name, "counter_64bit")) return kc_bool((id >> 13) & 1);
        if (!strcmp(name, "legacy_replacement")) return kc_bool((id >> 15) & 1);
    }
    return kc_null();
}

static KcValue ac_fields(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    if (n != 1) kc_error_at(pos, "acpi.fields(table) -> string[]   (fields present in THIS table instance)");
    Table *t = get_table(a[0], pos, "acpi.fields");
    KcValue o = kc_array_new();
    const FieldSet *fs = field_set(t);
    if (fs) for (size_t i = 0; i < fs->n; i++) if (field_present(t, &fs->f[i])) kc_array_push(o, kc_string(fs->f[i].name));
    if (!memcmp(t->sig, "FACP", 4) && t->len > 45) kc_array_push(o, kc_string("pm_profile_name"));
    if (!memcmp(t->sig, "HPET", 4) && t->len >= 40) {
        kc_array_push(o, kc_string("vendor_id")); kc_array_push(o, kc_string("comparators"));
        kc_array_push(o, kc_string("counter_64bit")); kc_array_push(o, kc_string("legacy_replacement"));
    }
    return o;
}

static KcValue ac_gas(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    if (n != 2 || a[1].type != VAL_STRING) kc_error_at(pos, "acpi.gas(table, field: string) -> [space_id, space, bit_width, bit_offset, access_size, address] | null");
    Table *t = get_table(a[0], pos, "acpi.gas");
    const Field *f = find_field(t, a[1].as.s->data);
    if (!f || f->kind != F_GAS) return kc_null();
    if (!field_present(t, f)) return kc_null();
    return gas_value(t->data + f->off);
}

static KcValue ac_flags(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    if (n != 1) kc_error_at(pos, "acpi.flags(table) -> string[] | null   (decoded names of set flag bits; FACP and APIC)");
    Table *t = get_table(a[0], pos, "acpi.flags");
    static const char *fadt[] = { "WBINVD","WBINVD_FLUSH","PROC_C1","P_LVL2_UP","PWR_BUTTON","SLP_BUTTON","FIX_RTC","RTC_S4",
        "TMR_VAL_EXT","DCK_CAP","RESET_REG_SUP","SEALED_CASE","HEADLESS","CPU_SW_SLP","PCI_EXP_WAK","USE_PLATFORM_CLOCK",
        "S4_RTC_STS_VALID","REMOTE_POWER_ON_CAPABLE","FORCE_APIC_CLUSTER_MODEL","FORCE_APIC_PHYSICAL_DESTINATION_MODE",
        "HW_REDUCED_ACPI","LOW_POWER_S0_IDLE_CAPABLE","PERSISTENT_CPU_CACHES_0","PERSISTENT_CPU_CACHES_1" };
    uint32_t v; const char **names; size_t cnt;
    if (!memcmp(t->sig, "FACP", 4) && t->len >= 116) { v = le32(t->data + 112); names = fadt; cnt = sizeof fadt / sizeof *fadt; }
    else if (!memcmp(t->sig, "APIC", 4) && t->len >= 44) { static const char *m[] = { "PCAT_COMPAT" }; v = le32(t->data + 40); names = m; cnt = 1; }
    else return kc_null();
    KcValue o = kc_array_new();
    for (size_t i = 0; i < cnt; i++) if ((v >> i) & 1u) kc_array_push(o, kc_string(names[i]));
    return o;
}

/* ------------------------- repeating structures --------------------- */

static KcValue row(const char *kind) { KcValue r = kc_array_new(); kc_array_push(r, kc_string(kind)); return r; }

/* [kind, ...fields]. Layouts are documented in docs/STD_ACPI.md. Unknown
 * record types come back as ["unknown", type, length]; a record that
 * overruns the table or is shorter than its type requires ends the walk
 * with ["malformed", type, length, offset]. */
static KcValue madt_entries(const Table *t) {
    KcValue out = kc_array_new();
    const uint8_t *d = t->data; size_t off = 44;
    while (off + 2 <= t->len) {
        unsigned type = d[off], len = d[off + 1];
        if (len < 2 || off + len > t->len) {
            KcValue r = row("malformed"); kc_array_push(r, kc_int(type)); kc_array_push(r, kc_int(len)); kc_array_push(r, kc_int((int64_t)off));
            kc_array_push(out, r); break;
        }
        const uint8_t *p = d + off; KcValue r; unsigned need = 0;
        switch (type) {
        case 0x0: need = 8;  break; case 0x1: need = 12; break; case 0x2: need = 10; break; case 0x3: need = 8; break;
        case 0x4: need = 6;  break; case 0x5: need = 12; break; case 0x9: need = 16; break; case 0xA: need = 12; break;
        case 0xB: need = 76; break; case 0xC: need = 24; break; default: need = 2; break;
        }
        if (len < need) {
            r = row("malformed"); kc_array_push(r, kc_int(type)); kc_array_push(r, kc_int(len)); kc_array_push(r, kc_int((int64_t)off));
            kc_array_push(out, r); break;
        }
        switch (type) {
        case 0x0: r = row("lapic");          kc_array_push(r, kc_int(p[2])); kc_array_push(r, kc_int(p[3])); kc_array_push(r, u(le32(p + 4))); break;
        case 0x1: r = row("ioapic");         kc_array_push(r, kc_int(p[2])); kc_array_push(r, u(le32(p + 4))); kc_array_push(r, u(le32(p + 8))); break;
        case 0x2: r = row("iso");            kc_array_push(r, kc_int(p[2])); kc_array_push(r, kc_int(p[3])); kc_array_push(r, u(le32(p + 4))); kc_array_push(r, kc_int(le16(p + 8))); break;
        case 0x3: r = row("nmi_source");     kc_array_push(r, kc_int(le16(p + 2))); kc_array_push(r, u(le32(p + 4))); break;
        case 0x4: r = row("lapic_nmi");      kc_array_push(r, kc_int(p[2])); kc_array_push(r, kc_int(le16(p + 3))); kc_array_push(r, kc_int(p[5])); break;
        case 0x5: r = row("lapic_override"); kc_array_push(r, u(le64(p + 4))); break;
        case 0x9: r = row("x2apic");         kc_array_push(r, u(le32(p + 4))); kc_array_push(r, u(le32(p + 8))); kc_array_push(r, u(le32(p + 12))); break;
        case 0xA: r = row("x2apic_nmi");     kc_array_push(r, kc_int(le16(p + 2))); kc_array_push(r, u(le32(p + 4))); kc_array_push(r, kc_int(p[8])); break;
        case 0xB: r = row("gicc");           kc_array_push(r, u(le32(p + 4))); kc_array_push(r, u(le32(p + 8))); kc_array_push(r, u(le32(p + 12))); kc_array_push(r, u(le64(p + 68))); break;
        case 0xC: r = row("gicd");           kc_array_push(r, u(le32(p + 4))); kc_array_push(r, u(le64(p + 8))); kc_array_push(r, u(le32(p + 16))); kc_array_push(r, kc_int(p[20])); break;
        default:  r = row("unknown");        kc_array_push(r, kc_int(type)); kc_array_push(r, kc_int(len)); break;
        }
        kc_array_push(out, r);
        off += len;
    }
    return out;
}

static KcValue mcfg_entries(const Table *t) {
    KcValue out = kc_array_new();
    for (size_t off = 44; off + 16 <= t->len; off += 16) {
        const uint8_t *p = t->data + off; KcValue r = kc_array_new();
        kc_array_push(r, u(le64(p))); kc_array_push(r, kc_int(le16(p + 8))); kc_array_push(r, kc_int(p[10])); kc_array_push(r, kc_int(p[11]));
        kc_array_push(out, r);
    }
    return out;
}

static KcValue srat_entries(const Table *t) {
    KcValue out = kc_array_new();
    const uint8_t *d = t->data; size_t off = 48;
    while (off + 2 <= t->len) {
        unsigned type = d[off], len = d[off + 1];
        unsigned need = type == 0 ? 16 : type == 1 ? 40 : type == 2 ? 24 : 2;
        if (len < 2 || off + len > t->len || len < need) {
            KcValue r = row("malformed"); kc_array_push(r, kc_int(type)); kc_array_push(r, kc_int(len)); kc_array_push(r, kc_int((int64_t)off));
            kc_array_push(out, r); break;
        }
        const uint8_t *p = d + off; KcValue r;
        if (type == 0) { /* proximity domain = hi[3] . lo, per ACPI 6.x */
            uint32_t dom = (uint32_t)p[2] | ((uint32_t)p[9] << 8) | ((uint32_t)p[10] << 16) | ((uint32_t)p[11] << 24);
            r = row("lapic_affinity"); kc_array_push(r, u(dom)); kc_array_push(r, kc_int(p[3])); kc_array_push(r, u(le32(p + 4)));
        } else if (type == 1) {
            r = row("memory_affinity"); kc_array_push(r, u(le32(p + 2))); kc_array_push(r, u(le64(p + 8))); kc_array_push(r, u(le64(p + 16))); kc_array_push(r, u(le32(p + 28)));
        } else if (type == 2) {
            r = row("x2apic_affinity"); kc_array_push(r, u(le32(p + 4))); kc_array_push(r, u(le32(p + 8))); kc_array_push(r, u(le32(p + 12)));
        } else { r = row("unknown"); kc_array_push(r, kc_int(type)); kc_array_push(r, kc_int(len)); }
        kc_array_push(out, r);
        off += len;
    }
    return out;
}

static KcValue slit_rows(const Table *t) {
    KcValue out = kc_array_new();
    if (t->len < 44) return out;
    uint64_t nloc = le64(t->data + 36);
    /* 36-byte header + 8-byte locality count + N*N distance bytes */
    if (nloc == 0 || nloc > 1024 || nloc * nloc > t->len - 44) {
        KcValue r = row("malformed"); kc_array_push(r, u(nloc)); kc_array_push(out, r); return out;
    }
    for (uint64_t i = 0; i < nloc; i++) {
        KcValue r = kc_array_new();
        for (uint64_t j = 0; j < nloc; j++) kc_array_push(r, kc_int(t->data[44 + i * nloc + j]));
        kc_array_push(out, r);
    }
    return out;
}

static KcValue sdt_pointers(const Table *t, int w) {
    KcValue out = kc_array_new();
    for (size_t off = SDT_HEADER_LEN; off + (size_t)w <= t->len; off += (size_t)w)
        kc_array_push(out, u(w == 4 ? le32(t->data + off) : le64(t->data + off)));
    return out;
}

static KcValue ac_entries(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    if (n != 1) kc_error_at(pos, "acpi.entries(table) -> array | null   (APIC, MCFG, SRAT, SLIT, RSDT, XSDT)");
    Table *t = get_table(a[0], pos, "acpi.entries");
    if (!memcmp(t->sig, "APIC", 4) && t->len >= 44) return madt_entries(t);
    if (!memcmp(t->sig, "MCFG", 4) && t->len >= 44) return mcfg_entries(t);
    if (!memcmp(t->sig, "SRAT", 4) && t->len >= 48) return srat_entries(t);
    if (!memcmp(t->sig, "SLIT", 4)) return slit_rows(t);
    if (!memcmp(t->sig, "RSDT", 4)) return sdt_pointers(t, 4);
    if (!memcmp(t->sig, "XSDT", 4)) return sdt_pointers(t, 8);
    return kc_null();
}

/* ----------------------- RSDP and physical walk --------------------- */

typedef struct { uint8_t revision; char oem[7]; uint32_t rsdt; uint64_t xsdt; uint32_t length; } Rsdp;

/* Pure validator over a candidate RSDP: signature, v1 checksum (first
 * 20 bytes), and for revision >= 2 the length field and extended
 * checksum. */
static bool rsdp_parse(const uint8_t *p, size_t avail, Rsdp *out) {
    if (avail < 20 || memcmp(p, "RSD PTR ", 8) != 0) return false;
    if (sum8(p, 20) != 0) return false;
    memset(out, 0, sizeof *out);
    out->revision = p[15];
    memcpy(out->oem, p + 9, 6); out->oem[6] = 0;
    out->rsdt = le32(p + 16);
    out->length = 20;
    if (out->revision >= 2) {
        if (avail < 36) return false;
        uint32_t len = le32(p + 20);
        if (len < 36 || len > avail || sum8(p, len) != 0) return false;
        out->length = len; out->xsdt = le64(p + 24);
    }
    return true;
}

#if !defined(KC_HW_WINDOWS)
static int phys_read(uint64_t addr, size_t len, uint8_t *dst, char *err, size_t errn) {
    KcHwMap m;
    if (kc_hw_map_phys(&m, addr, len, false, err, errn) != 0) return -1;
    for (size_t i = 0; i < len; i++) dst[i] = m.ptr[i];
    kc_hw_unmap(&m);
    return 0;
}

static bool efi_rsdp_hint(uint64_t *addr) {
    FILE *f = fopen("/sys/firmware/efi/systab", "r");
    if (!f) return false;
    char line[128]; uint64_t v2 = 0, v1 = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long long v;
        if (sscanf(line, "ACPI20=%llx", &v) == 1) v2 = v;
        else if (sscanf(line, "ACPI=%llx", &v) == 1) v1 = v;
    }
    fclose(f);
    *addr = v2 ? v2 : v1;
    return *addr != 0;
}

static bool scan_for_rsdp(uint64_t base, size_t len, uint64_t *found, Rsdp *r, char *err, size_t errn) {
    uint8_t *buf = kc_malloc(len);
    bool ok = false;
    if (phys_read(base, len, buf, err, errn) == 0)
        for (size_t o = 0; o + 20 <= len; o += 16)
            if (rsdp_parse(buf + o, len - o, r)) { *found = base + o; ok = true; break; }
    free(buf);
    return ok;
}

static bool find_rsdp(uint64_t *addr, Rsdp *r, char *err, size_t errn) {
    uint8_t buf[64];
    uint64_t hint;
    if (efi_rsdp_hint(&hint) && phys_read(hint, sizeof buf, buf, err, errn) == 0 && rsdp_parse(buf, sizeof buf, r)) { *addr = hint; return true; }
    uint8_t w[2];
    if (phys_read(0x40E, 2, w, err, errn) == 0) { /* EBDA segment pointer */
        uint64_t ebda = (uint64_t)le16(w) << 4;
        if (ebda >= 0x80000 && ebda < 0xA0000 && scan_for_rsdp(ebda, 1024, addr, r, err, errn)) return true;
    }
    if (scan_for_rsdp(0xE0000, 0x20000, addr, r, err, errn)) return true;
    if (!err[0]) snprintf(err, errn, "RSDP not found (EFI hint, EBDA and BIOS area all checked)");
    return false;
}

static Table *load_phys_table(uint64_t addr, const char *label) {
    char err[200] = "";
    uint8_t hdr[SDT_HEADER_LEN];
    if (phys_read(addr, sizeof hdr, hdr, err, sizeof err) != 0) { set_err("%s", err); return NULL; }
    uint32_t len = le32(hdr + 4);
    if (len < SDT_HEADER_LEN || len > MAX_TABLE_BYTES) { set_err("%s @0x%llx: implausible length %u", label, (unsigned long long)addr, len); return NULL; }
    uint8_t *buf = kc_malloc(len);
    if (phys_read(addr, len, buf, err, sizeof err) != 0) { set_err("%s", err); free(buf); return NULL; }
    char origin[96]; snprintf(origin, sizeof origin, "phys:0x%llx", (unsigned long long)addr);
    Table *t = table_from_buffer(buf, len, origin);
    free(buf);
    return t;
}
#endif

static KcValue ac_rsdp(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)a;
    need_unsafe(ip, pos, "acpi.rsdp");
    if (n != 0) kc_error_at(pos, "acpi.rsdp() -> [address, revision, oem_id, rsdt, xsdt, length] | null");
#if defined(KC_HW_WINDOWS)
    set_err("raw RSDP discovery needs physical memory access, unavailable on Windows; use acpi.load(\"RSDT\"/\"XSDT\")");
    return kc_null();
#else
    uint64_t addr; Rsdp r; char err[200] = "";
    if (!find_rsdp(&addr, &r, err, sizeof err)) { set_err("%s", err); return kc_null(); }
    KcValue o = kc_array_new();
    kc_array_push(o, u(addr)); kc_array_push(o, kc_int(r.revision)); kc_array_push(o, kc_string(r.oem));
    kc_array_push(o, u(r.rsdt)); kc_array_push(o, u(r.xsdt)); kc_array_push(o, u(r.length));
    return o;
#endif
}

/* [[signature, address, length, checksum_ok], ...] by walking the XSDT
 * (or the RSDT on revision < 2), then adding the FADT's DSDT and FACS. */
static KcValue ac_scan_phys(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)a;
    need_unsafe(ip, pos, "acpi.scan_phys");
    if (n != 0) kc_error_at(pos, "acpi.scan_phys() -> [[signature, address, length, checksum_ok], ...] | null");
#if defined(KC_HW_WINDOWS)
    set_err("physical table walk is unavailable on Windows; use acpi.list()/acpi.load()");
    return kc_null();
#else
    uint64_t raddr; Rsdp r; char err[200] = "";
    if (!find_rsdp(&raddr, &r, err, sizeof err)) { set_err("%s", err); return kc_null(); }
    bool use_x = r.revision >= 2 && r.xsdt != 0;
    Table *root = load_phys_table(use_x ? r.xsdt : r.rsdt, use_x ? "XSDT" : "RSDT");
    if (!root) return kc_null();
    size_t w = use_x ? 8 : 4, count = (root->len - SDT_HEADER_LEN) / w;
    if (count > 4096) count = 4096;
    KcValue out = kc_array_new();
    uint64_t extra[2] = { 0, 0 }; /* DSDT, FACS from the FADT */
    for (size_t i = 0; i < count; i++) {
        uint64_t addr = w == 8 ? le64(root->data + SDT_HEADER_LEN + i * w) : le32(root->data + SDT_HEADER_LEN + i * w);
        if (!addr) continue;
        Table *t = load_phys_table(addr, "table");
        if (!t) continue;
        KcValue rowv = kc_array_new();
        kc_array_push(rowv, kc_string(t->sig)); kc_array_push(rowv, u(addr)); kc_array_push(rowv, u(t->len)); kc_array_push(rowv, kc_bool(sum8(t->data, t->len) == 0));
        kc_array_push(out, rowv);
        if (!memcmp(t->sig, "FACP", 4)) {
            if (t->len >= 148 && le64(t->data + 140)) extra[0] = le64(t->data + 140); else if (t->len >= 44) extra[0] = le32(t->data + 40);
            if (t->len >= 140 && le64(t->data + 132)) extra[1] = le64(t->data + 132); else if (t->len >= 40) extra[1] = le32(t->data + 36);
        }
        free(t->data); free(t);
    }
    for (int k = 0; k < 2; k++) {
        if (!extra[k]) continue;
        Table *t = load_phys_table(extra[k], k ? "FACS" : "DSDT");
        if (!t) continue;
        KcValue rowv = kc_array_new();
        kc_array_push(rowv, kc_string(t->sig)); kc_array_push(rowv, u(extra[k])); kc_array_push(rowv, u(t->len));
        kc_array_push(rowv, memcmp(t->sig, "FACS", 4) ? kc_bool(sum8(t->data, t->len) == 0) : kc_null());
        kc_array_push(out, rowv);
        free(t->data); free(t);
    }
    free(root->data); free(root);
    return out;
#endif
}

static KcValue ac_load_phys(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need_unsafe(ip, pos, "acpi.load_phys");
    if (n != 1 || a[0].type != VAL_INT || a[0].as.i <= 0) kc_error_at(pos, "acpi.load_phys(address: int) -> table | null");
#if defined(KC_HW_WINDOWS)
    set_err("physical table loading is unavailable on Windows; use acpi.load()");
    return kc_null();
#else
    Table *t = load_phys_table((uint64_t)a[0].as.i, "table");
    return t ? table_value(t) : kc_null();
#endif
}

/* --------------------------- GAS register reads --------------------- */

static int gas_width_bytes(unsigned access_size, unsigned bit_off, unsigned bit_w) {
    unsigned total = bit_off + bit_w;
    if (access_size >= 1 && access_size <= 4) return 1 << (access_size - 1);
    return total <= 8 ? 1 : total <= 16 ? 2 : total <= 32 ? 4 : 8;
}

/* Read the register a GAS describes (system memory or x86 I/O space)
 * and extract its bit field. Read-only by design. */
static bool gas_read_raw(unsigned space, unsigned bit_w, unsigned bit_off, unsigned access, uint64_t addr, uint64_t *out, char *err, size_t errn) {
    if (bit_w == 0 || bit_w > 64 || bit_off + bit_w > 64) { snprintf(err, errn, "unsupported GAS bit field (width %u, offset %u)", bit_w, bit_off); return false; }
    int w = gas_width_bytes(access, bit_off, bit_w);
    if ((unsigned)w * 8u < bit_off + bit_w) { snprintf(err, errn, "GAS field spans multiple accesses; not supported"); return false; }
#if !KC_HW_HAS_64BIT_ACCESS
    if (w == 8) { snprintf(err, errn, "64-bit GAS access needs a 64-bit host"); return false; }
#endif
    uint64_t raw = 0;
    if (space == 0) {
        if (addr & (uint64_t)(w - 1)) { snprintf(err, errn, "misaligned GAS memory address 0x%llx", (unsigned long long)addr); return false; }
        KcHwMap m;
        if (kc_hw_map_phys(&m, addr, (size_t)w, false, err, errn) != 0) return false;
        raw = kc_hw_rd(m.ptr, w); kc_hw_post_read();
        kc_hw_unmap(&m);
    } else if (space == 1) {
        if (addr + (uint64_t)w > 0x10000u) { snprintf(err, errn, "I/O port 0x%llx out of range", (unsigned long long)addr); return false; }
        if (w > 4) { snprintf(err, errn, "64-bit I/O port access is not possible"); return false; }
        if (kc_hw_port_access((uint16_t)addr, w, err, errn) != 0) return false;
        raw = kc_hw_port_in((uint16_t)addr, w);
    } else {
        snprintf(err, errn, "address space '%s' (%u) is not readable from user space here", gas_space_name(space), space);
        return false;
    }
    raw >>= bit_off;
    if (bit_w < 64) raw &= (1ull << bit_w) - 1;
    *out = raw;
    return true;
}

static KcValue ac_gas_read(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need_unsafe(ip, pos, "acpi.gas_read");
    const char *u_ = "acpi.gas_read(gas: array from acpi.gas()) -> int | null";
    if (n != 1 || a[0].type != VAL_ARRAY || a[0].as.arr->count != 6) kc_error_at(pos, "%s", u_);
    KcArray *g = a[0].as.arr;
    for (int i = 0; i < 6; i++) if (i != 1 && g->items[i].type != VAL_INT) kc_error_at(pos, "%s", u_);
    if (g->items[0].as.i < 0 || g->items[2].as.i < 0 || g->items[3].as.i < 0 || g->items[4].as.i < 0) kc_error_at(pos, "%s", u_);
    char err[200] = ""; uint64_t v = 0;
    if (!gas_read_raw((unsigned)g->items[0].as.i, (unsigned)g->items[2].as.i, (unsigned)g->items[3].as.i, (unsigned)g->items[4].as.i, (uint64_t)g->items[5].as.i, &v, err, sizeof err)) { set_err("%s", err); return kc_null(); }
    return u(v);
}

#define PM_TIMER_HZ 3579545
static KcValue ac_pm_timer_hz(KcInterp *ip, KcValue *a, int n, KcPos pos) { (void)ip; (void)a; if (n != 0) kc_error_at(pos, "acpi.pm_timer_hz() -> int"); return kc_int(PM_TIMER_HZ); }

/* Current ACPI PM timer count (24-bit, or 32-bit when the FADT sets
 * TMR_VAL_EXT), ticking at 3.579545 MHz. Source: the FADT's X_PM_TMR_BLK
 * if present and non-zero, else the legacy PM_TMR_BLK I/O port. */
static KcValue ac_pm_timer(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)a;
    need_unsafe(ip, pos, "acpi.pm_timer");
    if (n != 0) kc_error_at(pos, "acpi.pm_timer() -> int | null   (ticks at acpi.pm_timer_hz())");
    Table *t = os_load("FACP");
    if (!t) return kc_null();
    unsigned space = 1, access = 3; uint64_t addr = 0;
    bool wide = t->len >= 116 && ((le32(t->data + 112) >> 8) & 1u);
    if (t->len >= 220 && le64(t->data + 212) != 0) { space = t->data[208]; access = t->data[211]; addr = le64(t->data + 212); } /* X_PM_TMR_BLK */
    else if (t->len >= 80) addr = le32(t->data + 76);                                                                     /* legacy PM_TMR_BLK (I/O) */
    free(t->data); free(t);
    if (addr == 0) { set_err("this platform has no PM timer (FADT PM_TMR_BLK is zero; HW-reduced ACPI?)"); return kc_null(); }
    char err[200] = ""; uint64_t v = 0;
    if (!gas_read_raw(space, wide ? 32 : 24, 0, access ? access : 3, addr, &v, err, sizeof err)) { set_err("%s", err); return kc_null(); }
    return u(v);
}

/* ------------------------- OS power / thermal ----------------------- */

#if !defined(KC_HW_WINDOWS)
static bool read_line_file(const char *path, char *buf, size_t n) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    bool ok = fgets(buf, (int)n, f) != NULL;
    fclose(f);
    if (ok) { size_t l = strlen(buf); while (l && (buf[l - 1] == '\n' || buf[l - 1] == '\r')) buf[--l] = 0; }
    return ok;
}
static bool read_ll_file(const char *path, long long *v) {
    char b[64];
    if (!read_line_file(path, b, sizeof b)) return false;
    char *end; errno = 0; long long x = strtoll(b, &end, 10);
    if (end == b || errno) return false;
    *v = x; return true;
}
#endif

/* [ac_online (bool|null), battery_present, battery_percent (-1 unknown),
 *  charging, seconds_remaining (-1 unknown)] */
static KcValue ac_power(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; (void)a;
    if (n != 0) kc_error_at(pos, "acpi.power() -> [ac_online, battery_present, battery_percent, charging, seconds_remaining]");
    KcValue ac = kc_null(); bool bat = false, charging = false; int pct = -1; int64_t secs = -1;
#if defined(KC_HW_WINDOWS)
    SYSTEM_POWER_STATUS s;
    if (GetSystemPowerStatus(&s)) {
        if (s.ACLineStatus != 255) ac = kc_bool(s.ACLineStatus == 1);
        bat = !(s.BatteryFlag & 128) && s.BatteryFlag != 255;
        if (bat) {
            if (s.BatteryLifePercent != 255) pct = s.BatteryLifePercent;
            charging = (s.BatteryFlag & 8) != 0;
            if (s.BatteryLifeTime != (DWORD)-1) secs = (int64_t)s.BatteryLifeTime;
        }
    } else set_err("GetSystemPowerStatus failed (Windows error %lu)", (unsigned long)GetLastError());
#else
    DIR *d = opendir("/sys/class/power_supply");
    if (!d) { set_err("/sys/class/power_supply: %s", strerror(errno)); }
    else {
        struct dirent *e; bool ac_seen = false, ac_on = false, bat_done = false;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.' || !name_ok(e->d_name)) continue;
            char base[320], p[400], type[64] = "";
            snprintf(base, sizeof base, "/sys/class/power_supply/%s", e->d_name);
            snprintf(p, sizeof p, "%s/type", base);
            if (!read_line_file(p, type, sizeof type)) continue;
            long long v;
            if (!strcmp(type, "Mains") || !strncmp(type, "USB", 3)) {
                snprintf(p, sizeof p, "%s/online", base);
                if (read_ll_file(p, &v)) { ac_seen = true; if (v) ac_on = true; }
            } else if (!strcmp(type, "Battery") && !bat_done) {
                char scope[32] = "";
                snprintf(p, sizeof p, "%s/scope", base);
                if (read_line_file(p, scope, sizeof scope) && !strcmp(scope, "Device")) continue; /* mouse/headset, not the system battery */
                snprintf(p, sizeof p, "%s/present", base);
                if (read_ll_file(p, &v) && v == 0) continue;
                bat = true; bat_done = true;
                snprintf(p, sizeof p, "%s/capacity", base);
                if (read_ll_file(p, &v) && v >= 0 && v <= 100) pct = (int)v;
                char st[32] = "";
                snprintf(p, sizeof p, "%s/status", base);
                read_line_file(p, st, sizeof st);
                charging = !strcmp(st, "Charging");
                if (!strcmp(st, "Discharging")) {
                    long long now = -1, rate = -1;
                    snprintf(p, sizeof p, "%s/time_to_empty_now", base);
                    if (read_ll_file(p, &v) && v > 0) secs = v;
                    else {
                        snprintf(p, sizeof p, "%s/energy_now", base); bool en = read_ll_file(p, &now);
                        snprintf(p, sizeof p, "%s/power_now", base);  bool pw = read_ll_file(p, &rate);
                        if (!(en && pw)) {
                            snprintf(p, sizeof p, "%s/charge_now", base); en = read_ll_file(p, &now);
                            snprintf(p, sizeof p, "%s/current_now", base); pw = read_ll_file(p, &rate);
                        }
                        if (en && pw && now >= 0 && rate > 0) secs = (int64_t)((double)now * 3600.0 / (double)rate);
                    }
                }
            }
        }
        closedir(d);
        if (ac_seen) ac = kc_bool(ac_on);
    }
#endif
    KcValue o = kc_array_new();
    kc_array_push(o, ac); kc_array_push(o, kc_bool(bat)); kc_array_push(o, kc_int(pct));
    kc_array_push(o, kc_bool(charging)); kc_array_push(o, kc_int(secs));
    return o;
}

/* [[name, type, temp_millicelsius], ...] from the OS thermal framework
 * (ACPI thermal zones appear there with type "acpitz"). Linux only. */
static KcValue ac_thermal_zones(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; (void)a;
    if (n != 0) kc_error_at(pos, "acpi.thermal_zones() -> [[name, type, temp_millicelsius], ...]");
    KcValue out = kc_array_new();
#if defined(KC_HW_WINDOWS)
    set_err("thermal zones are not exposed through a user-mode API on Windows");
#else
    DIR *d = opendir("/sys/class/thermal");
    if (!d) { set_err("/sys/class/thermal: %s", strerror(errno)); return out; }
    char **names = NULL; int cnt = 0, cap = 0; struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "thermal_zone", 12) != 0 || !name_ok(e->d_name)) continue;
        if (cnt == cap) { cap = cap ? cap * 2 : 16; names = kc_realloc(names, sizeof(char *) * (size_t)cap); }
        names[cnt++] = kc_strdup(e->d_name);
    }
    closedir(d);
    if (cnt) qsort(names, (size_t)cnt, sizeof(char *), cmp_str);
    for (int i = 0; i < cnt; i++) {
        char p[200], type[64] = ""; long long temp;
        snprintf(p, sizeof p, "/sys/class/thermal/%s/type", names[i]);
        read_line_file(p, type, sizeof type);
        snprintf(p, sizeof p, "/sys/class/thermal/%s/temp", names[i]);
        KcValue r = kc_array_new();
        kc_array_push(r, kc_string(names[i])); kc_array_push(r, kc_string(type));
        kc_array_push(r, read_ll_file(p, &temp) ? kc_int(temp) : kc_null());
        kc_array_push(out, r);
        free(names[i]);
    }
    free(names);
#endif
    return out;
}

static KcValue ac_sleep_states(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; (void)a;
    if (n != 0) kc_error_at(pos, "acpi.sleep_states() -> string[]   (e.g. [\"freeze\", \"mem\", \"disk\"]; Linux)");
    KcValue out = kc_array_new();
#if defined(KC_HW_WINDOWS)
    set_err("sleep state enumeration is not exposed through a simple user-mode API on Windows");
#else
    char b[128];
    if (!read_line_file("/sys/power/state", b, sizeof b)) { set_err("/sys/power/state: %s", strerror(errno)); return out; }
    for (char *t = strtok(b, " "); t; t = strtok(NULL, " ")) kc_array_push(out, kc_string(t));
#endif
    return out;
}

/* ------------------------------- table ------------------------------ */

static KcModuleEntry ENTRIES[] = {
    {"available", ac_available}, {"source", ac_source}, {"list", ac_list}, {"last_error", ac_last_error},
    {"load", ac_load}, {"load_file", ac_load_file}, {"from_bytes", ac_from_bytes}, {"load_phys", ac_load_phys},
    {"free", ac_free}, {"save", ac_save},
    {"signature", ac_signature}, {"length", ac_length}, {"revision", ac_revision}, {"checksum_ok", ac_checksum_ok},
    {"oem_id", ac_oem_id}, {"oem_table_id", ac_oem_table_id}, {"oem_revision", ac_oem_revision},
    {"creator_id", ac_creator_id}, {"creator_revision", ac_creator_revision}, {"header", ac_header}, {"origin", ac_origin},
    {"u8", ac_u8}, {"u16", ac_u16}, {"u32", ac_u32}, {"u64", ac_u64}, {"bytes", ac_bytes}, {"str", ac_str},
    {"fields", ac_fields}, {"get", ac_get}, {"gas", ac_gas}, {"flags", ac_flags}, {"entries", ac_entries},
    {"rsdp", ac_rsdp}, {"scan_phys", ac_scan_phys},
    {"gas_read", ac_gas_read}, {"pm_timer", ac_pm_timer}, {"pm_timer_hz", ac_pm_timer_hz},
    {"power", ac_power}, {"thermal_zones", ac_thermal_zones}, {"sleep_states", ac_sleep_states},
};

KcModule *kc_mod_acpi(void) {
    static KcModule m = { "acpi", ENTRIES, sizeof(ENTRIES)/sizeof(ENTRIES[0]) };
    return &m;
}
