/* KingCobra std.fbin — binary files and byte streams.
 *
 * One stream handle type backs both real files (fbin.open) and in-memory buffers (fbin.buffer,
 * fbin.from_hex, fbin.load ...), so every read/write/seek function works on either.
 *
 *  - typed integers u8..u64 / i8..i64 and f32/f64, little or big endian (per stream or per call)
 *  - LEB128 varints (unsigned + zigzag), bit-level reading/writing (MSB first)
 *  - strings: raw, NUL-terminated, length-prefixed; byte arrays; hex / base64 import and export
 *  - Python-struct-style pack/unpack format strings ("<4sHIf", "!BBH")
 *  - seek/tell/size/truncate/align/skip, find (string or hex), magic-number checks, hexdump
 *  - crc32, adler32, sha256 over any region
 *  - every read is bounds-checked: running off the end is a clear error, never garbage
 *
 * Integers in KingCobra are signed 64-bit: u64 values above 2^63-1 appear as negative ints
 * (same bits). fbin.u64_to_string() prints them unsigned.
 *
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com>
 */
#include "interp.h"
#include <errno.h>
#include <ctype.h>
#include <math.h>
#include <limits.h>
#if defined(_WIN32)
#include <io.h>
#define FB_SEEK _fseeki64
#define FB_TELL _ftelli64
#else
#include <unistd.h>
#define FB_SEEK fseeko
#define FB_TELL ftello
#endif

#define FB_MAX_MEM ((size_t)0x7fffffff)

typedef struct {
    FILE *f;
    uint8_t *buf; size_t len, cap, pos;          /* memory stream */
    bool can_read, can_write, big;
    int last_op;                                  /* file streams: 0 none, 1 read, 2 write */
    int bit_mode, bit_n; uint8_t bit_byte;        /* 0 none, 1 reading, 2 writing */
    char *path;
} FB;

/* ============================================================ raw stream ops */

static int64_t raw_tell(FB *s) { return s->f ? (int64_t)FB_TELL(s->f) : (int64_t)s->pos; }

static int64_t raw_size(FB *s) {
    if (!s->f) return (int64_t)s->len;
    int64_t save = (int64_t)FB_TELL(s->f);
    FB_SEEK(s->f, 0, SEEK_END);
    int64_t sz = (int64_t)FB_TELL(s->f);
    FB_SEEK(s->f, (long)save, SEEK_SET);
    s->last_op = 0;
    return sz;
}

static bool raw_seek(FB *s, int64_t off, int whence) {
    if (s->f) {
        s->last_op = 0;
        if (whence == SEEK_SET && off < 0) return false;
        return FB_SEEK(s->f, (long)off, whence) == 0;
    }
    int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (int64_t)s->pos : (int64_t)s->len;
    int64_t np = base + off;
    if (np < 0 || (uint64_t)np > FB_MAX_MEM) return false;
    s->pos = (size_t)np;
    return true;
}

static size_t raw_read(FB *s, void *dst, size_t n) {
    if (s->f) {
        if (s->last_op == 2) FB_SEEK(s->f, 0, SEEK_CUR);
        s->last_op = 1;
        return fread(dst, 1, n, s->f);
    }
    if (s->pos >= s->len) return 0;
    size_t avail = s->len - s->pos;
    if (n > avail) n = avail;
    memcpy(dst, s->buf + s->pos, n);
    s->pos += n;
    return n;
}

static bool mem_reserve(FB *s, size_t need) {
    if (need > FB_MAX_MEM) return false;
    if (need <= s->cap) return true;
    size_t nc = s->cap ? s->cap : 256;
    while (nc < need) nc *= 2;
    if (nc > FB_MAX_MEM) nc = FB_MAX_MEM;
    s->buf = kc_realloc(s->buf, nc);
    s->cap = nc;
    return true;
}

static bool raw_write(FB *s, const void *src, size_t n) {
    if (s->f) {
        if (s->last_op == 1) FB_SEEK(s->f, 0, SEEK_CUR);
        s->last_op = 2;
        return fwrite(src, 1, n, s->f) == n;
    }
    if (s->pos + n > FB_MAX_MEM || !mem_reserve(s, s->pos + n)) return false;
    if (s->pos > s->len) memset(s->buf + s->len, 0, s->pos - s->len);     /* seeking past the end zero-fills */
    memcpy(s->buf + s->pos, src, n);
    s->pos += n;
    if (s->pos > s->len) s->len = s->pos;
    return true;
}

/* read at an absolute offset without disturbing the position */
static size_t raw_pread(FB *s, int64_t off, void *dst, size_t n) {
    if (!s->f) {
        if (off < 0 || (size_t)off >= s->len) return 0;
        size_t avail = s->len - (size_t)off;
        if (n > avail) n = avail;
        memcpy(dst, s->buf + off, n);
        return n;
    }
    int64_t save = (int64_t)FB_TELL(s->f);
    FB_SEEK(s->f, (long)off, SEEK_SET);
    size_t got = fread(dst, 1, n, s->f);
    FB_SEEK(s->f, (long)save, SEEK_SET);
    s->last_op = 0;
    return got;
}

/* ---- bit state ---- */
static void bit_sync(FB *s) {
    if (s->bit_mode == 2 && s->bit_n > 0) {
        uint8_t b = (uint8_t)(s->bit_byte << (8 - s->bit_n));
        raw_write(s, &b, 1);
    }
    s->bit_mode = 0; s->bit_n = 0; s->bit_byte = 0;
}

/* ============================================================ handle plumbing */

static void need(int n, int lo, int hi, KcPos pos, const char *sig) {
    if (n < lo || n > hi) kc_error_at(pos, "usage: fbin.%s", sig);
}

static KcValue wrap_stream(FB *s) {
    KcHandle *h = kc_malloc(sizeof(KcHandle));
    h->kind = H_FBIN; h->ptr = s; h->size = 0; h->extra = 0;
    KcValue v; v.type = VAL_HANDLE; v.as.handle = h;
    return v;
}

static FB *new_mem_stream(void) {
    FB *s = kc_malloc(sizeof(FB));
    memset(s, 0, sizeof *s);
    s->can_read = s->can_write = true;
    return s;
}

static FB *get_fb(KcValue v, KcPos pos, const char *who) {
    if (v.type != VAL_HANDLE || v.as.handle->kind != H_FBIN) kc_error_at(pos, "%s: expected an fbin stream (fbin.open / fbin.buffer / ...), got %s", who, kc_type_name(v));
    if (!v.as.handle->ptr) kc_error_at(pos, "%s: the stream is already closed", who);
    return v.as.handle->ptr;
}
static void need_readable(FB *s, KcPos pos, const char *who) { if (!s->can_read) kc_error_at(pos, "%s: the stream was not opened for reading", who); }
static void need_writable(FB *s, KcPos pos, const char *who) { if (!s->can_write) kc_error_at(pos, "%s: the stream was not opened for writing", who); }

static int64_t arg_int(KcValue v, KcPos pos, const char *who, const char *what) {
    if (v.type != VAL_INT) kc_error_at(pos, "%s: %s must be an int, got %s", who, what, kc_type_name(v));
    return v.as.i;
}
static KcString *arg_str(KcValue v, KcPos pos, const char *who, const char *what) {
    if (v.type != VAL_STRING) kc_error_at(pos, "%s: %s must be a string, got %s", who, what, kc_type_name(v));
    return v.as.s;
}

static bool parse_endian(const char *e, bool *big) {
    if (!strcmp(e, "le") || !strcmp(e, "little") || !strcmp(e, "<") || !strcmp(e, "native")) { *big = false; return true; }
    if (!strcmp(e, "be") || !strcmp(e, "big") || !strcmp(e, ">") || !strcmp(e, "network") || !strcmp(e, "!")) { *big = true; return true; }
    return false;
}
static bool endian_for(FB *s, KcValue *a, int n, int idx, KcPos pos, const char *who) {
    if (n <= idx || a[idx].type == VAL_NULL) return s->big;
    bool big;
    if (a[idx].type != VAL_STRING || !parse_endian(a[idx].as.s->data, &big)) kc_error_at(pos, "%s: endian must be \"le\" or \"be\"", who);
    return big;
}

/* ---- checked byte-level operations used by every public function ---- */

static void read_exact(FB *s, void *dst, size_t n, KcPos pos, const char *who) {
    need_readable(s, pos, who);
    bit_sync(s);
    int64_t at = raw_tell(s);
    size_t got = raw_read(s, dst, n);
    if (got < n) {
        int64_t left = raw_size(s) - at; if (left < 0) left = 0;
        kc_error_at(pos, "%s: unexpected end of data (wanted %zu byte(s) at offset %lld, only %lld left)", who, n, (long long)at, (long long)left);
    }
}
static void write_exact(FB *s, const void *src, size_t n, KcPos pos, const char *who) {
    need_writable(s, pos, who);
    bit_sync(s);
    if (!raw_write(s, src, n)) kc_error_at(pos, "%s: write failed (%s)", who, s->f ? strerror(errno) : "in-memory stream would exceed 2 GiB");
}

static void check_len(int64_t n, KcPos pos, const char *who) {
    if (n < 0) kc_error_at(pos, "%s: length must not be negative", who);
    if ((uint64_t)n > FB_MAX_MEM) kc_error_at(pos, "%s: length %lld is too large", who, (long long)n);
}

/* ============================================================ integers */

static uint64_t decode_uint(const uint8_t *b, int w, bool big) {
    uint64_t v = 0;
    if (big) for (int i = 0; i < w; i++) v = (v << 8) | b[i];
    else for (int i = w - 1; i >= 0; i--) v = (v << 8) | b[i];
    return v;
}
static void encode_uint(uint8_t *b, uint64_t v, int w, bool big) {
    for (int i = 0; i < w; i++) b[big ? w - 1 - i : i] = (uint8_t)(v >> (8 * i));
}
static int64_t sign_extend(uint64_t v, int w) {
    if (w == 8) return (int64_t)v;
    uint64_t sign = 1ULL << (8 * w - 1);
    return (int64_t)((v ^ sign) - sign);
}
static void check_range(int64_t v, int w, bool is_signed, KcPos pos, const char *who) {
    if (w == 8) return;                                                   /* any int64 is valid (u64 keeps the bits) */
    if (is_signed) {
        int64_t lo = -((int64_t)1 << (8 * w - 1)), hi = ((int64_t)1 << (8 * w - 1)) - 1;
        if (v < lo || v > hi) kc_error_at(pos, "%s: %lld does not fit in a signed %d-bit integer", who, (long long)v, 8 * w);
    } else if (v < 0 || v > (int64_t)(((uint64_t)1 << (8 * w)) - 1)) kc_error_at(pos, "%s: %lld does not fit in an unsigned %d-bit integer", who, (long long)v, 8 * w);
}

#define DEF_INT(NAME, W, SIGNED) \
    static KcValue fb_read_##NAME(KcInterp *ip, KcValue *a, int n, KcPos pos) { \
        need(n, 1, 2, pos, "read_" #NAME "(stream[, endian]) -> int"); \
        FB *s = get_fb(a[0], pos, "fbin.read_" #NAME); \
        bool big = endian_for(s, a, n, 1, pos, "fbin.read_" #NAME); \
        uint8_t b[8]; read_exact(s, b, W, pos, "fbin.read_" #NAME); \
        uint64_t v = decode_uint(b, W, big); \
        return kc_int((SIGNED) ? sign_extend(v, W) : (int64_t)v); \
    } \
    static KcValue fb_write_##NAME(KcInterp *ip, KcValue *a, int n, KcPos pos) { \
        kc_forbid_in_comptime(ip, pos, "fbin.write_" #NAME); \
        need(n, 2, 3, pos, "write_" #NAME "(stream, value[, endian]) -> null"); \
        FB *s = get_fb(a[0], pos, "fbin.write_" #NAME); \
        int64_t v = arg_int(a[1], pos, "fbin.write_" #NAME, "value"); \
        check_range(v, W, SIGNED, pos, "fbin.write_" #NAME); \
        bool big = endian_for(s, a, n, 2, pos, "fbin.write_" #NAME); \
        uint8_t b[8]; encode_uint(b, (uint64_t)v, W, big); \
        write_exact(s, b, W, pos, "fbin.write_" #NAME); \
        return kc_null(); \
    }
DEF_INT(u8, 1, false) DEF_INT(i8, 1, true)
DEF_INT(u16, 2, false) DEF_INT(i16, 2, true)
DEF_INT(u32, 4, false) DEF_INT(i32, 4, true)
DEF_INT(u64, 8, false) DEF_INT(i64, 8, true)

static double num_arg(KcValue v, KcPos pos, const char *who) {
    if (v.type == VAL_INT) return (double)v.as.i;
    if (v.type == VAL_FLOAT) return v.as.f;
    kc_error_at(pos, "%s: value must be a number, got %s", who, kc_type_name(v));
    return 0;
}
static KcValue fb_read_f32(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 2, pos, "read_f32(stream[, endian]) -> float");
    FB *s = get_fb(a[0], pos, "fbin.read_f32");
    bool big = endian_for(s, a, n, 1, pos, "fbin.read_f32");
    uint8_t b[4]; read_exact(s, b, 4, pos, "fbin.read_f32");
    uint32_t u = (uint32_t)decode_uint(b, 4, big); float f; memcpy(&f, &u, 4);
    return kc_float((double)f);
}
static KcValue fb_read_f64(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 2, pos, "read_f64(stream[, endian]) -> float");
    FB *s = get_fb(a[0], pos, "fbin.read_f64");
    bool big = endian_for(s, a, n, 1, pos, "fbin.read_f64");
    uint8_t b[8]; read_exact(s, b, 8, pos, "fbin.read_f64");
    uint64_t u = decode_uint(b, 8, big); double d; memcpy(&d, &u, 8);
    return kc_float(d);
}
static KcValue fb_write_f32(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    kc_forbid_in_comptime(ip, pos, "fbin.write_f32");
    need(n, 2, 3, pos, "write_f32(stream, value[, endian]) -> null");
    FB *s = get_fb(a[0], pos, "fbin.write_f32");
    float f = (float)num_arg(a[1], pos, "fbin.write_f32");
    bool big = endian_for(s, a, n, 2, pos, "fbin.write_f32");
    uint32_t u; memcpy(&u, &f, 4);
    uint8_t b[4]; encode_uint(b, u, 4, big);
    write_exact(s, b, 4, pos, "fbin.write_f32");
    return kc_null();
}
static KcValue fb_write_f64(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    kc_forbid_in_comptime(ip, pos, "fbin.write_f64");
    need(n, 2, 3, pos, "write_f64(stream, value[, endian]) -> null");
    FB *s = get_fb(a[0], pos, "fbin.write_f64");
    double d = num_arg(a[1], pos, "fbin.write_f64");
    bool big = endian_for(s, a, n, 2, pos, "fbin.write_f64");
    uint64_t u; memcpy(&u, &d, 8);
    uint8_t b[8]; encode_uint(b, u, 8, big);
    write_exact(s, b, 8, pos, "fbin.write_f64");
    return kc_null();
}

/* ---- varints ---- */
static KcValue fb_read_uvarint(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 1, pos, "read_uvarint(stream) -> int   (LEB128)");
    FB *s = get_fb(a[0], pos, "fbin.read_uvarint");
    uint64_t v = 0; int shift = 0;
    for (;;) {
        uint8_t b; read_exact(s, &b, 1, pos, "fbin.read_uvarint");
        if (shift == 63 && (b & 0x7e)) kc_error_at(pos, "fbin.read_uvarint: varint overflows 64 bits");
        v |= (uint64_t)(b & 0x7f) << shift;
        if (!(b & 0x80)) break;
        shift += 7;
        if (shift > 63) kc_error_at(pos, "fbin.read_uvarint: varint is longer than 10 bytes");
    }
    return kc_int((int64_t)v);
}
static KcValue fb_read_svarint(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 1, pos, "read_svarint(stream) -> int   (zigzag LEB128)");
    KcValue u = fb_read_uvarint(ip, a, n, pos);
    uint64_t v = (uint64_t)u.as.i;
    return kc_int((int64_t)(v >> 1) ^ -(int64_t)(v & 1));
}
static void put_uvarint(FB *s, uint64_t v, KcPos pos, const char *who) {
    uint8_t b[10]; int k = 0;
    do { uint8_t x = v & 0x7f; v >>= 7; if (v) x |= 0x80; b[k++] = x; } while (v);
    write_exact(s, b, (size_t)k, pos, who);
}
static KcValue fb_write_uvarint(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    kc_forbid_in_comptime(ip, pos, "fbin.write_uvarint");
    need(n, 2, 2, pos, "write_uvarint(stream, value) -> null   (negative ints are written as their unsigned 64-bit value)");
    FB *s = get_fb(a[0], pos, "fbin.write_uvarint");
    put_uvarint(s, (uint64_t)arg_int(a[1], pos, "fbin.write_uvarint", "value"), pos, "fbin.write_uvarint");
    return kc_null();
}
static KcValue fb_write_svarint(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    kc_forbid_in_comptime(ip, pos, "fbin.write_svarint");
    need(n, 2, 2, pos, "write_svarint(stream, value) -> null   (zigzag)");
    FB *s = get_fb(a[0], pos, "fbin.write_svarint");
    int64_t v = arg_int(a[1], pos, "fbin.write_svarint", "value");
    put_uvarint(s, ((uint64_t)v << 1) ^ (uint64_t)(v >> 63), pos, "fbin.write_svarint");
    return kc_null();
}

/* ---- bits ---- */
static KcValue fb_read_bits(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 2, 2, pos, "read_bits(stream, count) -> int   (1..64 bits, MSB first)");
    FB *s = get_fb(a[0], pos, "fbin.read_bits");
    need_readable(s, pos, "fbin.read_bits");
    int64_t cnt = arg_int(a[1], pos, "fbin.read_bits", "count");
    if (cnt < 1 || cnt > 64) kc_error_at(pos, "fbin.read_bits: count must be 1..64");
    if (s->bit_mode == 2) bit_sync(s);
    s->bit_mode = 1;
    uint64_t v = 0;
    for (int64_t k = 0; k < cnt; k++) {
        if (s->bit_n == 0) {
            int64_t at = raw_tell(s);
            if (raw_read(s, &s->bit_byte, 1) != 1) kc_error_at(pos, "fbin.read_bits: unexpected end of data at offset %lld", (long long)at);
            s->bit_n = 8;
        }
        s->bit_n--;
        v = (v << 1) | (uint64_t)((s->bit_byte >> s->bit_n) & 1);
    }
    return kc_int((int64_t)v);
}
static KcValue fb_write_bits(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    kc_forbid_in_comptime(ip, pos, "fbin.write_bits");
    need(n, 3, 3, pos, "write_bits(stream, value, count) -> null   (low `count` bits of value, MSB first)");
    FB *s = get_fb(a[0], pos, "fbin.write_bits");
    need_writable(s, pos, "fbin.write_bits");
    uint64_t v = (uint64_t)arg_int(a[1], pos, "fbin.write_bits", "value");
    int64_t cnt = arg_int(a[2], pos, "fbin.write_bits", "count");
    if (cnt < 1 || cnt > 64) kc_error_at(pos, "fbin.write_bits: count must be 1..64");
    if (cnt < 64 && (v >> cnt) != 0) kc_error_at(pos, "fbin.write_bits: value %llu does not fit in %lld bit(s)", (unsigned long long)v, (long long)cnt);
    if (s->bit_mode == 1) { s->bit_mode = 0; s->bit_n = 0; }
    s->bit_mode = 2;
    for (int64_t k = cnt - 1; k >= 0; k--) {
        s->bit_byte = (uint8_t)((s->bit_byte << 1) | ((v >> k) & 1));
        if (++s->bit_n == 8) {
            if (!raw_write(s, &s->bit_byte, 1)) kc_error_at(pos, "fbin.write_bits: write failed");
            s->bit_n = 0; s->bit_byte = 0;
        }
    }
    return kc_null();
}
static KcValue fb_bit_align(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 1, pos, "bit_align(stream) -> null   (finishes a partial byte: zero-pads when writing, skips when reading)");
    bit_sync(get_fb(a[0], pos, "fbin.bit_align"));
    return kc_null();
}

/* ============================================================ strings and bytes */

static KcValue mk_string(const uint8_t *d, size_t n) {
    char *c = kc_malloc(n + 1);
    if (n) memcpy(c, d, n);
    c[n] = 0;
    return kc_string_take(c, (int)n);
}

static KcValue fb_read_string(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 2, 2, pos, "read_string(stream, length) -> string   (raw bytes)");
    FB *s = get_fb(a[0], pos, "fbin.read_string");
    int64_t len = arg_int(a[1], pos, "fbin.read_string", "length");
    check_len(len, pos, "fbin.read_string");
    uint8_t *tmp = kc_malloc((size_t)len + 1);
    read_exact(s, tmp, (size_t)len, pos, "fbin.read_string");
    KcValue r = mk_string(tmp, (size_t)len);
    free(tmp);
    return r;
}
static KcValue fb_write_string(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    kc_forbid_in_comptime(ip, pos, "fbin.write_string");
    need(n, 2, 2, pos, "write_string(stream, text) -> int   (raw bytes, no terminator)");
    FB *s = get_fb(a[0], pos, "fbin.write_string");
    KcString *t = arg_str(a[1], pos, "fbin.write_string", "text");
    write_exact(s, t->data, (size_t)t->len, pos, "fbin.write_string");
    return kc_int(t->len);
}
static KcValue fb_read_cstring(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 2, pos, "read_cstring(stream[, max_length=65536]) -> string   (consumes the NUL)");
    FB *s = get_fb(a[0], pos, "fbin.read_cstring");
    int64_t max = n > 1 ? arg_int(a[1], pos, "fbin.read_cstring", "max_length") : 65536;
    check_len(max, pos, "fbin.read_cstring");
    size_t cap = 64, len = 0;
    uint8_t *b = kc_malloc(cap);
    for (;;) {
        uint8_t c; read_exact(s, &c, 1, pos, "fbin.read_cstring");
        if (c == 0) break;
        if ((int64_t)len >= max) kc_error_at(pos, "fbin.read_cstring: no terminating NUL within %lld bytes", (long long)max);
        if (len + 1 >= cap) { cap *= 2; b = kc_realloc(b, cap); }
        b[len++] = c;
    }
    KcValue r = mk_string(b, len);
    free(b);
    return r;
}
static KcValue fb_write_cstring(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    kc_forbid_in_comptime(ip, pos, "fbin.write_cstring");
    need(n, 2, 2, pos, "write_cstring(stream, text) -> int   (writes the bytes plus a NUL)");
    FB *s = get_fb(a[0], pos, "fbin.write_cstring");
    KcString *t = arg_str(a[1], pos, "fbin.write_cstring", "text");
    write_exact(s, t->data, (size_t)t->len + 1, pos, "fbin.write_cstring");   /* data is NUL-terminated */
    return kc_int(t->len + 1);
}
static int prefix_width(KcValue *a, int n, int idx, KcPos pos, const char *who) {
    if (n <= idx || a[idx].type == VAL_NULL) return 1;
    int64_t w = arg_int(a[idx], pos, who, "prefix width");
    if (w != 1 && w != 2 && w != 4 && w != 8) kc_error_at(pos, "%s: prefix width must be 1, 2, 4 or 8", who);
    return (int)w;
}
static KcValue fb_read_pstring(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 3, pos, "read_pstring(stream[, prefix_width=1[, endian]]) -> string   (length-prefixed)");
    FB *s = get_fb(a[0], pos, "fbin.read_pstring");
    int w = prefix_width(a, n, 1, pos, "fbin.read_pstring");
    bool big = endian_for(s, a, n, 2, pos, "fbin.read_pstring");
    uint8_t b[8]; read_exact(s, b, (size_t)w, pos, "fbin.read_pstring");
    uint64_t len = decode_uint(b, w, big);
    if (len > FB_MAX_MEM) kc_error_at(pos, "fbin.read_pstring: implausible length %llu", (unsigned long long)len);
    uint8_t *tmp = kc_malloc((size_t)len + 1);
    read_exact(s, tmp, (size_t)len, pos, "fbin.read_pstring");
    KcValue r = mk_string(tmp, (size_t)len);
    free(tmp);
    return r;
}
static KcValue fb_write_pstring(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    kc_forbid_in_comptime(ip, pos, "fbin.write_pstring");
    need(n, 2, 4, pos, "write_pstring(stream, text[, prefix_width=1[, endian]]) -> int");
    FB *s = get_fb(a[0], pos, "fbin.write_pstring");
    KcString *t = arg_str(a[1], pos, "fbin.write_pstring", "text");
    int w = prefix_width(a, n, 2, pos, "fbin.write_pstring");
    bool big = endian_for(s, a, n, 3, pos, "fbin.write_pstring");
    if (w < 8 && (uint64_t)t->len >= ((uint64_t)1 << (8 * w))) kc_error_at(pos, "fbin.write_pstring: a %d-byte length prefix cannot hold %d", w, t->len);
    uint8_t b[8]; encode_uint(b, (uint64_t)t->len, w, big);
    write_exact(s, b, (size_t)w, pos, "fbin.write_pstring");
    write_exact(s, t->data, (size_t)t->len, pos, "fbin.write_pstring");
    return kc_int(w + t->len);
}

static KcValue fb_read_bytes(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 2, 2, pos, "read_bytes(stream, count) -> int[]");
    FB *s = get_fb(a[0], pos, "fbin.read_bytes");
    int64_t cnt = arg_int(a[1], pos, "fbin.read_bytes", "count");
    check_len(cnt, pos, "fbin.read_bytes");
    uint8_t *tmp = kc_malloc((size_t)cnt + 1);
    read_exact(s, tmp, (size_t)cnt, pos, "fbin.read_bytes");
    KcValue arr = kc_array_new();
    for (int64_t i = 0; i < cnt; i++) kc_array_push(arr, kc_int(tmp[i]));
    free(tmp);
    return arr;
}
static KcValue fb_write_bytes(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    kc_forbid_in_comptime(ip, pos, "fbin.write_bytes");
    need(n, 2, 2, pos, "write_bytes(stream, bytes) -> int   (bytes: array of ints 0..255)");
    FB *s = get_fb(a[0], pos, "fbin.write_bytes");
    if (a[1].type != VAL_ARRAY) kc_error_at(pos, "fbin.write_bytes: bytes must be an array of ints");
    int cnt = a[1].as.arr->count;
    uint8_t *tmp = kc_malloc((size_t)cnt + 1);
    for (int i = 0; i < cnt; i++) {
        KcValue v = a[1].as.arr->items[i];
        if (v.type != VAL_INT || v.as.i < 0 || v.as.i > 255) kc_error_at(pos, "fbin.write_bytes: item %d is not an int in 0..255", i);
        tmp[i] = (uint8_t)v.as.i;
    }
    write_exact(s, tmp, (size_t)cnt, pos, "fbin.write_bytes");
    free(tmp);
    return kc_int(cnt);
}

/* ---- hex / base64 codecs ---- */
static KcValue hex_of(const uint8_t *d, size_t n) {
    static const char H[] = "0123456789abcdef";
    char *o = kc_malloc(n * 2 + 1);
    for (size_t i = 0; i < n; i++) { o[2 * i] = H[d[i] >> 4]; o[2 * i + 1] = H[d[i] & 15]; }
    o[n * 2] = 0;
    return kc_string_take(o, (int)(n * 2));
}
static int hexval(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
/* hex text (whitespace and an optional 0x are ignored) -> bytes. Returns NULL on bad input. */
static uint8_t *hex_decode(const char *s, size_t n, size_t *outlen, const char **why) {
    uint8_t *o = kc_malloc(n / 2 + 1);
    size_t k = 0; int hi = -1;
    size_t i = 0;
    if (n >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) i = 2;
    for (; i < n; i++) {
        if (isspace((unsigned char)s[i])) continue;
        int v = hexval((unsigned char)s[i]);
        if (v < 0) { free(o); *why = "invalid hex digit"; return NULL; }
        if (hi < 0) hi = v; else { o[k++] = (uint8_t)(hi * 16 + v); hi = -1; }
    }
    if (hi >= 0) { free(o); *why = "odd number of hex digits"; return NULL; }
    *outlen = k;
    return o;
}
static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static KcValue b64_of(const uint8_t *d, size_t n) {
    char *o = kc_malloc((n + 2) / 3 * 4 + 1);
    size_t k = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)d[i] << 16;
        if (i + 1 < n) v |= (uint32_t)d[i + 1] << 8;
        if (i + 2 < n) v |= d[i + 2];
        o[k++] = B64[(v >> 18) & 63]; o[k++] = B64[(v >> 12) & 63];
        o[k++] = i + 1 < n ? B64[(v >> 6) & 63] : '=';
        o[k++] = i + 2 < n ? B64[v & 63] : '=';
    }
    o[k] = 0;
    return kc_string_take(o, (int)k);
}
static uint8_t *b64_decode(const char *s, size_t n, size_t *outlen, const char **why) {
    uint8_t *o = kc_malloc(n / 4 * 3 + 3);
    size_t k = 0; uint32_t acc = 0; int bits = 0; size_t pad = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (isspace(c)) continue;
        if (c == '=') { pad++; continue; }
        if (pad) { free(o); *why = "data after '=' padding"; return NULL; }
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A'; else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52; else if (c == '+' || c == '-') v = 62;
        else if (c == '/' || c == '_') v = 63; else { free(o); *why = "invalid base64 character"; return NULL; }
        acc = (acc << 6) | (uint32_t)v; bits += 6;
        if (bits >= 8) { bits -= 8; o[k++] = (uint8_t)(acc >> bits); acc &= (1u << bits) - 1; }
    }
    if (bits >= 6) { free(o); *why = "truncated base64 data"; return NULL; }
    *outlen = k;
    return o;
}

/* Copies [off, off+len) of the stream (clamped to the end) into a fresh buffer. len < 0 = to the end. */
static uint8_t *region(FB *s, int64_t off, int64_t len, size_t *outlen, KcPos pos, const char *who) {
    bit_sync(s);
    int64_t size = raw_size(s);
    if (off < 0 || off > size) kc_error_at(pos, "%s: offset %lld is outside the stream (size %lld)", who, (long long)off, (long long)size);
    int64_t avail = size - off;
    if (len < 0 || len > avail) len = avail;
    check_len(len, pos, who);
    uint8_t *b = kc_malloc((size_t)len + 1);
    size_t got = raw_pread(s, off, b, (size_t)len);
    *outlen = got;
    return b;
}
static void region_args(KcValue *a, int n, int idx, int64_t *off, int64_t *len, KcPos pos, const char *who) {
    *off = 0; *len = -1;
    if (n > idx && a[idx].type != VAL_NULL) *off = arg_int(a[idx], pos, who, "offset");
    if (n > idx + 1 && a[idx + 1].type != VAL_NULL) {
        *len = arg_int(a[idx + 1], pos, who, "length");
        if (*len < 0) kc_error_at(pos, "%s: length must not be negative", who);
    }
}

static KcValue fb_read_hex(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 2, 2, pos, "read_hex(stream, count) -> string   (lowercase hex of the next `count` bytes)");
    FB *s = get_fb(a[0], pos, "fbin.read_hex");
    int64_t cnt = arg_int(a[1], pos, "fbin.read_hex", "count");
    check_len(cnt, pos, "fbin.read_hex");
    uint8_t *tmp = kc_malloc((size_t)cnt + 1);
    read_exact(s, tmp, (size_t)cnt, pos, "fbin.read_hex");
    KcValue r = hex_of(tmp, (size_t)cnt);
    free(tmp);
    return r;
}
static KcValue fb_write_hex(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    kc_forbid_in_comptime(ip, pos, "fbin.write_hex");
    need(n, 2, 2, pos, "write_hex(stream, hex) -> int   (whitespace and a leading 0x are ignored)");
    FB *s = get_fb(a[0], pos, "fbin.write_hex");
    KcString *h = arg_str(a[1], pos, "fbin.write_hex", "hex");
    size_t len = 0; const char *why = "";
    uint8_t *b = hex_decode(h->data, (size_t)h->len, &len, &why);
    if (!b) kc_error_at(pos, "fbin.write_hex: %s", why);
    write_exact(s, b, len, pos, "fbin.write_hex");
    free(b);
    return kc_int((int64_t)len);
}
static KcValue fb_peek_u8(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 1, pos, "peek_u8(stream) -> int   (-1 at the end; does not advance)");
    FB *s = get_fb(a[0], pos, "fbin.peek_u8");
    need_readable(s, pos, "fbin.peek_u8");
    bit_sync(s);
    uint8_t b;
    return kc_int(raw_pread(s, raw_tell(s), &b, 1) == 1 ? b : -1);
}
static KcValue fb_peek_hex(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 2, 2, pos, "peek_hex(stream, count) -> string   (up to `count` bytes as hex; does not advance)");
    FB *s = get_fb(a[0], pos, "fbin.peek_hex");
    need_readable(s, pos, "fbin.peek_hex");
    int64_t cnt = arg_int(a[1], pos, "fbin.peek_hex", "count");
    size_t got; uint8_t *b = region(s, raw_tell(s), cnt, &got, pos, "fbin.peek_hex");
    KcValue r = hex_of(b, got);
    free(b);
    return r;
}

/* ============================================================ constructors / lifecycle */

static bool open_mode(const char *m, char *out, bool *rd, bool *wr) {
    char kind = m[0];
    if (kind != 'r' && kind != 'w' && kind != 'a') return false;
    bool plus = false, bin = false;
    for (const char *p = m + 1; *p; p++) { if (*p == '+') plus = true; else if (*p == 'b') bin = true; else return false; }
    (void)bin;
    out[0] = kind; out[1] = 0;
    if (plus) strcat(out, "+");
    strcat(out, "b");
    *rd = kind == 'r' || plus;
    *wr = kind != 'r' || plus;
    return true;
}

static FB *open_file(const char *path, const char *mode, KcPos pos, const char *who, bool fatal) {
    char cm[8]; bool rd = false, wr = false;
    if (!open_mode(mode, cm, &rd, &wr)) kc_error_at(pos, "%s: mode must be r, w, a, r+, w+ or a+ (got '%s')", who, mode);
    FILE *f = fopen(path, cm);
    if (!f) {
        if (fatal) kc_error_at(pos, "%s: cannot open '%s': %s", who, path, strerror(errno));
        return NULL;
    }
    FB *s = kc_malloc(sizeof(FB));
    memset(s, 0, sizeof *s);
    s->f = f; s->can_read = rd; s->can_write = wr; s->path = kc_strdup(path);
    return s;
}

static KcValue fb_open(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 2, pos, "open(path[, mode=\"r\"]) -> stream   (modes: r w a r+ w+ a+)");
    KcString *p = arg_str(a[0], pos, "fbin.open", "path");
    const char *mode = n > 1 ? arg_str(a[1], pos, "fbin.open", "mode")->data : "r";
    if (mode[0] != 'r') kc_forbid_in_comptime(ip, pos, "fbin.open (write mode)");
    return wrap_stream(open_file(p->data, mode, pos, "fbin.open", true));
}
static KcValue fb_try_open(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 2, pos, "try_open(path[, mode=\"r\"]) -> stream or null");
    KcString *p = arg_str(a[0], pos, "fbin.try_open", "path");
    const char *mode = n > 1 ? arg_str(a[1], pos, "fbin.try_open", "mode")->data : "r";
    if (mode[0] != 'r') kc_forbid_in_comptime(ip, pos, "fbin.try_open (write mode)");
    FB *s = open_file(p->data, mode, pos, "fbin.try_open", false);
    return s ? wrap_stream(s) : kc_null();
}
static KcValue fb_buffer(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 0, 1, pos, "buffer([capacity]) -> stream   (empty in-memory read/write stream)");
    FB *s = new_mem_stream();
    if (n > 0) { int64_t c = arg_int(a[0], pos, "fbin.buffer", "capacity"); check_len(c, pos, "fbin.buffer"); mem_reserve(s, (size_t)c); }
    return wrap_stream(s);
}
static KcValue stream_from_bytes(const uint8_t *d, size_t n) {
    FB *s = new_mem_stream();
    if (n) { mem_reserve(s, n); memcpy(s->buf, d, n); s->len = n; }
    return wrap_stream(s);
}
static KcValue fb_from_hex(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 1, pos, "from_hex(hex) -> stream");
    KcString *h = arg_str(a[0], pos, "fbin.from_hex", "hex");
    size_t len = 0; const char *why = "";
    uint8_t *b = hex_decode(h->data, (size_t)h->len, &len, &why);
    if (!b) kc_error_at(pos, "fbin.from_hex: %s", why);
    KcValue r = stream_from_bytes(b, len);
    free(b);
    return r;
}
static KcValue fb_from_base64(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 1, pos, "from_base64(text) -> stream   (standard or URL-safe alphabet, padding optional)");
    KcString *t = arg_str(a[0], pos, "fbin.from_base64", "text");
    size_t len = 0; const char *why = "";
    uint8_t *b = b64_decode(t->data, (size_t)t->len, &len, &why);
    if (!b) kc_error_at(pos, "fbin.from_base64: %s", why);
    KcValue r = stream_from_bytes(b, len);
    free(b);
    return r;
}
static KcValue fb_from_string(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 1, pos, "from_string(text) -> stream   (the string's bytes)");
    KcString *t = arg_str(a[0], pos, "fbin.from_string", "text");
    return stream_from_bytes((const uint8_t *)t->data, (size_t)t->len);
}
static KcValue fb_from_bytes(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 1, pos, "from_bytes(array) -> stream   (array of ints 0..255)");
    if (a[0].type != VAL_ARRAY) kc_error_at(pos, "fbin.from_bytes: expected an array of ints");
    int cnt = a[0].as.arr->count;
    uint8_t *b = kc_malloc((size_t)cnt + 1);
    for (int i = 0; i < cnt; i++) {
        KcValue v = a[0].as.arr->items[i];
        if (v.type != VAL_INT || v.as.i < 0 || v.as.i > 255) kc_error_at(pos, "fbin.from_bytes: item %d is not an int in 0..255", i);
        b[i] = (uint8_t)v.as.i;
    }
    KcValue r = stream_from_bytes(b, (size_t)cnt);
    free(b);
    return r;
}
static KcValue fb_load(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 1, pos, "load(path) -> stream   (reads the whole file into an in-memory stream)");
    KcString *p = arg_str(a[0], pos, "fbin.load", "path");
    FILE *f = fopen(p->data, "rb");
    if (!f) kc_error_at(pos, "fbin.load: cannot open '%s': %s", p->data, strerror(errno));
    FB *s = new_mem_stream();
    uint8_t chunk[65536]; size_t got;
    while ((got = fread(chunk, 1, sizeof chunk, f)) > 0) {
        if (s->len + got > FB_MAX_MEM) { fclose(f); kc_error_at(pos, "fbin.load: '%s' is larger than 2 GiB; use fbin.open instead", p->data); }
        mem_reserve(s, s->len + got); memcpy(s->buf + s->len, chunk, got); s->len += got;
    }
    fclose(f);
    return wrap_stream(s);
}
static KcValue fb_save(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    kc_forbid_in_comptime(ip, pos, "fbin.save");
    need(n, 2, 2, pos, "save(stream, path) -> int   (writes the stream's entire contents to a file)");
    FB *s = get_fb(a[0], pos, "fbin.save");
    KcString *p = arg_str(a[1], pos, "fbin.save", "path");
    size_t len; uint8_t *b = region(s, 0, -1, &len, pos, "fbin.save");
    FILE *f = fopen(p->data, "wb");
    if (!f) { free(b); kc_error_at(pos, "fbin.save: cannot write '%s': %s", p->data, strerror(errno)); }
    bool ok = fwrite(b, 1, len, f) == len;
    if (fclose(f) != 0) ok = false;
    free(b);
    if (!ok) kc_error_at(pos, "fbin.save: write to '%s' failed", p->data);
    return kc_int((int64_t)len);
}
static KcValue fb_file_size(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 1, pos, "file_size(path) -> int   (-1 if the file cannot be opened)");
    KcString *p = arg_str(a[0], pos, "fbin.file_size", "path");
    FILE *f = fopen(p->data, "rb");
    if (!f) return kc_int(-1);
    FB_SEEK(f, 0, SEEK_END);
    int64_t sz = (int64_t)FB_TELL(f);
    fclose(f);
    return kc_int(sz);
}
static KcValue fb_close(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 1, pos, "close(stream) -> null");
    if (a[0].type != VAL_HANDLE || a[0].as.handle->kind != H_FBIN) kc_error_at(pos, "fbin.close: expected an fbin stream");
    KcHandle *h = a[0].as.handle;
    if (!h->ptr) return kc_null();
    FB *s = h->ptr;
    bit_sync(s);
    if (s->f) fclose(s->f);
    free(s->buf); free(s->path); free(s);
    h->ptr = NULL;
    return kc_null();
}
static KcValue fb_flush(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 1, pos, "flush(stream) -> null");
    FB *s = get_fb(a[0], pos, "fbin.flush");
    bit_sync(s);
    if (s->f) fflush(s->f);
    return kc_null();
}

/* ============================================================ positioning */

static KcValue fb_size(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 1, pos, "size(stream) -> int");
    FB *s = get_fb(a[0], pos, "fbin.size"); bit_sync(s);
    return kc_int(raw_size(s));
}
static KcValue fb_tell(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 1, pos, "tell(stream) -> int");
    FB *s = get_fb(a[0], pos, "fbin.tell");
    return kc_int(raw_tell(s));
}
static KcValue fb_seek(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 2, 3, pos, "seek(stream, offset[, \"set\"|\"cur\"|\"end\"]) -> int   (returns the new position)");
    FB *s = get_fb(a[0], pos, "fbin.seek"); bit_sync(s);
    int64_t off = arg_int(a[1], pos, "fbin.seek", "offset");
    int wh = SEEK_SET;
    if (n > 2) {
        const char *w = arg_str(a[2], pos, "fbin.seek", "whence")->data;
        if (!strcmp(w, "set")) wh = SEEK_SET; else if (!strcmp(w, "cur")) wh = SEEK_CUR; else if (!strcmp(w, "end")) wh = SEEK_END;
        else kc_error_at(pos, "fbin.seek: whence must be \"set\", \"cur\" or \"end\"");
    }
    if (!raw_seek(s, off, wh)) kc_error_at(pos, "fbin.seek: cannot seek to that position");
    return kc_int(raw_tell(s));
}
static KcValue fb_eof(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 1, pos, "eof(stream) -> bool   (true when no bytes are left to read)");
    FB *s = get_fb(a[0], pos, "fbin.eof"); 
    if (s->bit_mode == 1 && s->bit_n > 0) return kc_bool(false);
    return kc_bool(raw_tell(s) >= raw_size(s));
}
static KcValue fb_remaining(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 1, pos, "remaining(stream) -> int   (bytes left to read)");
    FB *s = get_fb(a[0], pos, "fbin.remaining"); bit_sync(s);
    int64_t r = raw_size(s) - raw_tell(s);
    return kc_int(r < 0 ? 0 : r);
}
static KcValue fb_skip(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 2, 2, pos, "skip(stream, count) -> int   (moves forward; fatal if that passes the end of a read-only stream)");
    FB *s = get_fb(a[0], pos, "fbin.skip"); bit_sync(s);
    int64_t c = arg_int(a[1], pos, "fbin.skip", "count");
    if (c < 0) kc_error_at(pos, "fbin.skip: count must not be negative (use seek)");
    if (!s->can_write && raw_tell(s) + c > raw_size(s)) kc_error_at(pos, "fbin.skip: skipping %lld byte(s) passes the end of the data", (long long)c);
    if (!raw_seek(s, c, SEEK_CUR)) kc_error_at(pos, "fbin.skip: cannot seek");
    return kc_int(raw_tell(s));
}
static KcValue fb_align(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 2, 2, pos, "align(stream, boundary) -> int   (pads with zeros when writable, else skips; returns the new position)");
    FB *s = get_fb(a[0], pos, "fbin.align"); bit_sync(s);
    int64_t b = arg_int(a[1], pos, "fbin.align", "boundary");
    if (b < 1 || b > (1 << 20)) kc_error_at(pos, "fbin.align: boundary must be between 1 and 1048576");
    int64_t at = raw_tell(s), pad = (b - at % b) % b;
    if (pad) {
        if (s->can_write) { uint8_t *z = kc_malloc((size_t)pad); memset(z, 0, (size_t)pad); write_exact(s, z, (size_t)pad, pos, "fbin.align"); free(z); }
        else { if (at + pad > raw_size(s)) kc_error_at(pos, "fbin.align: alignment passes the end of the data"); raw_seek(s, pad, SEEK_CUR); }
    }
    return kc_int(raw_tell(s));
}
static KcValue fb_fill(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    kc_forbid_in_comptime(ip, pos, "fbin.fill");
    need(n, 3, 3, pos, "fill(stream, byte, count) -> null   (writes `count` copies of `byte`)");
    FB *s = get_fb(a[0], pos, "fbin.fill");
    int64_t v = arg_int(a[1], pos, "fbin.fill", "byte"), c = arg_int(a[2], pos, "fbin.fill", "count");
    if (v < 0 || v > 255) kc_error_at(pos, "fbin.fill: byte must be 0..255");
    check_len(c, pos, "fbin.fill");
    uint8_t chunk[4096]; memset(chunk, (int)v, sizeof chunk);
    while (c > 0) { size_t k = c > (int64_t)sizeof chunk ? sizeof chunk : (size_t)c; write_exact(s, chunk, k, pos, "fbin.fill"); c -= (int64_t)k; }
    return kc_null();
}
static KcValue fb_truncate(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    kc_forbid_in_comptime(ip, pos, "fbin.truncate");
    need(n, 2, 2, pos, "truncate(stream, size) -> null   (shrinks, or grows with zero bytes)");
    FB *s = get_fb(a[0], pos, "fbin.truncate");
    need_writable(s, pos, "fbin.truncate"); bit_sync(s);
    int64_t sz = arg_int(a[1], pos, "fbin.truncate", "size");
    check_len(sz, pos, "fbin.truncate");
    if (!s->f) {
        if ((size_t)sz > s->len) { mem_reserve(s, (size_t)sz); memset(s->buf + s->len, 0, (size_t)sz - s->len); }
        s->len = (size_t)sz;
    } else {
        fflush(s->f);
#if defined(_WIN32)
        if (_chsize_s(_fileno(s->f), sz) != 0) kc_error_at(pos, "fbin.truncate: failed");
#else
        if (ftruncate(fileno(s->f), (off_t)sz) != 0) kc_error_at(pos, "fbin.truncate: %s", strerror(errno));
#endif
        s->last_op = 0;
    }
    return kc_null();
}
static KcValue fb_endian(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 2, pos, "endian(stream[, \"le\"|\"be\"]) -> string   (gets or sets the stream's default byte order; initially \"le\")");
    FB *s = get_fb(a[0], pos, "fbin.endian");
    if (n > 1) {
        bool big; const char *e = arg_str(a[1], pos, "fbin.endian", "order")->data;
        if (!parse_endian(e, &big)) kc_error_at(pos, "fbin.endian: order must be \"le\" or \"be\"");
        s->big = big;
    }
    return kc_string(s->big ? "be" : "le");
}

/* ============================================================ export, search, copy */

static KcValue fb_to_hex(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 3, pos, "to_hex(stream[, offset[, length]]) -> string   (does not move the position)");
    FB *s = get_fb(a[0], pos, "fbin.to_hex"); int64_t off, len; region_args(a, n, 1, &off, &len, pos, "fbin.to_hex");
    size_t got; uint8_t *b = region(s, off, len, &got, pos, "fbin.to_hex");
    KcValue r = hex_of(b, got); free(b); return r;
}
static KcValue fb_to_base64(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 3, pos, "to_base64(stream[, offset[, length]]) -> string");
    FB *s = get_fb(a[0], pos, "fbin.to_base64"); int64_t off, len; region_args(a, n, 1, &off, &len, pos, "fbin.to_base64");
    size_t got; uint8_t *b = region(s, off, len, &got, pos, "fbin.to_base64");
    KcValue r = b64_of(b, got); free(b); return r;
}
static KcValue fb_to_bytes(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 3, pos, "to_bytes(stream[, offset[, length]]) -> int[]");
    FB *s = get_fb(a[0], pos, "fbin.to_bytes"); int64_t off, len; region_args(a, n, 1, &off, &len, pos, "fbin.to_bytes");
    size_t got; uint8_t *b = region(s, off, len, &got, pos, "fbin.to_bytes");
    KcValue arr = kc_array_new();
    for (size_t i = 0; i < got; i++) kc_array_push(arr, kc_int(b[i]));
    free(b); return arr;
}
static KcValue fb_to_string(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 3, pos, "to_string(stream[, offset[, length]]) -> string   (raw bytes)");
    FB *s = get_fb(a[0], pos, "fbin.to_string"); int64_t off, len; region_args(a, n, 1, &off, &len, pos, "fbin.to_string");
    size_t got; uint8_t *b = region(s, off, len, &got, pos, "fbin.to_string");
    KcValue r = mk_string(b, got); free(b); return r;
}
static KcValue fb_hexdump(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 3, pos, "hexdump(stream[, offset[, length]]) -> string   (xxd-style; does not move the position)");
    FB *s = get_fb(a[0], pos, "fbin.hexdump"); int64_t off, len; region_args(a, n, 1, &off, &len, pos, "fbin.hexdump");
    size_t got; uint8_t *b = region(s, off, len, &got, pos, "fbin.hexdump");
    size_t cap = (got / 16 + 1) * 80 + 16, k = 0;
    char *o = kc_malloc(cap);
    for (size_t i = 0; i < got; i += 16) {
        k += (size_t)snprintf(o + k, cap - k, "%08llx ", (unsigned long long)(off + (int64_t)i));
        for (size_t j = 0; j < 16; j++) {
            if (j == 8) o[k++] = ' ';
            if (i + j < got) k += (size_t)snprintf(o + k, cap - k, " %02x", b[i + j]);
            else { o[k++] = ' '; o[k++] = ' '; o[k++] = ' '; }
        }
        o[k++] = ' '; o[k++] = ' '; o[k++] = '|';
        for (size_t j = 0; j < 16 && i + j < got; j++) o[k++] = (b[i + j] >= 32 && b[i + j] < 127) ? (char)b[i + j] : '.';
        o[k++] = '|';
        if (i + 16 < got) o[k++] = '\n';
    }
    o[k] = 0;
    free(b);
    return kc_string_take(o, (int)k);
}

static int64_t find_bytes(FB *s, const uint8_t *needle, size_t nl, int64_t start) {
    if (nl == 0) return start;
    bit_sync(s);
    int64_t size = raw_size(s);
    size_t chunk = 65536 + nl;
    uint8_t *buf = kc_malloc(chunk);
    int64_t pos = start, found = -1;
    while (pos < size && found < 0) {
        size_t got = raw_pread(s, pos, buf, chunk);
        if (got < nl) break;
        for (size_t i = 0; i + nl <= got; i++) {
            if (buf[i] == needle[0] && memcmp(buf + i, needle, nl) == 0) { found = pos + (int64_t)i; break; }
        }
        if (found >= 0 || got < chunk) break;
        pos += (int64_t)(got - nl + 1);
    }
    free(buf);
    return found;
}
static KcValue fb_find(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 2, 3, pos, "find(stream, text[, start]) -> int   (byte offset of the first match at or after start, else -1; position unchanged)");
    FB *s = get_fb(a[0], pos, "fbin.find"); need_readable(s, pos, "fbin.find");
    KcString *t = arg_str(a[1], pos, "fbin.find", "text");
    int64_t st = n > 2 ? arg_int(a[2], pos, "fbin.find", "start") : 0;
    if (st < 0) kc_error_at(pos, "fbin.find: start must not be negative");
    return kc_int(find_bytes(s, (const uint8_t *)t->data, (size_t)t->len, st));
}
static KcValue fb_find_hex(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 2, 3, pos, "find_hex(stream, hex[, start]) -> int   (like find, for arbitrary bytes)");
    FB *s = get_fb(a[0], pos, "fbin.find_hex"); need_readable(s, pos, "fbin.find_hex");
    KcString *h = arg_str(a[1], pos, "fbin.find_hex", "hex");
    size_t len = 0; const char *why = "";
    uint8_t *b = hex_decode(h->data, (size_t)h->len, &len, &why);
    if (!b) kc_error_at(pos, "fbin.find_hex: %s", why);
    int64_t st = n > 2 ? arg_int(a[2], pos, "fbin.find_hex", "start") : 0;
    if (st < 0) kc_error_at(pos, "fbin.find_hex: start must not be negative");
    int64_t r = find_bytes(s, b, len, st);
    free(b);
    return kc_int(r);
}

static bool expect_bytes(FB *s, const uint8_t *want, size_t n) {
    bit_sync(s);
    int64_t at = raw_tell(s);
    uint8_t *got = kc_malloc(n + 1);
    size_t r = raw_pread(s, at, got, n);
    bool ok = r == n && memcmp(got, want, n) == 0;
    free(got);
    if (ok) raw_seek(s, (int64_t)n, SEEK_CUR);
    return ok;
}
static KcValue fb_expect(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 2, 2, pos, "expect(stream, text) -> bool   (true and advances if the next bytes equal text; else false, unchanged)");
    FB *s = get_fb(a[0], pos, "fbin.expect"); need_readable(s, pos, "fbin.expect");
    KcString *t = arg_str(a[1], pos, "fbin.expect", "text");
    return kc_bool(expect_bytes(s, (const uint8_t *)t->data, (size_t)t->len));
}
static KcValue fb_expect_hex(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 2, 2, pos, "expect_hex(stream, hex) -> bool   (magic-number check, e.g. \"89504e47\" for PNG)");
    FB *s = get_fb(a[0], pos, "fbin.expect_hex"); need_readable(s, pos, "fbin.expect_hex");
    KcString *h = arg_str(a[1], pos, "fbin.expect_hex", "hex");
    size_t len = 0; const char *why = "";
    uint8_t *b = hex_decode(h->data, (size_t)h->len, &len, &why);
    if (!b) kc_error_at(pos, "fbin.expect_hex: %s", why);
    bool ok = expect_bytes(s, b, len);
    free(b);
    return kc_bool(ok);
}

static KcValue fb_copy(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    kc_forbid_in_comptime(ip, pos, "fbin.copy");
    need(n, 2, 3, pos, "copy(src, dst[, count]) -> int   (copies from src's position to dst's position; default: everything left)");
    FB *src = get_fb(a[0], pos, "fbin.copy"), *dst = get_fb(a[1], pos, "fbin.copy");
    if (src == dst) kc_error_at(pos, "fbin.copy: source and destination must be different streams");
    need_readable(src, pos, "fbin.copy"); need_writable(dst, pos, "fbin.copy"); bit_sync(src); bit_sync(dst);
    int64_t left = raw_size(src) - raw_tell(src); if (left < 0) left = 0;
    int64_t want = n > 2 ? arg_int(a[2], pos, "fbin.copy", "count") : left;
    if (want < 0) kc_error_at(pos, "fbin.copy: count must not be negative");
    if (want > left) kc_error_at(pos, "fbin.copy: wanted %lld byte(s) but only %lld are left in the source", (long long)want, (long long)left);
    uint8_t chunk[65536]; int64_t done = 0;
    while (done < want) {
        size_t k = (want - done) > (int64_t)sizeof chunk ? sizeof chunk : (size_t)(want - done);
        size_t got = raw_read(src, chunk, k);
        if (got == 0) break;
        if (!raw_write(dst, chunk, got)) kc_error_at(pos, "fbin.copy: write failed");
        done += (int64_t)got;
    }
    return kc_int(done);
}

/* ============================================================ checksums and hashes */

static uint32_t g_crc_table[256]; static bool g_crc_ready = false;
static void crc_init(void) {
    if (g_crc_ready) return;
    for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1; g_crc_table[i] = c; }
    g_crc_ready = true;
}
static uint32_t crc32_update(uint32_t crc, const uint8_t *d, size_t n) {
    crc_init();
    crc = ~crc;
    for (size_t i = 0; i < n; i++) crc = g_crc_table[(crc ^ d[i]) & 0xff] ^ (crc >> 8);
    return ~crc;
}
static uint32_t adler_update(uint32_t adler, const uint8_t *d, size_t n) {
    uint32_t a = adler & 0xffff, b = adler >> 16;
    for (size_t i = 0; i < n; i++) { a = (a + d[i]) % 65521; b = (b + a) % 65521; }
    return (b << 16) | a;
}

typedef struct { uint32_t h[8]; uint8_t buf[64]; size_t blen; uint64_t total; } Sha;
static const uint32_t K256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void sha_block(Sha *s, const uint8_t *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = ((uint32_t)p[4*i] << 24) | ((uint32_t)p[4*i+1] << 16) | ((uint32_t)p[4*i+2] << 8) | p[4*i+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i-15], 7) ^ ROR(w[i-15], 18) ^ (w[i-15] >> 3), s1 = ROR(w[i-2], 17) ^ ROR(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25), ch = (e & f) ^ (~e & g), t1 = h + S1 + ch + K256[i] + w[i];
        uint32_t S0 = ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22), mj = (a & b) ^ (a & c) ^ (b & c), t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}
static void sha_init(Sha *s) {
    static const uint32_t init[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    memcpy(s->h, init, sizeof init); s->blen = 0; s->total = 0;
}
static void sha_update(Sha *s, const uint8_t *d, size_t n) {
    s->total += n;
    while (n) {
        size_t k = 64 - s->blen; if (k > n) k = n;
        memcpy(s->buf + s->blen, d, k); s->blen += k; d += k; n -= k;
        if (s->blen == 64) { sha_block(s, s->buf); s->blen = 0; }
    }
}
static void sha_final(Sha *s, uint8_t out[32]) {
    uint64_t bits = s->total * 8;
    uint8_t pad = 0x80; sha_update(s, &pad, 1);
    uint8_t z = 0; while (s->blen != 56) sha_update(s, &z, 1);
    uint8_t len[8]; for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha_update(s, len, 8);
    for (int i = 0; i < 8; i++) { out[4*i] = (uint8_t)(s->h[i] >> 24); out[4*i+1] = (uint8_t)(s->h[i] >> 16); out[4*i+2] = (uint8_t)(s->h[i] >> 8); out[4*i+3] = (uint8_t)s->h[i]; }
}

typedef enum { HK_CRC, HK_ADLER, HK_SHA } HashKind;

static KcValue hash_region(FB *s, int64_t off, int64_t len, HashKind k, KcPos pos, const char *who) {
    bit_sync(s);
    int64_t size = raw_size(s);
    if (off < 0 || off > size) kc_error_at(pos, "%s: offset %lld is outside the stream (size %lld)", who, (long long)off, (long long)size);
    if (len < 0 || len > size - off) len = size - off;
    uint32_t crc = 0, adl = 1; Sha sha; sha_init(&sha);
    uint8_t *buf = kc_malloc(65536);
    int64_t done = 0;
    while (done < len) {
        size_t want = (len - done) > 65536 ? 65536 : (size_t)(len - done);
        size_t got = raw_pread(s, off + done, buf, want);
        if (got == 0) break;
        if (k == HK_CRC) crc = crc32_update(crc, buf, got); else if (k == HK_ADLER) adl = adler_update(adl, buf, got); else sha_update(&sha, buf, got);
        done += (int64_t)got;
    }
    free(buf);
    if (k == HK_CRC) return kc_int(crc);
    if (k == HK_ADLER) return kc_int(adl);
    uint8_t dg[32]; sha_final(&sha, dg);
    return hex_of(dg, 32);
}
#define DEF_HASH(NAME, KIND) \
    static KcValue fb_##NAME(KcInterp *ip, KcValue *a, int n, KcPos pos) { \
        need(n, 1, 3, pos, #NAME "(stream[, offset[, length]])"); \
        FB *s = get_fb(a[0], pos, "fbin." #NAME); int64_t off, len; region_args(a, n, 1, &off, &len, pos, "fbin." #NAME); \
        return hash_region(s, off, len, KIND, pos, "fbin." #NAME); \
    } \
    static KcValue fb_##NAME##_string(KcInterp *ip, KcValue *a, int n, KcPos pos) { \
        need(n, 1, 1, pos, #NAME "_string(text)"); \
        KcString *t = arg_str(a[0], pos, "fbin." #NAME "_string", "text"); \
        FB *tmp = new_mem_stream(); mem_reserve(tmp, (size_t)t->len); if (t->len) memcpy(tmp->buf, t->data, (size_t)t->len); tmp->len = (size_t)t->len; \
        KcValue r = hash_region(tmp, 0, -1, KIND, pos, "fbin." #NAME "_string"); free(tmp->buf); free(tmp); return r; \
    }
DEF_HASH(crc32, HK_CRC) DEF_HASH(adler32, HK_ADLER) DEF_HASH(sha256, HK_SHA)

/* ============================================================ pack / unpack */

typedef struct { char code; int64_t count; bool has_count; } FmtItem;

/* Parses a struct-style format string. Returns the item count; sets *big. Fatal on malformed formats. */
static int parse_fmt(const char *f, FmtItem *items, int max, bool *big, bool default_big, KcPos pos, const char *who) {
    int n = 0; *big = default_big;
    if (*f == '<' || *f == '>' || *f == '=' || *f == '@' || *f == '!') {
        *big = (*f == '>' || *f == '!') ? true : (*f == '<' || *f == '=' || *f == '@') ? false : default_big;
        f++;
    }
    while (*f) {
        if (isspace((unsigned char)*f)) { f++; continue; }
        int64_t cnt = 0; bool has = false;
        while (isdigit((unsigned char)*f)) { cnt = cnt * 10 + (*f - '0'); has = true; f++; if (cnt > 100000000) kc_error_at(pos, "%s: repeat count too large", who); }
        char c = *f++;
        if (!c) kc_error_at(pos, "%s: format ends with a repeat count and no type", who);
        if (!strchr("xbBhHiIlLqQfd?sz", c)) kc_error_at(pos, "%s: unknown format code '%c' (x b B h H i I l L q Q f d ? s z)", who, c);
        if (n >= max) kc_error_at(pos, "%s: format has too many items", who);
        items[n].code = c; items[n].count = has ? cnt : 1; items[n].has_count = has; n++;
    }
    return n;
}
static int code_width(char c) {
    switch (c) { case 'x': case 'b': case 'B': case '?': return 1; case 'h': case 'H': return 2; case 'i': case 'I': case 'l': case 'L': case 'f': return 4; case 'q': case 'Q': case 'd': return 8; default: return 0; }
}

static void do_pack(FB *s, const char *fmt, KcValue vals, KcPos pos, const char *who) {
    FmtItem items[256]; bool big;
    int ni = parse_fmt(fmt, items, 256, &big, s->big, pos, who);
    if (vals.type != VAL_ARRAY) kc_error_at(pos, "%s: values must be an array", who);
    int vi = 0, total = vals.as.arr->count;
    for (int i = 0; i < ni; i++) {
        char c = items[i].code; int64_t cnt = items[i].count;
        if (c == 'x') { for (int64_t k = 0; k < cnt; k++) { uint8_t z = 0; write_exact(s, &z, 1, pos, who); } continue; }
        if (c == 's') {
            if (vi >= total) kc_error_at(pos, "%s: format needs more values than the %d given", who, total);
            KcValue v = vals.as.arr->items[vi++];
            if (v.type != VAL_STRING) kc_error_at(pos, "%s: value %d for 's' must be a string", who, vi - 1);
            if (v.as.s->len > cnt) kc_error_at(pos, "%s: string of %d byte(s) does not fit in %lld", who, v.as.s->len, (long long)cnt);
            write_exact(s, v.as.s->data, (size_t)v.as.s->len, pos, who);
            for (int64_t k = v.as.s->len; k < cnt; k++) { uint8_t z = 0; write_exact(s, &z, 1, pos, who); }
            continue;
        }
        for (int64_t k = 0; k < cnt; k++) {
            if (vi >= total) kc_error_at(pos, "%s: format needs more values than the %d given", who, total);
            KcValue v = vals.as.arr->items[vi++];
            if (c == 'z') {
                if (v.type != VAL_STRING) kc_error_at(pos, "%s: value %d for 'z' must be a string", who, vi - 1);
                write_exact(s, v.as.s->data, (size_t)v.as.s->len + 1, pos, who);
            } else if (c == 'f' || c == 'd') {
                double d = num_arg(v, pos, who); uint8_t b[8];
                if (c == 'f') { float f = (float)d; uint32_t u; memcpy(&u, &f, 4); encode_uint(b, u, 4, big); write_exact(s, b, 4, pos, who); }
                else { uint64_t u; memcpy(&u, &d, 8); encode_uint(b, u, 8, big); write_exact(s, b, 8, pos, who); }
            } else if (c == '?') {
                uint8_t b = kc_truthy(v) ? 1 : 0; write_exact(s, &b, 1, pos, who);
            } else {
                int w = code_width(c); bool sg = (c == 'b' || c == 'h' || c == 'i' || c == 'l' || c == 'q');
                int64_t x = arg_int(v, pos, who, "value");
                check_range(x, w, sg, pos, who);
                uint8_t b[8]; encode_uint(b, (uint64_t)x, w, big); write_exact(s, b, (size_t)w, pos, who);
            }
        }
    }
    if (vi != total) kc_error_at(pos, "%s: format consumes %d value(s) but %d were given", who, vi, total);
}

static KcValue do_unpack(FB *s, const char *fmt, KcPos pos, const char *who) {
    FmtItem items[256]; bool big;
    int ni = parse_fmt(fmt, items, 256, &big, s->big, pos, who);
    KcValue out = kc_array_new();
    for (int i = 0; i < ni; i++) {
        char c = items[i].code; int64_t cnt = items[i].count;
        if (c == 'x') { uint8_t z; for (int64_t k = 0; k < cnt; k++) read_exact(s, &z, 1, pos, who); continue; }
        if (c == 's') {
            check_len(cnt, pos, who);
            uint8_t *b = kc_malloc((size_t)cnt + 1);
            read_exact(s, b, (size_t)cnt, pos, who);
            size_t l = (size_t)cnt; while (l > 0 && b[l - 1] == 0) l--;          /* trailing NUL padding is stripped */
            kc_array_push(out, mk_string(b, l)); free(b);
            continue;
        }
        for (int64_t k = 0; k < cnt; k++) {
            if (c == 'z') {
                size_t cap = 32, len = 0; uint8_t *b = kc_malloc(cap);
                for (;;) { uint8_t ch; read_exact(s, &ch, 1, pos, who); if (!ch) break; if (len + 1 >= cap) { cap *= 2; b = kc_realloc(b, cap); } b[len++] = ch; }
                kc_array_push(out, mk_string(b, len)); free(b);
            } else if (c == 'f') { uint8_t b[4]; read_exact(s, b, 4, pos, who); uint32_t u = (uint32_t)decode_uint(b, 4, big); float f; memcpy(&f, &u, 4); kc_array_push(out, kc_float((double)f)); }
            else if (c == 'd') { uint8_t b[8]; read_exact(s, b, 8, pos, who); uint64_t u = decode_uint(b, 8, big); double d; memcpy(&d, &u, 8); kc_array_push(out, kc_float(d)); }
            else if (c == '?') { uint8_t b; read_exact(s, &b, 1, pos, who); kc_array_push(out, kc_bool(b != 0)); }
            else {
                int w = code_width(c); bool sg = (c == 'b' || c == 'h' || c == 'i' || c == 'l' || c == 'q');
                uint8_t b[8]; read_exact(s, b, (size_t)w, pos, who);
                uint64_t u = decode_uint(b, w, big);
                kc_array_push(out, kc_int(sg ? sign_extend(u, w) : (int64_t)u));
            }
        }
    }
    return out;
}

static KcValue fb_pack(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    kc_forbid_in_comptime(ip, pos, "fbin.pack");
    need(n, 3, 3, pos, "pack(stream, format, values) -> int   (format like \"<4sHIf\": < > ! = byte order; b B h H i I q Q f d ? s z x)");
    FB *s = get_fb(a[0], pos, "fbin.pack");
    KcString *f = arg_str(a[1], pos, "fbin.pack", "format");
    int64_t before = raw_tell(s);
    do_pack(s, f->data, a[2], pos, "fbin.pack");
    bit_sync(s);
    return kc_int(raw_tell(s) - before);
}
static KcValue fb_unpack(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 2, 2, pos, "unpack(stream, format) -> array");
    FB *s = get_fb(a[0], pos, "fbin.unpack");
    KcString *f = arg_str(a[1], pos, "fbin.unpack", "format");
    return do_unpack(s, f->data, pos, "fbin.unpack");
}
static KcValue fb_calcsize(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 1, pos, "calcsize(format) -> int   (-1 if the format contains variable-size 'z' items)");
    KcString *f = arg_str(a[0], pos, "fbin.calcsize", "format");
    FmtItem items[256]; bool big;
    int ni = parse_fmt(f->data, items, 256, &big, false, pos, "fbin.calcsize");
    int64_t total = 0;
    for (int i = 0; i < ni; i++) {
        if (items[i].code == 'z') return kc_int(-1);
        total += items[i].code == 's' ? items[i].count : (int64_t)code_width(items[i].code) * items[i].count;
    }
    return kc_int(total);
}
static KcValue fb_pack_hex(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 2, 2, pos, "pack_hex(format, values) -> string   (packs into a temporary buffer and returns it as hex)");
    KcString *f = arg_str(a[0], pos, "fbin.pack_hex", "format");
    FB *tmp = new_mem_stream();
    do_pack(tmp, f->data, a[1], pos, "fbin.pack_hex");
    KcValue r = hex_of(tmp->buf ? tmp->buf : (const uint8_t *)"", tmp->len);
    free(tmp->buf); free(tmp);
    return r;
}
static KcValue fb_unpack_hex(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 2, 2, pos, "unpack_hex(format, hex) -> array");
    KcString *f = arg_str(a[0], pos, "fbin.unpack_hex", "format");
    KcString *h = arg_str(a[1], pos, "fbin.unpack_hex", "hex");
    size_t len = 0; const char *why = "";
    uint8_t *b = hex_decode(h->data, (size_t)h->len, &len, &why);
    if (!b) kc_error_at(pos, "fbin.unpack_hex: %s", why);
    FB *tmp = new_mem_stream(); mem_reserve(tmp, len); if (len) memcpy(tmp->buf, b, len); tmp->len = len; free(b);
    KcValue r = do_unpack(tmp, f->data, pos, "fbin.unpack_hex");
    free(tmp->buf); free(tmp);
    return r;
}

/* ============================================================ integer utilities */

static KcValue fb_bswap16(KcInterp *ip, KcValue *a, int n, KcPos pos) { need(n, 1, 1, pos, "bswap16(x) -> int"); uint16_t v = (uint16_t)arg_int(a[0], pos, "fbin.bswap16", "x"); return kc_int((uint16_t)((v >> 8) | (v << 8))); }
static KcValue fb_bswap32(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 1, pos, "bswap32(x) -> int");
    uint32_t v = (uint32_t)arg_int(a[0], pos, "fbin.bswap32", "x");
    return kc_int((int64_t)(((v & 0xff) << 24) | ((v & 0xff00) << 8) | ((v >> 8) & 0xff00) | (v >> 24)));
}
static KcValue fb_bswap64(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 1, pos, "bswap64(x) -> int");
    uint64_t v = (uint64_t)arg_int(a[0], pos, "fbin.bswap64", "x"), r = 0;
    for (int i = 0; i < 8; i++) { r = (r << 8) | (v & 0xff); v >>= 8; }
    return kc_int((int64_t)r);
}
static KcValue fb_u64_to_string(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 1, pos, "u64_to_string(x) -> string   (prints the int's 64 bits as an unsigned decimal)");
    char b[32]; snprintf(b, sizeof b, "%llu", (unsigned long long)arg_int(a[0], pos, "fbin.u64_to_string", "x"));
    return kc_string(b);
}
static KcValue fb_parse_u64(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    need(n, 1, 1, pos, "parse_u64(text) -> int   (unsigned decimal up to 2^64-1; values above 2^63-1 come back negative, same bits)");
    KcString *t = arg_str(a[0], pos, "fbin.parse_u64", "text");
    if (t->len == 0 || t->len > 20) kc_error_at(pos, "fbin.parse_u64: not an unsigned 64-bit decimal");
    unsigned __int128 v = 0;
    for (int i = 0; i < t->len; i++) {
        if (!isdigit((unsigned char)t->data[i])) kc_error_at(pos, "fbin.parse_u64: not an unsigned 64-bit decimal");
        v = v * 10 + (unsigned)(t->data[i] - '0');
        if (v > (unsigned __int128)UINT64_MAX) kc_error_at(pos, "fbin.parse_u64: value exceeds 2^64-1");
    }
    return kc_int((int64_t)(uint64_t)v);
}

/* ============================================================ registration */

static KcModuleEntry ENTRIES[] = {
    {"open", fb_open}, {"try_open", fb_try_open}, {"buffer", fb_buffer}, {"from_hex", fb_from_hex},
    {"from_base64", fb_from_base64}, {"from_string", fb_from_string}, {"from_bytes", fb_from_bytes},
    {"load", fb_load}, {"save", fb_save}, {"file_size", fb_file_size}, {"close", fb_close}, {"flush", fb_flush},
    {"size", fb_size}, {"tell", fb_tell}, {"seek", fb_seek}, {"eof", fb_eof}, {"remaining", fb_remaining},
    {"skip", fb_skip}, {"align", fb_align}, {"fill", fb_fill}, {"truncate", fb_truncate}, {"endian", fb_endian},
    {"read_u8", fb_read_u8}, {"read_i8", fb_read_i8}, {"read_u16", fb_read_u16}, {"read_i16", fb_read_i16},
    {"read_u32", fb_read_u32}, {"read_i32", fb_read_i32}, {"read_u64", fb_read_u64}, {"read_i64", fb_read_i64},
    {"read_f32", fb_read_f32}, {"read_f64", fb_read_f64},
    {"write_u8", fb_write_u8}, {"write_i8", fb_write_i8}, {"write_u16", fb_write_u16}, {"write_i16", fb_write_i16},
    {"write_u32", fb_write_u32}, {"write_i32", fb_write_i32}, {"write_u64", fb_write_u64}, {"write_i64", fb_write_i64},
    {"write_f32", fb_write_f32}, {"write_f64", fb_write_f64},
    {"read_uvarint", fb_read_uvarint}, {"read_svarint", fb_read_svarint}, {"write_uvarint", fb_write_uvarint}, {"write_svarint", fb_write_svarint},
    {"read_bits", fb_read_bits}, {"write_bits", fb_write_bits}, {"bit_align", fb_bit_align},
    {"read_string", fb_read_string}, {"write_string", fb_write_string}, {"read_cstring", fb_read_cstring}, {"write_cstring", fb_write_cstring},
    {"read_pstring", fb_read_pstring}, {"write_pstring", fb_write_pstring},
    {"read_bytes", fb_read_bytes}, {"write_bytes", fb_write_bytes}, {"read_hex", fb_read_hex}, {"write_hex", fb_write_hex},
    {"peek_u8", fb_peek_u8}, {"peek_hex", fb_peek_hex},
    {"to_hex", fb_to_hex}, {"to_base64", fb_to_base64}, {"to_bytes", fb_to_bytes}, {"to_string", fb_to_string}, {"hexdump", fb_hexdump},
    {"find", fb_find}, {"find_hex", fb_find_hex}, {"expect", fb_expect}, {"expect_hex", fb_expect_hex}, {"copy", fb_copy},
    {"crc32", fb_crc32}, {"crc32_string", fb_crc32_string}, {"adler32", fb_adler32}, {"adler32_string", fb_adler32_string},
    {"sha256", fb_sha256}, {"sha256_string", fb_sha256_string},
    {"pack", fb_pack}, {"unpack", fb_unpack}, {"calcsize", fb_calcsize}, {"pack_hex", fb_pack_hex}, {"unpack_hex", fb_unpack_hex},
    {"bswap16", fb_bswap16}, {"bswap32", fb_bswap32}, {"bswap64", fb_bswap64},
    {"u64_to_string", fb_u64_to_string}, {"parse_u64", fb_parse_u64},
};

KcModule *kc_mod_fbin(void) {
    static KcModule m = { "fbin", ENTRIES, sizeof(ENTRIES) / sizeof(ENTRIES[0]) };
    return &m;
}
