# `kcc` — the KingCobra native compiler, written in KingCobra

`kcc` compiles KingCobra source into native machine code for **x86-64** and writes it as a standalone
**executable** (`.exe`, ELF, Mach-O), a relocatable object (`.o` / `.obj`) or a shared library (`.so`, `.dll`,
`.dylib`). The compiler itself is ordinary KingCobra (`kcc/src/*.kc`) and runs on the `kingcobra` interpreter.
It needs **no C compiler, assembler or linker**: instruction encoding, object files, executables and all three
shared-library formats are produced by the compiler itself.

```
kcc/kcc app.kc -o app.exe                  # Windows PE32+ executable  (target windows)
kcc/kcc app.kc -o app                      # executable for this OS    (ELF on Linux; --target picks another)
kcc/kcc app.kc -o app --target macos       # Mach-O executable
kcc/kcc input.kc -o out.so                 # ELF shared library        (target linux)
kcc/kcc input.kc -o out.dll                # PE32+ DLL                 (target windows)
kcc/kcc input.kc -o out.dylib              # Mach-O dylib              (target macos)
kcc/kcc input.kc -o out.o [--target T]     # object: ELF, COFF or Mach-O (default: this OS)
kingcobra kcc/src/main.kc input.kc -o out.so   # same thing without the wrapper
```
`-v/--verbose` (or `KCC_TRACE=1`) prints each stage (parse, check, codegen, write, save) so a crash shows where it happened.
The output extension picks the kind (`.exe` / `.elf` / `.out` / no extension → executable; `--exe` forces an executable for any
name; `--subsystem windows` makes a Windows GUI program without a console window); `--target linux|windows|macos` picks the OS/ABI for objects
(`linux`/`macos` use the System V ABI, `windows` the Win64 ABI). `import "other.kc";` pulls another file in.

## Pipeline

| Stage | File | |
|---|---|---|
| lexer, parser | `lexer.kc`, `parser.kc` | tokens → AST (KingCobra grammar, same precedence rules) |
| checker | `check.kc`, `types.kc` | names, types, struct layout, constant folding, diagnostics with `line:col` |
| code generator | `x64.kc`, `codegen.kc` | AST → x86-64 into a format-neutral module (text/rodata/data/bss, symbols, relocations) |
| object writers | `elf_obj.kc`, `coff_obj.kc`, `macho.kc` | `.o` / `.obj` |
| linkers | `elf_so.kc`, `pe_dll.kc`, `macho.kc` | `.so` / `.dll` / `.dylib` **and executables** from the same module |
| start-up code | `entry.kc` | the few bytes between the OS entry point and `fn main` (ELF, PE) |

Every function is exported unless declared `static fn`. Calls inside the module are resolved at compile
time; calls to `extern fn` go through a small `jmp [GOT/IAT slot]` stub (ELF GOT, PE IAT, Mach-O `__got`),
bound by the loader. `extern fn name(...) from "lib"` adds `DT_NEEDED` / an import / `LC_LOAD_DYLIB`;
`libc.so.6` ↔ `msvcrt.dll` ↔ `libSystem.B.dylib` are translated for the target.

## Executables

```
fn main(argc: int, argv: **u8) -> int { ... }      // or just  fn main() -> int  /  fn main()
```
`main` is the program's entry; its return value is the process exit code. Parameters are optional (`argc`, `argv`, and
`envp` as a third on Windows/Linux are passed per the platform ABI). See `kcc/examples/hello.kc` and `nolibc.kc`.

| Target | Format | Start-up | Libraries |
|---|---|---|---|
| Windows | PE32+ `.exe`, console or GUI, ASLR (`.reloc`), image base `0x140000000` | stub: `__getmainargs`, `main`, `exit` | imports through `msvcrt.dll` (default) or any `from "lib.dll"`; `msvcrt.dll` is always imported for start-up and `exit`, which flushes C stdio |
| Linux | ELF64 PIE with `PT_INTERP`/`PT_PHDR` (`/lib64/ld-linux-x86-64.so.2`) | libc present: standard crt1 sequence into `__libc_start_main`; no imports: `main` then the `exit` system call, **no libc loaded** | `DT_NEEDED` for every `from "lib"`; `libc.so.6` added when anything is imported |
| macOS | Mach-O `MH_EXECUTE`, `__PAGEZERO`, `LC_MAIN`, PIE | none: dyld calls `main` and exits with its result | `libSystem.B.dylib` always |

Unix executables are written with mode 0755. A program without a `fn main` is rejected (`an executable needs a fn main`).
Everything else (types, imports, globals, data relocations) works exactly as in libraries; in an executable nothing is exported.

## The native subset

`kcc` compiles every part of the KingCobra grammar that has a machine-code meaning without a runtime. As of
v1.0.0 that is nearly all of the *language*; what is left out is what is *library* (`std.*`) or *dynamic data*.

**Types.** `int`/`i64`, `i8 i16 i32 u8 u16 u32 u64 usize isize`, `bool`, `float`/`f64`, `f32` (memory type),
`*T`, `T[N]` (also `T[N][M]`), `struct` (C layout, `packed struct`), `union`, `enum`, `string` = `*u8`,
function pointers `fn(int, *u8) -> int`, `type Alias = ...;` and `typedef T Alias;`.

**Declarations.** `fn`, `static fn`, `extern fn … from "lib";`, `impl Type { fn m(self, …) }` (methods and
`Type.func()` statics, also `impl int`/`impl string`), generic functions `fn max<T>(a: T, b: T)` (monomorphised per
call, `T` inferred), default arguments, `let/var/const` (several names per statement), `static` locals, `enum` with
constant expressions, B-style `name(a, b) { … }` functions with `auto`/`extrn`, `comptime { const … }` blocks (constant
declarations only), local `fn/struct/enum/impl` declarations (hoisted), and `import "file.kc" as ns;` namespaces
(`ns.func()`, `ns.var`; types stay global). The **C preprocessor** (`#define` incl. function-like macros, `#if/#elif/#ifdef`,
`#include`, `-DNAME=val`) is the interpreter's own (`preprocess()` builtin), so both agree exactly.

**Statements.** `if/else`, `while`, `do/while`, C `for`, `for (i in lo..hi)`, `for (x in array)` and `for (i, x in array)`,
`switch` with fall-through, `match` (statement or value; integer/bool/enum/char/`null` patterns, `A | B`, `_`),
labels, `goto`, labeled `break`/`continue`, `defer`, `try`/`catch`/`throw`, destructuring
(`let [a, b] = arr;`, `let { x, y: z } = point;`), `++`/`--`, all compound assignments.

**Expressions.** every operator with KingCobra precedence, `?:`, `??` (pointers), `x in array`, `ch in "string"`,
assignment and `++/--` as expressions, comma `(a, b)`, `as`, `&x`, `*p`, `p->f`, `s.f`, `a[i]`, scaled pointer
arithmetic, `sizeof alignof offsetof`, array literals `[1, 2, 3]` / `[0; 100]`, struct literals `P { x: 1 }`
(initialisers for locals, globals and nested aggregates), non-capturing closures `fn(a: int) -> int { … }`,
function values (`let f = double;`, `&double`, `f(3)`, fields that hold functions), and the builtins
`sqrt abs print println char lchar`. `print`/`println` lower to `printf`, and common libc functions (`printf malloc free
memcpy strlen qsort sin pow …`, see `LIBC` in `check.kc`) are declared implicitly.

**Structs are values** (C semantics): `let b = a;`, `a = b`, passing and returning them copy. Struct arguments and
results follow the real SysV and Microsoft x64 ABIs (register classes, memory class, hidden return pointer), so they
interoperate with C.

**Native semantics, not interpreter semantics.** Integer arithmetic is 64-bit two's complement; `/` on integers is
truncating division; `u64` uses unsigned division/comparison/shift; narrow integers are truncated when stored and
extended when loaded; falling off the end of a non-void function returns 0; division by zero traps.
`throw` carries one 64-bit value (integer or pointer); the catch variable is an `int`. `defer` runs at the end of its
block, on `return`/`break`/`continue`, and while a `throw` unwinds (frames are walked through a handler chain stored in
the frame, no unwind tables needed). An uncaught `throw` prints `uncaught exception` and exits with 70 on Linux
(traps elsewhere). A value `match` with no matching arm and no `_` yields 0. `goto` must not jump out of a block that
has a pending `defer`/`try`.

**Still rejected, with a message that says why:** `import std.*` (library code), f-strings, slices `a[i..j]`,
string/array/object values that allocate (`{ a: 1 }`, `[]`, string methods, `+` on strings), variadic *definitions*
(`...rest`), closures that capture variables (pass them as parameters), `unsafe { }` as an expression, float patterns and
string patterns in `match`, `f32` parameters/returns, taking the address of an `extern fn`, generics with explicit
`<T>` arguments at the call, and generic *types*.

## Verification (what was actually run)

`kcc/tests/verify.sh` builds `kcc/examples/showcase.kc` and `features.kc` for every target and checks them
with independent tools:

| Output | Check |
|---|---|
| ELF `.o` | linked by `gcc`, executed from a C harness; all results compared |
| ELF `.so` (native linker) | loaded by Python `ctypes` and linked against by `gcc`; exported data, imports (`printf`, `strlen`) and 9-argument stack calls verified |
| COFF `.obj` | linked into a DLL by `x86_64-w64-mingw32-gcc` |
| PE `.dll` (native linker) | **loaded and called under wine**: results, `printf` through `msvcrt.dll`, exported variable, base relocations |
| Mach-O `.o` / `.dylib` | parsed by LIEF (exports, binds, load commands). **Not executed**: no macOS machine was available — treat the dylib writer as structurally validated, not run-tested |
| ELF executable | **run directly**: output, `argc`/`argv`, exit code, stdout flushed when piped; libc-free variant runs and has no `DT_NEEDED` |
| PE `.exe` | loaded by a miniature PE loader (`tests/harness_exe_win.c`) at a *non-preferred* base so base relocations are applied, imports bound to Win64-ABI shims, then entered at its real entry point: output, `argc`/`argv` and exit code verified. Also run under **wine** when installed. **Not yet run on real Windows** |
| Mach-O executable | parsed by LIEF (`MH_EXECUTE`, `LC_MAIN`, segments, binds). **Not executed** (no macOS machine): structurally validated only |

`kingcobra test kcc/tests/kcc_test.kc` covers diagnostics, layout, folding, determinism and a compile-then-`extern`-call round trip.
`kcc/tests/grammar/g*.kc` are whole programs for the language additions: each is compiled, **run**, and compared with a
golden `.expected` file (g01, g02, g05 were also checked line-by-line against the interpreter; where native semantics differ
on purpose — struct copies — the golden file records the native result). `kcc/tests/abi/run.sh` checks struct-by-value
calls in both directions against `gcc` for the SysV ABI, and for the Microsoft x64 ABI through gcc's `ms_abi`
(`KCC_FORCE_ABI=win64` makes the ELF backend use that convention, for testing).

## Known limits

Executables are x86-64 only and dynamically linked to the platform's C library/loader (no fully static binaries; a Linux
program must run where `/lib64/ld-linux-x86-64.so.2` exists). No resources/icon/manifest embedding for `.exe` yet.
x86-64 only (no AArch64 — Apple Silicon runs the x86-64 dylib under Rosetta; sign with `codesign -s -` if required).
No optimiser beyond constant folding and compare-and-branch fusion; code is a straightforward stack-slot design.
No SEH unwind tables on Windows (exceptions thrown through KingCobra frames cannot be unwound) and no debug info.

## Freestanding target: `kcos`

`kcc kernel.kc -o kernel.bin --target kcos` produces a **flat binary** with no loader, libc or OS underneath, for bare-metal
x86-64 code such as the [KCOS](KCOS.md) kernel. Options: `--base ADDR` (physical load address, default `0x10000`),
`--stack ADDR` (initial `rsp`, default `0x80000`), `--entry NAME` (entry function, default `kmain`), `--map` (also write `kernel.bin.map` with every symbol's address).
The code runs in 64-bit long mode with the first 4 GiB identity-mapped (that is what the KCOS boot stage provides).

Image layout: entry stub at `base` (`cli; cld; mov rsp, <stack>; call kmain`, then a halt loop), 256 interrupt stubs of 16 bytes
(`__kc_isr_base`), the shared interrupt path, then `.text`, `.rodata`, `.data`, and `.bss` stored as zeros (so nothing has to clear it).
The program must define

```
fn kmain() { ... }                 // called once, on a fresh stack
fn kc_interrupt(frame: *u64) { }   // every exception and interrupt lands here
```
`frame` points at the saved registers: `frame[0..14]` = r15 r14 r13 r12 r11 r10 r9 r8 rdi rsi rbp rbx rdx rcx rax, `[15]` = vector,
`[16]` = error code (0 for vectors that have none), `[17]` = rip, `[18]` = cs, `[19]` = rflags, `[20]` = rsp, `[21]` = ss.
The stub saves the SSE state with `fxsave`, so interrupt handlers may use floats. Imports (`extern fn`) are rejected: nothing exists to link against.

**Intrinsics** (compiled inline; available in every kcc target, but privileged ones only make sense in ring 0):

| | |
|---|---|
| `inb(port)` `inw` `inl` → int | read an I/O port |
| `outb(port, value)` `outw` `outl` | write an I/O port |
| `cli()` `sti()` `hlt()` `pause()` `int3()` | interrupt flag, halt, spin hint, breakpoint |
| `lidt(p)` `lgdt(p)` `ltr(sel)` | load IDT / GDT from a 10-byte descriptor at `p`; load the task register |
| `read_cr0()` `read_cr2()` `read_cr3()` → int, `write_cr3(v)`, `invlpg(addr)` | control registers, TLB flush |
| `rdtsc()` → int | time-stamp counter |
| `cpuid(leaf, out: *u32)` | `out[0..3]` = eax ebx ecx edx |
| `isr_stub(n)` → int | address of the interrupt entry stub for vector `n` (for building IDT gates) |
| `rdmsr(msr)` → int, `wrmsr(msr, v)` | model-specific registers |
| `read_cr4()` → int, `write_cr0(v)`, `write_cr4(v)`, `read_rflags()`, `read_rsp()` | more control state |
| `mfence()` `lfence()` `sfence()` `wbinvd()` `ud2()` | barriers, cache flush, guaranteed #UD |
| `popcnt(x)` `bsf(x)` `bsr(x)` → int | bit count; index of lowest / highest set bit (`-1` for 0) |
| `atomic_xchg(p: *int, v)` `atomic_add(p: *int, v)` → old value | locked exchange / fetch-and-add (spinlocks, counters, SMP-safe) |
| `atomic_cas(p: *int, expected, new)` → old value | locked compare-and-swap; it worked when the result equals `expected` |
| `insw_rep(port, buf, count)` `outsw_rep(port, buf, count)` | `rep insw` / `rep outsw`: 16-bit string I/O (ATA PIO disks) |
| `fnaddr(name)` → int | address of a function defined in this program (kcc has no function pointers) |
| `callp(addr, a1 .. a6)` → int | call a function by address, System V register arguments (callbacks, handler tables, thread entry points) |

Memory-mapped I/O is just pointer access (`*u16` at `0xB8000`); kcc has no optimiser, so every load and store is emitted exactly as written.

**Linker-provided runtime** (declare with `extern fn`; resolved by the kcos linker, no library involved):

```
extern fn kc_switch(old_sp: *int, new_sp: int);   // save rbp rbx r12-r15, store rsp at *old_sp, load new_sp, restore, ret
extern fn kc_reload_cs(selector: int);            // load CS (far return), e.g. after replacing the GDT
```
`kc_switch` is the whole context switch: a new thread's stack is prepared as six zero words (r15 r14 r13 r12 rbx rbp) followed by the
address its first `ret` should reach (see `kernel/sched.kc`).

**Page-aligned globals.** In the kcos target a global whose size is a multiple of 4096 bytes is placed on a 4096-byte boundary
(`var pml4: u64[512];` is a valid page table), and `.data` / `.bss` start on page boundaries.
