/* KingCobra built-in string/array methods — `s.split(",")`, `a.push(x)`,
 * etc. See methods.h for how this plugs into N_CALL dispatch.
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com>
 */
#include "methods.h"
#include "ptr.h"
#include "mem.h"
#include "lib.h"
#include "lang.h"
#include <ctype.h>

/* ---------------------------- string methods ---------------------------- */

static KcValue str_upper(KcValue s) {
    char *out = kc_malloc((size_t)s.as.s->len + 1);
    for (int i = 0; i < s.as.s->len; i++) out[i] = (char)toupper((unsigned char)s.as.s->data[i]);
    out[s.as.s->len] = 0;
    return kc_string_adopt(out, s.as.s->len);
}
static KcValue str_lower(KcValue s) {
    char *out = kc_malloc((size_t)s.as.s->len + 1);
    for (int i = 0; i < s.as.s->len; i++) out[i] = (char)tolower((unsigned char)s.as.s->data[i]);
    out[s.as.s->len] = 0;
    return kc_string_adopt(out, s.as.s->len);
}
static KcValue str_trim(KcValue s) {
    const char *start = s.as.s->data;
    const char *end = start + s.as.s->len;
    while (start < end && isspace((unsigned char)*start)) start++;
    while (end > start && isspace((unsigned char)*(end - 1))) end--;
    int len = (int)(end - start);
    char *out = kc_malloc((size_t)len + 1);
    memcpy(out, start, len);
    out[len] = 0;
    return kc_string_adopt(out, len);
}

static int str_find(KcValue hay, const char *needle, int needle_len, int from) {
    if (needle_len == 0) return from <= hay.as.s->len ? from : -1;
    for (int i = from; i + needle_len <= hay.as.s->len; i++)
        if (memcmp(hay.as.s->data + i, needle, needle_len) == 0) return i;
    return -1;
}

static KcValue str_split(KcValue s, KcValue *args, int nargs, KcPos pos) {
    if (nargs != 1 || args[0].type != VAL_STRING) kc_error_at(pos, "split(separator: string) -> string[]");
    KcValue arr = kc_array_new();
    const char *sep = args[0].as.s->data;
    int sep_len = args[0].as.s->len;
    if (sep_len == 0) kc_error_at(pos, "split: separator must not be empty");
    int start = 0;
    for (;;) {
        int found = str_find(s, sep, sep_len, start);
        if (found < 0) {
            int len = s.as.s->len - start;
            char *piece = kc_malloc((size_t)len + 1);
            memcpy(piece, s.as.s->data + start, len);
            piece[len] = 0;
            kc_array_push(arr, kc_string_adopt(piece, len));
            break;
        }
        int len = found - start;
        char *piece = kc_malloc((size_t)len + 1);
        memcpy(piece, s.as.s->data + start, len);
        piece[len] = 0;
        kc_array_push(arr, kc_string_adopt(piece, len));
        start = found + sep_len;
    }
    return arr;
}

static KcValue str_contains(KcValue s, KcValue *args, int nargs, KcPos pos) {
    if (nargs != 1 || args[0].type != VAL_STRING) kc_error_at(pos, "contains(sub: string) -> bool");
    return kc_bool(str_find(s, args[0].as.s->data, args[0].as.s->len, 0) >= 0);
}

static KcValue str_index_of(KcValue s, KcValue *args, int nargs, KcPos pos) {
    if (nargs != 1 || args[0].type != VAL_STRING) kc_error_at(pos, "index_of(sub: string) -> int");
    return kc_int(str_find(s, args[0].as.s->data, args[0].as.s->len, 0));
}

static KcValue str_starts_with(KcValue s, KcValue *args, int nargs, KcPos pos) {
    if (nargs != 1 || args[0].type != VAL_STRING) kc_error_at(pos, "starts_with(prefix: string) -> bool");
    int plen = args[0].as.s->len;
    if (plen > s.as.s->len) return kc_bool(false);
    return kc_bool(memcmp(s.as.s->data, args[0].as.s->data, plen) == 0);
}

static KcValue str_ends_with(KcValue s, KcValue *args, int nargs, KcPos pos) {
    if (nargs != 1 || args[0].type != VAL_STRING) kc_error_at(pos, "ends_with(suffix: string) -> bool");
    int slen = args[0].as.s->len;
    if (slen > s.as.s->len) return kc_bool(false);
    return kc_bool(memcmp(s.as.s->data + s.as.s->len - slen, args[0].as.s->data, slen) == 0);
}

static KcValue str_replace(KcValue s, KcValue *args, int nargs, KcPos pos) {
    if (nargs != 2 || args[0].type != VAL_STRING || args[1].type != VAL_STRING)
        kc_error_at(pos, "replace(old: string, new: string) -> string");
    const char *old = args[0].as.s->data;
    int old_len = args[0].as.s->len;
    const char *rep = args[1].as.s->data;
    int rep_len = args[1].as.s->len;
    if (old_len == 0) kc_error_at(pos, "replace: 'old' must not be empty");
    size_t cap = (size_t)s.as.s->len + 1, len = 0;
    char *out = kc_malloc(cap);
    int i = 0;
    while (i < s.as.s->len) {
        if (i + old_len <= s.as.s->len && memcmp(s.as.s->data + i, old, old_len) == 0) {
            while (len + (size_t)rep_len + 1 > cap) { cap *= 2; out = kc_realloc(out, cap); }
            memcpy(out + len, rep, rep_len);
            len += rep_len;
            i += old_len;
        } else {
            while (len + 2 > cap) { cap *= 2; out = kc_realloc(out, cap); }
            out[len++] = s.as.s->data[i++];
        }
    }
    out[len] = 0;
    return kc_string_adopt(out, (int)len);
}

static KcValue str_slice(KcValue s, KcValue *args, int nargs, KcPos pos) {
    if (nargs != 2 || args[0].type != VAL_INT || args[1].type != VAL_INT)
        kc_error_at(pos, "slice(start: int, end: int) -> string");
    int64_t start = args[0].as.i, end = args[1].as.i;
    int len = s.as.s->len;
    if (start < 0) start = 0;
    if (end > len) end = len;
    if (end < start) end = start;
    int outlen = (int)(end - start);
    char *out = kc_malloc((size_t)outlen + 1);
    memcpy(out, s.as.s->data + start, outlen);
    out[outlen] = 0;
    return kc_string_adopt(out, outlen);
}

/* ---------------------------- array methods ---------------------------- */

static KcValue arr_push(KcValue a, KcValue *args, int nargs, KcPos pos) {
    if (nargs != 1) kc_error_at(pos, "push(value) -> null");
    kc_array_push(a, args[0]);
    return kc_null();
}

static KcValue arr_pop(KcValue a, KcValue *args, int nargs, KcPos pos) {
    (void)args;
    if (nargs != 0) kc_error_at(pos, "pop() -> value");
    if (a.as.arr->count == 0) kc_error_at(pos, "pop: array is empty");
    return a.as.arr->items[--a.as.arr->count];
}

static KcValue arr_contains(KcValue a, KcValue *args, int nargs, KcPos pos) {
    if (nargs != 1) kc_error_at(pos, "contains(value) -> bool");
    for (int i = 0; i < a.as.arr->count; i++) if (kc_equal(a.as.arr->items[i], args[0])) return kc_bool(true);
    return kc_bool(false);
}

static KcValue arr_index_of(KcValue a, KcValue *args, int nargs, KcPos pos) {
    if (nargs != 1) kc_error_at(pos, "index_of(value) -> int");
    for (int i = 0; i < a.as.arr->count; i++) if (kc_equal(a.as.arr->items[i], args[0])) return kc_int(i);
    return kc_int(-1);
}

static KcValue arr_slice(KcValue a, KcValue *args, int nargs, KcPos pos) {
    if (nargs != 2 || args[0].type != VAL_INT || args[1].type != VAL_INT)
        kc_error_at(pos, "slice(start: int, end: int) -> array");
    int64_t start = args[0].as.i, end = args[1].as.i;
    int count = a.as.arr->count;
    if (start < 0) start = 0;
    if (end > count) end = count;
    if (end < start) end = start;
    KcValue out = kc_array_new();
    for (int64_t i = start; i < end; i++) kc_array_push(out, a.as.arr->items[i]);
    return out;
}

static KcValue arr_join(KcValue a, KcValue *args, int nargs, KcPos pos) {
    if (nargs != 1 || args[0].type != VAL_STRING) kc_error_at(pos, "join(separator: string) -> string");
    const char *sep = args[0].as.s->data;
    int sep_len = args[0].as.s->len;
    size_t cap = 64, len = 0;
    char *out = kc_malloc(cap);
    out[0] = 0;
    for (int i = 0; i < a.as.arr->count; i++) {
        char *piece = kc_to_display_string(a.as.arr->items[i]);
        size_t piece_len = strlen(piece);
        while (len + piece_len + (size_t)sep_len + 1 > cap) { cap *= 2; out = kc_realloc(out, cap); }
        if (i) { memcpy(out + len, sep, sep_len); len += sep_len; }
        memcpy(out + len, piece, piece_len);
        len += piece_len;
        free(piece);
    }
    out[len] = 0;
    return kc_string_adopt(out, (int)len);
}

static KcValue arr_reverse(KcValue a, KcValue *args, int nargs, KcPos pos) {
    (void)args; (void)pos;
    if (nargs != 0) kc_error_at(pos, "reverse() -> array (reverses in place, returns the same array)");
    int n = a.as.arr->count;
    for (int i = 0; i < n / 2; i++) {
        KcValue tmp = a.as.arr->items[i];
        a.as.arr->items[i] = a.as.arr->items[n - 1 - i];
        a.as.arr->items[n - 1 - i] = tmp;
    }
    return a;
}

/* Id-dispatched: `mid` comes from kc_method_id(name), resolved once per call site and cached in
 * the AST node, so a built-in method call performs no string comparison at all. */
bool kc_try_builtin_method_id(KcInterp *ip, KcValue obj, const char *name, int mid, KcValue *args, int nargs, KcPos pos, KcValue *out) {
    if (__builtin_expect(kc_any_freed, 0)) {            /* tombstones can only exist once something was freed */
        if (kc_value_freed(obj)) kc_error_freed(obj, pos);
        for (int i = 0; i < nargs; i++)
            if (kc_value_freed(args[i])) { kc_check_live_args(args, nargs, pos); break; }
    }
    if (obj.type == VAL_PTR || obj.type == VAL_BUFFER) return kc_ptr_method(ip, obj, name, args, nargs, pos, out);
    if (mid == M_NONE) return false;
    if (ip->nmethods) {                                 /* a user `impl` method of the same name wins over a built-in */
        KcValue f; bool inst;
        if (kc_method_find(ip, kc_value_type_key(obj), name, &f, &inst)) return false;
    }
    switch (obj.type) {
        case VAL_STRING:
            switch (mid) {
                case M_UPPER: if (nargs == 0) { *out = str_upper(obj); return true; } return false;
                case M_LOWER: if (nargs == 0) { *out = str_lower(obj); return true; } return false;
                case M_TRIM:  if (nargs == 0) { *out = str_trim(obj); return true; } return false;
                case M_SPLIT: *out = str_split(obj, args, nargs, pos); return true;
                case M_CONTAINS: *out = str_contains(obj, args, nargs, pos); return true;
                case M_INDEX_OF: *out = str_index_of(obj, args, nargs, pos); return true;
                case M_STARTS_WITH: *out = str_starts_with(obj, args, nargs, pos); return true;
                case M_ENDS_WITH: *out = str_ends_with(obj, args, nargs, pos); return true;
                case M_REPLACE: *out = str_replace(obj, args, nargs, pos); return true;
                case M_SLICE: *out = str_slice(obj, args, nargs, pos); return true;
                default: return kc_lib_method(ip, obj, mid, args, nargs, pos, out);
            }
        case VAL_ARRAY:
            switch (mid) {
                case M_PUSH: *out = arr_push(obj, args, nargs, pos); return true;
                case M_POP: *out = arr_pop(obj, args, nargs, pos); return true;
                case M_CONTAINS: *out = arr_contains(obj, args, nargs, pos); return true;
                case M_INDEX_OF: *out = arr_index_of(obj, args, nargs, pos); return true;
                case M_SLICE: *out = arr_slice(obj, args, nargs, pos); return true;
                case M_JOIN: *out = arr_join(obj, args, nargs, pos); return true;
                case M_REVERSE: *out = arr_reverse(obj, args, nargs, pos); return true;
                default: return kc_lib_method(ip, obj, mid, args, nargs, pos, out);
            }
        case VAL_STRUCT: {
            for (int i = 0; i < obj.as.strct->count; i++)       /* a field of that name (e.g. a closure) wins */
                if (strcmp(obj.as.strct->names[i], name) == 0) return false;
            return kc_lib_method(ip, obj, mid, args, nargs, pos, out);
        }
        default: return false;
    }
}

bool kc_try_builtin_method(KcInterp *ip, KcValue obj, const char *name, KcValue *args, int nargs, KcPos pos, KcValue *out) {
    return kc_try_builtin_method_id(ip, obj, name, kc_method_id(name), args, nargs, pos, out);
}
