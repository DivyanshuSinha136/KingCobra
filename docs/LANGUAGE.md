# KingCobra Language Reference (v0.8)

Companion documents: [GRAMMAR.md](GRAMMAR.md) (formal syntax), [STABILITY.md](STABILITY.md) (what
may change), [MEMORY.md](MEMORY.md), [POINTERS.md](POINTERS.md), [REPL.md](REPL.md),
[PERFORMANCE.md](PERFORMANCE.md).

## Lexical elements
- Comments: `// line` and `/* block */` (an unterminated block comment is an error).
- Identifiers: `[A-Za-z_][A-Za-z0-9_]*`.
- Integers are **exact 64-bit**: `42`, `1_000_000` (`_` separates digits), `0xFF`, `0b1010`,
  `0o17`. A decimal literal above `9223372036854775807` is an error (write a float, `1e19`);
  `-9223372036854775808` is valid. Hex/binary/octal literals may use all 64 bits
  (`0xFFFFFFFFFFFFFFFF` is the bit pattern `-1`, as in C).
- Floats: `3.14`, `1e9`, `2.5E-3`.
- Characters: `'a'`, `'\n'`, `'\x41'`, `'\u{e9}'`, `'é'` — each is an **int** holding the Unicode
  code point (`'a' == 97`).
- Strings: `"double quoted"` (may span lines). Escapes: `\n \t \r \0 \\ \" \'`,
  `\xHH` (a raw byte), `\u{1F600}` (a code point, stored as UTF-8). An unterminated string is
  an error. Braces in a plain string are literal: `"{\"a\": 1}"` is just text.
- **f-strings**: `f"x = {x}, next = {x + 1}"` — any expression inside `{...}`, `{{` and `}}`
  for literal braces. Values are formatted as `print` would. (Strings *without* the `f` prefix
  never interpolate, so existing text with braces — JSON, templates — is unaffected.)
- Booleans `true`/`false`; `null`.

## Declarations
```
let name: type = expr;         // immutable binding (the type annotation is optional)
var name: type = expr;         // mutable binding
const NAME = expr;             // same as let; reads as intent
let [a, _, ...rest] = array;   // destructuring: `_` skips, `...rest` collects the remainder
let {x, y: renamed} = object;  // by field name (open objects: a missing key is null)

fn name(p1: t1, p2: t2 = default, ...rest: t3) -> rettype { ... }
fn id<T>(x: T) -> T { return x; }          // generics: T is erased, it type-checks as `any`

struct Name { field1: type1, field2: type2 }
enum Color { Red, Green = 5, Blue }        // Color.Red == 0, Color.Green == 5, Color.Blue == 6
impl Name { fn method(self, a: int) -> int { ... }   fn make() -> Name { ... } }
type Score = int;                          // alias, resolved while parsing (declare before use)

import std.module;  import std.module as alias;
import "path/to/file.kc";  import "path/to/file.kc" as alias;
import "path/to/lib.o" as alias;  import "path/to/lib.so" as alias;     // native import
extern fn symbol(t1, t2) -> rettype from "libname.so";
```
- **Parameters.** A default is evaluated *at each call*, in the callee's scope, so it may use
  earlier parameters (`fn area(w: int, h: int = w)`). A parameter without a default may not follow
  one with a default. `...rest` must be last and arrives as an array (a typed `...xs: int` makes
  it an `int[]`). Calling with too few or too many arguments is an **error** at run time
  (and at compile time when the callee is a known function).
- **Enums** are int constants in an immutable object; use them in `match` (`Color.Red => ...`).
  To get a name back from a value, iterate the enum: `for (name, v in Color) { ... }`
  (`items(Color)` lists `[name, value]` pairs).
- **`impl`** attaches functions to a type. A function whose first parameter is named `self` is an
  instance method (`value.method(args)`); any other is called on the type (`Name.make()`).
  It works for structs *and* built-in types (`impl string { fn shout(self) ... }`,
  `impl array`, `impl int`). Resolution order for `x.name(...)`: a struct's own field, then a
  user `impl` method, then a built-in method.
- `type`, `enum`, `impl`, `const`, `try`, `throw` and `defer` are *contextual*: they are keywords
  only where this syntax applies, so variables and fields with those names still work.

## Statements
```
if (cond) { } else if (cond) { } else { }
while (cond) { }
for (i in 0..10) { }                     // integer range, half-open
for (x in array) { }  for (i, x in array) { }
for (ch in "text") { }  for (i, ch in "text") { }           // bytes, like s[i]
for (key in object) { }  for (key, value in object) { }
outer: for (...) { for (...) { if (c) { continue outer; } if (d) { break outer; } } }
return expr;   break;   continue;   break label;   continue label;
unsafe { }   comptime { }   defer statement;   defer { ... }
try { } catch (e) { }    try { } catch { }    throw expr;
match (x) { ... }        // as a statement it needs no trailing `;`
do { } while (cond);   for (var i = 0; i < n; i++) { }   switch (x) { case 1, 2: ...; break; default: ... }
goto label;   label: statement      static n = 0;   typedef int word;   union U { a: u8, b: u32 }
```
The C and B forms (fall-through `switch`, `goto`, `++`/`--`, `static`, the preprocessor, `printf`, B's `auto`/`extrn`)
are specified in [C_AND_B.md](C_AND_B.md).
A loop body that declares nothing shares its enclosing scope; `for` reuses one scope per loop
unless a closure or `&variable` captures it.

### Error handling
Every runtime error is catchable. Inside `catch (e)`, `e` is:
- for a runtime error: an object `{ message, kind: "runtime", file, line, col }`;
- for `throw v;`: the thrown value `v` itself (any value: string, number, object…);
- `error("text")` builds `{ message, kind: "error" }` for throwing.

```
try { risky(); } catch (e) { print("failed:", e.message); }
throw error("bad input");        // rethrow with `throw e;`
```
While an error unwinds, **every `defer` registered since the `try` runs, innermost first**, so
`defer free(p);` is safe against errors. Stack overflow (even in JIT-compiled recursion) is a
catchable error. An uncaught error prints the message with the source line and a caret, and
exits with status 65. Errors that `try` cannot catch: out-of-memory and a genuine crash in
native (`extern`) code.

## Expressions
Precedence, low to high:

| level | operators |
|---|---|
| assignment | `=  +=  -=  *=  /=  %=  &=  \|=  ^=  <<=  >>=` |
| conditional | `cond ? a : b` (right-associative) |
| null-coalescing | `a ?? b` (b evaluated only if a is `null`) |
| logical | `\|\|` then `&&` |
| equality / order | `== !=` then `< <= > >= in` |
| bitwise | `\|` then `^` then `&` then `<< >> >>>` (Rust's order: tighter than comparisons) |
| arithmetic | `+ -` then `* / %` |
| cast | `x as Type` |
| unary | `- ! ~ & *` |
| postfix | call `f(x)`, index `a[i]`, slice `a[lo..hi]`, member `a.b`, pointer member `p->b` |

- `/` of two ints gives an int when exact (`8 / 2 == 4`) and a float otherwise (`7 / 2 == 3.5`);
  `idiv(a, b)` is floor division; `%` follows C (the sign of the dividend). Division or modulo
  by zero is an error. `int(a / b)` truncates toward zero. `+ - *` on ints wrap around (two's complement).
  Arguments are evaluated left to right; in `obj.method(args)` the receiver comes first.
- **`==` compares by value**: arrays, structs and objects are equal when their contents are
  (object key order does not matter). Closures and handles compare by identity.
- `x in c` is true for an element of an array, a substring of a string, or a key/field of an
  object or struct.
- **Slicing** `a[lo..hi]` (half-open; either end may be omitted) copies an array or a string
  (byte offsets) and gives a zero-copy view of a buffer. Out-of-range bounds are an error, never
  silently clamped.
- Object literals: `{ name: "ada", "full name": "A. L.", n: 1 + 2 }` make an open object (the
  same kind `std.json` returns).
- Arrays `[1, 2, 3]`, indexed `a[i]` (strict bounds, no negative indices); `len(a)`.
- Closures: `fn(x: int) -> int { return x * 2; }` are first-class values.

Pointers, buffers, offsets, `sizeof`/`alignof`/`offsetof`, casts and `packed struct` are in
[POINTERS.md](POINTERS.md).

### Structs
Declare a type, then construct it with a literal:
```
struct Point { x: int, y: int }

fn main() {
    let p = Point { x: 3, y: 4 };
    print(p.x, p.y);
    p.y = 10;        // mutate a field
}
```
Rules:
- The struct type must be declared (with `struct Name { ... }`, executed
  before the literal is evaluated — top-level declarations run first)
  before any `Name { ... }` literal is used. An undeclared type name is
  an error, not a silently-created ad-hoc object.
- A literal must supply exactly the declared fields, by name; an unknown
  field name or a wrong field count is an error raised at the literal's
  evaluation. The static type checker (below) additionally checks each
  field *value*'s type against the declaration, at both the literal and
  later assignments.
- **Structs are reference types**, like arrays: `let b = a;` makes `b`
  an alias of the same underlying fields, not a copy. Mutating through
  either name is visible through both. Pass a struct into a function
  and mutations the function makes are visible to the caller — this is
  intentional and mirrors how arrays already behave.

### Open objects (`std.json`, `std.csv`)
`json.parse` and header-mode `csv.parse` return *objects*: structs whose
field set is open. They behave like any struct (reference type, `o.field`
read/assign for existing keys) with three additions, which apply **only**
to objects and not to declared structs:
```
o["key"]            // read; a missing key is null (o.key on a missing key is an error)
o["key"] = value;   // replace, or add a new key
len(o)              // number of keys
```
`o["x"]` on a *declared* struct reads/replaces an existing field and is an
error for unknown names. Keys keep insertion order. `typeof(o)` is `"object"`.

### match expressions
```
let label = match (n) {
    0 => "zero",
    1 | 2 | 3 => "few",              // alternatives share one arm
    Color.Red => "red",              // an Enum.Member (or any constant name) is compared with ==
    -1 => "minus one",
    _ => "many",
};
```
- Patterns: literals (int, float, string, bool, `null`, negative numbers), the wildcard `_`
  (at most one, and not combined with `|`), constant names, and `Name.Member` paths.
- Arms are checked top to bottom; the first pattern equal (`==`, so structural) to the subject wins.
- If nothing matches and there is no `_`, it is a runtime error, never a silent `null`.

### Methods on built-in types
Strings: `upper lower trim split contains index_of starts_with ends_with replace slice` and
`repeat(n) pad_left(width[, fill]) pad_right(width[, fill]) chars() lines()`.
Arrays: `push pop contains index_of slice join reverse` and
`map(f) filter(f) reduce(f[, init]) find(f) any(f) all(f) each(f) sort([cmp]) concat(other)
first() last() clear() insert(i, v) remove(i) extend(other) sum() min() max()`.
Objects and structs: `keys() values() items() has(key) get(key[, default]) remove(key) merge(other)`.

Callbacks that declare two or more parameters also receive the element's index.
`sort` is **stable** and in place; `sorted(a[, cmp])` returns a sorted copy. A comparator returns
a number (negative, zero, positive). Method calls resolve the method once per call site, so they
involve no string comparison.

## Documentation: `help()` and `///`
```
help()                 overview and the topic list          help("topics")   help("builtins")   help("keywords")
help(value)            a function's signature and doc, a module's functions, a struct, the type of anything else
help("name")           a builtin, keyword, method ("string.split", "array.map"), module ("std.json"), struct or topic
```
Document your own code with `///` lines directly above a `fn` or `struct` (or a B-style function); they are what
`help(name)` shows next to the signature:
```
/// Returns the area of a w x h rectangle.
fn area(w: float, h: float) -> float { return w * h; }
help(area);
```
A name that is not found gets suggestions (`help("sortd")` → did you mean 'sorted'?). Four slashes (`////`) is an
ordinary comment. In the REPL: `help`, `?name`, `:help name`; from the shell: `kingcobra help map`.

## Built-in functions
- Core: `print println len str int float typeof push range clock sqrt input help`.
- Checks and errors: `assert(cond[, msg])`, `assert_eq(actual, expected[, msg])`, `error(msg)`.
- Numbers: `abs min max clamp idiv`; text: `ord(s)` (code point of the first character),
  `chr(n)`.
- Collections (also as methods): `map filter reduce find any all each sum sorted zip keys values
  items has`.
- Memory: `free free_deep is_freed mem_stats mem_report mem_quarantine` ([MEMORY.md](MEMORY.md)).
- Low level: `alloc alloc_array realloc memcpy memmove memset memcmp buffer buffer_from
  ptr_from_addr align_up align_down is_aligned` ([POINTERS.md](POINTERS.md)).

A user definition with the same name shadows a built-in.

## Tooling
```
kingcobra file.kc                      run
kingcobra check a.kc b.kc              parse + type-check only (exit 65 if any file has errors)
kingcobra test [--filter text] [files] run every test_* function; default: test_*.kc / *_test.kc
                                       in ./ and tests/. Exit 1 on any failure.
kingcobra --leaks file.kc              report unfreed alloc()/buffer() blocks with their source line
kingcobra repl                         interactive session (docs/REPL.md)
```
A test file is ordinary KingCobra: define `fn test_name() { ... }` and use `assert`/`assert_eq`.
Each test runs contained: a failure (assertion, runtime error, `throw`, stack overflow) is
reported with its location, pending `defer`s still run, and the next test starts clean.
`tests/lang_test.kc` is the language's own test suite.

Error messages show the source line with a caret and suggest likely corrections:
```
f.kc:5:12: error: struct 'Point' has no field 'xx' (did you mean 'x'?)
    5 |     print(p.xx);
      |            ^
```

## Modules
Three kinds of `import`, distinguished by extension:
```
import std.system;                     // one of the 7 built-in std.* modules
import "mathutils.kc" as mu;           // a user KingCobra source file
import "mathutil.o" as mu;             // a native library (.o/.so/.dll/.dylib)
```

**File imports** (`.kc`) resolve the path relative to the *importing
file's own directory* first (falling back to the current working
directory), so it works no matter where the process was launched from.
The module's top-level `fn`/`let` bindings become `mu.name`; the module
runs in its own isolated scope — it can use the shared builtins
(`print`, `len`, ...) but can't see the importer's globals, and the
importer can't see anything the module didn't itself end up binding at
its own top level. No import-cycle detection yet. See
`examples/modules_demo.kc`.

**Native imports** (`.o`, `.so`, `.dll`, `.dylib`) open a compiled
library directly, with no `extern` declaration at all:
```
import "mathutil.o" as mu;
fn main() {
    print(mu.square(6));   // 36 — all-int args/return, inferred
    print(mu.half(7.0));   // 3.5 — first arg is a float, so double args/return
}
```
A `.o` is first linked into a shared library alongside itself (needs a
C compiler on `PATH`, and the `.o` must have been compiled with
`-fPIC`); `.so`/`.dll`/`.dylib` load as-is. Every call resolves the
symbol fresh with `dlsym` and infers its calling convention from the
*arguments you actually pass*: all int-like → everything is an int64,
including the return value; the first argument a float → everything
is a double. There's no declared signature, so this can guess wrong
for a function that, say, takes ints but returns a float — use a
declared `extern fn ... from "lib.so";` instead when you need that
precision (see `examples/std_ffi.kc`). See
`examples/native_import_demo.kc` and `examples/native/mathutil.c`.

## Standard library
See the table in the top-level README. Call pattern:
```
import std.system;
system.platform();
```

`std.io` is worth calling out specifically since it's the most
commonly needed one:
```
import std.io;

io.write_file("out.txt", "hello\n");
let content = io.read_file("out.txt");
let lines = io.lines("out.txt");     // string[], split on \n (CRLF-safe)
io.append_file("out.txt", "more\n");
io.exists("out.txt");                 // bool
io.delete("out.txt");                 // bool
let line = io.read_line();            // one line from stdin
```

## Static type checking
Runs automatically after parsing, before the program executes (skip it
with `kingcobra --no-typecheck run file.kc`). It checks:
- `let`/`var` initializers against an explicit type annotation
- call argument count and types, for calls to a named top-level function
- `return <expr>;` against the enclosing function's declared return type
- struct-literal and struct-field-assignment values against the
  struct's declared field types
- undefined variables and functions (`foo(1)` with no `foo`), with a
  "did you mean" hint; builtins, top-level declarations, import aliases
  and local scopes all count as defined
- `impl` method calls (`Type.make(args)`, `value.method(args)`): argument
  count and types, when the receiver's struct type is known statically

It's deliberately permissive, not exhaustive: anywhere a type can't be
determined statically — values from `std.*` modules, a closure stored
in a variable rather than called by name, anything from an imported
file, array element types — it's treated as the wildcard type `any`
and skipped, rather than guessing and risking a false error on a valid
program. `int` is accepted where `float` is expected (implicit
widening); nothing else is.

## comptime blocks
```
comptime {
    fn fib(n: int) -> int {
        if (n < 2) { return n; }
        return fib(n - 1) + fib(n - 2);
    }
    let FIB10 = fib(10);
}
fn main() { print(FIB10); }   // 55
```
Runs once, when execution reaches it. `let`/`fn` bindings made inside
land in the *enclosing* scope (not a nested one), so they're usable
afterward as ordinary values — that's the point of the feature.
Side-effecting natives are refused inside, with a specific error naming
which one: file/network/process I/O (`std.io` writes/deletes/reads a
line, all of `std.network`, every `std.http` call that touches the
network (the pure URL/encoding helpers are allowed), every `std.system` call that
runs a process, changes the filesystem or environment, or reads a
volatile value — `exec`/`run*`/`spawn`/`exit`/`sleep`/`setenv`/`mkdir`/
`time`/`random_*`/`mem_free`/…), raw machine code and real-hardware access (`std.rhex`'s
code-execution/MSR/port/cache entries, even inside `unsafe { }`), and
the non-deterministic CPU reads `std.rhex` exposes without needing
`unsafe` at all (`rhex.rdtsc`/`rdtscp`/`rdrand`/`rdseed` — a cycle
count or a hardware RNG draw is never a compile-time constant), plus
any `extern` or native-import call. Pure reads (`io.read_file`,
`io.exists`, `io.lines`, `system.getenv`/`platform`/`os`/`cwd`/`pid`,
the `system` path functions and metadata reads, `memory.*`,
the pure `music_note` computations (theory, `parse`, `render`, DSP, analysis), the pure `tts` computations
(`speak`, `phonemes`, `normalize`, `align`, … — but not `say`, `to_wav`, `save_wav` or lexicon file I/O), `rhex.cpuid`/`cpu_vendor`/`cpu_brand`/
`has_feature`) are allowed.

This is **not** general macro expansion or AST manipulation — it's a
purity-enforced "run this once and trust the result" block, not a
separate compilation phase (the interpreter has no such phase to run
it in). A `comptime` block inside a loop re-runs every iteration rather
than truly running once ever; there's no way to memoize an expression
across different calls with different arguments, because doing so
safely would need static guarantees this interpreter doesn't have (see
`docs/ROADMAP.md` for why that was deliberately left out rather than
shipped half-correct). See `examples/comptime.kc`.

## Safety model
Everything in KingCobra is memory-safe by default (bounds-checked arrays,
bounds-checked `std.memory` buffers, and bounds- and lifetime-checked
pointers/buffers: out-of-bounds, use-after-free, double free and null
dereference are errors, not undefined behaviour). Raw machine addresses,
forging a pointer from an integer, and handing pointers to native code
require `unsafe { }`. The deliberate exceptions are
`std.rhex`'s raw-machine-code and real-hardware entries (buffers, calling
them, MSRs, port I/O, cache control) — each refuses to run unless it's
inside an `unsafe { }` block, so the unsafety is always visible at the
call site, the same philosophy as Rust's `unsafe`. `std.rhex`'s CPU-
identification/timing/entropy reads (`cpuid`, `rdtsc`, `rdrand`, ...) do
not require it, by the same reasoning `std.mmio`'s `virtual()` regions
skip it: each is a bounded, side-effect-free instruction rather than
code execution or a real-hardware mutation. See `docs/STD_RHEX.md`.

## Memory

Memory is explicit — **there is no garbage collector, by design.** `free(x)`
releases any string, array, struct, object, buffer or `alloc()` block; a freed
value is a tombstone, and using it or freeing it twice is a clear error.
`defer free(x);` runs at block exit; `free_deep(x)` frees a whole structure;
`kingcobra --leaks file.kc` reports blocks that were never freed, with the
source position of each. See [MEMORY.md](MEMORY.md).

## What v0.8 does not have yet
Not in the language, by choice or not yet (see `docs/ROADMAP.md` for the reasons): destructuring
`match` patterns, negative array indices, operator overloading, user-defined generic *types*
(generic functions are erased to `any`), tagged enums with payloads, closures that capture by
copy, full type inference, multi-statement JIT bodies, mixed-type `extern` signatures. There is
deliberately no garbage collector (see [MEMORY.md](MEMORY.md)).
