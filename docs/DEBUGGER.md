# KingCobra debugger

`kingcobra debug FILE.kc` runs a program under a source-level debugger. The debugger has no console UI of its
own: an IDE (or any script) drives it over two pipes. The bundled IDE, **kcide**, does this for you
(`Alt+F5`, see [IDE.md](IDE.md)).

What it supports: line breakpoints (set before or while running), step over / into / out, pause, call stack,
local and global variables of any frame, expression evaluation and assignment in any frame, and an automatic
stop when the program dies from an uncaught runtime error (the stack and variables stay inspectable).
Debugging turns the closure JIT off so that every statement runs in the interpreter; normal runs are unaffected
(the hooks are a single predicted-not-taken branch; no measurable slowdown on the benchmarks we tried).

## Channel

`$KC_DEBUG_PIPE` is a base path. The debuggee opens `<base>.cmd` for reading (IDE -> debuggee) and `<base>.evt` for
writing (debuggee -> IDE). On POSIX these are FIFOs (`mkfifo`); on Windows, named pipes
(`\\.\pipe\name.cmd` / `.evt`). The IDE must have both ends open first. The program's own stdin/stdout/stderr are
untouched, so `input()` and `print()` work as usual.

Messages are single lines, fields separated by TAB. Values are on one line (`\n`, `\t` escaped; long values cut at 300).

## IDE -> debuggee

| Command | Meaning |
|---|---|
| `break FILE LINE` / `unbreak FILE LINE` / `clear` | breakpoints (1-based lines, file path is normalised with realpath) |
| `go` / `entry` | leave the setup phase: run, or stop on the first statement |
| `continue`, `stepover`, `stepin`, `stepout` | resume (only while stopped) |
| `pause` | stop at the next statement (while running) |
| `stack` | list frames |
| `vars FRAME` | variables visible from frame FRAME (0 = innermost) |
| `eval FRAME EXPR` | evaluate an expression or assignment in that frame |
| `quit` | exit the debuggee |

## Debuggee -> IDE

| Message | Meaning |
|---|---|
| `ready<TAB>1` | channel is up; send breakpoints, then `go` or `entry` |
| `stopped<TAB>REASON<TAB>FILE<TAB>LINE<TAB>DEPTH<TAB>MESSAGE` | REASON: `breakpoint`, `step`, `pause`, `entry`, `exception` |
| `stack<TAB>N`, then N x `frame<TAB>IDX<TAB>NAME<TAB>FILE<TAB>LINE`, then `end` | reply to `stack` |
| `vars<TAB>FRAME`, then `var<TAB>SCOPE<TAB>NAME<TAB>TYPE<TAB>VALUE` lines, then `end` | SCOPE is `local` or `global` (callables are omitted from globals) |
| `result<TAB>ok<TAB>TYPE<TAB>VALUE` / `result<TAB>err<TAB>error<TAB>MESSAGE` | reply to `eval` (`TYPE` is `assigned` for assignments) |
| `exited` | the program ended |

Example session (`>` IDE, `<` debuggee):

```
< ready	1
> break	/work/t.kc	4
> go
< stopped	breakpoint	/work/t.kc	4	2
> stack
< stack	3
< frame	0	add	/work/t.kc	4
< frame	1	main	/work/t.kc	12
< frame	2	<module>	/work/t.kc	2
< end
> eval	1	p.x + 5
< result	ok	int	6
> stepover
< stopped	step	/work/t.kc	5	2
```

`ide/tests/test_debug.py` exercises this protocol end to end (`make ide-test`).

## Limits

* Statement granularity: a stop happens before a statement, not in the middle of an expression. Several
  statements on one line count as one stop.
* Variable values are shown as one-line summaries; use `eval` (for example `p.items[0].name`) to look deeper.
* `eval` accepts one expression or assignment, not `let` / control-flow statements.
* Code run through `comptime` and the closure JIT is not stepped (the JIT is disabled while debugging).
