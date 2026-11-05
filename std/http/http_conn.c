/* KingCobra std.http — connections and HTTP/1.1 message parsing, shared by
 * the client and the server.
 *
 * Every socket is non-blocking and every operation takes an absolute
 * deadline (kc_monotonic_seconds()), so a stalled peer can never hang the
 * interpreter. Parsing is defensive: header sections are capped (128 KiB,
 * 256 headers), body size is capped by the caller's Sink, conflicting or
 * invalid Content-Length / Transfer-Encoding framing is rejected (request
 * smuggling defence), and a body shorter than promised is an error rather
 * than silently truncated data.
 */
#include "http_glue.h"

#define MAX_HEADER_BYTES (128 * 1024)
#define MAX_HEADER_COUNT 256
#define MAX_LINE 16384

/* ------------------------------- errors ------------------------------- */

static char g_err[HERR * 2];
void ht_set_error(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vsnprintf(g_err, sizeof g_err, fmt, ap); va_end(ap); }
const char *ht_get_error(void) { return g_err; }

KcValue ht_make_handle(int subtype, void *ptr) {
    KcHandle *h = kc_malloc(sizeof *h);
    h->kind = H_HTTP; h->ptr = ptr; h->size = 0; h->extra = subtype;
    KcValue v; v.type = VAL_HANDLE; v.as.handle = h; return v;
}

const char *ht_reason(int s) {
    switch (s) {
    case 100: return "Continue"; case 101: return "Switching Protocols"; case 102: return "Processing"; case 103: return "Early Hints";
    case 200: return "OK"; case 201: return "Created"; case 202: return "Accepted"; case 203: return "Non-Authoritative Information";
    case 204: return "No Content"; case 205: return "Reset Content"; case 206: return "Partial Content";
    case 300: return "Multiple Choices"; case 301: return "Moved Permanently"; case 302: return "Found"; case 303: return "See Other";
    case 304: return "Not Modified"; case 307: return "Temporary Redirect"; case 308: return "Permanent Redirect";
    case 400: return "Bad Request"; case 401: return "Unauthorized"; case 402: return "Payment Required"; case 403: return "Forbidden";
    case 404: return "Not Found"; case 405: return "Method Not Allowed"; case 406: return "Not Acceptable"; case 407: return "Proxy Authentication Required";
    case 408: return "Request Timeout"; case 409: return "Conflict"; case 410: return "Gone"; case 411: return "Length Required";
    case 412: return "Precondition Failed"; case 413: return "Content Too Large"; case 414: return "URI Too Long"; case 415: return "Unsupported Media Type";
    case 416: return "Range Not Satisfiable"; case 417: return "Expectation Failed"; case 418: return "I'm a teapot"; case 421: return "Misdirected Request";
    case 422: return "Unprocessable Content"; case 423: return "Locked"; case 426: return "Upgrade Required"; case 428: return "Precondition Required";
    case 429: return "Too Many Requests"; case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error"; case 501: return "Not Implemented"; case 502: return "Bad Gateway"; case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout"; case 505: return "HTTP Version Not Supported"; case 507: return "Insufficient Storage";
    default: return "";
    }
}

void ht_http_date(char *out, size_t cap, time_t t) {
    static const char *D[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char *M[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    struct tm tm;
#if defined(KC_PLATFORM_WINDOWS)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    snprintf(out, cap, "%s, %02d %s %04d %02d:%02d:%02d GMT", D[tm.tm_wday], tm.tm_mday, M[tm.tm_mon], tm.tm_year + 1900, tm.tm_hour, tm.tm_min, tm.tm_sec);
}

/* ------------------------------- headers ------------------------------- */

bool ht_name_eq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    return *a == *b;
}

bool ht_token_ok(const char *s, size_t n) {
    if (!n) return false;
    for (size_t i = 0; i < n; i++) { unsigned char c = (unsigned char)s[i]; if (!(isalnum(c) || strchr("!#$%&'*+-.^_`|~", c))) return false; }
    return true;
}

void hh_add(HHdrs *h, const char *name, size_t nl, const char *val, size_t vl) {
    if (h->n == h->cap) { h->cap = h->cap ? h->cap * 2 : 16; h->v = kc_realloc(h->v, (size_t)h->cap * sizeof *h->v); }
    char *n = kc_malloc(nl + 1), *v = kc_malloc(vl + 1);
    memcpy(n, name, nl); n[nl] = 0; memcpy(v, val, vl); v[vl] = 0;
    h->v[h->n].name = n; h->v[h->n].value = v; h->n++;
}

void hh_remove(HHdrs *h, const char *name) {
    int j = 0;
    for (int i = 0; i < h->n; i++) {
        if (ht_name_eq(h->v[i].name, name)) { free(h->v[i].name); free(h->v[i].value); }
        else h->v[j++] = h->v[i];
    }
    h->n = j;
}

void hh_set(HHdrs *h, const char *name, const char *val) { hh_remove(h, name); hh_add(h, name, strlen(name), val, strlen(val)); }

const char *hh_get(const HHdrs *h, const char *name) {
    for (int i = 0; i < h->n; i++) if (ht_name_eq(h->v[i].name, name)) return h->v[i].value;
    return NULL;
}

void hh_free(HHdrs *h) {
    for (int i = 0; i < h->n; i++) { free(h->v[i].name); free(h->v[i].value); }
    free(h->v); memset(h, 0, sizeof *h);
}

/* ------------------------------ connection ------------------------------ */

Conn *conn_new(kc_sock_t fd, TlsConn *tls) {
    Conn *c = kc_malloc(sizeof *c); memset(c, 0, sizeof *c);
    c->fd = fd; c->tls = tls; c->last_used = kc_monotonic_seconds();
    return c;
}

void conn_free(Conn *c) {
    if (!c) return;
    if (c->tls) tls_close(c->tls);
    if (c->fd != KC_SOCK_INVALID) kc_sock_close(c->fd);
    free(c->buf); free(c->host); free(c);
}

#if defined(MSG_NOSIGNAL)
#define HT_SEND_FLAGS MSG_NOSIGNAL
#else
#define HT_SEND_FLAGS 0
#endif

long conn_read(Conn *c, void *dst, size_t n, double dl, char *err) {
    long r;
    if (c->tls) r = tls_read(c->tls, c->fd, dst, n, dl, err);
    else for (;;) {
        r = (long)recv(c->fd, dst, (int)(n > (size_t)INT_MAX ? INT_MAX : n), 0);
        if (r >= 0) break;
        if (!ht_wouldblock()) { ht_sockerr(err, "connection error"); return -1; }
        int w = ht_wait(c->fd, false, dl);
        if (w == 0) { snprintf(err, HERR, "timed out waiting for the server"); return -1; }
        if (w < 0) { ht_sockerr(err, "connection error"); return -1; }
    }
    if (r > 0) c->nread += (size_t)r;
    return r;
}

bool conn_write(Conn *c, const char *d, size_t n, double dl, char *err) {
    if (c->tls) return tls_write(c->tls, c->fd, d, n, dl, err);
    while (n) {
        long r = (long)send(c->fd, d, (int)(n > (size_t)INT_MAX ? INT_MAX : n), HT_SEND_FLAGS);
        if (r > 0) { d += r; n -= (size_t)r; continue; }
        if (r < 0 && !ht_wouldblock()) { ht_sockerr(err, "connection error while sending"); return false; }
        int w = ht_wait(c->fd, true, dl);
        if (w == 0) { snprintf(err, HERR, "timed out sending the request"); return false; }
        if (w < 0) { ht_sockerr(err, "connection error while sending"); return false; }
    }
    return true;
}

/* Pull more bytes into the read-ahead buffer. >0 appended, 0 EOF, -1 error. */
static long fill(Conn *c, double dl, char *err) {
    if (c->pos == c->len) c->pos = c->len = 0;
    if (c->len == c->cap) {
        if (c->pos > 0) { memmove(c->buf, c->buf + c->pos, c->len - c->pos); c->len -= c->pos; c->pos = 0; }
        else { c->cap = c->cap ? c->cap * 2 : 16384; c->buf = kc_realloc(c->buf, c->cap); }
    }
    long r = conn_read(c, c->buf + c->len, c->cap - c->len, dl, err);
    if (r > 0) c->len += (size_t)r;
    return r;
}

int conn_getline(Conn *c, SysBuf *line, size_t maxlen, double dl, char *err) {
    line->len = 0; if (line->p) line->p[0] = 0;
    size_t scanned = 0; bool any = false;
    for (;;) {
        size_t avail = c->len - c->pos;
        char *nl = avail > scanned ? memchr(c->buf + c->pos + scanned, '\n', avail - scanned) : NULL;
        if (nl) {
            size_t ll = (size_t)(nl - (c->buf + c->pos));
            if (ll > maxlen) { snprintf(err, HERR, "header line too long"); return -1; }
            size_t keep = ll; if (keep && c->buf[c->pos + keep - 1] == '\r') keep--;
            sysbuf_append(line, c->buf + c->pos, keep); if (!line->p) sysbuf_append(line, "", 0);
            c->pos += ll + 1;
            return 1;
        }
        scanned = avail;
        if (avail > maxlen) { snprintf(err, HERR, "header line too long"); return -1; }
        if (avail) any = true;
        long r = fill(c, dl, err);
        if (r < 0) return -1;
        if (r == 0) {
            if (!any) return 0;
            snprintf(err, HERR, "connection closed in the middle of a header line"); return -1;
        }
    }
}

/* ------------------------------- sink ------------------------------- */

bool sink_put(Sink *s, const char *d, size_t n, char *err) {
    if (s->total + n > s->max) { snprintf(err, HERR, "response body is larger than the limit of %zu bytes (raise it with set_option(\"max_body\", ...))", s->max); return false; }
    if (s->fp) { if (n && fwrite(d, 1, n, s->fp) != n) { snprintf(err, HERR, "could not write the downloaded data: %s", strerror(errno)); return false; } }
    else sysbuf_append(&s->mem, d, n);
    s->total += n;
    return true;
}

/* ------------------------------ headers ------------------------------ */

bool conn_read_headers(Conn *c, HHdrs *h, double dl, char *err) {
    SysBuf line = {0}; size_t total = 0; bool ok = false;
    for (;;) {
        int r = conn_getline(c, &line, MAX_LINE, dl, err);
        if (r < 0) goto done;
        if (r == 0) { snprintf(err, HERR, "connection closed before the headers ended"); goto done; }
        total += line.len + 2;
        if (total > MAX_HEADER_BYTES) { snprintf(err, HERR, "header section larger than %d bytes", MAX_HEADER_BYTES); goto done; }
        if (line.len == 0) { ok = true; goto done; }
        if (line.p[0] == ' ' || line.p[0] == '\t') {                    /* obsolete line folding: join onto previous value */
            if (!h->n) { snprintf(err, HERR, "malformed header (continuation with no header)"); goto done; }
            HHdr *p = &h->v[h->n - 1]; size_t ol = strlen(p->value);
            const char *t = line.p; while (*t == ' ' || *t == '\t') t++;
            p->value = kc_realloc(p->value, ol + strlen(t) + 2); p->value[ol] = ' '; strcpy(p->value + ol + 1, t);
            continue;
        }
        if (h->n >= MAX_HEADER_COUNT) { snprintf(err, HERR, "more than %d headers", MAX_HEADER_COUNT); goto done; }
        char *colon = memchr(line.p, ':', line.len);
        if (!colon || !ht_token_ok(line.p, (size_t)(colon - line.p))) { snprintf(err, HERR, "malformed header line"); goto done; }
        const char *v = colon + 1; const char *ve = line.p + line.len;
        while (v < ve && (*v == ' ' || *v == '\t')) v++;
        while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
        hh_add(h, line.p, (size_t)(colon - line.p), v, (size_t)(ve - v));
    }
done:
    sysbuf_free(&line);
    return ok;
}

int conn_read_response_head(Conn *c, RespHead *r, double dl, char *err) {
    SysBuf line = {0};
    int g = conn_getline(c, &line, 8192, dl, err);
    if (g <= 0) { sysbuf_free(&line); return g; }
    while (line.len == 0) {                                          /* tolerate stray blank lines before a status line */
        g = conn_getline(c, &line, 8192, dl, err);
        if (g <= 0) { sysbuf_free(&line); if (g == 0) snprintf(err, HERR, "connection closed before a status line"); return -1; }
    }
    int ok = 1;
    if (line.len < 12 || strncmp(line.p, "HTTP/", 5) != 0) { snprintf(err, HERR, "the server did not answer with HTTP (got '%.40s')", line.p); ok = -1; }
    else {
        const char *sp = strchr(line.p, ' ');
        if (!sp || !isdigit((unsigned char)sp[1]) || !isdigit((unsigned char)sp[2]) || !isdigit((unsigned char)sp[3])) { snprintf(err, HERR, "malformed status line"); ok = -1; }
        else {
            r->status = (sp[1] - '0') * 100 + (sp[2] - '0') * 10 + (sp[3] - '0');
            size_t vl = (size_t)(sp - line.p); if (vl >= sizeof r->version) vl = sizeof r->version - 1;
            memcpy(r->version, line.p, vl); r->version[vl] = 0;
            snprintf(r->reason, sizeof r->reason, "%s", sp[4] == ' ' ? sp + 5 : "");
            if (r->reason[0] == 0) snprintf(r->reason, sizeof r->reason, "%s", ht_reason(r->status));
        }
    }
    sysbuf_free(&line);
    if (ok < 0) return -1;
    return conn_read_headers(c, &r->hdrs, dl, err) ? 1 : -1;
}

/* -------------------------------- framing -------------------------------- */

bool ht_body_framing(const HHdrs *h, BodyMode *m, size_t *clen, char *err) {
    *m = BODY_CLOSE; *clen = 0;
    const char *te = NULL;
    for (int i = 0; i < h->n; i++) if (ht_name_eq(h->v[i].name, "Transfer-Encoding")) te = h->v[i].value;
    bool has_cl = false; unsigned long long cl = 0;
    for (int i = 0; i < h->n; i++) {
        if (!ht_name_eq(h->v[i].name, "Content-Length")) continue;
        const char *v = h->v[i].value; char *end;
        if (!isdigit((unsigned char)*v)) { snprintf(err, HERR, "invalid Content-Length '%.30s'", v); return false; }
        errno = 0; unsigned long long x = strtoull(v, &end, 10);
        while (*end == ' ') end++;
        if (*end || errno || x > (unsigned long long)SIZE_MAX / 2) { snprintf(err, HERR, "invalid Content-Length '%.30s'", v); return false; }
        if (has_cl && x != cl) { snprintf(err, HERR, "conflicting Content-Length headers"); return false; }
        has_cl = true; cl = x;
    }
    if (te) {
        const char *last = strrchr(te, ','); last = last ? last + 1 : te; while (*last == ' ') last++;
        if (ht_name_eq(last, "chunked")) { *m = BODY_CHUNKED; return true; }   /* Transfer-Encoding overrides Content-Length */
        snprintf(err, HERR, "unsupported Transfer-Encoding '%.30s'", te); return false;
    }
    if (has_cl) { *m = BODY_LENGTH; *clen = (size_t)cl; }
    return true;
}

bool conn_read_body(Conn *c, BodyMode m, size_t clen, Sink *s, double dl, char *err) {
    if (m == BODY_NONE) return true;
    if (m == BODY_LENGTH) {
        size_t left = clen;
        if (left > s->max) { snprintf(err, HERR, "response body (%zu bytes) is larger than the limit of %zu bytes", left, s->max); return false; }
        while (left) {
            if (c->pos == c->len) {
                long r = fill(c, dl, err);
                if (r < 0) return false;
                if (r == 0) { snprintf(err, HERR, "connection closed after %zu of %zu body bytes", clen - left, clen); return false; }
            }
            size_t k = c->len - c->pos; if (k > left) k = left;
            if (!sink_put(s, c->buf + c->pos, k, err)) return false;
            c->pos += k; left -= k;
        }
        return true;
    }
    if (m == BODY_CLOSE) {
        for (;;) {
            if (c->pos < c->len) { if (!sink_put(s, c->buf + c->pos, c->len - c->pos, err)) return false; c->pos = c->len; }
            long r = fill(c, dl, err);
            if (r < 0) return false;
            if (r == 0) return true;
        }
    }
    /* chunked */
    SysBuf line = {0}; bool ok = false;
    for (;;) {
        int g = conn_getline(c, &line, 1024, dl, err);
        if (g < 0) goto out;
        if (g == 0) { snprintf(err, HERR, "connection closed inside a chunked body"); goto out; }
        char *e; errno = 0;
        const char *p = line.p; while (*p == ' ') p++;
        if (!isxdigit((unsigned char)*p)) { snprintf(err, HERR, "invalid chunk size"); goto out; }
        unsigned long long sz = strtoull(p, &e, 16);
        if (errno || sz > (unsigned long long)SIZE_MAX / 2) { snprintf(err, HERR, "invalid chunk size"); goto out; }
        if (sz == 0) {                                                  /* last chunk, then optional trailers */
            for (;;) {
                g = conn_getline(c, &line, MAX_LINE, dl, err);
                if (g < 0) goto out;
                if (g == 0 || line.len == 0) break;
            }
            ok = true; goto out;
        }
        size_t left = (size_t)sz;
        if (s->total + left > s->max) { snprintf(err, HERR, "response body is larger than the limit of %zu bytes", s->max); goto out; }
        while (left) {
            if (c->pos == c->len) {
                long r = fill(c, dl, err);
                if (r < 0) goto out;
                if (r == 0) { snprintf(err, HERR, "connection closed inside a chunk"); goto out; }
            }
            size_t k = c->len - c->pos; if (k > left) k = left;
            if (!sink_put(s, c->buf + c->pos, k, err)) goto out;
            c->pos += k; left -= k;
        }
        g = conn_getline(c, &line, 16, dl, err);
        if (g <= 0 || line.len != 0) { if (g >= 0) snprintf(err, HERR, "malformed chunk terminator"); goto out; }
    }
out:
    sysbuf_free(&line);
    return ok;
}
