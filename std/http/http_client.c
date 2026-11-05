/* KingCobra std.http — the HTTP/1.1 client.
 *
 *   one-shot      http.get(url) / post / put / patch / delete / head / options / request
 *   configured    let c = http.client(); http.set_option(c, "timeout", 5); http.get(c, url)
 *   files         http.download(url, path)  — streams to disk, atomic rename, never held in memory
 *
 * Features: http and https (verified TLS via run-time OpenSSL), request
 * headers, any body (binary-safe), real deadlines (connect + total), redirect
 * following (301/302/303/307/308, relative Location, credentials stripped on
 * cross-origin hops, https->http refused), gzip/deflate decoding, chunked and
 * length-delimited and close-delimited bodies, keep-alive connection reuse with
 * safe retry of idempotent requests on a stale connection, a cookie jar,
 * Basic / Bearer auth, userinfo in URLs, and size limits.
 *
 * Responses are `HttpResponse { status, reason, ok, version, url, redirects,
 * headers, body, length, elapsed, tls, decoded }`. A failure to get any
 * response returns null and sets http.last_error(); an error *status* is a
 * normal response (ok == false).
 *
 * Not implemented: proxies, HTTP/2, multipart helpers, response streaming to
 * the script (use download() for large bodies).
 */
#include "http_glue.h"

/* -------------------------------- types -------------------------------- */

typedef struct { char *name, *value, *domain, *path; bool secure, hostonly; double expires; } Cookie;

typedef struct HClient {
    double timeout, connect_timeout;
    int max_redirects;
    bool follow, verify, cookies_on, keepalive, decompress;
    size_t max_body;
    char *ca_file, *user_agent, *auth;        /* auth: complete Authorization value, e.g. "Bearer abc" */
    HHdrs defaults;
    TlsCtx *tls;
    Conn *idle[4]; int nidle;
    Cookie *jar; int njar, capjar;
} HClient;

typedef struct {
    int status; char reason[128], version[16];
    HHdrs hdrs; Sink body;
    bool tls, decoded, saved;
    char *final_url; int redirects; double elapsed;
} HResp;

#define MAX_COOKIES 200
#define IDLE_MAX_AGE 30.0

static HClient *client_new(bool keepalive) {
    HClient *c = kc_malloc(sizeof *c); memset(c, 0, sizeof *c);
    c->timeout = 30; c->connect_timeout = 10; c->max_redirects = 10; c->follow = true; c->verify = true;
    c->keepalive = keepalive; c->decompress = true; c->max_body = (size_t)64 << 20;
    char ua[64]; snprintf(ua, sizeof ua, "KingCobra/%s", KC_VERSION); c->user_agent = kc_strdup(ua);
    return c;
}

static void cookie_free(Cookie *k) { free(k->name); free(k->value); free(k->domain); free(k->path); }

static void client_close_idle(HClient *c) { for (int i = 0; i < c->nidle; i++) conn_free(c->idle[i]); c->nidle = 0; }

static void client_free(HClient *c) {
    client_close_idle(c); tls_ctx_free(c->tls);
    for (int i = 0; i < c->njar; i++) cookie_free(&c->jar[i]);
    free(c->jar); hh_free(&c->defaults); free(c->ca_file); free(c->user_agent); free(c->auth); free(c);
}

static HClient *g_default;
static HClient *default_client(void) { if (!g_default) g_default = client_new(false); return g_default; }

static void resp_free(HResp *r) {
    hh_free(&r->hdrs); sysbuf_free(&r->body.mem);
    if (r->body.fp) fclose(r->body.fp);
    free(r->final_url); memset(r, 0, sizeof *r);
}

/* --------------------------------- cookies --------------------------------- */

static double wall_now(void) { return (double)time(NULL); }

static int64_t civil_days(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2; int64_t era = (y >= 0 ? y : y - 399) / 400; unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1; unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

/* "Wdy, DD Mon YYYY HH:MM:SS GMT" and the dash / 2-digit-year variants; -1 if unparseable. */
static double parse_http_date(const char *s) {
    static const char *M[] = { "jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec" };
    const char *c = strchr(s, ','); c = c ? c + 1 : s;
    while (*c && !isdigit((unsigned char)*c)) c++;
    int d, y, H, Mi, S; char mon[8];
    if (sscanf(c, "%d%*[ -]%3s%*[ -]%d %d:%d:%d", &d, mon, &y, &H, &Mi, &S) != 6) return -1;
    int mi = -1; for (int i = 0; i < 12; i++) if (strncasecmp(mon, M[i], 3) == 0) mi = i;
    if (mi < 0) return -1;
    if (y < 70) y += 2000; else if (y < 100) y += 1900;
    return (double)(civil_days(y, (unsigned)mi + 1, (unsigned)d) * 86400 + H * 3600 + Mi * 60 + S);
}

static bool domain_match(const char *host, const char *dom, bool hostonly) {
    if (strcasecmp(host, dom) == 0) return true;
    if (hostonly) return false;
    size_t hl = strlen(host), dl = strlen(dom);
    return hl > dl && host[hl - dl - 1] == '.' && strcasecmp(host + hl - dl, dom) == 0;
}

static bool path_match(const char *rp, const char *cp) {
    size_t cl = strlen(cp);
    if (strncmp(rp, cp, cl) != 0) return false;
    return cp[cl - 1] == '/' || rp[cl] == '/' || rp[cl] == 0;
}

static char *trim_dup(const char *s, const char *e) {
    while (s < e && isspace((unsigned char)*s)) s++;
    while (e > s && isspace((unsigned char)e[-1])) e--;
    char *o = kc_malloc((size_t)(e - s) + 1); memcpy(o, s, (size_t)(e - s)); o[e - s] = 0; return o;
}

static void jar_remove(HClient *c, int i) { cookie_free(&c->jar[i]); memmove(&c->jar[i], &c->jar[i + 1], (size_t)(c->njar - i - 1) * sizeof(Cookie)); c->njar--; }

static void cookie_store(HClient *c, const Url *u, const char *sc) {
    const char *semi = strchr(sc, ';'); const char *first_end = semi ? semi : sc + strlen(sc);
    const char *eq = memchr(sc, '=', (size_t)(first_end - sc));
    if (!eq) return;
    char *name = trim_dup(sc, eq), *value = trim_dup(eq + 1, first_end);
    if (!*name) { free(name); free(value); return; }
    char *domain = NULL, *path = NULL; bool secure = false, have_age = false; long age = 0; double exp = -1;
    const char *p = semi;
    while (p && *p == ';') {
        p++; const char *e = strchr(p, ';'); if (!e) e = p + strlen(p);
        const char *q = memchr(p, '=', (size_t)(e - p));
        char *key = trim_dup(p, q ? q : e), *val = q ? trim_dup(q + 1, e) : kc_strdup("");
        if (!strcasecmp(key, "domain")) { free(domain); const char *d = val; if (*d == '.') d++; domain = kc_strdup(d); for (char *t = domain; *t; t++) *t = (char)tolower((unsigned char)*t); }
        else if (!strcasecmp(key, "path")) { free(path); path = val[0] == '/' ? kc_strdup(val) : NULL; }
        else if (!strcasecmp(key, "max-age")) { char *end; long v = strtol(val, &end, 10); if (end != val) { have_age = true; age = v; } }
        else if (!strcasecmp(key, "expires")) exp = parse_http_date(val);
        else if (!strcasecmp(key, "secure")) secure = true;
        free(key); free(val); p = e;
    }
    bool hostonly = domain == NULL;
    if (domain && (!domain_match(u->host, domain, false) || (!strchr(domain, '.') && strcasecmp(domain, u->host) != 0) || ht_is_ip_literal(domain) != ht_is_ip_literal(u->host))) {
        free(name); free(value); free(domain); free(path); return;   /* a server may not set cookies for other sites or bare TLDs */
    }
    if (!domain) domain = kc_strdup(u->host);
    if (!path) {                                                      /* default-path (RFC 6265 5.1.4) */
        const char *ls = strrchr(u->path, '/');
        size_t pl = ls && ls != u->path ? (size_t)(ls - u->path) : 1;
        path = kc_malloc(pl + 1); memcpy(path, u->path, pl); path[pl] = 0;
        if (pl == 1) strcpy(path, "/");
    }
    double expires = 0, now = wall_now();
    if (have_age) expires = age <= 0 ? 1 : now + (double)age; else if (exp >= 0) expires = exp > 0 ? exp : 1;
    for (int i = 0; i < c->njar; i++)
        if (!strcmp(c->jar[i].name, name) && !strcasecmp(c->jar[i].domain, domain) && !strcmp(c->jar[i].path, path)) { jar_remove(c, i); break; }
    if (expires > 0 && expires <= now) { free(name); free(value); free(domain); free(path); return; }   /* server deleted it */
    if (c->njar == MAX_COOKIES) jar_remove(c, 0);
    if (c->njar == c->capjar) { c->capjar = c->capjar ? c->capjar * 2 : 16; c->jar = kc_realloc(c->jar, (size_t)c->capjar * sizeof(Cookie)); }
    Cookie *k = &c->jar[c->njar++]; k->name = name; k->value = value; k->domain = domain; k->path = path; k->secure = secure; k->hostonly = hostonly; k->expires = expires;
}

static char *cookie_header(HClient *c, const Url *u, bool https) {
    Cookie *m[MAX_COOKIES]; int nm = 0; double now = wall_now();
    for (int i = 0; i < c->njar;) {
        Cookie *k = &c->jar[i];
        if (k->expires > 0 && k->expires <= now) { jar_remove(c, i); continue; }
        if (domain_match(u->host, k->domain, k->hostonly) && path_match(u->path, k->path) && (!k->secure || https)) m[nm++] = k;
        i++;
    }
    if (!nm) return NULL;
    for (int i = 1; i < nm; i++) { Cookie *t = m[i]; int j = i; while (j > 0 && strlen(m[j - 1]->path) < strlen(t->path)) { m[j] = m[j - 1]; j--; } m[j] = t; }
    SysBuf b = {0};
    for (int i = 0; i < nm; i++) { if (i) sysbuf_append(&b, "; ", 2); sysbuf_append(&b, m[i]->name, strlen(m[i]->name)); sysbuf_append(&b, "=", 1); sysbuf_append(&b, m[i]->value, strlen(m[i]->value)); }
    return b.p;
}

/* -------------------------------- connecting -------------------------------- */

static bool readable_now(kc_sock_t fd) {
#if defined(KC_PLATFORM_WINDOWS)
    WSAPOLLFD p; p.fd = fd; p.events = POLLRDNORM; p.revents = 0; return WSAPoll(&p, 1, 0) > 0;
#else
    struct pollfd p; p.fd = fd; p.events = POLLIN; p.revents = 0; return poll(&p, 1, 0) > 0;
#endif
}

static Conn *dial(HClient *c, const Url *u, double dl, char *err) {
    static bool once;
    kc_sockets_init();
#if !defined(KC_PLATFORM_WINDOWS)
    if (!once) { signal(SIGPIPE, SIG_IGN); once = true; }
#else
    (void)once;
#endif
    bool https = strcmp(u->scheme, "https") == 0;
    if (https && !tls_available(err)) return NULL;
    struct addrinfo hints, *res = NULL; memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    char port[16]; snprintf(port, sizeof port, "%d", u->port);
    int gai = getaddrinfo(u->host, port, &hints, &res);
    if (gai != 0) {
#if defined(KC_PLATFORM_WINDOWS)
        snprintf(err, HERR, "could not resolve host '%.100s' (winsock error %d)", u->host, WSAGetLastError());
#else
        snprintf(err, HERR, "could not resolve host '%.100s': %s", u->host, gai_strerror(gai));
#endif
        return NULL;
    }
    double cdl = kc_monotonic_seconds() + c->connect_timeout; if (c->connect_timeout <= 0 || cdl > dl) cdl = dl;
    kc_sock_t fd = KC_SOCK_INVALID;
    snprintf(err, HERR, "could not connect to %.100s:%d", u->host, u->port);
    for (struct addrinfo *rp = res; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd == KC_SOCK_INVALID) continue;
        ht_nonblock(fd);
        bool ok = connect(fd, rp->ai_addr, (int)rp->ai_addrlen) == 0;
        if (!ok && ht_wouldblock()) {
            int w = ht_wait(fd, true, cdl);
            if (w == 0) { snprintf(err, HERR, "connecting to %.100s:%d timed out", u->host, u->port); }
            else if (w > 0) {
                int soerr = 0; socklen_t sl = sizeof soerr;
                getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&soerr, &sl);
                if (soerr == 0) ok = true;
                else {
#if defined(KC_PLATFORM_WINDOWS)
                    snprintf(err, HERR, "could not connect to %.100s:%d (winsock error %d)", u->host, u->port, soerr);
#else
                    snprintf(err, HERR, "could not connect to %.100s:%d: %s", u->host, u->port, strerror(soerr));
#endif
                }
            }
        } else if (!ok) {
            char what[160]; snprintf(what, sizeof what, "could not connect to %.100s:%d", u->host, u->port); ht_sockerr(err, what);
        }
        if (ok) break;
        kc_sock_close(fd); fd = KC_SOCK_INVALID;
        if (kc_monotonic_seconds() >= cdl) break;
    }
    freeaddrinfo(res);
    if (fd == KC_SOCK_INVALID) return NULL;
    int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    TlsConn *t = NULL;
    if (https) {
        if (!c->tls && !(c->tls = tls_ctx_new(c->verify, c->ca_file, err))) { kc_sock_close(fd); return NULL; }
        if (!(t = tls_connect(c->tls, fd, u->host, c->verify, dl, err))) { kc_sock_close(fd); return NULL; }
    }
    Conn *k = conn_new(fd, t); k->host = kc_strdup(u->host); k->port = u->port; k->https = https;
    return k;
}

static Conn *pool_take(HClient *c, const Url *u) {
    bool https = strcmp(u->scheme, "https") == 0;
    for (int i = c->nidle - 1; i >= 0; i--) {
        Conn *k = c->idle[i];
        if (k->https != https || k->port != u->port || strcmp(k->host, u->host) != 0) continue;
        memmove(&c->idle[i], &c->idle[i + 1], (size_t)(c->nidle - i - 1) * sizeof(Conn *)); c->nidle--;
        if (kc_monotonic_seconds() - k->last_used > IDLE_MAX_AGE || k->pos != k->len || readable_now(k->fd)) { conn_free(k); continue; }  /* server closed it, or sent junk */
        return k;
    }
    return NULL;
}

static void pool_put(HClient *c, Conn *k) {
    k->last_used = kc_monotonic_seconds();
    if (c->nidle == 4) { conn_free(c->idle[0]); memmove(&c->idle[0], &c->idle[1], 3 * sizeof(Conn *)); c->nidle--; }
    c->idle[c->nidle++] = k;
}

/* ------------------------------ one exchange ------------------------------ */

static bool is_idempotent(const char *m) { return !strcmp(m, "GET") || !strcmp(m, "HEAD") || !strcmp(m, "PUT") || !strcmp(m, "DELETE") || !strcmp(m, "OPTIONS") || !strcmp(m, "TRACE"); }

static bool has_token(const char *list, const char *tok) {
    size_t tl = strlen(tok);
    for (const char *p = list; *p;) {
        while (*p == ' ' || *p == ',') p++;
        const char *e = p; while (*e && *e != ',' && *e != ' ') e++;
        if ((size_t)(e - p) == tl && strncasecmp(p, tok, tl) == 0) return true;
        p = e;
    }
    return false;
}

static bool is_cred_header(const char *n) { return ht_name_eq(n, "Authorization") || ht_name_eq(n, "Cookie") || ht_name_eq(n, "Proxy-Authorization"); }

static bool replace_file(const char *from, const char *to) {
#if defined(KC_PLATFORM_WINDOWS)
    return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING) != 0;
#else
    return rename(from, to) == 0;
#endif
}

static bool exchange(HClient *c, const char *method, const Url *u, const char *body, size_t blen, const HHdrs *user, bool send_creds,
                     const char *dl_path, double dl, HResp *r, char *err) {
    bool https = strcmp(u->scheme, "https") == 0;
    /* ---- assemble the effective header set ---- */
    HHdrs eff = {0};
    for (int i = 0; i < c->defaults.n; i++) if (send_creds || !is_cred_header(c->defaults.v[i].name)) hh_set(&eff, c->defaults.v[i].name, c->defaults.v[i].value);
    for (int i = 0; i < user->n; i++) if (send_creds || !is_cred_header(user->v[i].name)) hh_set(&eff, user->v[i].name, user->v[i].value);
    hh_remove(&eff, "Content-Length"); hh_remove(&eff, "Transfer-Encoding"); hh_remove(&eff, "Connection");
    if (!hh_get(&eff, "Host")) {
        char host[300]; bool v6 = strchr(u->host, ':') != NULL;
        if (url_default_port(u)) snprintf(host, sizeof host, v6 ? "[%s]" : "%s", u->host); else snprintf(host, sizeof host, v6 ? "[%s]:%d" : "%s:%d", u->host, u->port);
        hh_set(&eff, "Host", host);
    }
    if (!hh_get(&eff, "User-Agent")) hh_set(&eff, "User-Agent", c->user_agent);
    if (!hh_get(&eff, "Accept")) hh_set(&eff, "Accept", "*/*");
    if (!hh_get(&eff, "Accept-Encoding")) hh_set(&eff, "Accept-Encoding", (c->decompress && !dl_path) ? "gzip, deflate" : "identity");
    if (send_creds && !hh_get(&eff, "Authorization")) {
        if (u->userinfo) {
            char *raw = kc_malloc(strlen(u->userinfo) + 1); size_t rl = ht_pct_decode(u->userinfo, strlen(u->userinfo), raw, false);
            size_t el; char *b = ht_b64_encode((const uint8_t *)raw, rl, &el);
            char *v = kc_malloc(el + 8); snprintf(v, el + 8, "Basic %s", b); hh_set(&eff, "Authorization", v); free(v); free(b); free(raw);
        } else if (c->auth) hh_set(&eff, "Authorization", c->auth);
    }
    if (c->cookies_on && !hh_get(&eff, "Cookie")) { char *ch = cookie_header(c, u, https); if (ch) { hh_set(&eff, "Cookie", ch); free(ch); } }
    if (blen > 0 || !strcmp(method, "POST") || !strcmp(method, "PUT") || !strcmp(method, "PATCH")) { char nb[32]; snprintf(nb, sizeof nb, "%zu", blen); hh_set(&eff, "Content-Length", nb); }
    hh_set(&eff, "Connection", c->keepalive ? "keep-alive" : "close");

    char *target = url_target(u);
    SysBuf req = {0};
    sysbuf_append(&req, method, strlen(method)); sysbuf_append(&req, " ", 1); sysbuf_append(&req, target, strlen(target)); sysbuf_append(&req, " HTTP/1.1\r\n", 11);
    free(target);
    for (int i = 0; i < eff.n; i++) {
        sysbuf_append(&req, eff.v[i].name, strlen(eff.v[i].name)); sysbuf_append(&req, ": ", 2);
        sysbuf_append(&req, eff.v[i].value, strlen(eff.v[i].value)); sysbuf_append(&req, "\r\n", 2);
    }
    sysbuf_append(&req, "\r\n", 2);
    if (blen && blen <= 65536) sysbuf_append(&req, body, blen);
    hh_free(&eff);

    bool ok = false; RespHead head; memset(&head, 0, sizeof head);
    Conn *k = NULL; char tmp_path[4096] = "";
    for (int attempt = 0; attempt < 2; attempt++) {
        bool reused = false;
        if (attempt == 0 && c->keepalive && (k = pool_take(c, u))) reused = true;
        if (!k && !(k = dial(c, u, dl, err))) goto finish;
        size_t nread0 = k->nread;
        bool sent = conn_write(k, req.p, req.len, dl, err) && (blen <= 65536 || conn_write(k, body, blen, dl, err));
        int rc = sent ? 1 : -2;
        if (sent) {
            for (;;) {
                hh_free(&head.hdrs); memset(&head, 0, sizeof head);
                rc = conn_read_response_head(k, &head, dl, err);
                if (rc != 1 || head.status < 100 || head.status >= 200 || head.status == 101) break;   /* skip 100/102/103 interim responses */
            }
        }
        if (rc != 1) {
            bool timed_out = kc_monotonic_seconds() >= dl || strstr(err, "timed out") != NULL;
            if (reused && !timed_out && is_idempotent(method) && (rc == -2 || k->nread == nread0)) { conn_free(k); k = NULL; err[0] = 0; continue; }   /* stale keep-alive connection: retry once on a fresh one */
            if (rc == 0) snprintf(err, HERR, "the server closed the connection without sending a response");
            goto finish;
        }
        break;
    }
    if (!k) goto finish;

    {
        BodyMode m; size_t clen = 0;
        bool nobody = !strcmp(method, "HEAD") || head.status == 204 || head.status == 304 || head.status == 101 || (head.status >= 100 && head.status < 200);
        if (nobody) m = BODY_NONE; else if (!ht_body_framing(&head.hdrs, &m, &clen, err)) goto finish;
        r->body.max = c->max_body;
        if (dl_path && head.status >= 200 && head.status < 300 && m != BODY_NONE) {
            snprintf(tmp_path, sizeof tmp_path, "%s.kc-dl-%ld.tmp", dl_path, kc_getpid());
            if (!(r->body.fp = fopen(tmp_path, "wb"))) { snprintf(err, HERR, "cannot create '%.150s': %s", tmp_path, strerror(errno)); tmp_path[0] = 0; goto finish; }
        }
        if (!conn_read_body(k, m, clen, &r->body, dl, err)) goto finish;
        if (r->body.fp) {
            bool good = fclose(r->body.fp) == 0; r->body.fp = NULL;
            if (!good || !replace_file(tmp_path, dl_path)) { snprintf(err, HERR, "cannot write '%.180s': %s", dl_path, strerror(errno)); goto finish; }
            tmp_path[0] = 0; r->saved = true;
        }
        bool ka = c->keepalive && m != BODY_CLOSE && head.status != 101 && k->pos == k->len;
        const char *cn = hh_get(&head.hdrs, "Connection");
        if (cn && has_token(cn, "close")) ka = false;
        if (!strcmp(head.version, "HTTP/1.0") && !(cn && has_token(cn, "keep-alive"))) ka = false;
        if (ka) { pool_put(c, k); } else conn_free(k);
        k = NULL;
        r->tls = https;
        r->status = head.status; snprintf(r->reason, sizeof r->reason, "%s", head.reason); snprintf(r->version, sizeof r->version, "%s", head.version);
        r->hdrs = head.hdrs; memset(&head, 0, sizeof head);
        ok = true;
    }
    if (ok && c->cookies_on) for (int i = 0; i < r->hdrs.n; i++) if (ht_name_eq(r->hdrs.v[i].name, "Set-Cookie")) cookie_store(c, u, r->hdrs.v[i].value);
    if (ok && c->decompress && !r->saved && r->body.mem.len > 0) {
        const char *ce = hh_get(&r->hdrs, "Content-Encoding");
        InfMode im = INF_AUTO; bool dec = false;
        if (ce && (ht_name_eq(ce, "gzip") || ht_name_eq(ce, "x-gzip"))) { im = INF_GZIP; dec = true; }
        else if (ce && ht_name_eq(ce, "deflate")) { im = INF_AUTO; dec = true; }
        if (dec) {
            SysBuf out = {0}; char e2[HERR];
            if (!kc_inflate((const uint8_t *)r->body.mem.p, r->body.mem.len, im, &out, c->max_body, e2)) { sysbuf_free(&out); snprintf(err, HERR, "could not decode the %.30s response body: %.150s", ce, e2); ok = false; }
            else {
                sysbuf_free(&r->body.mem); r->body.mem = out; r->body.total = out.len; r->decoded = true;
                char nb[32]; snprintf(nb, sizeof nb, "%zu", out.len);
                hh_remove(&r->hdrs, "Content-Encoding"); hh_set(&r->hdrs, "Content-Length", nb);
            }
        }
    }
finish:
    if (k) conn_free(k);
    if (tmp_path[0]) { if (r->body.fp) { fclose(r->body.fp); r->body.fp = NULL; } remove(tmp_path); }
    hh_free(&head.hdrs); sysbuf_free(&req);
    return ok;
}

/* ----------------------------- redirects / perform ----------------------------- */

static bool same_origin(const Url *a, const Url *b) { return !strcmp(a->scheme, b->scheme) && !strcmp(a->host, b->host) && a->port == b->port; }

static void hh_copy(HHdrs *dst, const HHdrs *src) { for (int i = 0; i < src->n; i++) hh_add(dst, src->v[i].name, strlen(src->v[i].name), src->v[i].value, strlen(src->v[i].value)); }

static bool perform(HClient *c, const char *method_in, const char *url_in, const char *body, size_t blen, const HHdrs *user,
                    const char *dl_path, double timeout, HResp *out, char *err) {
    double t0 = kc_monotonic_seconds();
    if (timeout < 0) timeout = c->timeout;
    double dl = timeout > 0 ? t0 + timeout : HT_NO_DEADLINE;
    Url cur;
    if (!url_parse(url_in, &cur, err)) return false;
    char method[16]; snprintf(method, sizeof method, "%s", method_in);
    HHdrs hdrs = {0}; hh_copy(&hdrs, user);
    bool send_creds = true, ok = false; int hop = 0;
    for (;; hop++) {
        memset(out, 0, sizeof *out);
        if (!exchange(c, method, &cur, body, blen, &hdrs, send_creds, dl_path, dl, out, err)) { resp_free(out); break; }
        int st = out->status;
        const char *loc = hh_get(&out->hdrs, "Location");
        if (c->follow && loc && (st == 301 || st == 302 || st == 303 || st == 307 || st == 308)) {
            if (hop >= c->max_redirects) { snprintf(err, HERR, "too many redirects (more than %d) while fetching %.120s", c->max_redirects, url_in); resp_free(out); break; }
            Url next;
            if (!url_resolve(&cur, loc, &next, err)) { resp_free(out); break; }
            if (!strcmp(cur.scheme, "https") && !strcmp(next.scheme, "http")) {
                snprintf(err, HERR, "refusing to follow a redirect from https to plain http (%.150s)", loc); url_free(&next); resp_free(out); break;
            }
            if ((st == 303 && strcmp(method, "HEAD")) || ((st == 301 || st == 302) && !strcmp(method, "POST"))) {
                snprintf(method, sizeof method, "GET"); body = NULL; blen = 0; hh_remove(&hdrs, "Content-Type");
            }
            if (!same_origin(&cur, &next)) send_creds = false;
            resp_free(out); url_free(&cur); cur = next;
            continue;
        }
        out->final_url = url_format(&cur, false); out->redirects = hop; out->elapsed = kc_monotonic_seconds() - t0;
        ok = true; break;
    }
    url_free(&cur); hh_free(&hdrs);
    return ok;
}

/* ------------------------------ argument handling ------------------------------ */

static HClient *get_client(KcValue v, KcPos pos, const char *who) {
    if (v.type != VAL_HANDLE || v.as.handle->kind != H_HTTP || v.as.handle->extra != HT_CLIENT) kc_error_at(pos, "%s: expected an http client handle from http.client()", who);
    if (!v.as.handle->ptr) kc_error_at(pos, "%s: this http client was already closed", who);
    return (HClient *)v.as.handle->ptr;
}

static HClient *peel_client(KcValue **a, int *n, KcPos pos, const char *who) {
    if (*n >= 1 && (*a)[0].type == VAL_HANDLE && (*a)[0].as.handle->kind == H_HTTP) {
        HClient *c = get_client((*a)[0], pos, who); (*a)++; (*n)--; return c;
    }
    return default_client();
}

static void collect_headers(KcValue v, HHdrs *out, KcPos pos, const char *who) {
    if (v.type == VAL_NULL) return;
    if (v.type != VAL_ARRAY) kc_error_at(pos, "%s: headers must be an array of \"Name: value\" strings", who);
    for (int i = 0; i < v.as.arr->count; i++) {
        KcValue e = v.as.arr->items[i];
        if (e.type != VAL_STRING) kc_error_at(pos, "%s: header %d is not a string", who, i);
        const char *s = e.as.s->data; const char *colon = strchr(s, ':');
        if (!colon || !ht_token_ok(s, (size_t)(colon - s))) kc_error_at(pos, "%s: header %d must look like \"Name: value\" (got '%.40s')", who, i, s);
        const char *val = colon + 1; while (*val == ' ' || *val == '\t') val++;
        if (strpbrk(val, "\r\n") || (int)strlen(s) != e.as.s->len) kc_error_at(pos, "%s: header %d contains a newline or NUL (header injection refused)", who, i);
        size_t vl = strlen(val); while (vl && (val[vl - 1] == ' ' || val[vl - 1] == '\t')) vl--;
        char *vv = kc_malloc(vl + 1); memcpy(vv, val, vl); vv[vl] = 0;
        char *nn = kc_malloc((size_t)(colon - s) + 1); memcpy(nn, s, (size_t)(colon - s)); nn[colon - s] = 0;
        hh_set(out, nn, vv); free(nn); free(vv);
    }
}

static KcValue headers_array(const HHdrs *h) {
    KcValue arr = kc_array_new();
    for (int i = 0; i < h->n; i++) {
        size_t l = strlen(h->v[i].name) + strlen(h->v[i].value) + 3; char *s = kc_malloc(l);
        snprintf(s, l, "%s: %s", h->v[i].name, h->v[i].value);
        kc_array_push(arr, kc_string_take(s, (int)strlen(s)));
    }
    return arr;
}

static KcValue resp_value(HResp *r) {
    static const char *names[] = { "status", "reason", "ok", "version", "url", "redirects", "headers", "body", "length", "elapsed", "tls", "decoded" };
    KcValue body;
    if (r->body.mem.p) { body = sys_bytes_str(r->body.mem.p, r->body.mem.len); r->body.mem.p = NULL; r->body.mem.len = r->body.mem.cap = 0; }
    else body = kc_string("");
    KcValue v[12] = { kc_int(r->status), kc_string(r->reason), kc_bool(r->status >= 200 && r->status < 300), kc_string(r->version),
                      kc_string(r->final_url ? r->final_url : ""), kc_int(r->redirects), headers_array(&r->hdrs), body,
                      kc_int((int64_t)r->body.total), kc_float(r->elapsed), kc_bool(r->tls), kc_bool(r->decoded) };
    KcValue out = sys_struct("HttpResponse", 12, names, v);
    resp_free(r);
    return out;
}

typedef enum { L_REQUEST, L_BODY, L_NOBODY, L_DOWNLOAD } Layout;

static KcValue do_request(KcInterp *ip, KcValue *a, int n, KcPos pos, const char *who, const char *fixed, Layout lay) {
    SYS_IMPURE(ip, pos, who);
    HClient *c = peel_client(&a, &n, pos, who);
    const char *sigs[] = {
        "http.request([client,] method: string, url: string, body: string = null, headers: array = null, timeout: float = null) -> HttpResponse",
        "(url: string, body: string, headers: array = null, timeout: float = null) -> HttpResponse",
        "(url: string, headers: array = null, timeout: float = null) -> HttpResponse",
        "http.download([client,] url: string, path: string, headers: array = null, timeout: float = null) -> HttpResponse",
    };
    char sig[300];
    if (lay == L_REQUEST || lay == L_DOWNLOAD) snprintf(sig, sizeof sig, "%s", sigs[lay == L_DOWNLOAD ? 3 : 0]); else snprintf(sig, sizeof sig, "%s%s", who, sigs[lay]);
    int i = 0, lo, hi;
    switch (lay) { case L_REQUEST: lo = 2; hi = 5; break; case L_BODY: lo = 2; hi = 4; break; case L_NOBODY: lo = 1; hi = 3; break; default: lo = 2; hi = 4; }
    sys_argc(n, lo, hi, sig, pos);
    char method[16];
    if (lay == L_REQUEST) {
        const char *m = sys_str(a, i++, sig, pos);
        if (!ht_token_ok(m, strlen(m)) || strlen(m) > 15) kc_error_at(pos, "%s: invalid HTTP method '%.20s'", who, m);
        size_t ml = strlen(m);
        for (size_t k = 0; k < ml; k++) method[k] = (char)toupper((unsigned char)m[k]);
        method[ml] = 0;
    } else if (lay == L_DOWNLOAD) snprintf(method, sizeof method, "GET");
    else snprintf(method, sizeof method, "%s", fixed);
    const char *url = sys_str(a, i++, sig, pos);
    const char *body = NULL; size_t blen = 0; const char *dl_path = NULL;
    if (lay == L_DOWNLOAD) { dl_path = sys_str(a, i++, sig, pos); if (!*dl_path) kc_error_at(pos, "http.download: path is empty"); }
    else if (lay == L_BODY || lay == L_REQUEST) {
        if (i < n && a[i].type != VAL_NULL) { sys_str(a, i, sig, pos); body = a[i].as.s->data; blen = (size_t)a[i].as.s->len; }
        else if (lay == L_BODY && i < n) sys_usage(pos, sig);
        i++;
    }
    HHdrs user = {0};
    if (i < n) collect_headers(a[i], &user, pos, who);
    i++;
    double timeout = -1;
    if (i < n && a[i].type != VAL_NULL) { timeout = sys_num(a, i, sig, pos); if (timeout < 0) kc_error_at(pos, "%s: timeout must be >= 0 (0 = no timeout)", who); }
    HResp r; char err[HERR] = "";
    memset(&r, 0, sizeof r);
    bool ok = perform(c, method, url, body, blen, &user, dl_path, timeout, &r, err);
    hh_free(&user);
    if (!ok) { ht_set_error("%s: %s", who, err); return kc_null(); }
    return resp_value(&r);
}

#define DEF(fn, who, fixed, lay) static KcValue fn(KcInterp *ip, KcValue *a, int n, KcPos pos) { return do_request(ip, a, n, pos, who, fixed, lay); }
DEF(hc_request, "http.request", NULL, L_REQUEST)
DEF(hc_get, "http.get", "GET", L_NOBODY)
DEF(hc_head, "http.head", "HEAD", L_NOBODY)
DEF(hc_options, "http.options", "OPTIONS", L_NOBODY)
DEF(hc_delete, "http.delete", "DELETE", L_NOBODY)
DEF(hc_post, "http.post", "POST", L_BODY)
DEF(hc_put, "http.put", "PUT", L_BODY)
DEF(hc_patch, "http.patch", "PATCH", L_BODY)
DEF(hc_download, "http.download", NULL, L_DOWNLOAD)

/* --------------------------------- client handle --------------------------------- */

static KcValue hc_client(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)a;
    SYS_IMPURE(ip, pos, "http.client");
    sys_argc(n, 0, 0, "http.client() -> client  (keep-alive on; configure with http.set_option)", pos);
    return ht_make_handle(HT_CLIENT, client_new(true));
}

static KcValue hc_set_option(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    const char *sig = "http.set_option(client, key: string, value)  keys: timeout connect_timeout max_redirects follow_redirects verify ca_file user_agent cookies keepalive decompress max_body header remove_header basic_auth bearer";
    sys_argc(n, 3, 3, sig, pos);
    HClient *c = get_client(a[0], pos, "http.set_option");
    const char *k = sys_str(a, 1, sig, pos);
    KcValue v = a[2];
    if (!strcmp(k, "timeout") || !strcmp(k, "connect_timeout")) {
        double d = sys_num(a, 2, sig, pos); if (d < 0) kc_error_at(pos, "http.set_option: %s must be >= 0 (0 = no timeout)", k);
        if (k[0] == 't') c->timeout = d; else c->connect_timeout = d;
    } else if (!strcmp(k, "max_redirects")) {
        int64_t x = sys_int(a, 2, sig, pos); if (x < 0 || x > 100) kc_error_at(pos, "http.set_option: max_redirects must be 0..100"); c->max_redirects = (int)x;
    } else if (!strcmp(k, "max_body")) {
        int64_t x = sys_int(a, 2, sig, pos); if (x < 1) kc_error_at(pos, "http.set_option: max_body must be >= 1"); c->max_body = (size_t)x;
    } else if (!strcmp(k, "follow_redirects") || !strcmp(k, "verify") || !strcmp(k, "cookies") || !strcmp(k, "keepalive") || !strcmp(k, "decompress")) {
        if (v.type != VAL_BOOL) kc_error_at(pos, "http.set_option: %s needs true or false", k);
        bool b = v.as.b;
        if (k[0] == 'f') c->follow = b;
        else if (k[0] == 'v') { c->verify = b; tls_ctx_free(c->tls); c->tls = NULL; client_close_idle(c); }
        else if (k[0] == 'c') c->cookies_on = b;
        else if (k[0] == 'k') { c->keepalive = b; if (!b) client_close_idle(c); }
        else c->decompress = b;
    } else if (!strcmp(k, "ca_file")) {
        const char *s = sys_str(a, 2, sig, pos); free(c->ca_file); c->ca_file = *s ? kc_strdup(s) : NULL; tls_ctx_free(c->tls); c->tls = NULL; client_close_idle(c);
    } else if (!strcmp(k, "user_agent")) {
        const char *s = sys_str(a, 2, sig, pos); if (strpbrk(s, "\r\n")) kc_error_at(pos, "http.set_option: user_agent contains a newline");
        free(c->user_agent); c->user_agent = kc_strdup(s);
    } else if (!strcmp(k, "header")) {
        KcValue arr = kc_array_new(); kc_array_push(arr, v); collect_headers(arr, &c->defaults, pos, "http.set_option");
    } else if (!strcmp(k, "remove_header")) {
        hh_remove(&c->defaults, sys_str(a, 2, sig, pos));
    } else if (!strcmp(k, "basic_auth")) {
        const char *s = sys_str(a, 2, sig, pos); free(c->auth); c->auth = NULL;
        if (*s) { size_t el; char *b = ht_b64_encode((const uint8_t *)s, strlen(s), &el); c->auth = kc_malloc(el + 8); snprintf(c->auth, el + 8, "Basic %s", b); free(b); }
    } else if (!strcmp(k, "bearer")) {
        const char *s = sys_str(a, 2, sig, pos); free(c->auth); c->auth = NULL;
        if (strpbrk(s, "\r\n")) kc_error_at(pos, "http.set_option: bearer token contains a newline");
        if (*s) { c->auth = kc_malloc(strlen(s) + 8); snprintf(c->auth, strlen(s) + 8, "Bearer %s", s); }
    } else kc_error_at(pos, "http.set_option: unknown option '%s'", k);
    return kc_null();
}

static KcValue hc_cookies(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    sys_argc(n, 1, 1, "http.cookies(client) -> array of \"name=value; domain=...; path=...\"", pos);
    HClient *c = get_client(a[0], pos, "http.cookies");
    KcValue out = kc_array_new();
    for (int i = 0; i < c->njar; i++) {
        Cookie *k = &c->jar[i]; char s[1200];
        snprintf(s, sizeof s, "%s=%s; domain=%s; path=%s%s%s", k->name, k->value, k->domain, k->path, k->secure ? "; secure" : "", k->expires > 0 ? "; expires" : "");
        kc_array_push(out, kc_string(s));
    }
    return out;
}

static KcValue hc_clear_cookies(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    sys_argc(n, 1, 1, "http.clear_cookies(client)", pos);
    HClient *c = get_client(a[0], pos, "http.clear_cookies");
    for (int i = 0; i < c->njar; i++) cookie_free(&c->jar[i]);
    c->njar = 0;
    return kc_null();
}

/* ---------------------------------- response helpers ---------------------------------- */

static KcValue *struct_field(KcValue st, const char *name) {
    if (st.type != VAL_STRUCT) return NULL;
    for (int i = 0; i < st.as.strct->count; i++) if (!strcmp(st.as.strct->names[i], name)) return &st.as.strct->values[i];
    return NULL;
}

static KcValue hc_header(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    const char *sig = "http.header(response, name: string) -> string  (first value, case-insensitive; null if absent)";
    sys_argc(n, 2, 2, sig, pos);
    KcValue *h = struct_field(a[0], "headers");
    if (!h || h->type != VAL_ARRAY) kc_error_at(pos, "http.header: first argument must be an HttpResponse");
    const char *name = sys_str(a, 1, sig, pos); size_t nl = strlen(name);
    for (int i = 0; i < h->as.arr->count; i++) {
        KcValue e = h->as.arr->items[i]; if (e.type != VAL_STRING) continue;
        const char *s = e.as.s->data; const char *colon = strchr(s, ':');
        if (colon && (size_t)(colon - s) == nl && strncasecmp(s, name, nl) == 0) { colon++; while (*colon == ' ') colon++; return kc_string(colon); }
    }
    return kc_null();
}

static KcValue hc_header_all(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    const char *sig = "http.header_all(response, name: string) -> array of string  (every value, e.g. Set-Cookie)";
    sys_argc(n, 2, 2, sig, pos);
    KcValue *h = struct_field(a[0], "headers");
    if (!h || h->type != VAL_ARRAY) kc_error_at(pos, "http.header_all: first argument must be an HttpResponse");
    const char *name = sys_str(a, 1, sig, pos); size_t nl = strlen(name);
    KcValue out = kc_array_new();
    for (int i = 0; i < h->as.arr->count; i++) {
        KcValue e = h->as.arr->items[i]; if (e.type != VAL_STRING) continue;
        const char *s = e.as.s->data; const char *colon = strchr(s, ':');
        if (colon && (size_t)(colon - s) == nl && strncasecmp(s, name, nl) == 0) { colon++; while (*colon == ' ') colon++; kc_array_push(out, kc_string(colon)); }
    }
    return out;
}

/* close() works for clients and servers, defined with the server code. */
void ht_close_server(void *p);
static KcValue hc_close(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    sys_argc(n, 1, 1, "http.close(client | server)  (frees sockets; safe to call twice)", pos);
    if (a[0].type != VAL_HANDLE || a[0].as.handle->kind != H_HTTP) kc_error_at(pos, "http.close: expected an http client or server handle");
    KcHandle *h = a[0].as.handle;
    if (!h->ptr) return kc_null();
    if (h->extra == HT_CLIENT) client_free((HClient *)h->ptr);
    else if (h->extra == HT_SERVER) ht_close_server(h->ptr);
    else kc_error_at(pos, "http.close: a request's connection is closed by http.respond()");
    h->ptr = NULL;
    return kc_null();
}

static KcValue hc_last_error(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; (void)a;
    sys_argc(n, 0, 0, "http.last_error() -> string", pos);
    return kc_string(ht_get_error());
}

static KcValue hc_clear_error(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; (void)a;
    sys_argc(n, 0, 0, "http.clear_error()", pos);
    ht_set_error("");
    return kc_null();
}

const HtEntry ht_client_entries[] = {
    {"request", hc_request}, {"get", hc_get}, {"head", hc_head}, {"options", hc_options}, {"delete", hc_delete},
    {"post", hc_post}, {"put", hc_put}, {"patch", hc_patch}, {"download", hc_download},
    {"client", hc_client}, {"set_option", hc_set_option}, {"cookies", hc_cookies}, {"clear_cookies", hc_clear_cookies},
    {"header", hc_header}, {"header_all", hc_header_all}, {"close", hc_close},
    {"last_error", hc_last_error}, {"clear_error", hc_clear_error},
};
const int ht_client_count = (int)(sizeof ht_client_entries / sizeof ht_client_entries[0]);
