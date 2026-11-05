/* Core standard library (v0.7). Plain C: flat loops, one stable merge sort, no per-element
 * allocation beyond the results. Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com> */
#include "lib.h"
#include "lang.h"
#include "mem.h"
#include "kc_platform.h"
#include <math.h>

static const struct { const char *name; int id; } MTAB[] = {
    {"upper", M_UPPER}, {"lower", M_LOWER}, {"trim", M_TRIM}, {"split", M_SPLIT}, {"contains", M_CONTAINS},
    {"index_of", M_INDEX_OF}, {"starts_with", M_STARTS_WITH}, {"ends_with", M_ENDS_WITH}, {"replace", M_REPLACE},
    {"slice", M_SLICE}, {"repeat", M_REPEAT}, {"pad_left", M_PAD_LEFT}, {"pad_right", M_PAD_RIGHT},
    {"chars", M_CHARS}, {"lines", M_LINES},
    {"push", M_PUSH}, {"pop", M_POP}, {"join", M_JOIN}, {"reverse", M_REVERSE},
    {"map", M_MAP}, {"filter", M_FILTER}, {"reduce", M_REDUCE}, {"find", M_FIND}, {"any", M_ANY}, {"all", M_ALL},
    {"each", M_EACH}, {"sort", M_SORT}, {"concat", M_CONCAT}, {"first", M_FIRST}, {"last", M_LAST},
    {"clear", M_CLEAR}, {"insert", M_INSERT}, {"remove", M_REMOVE}, {"extend", M_EXTEND},
    {"sum", M_SUM}, {"min", M_MIN}, {"max", M_MAX},
    {"keys", M_KEYS}, {"values", M_VALUES}, {"items", M_ITEMS}, {"has", M_HAS}, {"get", M_GET}, {"merge", M_MERGE},
};

int kc_method_id(const char *name) {
    for (size_t i = 0; i < sizeof MTAB / sizeof MTAB[0]; i++)
        if (name[0] == MTAB[i].name[0] && strcmp(name, MTAB[i].name) == 0) return MTAB[i].id;
    return M_NONE;
}

const char *kc_method_hint(const char *name) {
    const char *names[sizeof MTAB / sizeof MTAB[0]];
    int n = (int)(sizeof MTAB / sizeof MTAB[0]);
    for (int i = 0; i < n; i++) names[i] = MTAB[i].name;
    return kc_hint_for_module(names, n, name);
}

/* ---------------------------------------------------------------- helpers */

static void arity(int nargs, int lo, int hi, const char *sig, KcPos pos) {
    if (nargs < lo || nargs > hi) kc_error_at(pos, "wrong number of arguments: %s", sig);
}

/* Calls a user callback with the element, plus its index when the callback declares 2+ parameters. */
static KcValue call_cb(KcInterp *ip, KcValue f, KcValue item, int64_t index, KcPos pos) {
    if (f.type != VAL_CLOSURE && f.type != VAL_NATIVE) kc_error_at(pos, "expected a function, got a value of type %s", kc_type_name(f));
    KcValue a[2]; a[0] = item; a[1] = kc_int(index);
    int n = (f.type == VAL_CLOSURE && f.as.closure->fnnode->as.fn.params.count >= 2) ? 2 : 1;
    return kc_call_value(ip, f, a, n, pos);
}

static int type_rank(KcValue v) { return v.type == VAL_INT || v.type == VAL_FLOAT ? 1 : v.type == VAL_STRING ? 2 : 0; }

static int cmp_default(KcValue a, KcValue b, KcPos pos) {
    if (type_rank(a) == 1 && type_rank(b) == 1) {
        if (a.type == VAL_INT && b.type == VAL_INT) return a.as.i < b.as.i ? -1 : a.as.i > b.as.i;
        double x = a.type == VAL_INT ? (double)a.as.i : a.as.f, y = b.type == VAL_INT ? (double)b.as.i : b.as.f;
        return x < y ? -1 : x > y;
    }
    if (a.type == VAL_STRING && b.type == VAL_STRING) {
        int n = a.as.s->len < b.as.s->len ? a.as.s->len : b.as.s->len;
        int c = memcmp(a.as.s->data, b.as.s->data, (size_t)n);
        return c ? (c < 0 ? -1 : 1) : (a.as.s->len < b.as.s->len ? -1 : a.as.s->len > b.as.s->len);
    }
    kc_error_at(pos, "cannot order %s with %s values (only numbers with numbers, strings with strings)", kc_type_name(a), kc_type_name(b));
    return 0;
}

typedef struct { KcInterp *ip; KcValue cmp; KcPos pos; } SortCtx;

static int cmp_with(SortCtx *c, KcValue a, KcValue b) {
    if (c->cmp.type == VAL_NULL) return cmp_default(a, b, c->pos);
    KcValue args[2] = { a, b };
    KcValue r = kc_call_value(c->ip, c->cmp, args, 2, c->pos);
    if (r.type == VAL_INT) return r.as.i < 0 ? -1 : r.as.i > 0;
    if (r.type == VAL_FLOAT) return r.as.f < 0 ? -1 : r.as.f > 0;
    kc_error_at(c->pos, "a sort comparator must return a number (negative, zero or positive), got %s", kc_type_name(r));
    return 0;
}

/* Stable merge sort; insertion sort for short runs. */
static void msort(SortCtx *c, KcValue *a, KcValue *tmp, int n) {
    if (n <= 8) {
        for (int i = 1; i < n; i++) {
            KcValue v = a[i]; int j = i - 1;
            while (j >= 0 && cmp_with(c, a[j], v) > 0) { a[j + 1] = a[j]; j--; }
            a[j + 1] = v;
        }
        return;
    }
    int m = n / 2;
    msort(c, a, tmp, m); msort(c, a + m, tmp, n - m);
    int i = 0, j = m, k = 0;
    while (i < m && j < n) tmp[k++] = cmp_with(c, a[j], a[i]) < 0 ? a[j++] : a[i++];   /* ties take the left: stable */
    while (i < m) tmp[k++] = a[i++];
    while (j < n) tmp[k++] = a[j++];
    memcpy(a, tmp, sizeof(KcValue) * (size_t)n);
}

static void sort_values(KcInterp *ip, KcValue *items, int n, KcValue cmp, KcPos pos) {
    if (n < 2) return;
    SortCtx c = { ip, cmp, pos };
    KcValue *tmp = kc_malloc(sizeof(KcValue) * (size_t)n);
    msort(&c, items, tmp, n);
    free(tmp);
}

static KcValue obj_of(KcValue v, KcPos pos, const char *what) {
    if (v.type != VAL_STRUCT) kc_error_at(pos, "%s needs an object or struct, got a value of type %s", what, kc_type_name(v));
    return v;
}

/* ---------------------------------------------------------------- the methods */

static KcValue pad(KcValue s, KcValue *args, int nargs, bool left, KcPos pos) {
    arity(nargs, 1, 2, left ? "pad_left(width [, fill])" : "pad_right(width [, fill])", pos);
    if (args[0].type != VAL_INT) kc_error_at(pos, "padding width must be an int");
    char fill = ' ';
    if (nargs == 2) {
        if (args[1].type != VAL_STRING || args[1].as.s->len != 1) kc_error_at(pos, "the fill must be a one-character string");
        fill = args[1].as.s->data[0];
    }
    int64_t w = args[0].as.i, len = s.as.s->len;
    if (w <= len) { char *d; KcValue r = kc_string_alloc((int)len, &d); memcpy(d, s.as.s->data, (size_t)len); return r; }
    char *d; KcValue r = kc_string_alloc((int)w, &d);
    int64_t padn = w - len;
    if (left) { memset(d, fill, (size_t)padn); memcpy(d + padn, s.as.s->data, (size_t)len); }
    else { memcpy(d, s.as.s->data, (size_t)len); memset(d + len, fill, (size_t)padn); }
    return r;
}

static KcValue str_lib(KcValue s, int mid, KcValue *args, int nargs, KcPos pos) {
    switch (mid) {
        case M_REPEAT: {
            arity(nargs, 1, 1, "repeat(count)", pos);
            if (args[0].type != VAL_INT || args[0].as.i < 0) kc_error_at(pos, "repeat count must be a non-negative int");
            int64_t n = args[0].as.i, len = s.as.s->len;
            if (len * n > (1 << 30)) kc_error_at(pos, "repeat: result would be larger than 1 GiB");
            char *d; KcValue r = kc_string_alloc((int)(len * n), &d);
            for (int64_t i = 0; i < n; i++) memcpy(d + i * len, s.as.s->data, (size_t)len);
            return r;
        }
        case M_PAD_LEFT: return pad(s, args, nargs, true, pos);
        case M_PAD_RIGHT: return pad(s, args, nargs, false, pos);
        case M_CHARS: {                                         /* UTF-8 aware: one string per character */
            arity(nargs, 0, 0, "chars()", pos);
            KcValue r = kc_array_new();
            const unsigned char *p = (const unsigned char *)s.as.s->data;
            for (int i = 0; i < s.as.s->len;) {
                int n = p[i] >= 0xF0 ? 4 : p[i] >= 0xE0 ? 3 : p[i] >= 0xC0 ? 2 : 1;
                if (i + n > s.as.s->len) n = s.as.s->len - i;
                char *d; KcValue c = kc_string_alloc(n, &d); memcpy(d, p + i, (size_t)n);
                kc_array_push(r, c); i += n;
            }
            return r;
        }
        default: {                                              /* M_LINES */
            arity(nargs, 0, 0, "lines()", pos);
            KcValue r = kc_array_new();
            const char *d0 = s.as.s->data; int n = s.as.s->len, st = 0;
            for (int i = 0; i <= n; i++) {
                if (i == n || d0[i] == '\n') {
                    if (i == n && st == n) break;
                    int e = i; if (e > st && d0[e - 1] == '\r') e--;
                    char *d; KcValue l = kc_string_alloc(e - st, &d); memcpy(d, d0 + st, (size_t)(e - st));
                    kc_array_push(r, l); st = i + 1;
                }
            }
            return r;
        }
    }
}

static KcValue arr_lib(KcInterp *ip, KcValue a, int mid, KcValue *args, int nargs, KcPos pos) {
    KcArray *arr = a.as.arr;
    switch (mid) {
        case M_MAP: {
            arity(nargs, 1, 1, "map(fn)", pos);
            KcValue r = kc_array_new();
            for (int i = 0; i < arr->count; i++) kc_array_push(r, call_cb(ip, args[0], arr->items[i], i, pos));
            return r;
        }
        case M_FILTER: {
            arity(nargs, 1, 1, "filter(fn)", pos);
            KcValue r = kc_array_new();
            for (int i = 0; i < arr->count; i++) { KcValue it = arr->items[i]; if (kc_truthy(call_cb(ip, args[0], it, i, pos))) kc_array_push(r, it); }
            return r;
        }
        case M_REDUCE: {
            arity(nargs, 1, 2, "reduce(fn [, initial])", pos);
            int i = 0; KcValue acc;
            if (nargs == 2) acc = args[1];
            else { if (arr->count == 0) kc_error_at(pos, "reduce of an empty array needs an initial value"); acc = arr->items[0]; i = 1; }
            for (; i < arr->count; i++) { KcValue ca[2] = { acc, arr->items[i] }; acc = kc_call_value(ip, args[0], ca, 2, pos); }
            return acc;
        }
        case M_FIND:
            arity(nargs, 1, 1, "find(fn)", pos);
            for (int i = 0; i < arr->count; i++) { KcValue it = arr->items[i]; if (kc_truthy(call_cb(ip, args[0], it, i, pos))) return it; }
            return kc_null();
        case M_ANY:
            arity(nargs, 1, 1, "any(fn)", pos);
            for (int i = 0; i < arr->count; i++) if (kc_truthy(call_cb(ip, args[0], arr->items[i], i, pos))) return kc_bool(true);
            return kc_bool(false);
        case M_ALL:
            arity(nargs, 1, 1, "all(fn)", pos);
            for (int i = 0; i < arr->count; i++) if (!kc_truthy(call_cb(ip, args[0], arr->items[i], i, pos))) return kc_bool(false);
            return kc_bool(true);
        case M_EACH:
            arity(nargs, 1, 1, "each(fn)", pos);
            for (int i = 0; i < arr->count; i++) call_cb(ip, args[0], arr->items[i], i, pos);
            return kc_null();
        case M_SORT: {                                          /* in place, stable; optional comparator(a, b) */
            arity(nargs, 0, 1, "sort([comparator])", pos);
            sort_values(ip, arr->items, arr->count, nargs ? args[0] : kc_null(), pos);
            return a;
        }
        case M_CONCAT: {
            arity(nargs, 1, 1, "concat(other_array)", pos);
            if (args[0].type != VAL_ARRAY) kc_error_at(pos, "concat needs an array, got a value of type %s", kc_type_name(args[0]));
            KcValue r = kc_array_new();
            for (int i = 0; i < arr->count; i++) kc_array_push(r, arr->items[i]);
            for (int i = 0; i < args[0].as.arr->count; i++) kc_array_push(r, args[0].as.arr->items[i]);
            return r;
        }
        case M_FIRST: return arr->count ? arr->items[0] : kc_null();
        case M_LAST: return arr->count ? arr->items[arr->count - 1] : kc_null();
        case M_CLEAR: arr->count = 0; return a;
        case M_INSERT: {
            arity(nargs, 2, 2, "insert(index, value)", pos);
            if (args[0].type != VAL_INT || args[0].as.i < 0 || args[0].as.i > arr->count) kc_error_at(pos, "insert index out of range (length %d)", arr->count);
            kc_array_push(a, kc_null());
            memmove(arr->items + args[0].as.i + 1, arr->items + args[0].as.i, sizeof(KcValue) * (size_t)(arr->count - 1 - args[0].as.i));
            arr->items[args[0].as.i] = args[1];
            return a;
        }
        case M_REMOVE: {
            arity(nargs, 1, 1, "remove(index) -> the removed element", pos);
            if (args[0].type != VAL_INT || args[0].as.i < 0 || args[0].as.i >= arr->count) kc_error_at(pos, "remove index out of range (length %d)", arr->count);
            KcValue v = arr->items[args[0].as.i];
            memmove(arr->items + args[0].as.i, arr->items + args[0].as.i + 1, sizeof(KcValue) * (size_t)(arr->count - 1 - args[0].as.i));
            arr->count--;
            return v;
        }
        case M_EXTEND: {
            arity(nargs, 1, 1, "extend(other_array)", pos);
            if (args[0].type != VAL_ARRAY) kc_error_at(pos, "extend needs an array, got a value of type %s", kc_type_name(args[0]));
            KcArray *o = args[0].as.arr; int n = o->count;       /* n fixed first: a.extend(a) doubles, not loops */
            for (int i = 0; i < n; i++) kc_array_push(a, o->items[i]);
            return a;
        }
        case M_SUM: {
            arity(nargs, 0, 0, "sum()", pos);
            int64_t si = 0; double sf = 0; bool fl = false;
            for (int i = 0; i < arr->count; i++) {
                KcValue v = arr->items[i];
                if (v.type == VAL_INT) { si = (int64_t)((uint64_t)si + (uint64_t)v.as.i); sf += (double)v.as.i; }
                else if (v.type == VAL_FLOAT) { fl = true; sf += v.as.f; }
                else kc_error_at(pos, "sum: element %d is a %s, not a number", i, kc_type_name(v));
            }
            return fl ? kc_float(sf) : kc_int(si);
        }
        default: {                                              /* M_MIN / M_MAX */
            arity(nargs, 0, 0, mid == M_MIN ? "min()" : "max()", pos);
            if (arr->count == 0) kc_error_at(pos, "%s of an empty array", mid == M_MIN ? "min" : "max");
            KcValue best = arr->items[0];
            for (int i = 1; i < arr->count; i++) {
                int c = cmp_default(arr->items[i], best, pos);
                if (mid == M_MIN ? c < 0 : c > 0) best = arr->items[i];
            }
            return best;
        }
    }
}

static KcValue obj_lib(KcValue o, int mid, KcValue *args, int nargs, KcPos pos) {
    KcStructInst *st = o.as.strct;
    switch (mid) {
        case M_KEYS: { arity(nargs, 0, 0, "keys()", pos); KcValue r = kc_array_new(); for (int i = 0; i < st->count; i++) kc_array_push(r, kc_string(st->names[i])); return r; }
        case M_VALUES: { arity(nargs, 0, 0, "values()", pos); KcValue r = kc_array_new(); for (int i = 0; i < st->count; i++) kc_array_push(r, st->values[i]); return r; }
        case M_ITEMS: {
            arity(nargs, 0, 0, "items()", pos);
            KcValue r = kc_array_new();
            for (int i = 0; i < st->count; i++) { KcValue p = kc_array_new(); kc_array_push(p, kc_string(st->names[i])); kc_array_push(p, st->values[i]); kc_array_push(r, p); }
            return r;
        }
        case M_HAS:
            arity(nargs, 1, 1, "has(key)", pos);
            if (args[0].type != VAL_STRING) kc_error_at(pos, "has: the key must be a string");
            return kc_bool(kc_value_in(NULL, args[0], o, pos));
        case M_GET: {
            arity(nargs, 1, 2, "get(key [, default])", pos);
            if (args[0].type != VAL_STRING) kc_error_at(pos, "get: the key must be a string");
            for (int i = 0; i < st->count; i++) if (strcmp(st->names[i], args[0].as.s->data) == 0) return st->values[i];
            return nargs == 2 ? args[1] : kc_null();
        }
        case M_REMOVE:
            arity(nargs, 1, 1, "remove(key)", pos);
            if (!kc_is_object(o)) kc_error_at(pos, "cannot remove a field from a struct (only from objects)");
            if (args[0].type != VAL_STRING) kc_error_at(pos, "remove: the key must be a string");
            return kc_bool(kc_object_remove(o, args[0].as.s->data, (size_t)args[0].as.s->len));
        default: {                                              /* M_MERGE: a new object, right side wins */
            arity(nargs, 1, 1, "merge(other_object)", pos);
            KcValue b = obj_of(args[0], pos, "merge"), r = kc_object_new();
            for (int i = 0; i < st->count; i++) kc_object_set(r, st->names[i], strlen(st->names[i]), st->values[i]);
            for (int i = 0; i < b.as.strct->count; i++) kc_object_set(r, b.as.strct->names[i], strlen(b.as.strct->names[i]), b.as.strct->values[i]);
            return r;
        }
    }
}

bool kc_lib_method(KcInterp *ip, KcValue obj, int mid, KcValue *args, int nargs, KcPos pos, KcValue *out) {
    switch (obj.type) {
        case VAL_STRING:
            if (mid == M_REPEAT || mid == M_PAD_LEFT || mid == M_PAD_RIGHT || mid == M_CHARS || mid == M_LINES) { *out = str_lib(obj, mid, args, nargs, pos); return true; }
            return false;
        case VAL_ARRAY:
            switch (mid) {
                case M_MAP: case M_FILTER: case M_REDUCE: case M_FIND: case M_ANY: case M_ALL: case M_EACH: case M_SORT: case M_CONCAT:
                case M_FIRST: case M_LAST: case M_CLEAR: case M_INSERT: case M_REMOVE: case M_EXTEND: case M_SUM: case M_MIN: case M_MAX:
                    *out = arr_lib(ip, obj, mid, args, nargs, pos); return true;
                default: return false;
            }
        case VAL_STRUCT:
            switch (mid) {
                case M_KEYS: case M_VALUES: case M_ITEMS: case M_HAS: case M_GET: case M_REMOVE: case M_MERGE:
                    *out = obj_lib(obj, mid, args, nargs, pos); return true;
                default: return false;
            }
        default: return false;
    }
}

/* ---------------------------------------------------------------- free functions */

static KcValue b_assert(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    arity(n, 1, 2, "assert(condition [, message])", pos);
    if (kc_truthy(a[0])) return kc_null();
    if (n == 2) { char *m = kc_to_display_string(a[1]); char buf[400]; snprintf(buf, sizeof buf, "%s", m); free(m); kc_error_at(pos, "assertion failed: %s", buf); }
    kc_error_at(pos, "assertion failed");
    return kc_null();
}

static KcValue b_assert_eq(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    arity(n, 2, 3, "assert_eq(actual, expected [, message])", pos);
    if (kc_equal(a[0], a[1])) return kc_null();
    char *x = kc_to_display_string(a[0]), *y = kc_to_display_string(a[1]), *m = n == 3 ? kc_to_display_string(a[2]) : NULL;
    char buf[500];
    snprintf(buf, sizeof buf, "%s%sexpected %s, got %s", m ? m : "", m ? ": " : "", y, x);
    free(x); free(y); free(m);
    kc_error_at(pos, "assertion failed: %s", buf);
    return kc_null();
}

static KcValue b_error(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    arity(n, 1, 1, "error(message) -> an error object for `throw`", pos);
    char *m = kc_to_display_string(a[0]);
    KcValue e = kc_error_object(m, pos, "error");
    free(m);
    return e;
}

static KcValue b_idiv(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    arity(n, 2, 2, "idiv(a, b): floor division", pos);
    if (a[0].type == VAL_INT && a[1].type == VAL_INT) {
        if (a[1].as.i == 0) kc_error_at(pos, "division by zero");
        int64_t q = a[0].as.i / a[1].as.i;
        if ((a[0].as.i % a[1].as.i != 0) && ((a[0].as.i < 0) != (a[1].as.i < 0))) q--;
        return kc_int(q);
    }
    if ((a[0].type == VAL_INT || a[0].type == VAL_FLOAT) && (a[1].type == VAL_INT || a[1].type == VAL_FLOAT)) {
        double x = a[0].type == VAL_INT ? (double)a[0].as.i : a[0].as.f, y = a[1].type == VAL_INT ? (double)a[1].as.i : a[1].as.f;
        if (y == 0) kc_error_at(pos, "division by zero");
        return kc_float(floor(x / y));
    }
    kc_error_at(pos, "idiv needs numbers");
    return kc_null();
}

static KcValue b_abs(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    arity(n, 1, 1, "abs(number)", pos);
    if (a[0].type == VAL_INT) return kc_int(a[0].as.i < 0 ? (int64_t)(0 - (uint64_t)a[0].as.i) : a[0].as.i);
    if (a[0].type == VAL_FLOAT) return kc_float(fabs(a[0].as.f));
    kc_error_at(pos, "abs needs a number, got a value of type %s", kc_type_name(a[0]));
    return kc_null();
}

static KcValue b_minmax_impl(KcValue *a, int n, KcPos pos, bool is_min) {
    KcValue *items = a; int cnt = n;
    if (n == 1 && a[0].type == VAL_ARRAY) { items = a[0].as.arr->items; cnt = a[0].as.arr->count; }
    if (cnt == 0) kc_error_at(pos, "%s of nothing", is_min ? "min" : "max");
    KcValue best = items[0];
    for (int i = 1; i < cnt; i++) { int c = cmp_default(items[i], best, pos); if (is_min ? c < 0 : c > 0) best = items[i]; }
    return best;
}
static KcValue b_min(KcInterp *ip, KcValue *a, int n, KcPos pos) { (void)ip; return b_minmax_impl(a, n, pos, true); }
static KcValue b_max(KcInterp *ip, KcValue *a, int n, KcPos pos) { (void)ip; return b_minmax_impl(a, n, pos, false); }

static KcValue b_clamp(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    arity(n, 3, 3, "clamp(x, lo, hi)", pos);
    if (cmp_default(a[1], a[2], pos) > 0) kc_error_at(pos, "clamp: lo is greater than hi");
    if (cmp_default(a[0], a[1], pos) < 0) return a[1];
    if (cmp_default(a[0], a[2], pos) > 0) return a[2];
    return a[0];
}

static KcValue b_ord(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    arity(n, 1, 1, "ord(string): the Unicode code point of the first character", pos);
    if (a[0].type != VAL_STRING || a[0].as.s->len == 0) kc_error_at(pos, "ord needs a non-empty string");
    const unsigned char *p = (const unsigned char *)a[0].as.s->data;
    int extra = p[0] >= 0xF0 ? 3 : p[0] >= 0xE0 ? 2 : p[0] >= 0xC0 ? 1 : 0;
    unsigned long cp = extra == 3 ? (p[0] & 7u) : extra == 2 ? (p[0] & 15u) : extra == 1 ? (p[0] & 31u) : p[0];
    for (int i = 1; i <= extra && i < a[0].as.s->len; i++) cp = (cp << 6) | (p[i] & 0x3Fu);
    return kc_int((int64_t)cp);
}

static KcValue b_chr(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    arity(n, 1, 1, "chr(code_point) -> a one-character string", pos);
    if (a[0].type != VAL_INT || a[0].as.i < 0 || a[0].as.i > 0x10FFFF) kc_error_at(pos, "chr needs a Unicode code point (0..0x10FFFF)");
    unsigned long cp = (unsigned long)a[0].as.i; char tmp[4]; int k;
    if (cp < 0x80) { tmp[0] = (char)cp; k = 1; }
    else if (cp < 0x800) { tmp[0] = (char)(0xC0 | (cp >> 6)); tmp[1] = (char)(0x80 | (cp & 0x3F)); k = 2; }
    else if (cp < 0x10000) { tmp[0] = (char)(0xE0 | (cp >> 12)); tmp[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); tmp[2] = (char)(0x80 | (cp & 0x3F)); k = 3; }
    else { tmp[0] = (char)(0xF0 | (cp >> 18)); tmp[1] = (char)(0x80 | ((cp >> 12) & 0x3F)); tmp[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); tmp[3] = (char)(0x80 | (cp & 0x3F)); k = 4; }
    char *d; KcValue r = kc_string_alloc(k, &d); memcpy(d, tmp, (size_t)k);
    return r;
}

/* Free-function forms of the methods:  map(a, f)  ==  a.map(f) */
#define DELEGATE(NAME, MID, KIND_CHECK, EXPECT)                                                          \
    static KcValue b_##NAME(KcInterp *ip, KcValue *a, int n, KcPos pos) {                               \
        if (n < 1) kc_error_at(pos, #NAME ": missing the " EXPECT " argument");                          \
        if (!(KIND_CHECK)) kc_error_at(pos, #NAME " needs " EXPECT " as its first argument, got a value of type %s", kc_type_name(a[0])); \
        KcValue out; kc_lib_method(ip, a[0], MID, a + 1, n - 1, pos, &out); return out;                 \
    }
DELEGATE(map, M_MAP, a[0].type == VAL_ARRAY, "an array")
DELEGATE(filter, M_FILTER, a[0].type == VAL_ARRAY, "an array")
DELEGATE(reduce, M_REDUCE, a[0].type == VAL_ARRAY, "an array")
DELEGATE(find, M_FIND, a[0].type == VAL_ARRAY, "an array")
DELEGATE(any, M_ANY, a[0].type == VAL_ARRAY, "an array")
DELEGATE(all, M_ALL, a[0].type == VAL_ARRAY, "an array")
DELEGATE(each, M_EACH, a[0].type == VAL_ARRAY, "an array")
DELEGATE(sum, M_SUM, a[0].type == VAL_ARRAY, "an array")
DELEGATE(keys, M_KEYS, a[0].type == VAL_STRUCT, "an object or struct")
DELEGATE(values, M_VALUES, a[0].type == VAL_STRUCT, "an object or struct")
DELEGATE(items, M_ITEMS, a[0].type == VAL_STRUCT, "an object or struct")
DELEGATE(has, M_HAS, a[0].type == VAL_STRUCT, "an object or struct")

/* sorted(a [, cmp]) returns a sorted COPY (a.sort() sorts in place) */
static KcValue b_sorted(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    arity(n, 1, 2, "sorted(array [, comparator])", pos);
    if (a[0].type != VAL_ARRAY) kc_error_at(pos, "sorted needs an array, got a value of type %s", kc_type_name(a[0]));
    KcValue r = kc_array_new();
    for (int i = 0; i < a[0].as.arr->count; i++) kc_array_push(r, a[0].as.arr->items[i]);
    sort_values(ip, r.as.arr->items, r.as.arr->count, n == 2 ? a[1] : kc_null(), pos);
    return r;
}

static KcValue b_zip(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    arity(n, 2, 2, "zip(a, b): pairs elements, stopping at the shorter array", pos);
    if (a[0].type != VAL_ARRAY || a[1].type != VAL_ARRAY) kc_error_at(pos, "zip needs two arrays");
    int m = a[0].as.arr->count < a[1].as.arr->count ? a[0].as.arr->count : a[1].as.arr->count;
    KcValue r = kc_array_new();
    for (int i = 0; i < m; i++) { KcValue p = kc_array_new(); kc_array_push(p, a[0].as.arr->items[i]); kc_array_push(p, a[1].as.arr->items[i]); kc_array_push(r, p); }
    return r;
}

/* Method names for completion: kind 0 = string, 1 = array, 2 = object/struct. Returns how many were stored. */
int kc_method_names(int kind, const char **out, int cap) {
    int lo = kind == 0 ? M_UPPER : kind == 1 ? M_PUSH : M_KEYS, hi = kind == 0 ? M_LINES : kind == 1 ? M_MAX : M_MERGE, n = 0;
    for (size_t i = 0; i < sizeof MTAB / sizeof MTAB[0] && n < cap; i++)
        if (MTAB[i].id >= lo && MTAB[i].id <= hi) out[n++] = MTAB[i].name;
    return n;
}

void kc_register_lib_builtins(KcInterp *ip) {
    static const struct { const char *name; KcNativeFn fn; } fns[] = {
        {"assert", b_assert}, {"assert_eq", b_assert_eq}, {"error", b_error}, {"idiv", b_idiv}, {"abs", b_abs},
        {"min", b_min}, {"max", b_max}, {"clamp", b_clamp}, {"ord", b_ord}, {"chr", b_chr},
        {"map", b_map}, {"filter", b_filter}, {"reduce", b_reduce}, {"find", b_find}, {"any", b_any}, {"all", b_all},
        {"each", b_each}, {"sum", b_sum}, {"sorted", b_sorted}, {"zip", b_zip},
        {"keys", b_keys}, {"values", b_values}, {"items", b_items}, {"has", b_has},
    };
    for (size_t i = 0; i < sizeof fns / sizeof fns[0]; i++) {
        KcValue v; v.type = VAL_NATIVE; v.as.native = fns[i].fn;
        kc_env_define(ip->builtins_env, fns[i].name, v, false);
    }
}
