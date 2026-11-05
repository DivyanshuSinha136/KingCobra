import subprocess,time
import sys, os, tempfile
# usage: python3 tests/bench/compare.py [OLD_BINARY] [NEW_BINARY]
#   with one argument, benchmarks just that binary; with none, ./kingcobra
NEW = sys.argv[2] if len(sys.argv) > 2 else (sys.argv[1] if len(sys.argv) > 1 else "./kingcobra")
OLD = sys.argv[1] if len(sys.argv) > 2 else None
TMP = os.path.join(tempfile.gettempdir(), "kc_bench.kc")
def once(k, src):
    open(TMP, "w").write(src)
    t = time.perf_counter(); r = subprocess.run([k, TMP], capture_output=True, text=True)
    return time.perf_counter() - t, r.stdout.strip()

def measure(binaries, src, reps=7):
    """Best-of-`reps` for each binary, with the binaries INTERLEAVED (a, b, a, b, ...) so that
    machine-load drift hits both equally. Returns [(seconds, stdout), ...]."""
    best = [(9e9, "")] * len(binaries)
    for _ in range(reps):
        for i, k in enumerate(binaries):
            t, out = once(k, src)
            if t < best[i][0]: best[i] = (t, out)
    return best
N=1000000
cases=[
 ("while loop (cmp + add-assign)", N, "", f"var i=0; while (i<{N}) {{ i += 1; }}"),
 ("for i in range(n)", N, "", f"var s=0; for (i in range({N})) {{ s += i; }}"),
 ("loop + bitwise expr", N, "", f"var s=0; var i=0; while (i<{N}) {{ s += i & 7; i += 1; }}"),
 ("user fn call (JIT-eligible)", N, "fn add(a: int, b: int) -> int { return a + b; }", f"var s=0; var i=0; while (i<{N}) {{ s = add(s, i); i += 1; }}"),
 ("user fn call (interpreted)", N, "fn first(a: int[]) -> int { return a[0]; }", f"var a=[1,2,3]; var s=0; var i=0; while (i<{N}) {{ s += first(a); i += 1; }}"),
 ("closure call", N, "", f"let f = fn(x: int) -> int {{ return x + 1; }}; var s=0; var i=0; while (i<{N}) {{ s = f(s); i += 1; }}"),
 ("struct field read+write", N, "struct P { x: int, y: int }", f"var p = P {{ x: 1, y: 2 }}; var i=0; while (i<{N}) {{ p.x = p.x + p.y; i += 1; }}"),
 ("array index read+write", N, "", f"var a=[0,0,0,0]; var i=0; while (i<{N}) {{ a[i & 3] = a[(i+1) & 3] + 1; i += 1; }}"),
 ("string concat \"ab\" + str(i)", N, "", f"var s=\"\"; var i=0; while (i<{N}) {{ s = \"ab\" + str(i); i += 1; }}"),
 ("a.push(i)", N, "", f"var a=[]; var i=0; while (i<{N}) {{ a.push(i); i += 1; }}"),
 ("builtin len(a)", N, "", f"var a=[1]; var s=0; var i=0; while (i<{N}) {{ s += len(a); i += 1; }}"),
 ("if / else", N, "", f"var s=0; var i=0; while (i<{N}) {{ if (i % 3 == 0) {{ s += 1; }} else {{ s += 2; }} i += 1; }}"),
 ("pointer load/store p[i]", N, "", f"let p = alloc(8000) as *i32; var i=0; while (i<{N}) {{ p[i % 2000] = i; i += 1; }}"),
 ("recursive fib(27) (JIT)", 832040*0+635621, "fn fib(n: int) -> int { if (n < 2) { return n; } return fib(n-1) + fib(n-2); }", "print(fib(27));"),
]
if OLD:
    print("%-32s %10s %10s %8s" % ("operation (per iteration)", "old", "new", "speedup"))
else:
    print("%-32s %10s" % ("operation (per iteration)", "time"))
for name, n, pre, body in cases:
    src = pre + "\nfn main() {\n" + body + "\n}\n"
    if OLD:
        (to, oo), (tn, on) = measure([OLD, NEW], src)
        assert oo == on, (name, oo, on)      # the two builds must print the same result
        print("%-32s %8.0f ns %8.0f ns %7.1fx" % (name, to / n * 1e9, tn / n * 1e9, to / tn))
    else:
        ((tn, on),) = measure([NEW], src)
        print("%-32s %8.0f ns" % (name, tn / n * 1e9))
