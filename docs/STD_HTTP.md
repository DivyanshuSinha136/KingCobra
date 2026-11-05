# `std.http`

An HTTP/1.1 **client** (http and verified https) and a small blocking
**server**, in plain C with no build-time dependencies.

```kc
import std.http;

let r = http.get("https://example.com/");
if (r == null) { print("failed:", http.last_error()); }
else { print(r.status, r.reason, http.header(r, "content-type"), len(r.body)); }
```

`std.http` is split like `std.system`: `http.c` (module table), `http_url.c`
(URLs, encoding), `http_inflate.c` (gzip/zlib/deflate), `http_tls.c` (HTTPS),
`http_conn.c` (sockets + message parsing), `http_client.c`, `http_server.c`,
and the shared `http_glue.h`.

## Errors and `comptime`

* **Programming errors are fatal** with a positioned message: wrong argument
  types, a malformed header, a header containing a newline (header injection is
  refused), a closed handle, an unknown option.
* **Environmental failures return `null`** and set `http.last_error()`: DNS,
  refused, timeout, TLS verification, malformed response, body over the limit,
  a body shorter than `Content-Length`, a corrupt gzip stream.
* **An HTTP error status is not a failure.** `404` and `500` are normal
  responses with `ok == false`.
* Everything that touches the network (`get`…`download`, `client`, `listen`,
  `accept`, `respond`) is refused in `comptime { }`. The pure helpers
  (`url_*`, `query_parse`, `form`, `base64_*`, `decompress`, `status_text`) are allowed.

## Requests

```kc
http.get(url, headers = null, timeout = null)
http.head / http.options / http.delete      // same shape as get
http.post(url, body, headers = null, timeout = null)
http.put / http.patch                       // same shape as post
http.request(method, url, body = null, headers = null, timeout = null)
http.download(url, path, headers = null, timeout = null)
```

Every one of them also accepts a client as its first argument:
`http.get(client, url)`. `headers` is an array of `"Name: value"` strings.
`timeout` (seconds, float) overrides the client's; `0` means no timeout.
Bodies are binary-safe strings. A URL without a scheme is treated as `http://`.
`user:pass@` in a URL sends Basic auth (percent-decoded) and is removed from the
reported `url`.

### The response

`HttpResponse { status, reason, ok, version, url, redirects, headers, body, length, elapsed, tls, decoded }`

* `ok` is `200 ≤ status < 300`; `url` is the final URL after redirects (no credentials);
  `headers` is an array of `"Name: value"` in order; `length` is the number of body
  bytes received (for `download`, the bytes written to disk); `decoded` is true when a
  gzip/deflate body was decompressed (then `Content-Encoding` is removed and
  `Content-Length` is updated).
* `http.header(resp, name)` — first value, case-insensitive, or `null`.
  `http.header_all(resp, name)` — every value (e.g. `Set-Cookie`).

## What the client does for you

| | |
|---|---|
| **Redirects** | 301/302/303/307/308 followed up to `max_redirects` (10); relative `Location` resolved per RFC 3986. 303 (and 301/302 after POST) becomes GET and drops the body and `Content-Type`; 307/308 keep method and body. **Credentials (`Authorization`, `Cookie`, client auth, userinfo) are not sent to a different origin**, and an **https → http redirect is refused**. Exceeding the limit is an error. |
| **Compression** | sends `Accept-Encoding: gzip, deflate`; decodes gzip, zlib and raw deflate with checksum verification. The decoded size is capped by `max_body` (zip-bomb defence). |
| **Framing** | `Content-Length`, chunked (extensions and trailers), close-delimited, HEAD/204/304/1xx; conflicting or invalid `Content-Length`/`Transfer-Encoding` are rejected; a short body is an error, never silently truncated data. |
| **Timeouts** | real deadlines: connect (`connect_timeout`, 10 s) and one total deadline (`timeout`, 30 s) covering the whole call including redirects; every read/write/handshake honours it. DNS resolution itself (`getaddrinfo`) cannot be interrupted. |
| **Keep-alive** | clients from `http.client()` reuse connections (up to 4 idle, 30 s). A connection the server closed is detected before reuse; if one dies in flight and the request is idempotent, it is retried once on a fresh connection (never after a timeout, never for POST/PATCH). One-shot calls (`http.get(url)`) always use `Connection: close`. |
| **Cookies** | optional jar (`set_option(c, "cookies", true)`): `Domain`/`Path`/`Max-Age`/`Expires`/`Secure`, deletion, longest-path-first, a server cannot set cookies for another site. No public-suffix list (a `Domain=` with no dot is rejected as a simple guard). |
| **Limits** | headers ≤ 128 KiB / 256 lines; body ≤ `max_body` (64 MiB). |

## Clients and options

```kc
let c = http.client();                       // keep-alive on
http.set_option(c, "timeout", 5);
http.set_option(c, "bearer", "token");
let r = http.get(c, "https://api.example.com/items");
http.close(c);                               // frees sockets; safe to call twice
```

`http.set_option(client, key, value)`:

| key | value | meaning |
|---|---|---|
| `timeout`, `connect_timeout` | number ≥ 0 | seconds (0 = none) |
| `max_redirects` | 0–100 | exceeding it is an error |
| `follow_redirects` | bool | `false` returns the 3xx response itself |
| `verify` | bool | TLS certificate + host name verification (default **true**) |
| `ca_file` | path | CA bundle (PEM) instead of the system store; `""` resets |
| `user_agent` | string | default `KingCobra/<version>` |
| `cookies` | bool | enable the jar |
| `keepalive` | bool | connection reuse |
| `decompress` | bool | decode gzip/deflate (default true) |
| `max_body` | int | max body bytes, also the decoded size |
| `header` / `remove_header` | `"Name: value"` / name | default header on every request |
| `basic_auth` / `bearer` | `"user:pass"` / token | `Authorization` default; `""` clears |

`http.cookies(c)` lists the jar; `http.clear_cookies(c)` empties it.

## HTTPS

`https://` URLs use OpenSSL **loaded at run time** (`libssl` 1.1.1 or 3.x via
`dlopen`/`LoadLibrary`) — nothing is linked, so the interpreter builds and runs
without it, and `http://` never touches it. If the library is missing an
`https://` request returns `null` with a message saying so.

Verification is on by default: the certificate chain is checked against the
system CA store and the **host name (or IP) must match the certificate**; SNI is
sent; TLS 1.2 is the minimum. To trust a private CA use `ca_file`; to disable
verification (testing only) `set_option(c, "verify", false)`.
Messages are specific: `certificate verification failed: self-signed certificate`,
`... IP address mismatch`, `... certificate has expired`.

## URL, query and encoding helpers (pure)

`url_parse(url)` → `Url { scheme, host, port, path, query, userinfo, target }`
(null if malformed; IPv6 literals and default ports handled);
`url_join(base, ref)` (RFC 3986 resolution);
`url_encode(s)` / `url_decode(s, plus_is_space = false)`;
`query_parse("a=1&b=x%20y")` → `[[a, 1], [b, x y]]`;
`form([[k, v], ...])` → `application/x-www-form-urlencoded` (values may be strings, numbers, bools);
`base64_encode(s)` / `base64_decode(s)` (standard or URL-safe alphabet; null if invalid);
`decompress(data, format = "auto")` — `"gzip"`, `"zlib"`, `"deflate"` (raw) or `"auto"`;
`status_text(404)` → `"Not Found"`.

## Server

```kc
let s = http.listen(8080);                       // 127.0.0.1 by default; "0.0.0.0" to expose
while (true) {
    let req = http.accept(s, 1.0);               // null if nothing arrived within 1 s
    if (req != null) { http.respond(req, 200, "hello " + req.path); }
}
```

* `http.listen(port, host = "127.0.0.1", backlog = 64)` (port `0` picks a free one; `http.server_port(s)`).
* `http.accept(server, timeout = null)` →
  `HttpRequest { method, target, path (percent-decoded), query (raw), version, headers, body, remote, conn }` or `null`.
* `http.respond(req, status, body = "", headers = null)` → bool. Adds `Date`, `Server`,
  `Content-Length`, `Connection: close` and a default `Content-Type: text/plain`; honours HEAD, 204 and 304;
  answering twice is an error.
* `http.close(server)`.

Deliberately simple: **one request at a time, one request per connection**; request
headers ≤ 128 KiB, body ≤ 16 MiB; a client has 10 s to deliver its request (a stalled
client delays the loop by at most that); chunked bodies and `Expect: 100-continue` work;
malformed requests are answered 400/413/414/431/505 automatically and never reach the script.
**No TLS and no concurrency** — put a TLS terminator / reverse proxy in front for production.

## Not implemented

Proxies (`HTTP_PROXY`, CONNECT), HTTP/2, multipart/form-data helpers, streaming a
response body to the script (use `download` for large files), server-side TLS and
keep-alive, WebSockets, a public-suffix list for cookies. Windows paths (Winsock,
`WSAPoll`, `LoadLibrary` of libssl) are written against the platform docs but were
**not compiled or run** in the Linux development sandbox — build with MinGW and run `make test`.

## Migrating from v0.1

v0.1 had only `http.get(url)` and `http.post(url, body)`, which returned the **raw
response text** (status line + headers + undecoded body) and aborted the program on
a connection error. They now return an `HttpResponse` struct (`r.body` is what you
used to get after the blank line) and return `null` + `last_error()` on failure.
Redirects, chunked bodies and gzip, which v0.1 left to the caller, are handled.
