/* KingCobra pointers, buffers and offsets — see ptr.h for the model and
 * docs/POINTERS.md for the user-facing description.
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com>
 */
#include "ptr.h"
#include "kc_platform.h"
#include <math.h>
#include <ctype.h>

/* ======================================================================
 * regions + the live-region registry
 * ====================================================================== */

static KcRegion **g_live = NULL;
static int g_nlive = 0, g_caplive = 0;
int kc_ptr_live_blocks(void) { return g_nlive; }   /* alloc()/buffer() blocks not yet freed */
static uint64_t g_next_region_id = 1;

#define KC_MAX_ALLOC ((int64_t)1 << 36) /* 64 GiB sanity cap: catches size bugs, not a real limit */

static void registry_add(KcRegion *r) {
    if (g_nlive + 1 > g_caplive) {
        g_caplive = g_caplive ? g_caplive * 2 : 32;
        g_live = kc_realloc(g_live, sizeof(KcRegion *) * (size_t)g_caplive);
    }
    r->reg_index = g_nlive;
    g_live[g_nlive++] = r;
}

static void registry_remove(KcRegion *r) {
    if (r->reg_index < 0) return;
    int i = r->reg_index;
    g_live[i] = g_live[--g_nlive];
    g_live[i]->reg_index = i;
    r->reg_index = -1;
}

static KcRegion *region_new(size_t size, bool heap, KcPos site, const char *origin) {
    KcRegion *r = kc_malloc(sizeof(KcRegion));
    r->base = kc_malloc(size ? size : 1);
    memset(r->base, 0, size ? size : 1);
    r->size = size; r->alive = true; r->owned = true; r->readonly = false;
    r->heap = heap; r->id = g_next_region_id++; r->reg_index = -1;
    r->site = site; r->origin = origin;
    registry_add(r);
    return r;
}

/* The KcRegion struct itself is deliberately never freed: a stale pointer
 * must still find a region that says "not alive" so use-after-free is a
 * clean error rather than a read of recycled memory. */
static void region_release(KcRegion *r) {
    free(r->base);
    r->base = NULL; r->size = 0; r->alive = false;
    registry_remove(r);
}

/* ======================================================================
 * scalar types
 * ====================================================================== */

static int scalar_size(KcPType k) {
    switch (k) {
        case PT_U8: case PT_I8: case PT_BOOL: return 1;
        case PT_U16: case PT_I16: return 2;
        case PT_U32: case PT_I32: case PT_F32: return 4;
        case PT_U64: case PT_I64: case PT_F64: case PT_PTR: return 8;
        default: return 0;
    }
}

static KcType g_prim[PT_STRUCT + 1];
static bool g_prim_init = false;
static KcType *prim(KcPType k) {
    if (!g_prim_init) {
        for (int i = 0; i <= PT_STRUCT; i++) { g_prim[i].k = (KcPType)i; g_prim[i].to = NULL; g_prim[i].lay = NULL; }
        g_prim_init = true;
    }
    return &g_prim[k];
}

static KcType *mk_type(KcPType k, KcType *to, KcLayout *lay) {
    KcType *t = kc_malloc(sizeof(KcType));
    t->k = k; t->to = to; t->lay = lay;
    return t;
}

static void type_str(KcType *t, char *out, size_t cap) {
    size_t len = 0;
    out[0] = 0;
    while (t && t->k == PT_PTR) { if (len + 2 < cap) { out[len++] = '*'; out[len] = 0; } t = t->to; }
    const char *base = "?";
    if (!t) base = "void";
    else switch (t->k) {
        case PT_VOID: base = "void"; break;   case PT_ANY: base = "any"; break;
        case PT_U8: base = "u8"; break;       case PT_I8: base = "i8"; break;
        case PT_U16: base = "u16"; break;     case PT_I16: base = "i16"; break;
        case PT_U32: base = "u32"; break;     case PT_I32: base = "i32"; break;
        case PT_U64: base = "u64"; break;     case PT_I64: base = "i64"; break;
        case PT_F32: base = "f32"; break;     case PT_F64: base = "f64"; break;
        case PT_BOOL: base = "bool"; break;
        case PT_STRUCT: base = t->lay->name; break;
        default: break;
    }
    snprintf(out + len, cap - len, "%s", base);
}

/* ---- type text -> KcType ---- */

typedef struct { char *key; KcType *t; } TypeCacheEntry;
static TypeCacheEntry *g_tcache = NULL;
static int g_tcache_n = 0, g_tcache_cap = 0;

static void layout_bind(KcLayout *L, KcStructTypeDef *def) {
    L->name = def->name; L->packed = def->packed; L->is_union = def->is_union;
    L->fnames = def->field_names; L->ftypes = def->field_types; L->fcount = def->field_count;
}

static KcLayout *layout_shell(KcStructTypeDef *def) {
    if (!def->layout) {
        KcLayout *L = kc_malloc(sizeof(KcLayout));
        memset(L, 0, sizeof *L);
        layout_bind(L, def);
        def->layout = L;
    }
    return (KcLayout *)def->layout;
}

/* A struct name was declared again (REPL, or a later declaration in a file). Types already
 * parsed point at the existing shell, so reset it in place rather than replacing it. */
void kc_layout_invalidate(KcStructTypeDef *def) {
    KcLayout *L = def->layout;
    if (!L) return;
    free(L->f);
    L->f = NULL; L->n = 0; L->size = 0; L->align = 0;
    L->done = false; L->computing = false;
    layout_bind(L, def);
}

/* Returns NULL (no error) if the text is not a known memory type. */
static KcType *type_lookup(KcInterp *ip, const char *text) {
    while (*text == ' ') text++;
    size_t n = strlen(text);
    while (n > 0 && text[n - 1] == ' ') n--;
    if (n == 0) return NULL;

    char *key = kc_malloc(n + 1);
    memcpy(key, text, n); key[n] = 0;
    for (int i = 0; i < g_tcache_n; i++)
        if (strcmp(g_tcache[i].key, key) == 0) { free(key); return g_tcache[i].t; }

    KcType *t = NULL;
    if (key[0] == '*') {
        const char *inner = key + 1;
        while (*inner == ' ') inner++;
        KcType *to = (*inner == 0) ? prim(PT_VOID) : type_lookup(ip, inner);
        if (to) t = mk_type(PT_PTR, to, NULL);
    } else if (strcmp(key, "u8") == 0) t = prim(PT_U8);
    else if (strcmp(key, "i8") == 0) t = prim(PT_I8);
    else if (strcmp(key, "u16") == 0) t = prim(PT_U16);
    else if (strcmp(key, "i16") == 0) t = prim(PT_I16);
    else if (strcmp(key, "u32") == 0) t = prim(PT_U32);
    else if (strcmp(key, "i32") == 0) t = prim(PT_I32);
    else if (strcmp(key, "u64") == 0 || strcmp(key, "usize") == 0) t = prim(PT_U64);
    else if (strcmp(key, "i64") == 0 || strcmp(key, "int") == 0 || strcmp(key, "isize") == 0) t = prim(PT_I64);
    else if (strcmp(key, "f32") == 0) t = prim(PT_F32);
    else if (strcmp(key, "f64") == 0 || strcmp(key, "float") == 0) t = prim(PT_F64);
    else if (strcmp(key, "bool") == 0) t = prim(PT_BOOL);
    else if (strcmp(key, "void") == 0) t = prim(PT_VOID);
    else if (strcmp(key, "any") == 0) t = prim(PT_ANY);
    else {
        KcStructTypeDef *def = kc_find_struct_type(ip, key);
        if (def) t = mk_type(PT_STRUCT, NULL, layout_shell(def));
    }
    if (!t) { free(key); return NULL; }

    if (g_tcache_n + 1 > g_tcache_cap) {
        g_tcache_cap = g_tcache_cap ? g_tcache_cap * 2 : 32;
        g_tcache = kc_realloc(g_tcache, sizeof(TypeCacheEntry) * (size_t)g_tcache_cap);
    }
    g_tcache[g_tcache_n].key = key; g_tcache[g_tcache_n].t = t; g_tcache_n++;
    return t;
}

KcType *kc_type_parse(KcInterp *ip, const char *text, KcPos pos) {
    KcType *t = type_lookup(ip, text);
    if (!t) kc_error_at(pos, "unknown type '%s' (expected u8/i8/u16/i16/u32/i32/u64/i64/f32/f64/bool/int/float/usize, a declared struct, or *T)", text);
    return t;
}

/* ---- layouts ---- */

static int64_t align_up_i(int64_t v, int64_t a) { return a <= 1 ? v : ((v + a - 1) / a) * a; }

static void layout_complete(KcInterp *ip, KcLayout *L, KcPos pos);

int64_t kc_type_size(KcInterp *ip, KcType *t, KcPos pos) {
    switch (t->k) {
        case PT_VOID: case PT_ANY: return 1;
        case PT_STRUCT: layout_complete(ip, t->lay, pos); return t->lay->size;
        default: return scalar_size(t->k);
    }
}

int64_t kc_type_align(KcInterp *ip, KcType *t, KcPos pos) {
    switch (t->k) {
        case PT_VOID: case PT_ANY: return 1;
        case PT_STRUCT: layout_complete(ip, t->lay, pos); return t->lay->align;
        default: return scalar_size(t->k);
    }
}

static void layout_complete(KcInterp *ip, KcLayout *L, KcPos pos) {
    if (L->done) return;
    if (L->computing) kc_error_at(pos, "struct '%s' contains itself by value (use a pointer field: `next: *%s`)", L->name, L->name);
    L->computing = true;
    L->n = L->fcount;
    L->f = kc_malloc(sizeof(KcField) * (size_t)(L->n ? L->n : 1));
    int64_t cur = 0, maxalign = 1;
    for (int i = 0; i < L->n; i++) {
        const char *ftext = L->ftypes ? L->ftypes[i] : "any";
        char *tbuf = kc_strdup(ftext);
        int64_t count = 1; bool is_arr = false;
        size_t tl = strlen(tbuf);
        if (tl > 0 && tbuf[tl - 1] == ']') {
            char *lb = strrchr(tbuf, '[');
            if (!lb || lb + 1 == tbuf + tl - 1)
                kc_error_at(pos, "struct '%s' field '%s': dynamic array type '%s' has no fixed memory layout (use `T[N]` with a constant N)", L->name, L->fnames[i], ftext);
            count = atoll(lb + 1);
            if (count <= 0) kc_error_at(pos, "struct '%s' field '%s': array length must be positive", L->name, L->fnames[i]);
            *lb = 0; is_arr = true;
        }
        KcType *ty = type_lookup(ip, tbuf);
        if (!ty || ty->k == PT_VOID || ty->k == PT_ANY)
            kc_error_at(pos, "struct '%s' field '%s': type '%s' has no fixed memory layout (use u8..i64, f32/f64, bool, a pointer, or another struct)", L->name, L->fnames[i], ftext);
        int64_t esize = kc_type_size(ip, ty, pos);
        int64_t fa = L->packed ? 1 : kc_type_align(ip, ty, pos);
        int64_t off = L->is_union ? 0 : align_up_i(cur, fa);
        L->f[i].name = L->fnames[i];
        L->f[i].ty = ty; L->f[i].off = off; L->f[i].count = count;
        L->f[i].esize = esize; L->f[i].is_array = is_arr;
        if (L->is_union) { if (esize * count > cur) cur = esize * count; }
        else cur = off + esize * count;
        if (fa > maxalign) maxalign = fa;
        free(tbuf);
    }
    L->align = L->packed ? 1 : maxalign;
    L->size = L->packed ? cur : align_up_i(cur, L->align);
    L->computing = false; L->done = true;
}

static KcField *layout_field(KcLayout *L, const char *name) {
    for (int i = 0; i < L->n; i++) if (strcmp(L->f[i].name, name) == 0) return &L->f[i];
    return NULL;
}

/* sizeof/alignof on type text, including fixed arrays: "u8[16]", "Header[4]" */
int64_t kc_sizeof_text(KcInterp *ip, const char *text, int which, KcPos pos) {
    char *buf = kc_strdup(text);
    size_t n = strlen(buf);
    int64_t count = 1;
    if (n > 0 && buf[n - 1] == ']') {
        char *lb = strrchr(buf, '[');
        if (!lb || lb + 1 == buf + n - 1) kc_error_at(pos, "sizeof: '%s' has no fixed size (dynamic array)", text);
        count = atoll(lb + 1);
        *lb = 0;
    }
    KcType *t = kc_type_parse(ip, buf, pos);
    free(buf);
    if (which == 1) return kc_type_align(ip, t, pos);
    return kc_type_size(ip, t, pos) * count;
}

int64_t kc_offsetof(KcInterp *ip, const char *struct_name, const char *field, KcPos pos) {
    KcStructTypeDef *def = kc_find_struct_type(ip, struct_name);
    if (!def) kc_error_at(pos, "offsetof: unknown struct '%s'", struct_name);
    KcLayout *L = layout_shell(def);
    layout_complete(ip, L, pos);
    KcField *f = layout_field(L, field);
    if (!f) kc_error_at(pos, "offsetof: struct '%s' has no field '%s'", struct_name, field);
    return f->off;
}

/* ======================================================================
 * scalar load / store (explicit endianness)
 * ====================================================================== */

static bool host_le(void) { uint16_t x = 1; return *(uint8_t *)&x == 1; }

static void copy_ordered(uint8_t *dst, const uint8_t *src, int n, bool be) {
    if (be == host_le()) { for (int i = 0; i < n; i++) dst[i] = src[n - 1 - i]; }
    else memcpy(dst, src, (size_t)n);
}

static KcValue load_scalar(KcPType k, const uint8_t *a, bool be) {
    uint8_t t[8];
    int n = scalar_size(k);
    copy_ordered(t, a, n, be);
    switch (k) {
        case PT_U8: return kc_int(t[0]);
        case PT_I8: return kc_int((int8_t)t[0]);
        case PT_BOOL: return kc_bool(t[0] != 0);
        case PT_U16: { uint16_t x; memcpy(&x, t, 2); return kc_int(x); }
        case PT_I16: { int16_t x; memcpy(&x, t, 2); return kc_int(x); }
        case PT_U32: { uint32_t x; memcpy(&x, t, 4); return kc_int(x); }
        case PT_I32: { int32_t x; memcpy(&x, t, 4); return kc_int(x); }
        case PT_U64: { uint64_t x; memcpy(&x, t, 8); return kc_int((int64_t)x); }
        case PT_I64: { int64_t x; memcpy(&x, t, 8); return kc_int(x); }
        case PT_F32: { float x; memcpy(&x, t, 4); return kc_float((double)x); }
        case PT_F64: { double x; memcpy(&x, t, 8); return kc_float(x); }
        default: return kc_null();
    }
}

static void store_scalar(KcPType k, uint8_t *a, KcValue v, bool be, KcPos pos) {
    uint8_t t[8];
    int n = scalar_size(k);
    if (k == PT_F32 || k == PT_F64) {
        double d;
        if (v.type == VAL_INT) d = (double)v.as.i;
        else if (v.type == VAL_FLOAT) d = v.as.f;
        else { kc_error_at(pos, "cannot store a %s into a float location (expected a number)", kc_type_name(v)); return; }
        if (k == PT_F32) { float f = (float)d; memcpy(t, &f, 4); } else memcpy(t, &d, 8);
    } else {
        int64_t i;
        if (v.type == VAL_INT) i = v.as.i;
        else if (v.type == VAL_BOOL) i = v.as.b ? 1 : 0;
        else if (v.type == VAL_FLOAT) { kc_error_at(pos, "cannot store a float into an integer location (convert explicitly: `x as i32`)"); return; }
        else { kc_error_at(pos, "cannot store a %s into an integer location", kc_type_name(v)); return; }
        uint64_t u = (uint64_t)i;
        switch (n) {
            case 1: t[0] = (uint8_t)(k == PT_BOOL ? (i != 0) : u); break;
            case 2: { uint16_t x = (uint16_t)u; memcpy(t, &x, 2); break; }
            case 4: { uint32_t x = (uint32_t)u; memcpy(t, &x, 4); break; }
            default: memcpy(t, &u, 8); break;
        }
    }
    copy_ordered(a, t, n, be);
}

/* ======================================================================
 * pointer construction
 * ====================================================================== */

static KcValue ptr_val(KcPtr *p) { KcValue v; v.type = VAL_PTR; v.as.ptr = p; return v; }

static KcPtr *ptr_new(KcPtrKind k, KcType *et) {
    KcPtr *p = kc_malloc(sizeof(KcPtr));
    memset(p, 0, sizeof *p);
    p->kind = k; p->et = et;
    return p;
}

static KcValue mk_region_ptr(KcRegion *r, int64_t off, int64_t lo, int64_t hi, KcType *et) {
    KcPtr *p = ptr_new(PK_REGION, et);
    p->u.mem.reg = r; p->u.mem.off = off; p->u.mem.lo = lo; p->u.mem.hi = hi;
    return ptr_val(p);
}

static KcValue mk_raw_ptr(uint64_t addr, KcType *et) {
    if (addr == 0) return kc_null();
    KcPtr *p = ptr_new(PK_RAW, et);
    p->u.raw.addr = addr;
    return ptr_val(p);
}

static KcValue clone_ptr(KcPtr *src) {
    KcPtr *p = kc_malloc(sizeof(KcPtr));
    *p = *src;
    return ptr_val(p);
}

KcValue kc_ptr_slot(KcEnv *env, int idx, bool readonly) {
    KcPtr *p = ptr_new(PK_SLOT, prim(PT_ANY));
    p->u.slot.env = env; p->u.slot.idx = idx; p->readonly = readonly;
    kc_env_capture(env); /* the scope must outlive this pointer: never recycle it */
    return ptr_val(p);
}

KcValue kc_ptr_elem_ref(KcArray *arr, int64_t idx) {
    KcPtr *p = ptr_new(PK_ELEM, prim(PT_ANY));
    p->u.elem.arr = arr; p->u.elem.idx = idx;
    return ptr_val(p);
}

KcValue kc_ptr_field_ref(KcStructInst *st, const char *name, KcPos pos) {
    for (int i = 0; i < st->count; i++) {
        if (strcmp(st->names[i], name) == 0) {
            KcPtr *p = ptr_new(PK_FIELD, prim(PT_ANY));
            p->u.field.st = st; p->u.field.idx = i;
            return ptr_val(p);
        }
    }
    kc_error_at(pos, "struct '%s' has no field '%s'", st->type_name, name);
    return kc_null();
}

KcValue kc_ptr_from_address(KcInterp *ip, uint64_t addr, const char *ptr_type_text, KcPos pos) {
    KcType *t = kc_type_parse(ip, ptr_type_text, pos);
    if (t->k != PT_PTR) kc_error_at(pos, "expected a pointer type like \"*u8\", got '%s'", ptr_type_text);
    KcType *to = t->to;
    if (addr == 0) return kc_null();
    /* an address that lies inside a live KingCobra allocation becomes a
       fully checked pointer again; anything else stays a raw address */
    for (int i = 0; i < g_nlive; i++) {
        KcRegion *r = g_live[i];
        uint64_t b = (uint64_t)(uintptr_t)r->base;
        if (r->alive && addr >= b && addr <= b + r->size)
            return mk_region_ptr(r, (int64_t)(addr - b), 0, (int64_t)r->size, to);
    }
    return mk_raw_ptr(addr, to);
}

static KcValue from_address_t(uint64_t addr, KcType *to) {
    if (addr == 0) return kc_null();
    for (int i = 0; i < g_nlive; i++) {
        KcRegion *r = g_live[i];
        uint64_t b = (uint64_t)(uintptr_t)r->base;
        if (r->alive && addr >= b && addr <= b + r->size)
            return mk_region_ptr(r, (int64_t)(addr - b), 0, (int64_t)r->size, to);
    }
    return mk_raw_ptr(addr, to);
}

/* ======================================================================
 * checked memory access
 * ====================================================================== */

static KcPtr *as_ptr(KcValue v, KcPos pos) {
    if (v.type == VAL_NULL) kc_error_at(pos, "null pointer dereference");
    if (v.type != VAL_PTR) kc_error_at(pos, "cannot dereference a %s (expected a pointer)", kc_type_name(v));
    return v.as.ptr;
}

/* Effective upper bound of a region pointer's window (a region can shrink
 * via realloc/resize after the pointer was made). */
static int64_t win_hi(KcPtr *p) {
    int64_t hi = p->u.mem.hi;
    int64_t rs = (int64_t)p->u.mem.reg->size;
    return hi < rs ? hi : rs;
}

/* Address of `size` bytes at the pointer, after every applicable check. */
static uint8_t *mem_addr(KcInterp *ip, KcPtr *p, int64_t size, bool write, KcPos pos) {
    if (p->kind == PK_REGION) {
        KcRegion *r = p->u.mem.reg;
        if (!r->alive) kc_error_at(pos, "use after free (allocation #%llu was freed)", (unsigned long long)r->id);
        if (write && r->readonly) kc_error_at(pos, "write to read-only memory");
        int64_t off = p->u.mem.off, lo = p->u.mem.lo, hi = win_hi(p);
        if (size < 0 || off < lo || off > hi || size > hi - off)
            kc_error_at(pos, "pointer %s of %lld byte(s) at offset %lld is out of bounds (valid range %lld..%lld of allocation #%llu)",
                        write ? "write" : "read", (long long)size, (long long)off, (long long)lo, (long long)hi, (unsigned long long)r->id);
        return r->base + off;
    }
    if (p->kind == PK_RAW) {
        if (!ip->in_unsafe) kc_error_at(pos, "dereferencing a raw pointer (a bare machine address) requires an `unsafe { }` block");
        kc_forbid_in_comptime(ip, pos, "raw pointer dereference");
        return (uint8_t *)(uintptr_t)p->u.raw.addr;
    }
    kc_error_at(pos, "internal: mem_addr on a non-memory pointer");
    return NULL;
}

/* ---- value <-> memory ---- */

static KcValue load_mem(KcInterp *ip, KcType *t, const uint8_t *a, KcPos pos);
static void store_mem(KcInterp *ip, KcType *t, uint8_t *a, KcValue v, KcPos pos);

static uint64_t ptr_value_address(KcValue v, KcPos pos) {
    if (v.type == VAL_NULL) return 0;
    if (v.type == VAL_INT) return (uint64_t)v.as.i;
    if (v.type == VAL_PTR) {
        KcPtr *p = v.as.ptr;
        if (p->kind == PK_RAW) return p->u.raw.addr;
        if (p->kind == PK_REGION) {
            if (!p->u.mem.reg->alive) kc_error_at(pos, "use after free (allocation #%llu was freed)", (unsigned long long)p->u.mem.reg->id);
            return (uint64_t)(uintptr_t)(p->u.mem.reg->base + p->u.mem.off);
        }
        kc_error_at(pos, "cannot store or pass a reference pointer (&variable / &array[i] / &struct.field) as a machine address");
    }
    kc_error_at(pos, "expected a pointer, got a %s", kc_type_name(v));
    return 0;
}

static KcValue load_mem(KcInterp *ip, KcType *t, const uint8_t *a, KcPos pos) {
    switch (t->k) {
        case PT_PTR: { uint64_t x; memcpy(&x, a, 8); return from_address_t(x, t->to); }
        case PT_STRUCT: {
            KcLayout *L = t->lay;
            layout_complete(ip, L, pos);
            KcStructInst *inst = kc_struct_alloc();
            inst->type_name = L->name; inst->count = L->n;
            inst->names = kc_malloc(sizeof(char *) * (size_t)(L->n ? L->n : 1));
            inst->values = kc_malloc(sizeof(KcValue) * (size_t)(L->n ? L->n : 1));
            for (int i = 0; i < L->n; i++) {
                KcField *f = &L->f[i];
                inst->names[i] = f->name;
                if (f->is_array) {
                    KcValue arr = kc_array_new();
                    for (int64_t j = 0; j < f->count; j++) kc_array_push(arr, load_mem(ip, f->ty, a + f->off + j * f->esize, pos));
                    inst->values[i] = arr;
                } else inst->values[i] = load_mem(ip, f->ty, a + f->off, pos);
            }
            KcValue v; v.type = VAL_STRUCT; v.as.strct = inst;
            return v;
        }
        case PT_VOID: case PT_ANY:
            kc_error_at(pos, "cannot load through a *void pointer (cast it to a typed pointer first, e.g. `p as *u8`)");
            return kc_null();
        default: return load_scalar(t->k, a, !host_le());
    }
}

static void store_mem(KcInterp *ip, KcType *t, uint8_t *a, KcValue v, KcPos pos) {
    switch (t->k) {
        case PT_PTR: { uint64_t x = ptr_value_address(v, pos); memcpy(a, &x, 8); return; }
        case PT_STRUCT: {
            KcLayout *L = t->lay;
            layout_complete(ip, L, pos);
            if (v.type != VAL_STRUCT || strcmp(v.as.strct->type_name, L->name) != 0)
                kc_error_at(pos, "cannot store a %s into a '%s' location", kc_type_name(v), L->name);
            for (int i = 0; i < L->n; i++) {
                KcField *f = &L->f[i];
                KcValue *fv = NULL;
                for (int j = 0; j < v.as.strct->count; j++) if (strcmp(v.as.strct->names[j], f->name) == 0) { fv = &v.as.strct->values[j]; break; }
                if (!fv) kc_error_at(pos, "struct '%s' value is missing field '%s'", L->name, f->name);
                if (f->is_array) {
                    if (fv->type != VAL_ARRAY || fv->as.arr->count != f->count)
                        kc_error_at(pos, "field '%s.%s' is an array of %lld element(s)", L->name, f->name, (long long)f->count);
                    for (int64_t j = 0; j < f->count; j++) store_mem(ip, f->ty, a + f->off + j * f->esize, fv->as.arr->items[j], pos);
                } else store_mem(ip, f->ty, a + f->off, *fv, pos);
            }
            return;
        }
        case PT_VOID: case PT_ANY:
            kc_error_at(pos, "cannot store through a *void pointer (cast it to a typed pointer first)");
            return;
        default: store_scalar(t->k, a, v, !host_le(), pos); return;
    }
}

/* ======================================================================
 * dereference
 * ====================================================================== */

KcValue kc_ptr_load(KcInterp *ip, KcValue v, KcPos pos) {
    KcPtr *p = as_ptr(v, pos);
    switch (p->kind) {
        case PK_SLOT: return p->u.slot.env->entries[p->u.slot.idx].value;
        case PK_ELEM:
            if (p->u.elem.idx < 0 || p->u.elem.idx >= p->u.elem.arr->count)
                kc_error_at(pos, "pointer to array element %lld is out of bounds (array length %d)", (long long)p->u.elem.idx, p->u.elem.arr->count);
            return p->u.elem.arr->items[p->u.elem.idx];
        case PK_FIELD:
            if (p->u.field.idx >= p->u.field.st->count) kc_error_at(pos, "pointer to a struct field that no longer exists");
            return p->u.field.st->values[p->u.field.idx];
        default: break;
    }
    int64_t sz = kc_type_size(ip, p->et, pos);
    if (p->et->k == PT_VOID || p->et->k == PT_ANY)
        kc_error_at(pos, "cannot dereference a *void pointer (cast it to a typed pointer first, e.g. `p as *u8`)");
    uint8_t *a = mem_addr(ip, p, sz, false, pos);
    return load_mem(ip, p->et, a, pos);
}

void kc_ptr_store(KcInterp *ip, KcValue v_ptr, KcValue v, KcPos pos) {
    KcPtr *p = as_ptr(v_ptr, pos);
    switch (p->kind) {
        case PK_SLOT:
            if (p->readonly) kc_error_at(pos, "cannot assign through a pointer to an immutable variable (declare it with `var`)");
            p->u.slot.env->entries[p->u.slot.idx].value = v; return;
        case PK_ELEM:
            if (p->u.elem.idx < 0 || p->u.elem.idx >= p->u.elem.arr->count)
                kc_error_at(pos, "pointer to array element %lld is out of bounds (array length %d)", (long long)p->u.elem.idx, p->u.elem.arr->count);
            p->u.elem.arr->items[p->u.elem.idx] = v; return;
        case PK_FIELD:
            if (p->u.field.idx >= p->u.field.st->count) kc_error_at(pos, "pointer to a struct field that no longer exists");
            p->u.field.st->values[p->u.field.idx] = v; return;
        default: break;
    }
    int64_t sz = kc_type_size(ip, p->et, pos);
    if (p->et->k == PT_VOID || p->et->k == PT_ANY)
        kc_error_at(pos, "cannot dereference a *void pointer (cast it to a typed pointer first, e.g. `p as *u8`)");
    uint8_t *a = mem_addr(ip, p, sz, true, pos);
    store_mem(ip, p->et, a, v, pos);
}

bool kc_ptr_is_memory_struct(KcValue v) {
    return v.type == VAL_PTR && (v.as.ptr->kind == PK_REGION || v.as.ptr->kind == PK_RAW) && v.as.ptr->et->k == PT_STRUCT;
}

/* Field address of a struct pointer, written into `*out` (typically on the
 * caller's stack: loads/stores never retain it, so no allocation is needed).
 * Returns false if `base` is not a pointer into raw memory. */
bool kc_ptr_member_into(KcInterp *ip, KcValue base, const char *name, KcPos pos, KcPtr *out, bool *is_array) {
    *is_array = false;
    KcPtr *p = as_ptr(base, pos);
    if (p->kind != PK_REGION && p->kind != PK_RAW) return false;
    if (p->et->k != PT_STRUCT) {
        char tb[96]; type_str(p->et, tb, sizeof tb);
        kc_error_at(pos, "'->' needs a pointer to a struct, but this is *%s", tb);
    }
    KcLayout *L = p->et->lay;
    layout_complete(ip, L, pos);
    KcField *f = layout_field(L, name);
    if (!f) kc_error_at(pos, "struct '%s' has no field '%s'", L->name, name);
    *out = *p;
    out->et = f->ty;
    if (out->kind == PK_REGION) {
        out->u.mem.off += f->off;
        if (f->is_array) { out->u.mem.lo = out->u.mem.off; out->u.mem.hi = out->u.mem.off + f->count * f->esize; }
    } else out->u.raw.addr += (uint64_t)f->off;
    *is_array = f->is_array;
    return true;
}

/* p[idx] without allocating the intermediate pointer */
KcValue kc_ptr_load_idx(KcInterp *ip, KcValue pv, KcValue idx, KcPos pos) {
    if (idx.type != VAL_INT) kc_error_at(pos, "pointer index must be an int");
    KcPtr *p = as_ptr(pv, pos);
    if (p->kind != PK_REGION && p->kind != PK_RAW) return kc_ptr_load(ip, kc_ptr_index(ip, pv, idx, pos), pos);
    KcPtr tmp = *p;
    int64_t step = kc_type_size(ip, p->et, pos), delta;
    if (p->kind == PK_REGION) {
        if (__builtin_mul_overflow(idx.as.i, step, &delta) || __builtin_add_overflow(p->u.mem.off, delta, &delta)) kc_error_at(pos, "pointer arithmetic overflow");
        tmp.u.mem.off = delta;
    } else tmp.u.raw.addr = (uint64_t)((int64_t)p->u.raw.addr + idx.as.i * step);
    KcValue tv; tv.type = VAL_PTR; tv.as.ptr = &tmp;
    return kc_ptr_load(ip, tv, pos);
}

void kc_ptr_store_idx(KcInterp *ip, KcValue pv, KcValue idx, KcValue v, KcPos pos) {
    if (idx.type != VAL_INT) kc_error_at(pos, "pointer index must be an int");
    KcPtr *p = as_ptr(pv, pos);
    if (p->kind != PK_REGION && p->kind != PK_RAW) { kc_ptr_store(ip, kc_ptr_index(ip, pv, idx, pos), v, pos); return; }
    KcPtr tmp = *p;
    int64_t step = kc_type_size(ip, p->et, pos), delta;
    if (p->kind == PK_REGION) {
        if (__builtin_mul_overflow(idx.as.i, step, &delta) || __builtin_add_overflow(p->u.mem.off, delta, &delta)) kc_error_at(pos, "pointer arithmetic overflow");
        tmp.u.mem.off = delta;
    } else tmp.u.raw.addr = (uint64_t)((int64_t)p->u.raw.addr + idx.as.i * step);
    KcValue tv; tv.type = VAL_PTR; tv.as.ptr = &tmp;
    kc_ptr_store(ip, tv, v, pos);
}

KcValue kc_ptr_member(KcInterp *ip, KcValue base, const char *name, KcPos pos, bool *is_array) {
    *is_array = false;
    KcPtr *p = as_ptr(base, pos);
    if (p->kind == PK_REGION || p->kind == PK_RAW) {
        if (p->et->k != PT_STRUCT) {
            char tb[96]; type_str(p->et, tb, sizeof tb);
            kc_error_at(pos, "'->' needs a pointer to a struct, but this is *%s", tb);
        }
        KcLayout *L = p->et->lay;
        layout_complete(ip, L, pos);
        KcField *f = layout_field(L, name);
        if (!f) kc_error_at(pos, "struct '%s' has no field '%s'", L->name, name);
        KcValue q = clone_ptr(p);
        KcPtr *qp = q.as.ptr;
        qp->et = f->ty;
        if (qp->kind == PK_REGION) {
            qp->u.mem.off += f->off;
            if (f->is_array) { qp->u.mem.lo = qp->u.mem.off; qp->u.mem.hi = qp->u.mem.off + f->count * f->esize; }
        } else qp->u.raw.addr += (uint64_t)f->off;
        *is_array = f->is_array;
        return q;
    }
    /* pointer to a KingCobra struct value (&s): auto-dereference */
    KcValue obj = kc_ptr_load(ip, base, pos);
    if (obj.type == VAL_STRUCT) return kc_ptr_field_ref(obj.as.strct, name, pos);
    kc_error_at(pos, "'->' needs a pointer to a struct, but the pointer refers to a %s", kc_type_name(obj));
    return kc_null();
}

/* ======================================================================
 * pointer arithmetic and comparison
 * ====================================================================== */

static int64_t elem_step(KcInterp *ip, KcPtr *p, KcPos pos) {
    return kc_type_size(ip, p->et, pos); /* void/any -> 1 (byte arithmetic) */
}

static KcValue ptr_add(KcInterp *ip, KcPtr *p, int64_t n, KcPos pos) {
    switch (p->kind) {
        case PK_REGION: {
            int64_t step = elem_step(ip, p, pos), delta;
            if (__builtin_mul_overflow(n, step, &delta) || __builtin_add_overflow(p->u.mem.off, delta, &delta))
                kc_error_at(pos, "pointer arithmetic overflow");
            KcValue q = clone_ptr(p); q.as.ptr->u.mem.off = delta; return q;
        }
        case PK_RAW: {
            int64_t step = elem_step(ip, p, pos);
            KcValue q = clone_ptr(p); q.as.ptr->u.raw.addr = (uint64_t)((int64_t)p->u.raw.addr + n * step); return q;
        }
        case PK_ELEM: { KcValue q = clone_ptr(p); q.as.ptr->u.elem.idx += n; return q; }
        default:
            if (n == 0) return ptr_val(p);
            kc_error_at(pos, "pointer arithmetic is not allowed on a pointer to a variable or struct field (only on pointers into memory regions and arrays)");
            return kc_null();
    }
}

KcValue kc_ptr_index(KcInterp *ip, KcValue pv, KcValue idx, KcPos pos) {
    if (idx.type != VAL_INT) kc_error_at(pos, "pointer index must be an int");
    return ptr_add(ip, as_ptr(pv, pos), idx.as.i, pos);
}

static int ptr_compare(KcInterp *ip, KcPtr *a, KcPtr *b, KcPos pos, int64_t *byte_diff) {
    if (a->kind == PK_REGION && b->kind == PK_REGION && a->u.mem.reg == b->u.mem.reg) {
        *byte_diff = a->u.mem.off - b->u.mem.off; return *byte_diff < 0 ? -1 : *byte_diff > 0;
    }
    if (a->kind == PK_RAW && b->kind == PK_RAW) {
        *byte_diff = (int64_t)(a->u.raw.addr - b->u.raw.addr);
        return a->u.raw.addr < b->u.raw.addr ? -1 : a->u.raw.addr > b->u.raw.addr;
    }
    if (a->kind == PK_ELEM && b->kind == PK_ELEM && a->u.elem.arr == b->u.elem.arr) {
        *byte_diff = a->u.elem.idx - b->u.elem.idx; return *byte_diff < 0 ? -1 : *byte_diff > 0;
    }
    (void)ip;
    kc_error_at(pos, "cannot compare or subtract pointers into different allocations/objects");
    return 0;
}

KcValue kc_ptr_binop(KcInterp *ip, KcOp op, KcValue a, KcValue b, KcPos pos) {
    switch (op) {
        case OP_ADD:
            if (a.type == VAL_PTR && b.type == VAL_INT) return ptr_add(ip, a.as.ptr, b.as.i, pos);
            if (a.type == VAL_INT && b.type == VAL_PTR) return ptr_add(ip, b.as.ptr, a.as.i, pos);
            break;
        case OP_SUB:
            if (a.type == VAL_PTR && b.type == VAL_INT) return ptr_add(ip, a.as.ptr, -b.as.i, pos);
            if (a.type == VAL_PTR && b.type == VAL_PTR) {
                int64_t d; ptr_compare(ip, a.as.ptr, b.as.ptr, pos, &d);
                if (a.as.ptr->kind == PK_ELEM) return kc_int(d);
                int64_t step = elem_step(ip, a.as.ptr, pos);
                if (step == 0) kc_error_at(pos, "cannot subtract pointers to a zero-sized type");
                if (d % step != 0) kc_error_at(pos, "pointers are not a whole number of elements apart (%lld bytes, element size %lld)", (long long)d, (long long)step);
                return kc_int(d / step);
            }
            break;
        case OP_LT: case OP_LE: case OP_GT: case OP_GE:
            if (a.type == VAL_PTR && b.type == VAL_PTR) {
                int64_t d; int c = ptr_compare(ip, a.as.ptr, b.as.ptr, pos, &d);
                return kc_bool(op == OP_LT ? c < 0 : op == OP_LE ? c <= 0 : op == OP_GT ? c > 0 : c >= 0);
            }
            break;
        default: break;
    }
    kc_error_at(pos, "invalid pointer operation: cannot apply this operator to %s and %s", kc_type_name(a), kc_type_name(b));
    return kc_null();
}

bool kc_ptr_equal(KcValue a, KcValue b) {
    if (a.type != VAL_PTR || b.type != VAL_PTR) return false;
    KcPtr *x = a.as.ptr, *y = b.as.ptr;
    if (x->kind == PK_REGION && y->kind == PK_REGION) return x->u.mem.reg == y->u.mem.reg && x->u.mem.off == y->u.mem.off;
    if (x->kind == PK_RAW && y->kind == PK_RAW) return x->u.raw.addr == y->u.raw.addr;
    if ((x->kind == PK_REGION && y->kind == PK_RAW) || (x->kind == PK_RAW && y->kind == PK_REGION)) {
        KcPtr *m = x->kind == PK_REGION ? x : y, *r = x->kind == PK_RAW ? x : y;
        return m->u.mem.reg->alive && (uint64_t)(uintptr_t)(m->u.mem.reg->base + m->u.mem.off) == r->u.raw.addr;
    }
    if (x->kind != y->kind) return false;
    switch (x->kind) {
        case PK_SLOT: return x->u.slot.env == y->u.slot.env && x->u.slot.idx == y->u.slot.idx;
        case PK_ELEM: return x->u.elem.arr == y->u.elem.arr && x->u.elem.idx == y->u.elem.idx;
        case PK_FIELD: return x->u.field.st == y->u.field.st && x->u.field.idx == y->u.field.idx;
        default: return false;
    }
}

/* ======================================================================
 * display
 * ====================================================================== */

char *kc_ptr_display(KcValue v) {
    KcPtr *p = v.as.ptr;
    char tb[96], out[256];
    type_str(p->et, tb, sizeof tb);
    switch (p->kind) {
        case PK_REGION:
            snprintf(out, sizeof out, "<ptr *%s #%llu+%lld%s>", tb, (unsigned long long)p->u.mem.reg->id,
                     (long long)(p->u.mem.off - p->u.mem.lo), p->u.mem.reg->alive ? "" : " (freed)");
            break;
        case PK_RAW: snprintf(out, sizeof out, "<ptr *%s 0x%llx>", tb, (unsigned long long)p->u.raw.addr); break;
        case PK_SLOT: snprintf(out, sizeof out, "<ptr &%s>", p->u.slot.env->entries[p->u.slot.idx].name); break;
        case PK_ELEM: snprintf(out, sizeof out, "<ptr &array[%lld]>", (long long)p->u.elem.idx); break;
        case PK_FIELD: snprintf(out, sizeof out, "<ptr &%s.%s>", p->u.field.st->type_name, p->u.field.idx < p->u.field.st->count ? p->u.field.st->names[p->u.field.idx] : "?"); break;
    }
    return kc_strdup(out);
}

char *kc_buffer_display(KcValue v) {
    KcBuffer *b = v.as.buf;
    char out[96];
    snprintf(out, sizeof out, b->reg->alive ? "<buffer len=%lld pos=%lld>" : "<buffer (freed)>", (long long)b->len, (long long)b->pos);
    return kc_strdup(out);
}

#include "ptr_ops.inc"

void kc_ptr_live_regions(void (*cb)(const KcRegion *r, void *ud), void *ud) {
    for (int i = 0; i < g_nlive; i++) cb(g_live[i], ud);
}
