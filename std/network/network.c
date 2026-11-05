/* KingCobra std.network — native TCP/UDP/raw sockets, portable across
 * Linux/macOS (BSD sockets) and Windows (Winsock) via
 * compiler/include/kc_platform.h.
 */
#include "interp.h"
#include "kc_platform.h"

static KcValue sock_handle(kc_sock_t fd) {
    KcHandle *h = kc_malloc(sizeof(KcHandle));
    h->kind = H_SOCKET; h->ptr = NULL; h->size = 0; h->extra = (intptr_t)fd;
    KcValue v; v.type = VAL_HANDLE; v.as.handle = h;
    return v;
}

static kc_sock_t expect_sock(KcValue v, KcPos pos, const char *who) {
    if (v.type != VAL_HANDLE || v.as.handle->kind != H_SOCKET)
        kc_error_at(pos, "%s: expected a socket handle", who);
    return (kc_sock_t)v.as.handle->extra;
}

static KcValue net_connect(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    kc_forbid_in_comptime(ip, pos, "network.tcp_connect");
    kc_sockets_init();
    if (n != 2 || a[0].type != VAL_STRING || a[1].type != VAL_INT)
        kc_error_at(pos, "network.tcp_connect(host: string, port: int) -> handle");
    struct addrinfo hints; memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res;
    char portbuf[16]; snprintf(portbuf, sizeof portbuf, "%lld", (long long)a[1].as.i);
    int rc = getaddrinfo(a[0].as.s->data, portbuf, &hints, &res);
    if (rc != 0) kc_error_at(pos, "network.tcp_connect: could not resolve '%s'", a[0].as.s->data);
    kc_sock_t fd = KC_SOCK_INVALID;
    for (struct addrinfo *rp = res; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd == KC_SOCK_INVALID) continue;
        if (connect(fd, rp->ai_addr, (int)rp->ai_addrlen) == 0) break;
        kc_sock_close(fd); fd = KC_SOCK_INVALID;
    }
    freeaddrinfo(res);
    if (fd == KC_SOCK_INVALID) kc_error_at(pos, "network.tcp_connect: could not connect to %s:%lld", a[0].as.s->data, (long long)a[1].as.i);
    return sock_handle(fd);
}

static KcValue net_listen(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    kc_forbid_in_comptime(ip, pos, "network.tcp_listen");
    kc_sockets_init();
    if (n != 1 || a[0].type != VAL_INT) kc_error_at(pos, "network.tcp_listen(port: int) -> handle");
    kc_sock_t fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == KC_SOCK_INVALID) kc_error_at(pos, "network.tcp_listen: socket() failed");
    int opt = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof opt);
    struct sockaddr_in addr; memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET; addr.sin_addr.s_addr = htonl(INADDR_ANY); addr.sin_port = htons((uint16_t)a[0].as.i);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0) kc_error_at(pos, "network.tcp_listen: bind() failed on port %lld", (long long)a[0].as.i);
    if (listen(fd, 16) != 0) kc_error_at(pos, "network.tcp_listen: listen() failed");
    return sock_handle(fd);
}

static KcValue net_accept(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    kc_forbid_in_comptime(ip, pos, "network.tcp_accept");
    if (n != 1) kc_error_at(pos, "network.tcp_accept(server_handle) -> handle");
    kc_sock_t lfd = expect_sock(a[0], pos, "network.tcp_accept");
    kc_sock_t cfd = accept(lfd, NULL, NULL);
    if (cfd == KC_SOCK_INVALID) kc_error_at(pos, "network.tcp_accept: accept() failed");
    return sock_handle(cfd);
}

static KcValue net_send(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    kc_forbid_in_comptime(ip, pos, "network.tcp_send");
    if (n != 2 || a[1].type != VAL_STRING) kc_error_at(pos, "network.tcp_send(handle, data: string) -> int");
    kc_sock_t fd = expect_sock(a[0], pos, "network.tcp_send");
    long w = send(fd, a[1].as.s->data, a[1].as.s->len, 0);
    return kc_int(w);
}

static KcValue net_recv(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    kc_forbid_in_comptime(ip, pos, "network.tcp_recv");
    if (n != 2 || a[1].type != VAL_INT) kc_error_at(pos, "network.tcp_recv(handle, maxlen: int) -> string");
    kc_sock_t fd = expect_sock(a[0], pos, "network.tcp_recv");
    int64_t maxlen = a[1].as.i;
    char *buf = kc_malloc(maxlen + 1);
    long r = recv(fd, buf, (int)maxlen, 0);
    if (r < 0) r = 0;
    buf[r] = 0;
    return kc_string_take(buf, (int)r);
}

static KcValue net_close(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    kc_forbid_in_comptime(ip, pos, "network.close");
    if (n != 1) kc_error_at(pos, "network.close(handle)");
    kc_sock_close(expect_sock(a[0], pos, "network.close"));
    return kc_null();
}

/* ============================== UDP ============================== */

static KcValue net_udp_socket(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    kc_forbid_in_comptime(ip, pos, "network.udp_socket");
    kc_sockets_init();
    (void)a; (void)n;
    kc_sock_t fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd == KC_SOCK_INVALID) kc_error_at(pos, "network.udp_socket: socket() failed");
    return sock_handle(fd);
}

static KcValue net_udp_bind(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    kc_forbid_in_comptime(ip, pos, "network.udp_bind");
    kc_sockets_init();
    if (n != 1 || a[0].type != VAL_INT) kc_error_at(pos, "network.udp_bind(port: int) -> handle");
    kc_sock_t fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd == KC_SOCK_INVALID) kc_error_at(pos, "network.udp_bind: socket() failed");
    int opt = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof opt);
    struct sockaddr_in addr; memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET; addr.sin_addr.s_addr = htonl(INADDR_ANY); addr.sin_port = htons((uint16_t)a[0].as.i);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        kc_sock_close(fd);
        kc_error_at(pos, "network.udp_bind: bind() failed on port %lld", (long long)a[0].as.i);
    }
    return sock_handle(fd);
}

static KcValue net_udp_send_to(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    kc_forbid_in_comptime(ip, pos, "network.udp_send_to");
    if (n != 4 || a[1].type != VAL_STRING || a[2].type != VAL_INT || a[3].type != VAL_STRING)
        kc_error_at(pos, "network.udp_send_to(handle, host: string, port: int, data: string) -> int");
    kc_sock_t fd = expect_sock(a[0], pos, "network.udp_send_to");
    struct sockaddr_in addr; memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET; addr.sin_port = htons((uint16_t)a[2].as.i);
    if (inet_pton(AF_INET, a[1].as.s->data, &addr.sin_addr) != 1)
        kc_error_at(pos, "network.udp_send_to: invalid IPv4 address '%s'", a[1].as.s->data);
    long w = sendto(fd, a[3].as.s->data, a[3].as.s->len, 0, (struct sockaddr *)&addr, sizeof addr);
    return kc_int(w);
}

static KcValue net_udp_recv_from(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    kc_forbid_in_comptime(ip, pos, "network.udp_recv_from");
    if (n != 2 || a[1].type != VAL_INT) kc_error_at(pos, "network.udp_recv_from(handle, maxlen: int) -> [data, from_host, from_port]");
    kc_sock_t fd = expect_sock(a[0], pos, "network.udp_recv_from");
    int64_t maxlen = a[1].as.i;
    char *buf = kc_malloc(maxlen + 1);
    struct sockaddr_in src; socklen_t slen = sizeof src; memset(&src, 0, sizeof src);
    long r = recvfrom(fd, buf, (int)maxlen, 0, (struct sockaddr *)&src, &slen);
    if (r < 0) r = 0;
    buf[r] = 0;
    char hostbuf[INET6_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &src.sin_addr, hostbuf, sizeof hostbuf);
    KcValue out = kc_array_new();
    kc_array_push(out, kc_string_take(buf, (int)r));
    kc_array_push(out, kc_string(hostbuf));
    kc_array_push(out, kc_int(ntohs(src.sin_port)));
    return out;
}

/* ==================== raw sockets / custom protocols ====================
 * socket(AF_INET, SOCK_RAW, protocol) — lets a script see and build whole
 * IP datagrams itself (ICMP, a bespoke transport, anything above IP).
 * Like every OS, the kernel gates this behind root/Administrator: it is
 * not something this module can or should work around, so the error on
 * failure says so rather than leaving a bare "socket() failed". Combine
 * with set_hdrincl plus the pack_* / checksum16 helpers below to hand-build a
 * complete header. IPv4 only for now (see docs/ROADMAP.md). */

static KcValue net_raw_socket(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    kc_forbid_in_comptime(ip, pos, "network.raw_socket");
    kc_sockets_init();
    if (n != 1 || a[0].type != VAL_INT)
        kc_error_at(pos, "network.raw_socket(protocol: int) -> handle  (e.g. 1 = ICMP, 6 = TCP, 17 = UDP, 255 = IP_HDRINCL custom)");
    kc_sock_t fd = socket(AF_INET, SOCK_RAW, (int)a[0].as.i);
    if (fd == KC_SOCK_INVALID)
        kc_error_at(pos, "network.raw_socket: socket() failed (raw sockets usually require root / Administrator privileges)");
    return sock_handle(fd);
}

static KcValue net_set_hdrincl(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    kc_forbid_in_comptime(ip, pos, "network.set_hdrincl");
    if (n != 2 || a[1].type != VAL_BOOL)
        kc_error_at(pos, "network.set_hdrincl(handle, on: bool)  -- when on, raw_send_to's data must start with your own IP header");
    kc_sock_t fd = expect_sock(a[0], pos, "network.set_hdrincl");
    int opt = a[1].as.b ? 1 : 0;
    if (setsockopt(fd, IPPROTO_IP, IP_HDRINCL, (const char *)&opt, sizeof opt) != 0)
        kc_error_at(pos, "network.set_hdrincl: setsockopt(IP_HDRINCL) failed");
    return kc_null();
}

static KcValue net_raw_send_to(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    kc_forbid_in_comptime(ip, pos, "network.raw_send_to");
    if (n != 3 || a[1].type != VAL_STRING || a[2].type != VAL_STRING)
        kc_error_at(pos, "network.raw_send_to(handle, dest_ip: string, data: string) -> int");
    kc_sock_t fd = expect_sock(a[0], pos, "network.raw_send_to");
    struct sockaddr_in addr; memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    if (inet_pton(AF_INET, a[1].as.s->data, &addr.sin_addr) != 1)
        kc_error_at(pos, "network.raw_send_to: invalid IPv4 address '%s'", a[1].as.s->data);
    long w = sendto(fd, a[2].as.s->data, a[2].as.s->len, 0, (struct sockaddr *)&addr, sizeof addr);
    if (w < 0) kc_error_at(pos, "network.raw_send_to: sendto() failed");
    return kc_int(w);
}

static KcValue net_raw_recv_from(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    kc_forbid_in_comptime(ip, pos, "network.raw_recv_from");
    if (n != 2 || a[1].type != VAL_INT) kc_error_at(pos, "network.raw_recv_from(handle, maxlen: int) -> [data, src_ip]");
    kc_sock_t fd = expect_sock(a[0], pos, "network.raw_recv_from");
    int64_t maxlen = a[1].as.i;
    char *buf = kc_malloc(maxlen + 1);
    struct sockaddr_in src; socklen_t slen = sizeof src; memset(&src, 0, sizeof src);
    long r = recvfrom(fd, buf, (int)maxlen, 0, (struct sockaddr *)&src, &slen);
    if (r < 0) r = 0;
    buf[r] = 0;
    char hostbuf[INET6_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &src.sin_addr, hostbuf, sizeof hostbuf);
    KcValue out = kc_array_new();
    kc_array_push(out, kc_string_take(buf, (int)r));
    kc_array_push(out, kc_string(hostbuf));
    return out;
}

/* ======================= protocol-builder helpers =======================
 * A hand-built packet is just a KingCobra string used as a byte buffer —
 * the same convention tcp_recv/udp_recv_from already return bytes in.
 * pack_* appends a big-endian (network-order) integer and returns the
 * grown buffer; unpack_* reads one back out at a given offset. Together
 * with checksum16 (the RFC 1071 Internet checksum every IPv4/ICMP/UDP/TCP
 * header uses) that's enough to hand-assemble or parse an arbitrary wire
 * format a few fields at a time, without std.memory's heavier alloc/free
 * handles. Pure byte manipulation — no socket, no comptime restriction. */

static KcValue net_pack_u8(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    if (n != 2 || a[0].type != VAL_STRING || a[1].type != VAL_INT)
        kc_error_at(pos, "network.pack_u8(buf: string, val: int) -> string");
    int oldlen = a[0].as.s->len;
    char *out = kc_malloc(oldlen + 1);
    memcpy(out, a[0].as.s->data, oldlen);
    out[oldlen] = (char)(uint8_t)a[1].as.i;
    return kc_string_take(out, oldlen + 1);
}

static KcValue net_pack_u16(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    if (n != 2 || a[0].type != VAL_STRING || a[1].type != VAL_INT)
        kc_error_at(pos, "network.pack_u16(buf: string, val: int) -> string  (big-endian / network order)");
    int oldlen = a[0].as.s->len;
    uint16_t v = (uint16_t)a[1].as.i;
    char *out = kc_malloc(oldlen + 2);
    memcpy(out, a[0].as.s->data, oldlen);
    out[oldlen] = (char)(v >> 8); out[oldlen + 1] = (char)(v & 0xFF);
    return kc_string_take(out, oldlen + 2);
}

static KcValue net_pack_u32(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    if (n != 2 || a[0].type != VAL_STRING || a[1].type != VAL_INT)
        kc_error_at(pos, "network.pack_u32(buf: string, val: int) -> string  (big-endian / network order)");
    int oldlen = a[0].as.s->len;
    uint32_t v = (uint32_t)a[1].as.i;
    char *out = kc_malloc(oldlen + 4);
    memcpy(out, a[0].as.s->data, oldlen);
    out[oldlen]     = (char)((v >> 24) & 0xFF);
    out[oldlen + 1] = (char)((v >> 16) & 0xFF);
    out[oldlen + 2] = (char)((v >> 8) & 0xFF);
    out[oldlen + 3] = (char)(v & 0xFF);
    return kc_string_take(out, oldlen + 4);
}

static KcValue net_pack_bytes(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    if (n != 2 || a[0].type != VAL_STRING || a[1].type != VAL_STRING)
        kc_error_at(pos, "network.pack_bytes(buf: string, data: string) -> string");
    int oldlen = a[0].as.s->len, addlen = a[1].as.s->len;
    char *out = kc_malloc(oldlen + addlen);
    memcpy(out, a[0].as.s->data, oldlen);
    memcpy(out + oldlen, a[1].as.s->data, addlen);
    return kc_string_take(out, oldlen + addlen);
}

static void check_unpack_bounds(KcPos pos, KcValue *a, int n, int width, const char *who) {
    if (n != 2 || a[0].type != VAL_STRING || a[1].type != VAL_INT)
        kc_error_at(pos, "%s(buf: string, offset: int) -> int", who);
    int64_t off = a[1].as.i;
    if (off < 0 || off + width > a[0].as.s->len)
        kc_error_at(pos, "%s: offset %lld out of range for a %d-byte buffer", who, (long long)off, a[0].as.s->len);
}

static KcValue net_unpack_u8(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    check_unpack_bounds(pos, a, n, 1, "network.unpack_u8");
    const unsigned char *d = (const unsigned char *)a[0].as.s->data;
    return kc_int(d[a[1].as.i]);
}

static KcValue net_unpack_u16(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    check_unpack_bounds(pos, a, n, 2, "network.unpack_u16");
    const unsigned char *d = (const unsigned char *)a[0].as.s->data + a[1].as.i;
    return kc_int(((uint16_t)d[0] << 8) | d[1]);
}

static KcValue net_unpack_u32(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    check_unpack_bounds(pos, a, n, 4, "network.unpack_u32");
    const unsigned char *d = (const unsigned char *)a[0].as.s->data + a[1].as.i;
    return kc_int(((uint32_t)d[0] << 24) | ((uint32_t)d[1] << 16) | ((uint32_t)d[2] << 8) | d[3]);
}

/* RFC 1071 Internet checksum — the fold-and-one's-complement algorithm
 * every IPv4/ICMP/UDP/TCP header uses. Caller zeroes the checksum field
 * before computing, then writes the result back in with pack_u16. An
 * odd-length buffer has its trailing byte zero-padded, per the RFC. */
static KcValue net_checksum16(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    if (n != 1 || a[0].type != VAL_STRING) kc_error_at(pos, "network.checksum16(data: string) -> int");
    const unsigned char *d = (const unsigned char *)a[0].as.s->data;
    int len = a[0].as.s->len;
    uint32_t sum = 0;
    while (len > 1) { sum += (uint32_t)((d[0] << 8) | d[1]); d += 2; len -= 2; }
    if (len == 1) sum += (uint32_t)(d[0] << 8);
    while (sum >> 16) sum = (sum & 0xFFFFu) + (sum >> 16);
    return kc_int((~sum) & 0xFFFFu);
}

/* ============================ socket options ============================ */

static KcValue net_set_broadcast(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    kc_forbid_in_comptime(ip, pos, "network.set_broadcast");
    if (n != 2 || a[1].type != VAL_BOOL) kc_error_at(pos, "network.set_broadcast(handle, on: bool)");
    kc_sock_t fd = expect_sock(a[0], pos, "network.set_broadcast");
    int opt = a[1].as.b ? 1 : 0;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, (const char *)&opt, sizeof opt);
    return kc_null();
}

static KcValue net_set_keepalive(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    kc_forbid_in_comptime(ip, pos, "network.set_keepalive");
    if (n != 2 || a[1].type != VAL_BOOL) kc_error_at(pos, "network.set_keepalive(handle, on: bool)");
    kc_sock_t fd = expect_sock(a[0], pos, "network.set_keepalive");
    int opt = a[1].as.b ? 1 : 0;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (const char *)&opt, sizeof opt);
    return kc_null();
}

static KcValue net_set_nodelay(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    kc_forbid_in_comptime(ip, pos, "network.set_nodelay");
    if (n != 2 || a[1].type != VAL_BOOL)
        kc_error_at(pos, "network.set_nodelay(handle, on: bool)  -- disables Nagle's algorithm on a TCP socket");
    kc_sock_t fd = expect_sock(a[0], pos, "network.set_nodelay");
#ifdef TCP_NODELAY
    int opt = a[1].as.b ? 1 : 0;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&opt, sizeof opt);
#else
    (void)fd;
    kc_error_at(pos, "network.set_nodelay: TCP_NODELAY is not available on this platform");
#endif
    return kc_null();
}

static KcValue net_set_recv_timeout(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    kc_forbid_in_comptime(ip, pos, "network.set_recv_timeout");
    if (n != 2 || (a[1].type != VAL_INT && a[1].type != VAL_FLOAT))
        kc_error_at(pos, "network.set_recv_timeout(handle, seconds: float)");
    kc_sock_t fd = expect_sock(a[0], pos, "network.set_recv_timeout");
    double secs = a[1].type == VAL_FLOAT ? a[1].as.f : (double)a[1].as.i;
#if defined(KC_PLATFORM_WINDOWS)
    DWORD ms = (DWORD)(secs * 1000.0);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&ms, sizeof ms);
#else
    struct timeval tv; tv.tv_sec = (long)secs; tv.tv_usec = (long)((secs - (double)tv.tv_sec) * 1e6);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#endif
    return kc_null();
}

static KcValue net_set_send_timeout(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    kc_forbid_in_comptime(ip, pos, "network.set_send_timeout");
    if (n != 2 || (a[1].type != VAL_INT && a[1].type != VAL_FLOAT))
        kc_error_at(pos, "network.set_send_timeout(handle, seconds: float)");
    kc_sock_t fd = expect_sock(a[0], pos, "network.set_send_timeout");
    double secs = a[1].type == VAL_FLOAT ? a[1].as.f : (double)a[1].as.i;
#if defined(KC_PLATFORM_WINDOWS)
    DWORD ms = (DWORD)(secs * 1000.0);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&ms, sizeof ms);
#else
    struct timeval tv; tv.tv_sec = (long)secs; tv.tv_usec = (long)((secs - (double)tv.tv_sec) * 1e6);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#endif
    return kc_null();
}

static KcValue net_shutdown(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    kc_forbid_in_comptime(ip, pos, "network.shutdown");
    if (n != 2 || a[1].type != VAL_STRING)
        kc_error_at(pos, "network.shutdown(handle, how: string)  -- how is \"read\", \"write\" or \"both\"");
    kc_sock_t fd = expect_sock(a[0], pos, "network.shutdown");
    int how;
    if (strcmp(a[1].as.s->data, "read") == 0) how = 0;
    else if (strcmp(a[1].as.s->data, "write") == 0) how = 1;
    else if (strcmp(a[1].as.s->data, "both") == 0) how = 2;
    else { kc_error_at(pos, "network.shutdown: how must be \"read\", \"write\" or \"both\", got '%s'", a[1].as.s->data); return kc_null(); }
    shutdown(fd, how);
    return kc_null();
}

/* ========================= addresses & resolution ========================= */

static KcValue addr_to_string(struct sockaddr_storage *ss) {
    char host[INET6_ADDRSTRLEN] = {0};
    int port = 0;
    if (ss->ss_family == AF_INET) {
        struct sockaddr_in *s4 = (struct sockaddr_in *)ss;
        inet_ntop(AF_INET, &s4->sin_addr, host, sizeof host);
        port = ntohs(s4->sin_port);
    } else if (ss->ss_family == AF_INET6) {
        struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)ss;
        inet_ntop(AF_INET6, &s6->sin6_addr, host, sizeof host);
        port = ntohs(s6->sin6_port);
    } else {
        snprintf(host, sizeof host, "?");
    }
    char buf[INET6_ADDRSTRLEN + 16];
    snprintf(buf, sizeof buf, "%s:%d", host, port);
    return kc_string(buf);
}

static KcValue net_local_addr(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    kc_forbid_in_comptime(ip, pos, "network.local_addr");
    if (n != 1) kc_error_at(pos, "network.local_addr(handle) -> string \"ip:port\"");
    kc_sock_t fd = expect_sock(a[0], pos, "network.local_addr");
    struct sockaddr_storage ss; memset(&ss, 0, sizeof ss); socklen_t len = sizeof ss;
    if (getsockname(fd, (struct sockaddr *)&ss, &len) != 0) kc_error_at(pos, "network.local_addr: getsockname() failed");
    return addr_to_string(&ss);
}

static KcValue net_peer_addr(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    kc_forbid_in_comptime(ip, pos, "network.peer_addr");
    if (n != 1) kc_error_at(pos, "network.peer_addr(handle) -> string \"ip:port\"");
    kc_sock_t fd = expect_sock(a[0], pos, "network.peer_addr");
    struct sockaddr_storage ss; memset(&ss, 0, sizeof ss); socklen_t len = sizeof ss;
    if (getpeername(fd, (struct sockaddr *)&ss, &len) != 0) kc_error_at(pos, "network.peer_addr: getpeername() failed (not connected?)");
    return addr_to_string(&ss);
}

static KcValue net_resolve(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    kc_forbid_in_comptime(ip, pos, "network.resolve");
    kc_sockets_init();
    if (n != 1 || a[0].type != VAL_STRING) kc_error_at(pos, "network.resolve(host: string) -> array of ip strings");
    struct addrinfo hints; memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res;
    int rc = getaddrinfo(a[0].as.s->data, NULL, &hints, &res);
    if (rc != 0) kc_error_at(pos, "network.resolve: could not resolve '%s'", a[0].as.s->data);
    KcValue out = kc_array_new();
    char buf[INET6_ADDRSTRLEN];
    for (struct addrinfo *rp = res; rp; rp = rp->ai_next) {
        const void *addr_ptr;
        if (rp->ai_family == AF_INET) addr_ptr = &((struct sockaddr_in *)rp->ai_addr)->sin_addr;
        else if (rp->ai_family == AF_INET6) addr_ptr = &((struct sockaddr_in6 *)rp->ai_addr)->sin6_addr;
        else continue;
        if (!inet_ntop(rp->ai_family, addr_ptr, buf, sizeof buf)) continue;
        bool dup = false;
        for (int i = 0; i < out.as.arr->count; i++)
            if (strcmp(out.as.arr->items[i].as.s->data, buf) == 0) { dup = true; break; }
        if (!dup) kc_array_push(out, kc_string(buf));
    }
    freeaddrinfo(res);
    return out;
}

static KcModuleEntry ENTRIES[] = {
    {"tcp_connect", net_connect}, {"tcp_listen", net_listen}, {"tcp_accept", net_accept},
    {"tcp_send", net_send}, {"tcp_recv", net_recv}, {"tcp_close", net_close}, {"close", net_close},

    {"udp_socket", net_udp_socket}, {"udp_bind", net_udp_bind},
    {"udp_send_to", net_udp_send_to}, {"udp_recv_from", net_udp_recv_from},

    {"raw_socket", net_raw_socket}, {"set_hdrincl", net_set_hdrincl},
    {"raw_send_to", net_raw_send_to}, {"raw_recv_from", net_raw_recv_from},

    {"pack_u8", net_pack_u8}, {"pack_u16", net_pack_u16}, {"pack_u32", net_pack_u32},
    {"pack_bytes", net_pack_bytes},
    {"unpack_u8", net_unpack_u8}, {"unpack_u16", net_unpack_u16}, {"unpack_u32", net_unpack_u32},
    {"checksum16", net_checksum16},

    {"set_broadcast", net_set_broadcast}, {"set_keepalive", net_set_keepalive},
    {"set_nodelay", net_set_nodelay},
    {"set_recv_timeout", net_set_recv_timeout}, {"set_send_timeout", net_set_send_timeout},
    {"shutdown", net_shutdown},

    {"local_addr", net_local_addr}, {"peer_addr", net_peer_addr}, {"resolve", net_resolve},
};

KcModule *kc_mod_network(void) {
    static KcModule m = { "network", ENTRIES, sizeof(ENTRIES)/sizeof(ENTRIES[0]) };
    return &m;
}
