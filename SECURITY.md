# Security policy

## Supported versions

Security fixes go into the **latest release** and `main`.

| Version | Supported |
|---|---|
| Latest release (1.0.x) | ✅ |
| Older releases | ❌ — please upgrade |

## Reporting a vulnerability

**Please do not open a public issue for security problems.**

Report privately by either:

1. Using GitHub's **[Report a vulnerability](https://github.com/DivyanshuSinha136/kingcobra/security/advisories/new)** form, or
2. Emailing **divyanshu.sinha631@gmail.com** with the subject `KingCobra security`.

Please include the KingCobra version (`kingcobra info`), your platform, a minimal reproduction, and what you think the impact is.

You can expect an acknowledgement within about a week, and updates as the issue is investigated. Fixes are released with a
changelog note, and reporters are credited unless they prefer otherwise.

## What counts as a vulnerability

In scope, for example: memory-safety bugs in the interpreter, runtime or standard library (buffer overflows, use-after-free
in the C code), sandbox escapes from `comptime`, flaws in the HTTP/TLS client's certificate verification, and unsafe handling
in `install.sh` or release packaging (checksum verification).

Out of scope: behavior that is explicitly unsafe by design and documented as such — `unsafe { }` blocks, `std.rhex` raw
machine code, `std.mmio` / port / MSR access, raw sockets, and running untrusted `.kc` programs, which can use the file,
network and process APIs.
