# C and B in KingCobra (v0.8)

KingCobra is a systems language, so it speaks the two languages systems programming grew out of: **B**
(Ken Thompson's typeless language) and **C**. v0.8 adds the control flow, operators, declarations,
standard library and preprocessor that C and B programmers reach for. Everything here is optional — the
KingCobra way of writing it (typed `fn`, `for (i in 0..n)`, `match`) is unchanged.

Runnable tours: [`examples/c_style.kc`](../examples/c_style.kc), [`examples/b_style.kc`](../examples/b_style.kc),
[`examples/preprocessor.kc`](../examples/preprocessor.kc).

## Control flow

```
do { ... } while (cond);                  // body first, then the test; `continue` goes to the test
for (var i = 0, j = 9; i < j; i++, j--) { }   // C-style: init; condition; step  (each part optional)
for (;;) { if (done) { break; } }
if (x) y++; else z++;                     // a body may be one statement without braces
while (n > 0) n--;
;                                         // the empty statement
switch (x) {
    case 1: ...; break;                   // C semantics: execution FALLS THROUGH until a `break`
    case 2, 3: ...; break;                // several values per case
    default: ...                          // may stand anywhere; taken when nothing matches
}
retry: ... goto retry;                    // labels are goto targets (and `break`/`continue` targets on loops)
```

* `switch` compares with `==` (ints, strings, enum members, any value). All cases share one scope; a
  plain `break` ends the switch; `continue` and `return` travel on to the enclosing loop / function.
* `goto` finds its label in the **same or an enclosing block** of the same function, so it can jump out
  of loops and nested blocks (the classic "cleanup" and "retry" uses). Jumping *into* a nested block is
  not supported and is reported as `goto: no label '…' reachable in this function`. A backward `goto`
  over a `let` declares it again, so prefer a loop for long-running repetition.
* The `C-style for` evaluates its declarations once; the loop variables are shared by all iterations (as in
  C), and `continue` runs the step. `for (i in …)` is unchanged and chosen when the header is `name in …`.

## Operators and expressions

| Feature | Notes |
|---|---|
| `++x  --x  x++  x--` | On any place: variables, `a[i]`, `s.field`, `*p`, `p->f`. Postfix yields the old value. On a pointer they step by the element size. Compile to the same code as `x += 1`. |
| `(a, b, c)` | Comma operator: evaluates all, yields the last. Inside `for` headers the comma separates several steps. |
| `[value; count]` | Array of `count` copies. The value is evaluated per element, so `[[0; 3]; 3]` gives three distinct rows. |
| `"ab" "cd"` | Adjacent string literals join. |
| `'ab'` | Multi-character constant: the bytes packed big-endian into an int (up to 8). |
| `let a = 1, b = 2;` | Several declarators (`var`, `let`, `const`). |

## Declarations

```
static count = 0;          // inside a function: initialised ONCE, keeps its value between calls
                           // (each closure value has its own; at file scope it is an ordinary variable)
typedef int myint;         // same as `type myint = int;`
union Bits { i: u32, f: f32 }   // every field at offset 0; sizeof = the largest field
```

**Unions live in memory.** Because their fields overlap, a union is used through a pointer to a buffer or
an `alloc` block: `let b = alloc(sizeof(Bits)) as *Bits; b->f = 1.5; print(b->i);`. A struct-style literal
`Bits { i: 1 }` is refused with that advice rather than silently giving independent fields.

## The preprocessor

A C-style text pass that runs before the lexer, on every file, import and REPL input. Lines starting with
`#` are directives; line numbers are preserved, so diagnostics stay accurate.

```
#define N 10                      #define SQ(x) ((x) * (x))
#define LOG(f, ...) printf(f, __VA_ARGS__)
#define STR(x) #x                 #define CAT(a, b) a##b          // stringize, paste
#undef N
#if defined(DEBUG) && LEVEL >= 2  ...  #elif ...  #else ...  #endif   #ifdef X   #ifndef X
#include "util.kc"                #include <name>                  #pragma once
#error message                    #warning message
```

* Predefined: `__FILE__ __LINE__ __COUNTER__ __KC__ __KC_VERSION__` and one of `__LINUX__ __WINDOWS__ __APPLE__`.
* `kingcobra -DNAME[=value] run file.kc` defines a macro from the command line.
* `#if` accepts integers, `defined`, `! ~ - + * / % << >> < <= > >= == != & ^ | && ||` and `?:`; names that
  are not macros are `0`.
* `#include "f"` looks next to the including file, then the current directory; `<f>` looks in each directory of
  `$KC_INCLUDE` (colon-separated) and `./include`. The included text is spliced **onto the directive's line**
  with its comments removed, so an error inside it reports the including line. `#pragma once` makes a second
  include a no-op. Nesting is limited to 32.
* Macros are not expanded inside strings, character literals or comments. Inside an `f"…{expr}…"` the
  `{expr}` is not expanded either. Arguments of a macro call may span lines, and a comma inside `()`, `[]` or
  `{}` does not split them (a small, deliberate difference from C). A self-referential macro stops
  expanding rather than looping.
* In the REPL the macro table persists across inputs; `:macros` lists it.

## `main`

```
fn main() { }                // as before
fn main(argv) { }            // argv: array of strings, argv[0] is the script
fn main(argc, argv) -> int { return 0; }   // an int result is the process exit status
```

`exit(code)` ends the process at once (stdout is flushed).

## The C standard library

Familiar names, KingCobra values. Strings are values, so functions C runs in place return a new string, and
"pointer into the string" results are **indices** (`-1` = not found).

| Header | Functions |
|---|---|
| `<stdio.h>` | `printf sprintf format eprintf puts putchar getchar fflush` |
| `<string.h>` | `strlen strcmp strncmp strcasecmp strchr strrchr strstr strcat strdup atoi atol atof strtol strtod itoa` |
| `<ctype.h>` | `isdigit isalpha isalnum isspace isupper islower ispunct isxdigit isprint iscntrl isgraph toupper tolower` (a code or a one-character string) |
| `<stdlib.h>` | `exit abort rand srand qsort bsearch time` |
| `<math.h>` | `floor ceil round trunc fabs pow fmod hypot atan2 sin cos tan asin acos atan exp log log2 log10 cbrt` |
| constants | `NULL EOF EXIT_SUCCESS EXIT_FAILURE RAND_MAX INT_MAX INT_MIN INT64_MAX INT64_MIN LLONG_MAX LLONG_MIN UINT32_MAX CHAR_BIT M_PI M_E` |

**`printf` conversions:** `%d %i %u %x %X %o %b %B %c %s %f %F %e %E %g %G %a %A %p %%`, flags `- + space # 0`,
width and precision (numbers or `*`), length modifiers `h l ll z j t` accepted and ignored (every integer is
64-bit). `%b` prints binary (`%#b` adds `0b`); `%c` takes a character code; `%s` takes **any** value;
`%x` of a negative number prints its 64-bit two's complement. Too few arguments, or an unknown conversion, is
an error (not undefined behaviour). `printf` returns the number of bytes written.

`qsort(array, cmp)` sorts **in place, stably** (merge sort); `cmp(a, b)` returns a negative, zero or positive
number. A comparator that raises leaves the array untouched. `bsearch(sorted, key, cmp)` returns an index or `-1`.
`rand` is deterministic (xorshift64*) until `srand(seed)`; `rand`, `srand`, `time`, `getchar`, `putchar`,
`printf`, `exit` and the other effectful functions are refused inside `comptime`.

## B

B has one type, the machine word, and a handful of conveniences. KingCobra's B support:

```
gcd(a, b) {                       // a function without `fn`; parameters untyped; may return anything
    auto t;                       // auto declares variables…
    while (b) { t = b; b = a % b; a = t; }
    return a;
}
main() {
    extrn putchar, printn;        // `extrn` checks the names exist (when the statement runs)
    auto v[10];                   // …and vectors: v has 11 words, indices 0..10, as in B
    auto s;  s = strdup("hello");
    lchar(s, 0, 'H');            // char(s, i) reads a byte, lchar(s, i, c) stores one
    printn(255, 16);  putchar(10);   // printn(n, base) prints a number; putnumb(n) in decimal
}
```

`getvec(n)` returns a vector of `n+1` zero words and `rlsevec(v)` frees it. `char`/`lchar` work on strings,
buffers (bytes) and vectors (words); a string **literal** is read-only, so `strdup` it first.
`printf` takes B's `%d %c %s %o` and more. A B-style function definition is recognised at the statement level as
`name ( names ) {` — unambiguous, since a call is never followed by `{`.

Not included: B's `=+`/`=-` old-style assignment operators (`a =- 1` already means `a = -1`),
its word-addressed strings terminated by `*e`, and the `*n`/`*t` escapes. They conflict with
KingCobra syntax that already has a meaning.

## Not (yet) from C

* Bit-fields, compound literals `(T){…}`, `goto` *into* a block, `volatile`/`register`/`restrict`.
* Implicit conversions between pointer and integer without `unsafe`: pointers keep their bounds and lifetime
  checks ([POINTERS.md](POINTERS.md)).
* `#include` of C headers, `#pragma` other than `once`, token-level `#if` on `sizeof`.
* The JIT does not compile the new statements (`++`, `switch`, `goto`, C `for`, …); functions using them run
  in the interpreter, which is where they are as fast as the rest of the language.
