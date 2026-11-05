/* KingCobra explicit memory management.
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com>
 * License: MIT
 *
 * There is NO garbage collector. Every heap value (string, array, struct,
 * object, buffer, alloc()'d block) is released by an explicit `free`, and a
 * freed value becomes a tombstone: reads see it as empty, and the
 * interpreter reports `use after free` / `double free` precisely instead of
 * corrupting memory. Freed storage goes back to the size-class heap
 * (heap.c) and is reused. See docs/MEMORY.md.
 */
#include "ptr.h"
#include "mem.h"
#include "heap.h"
#include "kc_platform.h"

/* ---- shared error helper ---- */

static const char *kind_name(KcValue v) {
    switch (v.type) {
        case VAL_STRING: return "string";
        case VAL_ARRAY: return "array";
        case VAL_STRUCT: return kc_is_object(v) ? "object" : "struct";
        default: return kc_type_name(v);
    }
}

void kc_error_freed(KcValue v, KcPos pos) {
    kc_error_at(pos, "use after free (this %s was freed)", kind_name(v));
}

/* ---- releasing storage ---- */

static void release_value(KcValue v, bool deep, int depth, KcPos pos);

static void release_string(KcString *s) {
    kc_any_freed = true;
    size_t block = sizeof(KcString);
    if (s->flags & KC_STR_INLINE) block += (size_t)s->len + 1;
    else if (s->flags & KC_STR_OWNED) free(s->data);
    s->flags |= KC_STR_FREED;
    s->data = (char *)"";            /* a tombstone reads as the empty string */
    s->len = 0;
    kc_live.strings_freed++;
    kc_heap_defer_free(s, block);
}

static void release_array(KcArray *a, bool deep, int depth, KcPos pos) {
    kc_any_freed = true;
    KcValue *items = a->items;
    int count = a->count;
    a->items = NULL; a->count = 0; a->cap = 0;
    a->flags |= KC_OBJ_FREED;        /* mark BEFORE recursing: breaks reference cycles */
    kc_live.arrays_freed++;
    if (deep) for (int i = 0; i < count; i++) release_value(items[i], true, depth + 1, pos);
    free(items);
    kc_heap_defer_free(a, sizeof(KcArray));
}

static void release_struct(KcStructInst *st, bool deep, int depth, KcPos pos) {
    kc_any_freed = true;
    bool open_object = st->type_name == kc_object_type_name;   /* open objects own their key strings */
    char **names = st->names;
    KcValue *values = st->values;
    int count = st->count;
    st->names = NULL; st->values = NULL; st->count = 0;
    st->flags |= KC_OBJ_FREED;
    kc_live.structs_freed++;
    if (deep) for (int i = 0; i < count; i++) release_value(values[i], true, depth + 1, pos);
    if (open_object) for (int i = 0; i < count; i++) free(names[i]);
    free(names); free(values);
    kc_heap_defer_free(st, sizeof(KcStructInst));
}

/* free_deep: skips anything already freed and anything immortal, so aliases
 * and shared literals inside a structure are harmless. */
static void release_value(KcValue v, bool deep, int depth, KcPos pos) {
    if (depth > 20000) kc_error_at(pos, "free_deep: structure is nested too deeply");
    switch (v.type) {
        case VAL_STRING:
            if (!(v.as.s->flags & (KC_STR_FREED | KC_STR_IMMORTAL))) release_string(v.as.s);
            break;
        case VAL_ARRAY:
            if (!(v.as.arr->flags & KC_OBJ_FREED)) release_array(v.as.arr, deep, depth, pos);
            break;
        case VAL_STRUCT:
            if (!(v.as.strct->flags & KC_OBJ_FREED)) release_struct(v.as.strct, deep, depth, pos);
            break;
        default: break;
    }
}

/* ---- builtins ---- */

static KcValue b_free(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    if (n != 1) kc_error_at(pos, "free(value): frees a string, array, struct/object, buffer or alloc()'d pointer");
    KcValue v = a[0];
    switch (v.type) {
        case VAL_NULL: return kc_null();                       /* free(null) is a no-op, as in C */
        case VAL_STRING:
            if (v.as.s->flags & KC_STR_FREED) kc_error_at(pos, "double free (this string was already freed)");
            if (v.as.s->flags & KC_STR_IMMORTAL)
                kc_error_at(pos, "cannot free a string literal (it is static data shared by every evaluation of that expression); copy it first: `let s = \"\" + lit;`");
            release_string(v.as.s);
            return kc_null();
        case VAL_ARRAY:
            if (v.as.arr->flags & KC_OBJ_FREED) kc_error_at(pos, "double free (this array was already freed)");
            release_array(v.as.arr, false, 0, pos);
            return kc_null();
        case VAL_STRUCT:
            if (v.as.strct->flags & KC_OBJ_FREED) kc_error_at(pos, "double free (this %s was already freed)", kind_name(v));
            release_struct(v.as.strct, false, 0, pos);
            return kc_null();
        case VAL_PTR: case VAL_BUFFER:
            return kc_free_memory_value(ip, v, pos);
        default:
            kc_error_at(pos, "free: cannot free a value of type %s (only strings, arrays, structs, objects, buffers and alloc()'d pointers can be freed)", kc_type_name(v));
            return kc_null();
    }
}

static KcValue b_free_deep(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    if (n != 1) kc_error_at(pos, "free_deep(value): frees a value and everything it holds");
    if (kc_value_freed(a[0])) kc_error_at(pos, "double free (this %s was already freed)", kind_name(a[0]));
    if (a[0].type == VAL_STRING && (a[0].as.s->flags & KC_STR_IMMORTAL))
        kc_error_at(pos, "cannot free a string literal (copy it first: `\"\" + lit`)");
    release_value(a[0], true, 0, pos);
    return kc_null();
}

static KcValue b_is_freed(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    if (n != 1) kc_error_at(pos, "is_freed(value) -> bool");
    return kc_bool(kc_value_freed(a[0]) || kc_value_memory_freed(a[0]));
}

static KcValue b_mem_quarantine(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    if (n != 1 || a[0].type != VAL_INT || a[0].as.i < 0 || a[0].as.i > (1 << 24))
        kc_error_at(pos, "mem_quarantine(count: int 0..16777216): how many freed blocks are held back before reuse; returns the old setting");
    int64_t old = (int64_t)kc_heap_stats()->quarantine_cap;
    kc_heap_set_quarantine((size_t)a[0].as.i);
    return kc_int(old);
}

/* ---- accounting & leak report ---- */

typedef struct { uint64_t count, bytes; } RegionTotals;
static void sum_region(const KcRegion *r, void *ud) { RegionTotals *t = ud; t->count++; t->bytes += r->size; }

static KcValue b_mem_stats(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; (void)a;
    if (n != 0) kc_error_at(pos, "mem_stats() takes no arguments");
    const KcHeapStats *h = kc_heap_stats();
    RegionTotals rt = {0, 0};
    kc_ptr_live_regions(sum_region, &rt);
    KcValue o = kc_object_new();
    #define SET(k, v) kc_object_set(o, k, strlen(k), kc_int((int64_t)(v)))
    SET("strings_live", kc_live.strings - kc_live.strings_freed);
    SET("arrays_live", kc_live.arrays - kc_live.arrays_freed);
    SET("structs_live", kc_live.structs - kc_live.structs_freed);
    SET("strings_freed", kc_live.strings_freed);
    SET("arrays_freed", kc_live.arrays_freed);
    SET("structs_freed", kc_live.structs_freed);
    SET("regions_live", rt.count);
    SET("region_bytes_live", rt.bytes);
    SET("heap_bytes_in_use", h->bytes_in_use);
    SET("heap_bytes_reserved", h->bytes_reserved);
    SET("heap_allocs", h->allocs);
    SET("heap_frees", h->frees);
    SET("quarantined", h->quarantined);
    SET("quarantine_bytes", h->quarantine_bytes);
    #undef SET
    return o;
}

typedef struct { const KcRegion *r[4096]; int n, extra; } RegionList;
static void collect_region(const KcRegion *r, void *ud) {
    RegionList *l = ud;
    if (l->n < 4096) l->r[l->n++] = r; else l->extra++;
}
static int by_id(const void *a, const void *b) {
    const KcRegion *x = *(const KcRegion *const *)a, *y = *(const KcRegion *const *)b;
    return x->id < y->id ? -1 : x->id > y->id;
}

/* Printed by `kingcobra --leaks file.kc` at normal exit, or by mem_report(). */
void kc_mem_report(FILE *out) {
    RegionList *l = kc_malloc(sizeof(RegionList));
    l->n = 0; l->extra = 0;
    kc_ptr_live_regions(collect_region, l);
    qsort(l->r, (size_t)l->n, sizeof(l->r[0]), by_id);
    uint64_t bytes = 0;
    fprintf(out, "kingcobra: memory report\n");
    if (l->n + l->extra == 0) fprintf(out, "  no leaked alloc()/buffer() blocks\n");
    else fprintf(out, "  leaked blocks (allocated, never freed):\n");
    int shown = 0;
    for (int i = 0; i < l->n; i++) {
        const KcRegion *r = l->r[i];
        bytes += r->size;
        if (shown++ < 50)
            fprintf(out, "    %8zu bytes  %-13s at %s:%d:%d\n", r->size, r->origin ? r->origin : "?",
                    r->site.file ? r->site.file : "?", r->site.line, r->site.col);
    }
    if (l->n > 50) fprintf(out, "    ... and %d more\n", l->n - 50 + l->extra);
    if (l->n + l->extra > 0) fprintf(out, "  total: %d block(s), %llu byte(s)\n", l->n + l->extra, (unsigned long long)bytes);
    fprintf(out, "  still allocated: %llu string(s), %llu array(s), %llu struct/object(s)\n",
            (unsigned long long)(kc_live.strings - kc_live.strings_freed),
            (unsigned long long)(kc_live.arrays - kc_live.arrays_freed),
            (unsigned long long)(kc_live.structs - kc_live.structs_freed));
    const KcHeapStats *h = kc_heap_stats();
    fprintf(out, "  heap: %llu byte(s) in use, %llu reserved, %llu alloc(s), %llu free(s) (%llu byte(s) awaiting reuse)\n",
            (unsigned long long)h->bytes_in_use, (unsigned long long)h->bytes_reserved,
            (unsigned long long)h->allocs, (unsigned long long)h->frees, (unsigned long long)h->quarantine_bytes);
    free(l);
}

static KcValue b_mem_report(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; (void)a;
    if (n != 0) kc_error_at(pos, "mem_report() takes no arguments");
    fflush(stdout);
    kc_mem_report(stdout);
    return kc_null();
}

/* ---- guards used by the interpreter ---- */

/* Natives must not silently operate on tombstones. free/free_deep/is_freed
 * are exempt (they exist to report on exactly that). */
static bool accepts_freed(KcNativeFn f) {
    return f == b_free || f == b_free_deep || f == b_is_freed;
}

void kc_check_native_args(KcNativeFn f, const KcValue *args, int n, KcPos pos) {
    for (int i = 0; i < n; i++) {
        switch (args[i].type) {
            case VAL_STRING: case VAL_ARRAY: case VAL_STRUCT:
                if (kc_value_freed(args[i]) && !accepts_freed(f)) kc_error_freed(args[i], pos);
                break;
            default: break;
        }
    }
}

void kc_check_live_args(const KcValue *args, int n, KcPos pos) {
    for (int i = 0; i < n; i++) {
        switch (args[i].type) {
            case VAL_STRING: case VAL_ARRAY: case VAL_STRUCT:
                if (kc_value_freed(args[i])) kc_error_freed(args[i], pos);
                break;
            default: break;
        }
    }
}

void kc_register_mem_builtins(KcInterp *ip) {
    static const struct { const char *name; KcNativeFn fn; } fns[] = {
        {"free", b_free}, {"free_deep", b_free_deep}, {"is_freed", b_is_freed},
        {"mem_stats", b_mem_stats}, {"mem_report", b_mem_report}, {"mem_quarantine", b_mem_quarantine},
    };
    for (size_t i = 0; i < sizeof fns / sizeof fns[0]; i++) {
        KcValue v; v.type = VAL_NATIVE; v.as.native = fns[i].fn;
        kc_env_define(ip->builtins_env, fns[i].name, v, false);
    }
}
