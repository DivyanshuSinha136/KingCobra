/* Included from ptr.c — second half of the pointer/buffer runtime:
 * casts, buffers, pointer methods/properties, FFI marshalling, builtins.
 * (Shares ptr.c's static helpers on purpose; not a separate translation unit.) */

/* ======================================================================
 * casts
 * ====================================================================== */

static int64_t wrap_int(KcPType k, int64_t v) {
    switch (k) {
        case PT_U8: return (uint8_t)v;   case PT_I8: return (int8_t)v;
        case PT_U16: return (uint16_t)v; case PT_I16: return (int16_t)v;
        case PT_U32: return (uint32_t)v; case PT_I32: return (int32_t)v;
        default: return v;
    }
}

static KcValue buffer_ptr(KcBuffer *b, int64_t off, KcType *to) {
    return mk_region_ptr(b->reg, b->start + off, b->start, b->start + b->len, to);
}

static KcValue cast_to_ptr(KcInterp *ip, KcValue v, KcType *to, KcPos pos) {
    switch (v.type) {
        case VAL_NULL: return kc_null();
        case VAL_PTR: { KcValue q = clone_ptr(v.as.ptr); q.as.ptr->et = to; return q; }
        case VAL_BUFFER: return buffer_ptr(v.as.buf, 0, to);
        case VAL_STRING: {
            /* a read-only view of the string's own bytes */
            KcRegion *r = kc_malloc(sizeof(KcRegion));
            r->base = (uint8_t *)v.as.s->data; r->size = (size_t)v.as.s->len;
            r->alive = true; r->owned = false; r->readonly = true; r->heap = false;
            r->id = g_next_region_id++; r->reg_index = -1;
            r->site = pos; r->origin = NULL;   /* untracked view of a string */
            return mk_region_ptr(r, 0, 0, v.as.s->len, to);
        }
        case VAL_INT:
            if (!ip->in_unsafe) kc_error_at(pos, "casting an integer to a pointer forges an address and requires an `unsafe { }` block");
            kc_forbid_in_comptime(ip, pos, "integer-to-pointer cast");
            return from_address_t((uint64_t)v.as.i, to);
        default: break;
    }
    kc_error_at(pos, "cannot cast a %s to a pointer", kc_type_name(v));
    return kc_null();
}

KcValue kc_value_cast(KcInterp *ip, KcValue v, const char *text, KcPos pos) {
    KcType *t = kc_type_parse(ip, text, pos);
    switch (t->k) {
        case PT_PTR: return cast_to_ptr(ip, v, t->to, pos);
        case PT_F32: case PT_F64: {
            double d;
            if (v.type == VAL_INT) d = (double)v.as.i;
            else if (v.type == VAL_FLOAT) d = v.as.f;
            else if (v.type == VAL_BOOL) d = v.as.b ? 1.0 : 0.0;
            else { kc_error_at(pos, "cannot cast a %s to %s", kc_type_name(v), text); return kc_null(); }
            if (t->k == PT_F32) d = (double)(float)d;
            return kc_float(d);
        }
        case PT_STRUCT: case PT_VOID: case PT_ANY:
            kc_error_at(pos, "cannot cast a value to '%s' (cast pointers instead: `p as *%s`)", text, text);
            return kc_null();
        default: {
            int64_t i;
            switch (v.type) {
                case VAL_INT: i = v.as.i; break;
                case VAL_BOOL: i = v.as.b ? 1 : 0; break;
                case VAL_NULL: i = 0; break;
                case VAL_FLOAT:
                    if (isnan(v.as.f) || v.as.f >= 9223372036854775808.0 || v.as.f < -9223372036854775808.0)
                        kc_error_at(pos, "cannot cast the float %g to %s: out of range", v.as.f, text);
                    i = (int64_t)v.as.f; break;
                case VAL_PTR:
                    kc_forbid_in_comptime(ip, pos, "pointer-to-integer cast");
                    i = (int64_t)ptr_value_address(v, pos); break;
                default:
                    kc_error_at(pos, "cannot cast a %s to %s", kc_type_name(v), text);
                    return kc_null();
            }
            if (t->k == PT_BOOL) return kc_bool(i != 0);
            return kc_int(wrap_int(t->k, i));
        }
    }
}

/* ======================================================================
 * buffers
 * ====================================================================== */

static KcValue new_buffer(KcRegion *r, int64_t start, int64_t len, bool view) {
    KcBuffer *b = kc_malloc(sizeof(KcBuffer));
    b->reg = r; b->start = start; b->len = len; b->pos = 0; b->is_view = view;
    KcValue v; v.type = VAL_BUFFER; v.as.buf = b;
    return v;
}

static KcValue buffer_create(int64_t n, KcPos pos) {
    if (n < 0 || n > KC_MAX_ALLOC) kc_error_at(pos, "buffer size %lld is out of range", (long long)n);
    return new_buffer(region_new((size_t)n, false, pos, "buffer()"), 0, n, false);
}

static int64_t buf_len(KcBuffer *b, KcPos pos) {
    if (!b->reg->alive) kc_error_at(pos, "use after free (this buffer was freed)");
    int64_t avail = (int64_t)b->reg->size - b->start;
    if (avail < 0) avail = 0;
    return b->len < avail ? b->len : avail;
}

static uint8_t *buf_span(KcBuffer *b, int64_t off, int64_t n, bool write, KcPos pos) {
    int64_t len = buf_len(b, pos);
    if (write && b->reg->readonly) kc_error_at(pos, "write to a read-only buffer");
    if (off < 0 || n < 0 || off > len || n > len - off)
        kc_error_at(pos, "buffer %s of %lld byte(s) at offset %lld is out of bounds (len %lld)",
                    write ? "write" : "read", (long long)n, (long long)off, (long long)len);
    return b->reg->base + b->start + off;
}

KcValue kc_buffer_elem(KcInterp *ip, KcValue bv, KcValue idx, KcPos pos) {
    (void)ip;
    if (idx.type != VAL_INT) kc_error_at(pos, "buffer index must be an int");
    KcBuffer *b = bv.as.buf;
    int64_t len = buf_len(b, pos);
    if (idx.as.i < 0 || idx.as.i >= len)
        kc_error_at(pos, "buffer index %lld out of bounds (len %lld)", (long long)idx.as.i, (long long)len);
    return buffer_ptr(b, idx.as.i, prim(PT_U8));
}

typedef struct { const char *name; KcPType k; } ScName;
static const ScName SCN[] = {
    {"u8", PT_U8}, {"i8", PT_I8}, {"u16", PT_U16}, {"i16", PT_I16}, {"u32", PT_U32},
    {"i32", PT_I32}, {"u64", PT_U64}, {"i64", PT_I64}, {"f32", PT_F32}, {"f64", PT_F64},
};

/* get_u32 / put_i16_be / read_f64_le / write_u8 ... (little-endian by default) */
static bool parse_accessor(const char *name, int *mode, KcPType *k, bool *be) {
    static const struct { const char *p; int m; } PRE[] = { {"get_", 0}, {"put_", 1}, {"read_", 2}, {"write_", 3} };
    const char *rest = NULL;
    for (size_t i = 0; i < sizeof PRE / sizeof PRE[0]; i++) {
        size_t pl = strlen(PRE[i].p);
        if (strncmp(name, PRE[i].p, pl) == 0) { rest = name + pl; *mode = PRE[i].m; break; }
    }
    if (!rest) return false;
    char tmp[16];
    size_t rl = strlen(rest);
    *be = false;
    if (rl > 3 && strcmp(rest + rl - 3, "_be") == 0) { *be = true; rl -= 3; }
    else if (rl > 3 && strcmp(rest + rl - 3, "_le") == 0) rl -= 3;
    if (rl >= sizeof tmp) return false;
    memcpy(tmp, rest, rl); tmp[rl] = 0;
    for (size_t i = 0; i < sizeof SCN / sizeof SCN[0]; i++)
        if (strcmp(SCN[i].name, tmp) == 0) { *k = SCN[i].k; return true; }
    return false;
}

static int64_t arg_int(KcValue *a, int n, int i, KcPos pos, const char *sig) {
    if (i >= n || a[i].type != VAL_INT) kc_error_at(pos, "%s", sig);
    return a[i].as.i;
}

static KcValue bytes_to_string(const uint8_t *p, int64_t n) {
    char *s = kc_malloc((size_t)n + 1);
    memcpy(s, p, (size_t)n); s[n] = 0;
    return kc_string_adopt(s, (int)n);
}

static KcValue bytes_to_array(const uint8_t *p, int64_t n) {
    KcValue arr = kc_array_new();
    for (int64_t i = 0; i < n; i++) kc_array_push(arr, kc_int(p[i]));
    return arr;
}

static KcValue buffer_resize(KcBuffer *b, int64_t n, KcPos pos) {
    if (b->is_view || b->start != 0 || b->len != (int64_t)b->reg->size || !b->reg->alive)
        kc_error_at(pos, "resize: only a live, whole (non-slice) buffer can be resized");
    if (b->reg->readonly) kc_error_at(pos, "resize: buffer is read-only");
    if (n < 0 || n > KC_MAX_ALLOC) kc_error_at(pos, "resize: size %lld is out of range", (long long)n);
    size_t old = b->reg->size;
    b->reg->base = kc_realloc(b->reg->base, n ? (size_t)n : 1);
    if ((size_t)n > old) memset(b->reg->base + old, 0, (size_t)n - old);
    b->reg->size = (size_t)n; b->len = n;
    if (b->pos > n) b->pos = n;
    return kc_null();
}

static bool buffer_method(KcInterp *ip, KcValue obj, const char *name, KcValue *a, int n, KcPos pos, KcValue *out) {
    (void)ip;
    KcBuffer *b = obj.as.buf;
    int mode; KcPType k; bool be;
    if (parse_accessor(name, &mode, &k, &be)) {
        int sz = scalar_size(k);
        if (mode == 0) {
            int64_t off = arg_int(a, n, 0, pos, "get_<type>(offset)");
            *out = load_scalar(k, buf_span(b, off, sz, false, pos), be);
        } else if (mode == 1) {
            if (n != 2 || a[0].type != VAL_INT) kc_error_at(pos, "put_<type>(offset, value)");
            store_scalar(k, buf_span(b, a[0].as.i, sz, true, pos), a[1], be, pos);
            *out = kc_null();
        } else if (mode == 2) {
            if (n != 0) kc_error_at(pos, "read_<type>() takes no arguments");
            *out = load_scalar(k, buf_span(b, b->pos, sz, false, pos), be);
            b->pos += sz;
        } else {
            if (n != 1) kc_error_at(pos, "write_<type>(value)");
            store_scalar(k, buf_span(b, b->pos, sz, true, pos), a[0], be, pos);
            b->pos += sz; *out = kc_null();
        }
        return true;
    }
    if (strcmp(name, "seek") == 0) {
        int64_t p = arg_int(a, n, 0, pos, "seek(offset)");
        if (p < 0 || p > buf_len(b, pos)) kc_error_at(pos, "seek: offset %lld out of bounds (len %lld)", (long long)p, (long long)buf_len(b, pos));
        b->pos = p; *out = kc_int(p); return true;
    }
    if (strcmp(name, "skip") == 0) {
        int64_t d = arg_int(a, n, 0, pos, "skip(count)");
        int64_t p = b->pos + d;
        if (p < 0 || p > buf_len(b, pos)) kc_error_at(pos, "skip: cursor would leave the buffer (len %lld)", (long long)buf_len(b, pos));
        b->pos = p; *out = kc_int(p); return true;
    }
    if (strcmp(name, "rewind") == 0) { b->pos = 0; *out = kc_null(); return true; }
    if (strcmp(name, "slice") == 0) {
        int64_t off = arg_int(a, n, 0, pos, "slice(offset, length)");
        int64_t len = arg_int(a, n, 1, pos, "slice(offset, length)");
        buf_span(b, off, len, false, pos); /* range check only */
        *out = new_buffer(b->reg, b->start + off, len, true); return true;
    }
    if (strcmp(name, "fill") == 0) {
        int64_t v = arg_int(a, n, 0, pos, "fill(byte [, offset, length])");
        int64_t off = n >= 3 ? arg_int(a, n, 1, pos, "fill: offset must be int") : 0;
        int64_t len = n >= 3 ? arg_int(a, n, 2, pos, "fill: length must be int") : buf_len(b, pos);
        memset(buf_span(b, off, len, true, pos), (int)(v & 0xff), (size_t)len);
        *out = kc_null(); return true;
    }
    if (strcmp(name, "copy_from") == 0) {
        if (n != 4 || a[0].type != VAL_BUFFER) kc_error_at(pos, "copy_from(src_buffer, src_offset, dst_offset, length)");
        int64_t so = arg_int(a, n, 1, pos, "copy_from: src_offset must be int");
        int64_t d_o = arg_int(a, n, 2, pos, "copy_from: dst_offset must be int");
        int64_t len = arg_int(a, n, 3, pos, "copy_from: length must be int");
        uint8_t *src = buf_span(a[0].as.buf, so, len, false, pos);
        uint8_t *dst = buf_span(b, d_o, len, true, pos);
        memmove(dst, src, (size_t)len);
        *out = kc_null(); return true;
    }
    if (strcmp(name, "to_string") == 0) {
        int64_t off = n >= 1 ? arg_int(a, n, 0, pos, "to_string([offset, length])") : 0;
        int64_t len = n >= 2 ? arg_int(a, n, 1, pos, "to_string([offset, length])") : buf_len(b, pos) - off;
        *out = bytes_to_string(buf_span(b, off, len, false, pos), len); return true;
    }
    if (strcmp(name, "put_str") == 0) {
        if (n != 2 || a[0].type != VAL_INT || a[1].type != VAL_STRING) kc_error_at(pos, "put_str(offset, string)");
        memcpy(buf_span(b, a[0].as.i, a[1].as.s->len, true, pos), a[1].as.s->data, (size_t)a[1].as.s->len);
        *out = kc_int(a[1].as.s->len); return true;
    }
    if (strcmp(name, "write_str") == 0) {
        if (n != 1 || a[0].type != VAL_STRING) kc_error_at(pos, "write_str(string)");
        memcpy(buf_span(b, b->pos, a[0].as.s->len, true, pos), a[0].as.s->data, (size_t)a[0].as.s->len);
        b->pos += a[0].as.s->len; *out = kc_int(a[0].as.s->len); return true;
    }
    if (strcmp(name, "read_str") == 0) {
        int64_t len = arg_int(a, n, 0, pos, "read_str(length)");
        *out = bytes_to_string(buf_span(b, b->pos, len, false, pos), len);
        b->pos += len; return true;
    }
    if (strcmp(name, "to_array") == 0) { *out = bytes_to_array(buf_span(b, 0, buf_len(b, pos), false, pos), buf_len(b, pos)); return true; }
    if (strcmp(name, "hex") == 0) {
        int64_t len = buf_len(b, pos);
        const uint8_t *p = buf_span(b, 0, len, false, pos);
        char *s = kc_malloc((size_t)len * 2 + 1);
        for (int64_t i = 0; i < len; i++) snprintf(s + i * 2, 3, "%02x", p[i]);
        *out = kc_string_adopt(s, (int)(len * 2)); return true;
    }
    if (strcmp(name, "clone") == 0) {
        int64_t len = buf_len(b, pos);
        KcValue c = buffer_create(len, pos);
        memcpy(c.as.buf->reg->base, buf_span(b, 0, len, false, pos), (size_t)len);
        *out = c; return true;
    }
    if (strcmp(name, "resize") == 0) { *out = buffer_resize(b, arg_int(a, n, 0, pos, "resize(new_length)"), pos); return true; }
    if (strcmp(name, "free") == 0) {
        if (b->is_view) kc_error_at(pos, "free: a slice cannot be freed; free the buffer it came from");
        if (!b->reg->alive) kc_error_at(pos, "double free (buffer already freed)");
        region_release(b->reg);
        *out = kc_null(); return true;
    }
    return false;
}

/* ======================================================================
 * pointer properties and methods
 * ====================================================================== */

static bool is_mem_ptr(KcPtr *p) { return p->kind == PK_REGION || p->kind == PK_RAW; }

bool kc_ptr_property(KcInterp *ip, KcValue obj, const char *name, KcPos pos, KcValue *out) {
    if (obj.type == VAL_BUFFER) {
        KcBuffer *b = obj.as.buf;
        if (strcmp(name, "len") == 0) { *out = kc_int(buf_len(b, pos)); return true; }
        if (strcmp(name, "pos") == 0) { *out = kc_int(b->pos); return true; }
        if (strcmp(name, "remaining") == 0) { *out = kc_int(buf_len(b, pos) - b->pos); return true; }
        if (strcmp(name, "ptr") == 0) { buf_len(b, pos); *out = buffer_ptr(b, 0, prim(PT_U8)); return true; }
        return false;
    }
    if (obj.type != VAL_PTR) return false;
    KcPtr *p = obj.as.ptr;
    if (strcmp(name, "addr") == 0) {
        kc_forbid_in_comptime(ip, pos, "pointer .addr");
        *out = kc_int((int64_t)ptr_value_address(obj, pos)); return true;
    }
    if (strcmp(name, "elem_size") == 0) { *out = kc_int(p->kind == PK_REGION || p->kind == PK_RAW ? kc_type_size(ip, p->et, pos) : 1); return true; }
    if (strcmp(name, "type") == 0) { char tb[96], full[100]; type_str(p->et, tb, sizeof tb); snprintf(full, sizeof full, "*%s", tb); *out = kc_string(full); return true; }
    if (strcmp(name, "offset") == 0) {
        if (p->kind == PK_REGION) { *out = kc_int(p->u.mem.off - p->u.mem.lo); return true; }
        if (p->kind == PK_ELEM) { *out = kc_int(p->u.elem.idx); return true; }
        kc_error_at(pos, ".offset is only defined for pointers into a region or an array"); return true;
    }
    if (strcmp(name, "limit") == 0) {
        if (p->kind != PK_REGION) kc_error_at(pos, ".limit is only defined for pointers into a region");
        int64_t rem = win_hi(p) - p->u.mem.off;
        *out = kc_int(rem < 0 ? 0 : rem); return true;
    }
    return false;
}

static uint8_t *span_of(KcInterp *ip, KcValue v, int64_t n, bool write, KcPos pos, const char *who) {
    if (v.type == VAL_NULL) kc_error_at(pos, "%s: null pointer", who);
    if (v.type == VAL_BUFFER) return buf_span(v.as.buf, 0, n, write, pos);
    if (v.type != VAL_PTR) kc_error_at(pos, "%s: expected a pointer or buffer, got a %s", who, kc_type_name(v));
    if (!is_mem_ptr(v.as.ptr)) kc_error_at(pos, "%s: needs a pointer into memory (alloc/buffer), not a pointer to a variable", who);
    return mem_addr(ip, v.as.ptr, n, write, pos);
}

static bool pow2(int64_t a) { return a > 0 && (a & (a - 1)) == 0; }

static bool ptr_method(KcInterp *ip, KcValue obj, const char *name, KcValue *a, int n, KcPos pos, KcValue *out) {
    KcPtr *p = obj.as.ptr;
    if (strcmp(name, "byte_add") == 0) {
        int64_t d = arg_int(a, n, 0, pos, "byte_add(bytes)");
        if (p->kind == PK_REGION) { KcValue q = clone_ptr(p); q.as.ptr->u.mem.off += d; *out = q; return true; }
        if (p->kind == PK_RAW) { KcValue q = clone_ptr(p); q.as.ptr->u.raw.addr = (uint64_t)((int64_t)p->u.raw.addr + d); *out = q; return true; }
        kc_error_at(pos, "byte_add: needs a pointer into memory");
    }
    if (strcmp(name, "is_aligned") == 0 || strcmp(name, "align_up") == 0) {
        int64_t al = arg_int(a, n, 0, pos, "is_aligned(alignment) / align_up(alignment)");
        if (!pow2(al)) kc_error_at(pos, "%s: alignment must be a power of two", name);
        if (!is_mem_ptr(p)) kc_error_at(pos, "%s: needs a pointer into memory", name);
        uint64_t addr = ptr_value_address(obj, pos);
        uint64_t up = (addr + (uint64_t)al - 1) & ~((uint64_t)al - 1);
        if (name[0] == 'i') { *out = kc_bool(up == addr); return true; }
        int64_t d = (int64_t)(up - addr);
        if (p->kind == PK_REGION) { KcValue q = clone_ptr(p); q.as.ptr->u.mem.off += d; *out = q; }
        else { KcValue q = clone_ptr(p); q.as.ptr->u.raw.addr = up; *out = q; }
        return true;
    }
    if (strcmp(name, "read_bytes") == 0) {
        int64_t len = arg_int(a, n, 0, pos, "read_bytes(count)");
        *out = bytes_to_array(span_of(ip, obj, len, false, pos, "read_bytes"), len); return true;
    }
    if (strcmp(name, "write_bytes") == 0) {
        if (n != 1 || a[0].type != VAL_ARRAY) kc_error_at(pos, "write_bytes(array_of_ints)");
        int64_t len = a[0].as.arr->count;
        uint8_t *dst = span_of(ip, obj, len, true, pos, "write_bytes");
        for (int64_t i = 0; i < len; i++) {
            KcValue e = a[0].as.arr->items[i];
            if (e.type != VAL_INT) kc_error_at(pos, "write_bytes: element %lld is not an int", (long long)i);
            dst[i] = (uint8_t)e.as.i;
        }
        *out = kc_int(len); return true;
    }
    if (strcmp(name, "read_str") == 0) {
        int64_t len = arg_int(a, n, 0, pos, "read_str(count)");
        *out = bytes_to_string(span_of(ip, obj, len, false, pos, "read_str"), len); return true;
    }
    if (strcmp(name, "write_str") == 0) {
        if (n != 1 || a[0].type != VAL_STRING) kc_error_at(pos, "write_str(string)");
        memcpy(span_of(ip, obj, a[0].as.s->len, true, pos, "write_str"), a[0].as.s->data, (size_t)a[0].as.s->len);
        *out = kc_int(a[0].as.s->len); return true;
    }
    if (strcmp(name, "read_cstr") == 0) {
        int64_t max = n >= 1 ? arg_int(a, n, 0, pos, "read_cstr([max_length])") : (1 << 20);
        int64_t avail = max;
        if (p->kind == PK_REGION) {
            if (!p->u.mem.reg->alive) kc_error_at(pos, "use after free");
            int64_t rem = win_hi(p) - p->u.mem.off;
            if (p->u.mem.off < p->u.mem.lo || rem < 0) kc_error_at(pos, "read_cstr: pointer is outside its allocation");
            if (rem < avail) avail = rem;
        }
        const uint8_t *s = span_of(ip, obj, 0, false, pos, "read_cstr");
        const uint8_t *nul = memchr(s, 0, (size_t)avail);
        if (!nul) kc_error_at(pos, "read_cstr: no NUL terminator within %lld byte(s)", (long long)avail);
        *out = bytes_to_string(s, (int64_t)(nul - s)); return true;
    }
    return false;
}

bool kc_ptr_method(KcInterp *ip, KcValue obj, const char *name, KcValue *args, int nargs, KcPos pos, KcValue *out) {
    if (obj.type == VAL_BUFFER) return buffer_method(ip, obj, name, args, nargs, pos, out);
    if (obj.type == VAL_PTR) return ptr_method(ip, obj, name, args, nargs, pos, out);
    return false;
}

/* ======================================================================
 * FFI marshalling: pointers/buffers become machine addresses
 * ====================================================================== */

void kc_ffi_prepare_args(KcInterp *ip, KcValue *args, int nargs, KcPos pos) {
    for (int i = 0; i < nargs; i++) {
        if (args[i].type != VAL_PTR && args[i].type != VAL_BUFFER) continue;
        if (!ip->in_unsafe) kc_error_at(pos, "passing a pointer or buffer to native code requires an `unsafe { }` block (native code is not bounds-checked)");
        if (args[i].type == VAL_BUFFER) {
            buf_len(args[i].as.buf, pos);
            args[i] = kc_int((int64_t)(uintptr_t)(args[i].as.buf->reg->base + args[i].as.buf->start));
        } else args[i] = kc_int((int64_t)ptr_value_address(args[i], pos));
    }
}

/* ======================================================================
 * builtins
 * ====================================================================== */

static KcValue b_alloc(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    if (n != 1 || a[0].type != VAL_INT || a[0].as.i < 0 || a[0].as.i > KC_MAX_ALLOC)
        kc_error_at(pos, "alloc(size: int >= 0) -> *u8");
    KcRegion *r = region_new((size_t)a[0].as.i, true, pos, "alloc()");
    return mk_region_ptr(r, 0, 0, a[0].as.i, prim(PT_U8));
}

static KcValue b_alloc_array(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    if (n != 2 || a[0].type != VAL_STRING || a[1].type != VAL_INT || a[1].as.i < 0)
        kc_error_at(pos, "alloc_array(\"type\", count) -> *type   e.g. alloc_array(\"i32\", 16)");
    KcType *t = kc_type_parse(ip, a[0].as.s->data, pos);
    if (t->k == PT_VOID || t->k == PT_ANY) kc_error_at(pos, "alloc_array: cannot allocate elements of type '%s'", a[0].as.s->data);
    int64_t sz = kc_type_size(ip, t, pos), total;
    if (__builtin_mul_overflow(sz, a[1].as.i, &total) || total > KC_MAX_ALLOC) kc_error_at(pos, "alloc_array: size is out of range");
    KcRegion *r = region_new((size_t)total, true, pos, "alloc_array()");
    return mk_region_ptr(r, 0, 0, total, t);
}

static KcValue b_realloc(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    if (n != 2 || a[1].type != VAL_INT || a[1].as.i < 0 || a[1].as.i > KC_MAX_ALLOC) kc_error_at(pos, "realloc(ptr, new_size: int >= 0) -> ptr");
    if (a[0].type == VAL_NULL) { KcValue sz = a[1]; return b_alloc(ip, &sz, 1, pos); }
    if (a[0].type != VAL_PTR || a[0].as.ptr->kind != PK_REGION) kc_error_at(pos, "realloc: expected a pointer returned by alloc()");
    KcPtr *p = a[0].as.ptr; KcRegion *r = p->u.mem.reg;
    if (!r->alive) kc_error_at(pos, "realloc: use after free");
    if (!r->heap) kc_error_at(pos, "realloc: this pointer does not refer to alloc()'d memory");
    if (p->u.mem.off != 0 || p->u.mem.lo != 0) kc_error_at(pos, "realloc: pointer is not the start of the allocation");
    size_t old = r->size, nw = (size_t)a[1].as.i;
    r->base = kc_realloc(r->base, nw ? nw : 1);
    if (nw > old) memset(r->base + old, 0, nw - old);
    r->size = nw;
    /* Other pointers into this allocation stay valid (offset-based) and are
       clamped to the new size on use; the result carries the full new window. */
    return mk_region_ptr(r, 0, 0, a[1].as.i, p->et);
}

KcValue kc_free_memory_value(KcInterp *ip, KcValue v, KcPos pos) {
    if (v.type == VAL_NULL) return kc_null();
    if (v.type == VAL_BUFFER) { KcValue o; return buffer_method(ip, v, "free", NULL, 0, pos, &o) ? o : kc_null(); }
    if (v.type != VAL_PTR || v.as.ptr->kind != PK_REGION) kc_error_at(pos, "free: expected a pointer returned by alloc()");
    KcPtr *p = v.as.ptr; KcRegion *r = p->u.mem.reg;
    if (!r->alive) kc_error_at(pos, "double free (allocation #%llu was already freed)", (unsigned long long)r->id);
    if (!r->heap) kc_error_at(pos, "free: this pointer does not refer to alloc()'d memory (buffers are freed with buf.free())");
    if (p->u.mem.off != 0 || p->u.mem.lo != 0) kc_error_at(pos, "free: pointer is not the start of the allocation (offset %lld)", (long long)p->u.mem.off);
    region_release(r);
    return kc_null();
}

bool kc_value_memory_freed(KcValue v) {
    if (v.type == VAL_BUFFER) return !v.as.buf->reg->alive;
    if (v.type == VAL_PTR && v.as.ptr->kind == PK_REGION) return !v.as.ptr->u.mem.reg->alive;
    return false;
}

static KcValue b_memcpy_impl(KcInterp *ip, KcValue *a, int n, KcPos pos, bool allow_overlap) {
    const char *who = allow_overlap ? "memmove" : "memcpy";
    if (n != 3 || a[2].type != VAL_INT || a[2].as.i < 0) kc_error_at(pos, "%s(dst, src, byte_count)", who);
    int64_t len = a[2].as.i;
    uint8_t *dst = span_of(ip, a[0], len, true, pos, who);
    uint8_t *src = span_of(ip, a[1], len, false, pos, who);
    if (!allow_overlap && len > 0 && dst < src + len && src < dst + len)
        kc_error_at(pos, "memcpy: source and destination overlap (use memmove)");
    memmove(dst, src, (size_t)len);
    return a[0];
}
static KcValue b_memcpy(KcInterp *ip, KcValue *a, int n, KcPos pos) { return b_memcpy_impl(ip, a, n, pos, false); }
static KcValue b_memmove(KcInterp *ip, KcValue *a, int n, KcPos pos) { return b_memcpy_impl(ip, a, n, pos, true); }

static KcValue b_memset(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    if (n != 3 || a[1].type != VAL_INT || a[2].type != VAL_INT || a[2].as.i < 0) kc_error_at(pos, "memset(ptr, byte, byte_count)");
    memset(span_of(ip, a[0], a[2].as.i, true, pos, "memset"), (int)(a[1].as.i & 0xff), (size_t)a[2].as.i);
    return a[0];
}

static KcValue b_memcmp(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    if (n != 3 || a[2].type != VAL_INT || a[2].as.i < 0) kc_error_at(pos, "memcmp(a, b, byte_count) -> int");
    int c = memcmp(span_of(ip, a[0], a[2].as.i, false, pos, "memcmp"), span_of(ip, a[1], a[2].as.i, false, pos, "memcmp"), (size_t)a[2].as.i);
    return kc_int(c < 0 ? -1 : c > 0 ? 1 : 0);
}

static KcValue b_buffer(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    if (n != 1 || a[0].type != VAL_INT) kc_error_at(pos, "buffer(size: int) -> buffer");
    return buffer_create(a[0].as.i, pos);
}

static KcValue b_buffer_from(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    if (n != 1) kc_error_at(pos, "buffer_from(string | array_of_bytes) -> buffer");
    if (a[0].type == VAL_STRING) {
        KcValue b = buffer_create(a[0].as.s->len, pos);
        memcpy(b.as.buf->reg->base, a[0].as.s->data, (size_t)a[0].as.s->len);
        return b;
    }
    if (a[0].type == VAL_ARRAY) {
        KcValue b = buffer_create(a[0].as.arr->count, pos);
        for (int i = 0; i < a[0].as.arr->count; i++) {
            KcValue e = a[0].as.arr->items[i];
            if (e.type != VAL_INT) kc_error_at(pos, "buffer_from: element %d is not an int", i);
            b.as.buf->reg->base[i] = (uint8_t)e.as.i;
        }
        return b;
    }
    kc_error_at(pos, "buffer_from: expected a string or an array of ints");
    return kc_null();
}

static KcValue b_ptr_from_addr(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    if (n != 2 || a[0].type != VAL_INT || a[1].type != VAL_STRING) kc_error_at(pos, "ptr_from_addr(address: int, \"*type\") -> ptr");
    if (!ip->in_unsafe) kc_error_at(pos, "ptr_from_addr forges an address and requires an `unsafe { }` block");
    kc_forbid_in_comptime(ip, pos, "ptr_from_addr");
    return kc_ptr_from_address(ip, (uint64_t)a[0].as.i, a[1].as.s->data, pos);
}

static KcValue b_align_up(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    if (n != 2 || a[0].type != VAL_INT || a[1].type != VAL_INT || !pow2(a[1].as.i)) kc_error_at(pos, "align_up(value: int, alignment: power of two)");
    return kc_int(((a[0].as.i + a[1].as.i - 1) / a[1].as.i) * a[1].as.i);
}

static KcValue b_align_down(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    if (n != 2 || a[0].type != VAL_INT || a[1].type != VAL_INT || !pow2(a[1].as.i)) kc_error_at(pos, "align_down(value: int, alignment: power of two)");
    return kc_int((a[0].as.i / a[1].as.i) * a[1].as.i);
}

static KcValue b_is_aligned(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    if (n != 2 || a[1].type != VAL_INT || !pow2(a[1].as.i)) kc_error_at(pos, "is_aligned(int | ptr, alignment: power of two) -> bool");
    if (a[0].type == VAL_INT) return kc_bool(a[0].as.i % a[1].as.i == 0);
    if (a[0].type == VAL_PTR) { KcValue o; if (ptr_method(ip, a[0], "is_aligned", &a[1], 1, pos, &o)) return o; }
    kc_error_at(pos, "is_aligned: expected an int or a pointer");
    return kc_null();
}

void kc_register_ptr_builtins(KcInterp *ip) {
    static const struct { const char *name; KcNativeFn fn; } fns[] = {
        {"alloc", b_alloc}, {"alloc_array", b_alloc_array}, {"realloc", b_realloc},
        {"memcpy", b_memcpy}, {"memmove", b_memmove}, {"memset", b_memset}, {"memcmp", b_memcmp},
        {"buffer", b_buffer}, {"buffer_from", b_buffer_from}, {"ptr_from_addr", b_ptr_from_addr},
        {"align_up", b_align_up}, {"align_down", b_align_down}, {"is_aligned", b_is_aligned},
    };
    for (size_t i = 0; i < sizeof fns / sizeof fns[0]; i++) {
        KcValue v; v.type = VAL_NATIVE; v.as.native = fns[i].fn;
        kc_env_define(ip->builtins_env, fns[i].name, v, false);
    }
}
