# The KingCobra REPL

```bash
./kingcobra repl            # start a session
./kingcobra repl file.kc    # run file.kc into the session first, then prompt
./kingcobra -i file.kc      # same
./kingcobra                 # no arguments on a terminal also starts the REPL
./kingcobra --no-typecheck repl   # start with the static type checker off
```

## Using it

Type a statement or expression. The value of an expression is echoed
(`null` results and assignments are not); strings are shown quoted.

```text
>>> 1 + 2
3
>>> "hi" + " there"
"hi there"
>>> let xs = [1, "a", 2.5]
>>> xs
[1, "a", 2.5]
>>> _
[1, "a", 2.5]
```

- **Definitions persist.** `let`, `var`, `fn`, `struct`, `extern` and `import`
  all land in one global scope that lives for the whole session. Redefining a
  name (including a `fn` or `struct`) replaces it.
- **`_`** holds the value of the last echoed expression.
- **Trailing `;` is optional** for a single expression or statement
  (`x = 5` and `x = 5;` are the same). Several statements on one line still
  need their separators: `let a = 1; let b = 2;`.
- **Multi-line input.** If a `(`, `[`, `{`, string or `/* */` comment is still
  open, the prompt changes to `... ` and input continues until it balances.
  Ctrl-C abandons the pending input. The REPL also keeps reading when the
  statement obviously is not finished: `try { }` waits for its `catch`, an input
  ending in an operator (`1 +`, `a &&`) waits for its operand, and a complete
  `if (...) { }` offers one more line — if it starts with `else` it is joined,
  otherwise (or after a blank line) the `if` runs and that line is a new input.
  On a terminal continuation lines are indented by bracket depth.
- **The preprocessor runs here too.** `#define SQ(x) ((x)*(x))` then `SQ(7)`;
  macros persist for the session, `:macros` lists them, `#undef` removes one.
- **Pretty values.** Long arrays, structs and objects wrap one element or field per line
  (`:set width 100`; default 80); on a terminal values are coloured (numbers, strings,
  booleans). `:color off` or `NO_COLOR=1` turns colour off.
- **Type checking** runs on every input against everything declared so far in
  the session, so `f("oops")` is rejected if `f` was declared `(n: int)`. A
  rejected input leaves no trace. Toggle with `:typecheck off` / `on`.
- **Errors never end the session.** Parse, type, and runtime errors (including
  those raised inside `std.*` modules) print a normal diagnostic with the
  position `<repl:N>:line:col` (N numbers the inputs), the offending source line and a
  caret — also for a function defined many inputs ago, which shows *its* line — and return to the prompt. Deep/runaway recursion
  is reported as a stack overflow rather than crashing.
- **Ctrl-C** interrupts a running statement (`KeyboardInterrupt`) or discards
  the line you were typing. **Ctrl-D** (or `exit`, `quit`, `:quit`) leaves.
  `system.exit(n)` also leaves, with that exit status.

## Commands

| Command | |
|---|---|
| `:help`, `:h`, `:?` | show help |
| `:quit`, `:q`, `:exit` | leave the REPL |
| `:load <file.kc>` | run a file into the session; `main()` is **not** auto-called |
| `:reset` | throw everything away and start a fresh session |
| `:vars` | list everything defined (name, kind, value) |
| `:type <expr>` | evaluate an expression and print the type of its value |
| `:typecheck [on\|off]` | show / toggle the static type checker |

A command is recognised only at a fresh prompt, never in the middle of a
multi-line input.

## Language help

`help` (or `help()`), `?name` and `:help name` show the language's own documentation — the same as `help("name")` in a
program (builtins, keywords, methods, modules, your own functions and structs, topics). A `///` doc-comment line typed
at the prompt waits for the declaration that follows it, so `help(f)` shows your documentation for `f`.

## More commands

| Command | Does |
|---|---|
| `:info name` | a function's signature (`fn f(n: int) -> int`), a struct/union's fields, a module's members, or a value's type |
| `:del name` | forget a variable or function |
| `:macros` | list preprocessor macros |
| `:time code` / `:timing [on\|off]` | time one input / every input |
| `:history [n]`, `:redo [n]` | numbered inputs that ran; run one again |
| `:save file.kc` | write the session's inputs to a file |
| `:paste` | paste many lines, finish with `:end` or Ctrl-D |
| `:reload` | run the last `:load` again |
| `:set width n`, `:color [on\|off]` | display options |
| `:mem` | live allocations (`mem_report()`) |
| `:clear` | clear the screen |

**Startup file.** On a terminal the REPL loads `~/.kingcobrarc` if it exists; `$KC_REPL_INIT` names a
file to load in any mode (a `./.kingcobrarc` in the current directory is deliberately *not* run).

## Line editing, history, completion

When GNU readline is installed (`libreadline-dev` / `readline-devel`), the
Makefile detects it and builds with `-DKC_READLINE`: arrow keys, Emacs/vi line
editing, persistent history in `~/.kingcobra_history` (last 1000 lines), and
Tab completion of keywords, REPL commands, macros, and every name currently defined
(your variables, functions, structs, imported modules, and builtins); after a dot it
completes members (`system.<Tab>`, `p.<Tab>` for a struct, string/array methods); after
`:load`/`:save` it completes file names. Without
readline the REPL still works using plain line input. Build without it with
`make KC_NO_READLINE=1`. (On macOS the system "readline" is libedit; the
Makefile does not use it automatically.)

## Piped / scripted use

When stdin is not a terminal there is no banner and no prompt, but values are
still echoed, which makes the REPL handy for quick checks:

```bash
echo '2 * 10 == 20' | ./kingcobra repl
printf 'fn sq(n: int) -> int { return n*n; }\nsq(12)\n' | ./kingcobra repl
```

The exit status is `1` if any input failed, `0` otherwise.

## How it works

- One persistent interpreter (`KcInterp`); each input is parsed as a tiny
  program, checked by `kc_typecheck_repl` (whose function/struct registries
  persist between inputs), then each top-level statement runs against the
  global scope via `kc_interp_exec_repl`.
- **Recoverable errors.** Every fatal diagnostic goes through `kc_error_at`,
  which used to print and `exit(65)`. It now unwinds with `longjmp` to a
  `KcErrCatch` when one is active (`compiler/include/kc_common.h`). With no
  catcher active (every normal `kingcobra run`) behaviour is unchanged: print
  the diagnostic, exit 65. `std.nn`'s own error path was routed through the same
  mechanism (`kc_fatal`).
- **Optional `;`.** The REPL tries the input as written, then with `;`
  appended, and reports the error from the last attempt.
- **Multi-line detection** is a small scanner for unbalanced brackets, strings
  and block comments (`input_incomplete` in `compiler/src/repl.c`).
- **Memory.** Parsed ASTs and their source text are never freed, because
  closures defined in the session point into them. A long session therefore
  grows slowly with the amount of code typed; `:reset` releases nothing either
  (the previous session's memory is leaked, not freed). This is fine for
  interactive use, not for a REPL embedded in a server. Runtime *values* are a
  different matter: memory is explicit in KingCobra (no GC), so in a session you
  can `free(x)` strings, arrays, structs and `alloc()` blocks like anywhere else
  (`docs/MEMORY.md`). Echoing a freed value (`x` after `free(x)`) is a
  recoverable use-after-free error, and `:vars` / nested echoes show
  `<freed string>` / `<freed array>`.
- **Errors and `defer`.** A recovered error unwinds with `longjmp`, so any
  `defer` statements still pending in the aborted frames do **not** run, and
  those frames' scopes are leaked rather than recycled. State stays consistent
  (the scope pool never holds an in-use scope); put cleanup you need on every
  path in code that cannot raise, or free explicitly.
- **Struct redeclaration.** Declaring a struct name again replaces it, including
  its memory layout (`sizeof`/`offsetof`/`->`), which is recomputed from the new
  declaration. Raw memory you allocated under the old layout keeps its bytes but
  is now interpreted by the new layout.
- **Runaway recursion.** The call-depth limit (4000) is not enough on its own,
  because the evaluator exhausts the real C stack first; on POSIX
  `kc_call_value` now also compares actual stack use to `RLIMIT_STACK`
  (75% budget). Hot functions taking only ints are compiled by the JIT to
  machine code, which has no depth counter, so the REPL additionally installs a
  `SIGSEGV` handler on an alternate stack and, if the fault lands at the stack
  limit while evaluating, unwinds to the prompt with a stack-overflow error.
  Any other fault is not intercepted.

## Limitations

- Ctrl-C and stack-overflow recovery use `siglongjmp` out of a signal handler.
  That is safe for the interpreter's own state (counters are reset) but can in
  principle interrupt `malloc` or a native library mid-update; treat a session
  as disposable after an interrupt inside heavy native code (e.g. `std.http`,
  `std.nn` training).
- Both are POSIX-only. The Windows build compiles the REPL without them
  (Ctrl-C ends the process; deep recursion relies on the 4000-call limit).
  The REPL has not been run on Windows yet.
- `import "file.kc"` inside the REPL resolves relative to the current
  directory.
- Member completion works for names defined in the session (`module.<Tab>`, `struct_value.<Tab>`,
  string/array/object methods); it does not look through expressions (`f().<Tab>`).
- Colour and auto-indent need a terminal (and readline for the indent); piped input is plain.
- An `if` statement always offers one more line on a terminal (to catch an `else`); press Enter.
- A top-level `goto` cannot jump between separate REPL inputs.
