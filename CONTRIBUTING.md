# Contributing to KingCobra

Thanks for helping. KingCobra is a plain-C11 project with no third-party build dependencies, and we'd like to keep it that way.

## Ways to contribute

- **Report a bug** — open a [bug report](https://github.com/DivyanshuSinha136/kingcobra/issues/new/choose). Include a minimal `.kc` reproduction and the output of `kingcobra info`.
- **Propose a feature** — open a feature or standard-library request first, so the design can be discussed before code is written.
- **Improve docs and examples** — fixes to `docs/` and `examples/` are always welcome.
- **Test on other platforms** — ARM64, ARMhf, RISC-V and Windows results are especially useful (see `docs/PLATFORMS.md`).
- **Security issues** — do **not** open a public issue; see [SECURITY.md](SECURITY.md).

## Getting set up

You need a C11 compiler and `make`. On Windows use MSYS2 / MinGW-w64.

```bash
git clone https://github.com/DivyanshuSinha136/kingcobra.git
cd kingcobra
make                     # builds ./kingcobra
make test                # runs every example and smoke test end-to-end
./kingcobra test         # runs every test_* function (tests/lang_test.kc is the language's own suite)
```

Useful extras:

```bash
./kingcobra check <file-or-dir>   # parse + type-check only
./kingcobra --leaks prog.kc       # report memory that was never freed
./kingcobra --no-jit prog.kc      # compare interpreter vs JIT results
make test-kcc                     # kcc compiler tests
make test-kasm                    # kasm assembler tests
```

## Project layout

| Path | What lives there |
|---|---|
| `compiler/` | Lexer, parser, type checker, interpreter, closure JIT, FFI (C11) |
| `std/<module>/` | One directory per standard-library module (C11) |
| `kcc/`, `kasm/`, `kcos/` | Compiler, assembler and OS, written in KingCobra |
| `ide/` | `kcide` terminal IDE and debugger |
| `examples/`, `tests/` | Runnable programs and test suites |
| `docs/` | Language spec, CLI, per-module references, changelog, roadmap |

## Making a change

1. **Open or find an issue** for anything bigger than a small fix.
2. **Branch** from `main`: `git checkout -b fix/short-description`.
3. **Keep it focused.** One logical change per pull request.
4. **Add tests.** A bug fix needs a regression test; a feature needs tests and an example.
5. **Update docs.** User-visible changes need a `docs/` update and an entry under **Unreleased** in `docs/CHANGELOG.md`.
6. **Run `make test`** and make sure it passes before you push.
7. **Open a pull request** and fill in the template.

## Coding guidelines

- **C11 only**, no third-party build-time dependencies. Optional system libraries (such as OpenSSL) are loaded at run time with `dlopen` / `LoadLibrary`, and the language must still work without them.
- Code must build without warnings under `-Wall -Wextra`.
- **Portability:** go through `compiler/include/kc_platform.h` for anything that differs between Linux and Windows. Remember `char` signedness differs by CPU; the Makefile pins `-fsigned-char`.
- **Never silently corrupt memory.** Misuse should produce a clear runtime error, in keeping with the memory model in `docs/MEMORY.md`.
- **Errors are written for people:** say what went wrong and, where possible, suggest a fix. The `file:line:col: error:` prefix is stable.
- **Tests match a distinctive fragment** of an error message, not the whole line (wording may change).
- KingCobra source files use 4-space indentation.

## Compatibility rules

KingCobra is pre-1.0. A **minor** release may change behavior, but every change must be listed under **Changed** in the changelog with a migration note. A **patch** release only fixes bugs. Read [docs/STABILITY.md](docs/STABILITY.md) before changing syntax, the memory model, exit statuses, or error-object shape.

## Commit messages

Short imperative summary (about 72 characters), then an optional body explaining *why*:

```text
std.json: reject lone surrogates in strict mode

RFC 8259 allows them syntactically but they are not valid Unicode scalar values…
```

Prefix with the area (`compiler:`, `std.csv:`, `kcc:`, `docs:`, `ci:`) where it helps.

## Review

Pull requests are reviewed by the maintainer. Expect questions about edge cases, portability and tests. CI must be green for the blocking targets (amd64, arm64); armhf and riscv64 run under QEMU and are reported but non-blocking.

## License

By contributing you agree that your contributions are licensed under the [MIT License](LICENSE).

Please also follow the [Code of Conduct](CODE_OF_CONDUCT.md).
