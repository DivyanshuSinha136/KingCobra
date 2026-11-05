/* KingCobra std.http — HTTPS through OpenSSL, loaded at run time.
 *
 * Nothing is linked at build time: the first https:// request dlopen()s
 * libssl (OpenSSL 1.1.x or 3.x; LoadLibrary on Windows) and resolves the
 * ~25 functions it needs. If the library is missing, https:// requests fail
 * cleanly with an explanatory http.last_error() and plain http:// is
 * unaffected — so the interpreter still builds and runs anywhere.
 *
 * Security defaults: the server certificate chain AND host name are verified
 * (SNI is sent, TLS 1.2 is the minimum protocol). Verification can only be
 * relaxed explicitly per client (set_option "verify" false) or pointed at a
 * custom CA bundle ("ca_file"). The socket stays non-blocking and every
 * handshake/read/write honours the request deadline.
 */
#include "http_glue.h"

struct TlsCtx { void *ctx; bool verify; };
struct TlsConn { void *ssl; };

#define SSL_ERROR_SSL 1
#define SSL_ERROR_WANT_READ 2
#define SSL_ERROR_WANT_WRITE 3
#define SSL_ERROR_SYSCALL 5
#define SSL_ERROR_ZERO_RETURN 6

static struct {
    bool tried, ok; char err[HERR]; kc_dl_handle ssl, crypto;
    const void *(*TLS_client_method)(void);
    void *(*SSL_CTX_new)(const void *);
    void (*SSL_CTX_free)(void *);
    int (*SSL_CTX_set_default_verify_paths)(void *);
    int (*SSL_CTX_load_verify_locations)(void *, const char *, const char *);
    void (*SSL_CTX_set_verify)(void *, int, void *);
    long (*SSL_CTX_ctrl)(void *, int, long, void *);
    void *(*SSL_new)(void *);
    void (*SSL_free)(void *);
    int (*SSL_set_fd)(void *, int);
    long (*SSL_ctrl)(void *, int, long, void *);
    int (*SSL_set1_host)(void *, const char *);
    int (*SSL_connect)(void *);
    int (*SSL_read)(void *, void *, int);
    int (*SSL_write)(void *, const void *, int);
    int (*SSL_shutdown)(void *);
    int (*SSL_get_error)(const void *, int);
    long (*SSL_get_verify_result)(const void *);
    int (*SSL_pending)(const void *);
    void *(*SSL_get0_param)(void *);
    unsigned long (*ERR_get_error)(void);
    void (*ERR_error_string_n)(unsigned long, char *, size_t);
    void (*ERR_clear_error)(void);
    const char *(*X509_verify_cert_error_string)(long);
    int (*X509_VERIFY_PARAM_set1_ip_asc)(void *, const char *);
} T;

static void *lookup(const char *name) {
    void *p = T.ssl ? kc_dlsym(T.ssl, name) : NULL;
    if (!p && T.crypto) p = kc_dlsym(T.crypto, name);
    return p;
}

#define NEED(fn) do { void *p_ = lookup(#fn); if (!p_) { snprintf(T.err, HERR, "the OpenSSL library found is too old or incompatible (missing %s); OpenSSL 1.1.1 or 3.x is required", #fn); return false; } memcpy(&T.fn, &p_, sizeof p_); } while (0)
#define OPT(fn) do { void *p_ = lookup(#fn); memcpy(&T.fn, &p_, sizeof p_); } while (0)

static bool load_openssl(void) {
    if (T.tried) return T.ok;
    T.tried = true;
#if defined(KC_PLATFORM_WINDOWS)
    static const char *ssl_names[] = { "libssl-3-x64.dll", "libssl-3.dll", "libssl-1_1-x64.dll", "libssl-1_1.dll", NULL };
    static const char *cry_names[] = { "libcrypto-3-x64.dll", "libcrypto-3.dll", "libcrypto-1_1-x64.dll", "libcrypto-1_1.dll", NULL };
#elif defined(__APPLE__)
    static const char *ssl_names[] = { "libssl.3.dylib", "/opt/homebrew/lib/libssl.3.dylib", "/usr/local/lib/libssl.3.dylib", "libssl.dylib", NULL };
    static const char *cry_names[] = { "libcrypto.3.dylib", "/opt/homebrew/lib/libcrypto.3.dylib", "/usr/local/lib/libcrypto.3.dylib", "libcrypto.dylib", NULL };
#else
    static const char *ssl_names[] = { "libssl.so.3", "libssl.so.1.1", "libssl.so", NULL };
    static const char *cry_names[] = { "libcrypto.so.3", "libcrypto.so.1.1", "libcrypto.so", NULL };
#endif
    for (int i = 0; ssl_names[i] && !T.ssl; i++) T.ssl = kc_dlopen(ssl_names[i]);
    for (int i = 0; cry_names[i] && !T.crypto; i++) T.crypto = kc_dlopen(cry_names[i]);
    if (!T.ssl) { snprintf(T.err, HERR, "HTTPS needs the OpenSSL library (libssl 1.1.1 or 3.x), which was not found on this system"); return false; }
    NEED(TLS_client_method); NEED(SSL_CTX_new); NEED(SSL_CTX_free); NEED(SSL_CTX_set_default_verify_paths);
    NEED(SSL_CTX_load_verify_locations); NEED(SSL_CTX_set_verify); NEED(SSL_CTX_ctrl); NEED(SSL_new); NEED(SSL_free);
    NEED(SSL_set_fd); NEED(SSL_ctrl); NEED(SSL_set1_host); NEED(SSL_connect); NEED(SSL_read); NEED(SSL_write);
    NEED(SSL_shutdown); NEED(SSL_get_error); NEED(SSL_get_verify_result); NEED(SSL_pending); NEED(SSL_get0_param);
    NEED(ERR_get_error); NEED(ERR_error_string_n); NEED(ERR_clear_error); NEED(X509_verify_cert_error_string);
    OPT(X509_VERIFY_PARAM_set1_ip_asc);
    T.ok = true;
    return true;
}

bool tls_available(char *err) {
    if (load_openssl()) return true;
    snprintf(err, HERR, "%s", T.err);
    return false;
}

static void ssl_error_text(char *err, const char *what, void *ssl, bool verify) {
    char one[160] = "";
    unsigned long e = T.ERR_get_error();
    if (e) T.ERR_error_string_n(e, one, sizeof one);
    long vr = ssl ? T.SSL_get_verify_result(ssl) : 0;
    if (verify && vr != 0) snprintf(err, HERR, "%s: certificate verification failed: %s", what, T.X509_verify_cert_error_string(vr));
    else if (one[0]) snprintf(err, HERR, "%s: %s", what, one);
    else snprintf(err, HERR, "%s failed", what);
}

TlsCtx *tls_ctx_new(bool verify, const char *ca_file, char *err) {
    if (!tls_available(err)) return NULL;
    T.ERR_clear_error();
    void *ctx = T.SSL_CTX_new(T.TLS_client_method());
    if (!ctx) { ssl_error_text(err, "TLS context", NULL, false); return NULL; }
    T.SSL_CTX_ctrl(ctx, 123 /* SSL_CTRL_SET_MIN_PROTO_VERSION */, 0x0303 /* TLS 1.2 */, NULL);
    if (verify) {
        int ok = (ca_file && *ca_file) ? T.SSL_CTX_load_verify_locations(ctx, ca_file, NULL) : T.SSL_CTX_set_default_verify_paths(ctx);
        if (ok != 1) {
            snprintf(err, HERR, (ca_file && *ca_file) ? "could not load CA bundle '%.150s'" : "could not load the system CA certificates", ca_file ? ca_file : "");
            T.SSL_CTX_free(ctx); return NULL;
        }
    }
    T.SSL_CTX_set_verify(ctx, verify ? 1 /* SSL_VERIFY_PEER */ : 0, NULL);
    TlsCtx *c = kc_malloc(sizeof *c); c->ctx = ctx; c->verify = verify;
    return c;
}

void tls_ctx_free(TlsCtx *c) { if (c) { if (T.ok) T.SSL_CTX_free(c->ctx); free(c); } }

TlsConn *tls_connect(TlsCtx *c, kc_sock_t fd, const char *host, bool verify, double deadline, char *err) {
    T.ERR_clear_error();
    void *ssl = T.SSL_new(c->ctx);
    if (!ssl) { ssl_error_text(err, "TLS session", NULL, false); return NULL; }
    T.SSL_set_fd(ssl, (int)fd);
    bool ip = ht_is_ip_literal(host);
    if (!ip) T.SSL_ctrl(ssl, 55 /* SSL_CTRL_SET_TLSEXT_HOSTNAME */, 0 /* host_name */, (void *)host);
    if (verify) {
        if (ip) {
            if (!T.X509_VERIFY_PARAM_set1_ip_asc || T.X509_VERIFY_PARAM_set1_ip_asc(T.SSL_get0_param(ssl), host) != 1) { snprintf(err, HERR, "cannot verify a certificate for the IP address %s with this OpenSSL", host); T.SSL_free(ssl); return NULL; }
        } else if (T.SSL_set1_host(ssl, host) != 1) { snprintf(err, HERR, "invalid host name for certificate verification"); T.SSL_free(ssl); return NULL; }
    }
    for (;;) {
        T.ERR_clear_error();
        int r = T.SSL_connect(ssl);
        if (r == 1) break;
        int e = T.SSL_get_error(ssl, r);
        if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
            int w = ht_wait(fd, e == SSL_ERROR_WANT_WRITE, deadline);
            if (w == 0) { snprintf(err, HERR, "TLS handshake timed out"); T.SSL_free(ssl); return NULL; }
            if (w < 0) { ht_sockerr(err, "TLS handshake"); T.SSL_free(ssl); return NULL; }
            continue;
        }
        if (e == SSL_ERROR_SYSCALL && r == 0) snprintf(err, HERR, "TLS handshake: the server closed the connection (is this an HTTPS port?)");
        else ssl_error_text(err, "TLS handshake", ssl, verify);
        T.SSL_free(ssl); return NULL;
    }
    TlsConn *t = kc_malloc(sizeof *t); t->ssl = ssl;
    return t;
}

long tls_read(TlsConn *t, kc_sock_t fd, void *buf, size_t n, double deadline, char *err) {
    int want = n > (size_t)INT_MAX ? INT_MAX : (int)n;
    for (;;) {
        T.ERR_clear_error();
        int r = T.SSL_read(t->ssl, buf, want);
        if (r > 0) return r;
        int e = T.SSL_get_error(t->ssl, r);
        if (e == SSL_ERROR_ZERO_RETURN) return 0;
        if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
            int w = ht_wait(fd, e == SSL_ERROR_WANT_WRITE, deadline);
            if (w == 0) { snprintf(err, HERR, "timed out waiting for the server"); return -1; }
            if (w < 0) { ht_sockerr(err, "TLS read"); return -1; }
            continue;
        }
        if (e == SSL_ERROR_SYSCALL && r == 0) return 0;      /* peer closed without close_notify: treat as EOF */
        ssl_error_text(err, "TLS read", t->ssl, false);
        return -1;
    }
}

bool tls_write(TlsConn *t, kc_sock_t fd, const char *buf, size_t n, double deadline, char *err) {
    while (n) {
        int want = n > (size_t)INT_MAX ? INT_MAX : (int)n;
        T.ERR_clear_error();
        int r = T.SSL_write(t->ssl, buf, want);
        if (r > 0) { buf += r; n -= (size_t)r; continue; }
        int e = T.SSL_get_error(t->ssl, r);
        if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
            int w = ht_wait(fd, e == SSL_ERROR_WANT_WRITE, deadline);
            if (w == 0) { snprintf(err, HERR, "timed out sending the request"); return false; }
            if (w < 0) { ht_sockerr(err, "TLS write"); return false; }
            continue;
        }
        ssl_error_text(err, "TLS write", t->ssl, false);
        return false;
    }
    return true;
}

void tls_close(TlsConn *t) {
    if (!t) return;
    T.SSL_shutdown(t->ssl);       /* best effort close_notify; never blocks (socket is non-blocking) */
    T.SSL_free(t->ssl);
    free(t);
}
