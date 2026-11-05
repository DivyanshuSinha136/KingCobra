/* KingCobra std.rhex — invoking a sealed buffer as code.
 *
 * v0.1 could only call a buffer as a zero-argument `int64_t (*)(void)`.
 * This file adds argumented calls by reusing the SAME calling-
 * convention inference the native-import FFI already relies on
 * (kc_ffi_call_dynamic in compiler/src/ffi.c: all-int64 or all-double,
 * inferred from the first argument's type, up to 6 args, platform ABI
 * handled there) rather than hand-rolling a second set of trampolines
 * for the same job — a raw rhex buffer and a dlopen'd function are
 * both just "a function pointer with no declared signature" from the
 * caller's point of view.
 *
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com>
 * License: MIT
 */
#include "rhex_glue.h"

static KcHandle *sealed_buf(KcValue v, KcPos pos, const char *who) {
    KcHandle *h = rhex_live_buf(v, pos, who);
    if (!h->extra) kc_error_at(pos, "%s: buffer is not sealed executable — call rhex.seal(handle) or rhex.protect(handle, \"rx\") first", who);
    return h;
}

static KcValue call_fn(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    rhex_require_unsafe(ip, pos, "rhex.call");
    if (n != 1) kc_error_at(pos, "rhex.call(handle) -> int");
    KcHandle *h = sealed_buf(a[0], pos, "rhex.call");
    /* result is read as one machine word (see the note in ffi.c): identical to
     * int64_t on 64-bit hosts, and the right register (r0 / a0) on 32-bit ones */
    intptr_t (*f)(void) = (intptr_t (*)(void))h->ptr;
    return kc_int((int64_t)f());
}

/* NEW: call with up to 6 arguments. Calling convention is inferred
 * from args[0] exactly like a native-import call (see ffi.h) — all
 * int-like (ints/bools) or all float-like, never mixed. Document the
 * same honest limitation ffi.h already states: this is cruder than a
 * declared signature, by design (no libffi here). */
static KcValue call_args(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    rhex_require_unsafe(ip, pos, "rhex.call_args");
    if (n != 2 || a[1].type != VAL_ARRAY) kc_error_at(pos, "rhex.call_args(handle, args: int[]|float[]) -> int|float");
    KcHandle *h = sealed_buf(a[0], pos, "rhex.call_args");
    KcArray *arr = a[1].as.arr;
    if (arr->count > 6) kc_error_at(pos, "rhex.call_args: at most 6 arguments are supported (got %d)", arr->count);
    return kc_ffi_call_dynamic(h->ptr, arr->items, arr->count, pos);
}

/* Convenience one-shot: build, seal, call, free a buffer of raw bytes
 * in a single call — the "run raw hex directly" entry point mentioned
 * in the project brief. The bytes are the HOST's machine code, e.g. on
 * x86-64 rhex.exec([0xB8,0x2A,0,0,0,0xC3]) is `mov eax,42; ret`, on
 * aarch64 [0x40,0x05,0x80,0x52, 0xC0,0x03,0x5F,0xD6] is `mov w0,#42; ret`
 * (see examples/std_rhex_arch.kc for ARM32 and RISC-V). */
static KcValue exec_fn(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    rhex_require_unsafe(ip, pos, "rhex.exec");
#ifdef KC_RHEX_UNSUPPORTED
    kc_error_at(pos, "rhex.exec: " KC_RHEX_ARCH_MSG);
#endif
    if (n != 1 || a[0].type != VAL_ARRAY) kc_error_at(pos, "rhex.exec(bytes: int[]) -> int");
    KcArray *arr = a[0].as.arr;
    size_t size = arr->count > 0 ? (size_t)arr->count : 1;
    void *mem = kc_exec_alloc(size);
    if (!mem) kc_error_at(pos, "rhex.exec: failed to allocate executable memory");
    for (int i = 0; i < arr->count; i++) ((uint8_t *)mem)[i] = (uint8_t)arr->items[i].as.i;
    if (kc_exec_seal(mem, size) != 0) {
        kc_exec_free(mem, size);
        kc_error_at(pos, "rhex.exec: failed to mark buffer executable");
    }
    intptr_t (*f)(void) = (intptr_t (*)(void))mem;
    intptr_t r = f();
    kc_exec_free(mem, size);
    return kc_int((int64_t)r);
}

/* NEW: the argumented sibling of exec() — one-shot build/seal/call-
 * with-args/free, same calling-convention inference as call_args. */
static KcValue exec_args(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    rhex_require_unsafe(ip, pos, "rhex.exec_args");
#ifdef KC_RHEX_UNSUPPORTED
    kc_error_at(pos, "rhex.exec_args: " KC_RHEX_ARCH_MSG);
#endif
    if (n != 2 || a[0].type != VAL_ARRAY || a[1].type != VAL_ARRAY)
        kc_error_at(pos, "rhex.exec_args(bytes: int[], args: int[]|float[]) -> int|float");
    KcArray *code = a[0].as.arr, *args = a[1].as.arr;
    if (args->count > 6) kc_error_at(pos, "rhex.exec_args: at most 6 arguments are supported (got %d)", args->count);
    size_t size = code->count > 0 ? (size_t)code->count : 1;
    void *mem = kc_exec_alloc(size);
    if (!mem) kc_error_at(pos, "rhex.exec_args: failed to allocate executable memory");
    for (int i = 0; i < code->count; i++) ((uint8_t *)mem)[i] = (uint8_t)code->items[i].as.i;
    if (kc_exec_seal(mem, size) != 0) {
        kc_exec_free(mem, size);
        kc_error_at(pos, "rhex.exec_args: failed to mark buffer executable");
    }
    KcValue r = kc_ffi_call_dynamic(mem, args->items, args->count, pos);
    kc_exec_free(mem, size);
    return r;
}

const RhexEntry rhex_call_entries[] = {
    {"call", call_fn}, {"call_args", call_args}, {"exec", exec_fn}, {"exec_args", exec_args},
};
const int rhex_call_count = sizeof(rhex_call_entries) / sizeof(rhex_call_entries[0]);
