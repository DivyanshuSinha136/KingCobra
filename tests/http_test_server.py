#!/usr/bin/env python3
"""Local HTTP(S) server used by tests/smoke_test.sh to exercise std.http.

    python3 tests/http_test_server.py PORTFILE [CERT KEY]

Writes the chosen port to PORTFILE, then serves until killed. With CERT/KEY it
serves HTTPS instead. Endpoints are deliberately awkward: redirects, chunked,
gzip/deflate, slow, truncated, close-delimited, cookies, keep-alive counters.
"""
import gzip, os, ssl, sys, time, zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

BIG = bytes((i * 7 + 3) % 251 for i in range(300000)) * 4      # ~1.2 MB, compressible + varied
conn_counter = [0]


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "T/1"

    def setup(self):
        super().setup()
        conn_counter[0] += 1
        self.conn_id = conn_counter[0]
        self.req_on_conn = 0

    def log_message(self, *a):
        pass

    # ---- helpers
    def body_in(self):
        if self.headers.get("Transfer-Encoding", "").lower() == "chunked":
            data = b""
            while True:
                size = int(self.rfile.readline().strip().split(b";")[0], 16)
                if size == 0:
                    self.rfile.readline()
                    return data
                data += self.rfile.read(size)
                self.rfile.readline()
        n = int(self.headers.get("Content-Length", 0))
        return self.rfile.read(n) if n else b""

    def send(self, code, body=b"", headers=(), close=False):
        self.send_response(code)
        for k, v in headers:
            self.send_header(k, v)
        if close:
            self.send_header("Connection", "close")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def redirect(self, code, loc):
        self.send(code, b"", [("Location", loc)])

    def handle_any(self):
        self.req_on_conn += 1
        data = self.body_in()
        path = self.path.split("?")[0]
        host = self.headers.get("Host")
        base = f"http://{host}"
        scheme_base = ("https" if isinstance(self.connection, ssl.SSLSocket) else "http") + "://" + host
        if path == "/echo":
            lines = [f"{self.command} {self.path}"] + [f"{k}: {v}" for k, v in self.headers.items()] + ["", data.decode("latin1")]
            return self.send(200, "\n".join(lines).encode("latin1"), [("Content-Type", "text/plain")])
        if path.startswith("/status/"):
            return self.send(int(path.split("/")[2]), b"status page")
        if path == "/204":
            self.send_response(204); self.end_headers(); return
        if path == "/304":
            self.send_response(304); self.end_headers(); return
        if path.startswith("/redirect/"):
            n = int(path.split("/")[2])
            return self.redirect(302, f"/redirect/{n-1}" if n > 1 else "/echo")
        if path == "/rel":
            return self.redirect(301, "sub/../echo?x=1")
        if path == "/loop":
            return self.redirect(302, "/loop")
        if path == "/r303":
            return self.redirect(303, "/echo")
        if path == "/r307":
            return self.redirect(307, "/echo")
        if path == "/r308":
            return self.redirect(308, "/echo")
        if path == "/xorigin":
            return self.redirect(302, f"http://localhost:{self.server.server_port}/echo")
        if path == "/to-http":
            return self.redirect(302, "http://127.0.0.1:1/x")
        if path == "/gzip":
            return self.send(200, gzip.compress(b"hello gzip " * 1000), [("Content-Encoding", "gzip")])
        if path == "/gzip-big":
            return self.send(200, gzip.compress(BIG, 6), [("Content-Encoding", "gzip")])
        if path == "/gzip-stored":
            return self.send(200, gzip.compress(os.urandom(70000), 0), [("Content-Encoding", "gzip")])
        if path == "/deflate":
            return self.send(200, zlib.compress(b"zlib wrapped " * 500), [("Content-Encoding", "deflate")])
        if path == "/rawdeflate":
            c = zlib.compressobj(wbits=-15); d = c.compress(b"raw deflate " * 500) + c.flush()
            return self.send(200, d, [("Content-Encoding", "deflate")])
        if path == "/badgzip":
            return self.send(200, b"\x1f\x8b\x08\x00garbage-not-deflate", [("Content-Encoding", "gzip")])
        if path == "/chunked":
            self.send_response(200); self.send_header("Transfer-Encoding", "chunked"); self.end_headers()
            for part in (b"alpha", b"beta-" * 50, b"gamma"):
                self.wfile.write(f"{len(part):x};ext=1\r\n".encode() + part + b"\r\n")
            self.wfile.write(b"0\r\nX-Trailer: yes\r\n\r\n"); return
        if path == "/close-delimited":
            self.send_response(200); self.send_header("Connection", "close"); self.end_headers()
            self.wfile.write(b"until-close " * 100); self.close_connection = True; return
        if path == "/truncated":
            self.send_response(200); self.send_header("Content-Length", "100"); self.end_headers()
            self.wfile.write(b"only-ten-b"); self.close_connection = True; return
        if path == "/slow":
            time.sleep(float(self.path.split("t=")[1]) if "t=" in self.path else 3)
            return self.send(200, b"slow done")
        if path == "/bigbody":
            return self.send(200, BIG)
        if path == "/cookie-set":
            self.send_response(200)
            self.send_header("Set-Cookie", "sid=abc123; Path=/")
            self.send_header("Set-Cookie", "pref=dark; Path=/echo; Max-Age=3600")
            self.send_header("Set-Cookie", "gone=1; Path=/")
            self.send_header("Set-Cookie", "evil=1; Domain=example.org")
            self.send_header("Content-Length", "2"); self.end_headers(); self.wfile.write(b"ok"); return
        if path == "/cookie-del":
            self.send_response(200)
            self.send_header("Set-Cookie", "gone=; Path=/; Max-Age=0")
            self.send_header("Content-Length", "2"); self.end_headers(); self.wfile.write(b"ok"); return
        if path == "/conn":
            return self.send(200, f"conn={self.conn_id} req={self.req_on_conn}".encode())
        if path == "/close-after":
            return self.send(200, f"conn={self.conn_id}".encode(), close=True)
        if path == "/drop-after-first":                          # kills keep-alive silently after 1 request
            self.send(200, f"conn={self.conn_id}".encode()); self.close_connection = True; return
        if path == "/headers":
            return self.send(200, b"x", [("X-Multi", "a"), ("X-Multi", "b"), ("Content-Type", "text/x-test")])
        return self.send(404, b"nope")

    do_GET = do_POST = do_PUT = do_PATCH = do_DELETE = do_HEAD = do_OPTIONS = handle_any


def main():
    portfile = sys.argv[1]
    srv = ThreadingHTTPServer(("127.0.0.1", 0), H)
    srv.daemon_threads = True
    if len(sys.argv) >= 4:
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(sys.argv[2], sys.argv[3])
        srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
    with open(portfile, "w") as f:
        f.write(str(srv.server_port))
    srv.serve_forever()


if __name__ == "__main__":
    main()
