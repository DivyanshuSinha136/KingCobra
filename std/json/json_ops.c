/* KingCobra std.json — operations on parsed values.
 *
 *  - object/array helpers: get / set / has / remove / keys / values / items / pick / omit
 *  - paths: "a.b[0].c", ["quoted key"], [-1] (from the end), and RFC 6901 pointers ("/a/0/b")
 *  - deep_equal / deep_copy / sort_keys / merge
 *  - RFC 7396 merge-patch, RFC 6902 patch (add remove replace move copy test) and diff
 *  - flatten / unflatten (round-trips through the path syntax)
 *
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com>
 */
#include "json_internal.h"
#include <math.h>
#include <ctype.h>

/* ============================================================ type helpers */

const char *jv_type_name(KcValue v) {
    switch (v.type) {
        case VAL_NULL: return "null";
        case VAL_BOOL: return "bool";
        case VAL_INT: return "int";
        case VAL_FLOAT: return "float";
        case VAL_STRING: return "string";
        case VAL_ARRAY: return "array";
        case VAL_STRUCT: return "object";
        default: return NULL;
    }
}

bool jv_is_container_obj(KcValue v) { return v.type == VAL_STRUCT; }

bool jv_equal(KcValue a, KcValue b) {
    if ((a.type == VAL_INT || a.type == VAL_FLOAT) && (b.type == VAL_INT || b.type == VAL_FLOAT)) {
        if (a.type == VAL_INT && b.type == VAL_INT) return a.as.i == b.as.i;
        double x = a.type == VAL_INT ? (double)a.as.i : a.as.f;
        double y = b.type == VAL_INT ? (double)b.as.i : b.as.f;
        return x == y;
    }
    if (a.type != b.type) return false;
    switch (a.type) {
        case VAL_NULL: return true;
        case VAL_BOOL: return a.as.b == b.as.b;
        case VAL_STRING: return a.as.s->len == b.as.s->len && memcmp(a.as.s->data, b.as.s->data, (size_t)a.as.s->len) == 0;
        case VAL_ARRAY: {
            if (a.as.arr == b.as.arr) return true;
            if (a.as.arr->count != b.as.arr->count) return false;
            for (int i = 0; i < a.as.arr->count; i++)
                if (!jv_equal(a.as.arr->items[i], b.as.arr->items[i])) return false;
            return true;
        }
        case VAL_STRUCT: {
            if (a.as.strct == b.as.strct) return true;
            if (a.as.strct->count != b.as.strct->count) return false;
            for (int i = 0; i < a.as.strct->count; i++) {
                KcValue *o = kc_object_get(b, a.as.strct->names[i], strlen(a.as.strct->names[i]));
                if (!o || !jv_equal(a.as.strct->values[i], *o)) return false;
            }
            return true;
        }
        default: return kc_equal(a, b);
    }
}

static KcValue str_dup_value(const char *s, size_t n) {
    char *d = kc_malloc(n + 1);
    if (n) memcpy(d, s, n);
    d[n] = 0;
    return kc_string_adopt(d, (int)n);
}

static KcValue copy_rec(KcValue v, int depth, bool *too_deep) {
    if (depth > 512) { *too_deep = true; return kc_null(); }
    switch (v.type) {
        case VAL_ARRAY: {
            KcValue r = kc_array_new();
            for (int i = 0; i < v.as.arr->count; i++) kc_array_push(r, copy_rec(v.as.arr->items[i], depth + 1, too_deep));
            return r;
        }
        case VAL_STRUCT: {
            KcStructInst *s = v.as.strct;
            KcStructInst *c = kc_struct_alloc();
            c->type_name = s->type_name;
            c->count = s->count;
            c->names = kc_malloc(sizeof(char *) * (size_t)(s->count ? s->count : 1));
            c->values = kc_malloc(sizeof(KcValue) * (size_t)(s->count ? s->count : 1));
            for (int i = 0; i < s->count; i++) {
                c->names[i] = kc_strdup(s->names[i]);
                c->values[i] = copy_rec(s->values[i], depth + 1, too_deep);
            }
            KcValue r; r.type = VAL_STRUCT; r.as.strct = c;
            return r;
        }
        case VAL_STRING: return str_dup_value(v.as.s->data, (size_t)v.as.s->len);
        default: return v;
    }
}

KcValue jv_copy(KcValue v) {
    bool deep = false;
    KcValue r = copy_rec(v, 0, &deep);
    return deep ? kc_null() : r;
}

/* ============================================================ argument helpers */

static void need(int n, int lo, int hi, KcPos pos, const char *sig) {
    if (n < lo || n > hi) kc_error_at(pos, "usage: json.%s", sig);
}
static KcString *get_str(KcValue v, KcPos pos, const char *who, const char *what) {
    if (v.type != VAL_STRING) kc_error_at(pos, "%s: %s must be a string, got %s", who, what, kc_type_name(v));
    return v.as.s;
}
static void need_object(KcValue v, KcPos pos, const char *who) {
    if (v.type != VAL_STRUCT) kc_error_at(pos, "%s: expected an object, got %s", who, kc_type_name(v));
}
static KcValue deep_copy_checked(KcValue v, KcPos pos, const char *who) {
    bool deep = false;
    KcValue r = copy_rec(v, 0, &deep);
    if (deep) kc_error_at(pos, "%s: value nests too deeply (cyclic structure?)", who);
    return r;
}

/* ============================================================ path tokens */

static void tok_add(PPath *p, int *cap, const char *s, size_t n, bool bracket) {
    if (p->count == *cap) { *cap = *cap ? *cap * 2 : 8; p->t = kc_realloc(p->t, sizeof(PTok) * (size_t)*cap); }
    char *c = kc_malloc(n + 1);
    memcpy(c, s, n); c[n] = 0;
    p->t[p->count].s = c; p->t[p->count].n = n; p->t[p->count].bracket = bracket;
    p->count++;
}

#define PFAIL(...) do { snprintf(err, errsz, __VA_ARGS__); return false; } while (0)

bool jpath_parse(const char *s, size_t n, PPath *out, char *err, size_t errsz) {
    out->t = NULL; out->count = 0;
    int cap = 0;
    if (n == 0) return true;
    if (s[0] == '/') {                                   /* RFC 6901 JSON Pointer */
        size_t i = 1;
        for (;;) {
            JBuf b; jb_init(&b);
            while (i < n && s[i] != '/') {
                if (s[i] == '~') {
                    if (i + 1 < n && s[i + 1] == '0') { jb_putc(&b, '~'); i += 2; continue; }
                    if (i + 1 < n && s[i + 1] == '1') { jb_putc(&b, '/'); i += 2; continue; }
                    free(b.p);
                    PFAIL("invalid pointer escape '~' at offset %zu (use ~0 for '~' and ~1 for '/')", i);
                }
                jb_putc(&b, s[i++]);
            }
            size_t kl; char *k = jb_take(&b, &kl);
            tok_add(out, &cap, k, kl, false);
            free(k);
            if (i >= n) break;
            i++;                                          /* skip '/' */
        }
        return true;
    }
    size_t i = 0;
    bool dollar = false;
    if (s[0] == '$') { i = 1; dollar = true; }
    bool at_start = true;
    while (i < n) {
        if (s[i] == '[') {
            i++;
            if (i >= n) PFAIL("unterminated '[' in path");
            if (s[i] == '"' || s[i] == '\'') {
                char q = s[i++];
                JBuf b; jb_init(&b);
                bool closed = false;
                while (i < n) {
                    char c = s[i];
                    if (c == '\\' && i + 1 < n) {
                        char e = s[i + 1];
                        jb_putc(&b, e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e);
                        i += 2; continue;
                    }
                    if (c == q) { closed = true; i++; break; }
                    jb_putc(&b, c); i++;
                }
                if (!closed) { free(b.p); PFAIL("unterminated quoted key in path"); }
                if (i >= n || s[i] != ']') { free(b.p); PFAIL("expected ']' after quoted key in path"); }
                i++;
                size_t kl; char *k = jb_take(&b, &kl);
                tok_add(out, &cap, k, kl, false);
                free(k);
            } else {
                size_t st = i;
                if (s[i] == '-') i++;
                size_t ds = i;
                while (i < n && isdigit((unsigned char)s[i])) i++;
                if (i == ds) PFAIL("expected an index, or a quoted key, inside [...] in path (wildcards need json.query)");
                if (i - ds > 18) PFAIL("index too large in path");
                if (i >= n || s[i] != ']') PFAIL("expected ']' after index in path");
                tok_add(out, &cap, s + st, i - st, true);
                i++;
            }
        } else if (s[i] == '.') {
            i++;
            if (i >= n) PFAIL("path ends with '.'");
            if (s[i] == '.' || s[i] == '[') PFAIL("empty key in path (use [\"...\"] for keys that are empty or contain '.' or '[')");
            size_t st = i;
            while (i < n && s[i] != '.' && s[i] != '[') i++;
            tok_add(out, &cap, s + st, i - st, false);
        } else if (at_start && !dollar) {
            size_t st = i;
            while (i < n && s[i] != '.' && s[i] != '[') i++;
            tok_add(out, &cap, s + st, i - st, false);
        } else {
            PFAIL("unexpected '%c' in path (expected '.' or '[')", s[i]);
        }
        at_start = false;
    }
    return true;
}

/* Index for a token against an array of length len. mode 0: element must exist.
   mode 1: may be one-past-the-end / "-" (append). Returns -1 if unusable. */
static int64_t tok_index(const PTok *t, int len, int mode) {
    if (t->n == 0) return -1;
    if (mode == 1 && t->n == 1 && t->s[0] == '-') return len;
    size_t i = 0; bool neg = false;
    if (t->s[0] == '-') { if (!t->bracket) return -1; neg = true; i = 1; }
    if (i >= t->n || t->n - i > 18) return -1;
    int64_t v = 0;
    for (; i < t->n; i++) {
        if (!isdigit((unsigned char)t->s[i])) return -1;
        v = v * 10 + (t->s[i] - '0');
    }
    if (neg) { v = len - v; if (v < 0 || v >= len) return -1; return v; }
    if (mode == 0) return v < len ? v : -1;
    if (mode == 2) return v;                               /* set_path: any index, gaps are padded with null */
    return v <= len ? v : -1;
}

static KcValue g_root_tmp;
KcValue *jpath_resolve(KcValue doc, const PPath *p) {
    g_root_tmp = doc;
    KcValue *slot = &g_root_tmp;
    for (int i = 0; i < p->count; i++) {
        const PTok *t = &p->t[i];
        if (slot->type == VAL_STRUCT) {
            KcValue *c = kc_object_get(*slot, t->s, t->n);
            if (!c) return NULL;
            slot = c;
        } else if (slot->type == VAL_ARRAY) {
            int64_t ix = tok_index(t, slot->as.arr->count, 0);
            if (ix < 0) return NULL;
            slot = &slot->as.arr->items[ix];
        } else return NULL;
    }
    return slot;
}

static void parse_path_or_die(KcValue pv, KcPos pos, const char *who, PPath *out) {
    KcString *s = get_str(pv, pos, who, "path");
    char err[160];
    if (!jpath_parse(s->data, (size_t)s->len, out, err, sizeof err)) kc_error_at(pos, "%s: %s", who, err);
}

/* ---- insertion into a container at a token (used by set_path and patch add) ---- */

/* Sets/adds child `tok` of container *c. insert_shift: arrays insert (RFC 6902 add) instead of replace. */
static bool put_child(KcValue c, const PTok *t, KcValue v, bool insert_shift, bool pad, char *err, size_t errsz) {
    if (c.type == VAL_STRUCT) {
        if (!kc_is_object(c) && !kc_object_get(c, t->s, t->n)) PFAIL("struct '%s' has no field '%.*s'", c.as.strct->type_name, (int)t->n, t->s);
        kc_object_set(c, t->s, t->n, v);
        return true;
    }
    if (c.type == VAL_ARRAY) {
        KcArray *a = c.as.arr;
        int64_t ix = tok_index(t, a->count, pad ? 2 : 1);
        if (ix < 0 || ix > 100000000) PFAIL("index '%.*s' is out of range for an array of length %d", (int)t->n, t->s, a->count);
        while (pad && ix > a->count) kc_array_push(c, kc_null());
        if (ix == a->count) { kc_array_push(c, v); return true; }
        if (insert_shift) {
            kc_array_push(c, kc_null());
            for (int i = a->count - 1; i > (int)ix; i--) a->items[i] = a->items[i - 1];
        }
        a->items[ix] = v;
        return true;
    }
    PFAIL("cannot add a child to a %s", kc_type_name(c));
}

static bool remove_child(KcValue c, const PTok *t, char *err, size_t errsz) {
    if (c.type == VAL_STRUCT) {
        if (!kc_is_object(c)) PFAIL("cannot remove a field from struct '%s'", c.as.strct->type_name);
        if (!kc_object_remove(c, t->s, t->n)) PFAIL("key '%.*s' does not exist", (int)t->n, t->s);
        return true;
    }
    if (c.type == VAL_ARRAY) {
        KcArray *a = c.as.arr;
        int64_t ix = tok_index(t, a->count, 0);
        if (ix < 0) PFAIL("index '%.*s' does not exist (array length %d)", (int)t->n, t->s, a->count);
        for (int i = (int)ix + 1; i < a->count; i++) a->items[i - 1] = a->items[i];
        a->count--;
        return true;
    }
    PFAIL("cannot remove a child of a %s", kc_type_name(c));
}

/* set_path core: creates intermediate containers; returns the new root (non-NULL result.ok). */
static bool set_impl(KcValue *root, const PPath *p, KcValue val, char *err, size_t errsz) {
    if (p->count == 0) { *root = val; return true; }
    if (root->type == VAL_NULL) {
        const PTok *t0 = &p->t[0];
        *root = (t0->bracket && t0->s[0] != '-') ? kc_array_new() : kc_object_new();
    }
    KcValue cur = *root;
    for (int i = 0; i < p->count - 1; i++) {
        const PTok *t = &p->t[i];
        KcValue *slot = NULL;
        if (cur.type == VAL_STRUCT) slot = kc_object_get(cur, t->s, t->n);
        else if (cur.type == VAL_ARRAY) { int64_t ix = tok_index(t, cur.as.arr->count, 0); if (ix >= 0) slot = &cur.as.arr->items[ix]; }
        else PFAIL("cannot descend into a %s at '%.*s'", kc_type_name(cur), (int)t->n, t->s);
        if (slot && slot->type != VAL_NULL) {
            if (slot->type != VAL_STRUCT && slot->type != VAL_ARRAY)
                PFAIL("'%.*s' holds a %s, not an object or array", (int)t->n, t->s, kc_type_name(*slot));
            cur = *slot;
            continue;
        }
        const PTok *nx = &p->t[i + 1];
        KcValue child = (nx->bracket && nx->s[0] != '-') ? kc_array_new() : kc_object_new();
        if (!put_child(cur, t, child, false, true, err, errsz)) return false;
        cur = child;
    }
    return put_child(cur, &p->t[p->count - 1], val, false, true, err, errsz);
}

/* ============================================================ basic helpers (module functions) */

static KcValue o_type_of(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 1, pos, "type_of(value) -> \"null\"|\"bool\"|\"int\"|\"float\"|\"string\"|\"array\"|\"object\"");
    const char *t = jv_type_name(a[0]);
    if (!t) kc_error_at(pos, "json.type_of: %s is not a JSON value", kc_type_name(a[0]));
    return kc_string(t);
}
static KcValue o_is_object(KcInterp *ip, KcValue *a, int n, KcPos pos) { need(n, 1, 1, pos, "is_object(value) -> bool"); return kc_bool(a[0].type == VAL_STRUCT); }
static KcValue o_is_array(KcInterp *ip, KcValue *a, int n, KcPos pos) { need(n, 1, 1, pos, "is_array(value) -> bool"); return kc_bool(a[0].type == VAL_ARRAY); }

static KcValue o_object(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 0, 1, pos, "object([pairs]) -> object   (pairs: [[key, value], ...])");
    KcValue o = kc_object_new();
    if (n == 1) {
        if (a[0].type != VAL_ARRAY) kc_error_at(pos, "json.object: pairs must be an array of [key, value] arrays");
        for (int i = 0; i < a[0].as.arr->count; i++) {
            KcValue p = a[0].as.arr->items[i];
            if (p.type != VAL_ARRAY || p.as.arr->count != 2 || p.as.arr->items[0].type != VAL_STRING)
                kc_error_at(pos, "json.object: pair %d must be [string key, value]", i);
            KcString *k = p.as.arr->items[0].as.s;
            kc_object_set(o, k->data, (size_t)k->len, p.as.arr->items[1]);
        }
    }
    return o;
}

static KcValue o_keys(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 1, pos, "keys(object) -> string[]");
    need_object(a[0], pos, "json.keys");
    KcValue r = kc_array_new();
    for (int i = 0; i < a[0].as.strct->count; i++) kc_array_push(r, kc_string(a[0].as.strct->names[i]));
    return r;
}
static KcValue o_values(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 1, pos, "values(object) -> array");
    need_object(a[0], pos, "json.values");
    KcValue r = kc_array_new();
    for (int i = 0; i < a[0].as.strct->count; i++) kc_array_push(r, a[0].as.strct->values[i]);
    return r;
}
static KcValue o_items(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 1, pos, "items(object) -> [[key, value], ...]");
    need_object(a[0], pos, "json.items");
    KcValue r = kc_array_new();
    for (int i = 0; i < a[0].as.strct->count; i++) {
        KcValue p = kc_array_new();
        kc_array_push(p, kc_string(a[0].as.strct->names[i]));
        kc_array_push(p, a[0].as.strct->values[i]);
        kc_array_push(r, p);
    }
    return r;
}

static KcValue o_has(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 2, 2, pos, "has(container, key_or_index) -> bool");
    if (a[0].type == VAL_STRUCT) {
        KcString *k = get_str(a[1], pos, "json.has", "key");
        return kc_bool(kc_object_get(a[0], k->data, (size_t)k->len) != NULL);
    }
    if (a[0].type == VAL_ARRAY) {
        if (a[1].type != VAL_INT) kc_error_at(pos, "json.has: an array index must be an int");
        int64_t i = a[1].as.i, len = a[0].as.arr->count;
        return kc_bool((i >= 0 && i < len) || (i < 0 && -i <= len));
    }
    kc_error_at(pos, "json.has: expected an object or array, got %s", kc_type_name(a[0]));
    return kc_null();
}

static KcValue o_get(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 2, 3, pos, "get(container, key_or_index[, default]) -> value");
    KcValue dflt = n > 2 ? a[2] : kc_null();
    if (a[0].type == VAL_STRUCT) {
        KcString *k = get_str(a[1], pos, "json.get", "key");
        KcValue *s = kc_object_get(a[0], k->data, (size_t)k->len);
        return s ? *s : dflt;
    }
    if (a[0].type == VAL_ARRAY) {
        if (a[1].type != VAL_INT) kc_error_at(pos, "json.get: an array index must be an int");
        int64_t i = a[1].as.i, len = a[0].as.arr->count;
        if (i < 0) i += len;
        return (i >= 0 && i < len) ? a[0].as.arr->items[i] : dflt;
    }
    kc_error_at(pos, "json.get: expected an object or array, got %s", kc_type_name(a[0]));
    return kc_null();
}

static KcValue o_set(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 3, 3, pos, "set(container, key_or_index, value) -> container   (mutates; arrays accept index == len to append)");
    if (a[0].type == VAL_STRUCT) {
        KcString *k = get_str(a[1], pos, "json.set", "key");
        if (!kc_is_object(a[0]) && !kc_object_get(a[0], k->data, (size_t)k->len))
            kc_error_at(pos, "json.set: struct '%s' has no field '%s'", a[0].as.strct->type_name, k->data);
        kc_object_set(a[0], k->data, (size_t)k->len, a[2]);
        return a[0];
    }
    if (a[0].type == VAL_ARRAY) {
        if (a[1].type != VAL_INT) kc_error_at(pos, "json.set: an array index must be an int");
        int64_t i = a[1].as.i, len = a[0].as.arr->count;
        if (i < 0) i += len;
        if (i == len) { kc_array_push(a[0], a[2]); return a[0]; }
        if (i < 0 || i > len) kc_error_at(pos, "json.set: index %lld is out of range for an array of length %lld", (long long)a[1].as.i, (long long)len);
        a[0].as.arr->items[i] = a[2];
        return a[0];
    }
    kc_error_at(pos, "json.set: expected an object or array, got %s", kc_type_name(a[0]));
    return kc_null();
}

static KcValue o_remove(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 2, 2, pos, "remove(container, key_or_index) -> bool   (true if something was removed)");
    if (a[0].type == VAL_STRUCT) {
        KcString *k = get_str(a[1], pos, "json.remove", "key");
        if (!kc_is_object(a[0])) kc_error_at(pos, "json.remove: cannot remove a field from struct '%s'", a[0].as.strct->type_name);
        return kc_bool(kc_object_remove(a[0], k->data, (size_t)k->len));
    }
    if (a[0].type == VAL_ARRAY) {
        if (a[1].type != VAL_INT) kc_error_at(pos, "json.remove: an array index must be an int");
        KcArray *arr = a[0].as.arr;
        int64_t i = a[1].as.i;
        if (i < 0) i += arr->count;
        if (i < 0 || i >= arr->count) return kc_bool(false);
        for (int j = (int)i + 1; j < arr->count; j++) arr->items[j - 1] = arr->items[j];
        arr->count--;
        return kc_bool(true);
    }
    kc_error_at(pos, "json.remove: expected an object or array, got %s", kc_type_name(a[0]));
    return kc_null();
}

static KcValue pick_omit(KcValue *a, int n, KcPos pos, bool keep, const char *who) {
    need_object(a[0], pos, who);
    if (a[1].type != VAL_ARRAY) kc_error_at(pos, "%s: keys must be an array of strings", who);
    KcValue r = kc_object_new();
    KcStructInst *s = a[0].as.strct;
    for (int i = 0; i < s->count; i++) {
        bool listed = false;
        for (int j = 0; j < a[1].as.arr->count && !listed; j++) {
            KcValue k = a[1].as.arr->items[j];
            if (k.type != VAL_STRING) kc_error_at(pos, "%s: keys must be strings", who);
            listed = (size_t)k.as.s->len == strlen(s->names[i]) && memcmp(k.as.s->data, s->names[i], (size_t)k.as.s->len) == 0;
        }
        if (listed == keep) kc_object_set(r, s->names[i], strlen(s->names[i]), s->values[i]);
    }
    (void)n;
    return r;
}
static KcValue o_pick(KcInterp *ip, KcValue *a, int n, KcPos pos) { need(n, 2, 2, pos, "pick(object, keys) -> object"); return pick_omit(a, n, pos, true, "json.pick"); }
static KcValue o_omit(KcInterp *ip, KcValue *a, int n, KcPos pos) { need(n, 2, 2, pos, "omit(object, keys) -> object"); return pick_omit(a, n, pos, false, "json.omit"); }

/* ============================================================ equal / copy / sort / merge */

static KcValue o_deep_equal(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 2, 2, pos, "deep_equal(a, b) -> bool   (key order is ignored; 1 equals 1.0)");
    return kc_bool(jv_equal(a[0], a[1]));
}
static KcValue o_deep_copy(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 1, pos, "deep_copy(value) -> value");
    return deep_copy_checked(a[0], pos, "json.deep_copy");
}

static int name_cmp(const void *x, const void *y) { return strcmp(*(char *const *)x, *(char *const *)y); }

static KcValue sort_rec(KcValue v, int depth, bool *too_deep) {
    if (depth > 512) { *too_deep = true; return kc_null(); }
    if (v.type == VAL_ARRAY) {
        KcValue r = kc_array_new();
        for (int i = 0; i < v.as.arr->count; i++) kc_array_push(r, sort_rec(v.as.arr->items[i], depth + 1, too_deep));
        return r;
    }
    if (v.type == VAL_STRUCT) {
        KcStructInst *s = v.as.strct;
        char **names = kc_malloc(sizeof(char *) * (size_t)(s->count ? s->count : 1));
        for (int i = 0; i < s->count; i++) names[i] = s->names[i];
        qsort(names, (size_t)s->count, sizeof(char *), name_cmp);
        KcValue r = kc_object_new();
        for (int i = 0; i < s->count; i++) {
            KcValue *val = kc_object_get(v, names[i], strlen(names[i]));
            kc_object_set(r, names[i], strlen(names[i]), sort_rec(*val, depth + 1, too_deep));
        }
        free(names);
        return r;
    }
    return v;
}
static KcValue o_sort_keys(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 1, pos, "sort_keys(value) -> value   (deep copy with object keys in sorted order)");
    bool deep = false;
    KcValue r = sort_rec(a[0], 0, &deep);
    if (deep) kc_error_at(pos, "json.sort_keys: value nests too deeply (cyclic structure?)");
    return r;
}

static KcValue merge_rec(KcValue base, KcValue over, KcPos pos, int depth) {
    if (depth > 512) kc_error_at(pos, "json.merge: value nests too deeply (cyclic structure?)");
    if (base.type == VAL_STRUCT && over.type == VAL_STRUCT) {
        KcValue r = deep_copy_checked(base, pos, "json.merge");
        if (!kc_is_object(r)) {                      /* declared struct: merge into an open object instead */
            KcValue o = kc_object_new();
            for (int i = 0; i < r.as.strct->count; i++) kc_object_set(o, r.as.strct->names[i], strlen(r.as.strct->names[i]), r.as.strct->values[i]);
            r = o;
        }
        KcStructInst *os = over.as.strct;
        for (int i = 0; i < os->count; i++) {
            size_t kl = strlen(os->names[i]);
            KcValue *cur = kc_object_get(r, os->names[i], kl);
            if (cur && cur->type == VAL_STRUCT && os->values[i].type == VAL_STRUCT)
                kc_object_set(r, os->names[i], kl, merge_rec(*cur, os->values[i], pos, depth + 1));
            else kc_object_set(r, os->names[i], kl, deep_copy_checked(os->values[i], pos, "json.merge"));
        }
        return r;
    }
    return deep_copy_checked(over, pos, "json.merge");
}
static KcValue o_merge(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    if (n < 2) kc_error_at(pos, "usage: json.merge(a, b[, c ...]) -> value   (objects merge recursively, later wins; arrays and scalars are replaced)");
    KcValue acc = a[0];
    for (int i = 1; i < n; i++) acc = merge_rec(acc, a[i], pos, 0);
    return acc;
}

/* RFC 7396 */
static KcValue mp_rec(KcValue target, KcValue patch, KcPos pos, int depth) {
    if (depth > 512) kc_error_at(pos, "json.merge_patch: value nests too deeply (cyclic structure?)");
    if (patch.type != VAL_STRUCT) return deep_copy_checked(patch, pos, "json.merge_patch");
    KcValue r;
    if (target.type == VAL_STRUCT) {
        r = deep_copy_checked(target, pos, "json.merge_patch");
        if (!kc_is_object(r)) {
            KcValue o = kc_object_new();
            for (int i = 0; i < r.as.strct->count; i++) kc_object_set(o, r.as.strct->names[i], strlen(r.as.strct->names[i]), r.as.strct->values[i]);
            r = o;
        }
    } else r = kc_object_new();
    KcStructInst *ps = patch.as.strct;
    for (int i = 0; i < ps->count; i++) {
        size_t kl = strlen(ps->names[i]);
        if (ps->values[i].type == VAL_NULL) { kc_object_remove(r, ps->names[i], kl); continue; }
        KcValue *cur = kc_object_get(r, ps->names[i], kl);
        kc_object_set(r, ps->names[i], kl, mp_rec(cur ? *cur : kc_null(), ps->values[i], pos, depth + 1));
    }
    return r;
}
static KcValue o_merge_patch(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 2, 2, pos, "merge_patch(target, patch) -> value   (RFC 7396; null in the patch deletes a key; non-mutating)");
    return mp_rec(a[0], a[1], pos, 0);
}

/* ============================================================ path functions */

static KcValue o_get_path(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 2, 3, pos, "get_path(doc, path[, default]) -> value   (path: \"a.b[0].c\", \"x[-1]\", or a pointer \"/a/0/b\")");
    PPath p; parse_path_or_die(a[1], pos, "json.get_path", &p);
    KcValue *slot = jpath_resolve(a[0], &p);
    return slot ? *slot : (n > 2 ? a[2] : kc_null());
}
static KcValue o_has_path(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 2, 2, pos, "has_path(doc, path) -> bool");
    PPath p; parse_path_or_die(a[1], pos, "json.has_path", &p);
    return kc_bool(jpath_resolve(a[0], &p) != NULL);
}
static KcValue o_set_path(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 3, 3, pos, "set_path(doc, path, value) -> doc   (mutates; creates missing objects/arrays; returns the root)");
    PPath p; parse_path_or_die(a[1], pos, "json.set_path", &p);
    KcValue root = a[0];
    char err[160];
    if (!set_impl(&root, &p, a[2], err, sizeof err)) kc_error_at(pos, "json.set_path: %s", err);
    return root;
}
static KcValue o_remove_path(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 2, 2, pos, "remove_path(doc, path) -> bool   (true if something was removed)");
    PPath p; parse_path_or_die(a[1], pos, "json.remove_path", &p);
    if (p.count == 0) kc_error_at(pos, "json.remove_path: cannot remove the root");
    PPath parent = { p.t, p.count - 1 };
    KcValue *slot = jpath_resolve(a[0], &parent);
    if (!slot) return kc_bool(false);
    char err[160];
    return kc_bool(remove_child(*slot, &p.t[p.count - 1], err, sizeof err));
}

static KcValue o_escape_pointer(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 1, pos, "escape_pointer(key) -> string   (RFC 6901: ~ becomes ~0, / becomes ~1)");
    KcString *s = get_str(a[0], pos, "json.escape_pointer", "key");
    JBuf b; jb_init(&b);
    for (int i = 0; i < s->len; i++) {
        if (s->data[i] == '~') jb_puts(&b, "~0");
        else if (s->data[i] == '/') jb_puts(&b, "~1");
        else jb_putc(&b, s->data[i]);
    }
    return jb_to_string(&b);
}

/* ---- flatten / unflatten ---- */

static bool key_is_plain(const char *k) {
    if (!*k) return false;
    for (; *k; k++) if (*k == '.' || *k == '[' || *k == ']' || *k == '"' || *k == '\\' || *k == '\'' || *k == '$') return false;
    return true;
}

static void flatten_rec(KcValue v, JBuf *path, KcValue out, KcPos pos, int depth) {
    if (depth > 512) kc_error_at(pos, "json.flatten: value nests too deeply (cyclic structure?)");
    size_t mark = path->len;
    if (v.type == VAL_STRUCT && v.as.strct->count > 0) {
        for (int i = 0; i < v.as.strct->count; i++) {
            const char *k = v.as.strct->names[i];
            if (key_is_plain(k)) { if (path->len) jb_putc(path, '.'); jb_puts(path, k); }
            else {
                jb_puts(path, "[\"");
                for (const char *c = k; *c; c++) { if (*c == '"' || *c == '\\') jb_putc(path, '\\'); jb_putc(path, *c); }
                jb_puts(path, "\"]");
            }
            flatten_rec(v.as.strct->values[i], path, out, pos, depth + 1);
            path->len = mark;
        }
        return;
    }
    if (v.type == VAL_ARRAY && v.as.arr->count > 0) {
        for (int i = 0; i < v.as.arr->count; i++) {
            jb_printf(path, "[%d]", i);
            flatten_rec(v.as.arr->items[i], path, out, pos, depth + 1);
            path->len = mark;
        }
        return;
    }
    kc_object_set(out, path->p ? path->p : "", path->len, v);
}
static KcValue o_flatten(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 1, pos, "flatten(value) -> object   (leaf paths like \"a.b[0].c\" -> value; empty {} and [] are leaves)");
    KcValue out = kc_object_new();
    JBuf path; jb_init(&path);
    flatten_rec(a[0], &path, out, pos, 0);
    free(path.p);
    return out;
}
static KcValue o_unflatten(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 1, pos, "unflatten(object) -> value   (inverse of flatten)");
    need_object(a[0], pos, "json.unflatten");
    KcValue root = kc_null();
    KcStructInst *s = a[0].as.strct;
    for (int i = 0; i < s->count; i++) {
        PPath p; char err[160];
        if (!jpath_parse(s->names[i], strlen(s->names[i]), &p, err, sizeof err)) kc_error_at(pos, "json.unflatten: key '%s': %s", s->names[i], err);
        if (!set_impl(&root, &p, s->values[i], err, sizeof err)) kc_error_at(pos, "json.unflatten: key '%s': %s", s->names[i], err);
    }
    return root;
}

/* ============================================================ RFC 6902 patch + diff */

static void ptr_escape_into(JBuf *b, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '~') jb_puts(b, "~0");
        else if (s[i] == '/') jb_puts(b, "~1");
        else jb_putc(b, s[i]);
    }
}

static KcValue make_op(const char *op, JBuf *path, KcValue *value) {
    KcValue o = kc_object_new();
    kc_object_set(o, "op", 2, kc_string(op));
    kc_object_set(o, "path", 4, str_dup_value(path->p, path->len));
    if (value) kc_object_set(o, "value", 5, *value);
    return o;
}

static void diff_rec(KcValue a, KcValue b, JBuf *path, KcValue ops, KcPos pos, int depth) {
    if (depth > 512) kc_error_at(pos, "json.diff: value nests too deeply (cyclic structure?)");
    if (jv_equal(a, b)) return;
    size_t mark = path->len;
    if (a.type == VAL_STRUCT && b.type == VAL_STRUCT) {
        KcStructInst *as = a.as.strct, *bs = b.as.strct;
        for (int i = 0; i < as->count; i++) {
            size_t kl = strlen(as->names[i]);
            jb_putc(path, '/'); ptr_escape_into(path, as->names[i], kl);
            KcValue *bv = kc_object_get(b, as->names[i], kl);
            if (!bv) kc_array_push(ops, make_op("remove", path, NULL));
            else diff_rec(as->values[i], *bv, path, ops, pos, depth + 1);
            path->len = mark;
        }
        for (int i = 0; i < bs->count; i++) {
            size_t kl = strlen(bs->names[i]);
            if (kc_object_get(a, bs->names[i], kl)) continue;
            jb_putc(path, '/'); ptr_escape_into(path, bs->names[i], kl);
            KcValue v = deep_copy_checked(bs->values[i], pos, "json.diff");
            kc_array_push(ops, make_op("add", path, &v));
            path->len = mark;
        }
        return;
    }
    if (a.type == VAL_ARRAY && b.type == VAL_ARRAY) {
        int la = a.as.arr->count, lb = b.as.arr->count, m = la < lb ? la : lb;
        for (int i = 0; i < m; i++) {
            jb_printf(path, "/%d", i);
            diff_rec(a.as.arr->items[i], b.as.arr->items[i], path, ops, pos, depth + 1);
            path->len = mark;
        }
        for (int i = la - 1; i >= m; i--) { jb_printf(path, "/%d", i); kc_array_push(ops, make_op("remove", path, NULL)); path->len = mark; }
        for (int i = m; i < lb; i++) {
            jb_printf(path, "/%d", i);
            KcValue v = deep_copy_checked(b.as.arr->items[i], pos, "json.diff");
            kc_array_push(ops, make_op("add", path, &v));
            path->len = mark;
        }
        return;
    }
    KcValue v = deep_copy_checked(b, pos, "json.diff");
    kc_array_push(ops, make_op("replace", path, &v));
}
static KcValue o_diff(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 2, 2, pos, "diff(a, b) -> array   (an RFC 6902 patch that turns a into b)");
    KcValue ops = kc_array_new();
    JBuf path; jb_init(&path);
    diff_rec(a[0], a[1], &path, ops, pos, 0);
    free(path.p);
    return ops;
}

static const char *opt_str(KcValue o, const char *key, size_t kl) {
    KcValue *v = kc_object_get(o, key, kl);
    return (v && v->type == VAL_STRING) ? v->as.s->data : NULL;
}

static KcValue o_patch(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 2, 2, pos, "patch(doc, ops) -> value   (RFC 6902: add remove replace move copy test; non-mutating; fatal on failure)");
    if (a[1].type != VAL_ARRAY) kc_error_at(pos, "json.patch: ops must be an array of operation objects");
    KcValue root = deep_copy_checked(a[0], pos, "json.patch");
    for (int oi = 0; oi < a[1].as.arr->count; oi++) {
        KcValue op = a[1].as.arr->items[oi];
        char err[200], e2[160];
        const char *name = NULL;
        if (op.type != VAL_STRUCT || !(name = opt_str(op, "op", 2))) kc_error_at(pos, "json.patch: operation %d must be an object with an \"op\" string", oi);
        const char *path_s = opt_str(op, "path", 4);
        if (!path_s) kc_error_at(pos, "json.patch: operation %d (%s) needs a \"path\" string", oi, name);
        PPath p;
        if (path_s[0] != '/' && path_s[0] != 0) kc_error_at(pos, "json.patch: operation %d (%s): path \"%s\" is not a JSON Pointer (must start with '/')", oi, name, path_s);
        if (!jpath_parse(path_s, strlen(path_s), &p, e2, sizeof e2)) kc_error_at(pos, "json.patch: operation %d (%s): %s", oi, name, e2);

        bool is_add = strcmp(name, "add") == 0, is_rep = strcmp(name, "replace") == 0, is_rem = strcmp(name, "remove") == 0;
        bool is_mov = strcmp(name, "move") == 0, is_cpy = strcmp(name, "copy") == 0, is_tst = strcmp(name, "test") == 0;
        if (!(is_add || is_rep || is_rem || is_mov || is_cpy || is_tst))
            kc_error_at(pos, "json.patch: operation %d: unknown op \"%s\"", oi, name);

        KcValue *vslot = kc_object_get(op, "value", 5);
        if ((is_add || is_rep || is_tst) && !vslot) kc_error_at(pos, "json.patch: operation %d (%s) needs a \"value\"", oi, name);

        PPath from = {0};
        if (is_mov || is_cpy) {
            const char *fs = opt_str(op, "from", 4);
            if (!fs) kc_error_at(pos, "json.patch: operation %d (%s) needs a \"from\" pointer", oi, name);
            if (fs[0] != '/' && fs[0] != 0) kc_error_at(pos, "json.patch: operation %d (%s): \"from\" must be a JSON Pointer", oi, name);
            if (!jpath_parse(fs, strlen(fs), &from, e2, sizeof e2)) kc_error_at(pos, "json.patch: operation %d (%s): %s", oi, name, e2);
        }

        bool ok = true; err[0] = 0;
        if (is_tst) {
            KcValue *cur = jpath_resolve(root, &p);
            if (!cur) { snprintf(err, sizeof err, "path \"%s\" does not exist", path_s); ok = false; }
            else if (!jv_equal(*cur, *vslot)) { snprintf(err, sizeof err, "test failed: value at \"%s\" differs", path_s); ok = false; }
        } else if (is_rem) {
            if (p.count == 0) { snprintf(err, sizeof err, "cannot remove the root"); ok = false; }
            else {
                PPath parent = { p.t, p.count - 1 };
                KcValue *ps = jpath_resolve(root, &parent);
                if (!ps) { snprintf(err, sizeof err, "path \"%s\" does not exist", path_s); ok = false; }
                else ok = remove_child(*ps, &p.t[p.count - 1], err, sizeof err);
            }
        } else if (is_rep) {
            KcValue nv = deep_copy_checked(*vslot, pos, "json.patch");
            if (p.count == 0) root = nv;
            else {
                KcValue *cur = jpath_resolve(root, &p);
                if (!cur) { snprintf(err, sizeof err, "path \"%s\" does not exist", path_s); ok = false; }
                else *cur = nv;
            }
        } else {
            KcValue nv;
            if (is_add) nv = deep_copy_checked(*vslot, pos, "json.patch");
            else {
                KcValue *src = jpath_resolve(root, &from);
                if (!src) { snprintf(err, sizeof err, "\"from\" path does not exist"); ok = false; nv = kc_null(); }
                else if (is_cpy) nv = deep_copy_checked(*src, pos, "json.patch");
                else {                                         /* move */
                    nv = *src;
                    if (from.count == 0) { snprintf(err, sizeof err, "cannot move the root"); ok = false; }
                    else {
                        PPath fparent = { from.t, from.count - 1 };
                        KcValue *fp = jpath_resolve(root, &fparent);
                        ok = fp && remove_child(*fp, &from.t[from.count - 1], err, sizeof err);
                    }
                }
            }
            if (ok) {
                if (p.count == 0) root = nv;
                else {
                    PPath parent = { p.t, p.count - 1 };
                    KcValue *ps = jpath_resolve(root, &parent);
                    if (!ps) { snprintf(err, sizeof err, "parent of \"%s\" does not exist", path_s); ok = false; }
                    else ok = put_child(*ps, &p.t[p.count - 1], nv, true, false, err, sizeof err);
                }
            }
        }
        if (!ok) kc_error_at(pos, "json.patch: operation %d (%s %s): %s", oi, name, path_s, err);
    }
    return root;
}

/* ============================================================ registration */

KcModuleEntry json_ops_entries[] = {
    {"type_of", o_type_of}, {"is_object", o_is_object}, {"is_array", o_is_array},
    {"object", o_object}, {"keys", o_keys}, {"values", o_values}, {"items", o_items},
    {"has", o_has}, {"get", o_get}, {"set", o_set}, {"remove", o_remove},
    {"pick", o_pick}, {"omit", o_omit},
    {"deep_equal", o_deep_equal}, {"deep_copy", o_deep_copy}, {"sort_keys", o_sort_keys},
    {"merge", o_merge}, {"merge_patch", o_merge_patch},
    {"get_path", o_get_path}, {"has_path", o_has_path}, {"set_path", o_set_path}, {"remove_path", o_remove_path},
    {"escape_pointer", o_escape_pointer},
    {"flatten", o_flatten}, {"unflatten", o_unflatten},
    {"diff", o_diff}, {"patch", o_patch},
};
const int json_ops_count = (int)(sizeof json_ops_entries / sizeof json_ops_entries[0]);
