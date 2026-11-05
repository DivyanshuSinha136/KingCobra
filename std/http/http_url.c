/* KingCobra std.http — URLs, percent-encoding, query strings, forms, base64.
 *
 * URL handling follows RFC 3986 closely enough for real servers: userinfo,
 * IPv6 literals ([::1]:8080), default ports, dot-segment removal, relative
 * reference resolution (used for redirects), and conservative re-encoding of
 * characters that may not appear raw in a request target. Only http:// and
 * https:// are accepted; a URL with no scheme is treated as http:// (the
 * v0.1 behaviour). Everything here is pure string work and is allowed in
 * comptime { }.
 */
#include "http_glue.h"

/* --------------------------- percent-encoding --------------------------- */

static bool unreserved(unsigned char c) { return isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~'; }

char *ht_pct_encode(const char *s, size_t n, bool plus_for_space, size_t *outlen) {
    static const char H[] = "0123456789ABCDEF";
    char *o = kc_malloc(n * 3 + 1); size_t l = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (unreserved(c)) o[l++] = (char)c;
        else if (c == ' ' && plus_for_space) o[l++] = '+';
        else { o[l++] = '%'; o[l++] = H[c >> 4]; o[l++] = H[c & 15]; }
    }
    o[l] = 0; if (outlen) *outlen = l; return o;
}

static int hexv(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

size_t ht_pct_decode(const char *s, size_t n, char *out, bool plus_to_space) {
    size_t l = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '%' && i + 2 < n && hexv((unsigned char)s[i + 1]) >= 0 && hexv((unsigned char)s[i + 2]) >= 0) {
            out[l++] = (char)(hexv((unsigned char)s[i + 1]) * 16 + hexv((unsigned char)s[i + 2])); i += 2;
        } else if (s[i] == '+' && plus_to_space) out[l++] = ' ';
        else out[l++] = s[i];
    }
    out[l] = 0; return l;
}

/* Encode only what must not appear raw in a request target; keep existing %XX and URL delimiters. */
static char *target_encode(const char *s, size_t n) {
    static const char H[] = "0123456789ABCDEF";
    char *o = kc_malloc(n * 3 + 1); size_t l = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c <= 0x20 || c >= 0x7f || strchr("\"<>\\^`{|}", c)) { o[l++] = '%'; o[l++] = H[c >> 4]; o[l++] = H[c & 15]; }
        else o[l++] = (char)c;
    }
    o[l] = 0; return o;
}

/* -------------------------------- Url -------------------------------- */

void url_free(Url *u) { free(u->userinfo); free(u->host); free(u->path); free(u->query); memset(u, 0, sizeof *u); }

bool url_default_port(const Url *u) { return u->port == (strcmp(u->scheme, "https") == 0 ? 443 : 80); }

static bool has_scheme(const char *s, size_t *len) {
    if (!isalpha((unsigned char)s[0])) return false;
    size_t i = 1;
    while (isalnum((unsigned char)s[i]) || s[i] == '+' || s[i] == '-' || s[i] == '.') i++;
    if (s[i] != ':') return false;
    *len = i; return true;
}

bool url_parse(const char *s, Url *u, char *err) {
    memset(u, 0, sizeof *u);
    while (isspace((unsigned char)*s)) s++;
    const char *p = s; size_t sl;
    if (has_scheme(s, &sl)) {
        if (sl >= sizeof u->scheme) { snprintf(err, HERR, "unsupported URL scheme in '%.60s'", s); return false; }
        for (size_t i = 0; i < sl; i++) u->scheme[i] = (char)tolower((unsigned char)s[i]);
        p = s + sl + 1;
        if (p[0] == '/' && p[1] == '/') p += 2;
        else if (strcmp(u->scheme, "http") && strcmp(u->scheme, "https")) { snprintf(err, HERR, "unsupported URL scheme '%s' (only http and https)", u->scheme); return false; }
        else { snprintf(err, HERR, "malformed URL '%.80s' (expected %s://host/...)", s, u->scheme); return false; }
    } else strcpy(u->scheme, "http"); /* lenient: "example.com/x" means http://example.com/x */
    if (strcmp(u->scheme, "http") && strcmp(u->scheme, "https")) { snprintf(err, HERR, "unsupported URL scheme '%s' (only http and https)", u->scheme); return false; }

    size_t alen = strcspn(p, "/?#");
    const char *auth = p, *rest = p + alen;
    const char *at = NULL;
    for (const char *c = auth; c < auth + alen; c++) if (*c == '@') at = c;
    if (at) { u->userinfo = kc_malloc((size_t)(at - auth) + 1); memcpy(u->userinfo, auth, (size_t)(at - auth)); u->userinfo[at - auth] = 0; auth = at + 1; alen = (size_t)(rest - auth); }
    const char *hs = auth, *he; const char *portp = NULL;
    if (*auth == '[') {
        const char *rb = memchr(auth, ']', alen);
        if (!rb) { snprintf(err, HERR, "malformed URL: unterminated IPv6 address"); url_free(u); return false; }
        hs = auth + 1; he = rb;
        if (rb + 1 < auth + alen) { if (rb[1] != ':') { snprintf(err, HERR, "malformed URL: junk after IPv6 address"); url_free(u); return false; } portp = rb + 2; }
    } else {
        const char *colon = NULL; for (const char *c = auth; c < auth + alen; c++) if (*c == ':') colon = c;
        he = colon ? colon : auth + alen; if (colon) portp = colon + 1;
    }
    size_t hl = (size_t)(he - hs);
    if (hl == 0) { snprintf(err, HERR, "malformed URL: empty host"); url_free(u); return false; }
    if (hl > 253) { snprintf(err, HERR, "malformed URL: host name too long"); url_free(u); return false; }
    u->host = kc_malloc(hl + 1);
    for (size_t i = 0; i < hl; i++) { unsigned char c = (unsigned char)hs[i]; if (c <= 0x20 || c == '/' || c == '\\' || c == '@') { snprintf(err, HERR, "malformed URL: bad character in host"); url_free(u); return false; } u->host[i] = (char)tolower(c); }
    u->host[hl] = 0;
    u->port = strcmp(u->scheme, "https") == 0 ? 443 : 80;
    if (portp && portp < auth + alen) {
        long pv = 0; const char *q = portp;
        if (q >= auth + alen) { /* "host:" -> default */ }
        for (; q < auth + alen; q++) { if (!isdigit((unsigned char)*q)) { snprintf(err, HERR, "malformed URL: bad port"); url_free(u); return false; } pv = pv * 10 + (*q - '0'); if (pv > 65535) break; }
        if (pv < 1 || pv > 65535) { snprintf(err, HERR, "malformed URL: port out of range"); url_free(u); return false; }
        u->port = (int)pv;
    }
    size_t flen = strcspn(rest, "#");                 /* drop the fragment */
    const char *q = memchr(rest, '?', flen);
    size_t plen = q ? (size_t)(q - rest) : flen;
    u->path = plen ? target_encode(rest, plen) : kc_strdup("/");
    if (q) u->query = target_encode(q + 1, flen - plen - 1);
    return true;
}

char *url_target(const Url *u) {
    size_t l = strlen(u->path) + (u->query ? strlen(u->query) + 1 : 0) + 1;
    char *o = kc_malloc(l + 1);
    snprintf(o, l + 1, "%s%s%s", u->path, u->query ? "?" : "", u->query ? u->query : "");
    return o;
}

char *url_format(const Url *u, bool with_userinfo) {
    char *t = url_target(u);
    size_t cap = strlen(t) + strlen(u->host) + (u->userinfo ? strlen(u->userinfo) : 0) + 40;
    char *o = kc_malloc(cap);
    bool v6 = strchr(u->host, ':') != NULL;
    int n = snprintf(o, cap, "%s://", u->scheme);
    if (with_userinfo && u->userinfo) n += snprintf(o + n, cap - (size_t)n, "%s@", u->userinfo);
    n += snprintf(o + n, cap - (size_t)n, v6 ? "[%s]" : "%s", u->host);
    if (!url_default_port(u)) n += snprintf(o + n, cap - (size_t)n, ":%d", u->port);
    snprintf(o + n, cap - (size_t)n, "%s", t);
    free(t); return o;
}

/* RFC 3986 5.2.4 remove_dot_segments for an absolute path. */
static char *remove_dots(const char *path) {
    size_t len = strlen(path);
    char *out = kc_malloc(len + 2); size_t ol = 0;
    size_t *seg = kc_malloc((len + 2) * sizeof *seg); size_t ns = 0; /* start offsets of kept segments in out */
    bool trailing = false;
    const char *p = path;
    while (*p) {
        if (*p == '/') p++;
        const char *e = p; while (*e && *e != '/') e++;
        size_t sl = (size_t)(e - p);
        trailing = (*e == '/') || false;
        if (sl == 1 && p[0] == '.') { trailing = true; }
        else if (sl == 2 && p[0] == '.' && p[1] == '.') { if (ns) { ol = seg[--ns]; } trailing = true; }
        else if (sl > 0) { seg[ns++] = ol; out[ol++] = '/'; memcpy(out + ol, p, sl); ol += sl; }
        else trailing = true;
        p = e;
    }
    if (ol == 0 || trailing) out[ol++] = '/';
    out[ol] = 0; free(seg);
    return out;
}

bool url_resolve(const Url *base, const char *ref, Url *out, char *err) {
    size_t sl;
    while (isspace((unsigned char)*ref)) ref++;
    if (has_scheme(ref, &sl)) return url_parse(ref, out, err);
    char *bauth = url_format(base, true);   /* scheme://[user@]host[:port]/path?query */
    char *root = kc_strdup(bauth);
    { char *s = strstr(root, "://") + 3; char *sl2 = strchr(s, '/'); if (sl2) *sl2 = 0; }
    char *full = NULL; size_t cap = strlen(bauth) + strlen(ref) * 2 + strlen(base->path) + 64;
    full = kc_malloc(cap);
    if (ref[0] == '/' && ref[1] == '/') snprintf(full, cap, "%s:%s", base->scheme, ref);
    else if (ref[0] == '/') {
        size_t pl = strcspn(ref, "?#"); char *dp = kc_malloc(pl + 1); memcpy(dp, ref, pl); dp[pl] = 0;
        char *clean = remove_dots(dp); snprintf(full, cap, "%s%s%s", root, clean, ref + pl); free(clean); free(dp);
    } else if (ref[0] == '?') snprintf(full, cap, "%s%s%s", root, base->path, ref);
    else if (ref[0] == '#' || ref[0] == 0) { char *t = url_target(base); snprintf(full, cap, "%s%s", root, t); free(t); }
    else {
        size_t pl = strcspn(ref, "?#");
        const char *ls = strrchr(base->path, '/'); size_t dl = ls ? (size_t)(ls - base->path) + 1 : 1;
        char *merged = kc_malloc(dl + pl + 2); memcpy(merged, base->path, dl); memcpy(merged + dl, ref, pl); merged[dl + pl] = 0;
        char *clean = remove_dots(merged);
        snprintf(full, cap, "%s%s%s", root, clean, ref + pl);
        free(clean); free(merged);
    }
    bool ok = url_parse(full, out, err);
    free(full); free(root); free(bauth);
    return ok;
}

/* -------------------------------- base64 -------------------------------- */

char *ht_b64_encode(const uint8_t *d, size_t n, size_t *outlen) {
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char *o = kc_malloc((n + 2) / 3 * 4 + 1); size_t l = 0, i = 0;
    for (; i + 2 < n; i += 3) {
        uint32_t v = (uint32_t)d[i] << 16 | (uint32_t)d[i + 1] << 8 | d[i + 2];
        o[l++] = T[v >> 18]; o[l++] = T[(v >> 12) & 63]; o[l++] = T[(v >> 6) & 63]; o[l++] = T[v & 63];
    }
    if (i < n) {
        uint32_t v = (uint32_t)d[i] << 16 | (i + 1 < n ? (uint32_t)d[i + 1] << 8 : 0);
        o[l++] = T[v >> 18]; o[l++] = T[(v >> 12) & 63]; o[l++] = i + 1 < n ? T[(v >> 6) & 63] : '='; o[l++] = '=';
    }
    o[l] = 0; if (outlen) *outlen = l; return o;
}

bool ht_b64_decode(const char *s, size_t n, SysBuf *out) {
    uint32_t acc = 0; int bits = 0; size_t pad = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i]; int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A'; else if (c >= 'a' && c <= 'z') v = c - 'a' + 26; else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+' || c == '-') v = 62; else if (c == '/' || c == '_') v = 63;
        else if (c == '=') { pad++; continue; } else if (isspace(c)) continue; else return false;
        if (pad) return false;               /* data after padding */
        acc = acc << 6 | (uint32_t)v; bits += 6;
        if (bits >= 8) { bits -= 8; char b = (char)((acc >> bits) & 0xff); sysbuf_append(out, &b, 1); }
    }
    return true;
}

/* ------------------------------ entry points ------------------------------ */

static KcValue hu_url_parse(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    const char *sig = "http.url_parse(url: string) -> Url { scheme, host, port, path, query, userinfo, target }  (null if malformed)";
    sys_argc(n, 1, 1, sig, pos);
    Url u; char err[HERR];
    if (!url_parse(sys_str(a, 0, sig, pos), &u, err)) { ht_set_error("%s", err); return kc_null(); }
    char *t = url_target(&u);
    static const char *names[] = { "scheme", "host", "port", "path", "query", "userinfo", "target" };
    KcValue v[7] = { kc_string(u.scheme), kc_string(u.host), kc_int(u.port), kc_string(u.path), kc_string(u.query ? u.query : ""), kc_string(u.userinfo ? u.userinfo : ""), kc_string(t) };
    free(t); url_free(&u);
    return sys_struct("Url", 7, names, v);
}

static KcValue hu_url_join(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    const char *sig = "http.url_join(base: string, ref: string) -> string  (RFC 3986 reference resolution)";
    sys_argc(n, 2, 2, sig, pos);
    Url b, r; char err[HERR];
    if (!url_parse(sys_str(a, 0, sig, pos), &b, err)) { ht_set_error("%s", err); return kc_null(); }
    bool ok = url_resolve(&b, sys_str(a, 1, sig, pos), &r, err);
    url_free(&b);
    if (!ok) { ht_set_error("%s", err); return kc_null(); }
    char *s = url_format(&r, true); url_free(&r);
    return kc_string_take(s, (int)strlen(s));
}

static KcValue hu_url_encode(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    const char *sig = "http.url_encode(text: string) -> string  (percent-encodes everything but A-Z a-z 0-9 - . _ ~)";
    sys_argc(n, 1, 1, sig, pos); sys_str(a, 0, sig, pos);
    size_t l; char *o = ht_pct_encode(a[0].as.s->data, (size_t)a[0].as.s->len, false, &l);
    return sys_bytes_str(kc_realloc(o, l + 1), l);
}

static KcValue hu_url_decode(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    const char *sig = "http.url_decode(text: string, plus_is_space: bool = false) -> string";
    sys_argc(n, 1, 2, sig, pos); sys_str(a, 0, sig, pos);
    bool plus = sys_opt_bool(a, n, 1, false, sig, pos);
    size_t len = (size_t)a[0].as.s->len; char *o = kc_malloc(len + 1);
    size_t l = ht_pct_decode(a[0].as.s->data, len, o, plus);
    return sys_bytes_str(o, l);
}

static KcValue pair_value(const char *s, size_t n, bool plus) {
    char *o = kc_malloc(n + 1); size_t l = ht_pct_decode(s, n, o, plus);
    return sys_bytes_str(o, l);
}

static KcValue hu_query_parse(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    const char *sig = "http.query_parse(query: string) -> array of [key, value]  (percent- and +-decoded; a leading ? is ignored)";
    sys_argc(n, 1, 1, sig, pos);
    const char *s = sys_str(a, 0, sig, pos);
    if (*s == '?') s++;
    KcValue out = kc_array_new();
    while (*s) {
        size_t el = strcspn(s, "&"); const char *eq = memchr(s, '=', el);
        if (el) {
            size_t kl = eq ? (size_t)(eq - s) : el;
            KcValue pr = kc_array_new();
            kc_array_push(pr, pair_value(s, kl, true));
            kc_array_push(pr, eq ? pair_value(eq + 1, el - kl - 1, true) : kc_string(""));
            kc_array_push(out, pr);
        }
        s += el; if (*s == '&') s++;
    }
    return out;
}

static void append_enc(SysBuf *b, KcValue v, KcPos pos, const char *sig) {
    char tmp[64]; const char *s; size_t sl;
    if (v.type == VAL_STRING) { s = v.as.s->data; sl = (size_t)v.as.s->len; }
    else if (v.type == VAL_INT) { sl = (size_t)snprintf(tmp, sizeof tmp, "%lld", (long long)v.as.i); s = tmp; }
    else if (v.type == VAL_FLOAT) { sl = (size_t)snprintf(tmp, sizeof tmp, "%g", v.as.f); s = tmp; }
    else if (v.type == VAL_BOOL) { s = v.as.b ? "true" : "false"; sl = strlen(s); }
    else { sys_usage(pos, sig); return; }
    size_t l; char *e = ht_pct_encode(s, sl, true, &l); sysbuf_append(b, e, l); free(e);
}

static KcValue hu_form(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    const char *sig = "http.form(pairs: array of [key, value]) -> string  (application/x-www-form-urlencoded)";
    sys_argc(n, 1, 1, sig, pos);
    if (a[0].type != VAL_ARRAY) sys_usage(pos, sig);
    SysBuf b = {0};
    for (int i = 0; i < a[0].as.arr->count; i++) {
        KcValue pr = a[0].as.arr->items[i];
        if (pr.type != VAL_ARRAY || pr.as.arr->count != 2) kc_error_at(pos, "http.form: element %d must be a [key, value] pair", i);
        if (b.len) sysbuf_append(&b, "&", 1);
        append_enc(&b, pr.as.arr->items[0], pos, sig); sysbuf_append(&b, "=", 1); append_enc(&b, pr.as.arr->items[1], pos, sig);
    }
    if (!b.p) return kc_string("");
    return sys_bytes_str(b.p, b.len);
}

static KcValue hu_b64e(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    const char *sig = "http.base64_encode(data: string) -> string";
    sys_argc(n, 1, 1, sig, pos); sys_str(a, 0, sig, pos);
    size_t l; char *o = ht_b64_encode((const uint8_t *)a[0].as.s->data, (size_t)a[0].as.s->len, &l);
    return sys_bytes_str(kc_realloc(o, l + 1), l);
}

static KcValue hu_b64d(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    const char *sig = "http.base64_decode(text: string) -> string  (standard or URL-safe alphabet; null if invalid)";
    sys_argc(n, 1, 1, sig, pos); sys_str(a, 0, sig, pos);
    SysBuf b = {0};
    if (!ht_b64_decode(a[0].as.s->data, (size_t)a[0].as.s->len, &b)) { sysbuf_free(&b); ht_set_error("http.base64_decode: invalid base64 input"); return kc_null(); }
    if (!b.p) return kc_string("");
    return sys_bytes_str(b.p, b.len);
}

static KcValue hu_status_text(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    const char *sig = "http.status_text(code: int) -> string  (\"Not Found\" for 404; \"\" if unknown)";
    sys_argc(n, 1, 1, sig, pos);
    return kc_string(ht_reason((int)sys_int(a, 0, sig, pos)));
}

const HtEntry ht_url_entries[] = {
    {"url_parse", hu_url_parse}, {"url_join", hu_url_join}, {"url_encode", hu_url_encode}, {"url_decode", hu_url_decode},
    {"query_parse", hu_query_parse}, {"form", hu_form}, {"base64_encode", hu_b64e}, {"base64_decode", hu_b64d},
    {"status_text", hu_status_text},
};
const int ht_url_count = (int)(sizeof ht_url_entries / sizeof ht_url_entries[0]);
