#!/usr/bin/env python3
"""Probe the KingCobra server fixture (tests/http_server_fixture.kc) with a standard HTTP client."""
import http.client, socket, sys
port = int(sys.argv[1])
def conn(): return http.client.HTTPConnection("127.0.0.1", port, timeout=10)
def show(label, r, body=None):
    print(f"{label}: {r.status} {r.reason} | {(body if body is not None else r.read()).decode()!r}")
c = conn(); c.request("GET", "/hello?name=world&x=1"); r = c.getresponse(); show("get", r); c.close()
c = conn(); c.request("POST", "/echo", body=b"post body", headers={"Content-Type": "text/plain"}); r = c.getresponse()
print("post hdr:", r.getheader("X-Echo-Len"), r.getheader("Content-Type"), r.getheader("Connection"), r.getheader("Server", "")[:10]); show("post", r); c.close()
c = conn(); c.request("POST", "/echo", body=iter([b"chunk-one,", b"chunk-two"]), encode_chunked=True, headers={"Transfer-Encoding": "chunked"}); show("chunked", c.getresponse()); c.close()
c = conn(); c.request("HEAD", "/hello?a"); r = c.getresponse(); print("head:", r.status, r.getheader("Content-Length"), len(r.read())); c.close()
c = conn(); c.request("GET", "/empty"); r = c.getresponse(); print("204:", r.status, len(r.read())); c.close()
c = conn(); c.request("GET", "/nothing"); show("404", c.getresponse()); c.close()
c = conn(); c.request("GET", "/spaced%20path"); show("encoded", c.getresponse()); c.close()
def raw(data):
    s = socket.create_connection(("127.0.0.1", port), timeout=10); s.sendall(data); out = b""
    while True:
        d = s.recv(4096)
        if not d: break
        out += d
    return out.split(b"\r\n")[0].decode()
print("garbage:", raw(b"GARBAGE\r\n\r\n")); print("http2:", raw(b"GET / HTTP/2.0\r\n\r\n"))
c = conn(); c.request("GET", "/twice"); show("twice", c.getresponse()); c.close()
