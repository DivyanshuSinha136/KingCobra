#!/usr/bin/env python3
"""Emit a KingCobra program that checks http.decompress() against zlib/gzip output
for many payloads, levels, strategies and container formats — and that corrupt,
truncated and random inputs fail cleanly. Prints `problems: 0` when all is well."""
import base64, gzip, io, random, zlib

random.seed(7)
b64 = lambda b: base64.b64encode(b).decode()
samples = {
    "empty": b"", "one": b"x", "text": b"The quick brown fox jumps over the lazy dog. " * 12,
    "rand": bytes(random.randrange(256) for _ in range(500)),
    "runs": b"\x00" * 900 + b"\xff" * 200 + b"ab" * 200,
    "binary": bytes(range(256)) * 3,
    "words": " ".join(random.choice(["alpha", "beta", "gamma", "delta", "kc", "http"]) for _ in range(300)).encode(),
}
cases = []
def add(name, comp, mode, plain): cases.append((name, b64(comp), mode, b64(plain) if plain is not None else "FAIL"))
for n, d in samples.items():
    for lvl in (0, 1, 6, 9):
        add(f"zlib-{n}-L{lvl}", zlib.compress(d, lvl), "zlib", d)
        add(f"gzip-{n}-L{lvl}", gzip.compress(d, lvl), "gzip", d)
        c = zlib.compressobj(lvl, zlib.DEFLATED, -15); add(f"raw-{n}-L{lvl}", c.compress(d) + c.flush(), "deflate", d)
    for strat, sn in ((zlib.Z_FIXED, "fixed"), (zlib.Z_HUFFMAN_ONLY, "huff"), (zlib.Z_RLE, "rle")):
        c = zlib.compressobj(6, zlib.DEFLATED, -15, 9, strat); add(f"raw-{n}-{sn}", c.compress(d) + c.flush(), "deflate", d)
add("auto-zlib", zlib.compress(samples["text"]), "auto", samples["text"])
add("auto-gzip", gzip.compress(samples["text"]), "auto", samples["text"])
c = zlib.compressobj(6, zlib.DEFLATED, -15); add("auto-raw", c.compress(samples["words"]) + c.flush(), "auto", samples["words"])
add("gzip-concat", gzip.compress(b"first-") + gzip.compress(b"second"), "gzip", b"first-second")
buf = io.BytesIO()
with gzip.GzipFile(filename="name.txt", mode="wb", fileobj=buf) as g: g.write(b"with a file name header")
add("gzip-fname", buf.getvalue(), "gzip", b"with a file name header")
good = zlib.compress(samples["words"]); gg = gzip.compress(samples["words"])
for i in (2, 10, len(good) // 2, len(good) - 1):
    bad = bytearray(good); bad[i] ^= 0x55; add(f"corrupt-zlib-{i}", bytes(bad), "zlib", None)
for i in (12, len(gg) // 2, len(gg) - 5, len(gg) - 1):
    bad = bytearray(gg); bad[i] ^= 0xAA; add(f"corrupt-gzip-{i}", bytes(bad), "gzip", None)
for cut in (1, 2, 5, len(good) // 2, len(good) - 1): add(f"trunc-zlib-{cut}", good[:cut], "zlib", None)
for cut in (3, 10, 20, len(gg) - 1): add(f"trunc-gzip-{cut}", gg[:cut], "gzip", None)
add("wrong-mode", good, "gzip", None)
random.seed(99)
for k in range(30): add(f"garbage-{k}", bytes(random.randrange(256) for _ in range(random.randrange(1, 150))), random.choice(["gzip", "zlib", "deflate", "auto"]), None)

print("import std.http;")
print("fn check(name: string, comp: string, mode: string, want: string) -> int {")
print("  let r = http.decompress(http.base64_decode(comp), mode);")
print('  if (want == "FAIL") { if (r == null) { return 0; } print("UNEXPECTED SUCCESS", name); return 1; }')
print('  if (r == null) { print("FAILED", name, http.last_error()); return 1; }')
print('  if (http.base64_encode(r) != want) { print("MISMATCH", name); return 1; }')
print("  return 0;\n}\nfn main() {\n  var bad = 0;")
for n, c, m, p in cases: print(f'  bad = bad + check("{n}", "{c}", "{m}", "{p}");')
print(f'  print("inflate cases: {len(cases)}, problems:", bad);\n}}')
