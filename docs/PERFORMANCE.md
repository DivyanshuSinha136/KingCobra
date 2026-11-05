# Performance notes

## v0.7: keeping speed while adding a language

v0.7 added a great deal (new syntax, `try/catch`, a standard library, diagnostics) to the code that
runs for *every* statement, so the work was making it cost nothing. Measured with
`python3 tests/bench/compare.py OLD NEW`, which **interleaves** the two binaries (a, b, a, b, …) and
takes the best of several runs, because machine load drifts over a benchmark and a harness that
runs all of one binary and then all of the other reports the drift as a result.

| per loop iteration | v0.5.0 | v0.7.0 (`make pgo`) | speedup |
|---|---:|---:|---:|
| `while` loop (compare + `+=`) | 84 ns | 22 ns | 3.8× |
| `for i in range(n)` | 292 ns | 25 ns | 11.7× |
| user function call (interpreted) | 465 ns | 65 ns | 7.2× |
| user function call (JIT-eligible) | 157 ns | 51 ns | 3.1× |
| closure call | 147 ns | 55 ns | 2.7× |
| struct field read + write | 159 ns | 58 ns | 2.7× |
| array index read + write | 169 ns | 59 ns | 2.9× |
| `"ab" + str(i)` | 576 ns | 121 ns | 4.8× |
| `a.push(i)` | 132 ns | 51 ns | 2.6× |
| builtin `len(a)` | 222 ns | 49 ns | 4.5× |
| `if` / `else` with `%` | 184 ns | 49 ns | 3.8× |
| pointer `p[i]` load/store | 202 ns | 57 ns | 3.5× |

0.6.0 → 0.7.0 alone is 1.0–1.3× (builtin calls 1.3×) **with all the new features included**; a plain
`make` build is within a few percent of 0.6.0. Numbers are one machine's; compare ratios.

### What the regressions taught (each was found by measuring, then fixed)
1. **`sizeof(KcNode)` is a performance parameter.** Adding two fields and a `defaults` pointer grew every
   AST node from 88 to 104 bytes — the widest member of the node union sets the size of *all* nodes — and
   everything got ~10% slower from cache pressure alone. Per-kind scratch fields now share one union slot
   and the rare `defaults` array lives in it; the node is 88 bytes again.
2. **`free(NULL)` is still a call.** An unconditional `free(e->hidx)` in scope release cost a library
   call on every function call. It is now guarded by the (rare) condition.
3. **Don't test rare operators before common ones.** `&&`, `||` and `??` were three comparisons paid by
   every binary operator; adjacent enum values make it one unsigned range check.
4. **Big scopes need an index, small scopes don't.** Adding ~45 builtins made every builtin lookup scan
   them all. Scopes with more than 16 names now keep an open-addressed hash index (keyed by the interned
   pointer); small scopes stay a linear scan, which is faster for them. `len(a)` got faster than before.
5. **Method dispatch by integer id.** A built-in method name is resolved once per call site (cached in
   the AST node) and dispatched by `switch`, so adding methods does not slow existing ones.
6. **Common cases get a lean path.** Exact-arity calls bind arguments in a tight loop (defaults,
   variadics and arity errors live in an out-of-line function); `%` and `/` on ints are in the inline
   fast path; structural helpers are `noinline` so the hot recursive functions keep a small frame.

### `make pgo`
`make pgo` builds an instrumented interpreter, runs `tests/pgo_train.sh` (about 25 programs covering
loops, calls, closures, structs, arrays, strings, methods, sorting, `try/catch`, recursion, the JIT,
pointers and buffers), then rebuilds using the recorded profile. GCC then lays out and inlines the hot
interpreter paths from observed behavior: typically 10–25% over a plain `-O2` build, no source changes.
One rule: **train every family at the same weight.** Profile-guided builds treat whatever the training
under-exercises as cold and optimize it for size; a first training run that did only a light pointer
workload made pointer access 20% *slower* than a plain build until the weights were rebalanced.
The profile is machine-local and the plain `make` build is unaffected.

---

## v0.5.1 — the first optimization round

KingCobra's interpreter walks the syntax tree directly. v0.5.1 keeps that
design but removes the per-operation overhead that dominated it. Typical
operations went from ~80–420 ns to ~20–55 ns, string-building from
~460 ns to ~100 ns, with identical program output.

## Measured (single run on a 1-vCPU Linux sandbox, `-O2`, per loop iteration)

| operation | v0.5.0 | v0.5.1 | speedup |
|---|---:|---:|---:|
| `while` loop (compare + `+=`) | 77 ns | 20 ns | 3.8× |
| `for i in range(n)` | 283 ns | 25 ns | 11× |
| user function call (interpreted) | 422 ns | 56 ns | 7.5× |
| user function call (JIT-eligible) | 127 ns | 54 ns | 2.4× |
| closure call | 134 ns | 41 ns | 3.3× |
| struct field read + write | 128 ns | 48 ns | 2.7× |
| array index read + write | 153 ns | 54 ns | 2.8× |
| `"ab" + str(i)` | 459 ns | 101 ns | 4.5× |
| `a.push(i)` | 118 ns | 44 ns | 2.7× |
| builtin `len(a)` | 197 ns | 47 ns | 4.2× |
| `if / else` | 167 ns | 46 ns | 3.6× |
| pointer `p[i]` load/store | 175 ns | 45 ns | 3.9× |

Absolute numbers depend on the CPU; the ratios are what to compare. Reproduce
on your machine (needs python3; it asserts both builds print identical output):

```
python3 tests/bench/compare.py ./old/kingcobra ./kingcobra   # before/after
python3 tests/bench/compare.py ./kingcobra                   # one build
```

A "per operation" figure is only meaningful with the operation named. In
v0.5.0 the cheapest loop iteration cost ~77 ns while string building cost
~460 ns–1 µs, so a single "1.2 µs" figure most likely reflects a
string- or allocation-heavy path (or a slower machine) — those were the
slowest paths and are now the ones that improved most in relative terms.

## What was slow

Profiling by call count on a mixed workload showed the cost was almost all
bookkeeping, not arithmetic:

- every `{ }` block and every loop iteration `malloc`ed a scope, plus a
  second `malloc` for its variable table;
- variable lookup was a `strcmp` per entry along the scope chain, and every
  `define` `strdup`ed the name;
- every call `malloc`/`free`d its argument array and `malloc`ed a frame;
- every string literal was re-allocated each time it was evaluated;
- `kc_int`, `kc_bool`, … were out-of-line calls across translation units;
- `a + b` on strings round-tripped both operands through heap-formatted text.

## What changed (C-level techniques)

1. **Interned names.** Every identifier maps to one canonical `char*`
   (`kc_intern`), so a variable lookup is a pointer compare, never a `strcmp`.
2. **Pooled scopes with inline storage.** A scope is a free-list pop with its
   first 6 variables stored inside it: no `malloc`, no second allocation.
   A scope is recycled when its owner finishes with it *unless something
   captured it*. Capture (a closure, or a `&variable` pointer) is tracked
   dynamically with a flag, which makes recycling exact: closures and
   pointers keep their scope alive; everything else is reused immediately.
3. **Scope elision.** A block that binds no names (`if` bodies, most loop
   bodies) runs directly in its parent's scope. Function bodies and loop
   bodies run in the frame/iteration scope itself (a later `let` shadows
   exactly as a nested scope would). The loop scope is reused every iteration.
4. **Stack arguments.** Call arguments live in a stack array (heap only past 8).
5. **Shared string literals.** Strings are immutable, so a literal is built
   once and cached on its syntax node.
6. **Size-class heap for strings.** Strings come from a segregated free-list
   heap (a free-list pop, or a pointer bump in a 1 MiB chunk) instead of
   `malloc`; header and bytes are one block, and `free(s)` returns the block
   for reuse (see `docs/MEMORY.md`).
7. **Inlined hot paths.** Value constructors are `static inline`; `int op int`
   and `array[int]` are handled in place; a variable or int-literal operand is
   resolved at the call site without a recursive `eval_expr` call; struct
   field names are compared by pointer first.
8. **Small hot function.** The heavy cases (`call`, `assign`) were moved out
   of line so the central dispatch function stays small.
9. **No-allocation pointers.** `p[i]` and `p->field` build their address in a
   stack temporary instead of allocating a pointer object.

## Safety of the recycling

Scope reuse is the part that could silently corrupt a program if it were
wrong, so it is tested hard: `tests/smoke_test.sh` has dedicated cases for
closures in loops, escaping closures, retained `&var` pointers, shadowing,
and `break`/`continue`/`return` paths. Build with `-DKC_ENV_NOPOOL` to turn
pooling off and make every scope a real `malloc`/`free`; under AddressSanitizer
that turns any use of a recycled scope into an immediate error. The whole
suite passes that way (the `std.rhex` CPUID tests are excluded only because
their inline assembly crashes AddressSanitizer itself, with or without this
work).

```
make OPT="-O1 -g -fsanitize=address,undefined -DKC_ENV_NOPOOL" LIBS="-lm -ldl -lX11 -fsanitize=address,undefined"
```

The interpreter is single-threaded; the intern table, scope pool and string
arena are process-global by design.

## What did not change

- The JIT (x86-64, integer-only functions) is untouched; recursion such as
  `fib` was already ~8 ns per call there.
- There is no GC; memory is released by explicit `free` (v0.6, `docs/MEMORY.md`).
  Programs that never free pay nothing for the checks that make `free` safe.
- Language semantics and error messages are unchanged; one deliberate
  difference: `"a" + b` now keeps embedded NUL bytes in strings that
  contain them instead of truncating at the first one.

## Where the remaining time goes / next steps

A tree walker pays a function call per syntax node (~3–4 ns). Going
meaningfully below ~20 ns per simple statement needs a different execution
model, in rough order of payoff:

- compile the tree to compact bytecode with slot-resolved locals and
  computed-goto dispatch (removes name lookup and most calls entirely);
- word-sized tagged values (NaN-boxing: 8 bytes instead of 16, as in B's
  single machine-word type) to halve value traffic;
- cache builtin lookups (`print`, `len`, `str`) per call site with an epoch
  that invalidates when a global shadows them;
- profile-guided optimization (`-fprofile-use`) for the build.
