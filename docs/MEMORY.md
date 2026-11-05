# Memory management (v0.6)

**KingCobra has no garbage collector, and will not get one.** Memory is
managed explicitly, like C — but with a checked model, so a mistake is a
precise error instead of silent corruption. You free what you create:

```
let s = "id-" + n;       // a new string
...
free(s);                  // its storage returns to the heap and is reused
```

Why no GC: it keeps timing deterministic (no pauses), matches the
pointer/buffer model the language already has, and keeps the runtime small.
What you get instead is `free` that works on every heap value, immediate
detection of misuse, `defer` for cleanup, and tooling to find leaks.

## What `free` accepts

| value | what happens |
|---|---|
| string, array, struct, object (`json.parse` result, …) | storage returned to the heap; the value becomes a **tombstone** |
| `alloc()` pointer, `alloc_array()` pointer | block released (must be the start of the allocation) |
| `buffer(n)` | released (a `slice` view cannot be freed) |
| `null` | no-op, as in C |
| string literal | **error** — literals are static data shared by every evaluation; copy first: `let s = "" + lit;` |
| numbers, bools, closures, handles | error naming the type |

`free` is shallow, like C. `free_deep(x)` frees a value and everything it
holds (nested arrays/structs/strings). It skips anything already freed and any
string literal, and it tolerates reference cycles, so aliases inside a
structure are harmless. Pointers/buffers inside a structure are *not* owned by
it and are left alone.

`free(a)` frees the object, not just one name: every alias of `a` now sees a
freed value. That is the point of the next section.

## Freed values are tombstones (use after free is an error)

After `free(x)`, `x` still exists but is marked freed. To code that does not
check it reads as empty (length 0); the interpreter checks and reports:

```
let a = [1, 2, 3];  let b = a;  free(a);
print(b[0]);         // error: use after free (this array was freed)
a.push(4);           // error: use after free (this array was freed)
for (x in a) { }     // error: use after free (this array was freed)
len(a); print(a);    // error: use after free ...
free(a);             // error: double free (this array was already freed)
```

Covered: indexing and index-assignment, field read/write, method calls
(receiver and arguments), `for` over an array, string `+`, and every native
function/builtin argument. `is_freed(x)` tests without erroring.

### Quarantine

A freed object's header is held back and **recycled only after
`mem_quarantine()` (default 4096) further frees**. So a stale reference to a
*recently* freed object is always detected rather than silently aliasing a new
object that happened to reuse its slot. Beyond that window a very old stale
reference could alias a recycled object — the same trade-off AddressSanitizer
makes. `mem_quarantine(n)` returns the previous size; `0` recycles
immediately (most memory-efficient, weakest detection). Memory held in
quarantine counts as freed in `mem_stats()` and is reported separately.

Region pointers and buffers (v0.5) keep their own stronger guarantee: their
descriptor is never recycled, so use-after-free on them is always detected.

## Reuse

Freed storage goes to a size-class free-list heap (16-byte classes up to 512
bytes; larger blocks use `malloc`). A loop that allocates and frees reaches a
steady state with no growth: 1,000,000 string allocate/free cycles reserve no
additional memory.

## Temporaries

A value with no name cannot be freed. `"id-" + str(n)` creates **two**
strings: the result, and the `str(n)` temporary. Prefer forms that do not
create temporaries — string `+` formats numbers directly (`"id-" + n`) — or
bind intermediate values to variables so you can free them:

```
let t = str(n);  let s = "id-" + t;  free(t);
```

The same applies to array/struct temporaries inside larger expressions.

## `defer`

`defer <statement>;` (or `defer { ... }`) runs when the **enclosing block**
exits — by reaching the end, `return`, `break` or `continue` — in reverse
order, after a pending `return` value has been computed:

```
fn checksum(n: int) -> int {
    let p = alloc(n);
    defer free(p);          // runs on every exit path
    memset(p, 1, n);
    return p[0] + p[n - 1]; // value computed first, THEN p is freed
}
```

The deferred statement runs in the block's scope and sees variables as they are
at exit (Zig semantics). A `defer` is registered when execution reaches it, so
an early return before it does not run it. Up to 32 per block; not allowed
outside a function/block. `defer` is a contextual keyword: a variable named
`defer` still works.

## Finding leaks

```
kingcobra --leaks program.kc      # report printed to stderr at normal exit
```
```
kingcobra: memory report
  leaked blocks (allocated, never freed):
        1024 bytes  alloc()       at server.kc:12:17
          64 bytes  buffer()      at server.kc:15:15
  total: 2 block(s), 1088 byte(s)
  still allocated: 3 string(s), 0 array(s), 1 struct/object(s)
  heap: 4096 byte(s) in use, 1048576 reserved, ...
```

Every `alloc`/`alloc_array`/`buffer` block records the source position that
created it. Strings, arrays and structs are counted (not sited): "still
allocated" is created minus freed (string literals are not counted).
In a program: `mem_report()` prints the same report; `mem_stats()` returns an
object with `strings_live`, `arrays_live`, `structs_live`, `*_freed`,
`regions_live`, `region_bytes_live`, `heap_bytes_in_use`,
`heap_bytes_reserved`, `heap_allocs`, `heap_frees`, `quarantined`,
`quarantine_bytes`.

## Limits and notes

- **Strings from std modules.** Strings built by the core, `std.json` and
  `std.io` own their bytes, so `free` reclaims everything. Strings built by
  some other modules reclaim their header only (the bytes are not released) —
  always memory-safe, just not fully reclaimed.
- **Closures and scopes** are never freed; a scope kept alive by a closure or
  `&variable` pointer lives for the whole run.
- **Handles** (`std.nn` tensors, files, sockets) keep their own `close`/`free`
  calls; `free()` does not apply to them.
- **No auto-free of temporaries and no scope-exit free** — that would need
  ownership tracking, which is deliberately out of scope. Use `defer`.
- **Recovered errors (the REPL).** An error that is caught instead of ending the
  program unwinds without running pending `defer`s and leaks the unwound frames'
  scopes (never corrupts them). See `docs/REPL.md`.
- Not thread-safe (the interpreter is single-threaded).
- **Cost.** Programs that never call `free` pay nothing measurable: the
  tombstone checks are skipped by a single flag until the first free.

## Debugging the runtime

Build with `-DKC_HEAP_NOPOOL -DKC_ENV_NOPOOL` to turn the allocator and scope
pools into plain `malloc`/`free`, so AddressSanitizer sees every block:

```
make OPT="-O1 -g -fsanitize=address,undefined -DKC_HEAP_NOPOOL -DKC_ENV_NOPOOL" \
     LIBS="-lm -ldl -lX11 -fsanitize=address,undefined"
```
