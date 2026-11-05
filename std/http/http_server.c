/* KingCobra std.http — a small blocking HTTP/1.1 server.
 *
 *   let s = http.listen(8080);                     // "127.0.0.1" by default; pass "0.0.0.0" to expose it
 *   while (true) {
 *       let req = http.accept(s, 1.0);              // null if nothing arrived within 1 s
 *       if (req != null) { http.respond(req, 200, "hello " + req.path); }
 *   }
 *
 * Deliberately simple and safe rather than fast: one request at a time, one
 * request per connection (every response carries Connection: close), request
 * line/headers/body size limits (128 KiB headers, 16 MiB body), a 10-second
 * deadline for a client to deliver its request (a stalled client cannot wedge
 * the loop for longer), chunked request bodies, Expect: 100-continue, and
 * automatic 400/431/505 answers for malformed requests (these never reach the
 * script). TLS is not supported on the server side — put a TLS terminator in
 * front for HTTPS.
 */
#include "http_glue.h"

typedef struct { kc_sock_t fd; int port; } HServer;

#define REQUEST_DEADLINE 10.0
#define MAX_REQ_BODY ((size_t)16 << 20)

void ht_close_server(void *p) { HServer *s = p; if (s->fd != KC_SOCK_INVALID) kc_sock_close(s->fd); free(s); }

static HServer *get_server(KcValue v, KcPos pos, const char *who) {
    if (v.type != VAL_HANDLE || v.as.handle->kind != H_HTTP || v.as.handle->extra != HT_SERVER) kc_error_at(pos, "%s: expected a server handle from http.listen()", who);
    if (!v.as.handle->ptr) kc_error_at(pos, "%s: this server was already closed", who);
    return (HServer *)v.as.handle->ptr;
}

static KcValue hs_listen(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    SYS_IMPURE(ip, pos, "http.listen");
    const char *sig = "http.listen(port: int, host: string = \"127.0.0.1\", backlog: int = 64) -> server  (port 0 picks a free port)";
    sys_argc(n, 1, 3, sig, pos);
    int64_t port = sys_int(a, 0, sig, pos);
    if (port < 0 || port > 65535) kc_error_at(pos, "http.listen: port must be 0..65535");
    const char *host = n > 1 && a[1].type != VAL_NULL ? sys_str(a, 1, sig, pos) : "127.0.0.1";
    int64_t backlog = n > 2 ? sys_int(a, 2, sig, pos) : 64;
    if (backlog < 1 || backlog > 4096) kc_error_at(pos, "http.listen: backlog must be 1..4096");
    kc_sockets_init();
#if !defined(KC_PLATFORM_WINDOWS)
    signal(SIGPIPE, SIG_IGN);
#endif
    struct addrinfo hints, *res = NULL; memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM; hints.ai_flags = AI_PASSIVE;
    char ps[16]; snprintf(ps, sizeof ps, "%d", (int)port);
    if (getaddrinfo(host, ps, &hints, &res) != 0 || !res) { ht_set_error("http.listen: cannot resolve '%.100s'", host); return kc_null(); }
    kc_sock_t fd = KC_SOCK_INVALID; char err[HERR] = "";
    for (struct addrinfo *rp = res; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd == KC_SOCK_INVALID) continue;
        int one = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
#if defined(IPV6_V6ONLY)
        if (rp->ai_family == AF_INET6) { int off = 0; setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&off, sizeof off); }
#endif
        if (bind(fd, rp->ai_addr, (int)rp->ai_addrlen) == 0 && listen(fd, (int)backlog) == 0) break;
        ht_sockerr(err, "cannot listen");
        kc_sock_close(fd); fd = KC_SOCK_INVALID;
    }
    freeaddrinfo(res);
    if (fd == KC_SOCK_INVALID) { ht_set_error("http.listen: %s on %s:%d", err[0] ? err : "cannot bind", host, (int)port); return kc_null(); }
    ht_nonblock(fd);
    struct sockaddr_storage ss; socklen_t sl = sizeof ss; int actual = (int)port;
    if (getsockname(fd, (struct sockaddr *)&ss, &sl) == 0) actual = ntohs(ss.ss_family == AF_INET6 ? ((struct sockaddr_in6 *)&ss)->sin6_port : ((struct sockaddr_in *)&ss)->sin_port);
    HServer *s = kc_malloc(sizeof *s); s->fd = fd; s->port = actual;
    return ht_make_handle(HT_SERVER, s);
}

static KcValue hs_server_port(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    sys_argc(n, 1, 1, "http.server_port(server) -> int", pos);
    return kc_int(get_server(a[0], pos, "http.server_port")->port);
}

static void reply_simple(Conn *k, int status, const char *msg) {
    char buf[512], err[HERR];
    int l = snprintf(buf, sizeof buf, "HTTP/1.1 %d %s\r\nConnection: close\r\nContent-Type: text/plain\r\nContent-Length: %zu\r\n\r\n%s", status, ht_reason(status), strlen(msg), msg);
    conn_write(k, buf, (size_t)l, kc_monotonic_seconds() + 2.0, err);
}

/* 1 = request parsed into the out params, 0 = client went away / garbage already answered. */
static int read_request(Conn *k, char **method, char **target, char **version, HHdrs *h, Sink *body) {
    char err[HERR] = ""; double dl = kc_monotonic_seconds() + REQUEST_DEADLINE;
    SysBuf line = {0};
    int g = conn_getline(k, &line, 8192, dl, err);
    if (g <= 0) { if (g < 0 && strstr(err, "too long")) reply_simple(k, 414, "request line too long"); sysbuf_free(&line); return 0; }
    char *sp1 = strchr(line.p, ' '), *sp2 = sp1 ? strchr(sp1 + 1, ' ') : NULL;
    if (!sp1 || !sp2 || sp2 == sp1 + 1 || !ht_token_ok(line.p, (size_t)(sp1 - line.p))) { reply_simple(k, 400, "malformed request line"); sysbuf_free(&line); return 0; }
    if (strncmp(sp2 + 1, "HTTP/1.", 7) != 0 || (sp2[8] != '0' && sp2[8] != '1') || sp2[9]) { reply_simple(k, 505, "only HTTP/1.0 and HTTP/1.1 are supported"); sysbuf_free(&line); return 0; }
    *method = kc_malloc((size_t)(sp1 - line.p) + 1); memcpy(*method, line.p, (size_t)(sp1 - line.p)); (*method)[sp1 - line.p] = 0;
    *target = kc_malloc((size_t)(sp2 - sp1)); memcpy(*target, sp1 + 1, (size_t)(sp2 - sp1 - 1)); (*target)[sp2 - sp1 - 1] = 0;
    *version = kc_strdup(sp2 + 1);
    sysbuf_free(&line);
    if (!conn_read_headers(k, h, dl, err)) { reply_simple(k, strstr(err, "larger") || strstr(err, "more than") || strstr(err, "too long") ? 431 : 400, err); return 0; }
    BodyMode m; size_t clen;
    if (!ht_body_framing(h, &m, &clen, err)) { reply_simple(k, 400, err); return 0; }
    if (m == BODY_CLOSE) m = BODY_NONE;                                     /* a request with no framing has no body */
    if (m != BODY_NONE) {
        const char *ex = hh_get(h, "Expect");
        if (ex && ht_name_eq(ex, "100-continue")) conn_write(k, "HTTP/1.1 100 Continue\r\n\r\n", 25, dl, err);
        body->max = MAX_REQ_BODY;
        if (m == BODY_LENGTH && clen > MAX_REQ_BODY) { reply_simple(k, 413, "request body too large"); return 0; }
        if (!conn_read_body(k, m, clen, body, dl, err)) { reply_simple(k, strstr(err, "larger") ? 413 : 400, err); return 0; }
    }
    return 1;
}

static KcValue hs_accept(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    SYS_IMPURE(ip, pos, "http.accept");
    const char *sig = "http.accept(server, timeout: float = null) -> HttpRequest  (null if the timeout passes first)";
    sys_argc(n, 1, 2, sig, pos);
    HServer *s = get_server(a[0], pos, "http.accept");
    double timeout = sys_opt_num(a, n, 1, -1, sig, pos);
    double dl = timeout >= 0 ? kc_monotonic_seconds() + timeout : HT_NO_DEADLINE;
    for (;;) {
        int w = ht_wait(s->fd, false, dl);
        if (w == 0) return kc_null();
        if (w < 0) { char e[HERR]; ht_sockerr(e, "accept"); ht_set_error("http.accept: %s", e); return kc_null(); }
        struct sockaddr_storage ss; socklen_t sl = sizeof ss;
        kc_sock_t cfd = accept(s->fd, (struct sockaddr *)&ss, &sl);
        if (cfd == KC_SOCK_INVALID) { if (ht_wouldblock()) continue; char e[HERR]; ht_sockerr(e, "accept"); ht_set_error("http.accept: %s", e); return kc_null(); }
        ht_nonblock(cfd);
        int one = 1; setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
        char host[64] = "?", serv[16] = "0";
        getnameinfo((struct sockaddr *)&ss, sl, host, sizeof host, serv, sizeof serv, NI_NUMERICHOST | NI_NUMERICSERV);
        Conn *k = conn_new(cfd, NULL);
        char *method = NULL, *target = NULL, *version = NULL; HHdrs h = {0}; Sink body; memset(&body, 0, sizeof body); body.max = MAX_REQ_BODY;
        if (!read_request(k, &method, &target, &version, &h, &body)) {
            free(method); free(target); free(version); hh_free(&h); sysbuf_free(&body.mem); conn_free(k);
            continue;
        }
        const char *q = strchr(target, '?');
        size_t pl = q ? (size_t)(q - target) : strlen(target);
        char *dpath = kc_malloc(pl + 1); size_t dl2 = ht_pct_decode(target, pl, dpath, false);
        char remote[96]; snprintf(remote, sizeof remote, strchr(host, ':') ? "[%s]:%s" : "%s:%s", host, serv);
        KcValue hv = kc_array_new();
        for (int i = 0; i < h.n; i++) { size_t l = strlen(h.v[i].name) + strlen(h.v[i].value) + 3; char *t = kc_malloc(l); snprintf(t, l, "%s: %s", h.v[i].name, h.v[i].value); kc_array_push(hv, kc_string_take(t, (int)strlen(t))); }
        KcValue bv; if (body.mem.p) { bv = sys_bytes_str(body.mem.p, body.mem.len); } else bv = kc_string("");
        static const char *names[] = { "method", "target", "path", "query", "version", "headers", "body", "remote", "conn" };
        KcValue v[9] = { kc_string(method), kc_string(target), sys_bytes_str(dpath, dl2), kc_string(q ? q + 1 : ""), kc_string(version), hv, bv, kc_string(remote), ht_make_handle(HT_SCONN, k) };
        free(method); free(target); free(version); hh_free(&h);
        return sys_struct("HttpRequest", 9, names, v);
    }
}

static KcValue hs_respond(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    SYS_IMPURE(ip, pos, "http.respond");
    const char *sig = "http.respond(request, status: int, body: string = \"\", headers: array = null) -> bool";
    sys_argc(n, 2, 4, sig, pos);
    if (a[0].type != VAL_STRUCT) kc_error_at(pos, "http.respond: first argument must be the request from http.accept()");
    KcStructInst *st = a[0].as.strct; KcValue *conn = NULL; const char *method = "GET";
    for (int i = 0; i < st->count; i++) { if (!strcmp(st->names[i], "conn")) conn = &st->values[i]; else if (!strcmp(st->names[i], "method") && st->values[i].type == VAL_STRING) method = st->values[i].as.s->data; }
    if (!conn || conn->type != VAL_HANDLE || conn->as.handle->kind != H_HTTP || conn->as.handle->extra != HT_SCONN) kc_error_at(pos, "http.respond: first argument must be the request from http.accept()");
    if (!conn->as.handle->ptr) kc_error_at(pos, "http.respond: this request was already answered");
    int64_t status = sys_int(a, 1, sig, pos);
    if (status < 200 || status > 599) kc_error_at(pos, "http.respond: status must be 200..599");
    const char *body = ""; size_t blen = 0;
    if (n > 2 && a[2].type != VAL_NULL) { sys_str(a, 2, sig, pos); body = a[2].as.s->data; blen = (size_t)a[2].as.s->len; }
    HHdrs uh = {0};
    if (n > 3 && a[3].type != VAL_NULL) {
        if (a[3].type != VAL_ARRAY) sys_usage(pos, sig);
        for (int i = 0; i < a[3].as.arr->count; i++) {
            KcValue e = a[3].as.arr->items[i];
            if (e.type != VAL_STRING) kc_error_at(pos, "http.respond: header %d is not a string", i);
            const char *s = e.as.s->data, *colon = strchr(s, ':');
            if (!colon || !ht_token_ok(s, (size_t)(colon - s))) kc_error_at(pos, "http.respond: header %d must look like \"Name: value\"", i);
            const char *val = colon + 1; while (*val == ' ') val++;
            if (strpbrk(val, "\r\n")) kc_error_at(pos, "http.respond: header %d contains a newline (header injection refused)", i);
            char *nm = kc_malloc((size_t)(colon - s) + 1); memcpy(nm, s, (size_t)(colon - s)); nm[colon - s] = 0;
            hh_set(&uh, nm, val); free(nm);
        }
    }
    hh_remove(&uh, "Content-Length"); hh_remove(&uh, "Connection"); hh_remove(&uh, "Transfer-Encoding");
    bool head = !strcmp(method, "HEAD"), nobody = head || status == 204 || status == 304;
    char date[40]; ht_http_date(date, sizeof date, time(NULL));
    char line[256];
    SysBuf out = {0};
    int l = snprintf(line, sizeof line, "HTTP/1.1 %d %s\r\nDate: %s\r\nServer: KingCobra/%s\r\nConnection: close\r\n", (int)status, ht_reason((int)status), date, KC_VERSION);
    sysbuf_append(&out, line, (size_t)l);
    if (status != 204 && status != 304) { l = snprintf(line, sizeof line, "Content-Length: %zu\r\n", blen); sysbuf_append(&out, line, (size_t)l); }
    if (blen && !hh_get(&uh, "Content-Type")) sysbuf_append(&out, "Content-Type: text/plain; charset=utf-8\r\n", 41);
    for (int i = 0; i < uh.n; i++) { sysbuf_append(&out, uh.v[i].name, strlen(uh.v[i].name)); sysbuf_append(&out, ": ", 2); sysbuf_append(&out, uh.v[i].value, strlen(uh.v[i].value)); sysbuf_append(&out, "\r\n", 2); }
    sysbuf_append(&out, "\r\n", 2);
    if (!nobody && blen) sysbuf_append(&out, body, blen);
    hh_free(&uh);
    Conn *k = (Conn *)conn->as.handle->ptr; conn->as.handle->ptr = NULL;
    char err[HERR] = "";
    bool ok = conn_write(k, out.p, out.len, kc_monotonic_seconds() + 30.0, err);
    sysbuf_free(&out);
    if (ok) shutdown(k->fd, 1 /* SHUT_WR == SD_SEND == 1 */);
    conn_free(k);
    if (!ok) ht_set_error("http.respond: %s", err);
    return kc_bool(ok);
}

const HtEntry ht_server_entries[] = { {"listen", hs_listen}, {"accept", hs_accept}, {"respond", hs_respond}, {"server_port", hs_server_port} };
const int ht_server_count = 4;
