# Platforms and CPU architectures

KingCobra builds with a C11 compiler and runs on these CPUs. The language and
standard library behave identically on all of them; the only difference is speed
on the closure JIT, which exists for x86-64 only.

| Architecture | `info` / `system.arch()` | Interpreter | Closure JIT | `std.rhex` raw code |
|---|---|---|---|---|
| x86-64 | `x86_64` | yes | **yes** | yes, plus cpuid/rdtsc/rdrand/port I/O/MSR |
| ARM64 | `aarch64` | yes | no (interpreter) | yes (AArch64 code) |
| ARMhf (32-bit, hard-float) | `armhf` (`arm` for soft-float) | yes | no (interpreter) | yes (ARM code) |
| RISC-V 64 / 32 | `riscv64` / `riscv32` | yes | no (interpreter) | yes (RISC-V code) |
| x86 (32-bit) | `x86` | yes | no (interpreter) | yes |
| anything else | `unknown` | yes | no | refused |

Where there is no JIT, hot functions simply keep running in the tree-walking
interpreter, which is the reference semantics, so output is identical.
`kingcobra info` shows the architecture, word size and why the JIT is off.

## Building

Natively, `make` works on any of the above. To cross-compile (Debian/Ubuntu
toolchains `gcc-aarch64-linux-gnu`, `gcc-arm-linux-gnueabihf`,
`gcc-riscv64-linux-gnu`):

    make arm64      # -> kingcobra-arm64
    make armhf      # -> kingcobra-armhf
    make riscv64    # -> kingcobra-riscv64
    make CC=<triple>-gcc BIN=kingcobra-foo     # any other toolchain

Objects are kept per target triple under `build/<version>/<triple>/`, so native
and cross builds never mix. X11 and readline are linked only if the target
sysroot has their headers (`make KC_NO_X11=1` / `KC_NO_READLINE=1` to force off).
Run a cross build under user-mode QEMU with
`qemu-aarch64 -L /usr/aarch64-linux-gnu ./kingcobra-arm64 run prog.kc`, and the
smoke tests against it with `KC_BIN=./kingcobra-arm64 bash tests/smoke_test.sh`
(on the target machine itself: just `make test`).

## What differs by architecture, and what KingCobra does about it

- **`char` signedness.** Plain `char` is unsigned on ARM and RISC-V. The Makefile
  builds with `-fsigned-char` so every CPU behaves the same.
- **Float to int.** C leaves out-of-range conversion undefined and CPUs disagree.
  `int(x)` now truncates toward zero, saturates to the int64 range, and maps NaN
  to 0 on every architecture.
- **32-bit word size.** FFI integer arguments and results are one machine word,
  because on 32-bit ARM an `int64_t` argument would occupy a register pair and
  misalign every following argument. On 32-bit hosts an extern's integers are
  therefore 32-bit (a function returning a 64-bit integer cannot be called through
  the fixed-arity trampoline); doubles are unaffected. 64-bit hosts are unchanged.
  `off_t` is forced to 64 bits (`-D_FILE_OFFSET_BITS=64`) for `std.mmio`.
- **Instruction cache.** ARM and RISC-V do not keep instruction fetch coherent
  with stores. Sealing a `std.rhex` buffer now flushes the instruction cache.
- **MMIO ordering.** `std.mmio` uses `dmb` on ARM and `fence` on RISC-V with the
  same ordering contract as Linux `readl`/`writel`.

## `std.rhex` on non-x86

`rhex.arch()` tells you which machine code the host runs. `exec`, `call`,
`call_args`, buffers and `fence`/`pause` work everywhere; `cpuid`, `rdtsc`,
`rdtscp`, `rdrand`, `rdseed`, `clflush`, MSRs and port I/O are x86 instructions
and report a clear error on other CPUs. `examples/std_rhex_arch.kc` has working
byte sequences for each architecture.

## Microcomputers (Raspberry Pi and similar)

| Board | CPU | Install |
|---|---|---|
| Raspberry Pi 2, 3, 4, 5, Zero 2 W (64-bit OS) | arm64 | `.deb` / tarball |
| same boards with a 32-bit OS (ARMv7) | armhf | `.deb` / tarball |
| Raspberry Pi Zero, Zero W, Pi 1 (ARMv6) | armv6 | compiled on the board (20-40 min) |
| BeagleBone, Orange Pi, Rock, Odroid, Jetson, Banana Pi (ARMv7 / ARM64) | armhf / arm64 | `.deb` / tarball |
| VisionFive 2, Milk-V, other RISC-V 64 boards | riscv64 | `.deb` / tarball |
| Android phone / tablet (Termux) | any | compiled on the device |
| Arduino, ESP32, RP2040, STM32 (bare metal) | MCU | not supported: no OS / C library |

`install.sh` detects all of this itself (see docs/PACKAGING.md). These boards run the interpreter (the JIT is x86-64 only),
so results are identical; only speed differs.

## Release builds (what GitHub Actions compiles)

Every push is compiled and smoke-tested, and every release is built, for each of these devices
(`.github/workflows/ci.yml`, `release.yml`):

| Device | How it is built | Release file |
|---|---|---|
| Linux amd64, arm64 (glibc) | native runners, Debian 12 container | `.deb`, `-linux-<arch>.tar.gz` |
| Linux armhf, riscv64 (glibc) | QEMU emulation, Debian 12 / 13 container | `.deb`, `-linux-<arch>.tar.gz` |
| Linux amd64, arm64 (musl / Alpine) | native runners, Alpine container | `-linux-musl-<arch>.tar.gz` |
| Fedora / RHEL family amd64, arm64 | native runners, Fedora container | `.rpm` |
| macOS Apple Silicon, Intel | macOS runners | `-macos-<arch>.tar.gz` |
| Windows x64 | MSYS2 / MinGW-w64, static | `-windows-x64.zip` |
| any other machine | compiled by `install.sh` on the user's machine | `-src.tar.gz` |

amd64 and arm64 glibc builds, and the Windows build, must pass for a release. The musl, rpm and macOS builds
are new: a failure there only drops that one file from the release, and `install.sh` then compiles from source
instead. macOS has no JIT on Apple Silicon (interpreter, identical results) and does not run `std.rhex` machine code
(it needs `MAP_JIT`). Packages are installed in clean Debian, Ubuntu, Fedora, Rocky and Alpine containers before
a release is published.

## Honest status

All of this was developed and tested on x86-64 Linux. The ARM64, ARMhf and
RISC-V code paths (barriers, icache flush, word-size FFI, Makefile cross
targets) could not be compiled or run there. What *was* verified: the whole
smoke suite passes on a build forced to behave as an unknown, non-x86 CPU with
unsigned `char` (no JIT, no x86 assembly). Please run `make test` on real
hardware or under QEMU and report anything that fails. Not covered: Apple
Silicon (macOS needs `MAP_JIT` for executable memory, so `std.rhex` execution
will be refused there), big-endian targets, and the Windows-on-ARM toolchain.
