/* KingCobra std.rhex — executable-buffer lifecycle.
 *
 * A buffer is always in exactly one of two states — writable (plain
 * data) or executable (sealed, locked against writes) — never both:
 * W^X is enforced by construction, not by convention. v0.1 could only
 * go writable -> executable once; this version adds `rhex.protect()`
 * so a sealed buffer can go back to writable to be patched and
 * re-sealed, plus read-back (`read`/`read_bytes`) and a one-call
 * `from_bytes` convenience, so a buffer behaves like a real mutable
 * code object instead of a write-only staging area.
 *
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com>
 * License: MIT
 */
#include "rhex_glue.h"

static KcValue buf_alloc(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    rhex_require_unsafe(ip, pos, "rhex.alloc");
#ifdef KC_RHEX_UNSUPPORTED
    kc_error_at(pos, "rhex.alloc: " KC_RHEX_ARCH_MSG);
#endif
    if (n != 1 || a[0].type != VAL_INT || a[0].as.i <= 0)
        kc_error_at(pos, "rhex.alloc(size: int) -> handle");
    size_t size = (size_t)a[0].as.i;
    void *mem = kc_exec_alloc(size);
    if (!mem) kc_error_at(pos, "rhex.alloc: failed to allocate executable memory");
    KcHandle *h = kc_malloc(sizeof(KcHandle));
    h->kind = H_JITBUF; h->ptr = mem; h->size = size; h->extra = 0; /* 0 = writable, not yet executable */
    KcValue v; v.type = VAL_HANDLE; v.as.handle = h;
    return v;
}

static KcValue buf_write(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    rhex_require_unsafe(ip, pos, "rhex.write");
    if (n != 3) kc_error_at(pos, "rhex.write(handle, offset: int, byte: int)");
    KcHandle *h = rhex_live_buf(a[0], pos, "rhex.write");
    if (h->extra) kc_error_at(pos, "rhex.write: buffer is sealed executable; call rhex.protect(handle, \"rw\") first");
    int64_t off = a[1].as.i;
    if (off < 0 || (size_t)off >= h->size) kc_error_at(pos, "rhex.write: offset %lld out of bounds (size %zu)", (long long)off, h->size);
    ((uint8_t *)h->ptr)[off] = (uint8_t)a[2].as.i;
    return kc_null();
}

static KcValue buf_write_bytes(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    rhex_require_unsafe(ip, pos, "rhex.write_bytes");
    if (n != 2 || a[1].type != VAL_ARRAY) kc_error_at(pos, "rhex.write_bytes(handle, bytes: int[])");
    KcHandle *h = rhex_live_buf(a[0], pos, "rhex.write_bytes");
    if (h->extra) kc_error_at(pos, "rhex.write_bytes: buffer is sealed executable; call rhex.protect(handle, \"rw\") first");
    KcArray *arr = a[1].as.arr;
    if ((size_t)arr->count > h->size) kc_error_at(pos, "rhex.write_bytes: %d bytes does not fit in a %zu-byte buffer", arr->count, h->size);
    for (int i = 0; i < arr->count; i++) ((uint8_t *)h->ptr)[i] = (uint8_t)arr->items[i].as.i;
    return kc_null();
}

/* NEW: read bytes back out of a buffer — works in either protection
 * state, since inspecting bytes already present is never unsafe on
 * its own (writing or executing them is what the other entries gate). */
static KcValue buf_read_bytes(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    rhex_require_unsafe(ip, pos, "rhex.read_bytes");
    if (n != 2) kc_error_at(pos, "rhex.read_bytes(handle, count: int) -> int[]");
    KcHandle *h = rhex_live_buf(a[0], pos, "rhex.read_bytes");
    if (a[1].type != VAL_INT || a[1].as.i < 0) kc_error_at(pos, "rhex.read_bytes: count must be a non-negative int");
    size_t count = (size_t)a[1].as.i;
    if (count > h->size) kc_error_at(pos, "rhex.read_bytes: count %zu exceeds buffer size %zu", count, h->size);
    KcValue arr = kc_array_new();
    for (size_t i = 0; i < count; i++) kc_array_push(arr, kc_int(((uint8_t *)h->ptr)[i]));
    return arr;
}

static KcValue buf_read(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    rhex_require_unsafe(ip, pos, "rhex.read");
    if (n != 2) kc_error_at(pos, "rhex.read(handle, offset: int) -> int");
    KcHandle *h = rhex_live_buf(a[0], pos, "rhex.read");
    if (a[1].type != VAL_INT || a[1].as.i < 0 || (size_t)a[1].as.i >= h->size)
        kc_error_at(pos, "rhex.read: offset out of bounds (size %zu)", h->size);
    return kc_int(((uint8_t *)h->ptr)[a[1].as.i]);
}

/* NEW: generalizes the old one-way `seal` into a real toggle, driven
 * by kc_exec_seal (-> RX) / kc_exec_unseal (-> RW) in kc_platform.h.
 * Kept as exactly two named states — never a free-form flag string —
 * so W+X together is not expressible through this API at all. */
static KcValue buf_protect(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    rhex_require_unsafe(ip, pos, "rhex.protect");
    if (n != 2 || a[1].type != VAL_STRING) kc_error_at(pos, "rhex.protect(handle, \"rw\" | \"rx\")");
    KcHandle *h = rhex_live_buf(a[0], pos, "rhex.protect");
    const char *mode = a[1].as.s->data;
    if (!strcmp(mode, "rx")) {
        if (kc_exec_seal(h->ptr, h->size) != 0) kc_error_at(pos, "rhex.protect: failed to mark buffer executable");
        h->extra = 1;
    } else if (!strcmp(mode, "rw")) {
        if (kc_exec_unseal(h->ptr, h->size) != 0) kc_error_at(pos, "rhex.protect: failed to mark buffer writable");
        h->extra = 0;
    } else {
        kc_error_at(pos, "rhex.protect: mode must be \"rw\" or \"rx\" (got \"%s\")", mode);
    }
    return kc_null();
}

/* Back-compat name for `protect(handle, "rx")` — v0.1's one-way seal. */
static KcValue buf_seal(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    rhex_require_unsafe(ip, pos, "rhex.seal");
    if (n != 1) kc_error_at(pos, "rhex.seal(handle)");
    KcHandle *h = rhex_live_buf(a[0], pos, "rhex.seal");
    if (kc_exec_seal(h->ptr, h->size) != 0) kc_error_at(pos, "rhex.seal: failed to mark buffer executable");
    h->extra = 1;
    return kc_null();
}

static KcValue buf_is_sealed(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    rhex_require_unsafe(ip, pos, "rhex.is_sealed");
    if (n != 1) kc_error_at(pos, "rhex.is_sealed(handle) -> bool");
    KcHandle *h = rhex_live_buf(a[0], pos, "rhex.is_sealed");
    return kc_bool(h->extra != 0);
}

static KcValue buf_size(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    rhex_require_unsafe(ip, pos, "rhex.size");
    if (n != 1) kc_error_at(pos, "rhex.size(handle) -> int");
    KcHandle *h = rhex_live_buf(a[0], pos, "rhex.size");
    return kc_int((int64_t)h->size);
}

/* NEW: alloc + write_bytes in one call — the common case of "I have
 * a byte array, give me a (writable, unsealed) buffer holding it". */
static KcValue buf_from_bytes(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    rhex_require_unsafe(ip, pos, "rhex.from_bytes");
#ifdef KC_RHEX_UNSUPPORTED
    kc_error_at(pos, "rhex.from_bytes: " KC_RHEX_ARCH_MSG);
#endif
    if (n != 1 || a[0].type != VAL_ARRAY) kc_error_at(pos, "rhex.from_bytes(bytes: int[]) -> handle");
    KcArray *arr = a[0].as.arr;
    size_t size = arr->count > 0 ? (size_t)arr->count : 1;
    void *mem = kc_exec_alloc(size);
    if (!mem) kc_error_at(pos, "rhex.from_bytes: failed to allocate executable memory");
    for (int i = 0; i < arr->count; i++) ((uint8_t *)mem)[i] = (uint8_t)arr->items[i].as.i;
    KcHandle *h = kc_malloc(sizeof(KcHandle));
    h->kind = H_JITBUF; h->ptr = mem; h->size = size; h->extra = 0;
    KcValue v; v.type = VAL_HANDLE; v.as.handle = h;
    return v;
}

static KcValue buf_free(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    rhex_require_unsafe(ip, pos, "rhex.free");
    if (n != 1) kc_error_at(pos, "rhex.free(handle)");
    KcHandle *h = rhex_live_buf(a[0], pos, "rhex.free");
    kc_exec_free(h->ptr, h->size);
    h->ptr = NULL; h->size = 0;
    return kc_null();
}

const RhexEntry rhex_buf_entries[] = {
    {"alloc", buf_alloc}, {"write", buf_write}, {"write_bytes", buf_write_bytes},
    {"read", buf_read}, {"read_bytes", buf_read_bytes}, {"from_bytes", buf_from_bytes},
    {"protect", buf_protect}, {"seal", buf_seal}, {"is_sealed", buf_is_sealed},
    {"size", buf_size}, {"free", buf_free},
};
const int rhex_buf_count = sizeof(rhex_buf_entries) / sizeof(rhex_buf_entries[0]);
