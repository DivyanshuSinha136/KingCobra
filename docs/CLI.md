# The `kingcobra` command line (v0.9)

```
kingcobra [options] <command> [args]
kingcobra [options] <file.kc> [script args]      same as `run`
kingcobra -e <code>   kingcobra -p <expression>   kingcobra -E <file.kc>
kingcobra                                        on a terminal: start the REPL
```

`kc` is an alias; the name in messages follows however you invoked it. `kingcobra help <command>` or
`kingcobra <command> --help` shows the details of one command.

## Commands

| Command | What it does |
|---|---|
| `run <file.kc\|-> [args…]` | Run a program: top-level statements, then `main()`. `-` reads the program from standard input. |
| `repl [file.kc]` | Interactive session ([REPL.md](REPL.md)); the optional file is preloaded (its `main` is not called). |
| `eval <code>` / `-e <code>` | Run a snippet: `kingcobra -e 'print(6 * 7)'`. A missing final `;` is supplied. The arguments after the code arrive as `argv` / `argc`. |
| `-p <expr>` | Print the value of one expression: `kingcobra -p '2 ** 10 + 1'`. |
| `check [-q] [--format=text\|json] <file\|dir>…` | Parse and type-check without running. Directories are searched recursively for `*.kc`. |
| `test [options] [file\|dir]…` | Run every `test_*` function ([below](#test)). |
| `preprocess <file>` / `-E` | Print the source after the preprocessor: `kingcobra -DDEBUG -E main.kc`. |
| `new <name>` / `init` | Create a project (`main.kc`, `tests/test_main.kc`, `README.md`, `.gitignore`); `init` does it in the current directory and never overwrites. |
| `completions <bash\|zsh\|fish>` | Print a shell completion script. |
| `info` | Version, compiler, platform, whether the JIT is usable (and if not, why) and whether readline / X11 are in this build, the module list, `#include` path. |
| `modules` | The `import std.<name>` modules, one per line. |
| `help [command\|topic]` | Help for a command — or, for any other word, the language's own help: `kingcobra help map`, `kingcobra help switch`, `kingcobra help topics`. |
| `version` | |

## Options

Global options may come **before the command** or **between the command and the script**. Everything after the
script path belongs to the script and is never interpreted — so `kingcobra tool.kc -D --leaks -h` hands all
three to `tool.kc`. (`--` before the file name lets a file name start with `-`.)

| Option | |
|---|---|
| `-DNAME[=value]` | Define a preprocessor macro ([C_AND_B.md](C_AND_B.md#the-preprocessor)). |
| `-I DIR` | Add `DIR` to the `#include <…>` search path (also `$KC_INCLUDE`, colon-separated). |
| `--no-typecheck` | Skip the static type checker. |
| `--no-jit` | Never compile hot functions to machine code; everything runs in the interpreter (same as `KC_JIT=0`). The JIT already turns itself off when the platform cannot run generated code; this flag is for timing comparisons and debugging. |
| `--leaks` | At exit, report `alloc`/`buffer` blocks never freed (with where they were allocated) and objects still allocated. |
| `--time` | Report parse / check / run times on stderr. |
| `--color=auto\|always\|never`, `--no-color` | Diagnostic colour. `auto` (default): on when stderr is a terminal, off for `NO_COLOR`, on for `FORCE_COLOR`. |
| `-h`, `--help`, `-V`, `--version` | |

Mistakes are caught with a suggestion instead of being mistaken for a file name:

```
$ kingcobra chek a.kc
kingcobra: unknown command 'chek' — did you mean 'check'?
$ kingcobra --leek run a.kc
kingcobra: unknown option '--leek'; did you mean '--leaks'?
```

## Programs

```
fn main() { }                 # no arguments
fn main(argv) { }             # argv[0] is the script, argv[1…] the arguments
fn main(argc, argv) -> int { return 0; }     # an int result is the exit status
```

A script can start with `#!/usr/bin/env kingcobra` and be run directly (`chmod +x tool.kc; ./tool.kc`).
`system.args()` ([STD_SYSTEM.md](STD_SYSTEM.md)) returns the same arguments (without the script name).

## check

```
kingcobra check src/                       # every .kc file below src/
kingcobra check -q src/ && echo clean      # print only problems
kingcobra check --format=json a.kc b.kc    # editors / CI
```

JSON mode prints one object per problem on stdout and nothing else:
`{"file":"a.kc","line":3,"col":9,"severity":"error","message":"undefined variable 'x'"}`.
A file stops at its first error, so each file yields at most one object.

## test

```
kingcobra test                        # test_*.kc and *_test.kc in . and tests/
kingcobra test tests/ examples/       # directories are searched recursively
kingcobra test -k parse               # only tests whose name contains "parse"
kingcobra test -l                     # list them: tests/test_main.kc::test_greet
kingcobra test -x -q                  # stop at the first failure, show only failures
```

A test is a global `fn test_…()`. It passes if it returns, fails if an `assert` fails or an exception escapes.
The summary lists the failing tests as `file::name` so they can be pasted back into `-k`.

## Exit status

| Code | Meaning |
|---|---|
| 0 | success |
| 1 | at least one test failed |
| 64 | usage error (unknown command or option, missing argument) |
| 65 | the source has an error: syntax, type, or an uncaught runtime error / `throw` |
| 66 | an input file or directory could not be read |
| other | whatever `main()` returned or `exit(n)` was given |

## Environment

| Variable | Effect |
|---|---|
| `NO_COLOR`, `FORCE_COLOR` | Diagnostic colour (see `--color`). |
| `KC_INCLUDE` | Extra `#include <…>` directories. |
| `KC_REPL_INIT` | A file the REPL loads at start. |

## Shell completion

```
source <(kingcobra completions bash)                       # bash
kingcobra completions zsh  > "${fpath[1]}/_kingcobra"      # zsh
kingcobra completions fish > ~/.config/fish/completions/kingcobra.fish
```
