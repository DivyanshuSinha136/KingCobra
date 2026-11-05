# KingCobra showcase — real-world programs

Seven complete programs, each a job people actually do. Every one is deterministic, runs offline
(the REST demo talks to itself over loopback), and **asserts its own results**, so it doubles as a test:
`make showcase` runs them all, and `tests/smoke_test.sh` checks their key output lines.

| # | Program | The real-world job | What it shows off |
|---|---|---|---|
| 1 | `01_log_analyzer.kc` | Parse 3,000 web-server log lines; traffic, error rate, p50/p95/p99 latency, hourly chart, flag a credential-stuffing client; export JSON + CSV | `std.regx` named groups, `std.json`, `std.csv`, structs + `impl`, `enum`/`match`, closures, `defer` |
| 2 | `02_order_pipeline.kc` | Messy order CSV → schema-validated, priced invoices (tax, coupons, shipping) with a quarantine list of bad rows and reasons | JSON Schema validation, `try/catch/throw`, integer-cents money, invariants across JSON and CSV outputs |
| 3 | `03_support_triage.kc` | Support inbox → topic, sentiment, priority, suggested help article, merged duplicates | `std.nlp`: Naive Bayes (with held-out accuracy), sentiment, spell correction, BM25 search, extraction |
| 4 | `04_rest_api.kc` + `api/todo_server.kc` | A JSON REST service with bearer-token auth, plus a client exercising CRUD, validation and errors | `std.http` server **and** client, keep-alive client, `std.system.spawn` |
| 5 | `05_physics_lab.kc` | Artillery with air drag (best angle 45° → 36°) and a Verlet-integrated orbit that must conserve energy | `std.vector` (vec2/vec3, rotate, cross), numerical checks |
| 6 | `06_telemetry_protocol.kc` | Binary sensor protocol with CRC-16: encode, corrupt the stream, resync and recover; memory errors caught | `packed struct`, typed pointers over raw bytes, `buffer()`, bitwise ops, `defer`, leak check |
| 7 | `07_algorithms_speed.kc` | Fib, sieve of 2M, Collatz, big-number digits — each checked against published values, with timings | closure JIT, `alloc` + `defer free` |

```bash
make                                  # build ./kingcobra
make showcase                         # run all seven
./kingcobra run examples/showcase/03_support_triage.kc
./kingcobra --leaks examples/showcase/06_telemetry_protocol.kc
```

Notes learned while writing them (useful if you extend them): there is no `sin`/`cos` builtin (use
`vector.rotate2`), no `round` (add 0.5 and `int()`), `match` arms are expressions (use `if` for statements),
`from` is a keyword, and `pad_left`/`pad_right` are string methods (wrap numbers in `str()`).
