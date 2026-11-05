# Stability and compatibility policy

KingCobra is **1.x**. This page says what you can rely on within a major version, and how changes are made.

## Versioning
`MAJOR.MINOR.PATCH` ([semantic versioning](https://semver.org)). A **patch** release (1.0.0 -> 1.0.1) only fixes bugs and
never changes documented behavior. A **minor** release (1.0 -> 1.1) adds features and stays compatible with
everything in the stable list below. A **major** release (1.x -> 2.0) is the only place a breaking change to that list
can happen, and it is announced under "Changed" in [CHANGELOG.md](CHANGELOG.md) with what to do about it. Bug fixes
that make a previously accepted invalid program an error (or the reverse for a clear compiler bug) may land in a minor
release and are listed under "Fixed".

## What is considered stable (changing it is a deliberate, announced decision)
- The syntax in [GRAMMAR.md](GRAMMAR.md) and the semantics in [LANGUAGE.md](LANGUAGE.md):
  operators and their precedence, integer semantics (exact 64-bit, wrapping `+ - *`), `==` by
  value, strict array bounds, `let`/`var`, scoping, `defer`, `try`/`catch`/`throw`, `match`.
- The memory model in [MEMORY.md](MEMORY.md): explicit `free`, use-after-free and double free are
  errors, **there is no garbage collector and none is planned**.
- Process exit statuses: `0` success; `1` a `kingcobra test` run had failing tests; `64` bad command
  line; `65` a program error (parse, type, or uncaught runtime error); `70` out of memory.
- The shape of caught error objects: `message`, `kind`, and (for runtime errors) `file`, `line`, `col`.
- Built-in function and method names and argument order in [LANGUAGE.md](LANGUAGE.md).

## What is not covered by the promise
- **Message wording.** Error messages are written for people; tests should match a distinctive
  fragment, not the whole line. The `file:line:col: error:` prefix is stable.
- **`unsafe`, pointers, `extern`, `comptime`** — stable in intent, but their corners (alignment of
  odd layouts, FFI argument types) may tighten.
- **The JIT** is an optimization only: it must never change results, and anything it declines to
  compile runs in the interpreter. Its coverage will grow.
- **Standard library modules** (`std.*`) are documented individually; modules marked experimental in
  their page may change in a minor release (they say so on their page).
- **Performance characteristics** improve over time and are not part of the contract (a
  `make pgo` build is faster than a plain one).
- Anything listed under "not in the language" in LANGUAGE.md may be added; adding syntax never breaks
  a valid program because new keywords are contextual.

## How a change is made
1. It is proposed in [ROADMAP.md](ROADMAP.md) with a reason.
2. It lands with tests: shell tests in `tests/smoke_test.sh` and, for language behavior, tests in
   `tests/lang_test.kc` (the language's own test suite, run with `kingcobra test`).
3. A behavior change is recorded in CHANGELOG.md under **Changed**, with a migration note.
4. Where practical the old behavior is first a warning, then an error, before removal.

## Verifying a build
`make test` and `bash tests/smoke_test.sh` run the full suite; `kingcobra test tests/lang_test.kc`
runs the language-level tests. The suite is also run under AddressSanitizer and UBSan with the pools
disabled (`-DKC_ENV_NOPOOL -DKC_HEAP_NOPOOL`), see [PERFORMANCE.md](PERFORMANCE.md).
