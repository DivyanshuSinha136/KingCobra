# Pointers, buffers and offsets (v0.5)

KingCobra has C-style pointers, byte buffers and struct layouts at the
language level — `*p`, `&x`, `p->field`, `p + n`, `p as *u32`,
`sizeof(T)`, `offsetof(S, f)` — but with a memory model that turns the
classic C disasters (out-of-bounds, use-after-free, double free, null
dereference) into immediate, precise errors instead of undefined behaviour.

Raw machine addresses (from FFI, or forged from an integer) are still
possible. They just require `unsafe { }`, like everything else in
KingCobra that can corrupt memory.

Runnable tour: `examples/pointers.kc`.

## The model in one paragraph

Memory comes from **regions**: `alloc(n)`, `buffer(n)`, or the bytes of a
string. A region knows its size and whether it is still alive. A
**pointer** into a region is *(region, byte offset, accessible window)*.
Every load and store checks that the access is inside the window and that
the region has not been freed — so safe code can never read or write memory
it does not own. Pointer arithmetic is unrestricted (like C, you can step
one past the end); the check happens when you dereference.

## Pointer kinds

| Kind | Made by | Points at | Checked? |
|---|---|---|---|
| region pointer | `alloc`, `buffer.ptr`, `"str" as *u8`, casts of those | raw bytes | bounds + liveness, no `unsafe` needed |
| raw pointer | FFI results, `ptr_from_addr`, `int as *T` | a machine address | **not checked**: dereferencing needs `unsafe { }` |
| variable pointer | `&x` | a KingCobra variable | variable must exist; `&` of a `let` is read-only |
| element pointer | `&arr[i]` | an array element | index re-checked on every use |
| field pointer | `&s.f` | a struct field | field must still exist |

`null` is the null pointer. `p == null`, `p != null` and `if (p)` work as in C.
Dereferencing `null` is the error `null pointer dereference`.

Variable / element / field pointers hold KingCobra *values* (any type), so
`*p += 1` works on whatever the variable holds; they support `+`/`-` only
when they point into an array (`&arr[i] + 1` is `&arr[i+1]`).

## Types

Machine types for memory: `u8 i8 u16 i16 u32 i32 u64 i64 f32 f64 bool`,
plus `int` (= `i64`), `float` (= `f64`), `usize` (= `u64`), `isize` (= `i64`),
pointers `*T` (nestable: `**u8`, `*void`), declared structs, and
fixed arrays `T[N]` inside structs. `*void` is C's `void*`: byte-sized
arithmetic, not dereferenceable until cast.

`u64` values above 2^63 appear as negative `int`s (there is one 64-bit
integer type in the value model); use `>>>` for logical shifts.

## Operators

```
&place            address of a variable, array element, struct field or *p
*ptr              load / store through a pointer          *p = v;  *p += 1;
ptr->field        field of the struct a pointer points at  p->len = 5;
ptr[i]            *(ptr + i)                               p[3] = 9;
ptr + n  ptr - n  step by n ELEMENTS (scaled by sizeof(T))
ptr - ptr         distance in elements (same allocation only)
< <= > >=         order pointers into the same allocation
expr as Type      cast (below)
sizeof(T)  alignof(T)  offsetof(Struct, field)
```

### Bitwise operators

`& | ^ ~ << >> >>>` and `&= |= ^= <<= >>=`. `>>` is arithmetic, `>>>` is
logical. A shift count outside `0..63` is an error. **Precedence follows
Rust, not C**: `& ^ |` bind *tighter* than `==`/`<`, so `flags & MASK == 0`
means `(flags & MASK) == 0`. Low to high:
`||` → `&&` → `== !=` → `< <= > >=` → `|` → `^` → `&` → `<< >> >>>` →
`+ -` → `* / %` → `as` → unary (`- ! ~ & *`) → postfix.
Binary literals: `0b1010`.

### Casts

| `x as T` | result |
|---|---|
| int-like → `u8`…`i64`, `bool` | wraps like a C cast: `300 as u8` is 44, `-1 as u32` is 4294967295 |
| float → integer type | truncates toward zero; NaN/out-of-range is an error |
| number → `f32` / `f64` | converts (`f32` rounds to single precision) |
| pointer → pointer | reinterpretation; keeps the allocation and its bounds |
| buffer / string → `*T` | pointer to the start (strings are read-only) |
| pointer → int | the machine address (not allowed in `comptime`) |
| int → pointer | forges an address: **`unsafe` required** |

Stores truncate like C (`*p = 300` into a `u8` stores 44), but storing a
*float* into an integer location is an error — cast explicitly.

## Struct layout (offsets)

A `struct` whose field types are all machine types has a C layout:

```
struct Header { magic: u32, kind: u8, len: u16, id: u64 }
sizeof(Header)         // 16   (natural alignment, trailing padding)
offsetof(Header, len)  // 6
packed struct Wire { magic: u32, kind: u8, len: u16, id: u64 }
sizeof(Wire)           // 15   (no padding, alignment 1)
```

Field types may be scalars, pointers (`next: *Node`, also to itself),
other structs by value, or fixed arrays (`tag: u8[4]`, `pts: Pt[3]`).
`string`, `fn`, `T[]` (dynamic) have no layout and are an error *only when*
you ask for one (`sizeof`, `alloc` + `->`); such structs still work as
ordinary KingCobra values.

`let h = alloc(sizeof(Header)) as *Header;` then `h->len = 5`,
`h->id += 1`, `(*h).kind`, `h[2].len` (array of structs), `p->pts[1].x`,
`p->tag[0]`. Reading `*h` copies the struct out as a normal value.
An array field decays to a pointer limited to *that field*:
`p->tag[4]` on `u8[4]` is out of bounds even though the next byte exists.
A pointer to a scalar field is not narrowed (so `container_of`-style
arithmetic with `offsetof` works, as in C).

A pointer stored *inside* memory is kept as an address; when it is loaded
back and the address lies in a live KingCobra allocation it becomes a fully
checked pointer again, otherwise it is a raw pointer (deref needs `unsafe`).

## Allocation

| call | meaning |
|---|---|
| `alloc(n)` | `n` zeroed bytes → `*u8` |
| `alloc_array("T", count)` | `count` zeroed `T`s → `*T` (e.g. `"i32"`, `"Header"`) |
| `realloc(p, n)` | resize; new bytes are zero. Other pointers into the same block stay valid and are re-checked against the new size |
| `free(p)` | `p` must be the start of an `alloc`. Double free and freeing an interior pointer are errors. `free(null)` is a no-op |
| `memcpy(dst, src, n)` | byte copy; **overlap is an error** (use `memmove`) |
| `memmove`, `memset(p, byte, n)`, `memcmp(a, b, n)` | as in C, bounds-checked. Accept pointers or buffers |
| `ptr_from_addr(addr, "*T")` | forge a pointer — `unsafe` only. An address inside a live allocation becomes a checked pointer |
| `align_up(n, a)`, `align_down(n, a)`, `is_aligned(x, a)` | `a` must be a power of two |

There is no garbage collector (by design): memory you `alloc` and never `free`
leaks, as in C — but `kingcobra --leaks` lists every unfreed block with the
line that allocated it, and `defer free(p);` makes cleanup automatic on every
exit path. `free` also works on strings, arrays, structs and buffers
(`b.free()` is equivalent for a buffer). See [MEMORY.md](MEMORY.md).

Pointer helpers: `p.addr`, `p.offset`, `p.limit` (bytes left in the
window), `p.elem_size`, `p.type`; `p.byte_add(n)`, `p.align_up(a)`,
`p.is_aligned(a)`, `p.read_bytes(n)`, `p.write_bytes([..])`,
`p.read_str(n)`, `p.write_str(s)`, `p.read_cstr([max])`.

## Buffers

`buffer(n)`, `buffer_from("text" | [bytes])`. A buffer is a region plus a
cursor, with **typed, endian-aware accessors** — little-endian by default,
`_be`/`_le` suffix to be explicit:

```
b.get_u32(off)  b.put_u16_be(off, v)  b.get_f64(off)      // random access
b.read_u8()     b.write_u32_be(v)     b.read_str(n)       // advance b.pos
b[i]  b[i] = v                                            // one byte
b.len  b.pos  b.remaining  b.ptr                          // properties
b.seek(n) b.skip(n) b.rewind()
b.slice(off, len)        // view sharing the same memory (and its bounds)
b.fill(byte [, off, len]) b.copy_from(src, src_off, dst_off, n)
b.to_string([off, len]) b.put_str(off, s) b.write_str(s) b.to_array() b.hex()
b.resize(n)  b.clone()  b.free()
```

Types for `get_`/`put_`/`read_`/`write_`: `u8 i8 u16 i16 u32 i32 u64 i64 f32 f64`.
`len(b)` works. `b.ptr` is a `*u8` limited to the buffer (or slice).

## Native code

Passing a pointer or buffer to an `extern` function or a native-import call
hands C the real address, so it requires `unsafe { }` (C does no bounds
checking). An extern declared `-> *T` returns a pointer:

```
extern fn malloc(n: int) -> *u8 from "libc.so.6";
extern fn memset(p: *u8, v: int, n: int) -> *u8 from "libc.so.6";
unsafe { let c = malloc(16); memset(c, 0x41, 15); c[15] = 0; print(c.read_cstr()); }
```

The returned pointer is raw: every dereference needs `unsafe`.

## `comptime`

`alloc`/`free`/buffers and checked loads/stores are deterministic and allowed.
Observing or forging an address is not: `.addr`, pointer↔int casts,
`ptr_from_addr` and raw dereferences are rejected inside `comptime { }`.

## Static checking

The type checker understands pointer types: `let p: *u8 = alloc(4) as *i32;`
is a compile-time error (cast explicitly); `*void` converts freely;
`p + n` keeps the pointer type, `p - q` is `int`, `*p` yields the pointee
type, `x as T` has type `T`. Sized integers (`u8`…`i64`) and floats
(`f32`/`f64`) are `int`/`float` to the checker — range is a run-time
(truncate-on-store) matter, as in C.

## Not here yet

- JIT: functions that use these operators run in the interpreter (the JIT
  compiles only its existing integer-arithmetic shape).
- Pointers to pointers *inside raw memory* are plain addresses (see above);
  function pointers, unions, bit-fields, and `volatile`.
- `std.memory` handles and `std.rhex` buffers are separate, older APIs; the
  new `buffer`/`alloc` do not replace them yet.
