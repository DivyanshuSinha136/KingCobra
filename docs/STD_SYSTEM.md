# `std.system`

Native OS interaction: machine information, environment variables, script
arguments, paths and the filesystem tree, clocks and randomness, and running
other programs. Linux and Windows.

```kc
import std.system;
```

`std.system` is split like `std.nn` and `std.rhex` — one C file per concern
(`std/system/system.c`, `system_info.c`, `system_path.c`, `system_time.c`,
`system_proc.c`, shared `system_glue.h`). File *contents* stay in `std.io`;
`std.system` is about the tree, the machine, and the processes around them.

## Error model

There is no try/except yet, so the module follows the same split as
`std.mmio` / `std.rhex`:

* **Programming errors are fatal** with a positioned message: wrong argument
  count or type, a freed process handle, a bad option, a NUL in an argv entry.
* **Environmental failures return `null` (or `false` for yes/no operations)**
  and set `system.last_error()` — file not found, permission denied, command
  not found, unsupported on this OS. `system.clear_error()` resets it.

## `comptime`

Pure reads are allowed inside `comptime { }`: `platform`, `os`, `arch`,
`endian`, `getenv`/`has_env`/`env_list`/`expand_env`, `cwd`, `pid`, every
path-string function, and metadata reads (`exists`, `stat`, `list_dir`,
`which`, `realpath`, …). Anything that changes the outside world, starts a
process, or reads a **volatile** value is refused with a specific error:
`time`, `monotonic`, `sleep`, `random_*`, `mem_free`, `uptime`, `load_avg`,
`disk_usage`, every `run`/`spawn` function, and every create/delete/rename/
chdir/chmod operation. `date()`, `utc_offset()` and `format_time()` are
allowed when given an explicit timestamp, refused when they would read "now".

## Machine information

| Function | Returns |
|---|---|
| `os()` | `"linux"`, `"macos"`, `"windows"`, `"freebsd"`, `"unix"` |
| `arch()` | `"x86_64"`, `"aarch64"`, `"x86"`, `"arm"`, `"riscv64"`, `"ppc64"`, `"unknown"` |
| `endian()` | `"little"` / `"big"` |
| `os_version()` | kernel release (`uname -r`) / Windows `major.minor.build` |
| `hostname()`, `username()`, `home_dir()`, `temp_dir()` | strings (`null` + `last_error` if unknowable) |
| `exe_path()` | path of the running `kingcobra` binary (Linux, Windows) |
| `cpu_count()`, `page_size()` | ints |
| `mem_total()` | physical memory in bytes |
| `mem_free()` | memory *available to new programs* in bytes (`MemAvailable` on Linux) |
| `uptime()` | seconds since boot (float) |
| `load_avg()` | `[1min, 5min, 15min]` (not on Windows → `null`) |
| `disk_usage(path=".")` | `DiskUsage { total, free, used }` bytes (`free` = space available to non-root) |
| `is_admin()` | root on Unix / elevated Administrator on Windows |
| `is_tty(fd=1)`, `term_size()` | bool / `[columns, rows]` or `null` |
| `info()` | one `SystemInfo { os, os_version, arch, hostname, cpus, mem_total, page_size, pointer_bits, kingcobra }` |
| `platform()` | (v0.1, kept) `"linux"` or `"windows"` |

## Environment and arguments

`getenv(name)` (returns `""` if unset — v0.1 behaviour), `getenv_or(name, default)`,
`has_env(name)`, `setenv(name, value)`, `unsetenv(name)`,
`env_list()` → `["NAME=value", ...]`.

`expand_env(text)` expands `$NAME`, `${NAME}`, `$$` (a literal `$`) and a
leading `~` / `~/`. Unset variables expand to the empty string, like a shell.

`args()` → the arguments after the script path
(`kingcobra run prog.kc a b` → `["a", "b"]`); `script_path()`.

`cwd()`, `pid()`, `exit(code=0)` (flushes output first, code masked to 0–255).

> v0.1 bug fixed: on Windows `setenv` wrote the process environment block but
> `getenv` read the C runtime's stale copy, so a variable you had just set was
> invisible. Both now use the Win32 calls.

## Paths (pure string functions)

Understand `/` everywhere and `\` plus drive prefixes (`C:\`, `C:`, UNC) on Windows.

| Function | Notes |
|---|---|
| `path_sep()` | `"/"` or `"\\"` |
| `path_join(a, b, ...)` | an absolute part restarts the path; empty parts are skipped |
| `basename(p)`, `dirname(p)` | `dirname("a")` = `"."`, `dirname("/a")` = `"/"`, trailing separators ignored |
| `extension(p)`, `stem(p)` | `".gz"` for `a.tar.gz`; `.bashrc` has no extension; `"a."` → `"."` |
| `is_absolute(p)` | |
| `normalize(p)` | lexical: collapses `//`, drops `.`, resolves `..`; keeps leading `..` on relative paths; `..` above `/` stays at `/`; `""` → `"."` |
| `abspath(p)` | cwd-relative then normalized (does not touch the disk) |
| `path_parts(p)` | `["/", "usr", "local"]` — root first if absolute |

## Filesystem

Reads (allowed in comptime): `exists`, `is_dir`, `is_file`, `is_symlink`,
`stat(p)` → `FileStat { size, mtime, mode, is_dir, is_file, is_symlink }`,
`access(p, "rwx")`, `list_dir(p=".")` (sorted names, no `.`/`..`),
`realpath(p)` (symlinks resolved on POSIX; on Windows a full path, existence checked),
`which(cmd)` (searches `PATH`, and `PATHEXT` on Windows).

Changes (refused in comptime), all returning `true` / `false` + `last_error`:

* `mkdir(p, parents=false)` — with `parents=true` an existing directory is success.
* `rmdir(p, recursive=false)` — recursive mode **never follows symlinks** (it removes the link, not the target) and **refuses** a filesystem root, a drive root, the empty path, and the home directory.
* `remove_file(p)` — files and symlinks, never directories.
* `rename(from, to)` — replaces an existing target; on POSIX it is atomic.
* `copy_file(from, to, overwrite=true)` — writes a sibling temp file and renames it into place, so a failure never leaves a half-written destination; keeps permission bits on POSIX.
* `chdir(p)`, `chmod(p, mode)` (Windows honours only the write bit).
* `make_temp_file(prefix="kc")`, `make_temp_dir(prefix="kc")` — created atomically (`mkstemp`/`mkdtemp`, `CREATE_NEW`); **the caller deletes them**.

## Time and randomness

`time()` (float epoch seconds), `time_ms()`, `monotonic()` (use this to time
things), `sleep(seconds)` — resumes across signals, rejects negative/NaN.

`date(ts=now, utc=false)` → `DateTime { year, month, day, hour, minute, second, weekday (0=Sunday), yearday (1..366), is_dst, utc_offset }`.
`format_time(ts, fmt="%Y-%m-%d %H:%M:%S", utc=false)` is `strftime`
(Windows' `strftime` supports a smaller set of `%` codes). `utc_offset(ts=now)`
is seconds east of UTC with DST applied at that instant. Negative and
fractional timestamps floor correctly; the valid range is years 1–9999.

`random_bytes(n)`, `random_hex(n)`, `random_int(lo, hi)` (inclusive, no
modulo bias) draw from the OS CSPRNG (`/dev/urandom`, `RtlGenRandom`).
`n` is capped at 1 MiB per call.

## Running programs

| Function | Behaviour |
|---|---|
| `exec(cmd)` | shell, stdio inherited, returns the exit code (v0.1) |
| `run(cmd, input=null, timeout=null, cwd=null)` | shell; captures stdout and stderr **separately**; optional stdin and timeout |
| `run_argv(argv, input=null, timeout=null, cwd=null)` | **no shell** — `argv` is passed verbatim, so there is nothing to quote and nothing to inject |
| `output(cmd, timeout=null)` | stdout with the trailing newline stripped, or `null` if the command failed (stderr text goes to `last_error()`) |

`run`/`run_argv` return `RunResult { code, stdout, stderr, timed_out, signal, ok }`.
A command that cannot be started at all (not found, bad `cwd`) returns `null`.
Exit status is the real code, or `128 + signal` if the child was killed by a
signal (like a shell's `$?`), with `signal` set. Output is binary-safe.

stdout, stderr and stdin are pumped together with `poll()`, so a child that
produces megabytes of output while being fed megabytes of input cannot
deadlock. On timeout the child gets `SIGTERM`, half a second of grace, then
`SIGKILL`; captured children are in their own process group so grandchildren
(`sleep 30 &` inside `sh -c`) die too.

### Long-lived children

```kc
let p = system.spawn(["cat"], true);     // capture = true -> pipes
system.proc_write(p, "ping\n");
system.proc_close_stdin(p);
print(system.wait(p, 5.0));              // 0 — or null if 5 s elapsed first
print(system.proc_read(p));              // "ping\n"
system.proc_free(p);
```

`spawn(argv, capture=false, cwd=null)`, `wait(p, timeout=null)` (exit code or
`null` on timeout), `poll(p)` (code or `null` while running), `kill(p, force=false)`
(`SIGTERM`/`SIGKILL`), `proc_pid(p)`, `proc_read(p)` / `proc_read_err(p)`
(new output since the last call; **never block**), `proc_write(p, data)`,
`proc_close_stdin(p)`, `proc_free(p)` (kills a still-running child and reaps it,
so no zombie is left). `proc_read`/`proc_write` need `capture=true`.
Without `capture`, the child shares your terminal.

## Platform notes

* **Windows** is implemented with `CreateProcess` and anonymous pipes. `kill`
  terminates only the process itself (no job object, so no group kill),
  `proc_write` is limited to 512 KiB per call, and `exe_path`, `is_admin`,
  `os_version` use `GetModuleFileName`, `CheckTokenMembership`, `RtlGetVersion`.
  The Windows code paths were written against the Win32 documentation and
  reviewed carefully, **but not compiled or run** in the development sandbox
  (Linux-only) — the same caveat `kc_platform.h` carries. Please build with
  MinGW and run `make test`.
* **macOS/BSD** build through the POSIX paths; `exe_path`, `uptime` and
  `load_avg` fall back to a clear `null` + `last_error` where the Linux-specific
  source (`/proc`) is missing.

## Compatibility

All eight v0.1 functions keep their names and return types
(`exec`, `getenv`, `setenv`, `platform`, `cwd`, `sleep`, `exit`, `pid`).
Small behaviour changes: `exec` flushes your output first so it appears in
order and reports a signal death as `128 + signal`; `setenv` type-checks its
arguments; `sleep` rejects negative durations; `cwd` returns `null` instead of
aborting when `getcwd` fails.
