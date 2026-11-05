# std.rhex — raw machine code and hardware access

```
import std.rhex;
```

This is the sharpest tool in the standard library. It lets a KingCobra program
execute arbitrary native instructions and touch real hardware state directly.
x86-64 only — other architectures refuse rather than guess at machine-code or
instruction semantics they weren't written for.

The module is split into four focused pieces (mirroring how `std.nn` is split
into several files), aggregated into one `rhex` namespace:

| file | covers |
|---|---|
| `rhex_buf.c` | executable-buffer lifecycle: alloc, write, read, protect, free |
| `rhex_call.c` | invoking a buffer as code, with or without typed arguments |
| `rhex_cpu.c` | CPU identification/timing/entropy — safe, no `unsafe` needed |
| `rhex_hw.c` | MSRs, port I/O, cache control — real hardware, `unsafe` required |

## `unsafe { }` requirement

Every entry point that executes caller-supplied machine code, or that can
affect real hardware outside the process, **requires** an `unsafe { }` block
(enforced via `ip->in_unsafe`) — the language-level admission that what
follows is not memory-safe and is the caller's responsibility, mirroring
`unsafe` in Rust. None of it is permitted inside `comptime { }`, ever —
raw machine code and hardware state are never "compile-time constants".

CPU-identification/timing/entropy reads (`rhex_cpu.c`) are the one exception:
each is a single, bounded, side-effect-free instruction, so they do **not**
require `unsafe`, the same way `std.mmio`'s `virtual()` regions don't.

## Buffers (`rhex_buf.c`)

A buffer is always in exactly one of two states — **writable** or
**executable (sealed)** — never both. W^X is enforced by construction, not by
convention: `rhex.protect` only accepts the two named modes below, so there is
no way to ask for write+execute together through this API.

| call | meaning |
|---|---|
| `rhex.alloc(size)` -> handle | fresh writable buffer |
| `rhex.from_bytes(bytes)` -> handle | `alloc` + `write_bytes` in one call |
| `rhex.write(h, offset, byte)` | write one byte (buffer must be writable) |
| `rhex.write_bytes(h, bytes)` | write from offset 0 (buffer must be writable) |
| `rhex.read(h, offset)` -> int | read one byte back (either state) |
| `rhex.read_bytes(h, count)` -> int[] | read `count` bytes back from offset 0 (either state) |
| `rhex.protect(h, "rw" \| "rx")` | toggle writable <-> executable |
| `rhex.seal(h)` | back-compat alias for `protect(h, "rx")` |
| `rhex.is_sealed(h)` -> bool | current state |
| `rhex.size(h)` -> int | buffer size in bytes |
| `rhex.free(h)` | release the buffer; handle becomes dead |

`protect` is what makes buffers mature past v0.1's one-way seal: you can
build code, seal it, call it, `protect(h, "rw")` to patch it, and `protect(h,
"rx")` again — a real mutable code object, not a single-use staging buffer.

## Calling (`rhex_call.c`)

| call | meaning |
|---|---|
| `rhex.call(h)` -> int | invoke a sealed buffer as `int64_t (*)(void)` |
| `rhex.call_args(h, args)` -> int \| float | invoke with up to 6 arguments |
| `rhex.exec(bytes)` -> int | one-shot: alloc + write_bytes + seal + call + free |
| `rhex.exec_args(bytes, args)` -> int \| float | one-shot, with arguments |

`call_args`/`exec_args` reuse the exact same calling-convention inference the
native-import FFI already relies on (`kc_ffi_call_dynamic` in
`compiler/src/ffi.c`): if the first argument is a float, every argument is
passed as a double (and the result read as a double); otherwise every
argument is passed as an int64 (and the result read as an int64). Mixed
int/float arguments aren't supported — same documented limitation `extern`'s
dynamic path already has.

**This is the one place in `std.rhex` where the bytes you write are not
portable.** Arguments load into whichever calling convention the *host OS*
uses — System V (Linux/mac: first two integer args in `rdi`/`rsi`) or
Microsoft x64 (Windows: `rcx`/`rdx`) — and KingCobra has no way to translate
this for you, because it doesn't know what your bytes mean. Machine code
written for one convention will silently read leftover register garbage,
not an error, if run under the other — so branch on `system.platform()` if
your raw code needs to run on both:

```kingcobra
import std.rhex;
import std.system;

fn add_code() -> any {
    if (system.platform() == "windows") {
        return [0x89, 0xC8, 0x01, 0xD0, 0xC3]; // mov eax, ecx ; add eax, edx ; ret
    }
    return [0x89, 0xF8, 0x01, 0xF0, 0xC3];     // mov eax, edi ; add eax, esi ; ret
}

fn main() {
    unsafe {
        let buf = rhex.from_bytes(add_code());
        rhex.protect(buf, "rx");
        print(rhex.call_args(buf, [19, 23])); // 42 on both Linux and Windows
        rhex.free(buf);
    };
}
```

## CPU identification, timing, entropy (`rhex_cpu.c`) — no `unsafe` needed

| call | meaning |
|---|---|
| `rhex.cpuid(leaf [, subleaf])` -> `[eax, ebx, ecx, edx]` | raw `CPUID` |
| `rhex.cpu_vendor()` -> string | e.g. `"GenuineIntel"`, `"AuthenticAMD"` |
| `rhex.cpu_brand()` -> string | the 48-byte brand string, e.g. `"Intel(R) Core(TM)..."` |
| `rhex.has_feature(name)` -> bool | `sse2`, `sse3`, `ssse3`, `sse4_1`, `sse4_2`, `popcnt`, `aes`, `avx`, `fma`, `f16c`, `rdrand`, `bmi1`, `avx2`, `bmi2`, `rdseed`, `avx512f` |
| `rhex.rdtsc()` -> int | raw timestamp counter |
| `rhex.rdtscp()` -> `[tsc, cpu_id]` | serializing timestamp + the CPU it ran on |
| `rhex.rdrand()` -> int \| null | hardware RNG; `null` if transiently out of entropy or unsupported |
| `rhex.rdseed()` -> int \| null | hardware entropy source; `null` the same way |
| `rhex.pause()` | spin-wait hint (`PAUSE`) |
| `rhex.fence(["full" \| "read" \| "write" \| "compiler"])` | memory barrier |

## Real-hardware access (`rhex_hw.c`) — `unsafe` required

Error model, same split `std.mmio` already uses:
* **fatal** (exit 65, descriptive): wrong types, bad width, missing `unsafe`;
* **returns `null`/`false`** + `rhex.last_error()`: environment problems —
  no permission, no such device, not supported on this OS.

| call | meaning |
|---|---|
| `rhex.msr_read(msr [, cpu])` -> int \| null | read a model-specific register via `/dev/cpu/N/msr` (Linux, root) |
| `rhex.msr_write(msr, value [, cpu])` -> bool | write one — **can hang or misconfigure the CPU if you get this wrong** |
| `rhex.port_in(port, width)` -> int \| null | x86 port read (1/2/4 bytes) |
| `rhex.port_out(port, width, value)` -> bool | x86 port write |
| `rhex.clflush(h, offset)` | flush one cache line of an `rhex` buffer |
| `rhex.last_error()` -> string | the most recent environmental failure |

`port_in`/`port_out` call the exact same `kc_hw_port_in`/`kc_hw_port_out` in
`compiler/include/kc_hw.h` that `std.mmio.port_in`/`port_out` call — one real
implementation shared between the two modules, not two copies that could
drift apart.

## Platform notes

* MSRs have no supported user-mode API on Windows (needs a kernel driver) —
  `msr_read`/`msr_write` report that honestly there instead of pretending.
* Port I/O needs `<sys/io.h>` + `ioperm`/`iopl` (root), so it's x86 Linux
  only; `rhex.port_in`/`port_out` return `null`/`false` with a clear message
  everywhere else, exactly like `std.mmio`.
* `cpuid`/`rdtsc`/`rdtscp`/`rdrand`/`rdseed`/`clflush` are plain x86
  instructions, so they work identically on Linux and Windows (MinGW-w64/gcc)
  — only the OS-mediated MSR/port paths differ by platform.

## Roadmap

* ARM64 support (today everything here is x86-64 only and refuses cleanly
  rather than guess at instruction encodings it wasn't written for).
* A tiny built-in assembler (`mov`/`ret`/`nop`/jump helpers) so common stubs
  don't need to be hand-encoded as hex — scoped out of this pass to avoid
  shipping a half-built assembler; tracked in `docs/ROADMAP.md`.
