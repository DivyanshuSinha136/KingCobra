/* KingCobra std.http — DEFLATE decoder: raw (RFC 1951), zlib (RFC 1950), gzip (RFC 1952).
 *
 * A compact canonical-Huffman decoder in the style of zlib's "puff". It is
 * used for Content-Encoding: gzip / deflate, and exposed as http.decompress().
 * Checksums are verified (Adler-32 for zlib, CRC-32 + size for gzip), the
 * output is capped (`max_out`, so a tiny "zip bomb" cannot exhaust memory),
 * and every malformed stream returns an error instead of reading past the end.
 */
#include "http_glue.h"

#define MAXBITS 15
#define MAXLCODES 286
#define MAXDCODES 30
#define FIXLCODES 288

typedef struct {
    const uint8_t *in; size_t inlen, incnt;
    uint32_t bitbuf; int bitcnt;
    SysBuf *out; size_t max, base;      /* base: out->len at the start of this member */
} St;

typedef struct { short count[MAXBITS + 1]; short symbol[FIXLCODES]; } Huff;

enum { E_OK = 0, E_INPUT = -1, E_BLOCKTYPE = -2, E_STORED = -3, E_CODES = -4, E_LENGTHS = -5, E_DISTANCE = -6, E_BIG = -7, E_SYMBOL = -8, E_NOEND = -9 };

static const char *emsg(int c) {
    switch (c) {
    case E_INPUT: return "compressed data is truncated";
    case E_BLOCKTYPE: return "invalid deflate block type";
    case E_STORED: return "corrupt stored block";
    case E_CODES: return "too many length or distance codes";
    case E_LENGTHS: return "invalid code lengths";
    case E_DISTANCE: return "invalid distance in compressed data";
    case E_BIG: return "decompressed data exceeds the size limit";
    case E_SYMBOL: return "invalid symbol in compressed data";
    case E_NOEND: return "missing end-of-block code";
    default: return "invalid compressed data";
    }
}

static int bits(St *s, int need) {
    uint32_t val = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->incnt == s->inlen) return E_INPUT;
        val |= (uint32_t)s->in[s->incnt++] << s->bitcnt; s->bitcnt += 8;
    }
    s->bitbuf = val >> need; s->bitcnt -= need;
    return (int)(val & ((1u << need) - 1));
}

static int decode(St *s, const Huff *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= MAXBITS; len++) {
        int b = bits(s, 1); if (b < 0) return b;
        code |= b;
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count; first += count; first <<= 1; code <<= 1;
    }
    return E_SYMBOL;
}

static int construct(Huff *h, const short *length, int n) {
    short offs[MAXBITS + 1];
    for (int len = 0; len <= MAXBITS; len++) h->count[len] = 0;
    for (int sym = 0; sym < n; sym++) h->count[length[sym]]++;
    if (h->count[0] == n) return 0;                     /* no codes: OK (complete) */
    int left = 1;
    for (int len = 1; len <= MAXBITS; len++) { left <<= 1; left -= h->count[len]; if (left < 0) return left; }
    offs[1] = 0; for (int len = 1; len < MAXBITS; len++) offs[len + 1] = (short)(offs[len] + h->count[len]);
    for (int sym = 0; sym < n; sym++) if (length[sym]) h->symbol[offs[length[sym]]++] = (short)sym;
    return left;                                          /* >0 = incomplete */
}

static bool reserve(St *s, size_t n) {
    SysBuf *b = s->out;
    if (b->len - s->base + n > s->max) return false;
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap : 4096; while (b->len + n + 1 > nc) nc *= 2;
        b->p = kc_realloc(b->p, nc); b->cap = nc;
    }
    return true;
}

static const short LENS[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
static const short LEXT[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
static const short DISTS[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
static const short DEXT[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

static int codes(St *s, const Huff *lc, const Huff *dc) {
    for (;;) {
        int sym = decode(s, lc); if (sym < 0) return sym;
        if (sym < 256) {
            if (!reserve(s, 1)) return E_BIG;
            s->out->p[s->out->len++] = (char)sym;
        } else if (sym == 256) return E_OK;
        else {
            sym -= 257; if (sym >= 29) return E_SYMBOL;
            int e = bits(s, LEXT[sym]); if (e < 0) return e;
            int len = LENS[sym] + e;
            int ds = decode(s, dc); if (ds < 0) return ds;
            if (ds >= 30) return E_SYMBOL;
            e = bits(s, DEXT[ds]); if (e < 0) return e;
            size_t dist = (size_t)DISTS[ds] + (size_t)e;
            if (dist > s->out->len - s->base) return E_DISTANCE;
            if (!reserve(s, (size_t)len)) return E_BIG;
            char *p = s->out->p + s->out->len;
            for (int i = 0; i < len; i++) p[i] = p[(ptrdiff_t)i - (ptrdiff_t)dist];   /* may overlap: byte by byte */
            s->out->len += (size_t)len;
        }
    }
}

static int stored(St *s) {
    s->bitbuf = 0; s->bitcnt = 0;
    if (s->incnt + 4 > s->inlen) return E_INPUT;
    unsigned len = s->in[s->incnt] | (unsigned)s->in[s->incnt + 1] << 8;
    unsigned nlen = s->in[s->incnt + 2] | (unsigned)s->in[s->incnt + 3] << 8;
    s->incnt += 4;
    if (len != (~nlen & 0xffff)) return E_STORED;
    if (s->incnt + len > s->inlen) return E_INPUT;
    if (!reserve(s, len)) return E_BIG;
    memcpy(s->out->p + s->out->len, s->in + s->incnt, len);
    s->out->len += len; s->incnt += len;
    return E_OK;
}

static int fixed(St *s) {
    static Huff lc, dc; static bool built;
    if (!built) {
        short l[FIXLCODES]; int sym;
        for (sym = 0; sym < 144; sym++) l[sym] = 8;
        for (; sym < 256; sym++) l[sym] = 9;
        for (; sym < 280; sym++) l[sym] = 7;
        for (; sym < FIXLCODES; sym++) l[sym] = 8;
        construct(&lc, l, FIXLCODES);
        for (sym = 0; sym < MAXDCODES; sym++) l[sym] = 5;
        construct(&dc, l, MAXDCODES);
        built = true;
    }
    return codes(s, &lc, &dc);
}

static int dynamic(St *s) {
    static const short order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
    short lengths[MAXLCODES + MAXDCODES]; Huff lc, dc;
    int nlen = bits(s, 5); if (nlen < 0) return nlen; nlen += 257;
    int ndist = bits(s, 5); if (ndist < 0) return ndist; ndist += 1;
    int ncode = bits(s, 4); if (ncode < 0) return ncode; ncode += 4;
    if (nlen > MAXLCODES || ndist > MAXDCODES) return E_CODES;
    int index;
    for (index = 0; index < ncode; index++) { int b = bits(s, 3); if (b < 0) return b; lengths[order[index]] = (short)b; }
    for (; index < 19; index++) lengths[order[index]] = 0;
    if (construct(&lc, lengths, 19) != 0) return E_LENGTHS;
    index = 0;
    while (index < nlen + ndist) {
        int sym = decode(s, &lc); if (sym < 0) return sym;
        if (sym < 16) lengths[index++] = (short)sym;
        else {
            int len = 0, rep;
            if (sym == 16) { if (index == 0) return E_LENGTHS; len = lengths[index - 1]; rep = bits(s, 2); if (rep < 0) return rep; rep += 3; }
            else if (sym == 17) { rep = bits(s, 3); if (rep < 0) return rep; rep += 3; }
            else { rep = bits(s, 7); if (rep < 0) return rep; rep += 11; }
            if (index + rep > nlen + ndist) return E_LENGTHS;
            while (rep--) lengths[index++] = (short)len;
        }
    }
    if (lengths[256] == 0) return E_NOEND;
    int err = construct(&lc, lengths, nlen);
    if (err && (err < 0 || nlen - lc.count[0] != 1)) return E_LENGTHS;
    err = construct(&dc, lengths + nlen, ndist);
    if (err && (err < 0 || ndist - dc.count[0] != 1)) return E_LENGTHS;
    return codes(s, &lc, &dc);
}

static int inflate_raw(St *s) {
    int last, type;
    do {
        last = bits(s, 1); if (last < 0) return last;
        type = bits(s, 2); if (type < 0) return type;
        int e = type == 0 ? stored(s) : type == 1 ? fixed(s) : type == 2 ? dynamic(s) : E_BLOCKTYPE;
        if (e) return e;
    } while (!last);
    return E_OK;
}

static uint32_t crc32_of(const uint8_t *p, size_t n) {
    static uint32_t T[256]; static bool init;
    if (!init) { for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; T[i] = c; } init = true; }
    uint32_t c = 0xffffffffu;
    while (n--) c = T[(c ^ *p++) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffu;
}

static uint32_t adler32_of(const uint8_t *p, size_t n) {
    uint32_t a = 1, b = 0;
    while (n) { size_t k = n < 5552 ? n : 5552; n -= k; while (k--) { a += *p++; b += a; } a %= 65521; b %= 65521; }
    return b << 16 | a;
}

bool kc_inflate(const uint8_t *in, size_t n, InfMode mode, SysBuf *out, size_t max_out, char *err) {
    if (mode == INF_AUTO) {
        if (n >= 2 && in[0] == 0x1f && in[1] == 0x8b) mode = INF_GZIP;
        else if (n >= 2 && (in[0] & 0x0f) == 8 && ((in[0] << 8) | in[1]) % 31 == 0) mode = INF_ZLIB;
        else mode = INF_RAW;
    }
    size_t pos = 0;
    for (;;) {                                  /* gzip allows several concatenated members */
        St s; memset(&s, 0, sizeof s);
        s.out = out; s.max = max_out; s.base = out->len;
        if (mode == INF_GZIP) {
            if (n - pos < 18 || in[pos] != 0x1f || in[pos + 1] != 0x8b) { snprintf(err, HERR, "not a gzip stream"); return false; }
            if (in[pos + 2] != 8) { snprintf(err, HERR, "unsupported gzip compression method"); return false; }
            int flg = in[pos + 3]; size_t p = pos + 10;
            if (flg & 4) { if (p + 2 > n) goto trunc; size_t xl = in[p] | (size_t)in[p + 1] << 8; p += 2 + xl; }
            if (flg & 8) { while (p < n && in[p]) p++; p++; }
            if (flg & 16) { while (p < n && in[p]) p++; p++; }
            if (flg & 2) p += 2;
            if (p >= n) goto trunc;
            s.in = in + p; s.inlen = n - p;
        } else if (mode == INF_ZLIB) {
            if (n < 6) goto trunc;
            if ((in[0] & 0x0f) != 8 || ((in[0] << 8) | in[1]) % 31 != 0) { snprintf(err, HERR, "not a zlib stream"); return false; }
            if (in[1] & 0x20) { snprintf(err, HERR, "zlib preset dictionary is not supported"); return false; }
            s.in = in + 2; s.inlen = n - 2;
        } else { s.in = in; s.inlen = n; }
        int e = inflate_raw(&s);
        if (e) { snprintf(err, HERR, "%s", emsg(e)); return false; }
        const uint8_t *tail = s.in + s.incnt; size_t left = s.inlen - s.incnt;
        const uint8_t *mem = (const uint8_t *)out->p + s.base; size_t mlen = out->len - s.base;
        if (mode == INF_ZLIB) {
            if (left < 4) goto trunc;
            uint32_t want = (uint32_t)tail[0] << 24 | (uint32_t)tail[1] << 16 | (uint32_t)tail[2] << 8 | tail[3];
            if (want != adler32_of(mem, mlen)) { snprintf(err, HERR, "zlib checksum mismatch (corrupt data)"); return false; }
        } else if (mode == INF_GZIP) {
            if (left < 8) goto trunc;
            uint32_t crc = tail[0] | (uint32_t)tail[1] << 8 | (uint32_t)tail[2] << 16 | (uint32_t)tail[3] << 24;
            uint32_t isz = tail[4] | (uint32_t)tail[5] << 8 | (uint32_t)tail[6] << 16 | (uint32_t)tail[7] << 24;
            if (crc != crc32_of(mem, mlen) || isz != (uint32_t)(mlen & 0xffffffffu)) { snprintf(err, HERR, "gzip checksum mismatch (corrupt data)"); return false; }
            pos = (size_t)(tail + 8 - in);
            if (n - pos >= 18 && in[pos] == 0x1f && in[pos + 1] == 0x8b) continue;
        }
        if (out->p) out->p[out->len] = 0;
        return true;
    }
trunc:
    snprintf(err, HERR, "compressed data is truncated");
    return false;
}

/* http.decompress(data, format = "auto") — format: "gzip" | "zlib" | "deflate" (raw) | "auto" */
static KcValue hi_decompress(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    const char *sig = "http.decompress(data: string, format: string = \"auto\") -> string  (gzip | zlib | deflate | auto; null if corrupt)";
    sys_argc(n, 1, 2, sig, pos); sys_str(a, 0, sig, pos);
    InfMode m = INF_AUTO;
    if (n > 1) {
        const char *f = sys_str(a, 1, sig, pos);
        if (!strcmp(f, "gzip")) m = INF_GZIP; else if (!strcmp(f, "zlib")) m = INF_ZLIB;
        else if (!strcmp(f, "deflate") || !strcmp(f, "raw")) m = INF_RAW; else if (!strcmp(f, "auto")) m = INF_AUTO;
        else kc_error_at(pos, "http.decompress: format must be gzip, zlib, deflate or auto (got '%s')", f);
    }
    SysBuf b = {0}; char err[HERR];
    if (!kc_inflate((const uint8_t *)a[0].as.s->data, (size_t)a[0].as.s->len, m, &b, 0x7fffffffu, err)) { sysbuf_free(&b); ht_set_error("http.decompress: %s", err); return kc_null(); }
    if (!b.p) return kc_string("");
    return sys_bytes_str(b.p, b.len);
}

const HtEntry ht_inflate_entries[] = { {"decompress", hi_decompress} };
const int ht_inflate_count = 1;
