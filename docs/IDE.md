# KingCobra IDE (`kcide`)

A terminal IDE for KingCobra, written in portable **C11** with no third-party libraries
(POSIX terminals; Windows Terminal / ConPTY consoles through the Win32 console API).

```
make ide            # builds ide/kcide
make ide-test       # unit tests + fuzzers (undo/redo model, highlighter, UTF-8) under ASan/UBSan
./ide/kcide                 # project = current directory
./ide/kcide examples/       # project = examples/
./ide/kcide hello.kc a.kc   # open files
./ide/kcide --kc /path/to/kingcobra
```

`kcide` finds the compiler via `--kc`, `kc_path` in the config, `$KINGCOBRA`, `<project>/kingcobra`,
`<project>/../kingcobra`, next to the `kcide` binary (and one level up), then `$PATH`.
Use a truecolor terminal for the best colours (`COLORTERM=truecolor`); 256-colour terminals are mapped automatically.

## Features

| Area | What you get |
|---|---|
| **Syntax highlighting** | (Knows the v0.8 C/B syntax: `switch/case/default`, `do`, `goto`, `static`, `union`, `auto`, `extrn`, `#define`/`#include`/`#if` lines.) Hand-written, multi-line-aware tokenizer for the real KingCobra grammar: keywords, declarations, contextual keywords (`type`, `enum`, `try`, `defer`, `from`...), types, builtins, `std.*` modules, function/struct/enum definitions, `self`, labels, numbers (`0x`, `0b`, `0o`, `_`, floats, exponents), char literals, strings with escapes, **f-strings with nested `{expr}` interpolation** (state carried across lines), `//`, `///` doc and `/* */` block comments, `TODO/FIXME` markers. Five themes: Cobra Dark/Light, Monokai, Solarized Dark, High Contrast. |
| **Editor** | Tabs, undo/redo (grouped, 20k ops), selection by keyboard and mouse (double = word, triple = line), auto-indent, auto-close brackets/quotes with type-over and wrap-selection, smart Home, word motions, duplicate/delete/move lines, toggle comment, indent/outdent, re-indent **Format Document**, bracket matching, word-occurrence highlight, indent guides, change bars in the gutter, scrollbar with problem ticks, CRLF preservation, external-change reload, trim-on-save option, 240k-line files stay responsive. |
| **Completion** | Keywords, types, builtins, snippets (`fn`, `main`, `if`, `for`, `forr`, `match`, `struct`, `impl`, `try`, `test`, `import`... + Tab), project symbols, words from open files, `import std.<module>` names, and **members of every std module** (scanned from `std/<module>/*.c` of your KingCobra tree - including `import std.x as y` aliases). Auto popup after 2 characters or after `.`; `Ctrl+Space` forces it. |
| **Diagnostics** | Check-on-save runs `kingcobra check`; errors/warnings appear as gutter dots, underlines, inline messages, explorer colouring and in the Problems list. `F8` / `Shift+F8` jump between them; clicking an output line opens `file:line:col`. |
| **Run** | `F5` runs the file in the Output panel on a pseudo-terminal (colours, `input()` works - type in the panel), `Ctrl+F5` runs it in the real terminal (for `std.ascii_screen` / TUI programs), `F7` tests, `--leaks` run, REPL with the file preloaded, Stop with `Shift+F5`. |
| **Navigation** | Quick open (fuzzy), go to line, go to symbol (project-wide), go to definition (`F12`), back/forward history (`Alt+Left/Right`), find/replace with case and whole-word toggles, find in project (`F4`, clickable results). |
| **Explorer** | Tree with lazy loading, new file / folder, rename, delete, drag-resizable sidebar and panel. |
| **Terminal** | Full terminal emulator (VT100/xterm: cursor addressing, 256/truecolor, scroll regions, alternate screen, 3000-line scrollback, bracketed paste, mouse select-to-copy) on a pty (ConPTY on Windows). `Alt+T` opens a shell in the project folder; up to four terminals; `top`, `vim` and friends work. `Alt+J` returns to the editor. |
| **Console** | A KingCobra REPL in the panel (`Alt+C`): type expressions or statements, history with Up/Down, `Console: Restart REPL with Current File` loads your `.kc` file into the session. While the debugger is paused the same console evaluates expressions **in the selected stack frame** (assignments included). |
| **Debugger** | `Alt+F5` starts `kingcobra debug` on the current file. `F9` (or a click on the line number) toggles a breakpoint, `F5` continue, `F10` step over, `F11` step into, `Shift+F11` step out, `Shift+F5` stop. The Debug panel shows the call stack, variables (locals + globals of the selected frame) and watch expressions (`a` adds one); the current line is highlighted in the editor, breakpoints are red, and an uncaught runtime error stops the program with everything still inspectable. `F10` / `F11` with no session start one and stop on the first statement. Protocol: [DEBUGGER.md](DEBUGGER.md). |
| **KCM and SSM files** | Syntax highlighting, completion, snippets and comment toggling for `.kcm` music scores (tracks, notes, durations, chords, patterns, samples, timestamps), `.kcm` **kcmodel** 3D models (detected from the first line), and `.ssm` / `.ssml` SSML speech markup for `std.tts` (tags, attributes, entities, comments; typing `>` auto-closes a tag). |
| **Everything is discoverable** | `F1` opens the command palette (all commands + shortcuts); menus open with `Alt+M` or `Alt+F/E/S/V/R/B/H`. |

## Key bindings

`Help > Keyboard Shortcuts` opens the full generated list. The essentials:

| | | | |
|---|---|---|---|
| `Ctrl+S` save | `Ctrl+O` open path | `Ctrl+P` quick open | `F1` palette |
| `Ctrl+Z / Ctrl+Y` undo / redo | `Ctrl+F / Ctrl+R` find / replace | `F3 / Shift+F3` next / prev | `Ctrl+G` go to line |
| `Ctrl+D` duplicate line | `Ctrl+K` delete line | `Ctrl+/` comment | `Alt+Up/Down` move line |
| `F5` run | `F6` check | `F7` test | `F8` next problem |
| `Ctrl+Alt+L` format | `F12` definition | `Ctrl+T` symbols | `F4` find in project |
| `Ctrl+B` sidebar | `Ctrl+J` output panel | `Ctrl+E` explorer focus | `Ctrl+Q` quit |
| `Alt+T` terminal | `Alt+C` console | `Alt+D` debug panel | `Alt+J` panel <-> editor |
| `Alt+F5` debug | `F9` breakpoint | `F10` / `F11` step over / into | `Shift+F11` step out |

In the Terminal and Console tabs the keys go to the program (shell shortcuts like `Ctrl+R` and `Ctrl+W` work);
only the function keys, `Alt+J/T/C/D/M` and `Ctrl+PgUp/PgDn` stay with the IDE. `Shift+PgUp/PgDn` and the mouse wheel scroll the terminal history.

Tab/Shift+Tab indent selections; typing a snippet name then Tab expands it.

## Configuration

`~/.config/kcide/config` (`%APPDATA%\kcide\config` on Windows), `key = value` per line:
`theme`, `tab_width`, `use_spaces`, `check_on_save`, `save_before_run`, `auto_complete`,
`trim_trailing_whitespace`, `show_whitespace`, `show_sidebar`, `line_numbers`, `auto_pairs`, `kc_path`.
Most toggles are also in the command palette and are saved automatically.

## Source layout (`ide/src`)

| File | Role |
|---|---|
| `util.c` | memory/strings, UTF-8, paths, directory listing, fuzzy match |
| `term.c` | raw terminal, VT input decoding (keys, SGR mouse, bracketed paste), diffing cell screen |
| `syntax.c` | KingCobra tokenizer, symbol scanner, word lists, themes |
| `doc.c` | line-array document, undo/redo, highlight cache, motions, bracket matching |
| `editor.c`, `editor_view.c` | editing commands, find/replace, rendering, mouse |
| `complete.c` | completion engine + std API scanner |
| `proc.c`, `output.c` | pty / ConPTY / pipe process runner, ANSI interpreter and diagnostic parser |
| `vt.c`, `panel.c` | terminal emulator, bottom panel (tabs, terminals, console) |
| `dbg.c` | debugger front end: channel, protocol, breakpoints, Debug panel |
| `lang.c` | KCM (music + model) and SSM highlighters, completion vocabularies |
| `ui.c`, `cmds.c`, `main.c` | menus, tabs, explorer, panel, overlays, commands, key map, config, event loop |

## Notes and limits

* Linux terminal behaviour is tested end-to-end (pty + terminal emulator, ASan/UBSan, random-key fuzzing, 240k-line files).
  The Windows backend (console, ConPTY terminals, named-pipe debug channel) builds clean with MinGW but has **not been run on a real Windows machine yet**.
* Terminals cannot distinguish some chords (e.g. `Ctrl+Shift+F`), so a few commands use function keys instead.
* Not included yet: code folding, multi-cursor, soft wrap, regex search, expandable variable trees in the debugger, conditional breakpoints.
