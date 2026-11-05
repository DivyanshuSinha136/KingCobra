/* KingCobra std.memory — native, explicit memory control.
 * Buffers are opaque handles; every operation is bounds-checked
 * at the KingCobra/native boundary so a script cannot corrupt the
 * host process through this module alone (rhex is the escape hatch
 * for that, and it demands `unsafe { }`).
 */
#include "interp.h"
/* plain malloc-backed buffers — portable, no platform-specific headers needed */

static KcValue mem_alloc(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    if (n != 1 || a[0].type != VAL_INT || a[0].as.i <= 0)
        kc_error_at(pos, "memory.alloc(size: int) -> handle");
    KcHandle *h = kc_malloc(sizeof(KcHandle));
    h->kind = H_MEMBUF;
    h->size = (size_t)a[0].as.i;
    h->ptr = kc_malloc(h->size);
    memset(h->ptr, 0, h->size);
    KcValue v; v.type = VAL_HANDLE; v.as.handle = h;
    return v;
}

static KcHandle *expect_membuf(KcValue v, KcPos pos, const char *who) {
    if (v.type != VAL_HANDLE || v.as.handle->kind != H_MEMBUF)
        kc_error_at(pos, "%s: expected a memory.alloc() handle", who);
    return v.as.handle;
}

static KcValue mem_free(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    if (n != 1) kc_error_at(pos, "memory.free(handle)");
    KcHandle *h = expect_membuf(a[0], pos, "memory.free");
    free(h->ptr); h->ptr = NULL; h->size = 0;
    return kc_null();
}

static KcValue mem_size(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; if (n != 1) kc_error_at(pos, "memory.size(handle)");
    return kc_int((int64_t)expect_membuf(a[0], pos, "memory.size")->size);
}

static void check_bounds(KcHandle *h, int64_t off, KcPos pos, const char *who) {
    if (!h->ptr) kc_error_at(pos, "%s: use after free", who);
    if (off < 0 || (size_t)off >= h->size) kc_error_at(pos, "%s: offset %lld out of bounds (size %zu)", who, (long long)off, h->size);
}

static KcValue mem_set8(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; if (n != 3) kc_error_at(pos, "memory.set8(handle, offset, value)");
    KcHandle *h = expect_membuf(a[0], pos, "memory.set8");
    check_bounds(h, a[1].as.i, pos, "memory.set8");
    ((uint8_t *)h->ptr)[a[1].as.i] = (uint8_t)a[2].as.i;
    return kc_null();
}

static KcValue mem_get8(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; if (n != 2) kc_error_at(pos, "memory.get8(handle, offset)");
    KcHandle *h = expect_membuf(a[0], pos, "memory.get8");
    check_bounds(h, a[1].as.i, pos, "memory.get8");
    return kc_int(((uint8_t *)h->ptr)[a[1].as.i]);
}

static KcValue mem_copy_from_string(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; if (n != 3) kc_error_at(pos, "memory.write_string(handle, offset, str)");
    KcHandle *h = expect_membuf(a[0], pos, "memory.write_string");
    int64_t off = a[1].as.i;
    KcString *s = a[2].as.s;
    if (off < 0 || (size_t)(off + s->len) > h->size)
        kc_error_at(pos, "memory.write_string: write of %d bytes at %lld overflows buffer (size %zu)", s->len, (long long)off, h->size);
    memcpy((char *)h->ptr + off, s->data, s->len);
    return kc_null();
}

static KcValue mem_read_string(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; if (n != 3) kc_error_at(pos, "memory.read_string(handle, offset, len)");
    KcHandle *h = expect_membuf(a[0], pos, "memory.read_string");
    int64_t off = a[1].as.i, len = a[2].as.i;
    if (off < 0 || len < 0 || (size_t)(off + len) > h->size)
        kc_error_at(pos, "memory.read_string: read of %lld bytes at %lld overflows buffer (size %zu)", (long long)len, (long long)off, h->size);
    char *buf = kc_malloc(len + 1);
    memcpy(buf, (char *)h->ptr + off, len);
    buf[len] = 0;
    return kc_string_take(buf, (int)len);
}

static KcModuleEntry ENTRIES[] = {
    {"alloc", mem_alloc}, {"free", mem_free}, {"size", mem_size},
    {"set8", mem_set8}, {"get8", mem_get8},
    {"write_string", mem_copy_from_string}, {"read_string", mem_read_string},
};

KcModule *kc_mod_memory(void) {
    static KcModule m = { "memory", ENTRIES, sizeof(ENTRIES)/sizeof(ENTRIES[0]) };
    return &m;
}
