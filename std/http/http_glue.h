/* KingCobra std.http — shared glue for the http_*.c files.
 *
 * std.http is split like std.system / std.nn / std.rhex — one C file per concern:
 *
 *   http.c          module table, last_error(), small shared helpers
 *   http_url.c      URL parse/join/format, percent-encoding, query/form, base64
 *   http_inflate.c  gzip / zlib / raw-deflate decoder (RFC 1950/1951/1952)
 *   http_tls.c      HTTPS: OpenSSL loaded at run time with dlopen/LoadLibrary
 *   http_conn.c     socket connection with deadlines, HTTP/1.1 message parsing
 *                   (status/request line, headers, content-length, chunked)
 *   http_client.c   client handle, requests, redirects, cookies, keep-alive pool, download
 *   http_server.c   small blocking HTTP/1.1 server: listen / accept / respond
 *
 * Error model (same as std.system / std.mmio / std.rhex):
 *   - programming errors (wrong argument types, bad header text, a closed handle)
 *     are fatal with a positioned message;
 *   - environmental failures (DNS, refused, timeout, TLS verification, malformed
 *     response, body too large) return null and set http.last_error().
 *   - an HTTP error *status* (404, 500) is NOT a failure: it is a normal response
 *     with ok == false.
 *
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com>
 * License: MIT
 */
#ifndef KC_HTTP_GLUE_H
#define KC_HTTP_GLUE_H
#include "interp.h"
#include "kc_platform.h"
#include "../system/system_glue.h"   /* sys_argc/sys_str/... argument helpers, SysBuf, sys_struct */
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <time.h>
#if !defined(KC_PLATFORM_WINDOWS)
#include <poll.h>
#include <fcntl.h>
#include <signal.h>
#endif

#define HERR 256                       /* every `char *err` parameter points at HERR bytes */
#define HT_NO_DEADLINE 1e18

typedef struct { const char *name; KcNativeFn fn; } HtEntry;
extern const HtEntry ht_url_entries[];     extern const int ht_url_count;
extern const HtEntry ht_inflate_entries[]; extern const int ht_inflate_count;
extern const HtEntry ht_client_entries[];  extern const int ht_client_count;
extern const HtEntry ht_server_entries[];  extern const int ht_server_count;

/* handle subtypes carried in KcHandle.extra (kind is H_HTTP) */
enum { HT_CLIENT = 1, HT_SERVER = 2, HT_SCONN = 3 };

void        ht_set_error(const char *fmt, ...);
const char *ht_get_error(void);
KcValue     ht_make_handle(int subtype, void *ptr);
const char *ht_reason(int status);
void        ht_http_date(char *out, size_t cap, time_t t);   /* "Sun, 06 Nov 1994 08:49:37 GMT" */

/* ------------------------------ sockets ------------------------------ */

static inline void ht_nonblock(kc_sock_t fd) {
#if defined(KC_PLATFORM_WINDOWS)
    u_long m = 1; ioctlsocket(fd, FIONBIO, &m);
#else
    int fl = fcntl(fd, F_GETFL, 0); fcntl(fd, F_SETFL, fl | O_NONBLOCK);
#endif
}

static inline bool ht_wouldblock(void) {
#if defined(KC_PLATFORM_WINDOWS)
    int e = WSAGetLastError(); return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS || e == WSAEINTR;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS || errno == EINTR;
#endif
}

static inline void ht_sockerr(char *err, const char *what) {
#if defined(KC_PLATFORM_WINDOWS)
    snprintf(err, HERR, "%s: winsock error %d", what, WSAGetLastError());
#else
    snprintf(err, HERR, "%s: %s", what, strerror(errno));
#endif
}

/* Wait until `fd` is readable (or writable). 1 = ready, 0 = deadline passed, -1 = poll error. */
static inline int ht_wait(kc_sock_t fd, bool want_write, double deadline) {
    for (;;) {
        int ms = -1;
        if (deadline < 1e17) {
            double left = deadline - kc_monotonic_seconds();
            if (left <= 0) return 0;
            ms = (int)(left * 1000.0) + 1; if (ms > 1000000) ms = 1000000;
        }
#if defined(KC_PLATFORM_WINDOWS)
        WSAPOLLFD p; p.fd = fd; p.events = want_write ? POLLWRNORM : POLLRDNORM; p.revents = 0;
        int r = WSAPoll(&p, 1, ms);
        if (r > 0) return 1;
        if (r == 0) continue;
        return -1;
#else
        struct pollfd p; p.fd = fd; p.events = want_write ? POLLOUT : POLLIN; p.revents = 0;
        int r = poll(&p, 1, ms);
        if (r > 0) return 1;
        if (r == 0) continue;
        if (errno == EINTR) continue;
        return -1;
#endif
    }
}

static inline bool ht_is_ip_literal(const char *h) {
    if (strchr(h, ':')) return true;
    bool dot = false;
    for (const char *c = h; *c; c++) { if (*c == '.') dot = true; else if (!isdigit((unsigned char)*c)) return false; }
    return dot;
}

/* ------------------------------ headers ------------------------------ */

typedef struct { char *name, *value; } HHdr;
typedef struct { HHdr *v; int n, cap; } HHdrs;
void        hh_add(HHdrs *h, const char *name, size_t nl, const char *val, size_t vl);
void        hh_set(HHdrs *h, const char *name, const char *val);      /* replace (case-insensitive) or add */
void        hh_remove(HHdrs *h, const char *name);
const char *hh_get(const HHdrs *h, const char *name);                 /* first match or NULL */
void        hh_free(HHdrs *h);
bool        ht_token_ok(const char *s, size_t n);                     /* RFC 7230 token */
bool        ht_name_eq(const char *a, const char *b);                 /* case-insensitive */

/* ------------------------------- URLs ------------------------------- */

typedef struct { char scheme[8]; char *userinfo, *host, *path, *query; int port; } Url;
bool   url_parse(const char *s, Url *u, char *err);
void   url_free(Url *u);
bool   url_default_port(const Url *u);
bool   url_resolve(const Url *base, const char *ref, Url *out, char *err);
char  *url_target(const Url *u);                  /* "/path?query"  (malloc) */
char  *url_format(const Url *u, bool with_userinfo);
char  *ht_pct_encode(const char *s, size_t n, bool plus_for_space, size_t *outlen);
size_t ht_pct_decode(const char *s, size_t n, char *out, bool plus_to_space);   /* out has room for n+1 */
char  *ht_b64_encode(const uint8_t *d, size_t n, size_t *outlen);
bool   ht_b64_decode(const char *s, size_t n, SysBuf *out);

/* ------------------------------ inflate ------------------------------ */

typedef enum { INF_RAW, INF_ZLIB, INF_GZIP, INF_AUTO } InfMode;
bool kc_inflate(const uint8_t *in, size_t n, InfMode mode, SysBuf *out, size_t max_out, char *err);

/* -------------------------------- TLS -------------------------------- */

typedef struct TlsCtx TlsCtx;
typedef struct TlsConn TlsConn;
bool     tls_available(char *err);
TlsCtx  *tls_ctx_new(bool verify, const char *ca_file, char *err);
void     tls_ctx_free(TlsCtx *c);
TlsConn *tls_connect(TlsCtx *c, kc_sock_t fd, const char *host, bool verify, double deadline, char *err);
long     tls_read(TlsConn *t, kc_sock_t fd, void *buf, size_t n, double deadline, char *err);   /* >0 bytes, 0 EOF, -1 error */
bool     tls_write(TlsConn *t, kc_sock_t fd, const char *buf, size_t n, double deadline, char *err);
void     tls_close(TlsConn *t);

/* --------------------------- connection / IO --------------------------- */

typedef struct Conn {
    kc_sock_t fd; TlsConn *tls;
    char *buf; size_t pos, len, cap;     /* read-ahead buffer */
    size_t nread;                        /* total bytes ever received (stale-connection detection) */
    char *host; int port; bool https, reused;
    double last_used;
} Conn;

Conn *conn_new(kc_sock_t fd, TlsConn *tls);
void  conn_free(Conn *c);                /* closes the socket (and TLS) */
long  conn_read(Conn *c, void *dst, size_t n, double dl, char *err);       /* >0, 0 EOF, -1 error */
bool  conn_write(Conn *c, const char *d, size_t n, double dl, char *err);
int   conn_getline(Conn *c, SysBuf *line, size_t maxlen, double dl, char *err);   /* 1 line, 0 EOF before any byte, -1 error */

typedef struct { SysBuf mem; FILE *fp; size_t total, max; } Sink;
bool  sink_put(Sink *s, const char *d, size_t n, char *err);

typedef struct { int status; char version[16]; char reason[128]; HHdrs hdrs; } RespHead;
typedef enum { BODY_NONE, BODY_LENGTH, BODY_CHUNKED, BODY_CLOSE } BodyMode;

bool  conn_read_headers(Conn *c, HHdrs *h, double dl, char *err);
int   conn_read_response_head(Conn *c, RespHead *r, double dl, char *err);  /* 1 ok, 0 closed before any byte, -1 error */
bool  conn_read_body(Conn *c, BodyMode m, size_t clen, Sink *s, double dl, char *err);
/* Decide framing from headers. Returns false (err set) on conflicting/invalid framing. */
bool  ht_body_framing(const HHdrs *h, BodyMode *m, size_t *clen, char *err);

#endif
