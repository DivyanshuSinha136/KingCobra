# KASM — the KingCobra assembler

`kasm` is an x86 assembler **written in KingCobra** (`kasm/src/*.kc`, no C, no external tools). It reads NASM-style Intel
syntax and writes **object code** (flat binaries, ELF64 `.o`, Intel HEX, source arrays); executables and libraries are made by
**KCC**, which links KASM's object (or `.bin`) with its own linkers (ELF / PE / Mach-O). It
assembles 16-, 32- and 64-bit code, so the same tool builds boot sectors, kernels, libc-free programs, objects for `gcc`/`ld`,
and machine code you run in-process with `std.rhex`.

```
kasm hello.kasm -o hello.o && kcc hello.o -o hello && ./hello   # KASM writes the object, KCC links it
kasm hello.kasm -f elf -o hello                             # same two steps in one command (KCC's linker runs in-process)
kasm code.kasm -o code.bin && kcc code.bin -o code          # raw machine code, entry at byte 0 (exported as `main`)
kasm boot.kasm -o boot.img                                  # flat binary (default)
kasm lib.kasm -f elfobj -o lib.o && gcc -no-pie main.c lib.o   # object file for gcc / ld
kasm code.kasm -f kc --name CODE -o -                       # a KingCobra array literal on stdout
```

## Command line

| option | meaning |
|---|---|
| `-o FILE` | output (default: input name + the format's extension; `-` = stdout for text formats) |
| `-f FORMAT` | written by KASM: `bin` (default), `obj` / `elfobj` (relocatable `.o`), `hex`, `kc` / `c`, `hexdump`. Linked by KCC from the object: `exe` / `elf`, `so`, `dll`, `dylib`. Guessed from the `-o` extension (`.o .obj .hex .exe .elf .so .dll .kc .h .txt`) |
| `--target OS`, `-l LIB`, `--subsystem` | platform (`linux` `windows` `macos`) of `exe/so/dll` output, imported libraries, Windows GUI/console |
| `-l FILE` / `-m FILE` | listing / section-and-symbol map |
| `-b 16\|32\|64` | initial `bits` mode (default 64) |
| `--org ADDR` | flat-binary load address (`--base` is accepted and ignored: KCC links position-independent images) |
| `--entry SYM` | entry of an executable: `main` (C ABI, through KCC's start-up stub) if defined, else `_start` (entered directly, no libc start-up) |
| `-D NAME[=VAL]`, `-I DIR` | preprocessor define / include directory |
| `-w`, `-Werror`, `--check` | silence warnings / warnings are errors / assemble without writing |

Errors name file, line and column, show the source line with a caret, and suggest near misses
(`unknown instruction 'mvo' (did you mean 'mov'?)`, `undefined symbol 'msg_len' (did you mean 'msg_lne'?)`).
Up to 20 errors are listed.

## Source syntax

```
label:      mnemonic  operand, operand      ; comment
.local:                                     ; scoped to the previous non-local label
name        equ  expression                 ; also  db dw dd dq resb resw resd resq  without a colon
            times 16 db 0xCC
```

* **Operands** — registers (all of `rax..r15`, `eax..`, `ax..`, `al/ah/spl/r8b..`, `xmm0-15`, `ymm0-15`, segment, `cr0-15`,
  `dr0-15`, `st(0..7)`), immediates, and memory `[base + index*scale + disp]` with `byte word dword qword tword oword/xmmword
  ymmword` size keywords (`ptr` and `offset` are accepted and ignored), segment overrides (`fs:[0x28]`, `[gs:rax]`),
  `[rel sym]` / `[abs sym]`, `default rel`, `short` / `near` / `far` / `strict`, and `wrt ..plt` / `wrt ..got` (objects).
* **Numbers** — `42 0x2A 2Ah $2A? 0b101 101b 0o17 17q 1_000 'A' 'abcd'`, floats in `dd` / `dq` (`1.5 -2e3`).
* **Expressions** — `+ - * / // % %% & | ^ ~ ! << >> >>> == != < <= > >= && || ^^`, `$` (here), `$$` (section start),
  parentheses. A difference of two labels in one section is a constant (`len equ $ - msg`). Label arithmetic that cannot
  be expressed as address + constant is rejected with an explanation.
* **Data** — `db dw dd dq`, strings (`db "text", 10, 0`), `times N`, `resb/resw/resd/resq`, `incbin "file"[, skip[, len]]`,
  `align N[, fill]` (code is padded with multi-byte NOPs), `alignb`.
* **Layout** — `bits 16|32|64`, `org`, `default rel|abs`, `section .text|.data|.rodata|.bss|name [exec write alloc nobits align=N]`,
  `global name[:function|:data]`, `extern name`, `weak name`.
* **Structures** — `struc Name … endstruc` (defines `Name.field` offsets and `Name_size`), `istruc Name / at Name.f, dd 1 / iend`.
* **Prefixes** — `lock rep repe repz repne repnz bnd o16 o32 a16 a32`.

### Preprocessor

`%define NAME body`, `%define f(a, b) body`, `%xdefine`, `%undef`, `%assign N expr`, `%strlen`, `%macro name N[-M|-*] [defaults] … %endmacro`
(`%1 %2 … %0`, `%%local` labels, overloading by argument count), `%rep N … %endrep` (`%exitrep`), `%if / %elif / %else / %endif`
and `%ifdef %ifndef %ifidn %ifidni %ifnidn %ifmacro %ifnum %ifstr %ifid`, `%include "file"`, `%error`, `%warning`,
`__LINE__`, `__FILE__`, `__KASM__`, line continuation with `\`.

## Instruction coverage

About 1,000 mnemonics (1,666 encodings), table-driven (`kasm/src/tables.kc`):

* the full 8086–Pentium 4 integer set in 16/32/64-bit modes (ALU, shifts, bit ops, string ops with `rep`, I/O, segment loads,
  control/debug registers, `lgdt/lidt`, `syscall/sysenter`, `cmpxchg`, `xadd`, `lock`, `cpuid`, `rdmsr`, …), `popcnt lzcnt tzcnt
  movbe crc32 adcx adox rdrand rdseed rdfsbase xsave fxsave clflush prefetch*`
* x87 (`fld fstp fadd… fcomi fcmov* fsin fyl2x fnstsw fnstcw fninit` and the rest)
* SSE, SSE2, SSE3, SSSE3, SSE4.1/4.2, AES-NI, PCLMULQDQ (`movaps … pcmpistri aesenc pclmulqdq`)
* AVX, AVX2, F16C, FMA3 (all `vfmadd/vfmsub/vfnmadd/vfnmsub/vfmaddsub` orders), BMI1/BMI2 (`andn bextr bzhi blsi blsr pdep
  pext mulx rorx sarx shlx shrx`)

Encoding choices follow NASM/GAS: shortest form wins (`add eax, 5` → `83 C0 05`, `add eax, 500` → `05 F4 01 00 00`), `mov r64, imm`
is shortened to `mov r32, imm` when the value fits in 32 unsigned bits (`mov rax, qword 1` forces `movabs`), `jmp/jcc` use
the short form whenever the final layout allows it (the assembler iterates until sizes and labels agree), and an unsized memory
operand that could mean several sizes is an error rather than a guess.

## Output formats

| format | contents |
|---|---|
| `bin` | sections laid out from `--org` (or `org`), uninitialised sections omitted — boot sectors, ROMs, shellcode |
| `hex` | Intel HEX with extended-address records |
| `exe` / `elf`, `so`, `dll`, `dylib` | KASM writes the object, then KCC's linker (`kcc/src/objin.kc` + `link.kc`) makes the image: ELF64 PIE, shared object, PE32+ or Mach-O. Needs `bits 64`. Code must be position independent (see below) |
| `obj` / `elfobj` | ELF64 relocatable object: `R_X86_64_64/32/32S/PC32/PLT32/GOTPCREL/…`, section and global symbols, externs, `.note.GNU-stack`. Links with `ld` and `gcc` (PIE and non-PIE) |
| `kc`, `c` | the bytes as a KingCobra `let name = [ … ];` or a C array |
| `hexdump` | `xxd`-style dump |

## Library API

```
import "kasm/src/kasm.kc" as kasm;

let r = kasm.assemble("bits 64\nmov eax, 42\nret\n", {});      // r.bytes == [0xB8, 0x2A, 0, 0, 0, 0xC3]
let f = kasm.assemble_file("prog.kasm", { format: "elf", listing: true });   // r.listing, r.map, r.symbols, r.sections
let code = kasm.code("lea eax, [rdi+rsi]\nret");                 // 64-bit bytes
unsafe-free convenience:  kasm.run("popcnt rax, rdi\nret", [255])   // assembles and runs through std.rhex -> 8
```

Options: `format bits org base entry listing includes defines base_dir file name`. `assemble` throws
`{ kind: "compile", message, line, col, file, src, all }` on errors. `kasm.run` defines `WIN` on Windows so one source can pick
rcx/rdx vs rdi/rsi (see `kasm/examples/kasm_rhex.kc`).

## Verification

`make test-kasm` (or `sh kasm/tests/verify.sh`) runs, in order: the type check, 22 unit tests, **differential tests that
assemble 817 instructions with KASM and with GNU `as` and compare the bytes** (`kasm/tools/diff_gas.py`; the golden
encodings are committed as `kasm/tests/insns*.hex`, so the unit tests need no binutils), and — on Linux x86-64 — builds every example as `.o`/`.bin`, links it with KCC (and a gcc-built object too), runs the results, links an object with `gcc`, and executes machine code through `std.rhex`. A 180-branch random jump
program relaxes to the same bytes as GNU `as`.

## Limitations

KASM itself writes ELF64 objects only (no 32-bit ELF or COFF/Mach-O objects; KCC links them for other targets); no AVX-512/EVEX, MMX or 3DNow; `dt`/`do`/`dy` data and `%rotate`
are not implemented; `wrt` supports `..plt` and `..got` only; listings show instruction bytes before relocation; string
instructions with operands (`movs`, `stos`, …) use the operands only to pick the size; `[rax*2]`-style addresses follow
NASM (index only, 32-bit displacement). Absolute addresses in memory operands use the 32-bit form unless `default rel`
or `[rel x]` is given (NASM-compatible).

## Linking with KCC

`kcc input.o -o out` (also `.obj`, `.bin`, or `--raw`) links object code instead of compiling KingCobra source. It reads ELF64
x86-64 relocatable objects from KASM **and from `gcc -c` / GNU `as`**: sections are merged into text / rodata / data / bss,
`PC32`, `PLT32`, `GOTPCREL(X)` and `64` relocations are applied by KCC's linkers, undefined symbols become imports
(`libc.so.6` / `msvcrt.dll` by default, or `-l LIB`). A raw `.bin` becomes one code section with its entry at byte 0
(`--entry-offset N`, exported as `main` for executables, `code` otherwise; `--symbol NAME`). Executables are entered at `main`
(C ABI) or `_start` (`--entry SYM`).

Position independence: absolute 32-bit relocations (`mov eax, label`) and absolute 64-bit addresses in code are rejected with an
explanation, because the images are relocatable. Use `default rel` / `lea reg, [rel sym]`; pointer tables go in `.data`.
Mach-O executables need a `main`. Windows output from object code is structurally valid but was not run in the test environment.
