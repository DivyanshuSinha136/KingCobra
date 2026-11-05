#!/bin/bash
# Training workload for `make pgo` (profile-guided optimization).
#
# Runs the interpreter over a spread of typical programs so the compiler learns which paths are
# hot: tight loops, calls, closures, struct and array access, string building, method calls,
# sorting, try/catch, recursion, the JIT, pointers/buffers, and the example programs. Pure bash.
# Every family is run at the SAME weight: PGO treats whatever the training under-exercises as
# cold code and optimizes it for size, so a path left out (or run lightly) gets slower.
set -u
K="${1:-./kingcobra}"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
N=300000

run() { printf '%s\n' "$1" > "$T/p.kc"; "$K" "$T/p.kc" >/dev/null 2>&1 || true; }

run "fn main() { var i=0; while (i<$N) { i += 1; } }"
run "fn main() { var s=0; for (i in range($N)) { s += i; } print(s); }"
run "fn add(a: int[], b: int) -> int { return a[0] + b; } fn main() { var a=[1]; var s=0; var i=0; while (i<$N) { s = add(a, i); i += 1; } }"
run "fn main() { let f = fn(x: int) -> int { return x + 1; }; var s=0; var i=0; while (i<$N) { s = f(s); i += 1; } }"
run "fn main() { var s=\"\"; var i=0; while (i<$N) { s = \"ab\" + i; i += 1; } }"
run "fn main() { var s=\"\"; var i=0; while (i<$N) { s = f\"n={i} sq={i*i}\"; i += 1; } }"
run "fn main() { var a=[]; var i=0; while (i<$N) { a.push(i); i += 1; } var t=0; for (x in a) { t += x; } print(t); }"
run "struct P { x: int, y: int } fn main() { var p = P { x: 1, y: 2 }; var i=0; while (i<$N) { p.x = p.x + p.y; i += 1; } }"
run "fn main() { var a=[0,0,0,0]; var i=0; while (i<$N) { a[i & 3] = a[(i+1) & 3] + 1; i += 1; } }"
run "fn main() { var s=0; var i=0; while (i<$N) { if (i % 3 == 0) { s += 1; } else { s += 2; } i += 1; } }"
run "fn main() { var a=[1]; var s=0; var i=0; while (i<$N) { s += len(a); i += 1; } }"
run "fn main() { let o = { a: 1, b: 2 }; var s=0; var i=0; while (i<$N) { s += o.a + o.b; i += 1; } }"
run "fn fib(n: int) -> int { if (n < 2) { return n; } return fib(n-1) + fib(n-2); } fn main() { print(fib(25)); }"
run "fn fib(a: int[], n: int) -> int { if (n < 2) { return n; } return fib(a, n-1) + fib(a, n-2); } fn main() { print(fib([0], 22)); }"
run "fn main() { var xs = []; var i = 0; while (i < 40000) { push(xs, (i * 7919) % 10007); i += 1; } let s = sorted(xs); let m = xs.map(fn(x) -> int { return x * 2; }).filter(fn(x) -> bool { return x > 5; }); print(len(s), len(m), xs.sum()); }"
run "fn r(n: int) -> int { let a = [1]; return a[n]; } fn main() { var c = 0; for (i in 0..30000) { try { r(5); } catch (e) { c += 1; } } print(c); }"
run "fn main() { var t = 0; outer: for (i in 0..300) { for (j in 0..300) { if (j > i) { continue outer; } t += j; } } print(t); }"
run "fn main() { let p = alloc(8000) as *i32; var i = 0; while (i < $N) { p[i % 2000] = i; i += 1; } var s = 0; i = 0; while (i < $N) { s += p[i % 2000]; i += 1; } print(s); }"
run "struct H { a: u32, b: u16 } fn main() { let h = alloc(sizeof(H)) as *H; var i = 0; while (i < $N) { h->a = i; h->b = i & 255; i += 1; } print(h->a, h->b); }"
run "fn main() { let b = buffer(1024); var i = 0; while (i < $N) { b.put_u32((i % 256) * 4, i); i += 1; } var s = 0; i = 0; while (i < $N) { s += b.get_u32((i % 256) * 4); i += 1; } print(s); }"
run "fn main() { var i = 0; while (i < 60000) { let s = \"abc\" + i; free(s); let a = [i, i]; free(a); i += 1; } }"
for f in examples/*.kc; do "$K" "$f" >/dev/null 2>&1 < /dev/null || true; done
echo "pgo training complete"
