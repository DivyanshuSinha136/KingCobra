# std.network — TCP, UDP, raw sockets, and a protocol builder

```kingcobra
import std.network;
```

Built directly on BSD sockets / Winsock via `kc_platform.h`, so the same
source runs on Linux/macOS and Windows. Every function here is a
side-effecting native and is refused inside a `comptime { }` block (see
`docs/LANGUAGE.md`), except the pure byte-buffer helpers at the bottom
(`pack_*`, `unpack_*`, `checksum16`), which do no I/O and are always
allowed. A socket is an opaque `handle` value, closed with `close` (an
alias of the old `tcp_close`, kept for existing code — either name
works on a TCP, UDP, or raw handle).

## TCP

| Function | Notes |
|---|---|
| `tcp_connect(host, port)` -> handle | resolves `host` (IPv4/IPv6) and connects |
| `tcp_listen(port)` -> handle | binds `0.0.0.0:port`, backlog 16 |
| `tcp_accept(server)` -> handle | blocks for the next connection |
| `tcp_send(handle, data)` -> int | bytes written |
| `tcp_recv(handle, maxlen)` -> string | up to `maxlen` bytes, `""` on EOF |
| `close(handle)` | also `tcp_close` |
| `shutdown(handle, how)` | `how` is `"read"`, `"write"`, or `"both"` — half-close without releasing the handle |

## UDP

| Function | Notes |
|---|---|
| `udp_socket()` -> handle | unconnected, ready for `udp_send_to` |
| `udp_bind(port)` -> handle | `port` 0 lets the OS pick a free port — read it back with `local_addr` |
| `udp_send_to(handle, host, port, data)` -> int | one datagram |
| `udp_recv_from(handle, maxlen)` -> `[data, from_host, from_port]` | blocks for the next datagram |

## Raw sockets

`socket(AF_INET, SOCK_RAW, protocol)` — a script sees and builds whole
IP datagrams itself: ICMP, a custom transport, anything layered directly
on IP. IPv4 only for now. Like every OS, raw sockets require root /
Administrator; `raw_socket` reports that plainly if `socket()` is denied
rather than leaving a bare errno.

| Function | Notes |
|---|---|
| `raw_socket(protocol)` -> handle | e.g. `1` = ICMP, `6` = TCP, `17` = UDP, `255` = fully custom |
| `set_hdrincl(handle, on)` | when `on`, the data passed to `raw_send_to` must start with your own IP header (`IP_HDRINCL`) |
| `raw_send_to(handle, dest_ip, data)` -> int | one datagram, kernel adds the IP header unless `set_hdrincl` is on |
| `raw_recv_from(handle, maxlen)` -> `[data, src_ip]` | includes the IP header on the way in |

## Protocol builder

A hand-built packet is just a KingCobra string used as a byte buffer —
the same convention `tcp_recv`/`udp_recv_from` already return bytes in.
`pack_*` appends a big-endian (network-order) integer and returns the
grown buffer; `unpack_*` reads one back out at a byte offset (bounds-
checked — an out-of-range offset is a positioned error, not garbage).
These don't touch a socket and are pure, so they also work at any time,
including inside `comptime { }`.

| Function | Notes |
|---|---|
| `pack_u8(buf, val)` `pack_u16(buf, val)` `pack_u32(buf, val)` -> string | appends 1/2/4 bytes, big-endian |
| `pack_bytes(buf, data)` -> string | appends raw bytes (e.g. an already-built sub-header, or a payload) |
| `unpack_u8(buf, offset)` `unpack_u16(buf, offset)` `unpack_u32(buf, offset)` -> int | reads back out |
| `checksum16(data)` -> int | the RFC 1071 Internet checksum — the same algorithm IPv4/ICMP/UDP/TCP headers all use |

```kingcobra
// build an 8-byte UDP header, compute its checksum, then re-embed it
var hdr = "";
hdr = network.pack_u16(hdr, 53124);  // source port
hdr = network.pack_u16(hdr, 53);     // dest port
hdr = network.pack_u16(hdr, 8);      // length
hdr = network.pack_u16(hdr, 0);      // checksum placeholder
let sum = network.checksum16(hdr);
// ...rebuild hdr with pack_u16(..., sum) in the last field before sending
```

## Socket options, addresses, resolution

| Function | Notes |
|---|---|
| `set_broadcast(handle, on)` | `SO_BROADCAST` — needed to send to a UDP broadcast address |
| `set_keepalive(handle, on)` | `SO_KEEPALIVE` |
| `set_nodelay(handle, on)` | `TCP_NODELAY` — disables Nagle's algorithm |
| `set_recv_timeout(handle, seconds)` `set_send_timeout(handle, seconds)` | fractional seconds; a blocking call then returns/errors instead of hanging forever |
| `local_addr(handle)` `peer_addr(handle)` -> string | `"ip:port"` (`getsockname`/`getpeername`) |
| `resolve(host)` -> array of strings | every unique IPv4/IPv6 address `host` resolves to, no connection made |

Errors (resolution failure, bind/connect/socket failure, a raw socket
denied by the kernel, an out-of-range `unpack_*` offset) stop the
program with a positioned message, like every other std module.

See `examples/std_network.kc`.

## Not yet done

Non-blocking/async sockets and IPv6 raw sockets are tracked in
`docs/ROADMAP.md`, not implemented here. (`std.http` drives its own
non-blocking sockets internally.)
