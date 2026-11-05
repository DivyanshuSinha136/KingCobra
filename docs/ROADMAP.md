# KingCobra Roadmap

This file exists so that every honesty caveat made in the code and README
has one canonical home. v0.3.0 is a real, working core — everything below is
what's deliberately *not* done yet, and why.

## v0.2 — language completeness (done)
- [x] **Struct instance construction and field access.** `struct Point {
      x: int, y: int }` then `Point { x: 1, y: 2 }` as a literal. Structs
      are reference types like arrays — copies share the same fields,
      mutating through any alias is visible through every other. Field
      sets are validated against the declaration (unknown struct type,
      wrong field count, or an undeclared field name are all caught with
      a specific error). See `examples/structs.kc`. Field *values* are
      still unchecked at the value level by the interpreter itself
      (nothing stops constructing a struct at runtime with the "wrong"
      kind of value in a field) — but the static type checker below
      does catch a field-type mismatch at the literal and at assignment
      time for any struct it can see statically.
- [x] **`std.vector`**, **`std.regx`**, **`std.gui`** (v0.4). Vector/quaternion/matrix
      math; a from-scratch backtracking regex engine; a software-rendered GUI
      toolkit with X11 (tested under Xvfb), Win32 (written, **untested on real
      Windows**) and headless backends. Follow-ups: GUI menu bar, multi-line text
      area, scroll containers, images, multiple windows, a TTF/anti-aliased font;
      regex lookbehind and Unicode classes; a `docs/STD_GUI.md` Windows verification pass.
- [x] **`std.io`** — file and console I/O: `read_file`, `write_file`,
      `append_file`, `exists`, `delete`, `read_line`, `lines`, plus a
      global `input([prompt])` builtin. Built entirely on ANSI C
      `<stdio.h>`, so unlike `std.network`/`std.rhex` it needed no
      `kc_platform.h` branching to be portable. See `examples/std_io.kc`.
- [x] **`std.mmu`** (v0.5.x). Software MMU + real host VM. Nine page-table
      formats (x86-32, x86-64 4/5-level, ARM64 39/48-bit, RISC-V Sv32/39/48/57)
      stored inside sparse simulated physical memory; table-driven walker with
      large pages, split/merge, table reclamation; tagged set-associative TLB
      (stale until invalidated in `manual_tlb` mode); arch-native fault codes;
      hardware or software A/D; user fault handlers; VMAs with demand paging,
      huge pages, growsdown stacks, PROT_NONE, `fork` copy-on-write, shared
      memory, swap with clock reclaim; device windows backed by script
      closures; `host_*` real-memory control. Tests: `tests/mmu_test.kc`,
      `mmu_vm_test.kc`, `mmu_host_test.kc`, randomised `mmu_fuzz_test.kc`
      (all clean under ASan/UBSan). Not done: PAE/PSE-36, stage-2/EPT, other
      ARM granules, file-backed mappings, Windows pagemap equivalent.
- [x] **`std.tts`** (v0.7.x). Text-to-speech, native and offline. Text normaliser (numbers, years, dates,
      times, money, units, abbreviations, acronyms, URLs, camelCase), pronunciation engine (lexicon, ~1,200-word
      dictionary, morphology, compounds, homograph disambiguation, ~330 letter-to-sound rules, stress and vowel
      reduction; ARPAbet and IPA in/out; CMUdict loader), prosody (accents, question and continuation
      intonation, Klatt durations, flapping, UK r-dropping), a Klatt-style formant synthesizer with 10 voices
      plus `define_voice`, an SSML subset, and word/phoneme/viseme alignment. Output is a `std.music_note` audio
      buffer. Expressive styles: 54 `[tag]` styles with intensity/combination/typo correction, 17 sound events, 24 interjections,
      custom styles. Tests: `tests/tts_test.kc` (239 checks, clean under ASan/UBSan, fuzzed). Not done: non-English
      languages, neural voices, streaming output.
- [x] **`std.json`, `std.csv`, `std.fbin`** (v0.4.x). Data-format libraries.
      `std.json`: strict RFC 8259 parser/serializer, paths + pointers, JSONPath
      queries, RFC 6902 patch/diff, RFC 7396 merge-patch, schema validation,
      JSON Lines, number-preserving `format`. `std.csv`: RFC 4180 with header
      records, type inference, strict/lenient, sniffing, streaming reader/writer,
      table helpers. `std.fbin`: unified file/memory byte streams, typed I/O in
      either byte order, varints, bit fields, `struct`-style pack/unpack,
      search/magic checks, hexdump, base64, crc32/adler32/sha256. Supporting
      core change: **open objects** (`kc_object_*` in `value.h`), `o["key"]`
      read/write on objects, and `len(object)`. Tested under ASan/UBSan with a
      mutation fuzzer over the parsers. See `docs/STD_JSON.md`, `STD_CSV.md`,
      `STD_FBIN.md` and `examples/std_json.kc`, `std_csv.kc`, `std_fbin.kc`.
      Follow-ups: `for (k in object)` iteration, hashed key lookup for very wide
      objects, JSON Schema `dependentRequired`/`unevaluated*`, remote `$ref`,
      CSV column-type declarations, a `std.fbin` memory-mapped mode, and
      Windows verification (written to the same portability rules, **untested on
      real Windows**).
- [x] **`match` expressions.** `match (n) { 0 => "zero", _ => "many" }`.
      Patterns are literals (int/float/string/bool/null, with a leading
      `-` for negatives) or the `_` wildcard; at most one `_` per match,
      and a value that matches no arm (with no `_` present) is a runtime
      error naming the problem rather than silently returning null. No
      destructuring patterns yet — see `examples/match.kc`.
- [x] **A file-based module system.** `import "path/to/file.kc";` or
      `import "path.kc" as alias;`. Paths resolve relative to the
      *importing file's* directory first (falling back to CWD-relative),
      so modules work regardless of where the process was launched from.
      A module's own top-level functions/lets become `alias.name` —
      reusing the same reference-type struct machinery that backs
      `obj.field`, so no new value kind was needed. Each imported file
      runs in its own isolated scope (it can use the shared builtins,
      but can't see or collide with the importer's globals, and the
      importer can't see the module's private names). No cycle detection
      yet — an import cycle will recurse until the C stack gives up.
      Struct types declared inside an imported file are registered into
      the same global type-name space as everything else (not scoped to
      that module) — a collision between two files' same-named struct is
      a future item (not addressed by v0.3's module-system follow-ups
      below). See `examples/modules_demo.kc` and `examples/modules/`.
- [x] **String and array methods**, via `obj.method(args)` syntax tried
      before falling back to ordinary member access (so it coexists with
      `module.fn()` and struct fields that hold closures):
      strings get `upper`, `lower`, `trim`, `split`, `contains`,
      `index_of`, `starts_with`, `ends_with`, `replace`, `slice`; arrays
      get `push`, `pop`, `contains`, `index_of`, `slice`, `join`,
      `reverse`. See `examples/string_array_methods.kc`.
- [x] **A real (deliberately scoped) static type-checking pass**, run
      after parsing and before execution (`compiler/src/typecheck.c`,
      skip it with `--no-typecheck`). It checks `let`/`var` initializers
      against explicit annotations, call argument count/types against a
      function's declared top-level signature, `return` against the
      enclosing function's declared return type, and struct-literal
      field values against the struct's declared field types. It is
      deliberately permissive — anywhere a type can't be determined
      statically (values from `std.*` modules, a closure stored in a
      variable rather than called by name, anything from an imported
      file, array element types), it treats that as the wildcard type
      `any` and skips the check rather than guessing and risking a false
      positive on a valid program. This is real error-catching for the
      common cases, not a complete type system — still open for a future
      pass: true inference, exhaustive missing-return-path analysis,
      checked array element types, and type-checking across
      `import "file.kc"` boundaries (an imported file's own contents
      aren't re-checked at the call site today). Native-import calls
      (v0.3, below) are likewise untyped — always inferred as `any`.
- [x] ~~A tracing or generational GC~~ — **dropped (v0.6).** KingCobra uses
      explicit memory management by design: deterministic timing, no pauses,
      and one coherent model with pointers and buffers. Long-running programs
      free what they allocate; see `docs/MEMORY.md`.

## v0.3 — metaprogramming and native interop (done)
- [x] **`comptime { }` blocks** (`compiler/src/typecheck.c`'s sibling
      purity gate lives in `interp.c`'s `kc_forbid_in_comptime`, checked
      at the top of every side-effecting native: `io.write_file`,
      `io.append_file`, `io.delete`, `io.read_line`, `system.exec`,
      `system.exit`, `system.sleep`, `system.setenv`, every `std.network`
      and network-touching `std.http` function, every `std.rhex` entry that requires
      `unsafe` (code execution, MSRs, ports, cache control) plus the
      non-deterministic CPU reads that don't (`rdtsc`/`rdtscp`/`rdrand`/
      `rdseed` — a cycle count or an RNG draw is never a compile-time
      constant, even though reading them isn't memory-unsafe),
      `music_note.to_wav`, any `extern` call, and any native-
      import call). Scoped honestly, not oversold: this is **not** AST-
      level macro expansion (quoting/splicing a tree-walking interpreter
      safely is a much larger architecture change — a real "next thing"
      once there's a bytecode IR, see the JIT section below). What it
      *is*: a block that runs once, when execution reaches it, with
      externally-observable side effects disallowed inside, and whose
      `let`/`fn` bindings land in the enclosing scope (not a nested one)
      so they're usable afterward as ordinary values — i.e. "compute this
      once, trust that it's safe to treat as settled" with a real,
      enforced guarantee behind the "safe" part. Pure reads (`io.read_file`,
      `io.exists`, `io.lines`, `system.getenv`/`platform`/`cwd`/`pid`,
      `memory.*`, `music_note.freq`) are allowed, since they have no
      effect outside the computation itself. See `examples/comptime.kc`.
      Still open: this doesn't distinguish "compile time" from "the first
      time this statement executes" (our architecture has no separate
      compilation phase to run it in) — a `comptime` block inside a loop
      re-runs every iteration rather than truly running once ever, and a
      single global memoizing cache per call-site was deliberately NOT
      added for a `comptime(expr)` *expression* form, because caching an
      expression that reads a function parameter or loop variable across
      different invocations would silently return a stale value — a
      correctness hazard worse than not having the feature. The
      block-statement form here sidesteps that hazard entirely by running
      fully eagerly at the point reached, with ordinary scoping.
- [x] **Native import.** `import "lib.o" as alias;` / `import "lib.so"
      as alias;` (also `.dll`, `.dylib`) opens a compiled native library
      directly — no `extern` declaration at all. A `.o` is first linked
      into a shared library alongside itself (via whatever `cc`/`gcc` is
      on `PATH` — requires the `.o` to have been compiled with `-fPIC`);
      `.so`/`.dll`/`.dylib` load as-is. `alias.symbol(args)` then
      `dlsym`s the symbol fresh each call and dispatches through
      `kc_ffi_call_dynamic` (`compiler/src/ffi.c`), which infers the
      calling convention from the **actual argument values at that call**
      (all-int-like → int64 args and return; the first argument being a
      float → double args and return) since there's no declaration to
      read a signature from. This is deliberately cruder than the fully
      declared `extern` mechanism — use `extern` instead for a signature
      this inference can't guess right (e.g. a float-returning function
      called with only integer arguments). No symbol caching across
      calls yet (`dlsym` runs every call) and no re-link caching for `.o`
      imports (re-links on every `import` statement reached, even if the
      `.o` hasn't changed) — both are straightforward perf follow-ups,
      not correctness gaps. See `examples/native_import_demo.kc` and
      `examples/native/mathutil.c`.

## v0.9 — a mature command line (done)
- [x] Subcommands with per-command help, `eval`/`-p`, stdin programs, `preprocess`, `new`/`init`, shell completions, `info`.
- [x] `check --format=json`, recursive directories, `test --list/--filter/--fail-fast`, `--time`, `--color`, `-I`.
- [ ] `fmt` (a source formatter), `kingcobra doc` (extract signatures), `check --watch` / `test --watch`.
- [ ] A project manifest (`kingcobra.toml`) with dependencies; `kingcobra build` for the native backends.

## v0.8 — C and B capabilities, a mature REPL (done)
- [x] C control flow: `do/while`, C-style `for`, `switch` with fall-through, `goto`/labels, `++`/`--`, comma operator,
  single-statement bodies, `static` locals, `typedef`, `union`, `[v; n]`, multi-character constants.
- [x] A C preprocessor (`#define` incl. function-like/variadic/`#`/`##`, `#if` expressions, `#include`, `-D`).
- [x] The C standard library (`printf` family, `string.h`, `ctype.h`, `qsort`, `math.h`, constants) and B
  (`auto`, `extrn`, vectors, `char`/`lchar`, `printn`, functions without `fn`); `main(argc, argv)` and exit status.
- [x] REPL: persistent macros, smart continuation, pretty/colour values, per-input source excerpts, ten new commands,
  member completion, startup file.
- [ ] JIT support for `++`, C `for` and `switch` (they run in the interpreter today).
- [ ] Bit-fields, compound literals, `goto` into a block; B's `=+` operators (conflict with `= -x`).
- [ ] REPL: syntax highlighting while typing, `:doc` text for built-ins, multi-line editing of a previous input.

## v0.7 — a language you can build on (done)
- [x] Exact 64-bit ints, exponent floats, char literals, f-strings, escapes; unterminated strings/comments are errors.
- [x] `?:`, `??`, `in`, `*= /= %=`, slicing, object literals, structural `==`.
- [x] Default and variadic parameters, erased generics, destructuring, `enum`, `impl` (incl. methods on
  built-in types), `type`, `const`, two-variable `for`, labeled loops, richer `match`.
- [x] `try`/`catch`/`throw` with `defer` unwinding; catchable stack overflow (native and interpreted).
- [x] Standard library core (assert, higher-order functions, stable sort, object helpers, text helpers).
- [x] `kingcobra test`, `kingcobra check`, source excerpts and "did you mean", `docs/GRAMMAR.md`,
  `docs/STABILITY.md`, `docs/CHANGELOG.md`, `tests/lang_test.kc`.
- [x] Speed kept while adding all of it; `make pgo`.
- [ ] A formatter (`kingcobra fmt`): needs an AST printer that preserves comments.
- [ ] Destructuring `match` patterns and tagged enums with payloads.
- [ ] Negative indices / slice steps (deliberately *not* added: they hide off-by-one bugs; open to a
  `a.at(-1)` helper instead).
- [ ] Package manager / module search path.
- [ ] A language server (diagnostics already carry file:line:col and suggestions).
- [ ] Bytecode VM with slot-resolved locals (the remaining big performance step).

## v0.6 — explicit memory management, no GC (done)
- [x] **GC dropped for good.** Memory is explicit, by design (`docs/MEMORY.md`).
- [x] **`free` works on every heap value**: strings, arrays, structs, objects
  (including `std.json` documents and their keys), buffers and `alloc()` blocks.
  `free_deep` frees a structure (alias- and cycle-safe), `is_freed` inspects.
- [x] **Use-after-free and double free are errors**, with precise messages, on
  indexing, fields, methods, iteration, string `+` and every native argument.
  Freed values are tombstones; headers are quarantined (default 4096 frees) so
  stale references to recent frees are always detected.
- [x] **Freed memory is reused**: a size-class free-list heap replaces the bump
  arena; 1,000,000 string alloc/free cycles reserve no extra memory.
- [x] **`defer`** — statements run when a block exits (return/break/continue/end),
  in reverse order.
- [x] **Leak tooling**: `kingcobra --leaks`, `mem_report()`, `mem_stats()`,
  `mem_quarantine()`; each `alloc`/`buffer` records its allocation site.
- [x] Zero cost for programs that never free (checks are gated on one flag).
- [ ] Reclaim the byte buffers of strings built by the remaining std modules
  (they currently reclaim headers only), and free scopes retained by closures.
- [ ] Optional ownership checks (a static "moved/freed" analysis) — would catch
  some use-after-free at check time instead of run time.

## REPL (done)
- [x] `kingcobra repl [file.kc]` (or plain `kingcobra` on a terminal): persistent
  session, expression echo, `_`, multi-line input, optional `;`, recoverable
  errors, Ctrl-C, `:load` / `:reset` / `:vars` / `:type` / `:typecheck`,
  readline editing/history/completion when available. See `docs/REPL.md`.
- [x] Runaway recursion is now a catchable "stack overflow" error instead of a
  SIGSEGV (POSIX: real C-stack usage is checked; the REPL also recovers from
  stack overflow inside JIT-compiled machine code).
- [ ] Windows: verify the REPL on MinGW (it builds without the POSIX-only
  signal handling; Ctrl-C there ends the process).
- [ ] Member completion (`mod.<Tab>`), `:doc name`, `:time expr`.

## v0.5.1 — interpreter speed (done)
- [x] Scope pooling, interned names, scope elision, stack call arguments,
  cached string literals, string arena, inlined hot paths, allocation-free
  pointer loads/stores. 2.4×–11× faster on typical operations; see
  `docs/PERFORMANCE.md` (benchmarks in `tests/bench/`).
- [ ] Bytecode VM with slot-resolved locals and computed-goto dispatch.
- [ ] NaN-boxed 8-byte values; per-call-site builtin lookup cache; PGO build.

## v0.5 — pointers, buffers, offsets (done)
- [x] **Language-level pointers.** `*p`, `&x`, `p->f`, `p[i]`, scaled `p + n`,
  `p - q`, pointer comparison, `as` casts, `sizeof`/`alignof`/`offsetof`,
  `packed struct`, C struct layout (nested structs, fixed arrays, self-
  referential `*Node`). Region pointers are bounds- and liveness-checked with
  no `unsafe`; raw addresses (FFI results, int casts) need `unsafe`.
  Use-after-free is *detected* by never recycling a region's descriptor.
  Full model in `docs/POINTERS.md`.
- [x] **Buffers.** `buffer(n)` with typed, endian-aware accessors, a cursor,
  zero-copy slices, resize, and conversion to `*T`.
- [x] **Bitwise operators** (`& | ^ ~ << >> >>>` + compound forms, `0b` literals)
  — a prerequisite for any of the above to be usable. Precedence is Rust's,
  deliberately not C's.
- [x] Extern/native-import calls accept pointers and buffers (under
  `unsafe`) and externs can return `*T`.
- [ ] JIT support for pointer loads/stores (today those functions stay in the
  interpreter; the JIT's int-only shape is unchanged and still correct).
- [ ] Function pointers, unions, bit-fields, `volatile`, and unsigned 64-bit
  arithmetic (`u64` above 2^63 shows as negative today).

## JIT
- [x] **Graceful fallback.** The JIT probes the platform once (architecture,
      executable-memory allocation, `mprotect`/`VirtualProtect` to R+X, and a
      real execution of a tiny stub under a fault guard on POSIX) and turns
      itself off with a reason if any step fails; `--no-jit` / `KC_JIT=0`
      force it off. Windows: the allocation/protect steps are checked, but
      the stub run has no fault guard (a policy that blocks dynamic code
      fails at `VirtualProtect` with an error code there, not at execution).
      Not yet covered: ARM64 has no native code generator, so it is always
      interpreter-only (tracked under the ARM64 item).
- [x] **Extended the closure JIT** past the original single-expression
      shape: multiple statements, `let`/`var` locals, `if`/`else`,
      `while` with `break`/`continue`, comparisons, short-circuiting
      `&&`/`||`/`!` (matching the interpreter's truthiness semantics
      exactly, not a bitwise shortcut), compound assignment, and
      self-recursive calls (`fib`, iterative accumulators, and similar
      now genuinely compile and run as native code — not just one-line
      arithmetic helpers). Verified against independently-computed
      reference values across ~10 test programs covering nested loops,
      3-way self-call nesting, zero-param functions, anonymous
      closures, and the interpreter→JIT transition at the call
      threshold itself (no discontinuity at the switchover point).
      Also fixed a real stack-alignment hazard along the way: the
      original codegen used `push`/`pop` for binop operands, which
      would have left `rsp` misaligned for a `call` instruction nested
      inside a pending operand (`fib(n-1) + fib(n-2)` — the `+` has a
      pending spilled operand while compiling `fib(n-2)`, which now
      itself needs to `call`). Rewritten to use fixed per-variable
      stack slots instead, which keeps `rsp` constant (and correctly
      16-aligned) for the entire function body — see the "slot-
      addressed load/store" comment in `compiler/src/jit.c`.
      Measured ~108x speedup on `fib(28)` versus the same function
      forced to stay interpreted (via an interpreter-only control,
      not a synthetic benchmark) — see `examples/jit_recursive.kc`.
- [ ] **Floats in the JIT.** Deliberately not attempted in this pass:
      needs real per-variable type inference inside the JIT (int vs.
      double determines which registers and instructions to emit),
      which is a correctness-sensitive scope expansion substantial
      enough to warrant its own pass rather than being bolted on.
- [ ] **Calls to functions other than the one being compiled.**
      Self-calls work today because the function's own address is
      known before compiling (see `jit.c`); calling a *different*
      function needs either compiling callees in dependency order or a
      trampoline/patch-later scheme for forward/mutual references.
- [ ] **Division/modulo in the JIT.** Not compiled — no div-by-zero
      check has been written for the generated code, and the
      alternative (a native SIGFPE instead of KingCobra's own clean
      runtime error) is worse than just not compiling it.
- [ ] **Deoptimization.** Not yet relevant: nothing the JIT currently
      compiles uses type feedback whose assumptions could later be
      invalidated. Becomes necessary once float/polymorphic support
      (above) is added.
- [ ] A real bytecode IR between the AST and native code. This pass
      deliberately extended the existing AST-direct compiler instead
      of building one first — a reasonable, lower-risk path for what
      got added, but floats and non-self calls above would likely go
      much more smoothly with a real IR in place first, since both
      need tracking information (value types; call targets) that's
      awkward to carry through a direct AST walk repeatedly.

## FFI
- [ ] libffi-based (or hand-rolled full System V AMD64) marshalling so
      `extern` supports mixed int/float signatures, struct-by-value
      arguments, and variadic calls — v0.1 requires all parameters to be
      uniformly int-like or uniformly float-like, documented in `ffi.h`
- [ ] A `from "lib" as symbol_name` syntax so the declared KingCobra name
      doesn't have to match the C symbol name byte-for-byte

## Standard library
- [x] **`std.network` — UDP, raw sockets, and a protocol builder.**
      `udp_socket`/`udp_bind`/`udp_send_to`/`udp_recv_from` alongside the
      original TCP functions; `raw_socket`/`set_hdrincl`/`raw_send_to`/
      `raw_recv_from` for `SOCK_RAW` (IPv4 only, gated by the kernel
      behind root/Administrator like on every OS — `raw_socket` says so
      plainly rather than leaving a bare `socket() failed`); a pure,
      comptime-safe protocol builder (`pack_u8/u16/u32`, `pack_bytes`,
      bounds-checked `unpack_u8/u16/u32`, and `checksum16` — the RFC 1071
      Internet checksum every IPv4/ICMP/UDP/TCP header uses) for
      hand-assembling or parsing a wire format a few fields at a time;
      plus `set_broadcast`/`set_keepalive`/`set_nodelay`/
      `set_recv_timeout`/`set_send_timeout`/`shutdown`/`local_addr`/
      `peer_addr`/`resolve`. See `docs/STD_NETWORK.md` and
      `examples/std_network.kc`.
- [ ] `std.network`: non-blocking/async sockets, and IPv6
      support for the raw-socket functions above (TCP/UDP already
      resolve and connect over IPv6 via `tcp_connect`'s `getaddrinfo`
      call; only the new raw-socket path is IPv4-only today)
- [x] **`std.rhex` v0.4.1 maturation.** Split one 133-line file into four
      focused ones (`rhex_buf.c`/`rhex_call.c`/`rhex_cpu.c`/`rhex_hw.c`),
      the same pattern `std.nn` already uses, aggregated by `rhex.c`.
      Buffers: added read-back (`read`/`read_bytes`), a real `protect(h,
      "rw"|"rx")` toggle (new `kc_exec_unseal` in `kc_platform.h`) replacing
      the old one-way seal, `from_bytes`, `size`, `is_sealed` — W^X still
      enforced by construction (only two named states, never both).
      Calling: `call_args`/`exec_args` take up to 6 typed arguments by
      reusing `kc_ffi_call_dynamic` (the exact trampolines the native-
      import FFI already has) instead of writing a second set of ABI
      trampolines for the same job. Hardware access, genuinely new rather
      than just refactored: CPU id/timing/entropy (`cpuid`, `cpu_vendor`,
      `cpu_brand`, `has_feature`, `rdtsc`, `rdtscp`, `rdrand`, `rdseed`,
      `pause`, `fence`) deliberately *without* requiring `unsafe` — these
      are bounded, side-effect-free instructions, the same reasoning
      `std.mmio`'s `virtual()` regions use to skip it; MSR read/write,
      x86 port I/O (sharing `kc_hw_port_in`/`port_out` with `std.mmio`,
      not duplicating them), and `clflush` *do* require `unsafe` and use
      the same fatal-vs-environmental (`last_error()`) split `std.mmio`
      established. See `docs/STD_RHEX.md`. Deliberately did NOT add a
      built-in assembler (`mov`/`jmp`/etc. encoders) in this pass — users
      still hand-encode hex for anything beyond `from_bytes`/`exec`;
      scoping that as its own item below rather than shipping something
      half-built.
- [x] **`std.http` v0.4.3 maturation.** Rebuilt from a 93-line,
      2-function, raw-string client into a modular (`http.c`/`http_url.c`/
      `http_inflate.c`/`http_tls.c`/`http_conn.c`/`http_client.c`/
      `http_server.c`) ~35-function HTTP/1.1 stack: structured
      `HttpResponse`, any method, headers, binary-safe bodies, real
      connect/total deadlines, redirects (credentials stripped
      cross-origin, https->http refused), gzip/zlib/deflate with checksum
      and zip-bomb limits, chunked/length/close framing with smuggling
      checks, keep-alive pooling with safe retry, cookie jar, Basic/Bearer
      auth, streaming `download`, `Url`/query/form/base64 helpers, and
      **verified HTTPS through OpenSSL loaded at run time** (no build
      dependency; the roadmap's "HTTPS needs a TLS backend" item is done
      without any non-C backend). Also a small blocking HTTP server
      (`listen`/`accept`/`respond`). Tested against Python servers
      (redirects, chunked, gzip, slow, truncated, cookies, keep-alive),
      real TLS with self-signed/wrong-CA/mismatched certs, a 158-stream
      inflate corpus, curl-style clients against the KingCobra server,
      and ASan+UBSan; found and fixed one real heap-size bug in URL
      resolution on the way. See `docs/STD_HTTP.md`.
- [ ] `std.http`: proxies (`HTTP_PROXY`/CONNECT), HTTP/2, multipart
      helpers, response streaming to the script, server TLS/keep-alive/
      concurrency, WebSockets, a cookie public-suffix list. The Windows
      code paths are written but not compiled (Linux sandbox).
- [x] **`std.system` v0.4.2 maturation.** Split the 80-line, 8-function
      module into five files (`system.c`/`system_info.c`/`system_path.c`/
      `system_time.c`/`system_proc.c` + `system_glue.h`), the same pattern
      as `std.nn`/`std.rhex`, growing it to ~100 entries with all v0.1
      names/types kept. New: machine info (`os`/`arch`/`cpu_count`/memory/
      uptime/load/`disk_usage`/`is_admin`/`term_size`/`info()`),
      environment (`has_env`/`unsetenv`/`env_list`/`expand_env`) and
      `args()`, paths (`path_join`/`basename`/`dirname`/`extension`/
      `normalize`/`abspath`/…) and filesystem (`stat`/`list_dir`/`mkdir -p`/
      `rmdir` recursive that never follows symlinks and refuses roots/
      home/`copy_file` via atomic temp+rename/`make_temp_*`/`which`),
      time (`date`/`format_time`/`utc_offset` with real DST, OS CSPRNG
      `random_*`, signal-safe `sleep`), and processes (`run`/`run_argv`/
      `output` with separate stdout/stderr capture, stdin, real timeouts and
      process-group kill, plus `spawn`/`wait`/`poll`/`kill`/pipes — a
      `poll()` pump so large simultaneous input and output cannot deadlock).
      Fixed two v0.1 bugs (Windows `setenv`/`getenv` disagreeing; `setenv`
      dereferencing unchecked arguments). Same fatal-vs-`last_error()` split
      as `std.mmio`/`std.rhex`; comptime gate extended to the new
      impure/volatile calls. Verified: 30+ new smoke assertions, ASan+UBSan
      clean, TZ/DST checked against fixed zones. The Windows code paths are
      reviewed against the Win32 docs but **not compiled** in the (Linux)
      sandbox. See `docs/STD_SYSTEM.md`.
- [x] **`std.ascii_screen`**. Native terminal (TUI) screen and ASCII-art engine: diff-rendered cell screen with truecolor/256/16/mono
  output, live keyboard (kitty protocol / Windows console / timed fallback) + SGR mouse, image/JPEG/GIF/SVG/video → ASCII, animations,
  fonts (built-in pixel, FIGlet, BDF, user-defined), HUD widgets, and an ASCII game engine (sprites, tile maps, chunk-streamed worlds,
  entities + physics, particles, camera, controller bindings, A*, FOV, raycaster). POSIX / Windows console / headless backends.
- [x] **`std.game`** (v0.5). Software 2D + 3D engine: chunk-streamed voxel world (disk store, budgets, LRU),
      automatic fog/cloud hiding unloaded chunks, retained HUD (bars, hotbar, minimap, compass, dialog…), model
      loaders for OBJ / glTF / FBX / Unreal PSK(X) / STL / PLY, built-in default models, streamed 2D tilemaps.
      Open: audio, skeletal animation, shadows, JPEG/DDS, Windows verification of the Win32 backend.
- [ ] `std.system`: Windows job objects (group kill on timeout), env
      overrides per child (`run`/`spawn` currently inherit the parent's
      environment — use `setenv` first), signal handling (`on_signal`), and
      `symlink`/`glob`. Deferred rather than half-built.
- [ ] `std.rhex`: a small built-in assembler (`mov reg, imm64`, `ret`,
      `nop`, `int3`, conditional jumps) so common stubs don't need to be
      hand-encoded as hex — deferred out of the v0.4.1 maturation pass
      above to avoid shipping a half-built mini-assembler; `from_bytes`/
      `exec`/`exec_args` already cover "I already have the bytes".
- [x] `std.rhex` on ARM64 / ARMhf / RISC-V: raw-code execution with instruction-cache flush, `rhex.arch()`. The x86
      instruction wrappers (cpuid, rdtsc, rdrand, MSR, port I/O, clflush) stay x86-only with a clear error.
      Still open: ARM/RISC-V equivalents of cpu_vendor/has_feature/rdtsc, tested on real hardware.
- [ ] Verify ARM64/ARMhf/RISC-V builds on real hardware or QEMU (developed on x86-64 only), Apple Silicon `MAP_JIT`.
- [ ] `std.io.read_line`: distinguish end-of-input from a genuinely blank
      line (both currently return `""`)

## Windows portability notes (resolved issues, kept for context)
- `extern ... from "libm.so.6"`/`"libc.so.6"` now resolve transparently on
  Windows too: `kc_dlopen_portable()` in `kc_platform.h` maps those *nix
  names to whichever CRT DLL the interpreter is actually linked against
  (`ucrtbase.dll` or `msvcrt.dll`), so the same `.kc` source runs on both
  platforms. If you hit a library name this doesn't recognize, the fix is
  adding another alias entry there, not forking the example.
- Error/warning output uses ANSI color codes; `kc_enable_ansi_colors()`
  turns on `ENABLE_VIRTUAL_TERMINAL_PROCESSING` for stdout/stderr on
  Windows at startup. This covers Windows 10 (build 1511+)/11 consoles,
  Windows Terminal, and PowerShell. On older Windows consoles that don't
  support virtual terminal sequences at all, the escape bytes will still
  print literally — not yet handled, since it's a shrinking edge case.

## Native compilation
v0.1 ships an interpreter + tiered closure JIT, not an ahead-of-time
native compiler that emits standalone executables. A real `kingcobra
build` that lowers the whole program (not just hot closures) to object
code is the natural next step once the bytecode IR above exists.
