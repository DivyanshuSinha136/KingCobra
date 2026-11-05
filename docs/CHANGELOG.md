# Changelog

Format: *Added* / *Changed* (may need action from you) / *Fixed*. Behavior changes are always listed
under **Changed** with a migration note. See [STABILITY.md](STABILITY.md) for the policy.

## 1.0.0

**Fixed (release hardening)**
- `std.rhex` `cpuid` (behind `rhex.cpuid`, `rhex.cpu_vendor`, `rhex.cpu_brand`): the inline assembly declared its `%ebx` operand
  output-only and swapped it into `%ebx`, clobbering the callee-saved `rbx` register with an uninitialised value on
  every call. It only worked by luck of the register allocator; it crashed under AddressSanitizer. Now uses `<cpuid.h>`.
- `std.nlp`: the call position kept for error messages was a pointer to a by-value parameter that dies on return
  (dangling pointer, GCC `-Wdangling-pointer`). It now points at a static copy.
- `std.ascii_screen`: reading the sprite height of an entity whose sprite had no frames used an uninitialised variable
  (GCC `-Wmaybe-uninitialized`).
- Build: objects are now rebuilt when the compiler, flags or detected features (X11, readline) change, so installing
  `libx11-dev` / `libreadline-dev` after a first build can no longer leave a mix of differently-configured objects.
- Repository: removed a stray 3.6 MB Python wheel, a leftover `README.md.orig`, prebuilt `examples/native/mathutil.*`
  and `kcos/build/*` outputs from the tree; added `.gitignore` and `.gitattributes` (LF line endings for scripts).

**Added (distribution)**
- `install.sh` rewritten: detects Linux (glibc / musl) and macOS and the CPU (including Rosetta), installs `.deb` /
  `.rpm` / tarball, **compiles from the source release on the user's machine** when no prebuilt package exists or the
  download is missing, verifies SHA-256 (optionally the build-provenance attestation), and has `--uninstall`.
- `install.sh` knows microcomputers: shows the board model (Raspberry Pi, Orange Pi, BeagleBone, VisionFive, ...),
  compiles locally on ARMv6 boards (Pi Zero / Pi 1) and Android (Termux) instead of refusing, sizes the compile to the
  board's RAM and disk, can install build dependencies (`KC_INSTALL_DEPS=1`), and explains that bare-metal
  microcontrollers cannot run KingCobra.
- `install.ps1` for Windows (`irm ... | iex`), Scoop manifest, Homebrew formula (both rendered with real hashes at
  release time), RPM packaging (`make rpm`), man pages for `make install`, `make install-bin|install-man|install-docs`,
  `make dist-src`, `make check`, `make version`.
- GitHub Actions: CI and release now compile for Linux amd64/arm64/armhf/riscv64 (glibc), amd64/arm64 (musl),
  Fedora amd64/arm64, macOS arm64/amd64 and Windows x64; packages are verified by installing them in clean containers
  (Debian 12, Ubuntu 22.04/24.04, Fedora, Rocky 9, Alpine); releases get a source tarball, SHA256SUMS, a build-provenance
  attestation and notes taken from this file. `tools/check-release.sh` guards version/changelog/banner consistency.
**Added**
- **`kasm` — an x86 assembler written in KingCobra** (`kasm/`, docs in [KASM.md](KASM.md)). NASM-style Intel syntax for 16/32/64-bit
  code with a macro preprocessor (`%macro %define %if %rep %include`), sections, `struc`, expressions and automatic jump
  relaxation. Table-driven encoder covering the integer set, x87, SSE–SSE4.2, AES/PCLMUL, AVX/AVX2/F16C/FMA3 and BMI1/2 —
  checked byte-for-byte against GNU `as` on 817 instructions. Outputs: flat binary, Intel HEX, Linux ELF64 executable,
  linkable ELF64 object (works with `ld`/`gcc`), KingCobra/C arrays, listing and map. `kasm.run(src, args)` assembles and
  executes through `std.rhex`. `make test-kasm`.
- **KCOS — an operating system written in KingCobra** (`kcos/`, [KCOS.md](KCOS.md)). A bootable x86-64 BIOS disk image
  (`make -C kcos` → `kcos/build/kcos.img`): GDT/TSS, IDT with exception and IRQ stubs, PIC, 100 Hz PIT, PS/2 keyboard, serial
  console, E820 physical memory manager, kernel heap, an in-memory filesystem and a shell with 30 commands. The whole kernel is
  KingCobra compiled by `kcc`; only the 16/32-bit boot stage (`kcos/boot/boot.asm`, shipped pre-assembled) is assembly.
  Booted and checked in QEMU (`make -C kcos test`); not tried on real hardware.
- **`kcc --target kcos`: freestanding flat binaries.** `kcc kernel.kc -o kernel.bin --target kcos [--base ADDR] [--stack ADDR] [--map]`
  links a program with no OS, libc or loader. The compiler writes the entry stub and 256 interrupt stubs (register save,
  `fxsave`, call to `fn kc_interrupt(frame: *u64)`, `iretq`). New bare-metal intrinsics usable in any kcc program:
  `inb/inw/inl outb/outw/outl cli sti hlt pause int3 lidt lgdt ltr invlpg rdtsc read_cr0/2/3 write_cr3 cpuid isr_stub`.
  See [KCC.md](KCC.md#freestanding-target-kcos).
- **kcos, second step — what a larger kernel needs.** Atomics (`atomic_xchg/add/cas`), `rdmsr/wrmsr`, `read_cr4/write_cr0/write_cr4`,
  `read_rflags/read_rsp`, barriers, `popcnt/bsf/bsr`, `rep insw/outsw` string I/O, `fnaddr(fn)` and `callp(addr, args…)` for indirect calls
  without function pointers, linker-provided `kc_switch` (context switch) and `kc_reload_cs`, page-aligned globals, `--entry NAME`.
  KCOS uses them for preemptive kernel threads, an ATA PIO disk driver and a filesystem that survives reboots (`sync`).
- **`kcc` supports the whole native-meaningful grammar.** New: preprocessor (`#define/#if/#include`, `-D`), `switch`, `match`,
  labels/`goto`/labeled `break`/`continue`, `defer`, `try/catch/throw`, `impl` methods, generics (monomorphised), default
  arguments, function pointers and non-capturing closures, array/struct initialisers, struct copies and by-value
  arguments/results (SysV + Win64 ABIs), unions, `static` locals, destructuring, `for (x in array)`, `x in array`, `??`,
  `++/--` and assignment as expressions, comma operator, `comptime` constants, `import … as ns` namespaces, B-style
  functions (`auto`, `extrn`, `char`, `lchar`), `print`/`println` and implicit libc declarations. Contextual keywords
  (`type`, `enum`, `static`, `defer`, …) are ordinary names again. See [KCC.md](KCC.md).
- Builtins `preprocess(source, filename)` and `pp_define("NAME=value")` expose the interpreter's preprocessor to programs.
- **`kcc` builds executables.** `kcc app.kc -o app.exe` writes a Windows PE32+ program, `kcc app.kc -o app` a Linux ELF
  (PIE; libc-linked, or libc-free with raw syscalls when the program imports nothing), `kcc app.kc -o app --target macos`
  a Mach-O executable. The program starts at `fn main` (optionally `main(argc: int, argv: **u8)`; the return value is the exit
  code). New options: `--exe` (force an executable), `--subsystem console|windows` (Windows GUI programs). Executables get
  the execute bit on Unix. Still no C compiler, assembler or linker involved. See [KCC.md](KCC.md).
- `kcc -v` / `--verbose` (or `KCC_TRACE=1`) prints each stage so a crash shows where it happened.
- **`kingcobra debug FILE`** — a source-level debugger driven by an IDE over named pipes: line breakpoints, step over / into / out,
  pause, call stack, variables of any frame, expression evaluation and assignment, and an automatic stop on an uncaught runtime
  error. The closure JIT is switched off while debugging; normal runs are unaffected. See [DEBUGGER.md](DEBUGGER.md).
- **`kcide`: integrated Terminal, Console and Debugger.** A full terminal emulator (`Alt+T`), a KingCobra REPL console
  (`Alt+C`) that evaluates in the paused stack frame while debugging, and a Debug panel with call stack, variables and watches
  (`Alt+F5`, `F9`, `F10`, `F11`). Also highlighting, completion and snippets for `.kcm` (music scores and `kcmodel` 3D models) and
  `.ssm` / `.ssml` (SSML). Format Document moved to `Ctrl+Alt+L` and the menu bar to `Alt+M` (`F9`/`F10` are debugger keys).
  See [IDE.md](IDE.md).

## 0.10.0 — ARM64, ARMhf and RISC-V
**Added**
- **Architecture support: ARM64, ARMhf (32-bit), RISC-V 64/32.** They run on the interpreter (the closure JIT stays x86-64
  only, and turns itself off cleanly elsewhere). New `make arm64 | armhf | riscv64` cross targets, per-triple object
  directories, X11/readline detected through the target compiler. One architecture header (`kc_arch.h`) replaces the
  scattered `__x86_64__` checks. `info` shows architecture and word size. See docs/PLATFORMS.md.
- `rhex.arch()`; `std.rhex` raw-code execution on every supported CPU (`examples/std_rhex_arch.kc`), instruction-cache
  flush on seal, `fence`/`pause` everywhere. x86-only instructions give a clear error elsewhere.
- **Chocolatey package** (`choco install kingcobra`, Windows x64) and a MinGW Windows build in CI/release; `make LDFLAGS=...` is honoured.
- **GitHub Actions + `install.sh`:** CI on amd64/arm64 (native) and armhf/riscv64 (QEMU); tag-triggered releases that
  build all four architectures; a one-line installer that detects the CPU and installs the matching build.
- **Debian package and APT repository:** `make deb` builds `kingcobra_<version>_<arch>.deb` (debian/ directory, man page);
  `packaging/debian/make-apt-repo.sh` builds a (optionally GPG-signed) APT repository. See docs/PACKAGING.md.
- `kc_hw` barriers for ARMv7 and RISC-V (`dmb`, `fence`, Linux `readl`/`writel` ordering).

**Changed**
- **`int(float)` is now defined on every CPU:** truncate, saturate to the int64 range, NaN becomes 0. (On x86-64 an
  out-of-range value used to give `INT64_MIN`; ARM64/RISC-V hardware would have given a different answer.)
- FFI integer arguments/results are machine words (identical on 64-bit hosts; required for correct 32-bit ARM calls).
- Built with `-fsigned-char` and `-D_FILE_OFFSET_BITS=64`. `std.nlp` ASCII check no longer depends on `char` signedness.

## 0.9.3 — the JIT is optional
**Changed**
- **The closure JIT now checks that it can run before it compiles anything, and falls back to the interpreter when it cannot.**
  On the first hot function, one tiny self-test runs: is this an architecture the JIT emits code for, did you leave it enabled, does
  the OS hand out a writable page, will it make that page executable, and does an 11-byte stub actually execute and return the
  right value (on Linux/macOS under a SIGSEGV/SIGILL/SIGBUS guard, so a system that allows `mprotect` but still faults on
  execution degrades instead of dying). If any step fails — hardened kernels / SELinux `execmem`, PaX, locked-down containers and
  sandboxes, non-x86-64 CPUs — the JIT is simply off for the process and every function runs in the interpreter with identical
  results. Nothing to migrate: on a normal machine nothing changes. `kingcobra info` now says whether the JIT is usable and, when it
  is not, why.

**Added**
- **`--no-jit`** (and `KC_JIT=0` / `off` / `no` / `false` in the environment): never compile to machine code. Useful for A/B
  timing, for bisecting a suspected miscompile, and for locked-down hosts where you would rather not probe at all.

## 0.9.2 — fixes
**Fixed**
- **Stale objects can no longer be linked into a new build.** Object files now live in `build/<version>/` instead of next to the
  sources, so a new release always compiles everything fresh. A mix of old objects and new headers (for example after unpacking a
  zip over an existing tree whose timestamps look newer) shows up as garbage argument counts ("expects 0 to -2093761856
  argument(s)"), corrupted structs and silent crashes of the REPL — the symptom reported on Windows. If you hit it on an older tree,
  run `make clean` once (it also removes the old in-tree `*.o` files).
- A function written with a mistyped keyword — `let add = f(x: int, y: int) -> int { ... }`, `func(a) { ... }` — now says
  "use `fn`" instead of "expected ) but found ':'" / "expected ;".

## 0.9.1 — `help()`
**Added**
- **`help([thing])`**, Python-style: `help()` overview; `help(value)` documents a function (signature + doc), a module (its functions), a
  struct (fields, doc, methods) or the type of any other value; `help("name")` looks up a builtin, keyword, string/array/object
  method (`"string.split"`), module (`"std.json"`), struct, `Type.method`, or a topic (`"types"`, `"operators"`, `"memory"`,
  `"pointers"`, `"preprocessor"`, `"c"`, `"b"`, ...); unknown names get suggestions. Every builtin has an entry (a test enforces it).
- **`///` doc comments** above a `fn`, `struct` (or B-style function) are kept and shown by `help`. `////` stays an ordinary comment.
- REPL: `help`, `?name`, `:help name`; a `///` line waits for the declaration after it. CLI: `kingcobra help map` (any non-command word
  is looked up in the language help). `help` is a new builtin name (a user definition still shadows it).

## 0.9.0 — a mature command line
**Added**
- **`kingcobra` command line rewritten** (`docs/CLI.md`): real subcommands with per-command help (`help <command>`, `<command> --help`),
  `eval`/`-e` and `-p` for one-liners (arguments arrive as `argv`/`argc`), `run -` for a program on standard input,
  `preprocess`/`-E`, `new`/`init` project scaffolding, `completions bash|zsh|fish`, `info`, `modules`.
- **`check`** takes directories (recursive), `--quiet`, and `--format=json` (one diagnostic object per line, for editors and CI).
- **`test`** takes directories, `-k/--filter`, `-l/--list`, `-x/--fail-fast`, `-q/--quiet`, and ends with a `file::name`
  list of failures.
- **`-I DIR`**, **`--time`** (parse/check/run timings), **`--color=auto|always|never`** (diagnostics honour `NO_COLOR`/`FORCE_COLOR` and
  are plain when stderr is not a terminal).
- Mistyped commands and options get a "did you mean …?" instead of "cannot open"; a directory given to `run` is explained;
  file errors include the reason (`No such file or directory`).

**Fixed**
- `std.tts`: builds on MinGW and older C runtimes (a portable `strndup`), and no longer emits `-Wmisleading-indentation` warnings.

**Changed** (check these when upgrading)
- Global options are recognised **before the script path only**. Previously `--no-typecheck`, `--leaks` and `-D…` were stripped from
  anywhere on the command line, so a script could not receive them; now `kingcobra tool.kc --leaks` passes `--leaks` to `tool.kc`.
- Diagnostics are **no longer coloured when stderr is not a terminal** (colour used to be unconditional). Use `--color=always`.
- `kingcobra <word>` where `<word>` is not an existing file and has no `.` or `/` is an unknown-command error (exit 64), not "cannot open".
- `check` prints `N file(s) checked, all ok` after several clean files; unreadable inputs exit 66.

## 0.8.0 — C and B capabilities, a mature REPL
**Added**
- **C control flow and operators** — `do { } while`, C-style `for (init; cond; step)` (with the comma operator),
  `switch`/`case`/`default` with C fall-through, `goto` and labels, `++`/`--` (prefix and postfix, on any place,
  pointer-aware), single-statement bodies (`if (x) y++;`), the empty statement, `let a = 1, b = 2;`,
  `[value; count]`, adjacent string literals, multi-character constants `'ab'`. See [C_AND_B.md](C_AND_B.md).
- **C declarations** — function-level `static` locals, `typedef`, and `union` (fields overlap; used through
  pointers into buffers/`alloc` blocks).
- **A C preprocessor** — `#define` (object-like, function-like, variadic, `#`, `##`), `#undef`, `#if/#ifdef/#ifndef/
  #elif/#else/#endif` with a full constant-expression evaluator, `#include` with `#pragma once`, `#error`/`#warning`,
  `__FILE__ __LINE__ __COUNTER__`, and `-DNAME[=value]` on the command line. Line numbers are preserved.
- **The C standard library** — `printf sprintf format eprintf puts putchar getchar`, `str*`, `ctype`, `qsort`/`bsearch`,
  `rand`/`srand`, `exit`/`abort`, `math.h` functions and the usual constants. `printf` has the full `%` language
  (plus `%b`) and reports bad formats as errors.
- **B** — function definitions without `fn` (`name(a, b) { }`), `auto` (variables and `v[n]` vectors), `extrn`,
  `char`/`lchar`, `getvec`/`rlsevec`, `printn`/`putnumb`.
- **`main(argv)` / `main(argc, argv)`**; an int returned by `main` is the process exit status.
- **REPL** — macros persist (`:macros`); an `if` waits for a following `else`, `try` for its `catch`, a trailing
  operator for its operand; values pretty-print and wrap (`:set width`) and are coloured on a terminal; every input
  is its own `<repl:N>` source so errors show the offending line and caret, even for functions defined earlier;
  new commands `:info :del :time :timing :history :redo :save :paste :reload :set :color :mem :clear`; member
  completion after a dot, file-name completion after `:load`/`:save`, auto-indent on continuation lines; a
  startup file (`~/.kingcobrarc` or `$KC_REPL_INIT`); a lexer error no longer ends the session. See [REPL.md](REPL.md).
- *(carried over from before 0.8)*
- **`std.tts`** — offline text-to-speech in native C: text normaliser (numbers, years, dates, times, money,
  units, abbreviations, acronyms, URLs, camelCase), pronunciation engine (lexicon, dictionary, morphology,
  homograph disambiguation, ~330 letter-to-sound rules, stress; ARPAbet and IPA; CMUdict loader), prosody,
  a Klatt-style formant synthesizer with 10 voices + `define_voice`, an SSML subset, and word/phoneme/viseme
  alignment. Speech is a `std.music_note` audio buffer. See [STD_TTS.md](STD_TTS.md).
- **`std.tts` vocal styles**: inline tags `[happy]` `[sad]` `[whisper]` `[dramatically]` … (54 styles, intensity words,
  combinations, parameter tags, typo-tolerant), 17 synthesized sound events (`[laugh]` `[sigh]` `[gasp]` `[cry]` `[cough]` …),
  24 interjections, `define_style`, trail-off `...`, chat-spelling expansion and word stretching.
- `std.music_note` now exports its audio player to other native modules (no change for scripts).
- **Showcase programs** — seven real-world examples in `examples/showcase/` (log analyzer, order pipeline,
  support triage, REST API, physics lab, binary protocol, algorithms); `make showcase` runs them.
- **File icon** — the KingCobra logo is the icon of every `.kc` file: Linux (`make install` /
  `make install-mime-user`), Windows (embedded in `kingcobra.exe`; `make install-windows`). Regenerate with `make icons`.
- **`kcide` understands the v0.8 language** — highlighting for `switch/case/default`, `do`, `goto` (+ its label), `static`, `typedef`,
  `union`, `auto`, `extrn` and `#preprocessor` lines; completion for the C/B standard library; snippets `switch do cfor static union
  goto auto bmain printf #define #include #ifdef`; `union` in the outline. (They stay usable as ordinary names where they
  are not keywords.)
- **KingCobra IDE (`kcide`)** — a terminal IDE written in C11: KingCobra syntax highlighting, tabs, completion
  (including `std.*` members), diagnostics from `kingcobra check`, run/test in an output panel, go-to-definition,
  find/replace, themes. `make ide`; see [IDE.md](IDE.md).

**Changed** (check these when upgrading)
- A line starting with `#` is now a preprocessor directive (it used to be a syntax error). A `#` inside a string or
  comment is untouched.
- `fn main` may now take `(argv)` or `(argc, argv)`, and an **int returned by `main` is the process exit status**
  (it used to be ignored). A program whose `main` ends with `return 1;` now exits with 1.
- `name: statement` — a label may now precede any statement (it is a `goto` target). The old error "a label can
  only be placed before a for or while loop" is gone; `break name;` on a non-loop label is still "unknown loop label".
- `if`/`while`/`for` bodies may be a single statement; adjacent string literals join (`"a" "b"` was a syntax error);
  array literals accept a trailing comma.
- `x--y` / `x++y` written without spaces lex as `--`/`++` now (write `x - -y`).
- New names exist in the builtin scope: `printf format puts putchar getchar strlen … floor ceil round pow … exit rand
  time char lchar getvec NULL EOF INT_MAX M_PI …`. Your own definitions still shadow them.
- REPL: errors print the offending source line and a caret; `x++` echoes its (old) value like any expression.

**Fixed**
- `std.music_note`: biquad filters in several instruments (flute, strings, …) started from uninitialised delay
  state, so pitch detection could fail depending on stack contents. They are zero-initialised now.
- Windows REPL: calling a function stored in a variable that has default parameters
  (`let x = fn (a: int, b: int = 9) -> int {...}; x(1)`) could report garbage arity/name. The default-values
  slot of a function node was accessed through a type-punned pointer (undefined behavior); it is now a real
  `KcNode` member (`fn_defaults`).
- Makefile: on Windows the icon-resource rule became the default goal, so plain `make` did not build the
  program; `.DEFAULT_GOAL := all` is now explicit.

## 0.7.0 — a language you can build on
**Added**
- Syntax: ternary `c ? a : b`, null-coalescing `a ?? b`, the `in` operator, `*= /= %=`, f-strings
  `f"x = {x + 1}"`, object literals `{ k: v }`, slicing `a[1..3]` / `s[..n]`, character literals `'a'`,
  `0b`/`0o` literals and `_` digit separators, exponent floats (`1e9`), `\xHH` and `\u{…}` escapes.
- Declarations: default parameters, variadic `...rest`, erased generics `fn id<T>(x: T)`, destructuring
  `let [a, ...r] = …` / `let {x, y: z} = …`, `enum`, `impl` blocks (methods, static functions, and
  extension methods on built-in types), `type` aliases, `const`.
- Control flow: `for (i, x in …)` / `for (k, v in object)` / iteration over strings and objects, labeled
  `break`/`continue`, `match` alternatives (`1 | 2`) and `Enum.Member` patterns, `match` as a statement
  without `;`.
- **Error handling**: `try`/`catch`/`throw`. Runtime errors are catchable objects
  `{message, kind, file, line, col}`; any value can be thrown; `defer` runs while an error unwinds;
  stack overflow — including in JIT-compiled recursion — is a catchable error instead of a crash.
- Library: `assert`, `assert_eq`, `error`; `map filter reduce find any all each sort sorted concat first
  last insert remove extend sum min max zip`; `keys values items has get merge`; string `repeat pad_left
  pad_right chars lines`; `abs min max clamp idiv ord chr`.
- Tooling: `kingcobra test` (runs `test_*` functions; failures show file:line), `kingcobra check`,
  source excerpts with a caret in error messages, and "did you mean …?" suggestions (variables, struct
  fields, module members, built-in methods).
- `make pgo`: profile-guided build (`tests/pgo_train.sh`), measured 10–25% faster than a plain build.
- Documentation: [GRAMMAR.md](GRAMMAR.md), [STABILITY.md](STABILITY.md), this file; `tests/lang_test.kc`,
  the language's own tests written in KingCobra.
- Merged contributions: the REPL (`docs/REPL.md`), `std.mmu`, recoverable errors, and the native-stack
  overflow guard.

**Changed**
- **Integer literals are exact 64-bit.** Before, they passed through a `double`, so anything above 2^53
  was silently wrong. A decimal literal above `9223372036854775807` is now an error.
- **`==` compares arrays, structs and objects by value** (it used to compare by identity).
- **Calling a function with the wrong number of arguments is an error** (it used to warn and pad with
  `null`). Callbacks passed to `map`/`filter`/… may declare an extra index parameter.
- Strings can no longer be left unterminated, and an unterminated `/* */` is an error (both used to be
  accepted silently).
- Objects and structs now have built-in methods (`keys`, `get`, `remove`, …). A struct *field* or a user
  `impl` method of the same name still wins, so existing code keeps working.
- `"a" + b` keeps embedded NUL bytes (it used to truncate at the first one).

**Fixed**
- `INT64_MIN % -1` and `INT64_MIN / -1` crashed the process (a hardware trap); they now give 0 and the
  wrapped value.
- Runaway recursion in a JIT-compiled function segfaulted outside the REPL; it is now an error.
- A struct redeclared in the REPL kept its old memory layout, and a layout could keep a dangling pointer
  into the (reallocating) struct table.
- The native stack guard could report a false overflow when the current frame sat above its reference
  address (e.g. under AddressSanitizer); it is hardened.
- Uninitialized interpreter and parser state fields (they are now zeroed at allocation).

**Performance** (same machine, interleaved best-of-7; see [PERFORMANCE.md](PERFORMANCE.md))
2.6–11.7× faster than 0.5.0 across typical operations (`make pgo` build); 0.6.0 → 0.7.0 is 1.0–1.3× with the new features
included, and builtin calls are 30% faster (large scopes now use a hash index).

## 0.6.0 — explicit memory, no GC
- `free` works on strings, arrays, structs, objects, buffers and `alloc` blocks; freed values are
  tombstones, so use-after-free and double free are precise errors (with a quarantine so stale
  references to recent frees are always detected). Freed memory is reused by a size-class heap.
- `free_deep`, `is_freed`, `defer`, `mem_stats`, `mem_report`, `kingcobra --leaks` (with allocation sites).
- **The garbage collector was dropped for good** — see [MEMORY.md](MEMORY.md).
- Merged the REPL and `std.mmu` contributions.

## 0.5.1 — interpreter speed
Interned names, pooled scopes, scope elision, stack call arguments, cached string literals, inlined hot
paths, allocation-free pointer loads. 2.4–11× faster on typical operations.

## 0.5.0 — pointers, buffers, offsets
C-style pointers with bounds- and lifetime-checking, `buffer`, `sizeof/alignof/offsetof`, `packed struct`,
casts, bitwise operators, pointers across the FFI. See [POINTERS.md](POINTERS.md).
