/* Language-level runtime helpers (v0.7). */
#include "lang.h"
#include "ptr.h"
#include "mem.h"
#include "env.h"

/* `x in container`: array element (by ==), substring, object key / struct field name. */
bool kc_value_in(KcInterp *ip, KcValue x, KcValue c, KcPos pos) {
    (void)ip;
    if (kc_value_freed(c)) kc_error_freed(c, pos);
    switch (c.type) {
        case VAL_ARRAY:
            for (int i = 0; i < c.as.arr->count; i++) if (kc_equal(x, c.as.arr->items[i])) return true;
            return false;
        case VAL_STRING: {
            if (x.type != VAL_STRING) kc_error_at(pos, "'in' on a string needs a string on the left, got type %s", kc_type_name(x));
            if (x.as.s->len == 0) return true;
            if (x.as.s->len > c.as.s->len) return false;
            for (int i = 0; i + x.as.s->len <= c.as.s->len; i++)
                if (memcmp(c.as.s->data + i, x.as.s->data, (size_t)x.as.s->len) == 0) return true;
            return false;
        }
        case VAL_STRUCT: {
            if (x.type != VAL_STRING) kc_error_at(pos, "'in' on an object or struct needs a string key on the left, got type %s", kc_type_name(x));
            KcStructInst *st = c.as.strct;
            if (kc_value_freed(c)) kc_error_freed(c, pos);
            for (int i = 0; i < st->count; i++)
                if ((int)strlen(st->names[i]) == x.as.s->len && memcmp(st->names[i], x.as.s->data, (size_t)x.as.s->len) == 0) return true;
            return false;
        }
        default:
            kc_error_at(pos, "cannot use 'in' with a value of type %s on the right (expected an array, string, object or struct)", kc_type_name(c));
            return false;
    }
}

static int64_t bound_arg(KcValue v, int64_t dflt, KcPos pos) {
    if (v.type == VAL_NULL) return dflt;
    if (v.type != VAL_INT) kc_error_at(pos, "slice bounds must be ints, got type %s", kc_type_name(v));
    return v.as.i;
}

/* a[lo..hi] — half-open, strict (out-of-range bounds are an error, never silently clamped).
 * Arrays give a new array (shallow copy), strings a new string (BYTE offsets, like s[i]),
 * buffers a zero-copy view. */
KcValue kc_slice_value(KcInterp *ip, KcValue o, KcValue lov, KcValue hiv, KcPos pos) {
    if (kc_value_freed(o)) kc_error_freed(o, pos);
    int64_t len;
    switch (o.type) {
        case VAL_ARRAY: len = o.as.arr->count; break;
        case VAL_STRING: len = o.as.s->len; break;
        case VAL_BUFFER: { KcValue l; if (!kc_ptr_property(ip, o, "len", pos, &l)) kc_error_at(pos, "cannot slice this buffer"); len = l.as.i; break; }
        default: kc_error_at(pos, "cannot slice a value of type %s (expected an array, string or buffer)", kc_type_name(o)); return kc_null();
    }
    int64_t lo = bound_arg(lov, 0, pos), hi = bound_arg(hiv, len, pos);
    if (lo < 0 || hi > len || lo > hi)
        kc_error_at(pos, "slice [%lld..%lld] is out of range (length %lld)", (long long)lo, (long long)hi, (long long)len);
    if (o.type == VAL_ARRAY) {
        KcValue r = kc_array_new();
        for (int64_t i = lo; i < hi; i++) kc_array_push(r, o.as.arr->items[i]);
        return r;
    }
    if (o.type == VAL_STRING) {
        char *dst;
        KcValue r = kc_string_alloc((int)(hi - lo), &dst);
        memcpy(dst, o.as.s->data + lo, (size_t)(hi - lo));
        return r;
    }
    KcValue args[2] = { kc_int(lo), kc_int(hi - lo) }, out;
    if (!kc_ptr_method(ip, o, "slice", args, 2, pos, &out)) kc_error_at(pos, "cannot slice this buffer");
    return out;
}

/* ---- impl-block methods ---- */

const char *kc_value_type_key(KcValue v) {
    if (v.type == VAL_STRUCT) return v.as.strct->type_name;
    return kc_type_name(v);
}

void kc_method_add(KcInterp *ip, const char *type, const char *name, KcValue fn, bool instance) {
    type = kc_intern(type); name = kc_intern(name);
    for (int i = 0; i < ip->nmethods; i++)
        if (ip->methods[i].type == type && ip->methods[i].name == name) {       /* redefinition replaces */
            ip->methods[i].fn = fn; ip->methods[i].instance = instance; return;
        }
    if (ip->nmethods + 1 > ip->methods_cap) {
        ip->methods_cap = ip->methods_cap ? ip->methods_cap * 2 : 16;
        ip->methods = kc_realloc(ip->methods, sizeof(KcMethodEntry) * (size_t)ip->methods_cap);
    }
    KcMethodEntry *m = &ip->methods[ip->nmethods++];
    m->type = type; m->name = name; m->fn = fn; m->instance = instance;
}

bool kc_method_find(KcInterp *ip, const char *type, const char *name, KcValue *fn, bool *instance) {
    type = kc_intern(type); name = kc_intern(name);           /* interned: a lookup is two pointer compares */
    for (int i = 0; i < ip->nmethods; i++)
        if (ip->methods[i].type == type && ip->methods[i].name == name) {
            *fn = ip->methods[i].fn; *instance = ip->methods[i].instance; return true;
        }
    return false;
}

/* ---- errors as values ---- */

KcValue kc_error_object(const char *message, KcPos pos, const char *kind) {
    KcValue o = kc_object_new();
    kc_object_set(o, "message", 7, kc_string(message));
    kc_object_set(o, "kind", 4, kc_string(kind));
    if (pos.file) {
        kc_object_set(o, "file", 4, kc_string(pos.file));
        kc_object_set(o, "line", 4, kc_int(pos.line));
        kc_object_set(o, "col", 3, kc_int(pos.col));
    }
    return o;
}

char *kc_thrown_text(KcValue v) {
    if (v.type == VAL_STRUCT && kc_is_object(v) && !kc_value_freed(v)) {
        KcValue *m = kc_object_get(v, "message", 7);
        if (m && m->type == VAL_STRING) return kc_strdup(m->as.s->data);
    }
    return kc_to_display_string(v);
}

/* ---- did-you-mean ---- */

/* Levenshtein distance, giving up (returning max+1) as soon as it must exceed `max`. */
static int edit_distance(const char *a, const char *b, int max) {
    int la = (int)strlen(a), lb = (int)strlen(b);
    if (la > 63 || lb > 63) return max + 1;
    if (abs(la - lb) > max) return max + 1;
    int prev[65], cur[65];
    for (int j = 0; j <= lb; j++) prev[j] = j;
    for (int i = 1; i <= la; i++) {
        cur[0] = i; int rowmin = cur[0];
        for (int j = 1; j <= lb; j++) {
            int c = a[i - 1] == b[j - 1] ? 0 : 1;
            int v = prev[j - 1] + c;
            if (prev[j] + 1 < v) v = prev[j] + 1;
            if (cur[j - 1] + 1 < v) v = cur[j - 1] + 1;
            cur[j] = v;
            if (v < rowmin) rowmin = v;
        }
        if (rowmin > max) return max + 1;
        memcpy(prev, cur, sizeof(int) * (size_t)(lb + 1));
    }
    return prev[lb];
}

typedef struct { const char *name; int dist; int limit; } Best;
/* "cnt" is a subsequence of "count": a strong sign of an abbreviation or a dropped letter */
static bool is_subsequence(const char *a, const char *b) {
    for (; *b && *a; b++) if (*a == *b) a++;
    return *a == 0;
}

/* Candidates are offered from the innermost scope outward, and only a STRICTLY better match
 * replaces an earlier one, so a nearby variable beats an equally-close builtin. */
static void consider(Best *b, const char *cand, const char *name) {
    if (!cand || strcmp(cand, name) == 0 || cand[0] == '_') return;
    int d = edit_distance(name, cand, b->limit);
    if (d > b->limit && strlen(name) >= 3 && strlen(cand) > strlen(name) && is_subsequence(name, cand)) d = b->limit;
    else if (d <= b->limit && strlen(name) >= 3 && is_subsequence(name, cand)) d = d > 1 ? d - 1 : d;   /* abbreviation bonus */
    if (d <= b->limit && d < b->dist) { b->dist = d; b->name = cand; }
}
/* No suggestions for one-character names: every other short name is "close", which is just noise. */
static int limit_for(const char *name) { size_t l = strlen(name); return l <= 1 ? -1 : l <= 3 ? 1 : l <= 7 ? 2 : 3; }

static const char *hint_text(const char *best) {
    static char buf[160];
    if (!best) return "";
    snprintf(buf, sizeof buf, " (did you mean '%s'?)", best);
    return buf;
}

const char *kc_hint_for_name(KcEnv *env, const char *name) {
    Best b = { NULL, 99, limit_for(name) };
    for (KcEnv *e = env; e; e = e->parent)
        for (int i = 0; i < e->count; i++) consider(&b, e->entries[i].name, name);
    return hint_text(b.name);
}

const char *kc_hint_for_field(KcStructInst *st, const char *name) {
    Best b = { NULL, 99, limit_for(name) };
    for (int i = 0; i < st->count; i++) consider(&b, st->names[i], name);
    return hint_text(b.name);
}

const char *kc_hint_for_module(const char **names, int n, const char *name) {
    Best b = { NULL, 99, limit_for(name) };
    for (int i = 0; i < n; i++) consider(&b, names[i], name);
    return hint_text(b.name);
}
