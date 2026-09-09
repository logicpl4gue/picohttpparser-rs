# picohttpparser-rs

Drop-in **Rust rewrite of [PicoHTTPParser](https://github.com/h2o/picohttpparser)**
that preserves the original's observable behavior and C-facing API/ABI closely
enough that existing consumers can use it without application-level changes.

The original is treated as a **behavioral oracle**: we verify by differential
testing, fuzzing, and the upstream test suite — and we publish losses as well
as wins. See `picohttpparser-rs-plan.md` for the full project plan.

## Status — Milestone 3 (response parser)

- ✅ Milestone 0 baseline: pinned upstream revision
  `f4d94b48b31e0abae029ebeafcfd9ca0680ede58` in `reference/` (files +
  SHA-256 manifests + `PINNED.md`).
- ✅ Milestone 1 ABI shell: `src/ffi.rs` exports all five C symbols
  (`phr_parse_request`, `phr_parse_response`, `phr_parse_headers`,
  `phr_decode_chunked`, `phr_decode_chunked_is_in_data`) as **stubs** that
  pin the ABI (symbol names, `extern "C"` calling convention, argument/return
  types, `#[repr(C)]` struct layout via compile-time asserts) but return `-1`
  (`0` for `_is_in_data`) — no parsing yet. Rust↔C mapping table in
  `docs/api.md`.
- ✅ C baseline: upstream `bench` (10M iters, **mean 2.041s / 204.1 ns/parse
  over 6 trials** after 1 warmup, i5-12400F) and full upstream suite
  (**299 assertions, 8/8 subtests pass**) via `CC=gcc` (w64devkit, `D:/Tools`);
  `prove` absent so the TAP binary runs directly; sanitizers unavailable in
  this GCC (recorded in `results/baseline.json`, never fabricated).
- ✅ Milestone 2 (request parser): safe core + FFI seam; 87,052 differential
  cases vs C with 0 mismatches (`CC=gcc bash scripts/diff_request.sh`, log
  in `results/difftest-request.log`); unit vectors in `tests/request.rs`;
  corpus in `tests/corpus/request/` (60 files).
- ✅ Milestone 3 (response parser): `response` reuses the shared core;
  20,678 differential cases vs C with 0 mismatches
  (`CC=gcc bash scripts/diff_response.sh`, log in
  `results/difftest-response.log`); unit vectors in `tests/response.rs`;
  corpus in `tests/corpus/response/` (34 files).
- ⏳ Not started: headers/chunked parsers (Milestones 4–5), fuzzing,
  benchmarks, H2O integration.

## Layout

```
reference/            Pinned upstream (immutable) + PINNED.md + SHA256SUMS
src/                  Rust crate — core.rs (shared safe core), request.rs (M2),
                      response.rs (M3), ffi.rs (C ABI seam + remaining stubs)
docs/                 api.md, methodology.md, compatibility.md, divergences.md
results/              baseline.json, README.md (machine-readable evidence)
scripts/              run_baseline.sh, smoke_abi.c (C link+call test vs staticlib),
                      difftest_request.c + diff_request.sh (Layer-2 differential harness),
                      difftest_response.c + diff_response.sh (response harness)
```

## Commands

```bash
cargo build            # builds rlib + cdylib + staticlib
cargo test             # tests/abi.rs (layout + entry guards) + tests/request.rs +
                       # tests/response.rs (parser vectors)
CC=gcc bash scripts/diff_request.sh   # Layer-2 differential: C oracle vs release cdylib,
                                       # 43,372 cases, tees results/difftest-request.log
CC=gcc scripts/run_baseline.sh   # rebuilds C baseline, verifies pin, writes results/baseline.json
# C smoke test vs the Rust staticlib (needs cargo build first):
gcc -Ireference -o target/c-baseline/smoke_abi scripts/smoke_abi.c target/debug/picohttpparser_rs.lib
(cd reference && sha256sum -c SHA256SUMS)   # verifies pinned reference integrity
```

Honesty policy: `results/baseline.json` contains only real measurements;
missing tools are recorded as `skipped` with a reason, never approximated.

## License

MIT — the upstream source is dual-licensed (MIT / Perl); see `reference/`
headers.