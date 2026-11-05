#include "value.h"
char *nn_handle_display(KcHandle *h); /* std/nn/nn.c */
char *kc_mmu_display(KcHandle *h);   /* std/mmu/mmu_api.c */
/* std.music_note (std/music_note/music_note.c): text for audio / score handles */
void mn_handle_display(const KcHandle *h, char *buf, size_t n);
#include <stdio.h>


#include "heap.h"

KcLiveCounts kc_live;
bool kc_any_freed = false;

KcValue kc_string_take(char *s, int len) {
    /* Ownership of `s` is unknown (some callers pass borrowed memory such as
       getenv() results), so explicit free reclaims only the header. */
    KcValue v; v.type = VAL_STRING;
    KcString *ks = kc_heap_alloc(sizeof(KcString));
    ks->data = s; ks->len = len; ks->flags = 0;
    kc_live.strings++;
    v.as.s = ks;
    return v;
}

KcValue kc_string_adopt(char *s, int len) {
    KcValue v = kc_string_take(s, len);
    v.as.s->flags = KC_STR_OWNED;
    return v;
}

KcValue kc_string_alloc(int len, char **data_out) {
    KcString *ks = kc_heap_alloc(sizeof(KcString) + (size_t)len + 1);
    ks->data = (char *)(ks + 1); ks->len = len; ks->flags = KC_STR_INLINE; ks->data[len] = 0;
    kc_live.strings++;
    *data_out = ks->data;
    KcValue v; v.type = VAL_STRING; v.as.s = ks;
    return v;
}

KcValue kc_string(const char *s) {
    int len = (int)strlen(s);
    char *dst;
    KcValue v = kc_string_alloc(len, &dst);
    memcpy(dst, s, (size_t)len);
    return v;
}

KcStructInst *kc_struct_alloc(void) {
    KcStructInst *s = kc_heap_alloc(sizeof(KcStructInst));
    s->type_name = NULL; s->names = NULL; s->values = NULL; s->count = 0; s->flags = 0;
    kc_live.structs++;
    return s;
}

const char kc_object_type_name[] = "object";

bool kc_is_object(KcValue v) {
    return v.type == VAL_STRUCT && v.as.strct->type_name == kc_object_type_name;
}

KcValue kc_object_new(void) {
    KcStructInst *s = kc_struct_alloc();
    s->type_name = (char *)kc_object_type_name;
    s->names = NULL; s->values = NULL; s->count = 0;
    KcValue v; v.type = VAL_STRUCT; v.as.strct = s;
    return v;
}

int kc_object_index(KcValue obj, const char *key, size_t klen) {
    KcStructInst *s = obj.as.strct;
    for (int i = 0; i < s->count; i++)
        if (strlen(s->names[i]) == klen && memcmp(s->names[i], key, klen) == 0) return i;
    return -1;
}

KcValue *kc_object_get(KcValue obj, const char *key, size_t klen) {
    int i = kc_object_index(obj, key, klen);
    return i < 0 ? NULL : &obj.as.strct->values[i];
}

void kc_object_set(KcValue obj, const char *key, size_t klen, KcValue v) {
    KcStructInst *s = obj.as.strct;
    int i = kc_object_index(obj, key, klen);
    if (i >= 0) { s->values[i] = v; return; }
    s->names = kc_realloc(s->names, sizeof(char *) * (size_t)(s->count + 1));
    s->values = kc_realloc(s->values, sizeof(KcValue) * (size_t)(s->count + 1));
    char *k = kc_malloc(klen + 1);
    memcpy(k, key, klen); k[klen] = 0;
    s->names[s->count] = k;
    s->values[s->count] = v;
    s->count++;
}

bool kc_object_remove(KcValue obj, const char *key, size_t klen) {
    KcStructInst *s = obj.as.strct;
    int i = kc_object_index(obj, key, klen);
    if (i < 0) return false;
    free(s->names[i]);
    for (int j = i + 1; j < s->count; j++) { s->names[j - 1] = s->names[j]; s->values[j - 1] = s->values[j]; }
    s->count--;
    return true;
}

KcValue kc_array_new(void) {
    KcValue v; v.type = VAL_ARRAY;
    KcArray *a = kc_heap_alloc(sizeof(KcArray));
    a->items = NULL; a->count = 0; a->cap = 0; a->flags = 0;
    kc_live.arrays++;
    v.as.arr = a;
    return v;
}

void kc_array_push(KcValue arrv, KcValue item) {
    KcArray *a = arrv.as.arr;
    if (a->count + 1 > a->cap) {
        a->cap = a->cap ? a->cap * 2 : 4;
        a->items = kc_realloc(a->items, sizeof(KcValue) * a->cap);
    }
    a->items[a->count++] = item;
}

bool kc_truthy_slow(KcValue v) {
    switch (v.type) {
        case VAL_NULL: return false;
        case VAL_BOOL: return v.as.b;
        case VAL_INT: return v.as.i != 0;
        case VAL_FLOAT: return v.as.f != 0.0;
        case VAL_STRING: return v.as.s->len > 0;
        case VAL_ARRAY: return v.as.arr->count > 0;
        default: return true;
    }
}

/* `==` compares arrays, structs and objects by VALUE (element by element, field by field; object
 * keys may come in any order). Two references to the same object are trivially equal. Nesting
 * deeper than 200 levels (a cycle, in practice) compares unequal rather than recursing forever. */
static bool equal_deep(KcValue a, KcValue b, int depth);
bool kc_equal(KcValue a, KcValue b) { return equal_deep(a, b, 0); }

static bool equal_deep(KcValue a, KcValue b, int depth) {
    if (a.type == VAL_INT && b.type == VAL_FLOAT) return (double)a.as.i == b.as.f;
    if (a.type == VAL_FLOAT && b.type == VAL_INT) return a.as.f == (double)b.as.i;
    if (a.type != b.type) return false;
    switch (a.type) {
        case VAL_NULL: return true;
        case VAL_BOOL: return a.as.b == b.as.b;
        case VAL_INT: return a.as.i == b.as.i;
        case VAL_FLOAT: return a.as.f == b.as.f;
        case VAL_STRING: return a.as.s->len == b.as.s->len && memcmp(a.as.s->data, b.as.s->data, a.as.s->len) == 0;
        case VAL_ARRAY: {
            if (a.as.arr == b.as.arr) return true;
            if (depth > 200 || a.as.arr->count != b.as.arr->count) return false;
            for (int i = 0; i < a.as.arr->count; i++)
                if (!equal_deep(a.as.arr->items[i], b.as.arr->items[i], depth + 1)) return false;
            return true;
        }
        case VAL_STRUCT: {
            KcStructInst *x = a.as.strct, *y = b.as.strct;
            if (x == y) return true;
            if (depth > 200 || x->count != y->count || strcmp(x->type_name, y->type_name) != 0) return false;
            for (int i = 0; i < x->count; i++) {
                int j = i;
                if (strcmp(y->names[j], x->names[i]) != 0) {          /* different order: look the key up */
                    for (j = 0; j < y->count; j++) if (strcmp(y->names[j], x->names[i]) == 0) break;
                    if (j == y->count) return false;
                }
                if (!equal_deep(x->values[i], y->values[j], depth + 1)) return false;
            }
            return true;
        }
        case VAL_NATIVE_LIB: return a.as.native_lib == b.as.native_lib;
        case VAL_PTR: return kc_ptr_equal(a, b);
        case VAL_BUFFER: return a.as.buf == b.as.buf;
        default: return a.as.closure == b.as.closure;
    }
}

const char *kc_type_name(KcValue v) {
    switch (v.type) {
        case VAL_NULL: return "null"; case VAL_BOOL: return "bool";
        case VAL_INT: return "int"; case VAL_FLOAT: return "float";
        case VAL_STRING: return "string"; case VAL_ARRAY: return "array";
        case VAL_CLOSURE: return "fn"; case VAL_NATIVE: return "native_fn";
        case VAL_MODULE: return "module"; case VAL_HANDLE: return "handle";
        case VAL_EXTERN: return "extern_fn";
        case VAL_STRUCT: return v.as.strct->type_name;
        case VAL_NATIVE_LIB: return "native_lib";
        case VAL_PTR: return "ptr"; case VAL_BUFFER: return "buffer";
    }
    return "?";
}

char *kc_to_display_string(KcValue v) {
    char buf[512];
    switch (v.type) {
        case VAL_NULL: return kc_strdup("null");
        case VAL_BOOL: return kc_strdup(v.as.b ? "true" : "false");
        case VAL_INT: snprintf(buf, sizeof buf, "%lld", (long long)v.as.i); return kc_strdup(buf);
        case VAL_FLOAT: snprintf(buf, sizeof buf, "%g", v.as.f); return kc_strdup(buf);
        case VAL_STRING: return kc_strdup(v.as.s->data);
        case VAL_ARRAY: {
            size_t cap = 64, len = 0; char *out = kc_malloc(cap);
            out[len++] = '[';
            for (int i = 0; i < v.as.arr->count; i++) {
                char *item = kc_to_display_string(v.as.arr->items[i]);
                size_t il = strlen(item);
                while (len + il + 3 >= cap) { cap *= 2; out = kc_realloc(out, cap); }
                if (i) { out[len++] = ','; out[len++] = ' '; }
                memcpy(out + len, item, il); len += il;
                free(item);
            }
            out[len++] = ']'; out[len] = 0;
            return out;
        }
        case VAL_CLOSURE: snprintf(buf, sizeof buf, "<fn %s>", v.as.closure->name ? v.as.closure->name : "anonymous"); return kc_strdup(buf);
        case VAL_NATIVE: return kc_strdup("<native fn>");
        case VAL_MODULE: snprintf(buf, sizeof buf, "<module %s>", v.as.module->name); return kc_strdup(buf);
        case VAL_HANDLE:
            if (v.as.handle->kind == H_NN) return nn_handle_display(v.as.handle);
            if (v.as.handle->kind == H_NLP) {
                static const char *nm[] = { "tfidf", "bm25", "naive_bayes", "spell" };
                intptr_t x = v.as.handle->extra;
                snprintf(buf, sizeof buf, "<nlp.%s model>", (x >= 0 && x < 4) ? nm[x] : "?");
            } else if (v.as.handle->kind == H_PROC) {
                snprintf(buf, sizeof buf, v.as.handle->ptr ? "<process pid=%ld>" : "<process freed>", (long)v.as.handle->extra);
            } else if (v.as.handle->kind == H_HTTP) {
                static const char *hn[] = { "?", "http client", "http server", "http connection" };
                intptr_t x = v.as.handle->extra;
                snprintf(buf, sizeof buf, v.as.handle->ptr ? "<%s>" : "<%s (closed)>", (x >= 1 && x <= 3) ? hn[x] : hn[0]);
            } else if (v.as.handle->kind == H_CSV) {
                snprintf(buf, sizeof buf, "<csv %s>", v.as.handle->extra == 1 ? "writer" : "reader");
            } else if (v.as.handle->kind == H_AUDIO || v.as.handle->kind == H_SCORE) {
                mn_handle_display(v.as.handle, buf, sizeof buf);
            } else if (v.as.handle->kind == H_MMU) {
                return kc_mmu_display(v.as.handle);
            } else if (v.as.handle->kind == H_FBIN) {
                snprintf(buf, sizeof buf, "<fbin stream>");
            } else snprintf(buf, sizeof buf, "<handle kind=%d>", v.as.handle->kind);
            return kc_strdup(buf);
        case VAL_EXTERN: snprintf(buf, sizeof buf, "<extern %s>", v.as.ext->name); return kc_strdup(buf);
        case VAL_STRUCT: {
            KcStructInst *s = v.as.strct;
            size_t cap = 128, len = 0;
            char *out = kc_malloc(cap);
            len += snprintf(out, cap, "%s { ", s->type_name);
            for (int i = 0; i < s->count; i++) {
                char *fv = kc_to_display_string(s->values[i]);
                size_t need = strlen(s->names[i]) + strlen(fv) + 8;
                while (len + need >= cap) { cap *= 2; out = kc_realloc(out, cap); }
                len += snprintf(out + len, cap - len, "%s%s: %s", i ? ", " : "", s->names[i], fv);
                free(fv);
            }
            if (len + 3 >= cap) { cap += 3; out = kc_realloc(out, cap); }
            len += snprintf(out + len, cap - len, " }");
            return out;
        }
        case VAL_NATIVE_LIB: snprintf(buf, sizeof buf, "<native lib %s>", v.as.native_lib->label); return kc_strdup(buf);
        case VAL_PTR: return kc_ptr_display(v);
        case VAL_BUFFER: return kc_buffer_display(v);
    }
    return kc_strdup("?");
}

void kc_print_value(KcValue v, bool nl) {
    char *s = kc_to_display_string(v);
    fputs(s, stdout);
    if (nl) fputc('\n', stdout);
    free(s);
}
