# picohttpparser-rs

Drop-in **Rust rewrite of [PicoHTTPParser](https://github.com/h2o/picohttpparser)**
that preserves the original's observable behavior and C-facing API/ABI closely
enough that existing consumers can use it without application-level changes.

The original is treated as a **behavioral oracle**: we verify by differential
testing, fuzzing, and the upstream test suite — and we publish losses as well
as wins. See `picohttpparser-rs-plan.md` for the full project plan.

## Status — Milestones 0–7 & 9 complete; M8 (plan M7) partial

> Numbering note: `picohttpparser-rs-plan.md` defines Milestone 3 as
> "Responses + Headers"; the repo implemented it as two milestones
> (M3 response, M4 headers), so repo numbers run one ahead of the plan
> from here on (repo M5 chunked = plan M4, repo M6 gate = plan M5,
> repo M9 benchmarks = plan M8; repo M8 is reserved for plan M7/H2O).

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
- ✅ C baseline: upstream `bench` (10M iters, **mean 2.129s / 212.9 ns/parse
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
- ✅ Milestone 4 (standalone header parser): `phr_parse_headers` wired to
  the shared core; 21,728 differential cases vs C with 0 mismatches
  (`CC=gcc bash scripts/diff_headers.sh`, log in
  `results/difftest-headers.log`); unit vectors in `tests/headers.rs`;
  corpus in `tests/corpus/headers/` (31 files).
- ✅ Milestone 5 (chunked decoder): stateful in-place `chunked` core with
  full decoder-state lockstep; 3,506 differential cases vs C with 0
  mismatches (`CC=gcc bash scripts/diff_chunked.sh`, log in
  `results/difftest-chunked.log`); unit vectors in `tests/chunked.rs`;
  corpus in `tests/corpus/chunked/` (31 files, incl. 1 MB chunk, overhead
  bomb + exact rule-boundary pairs).
- ✅ Milestone 6 = plan M5, Compatibility Gate: **PASSED**. Unmodified
  upstream `test.c` linked against the release cdylib: 8/8 subtests, 299
  assertions, 0 failures (`results/upstream-rust.log`, via
  `CC=gcc bash scripts/run_baseline.sh`). All four differential harnesses
  re-run fresh the same day: 132,964 cases, 0 mismatches. Divergences: 0
  unresolved (4 INTENTIONAL fail-closed rows). Details in
  `docs/compatibility.md`.
- ✅ Milestone 7 (fuzz campaign, plan M6): deterministic differential
  fuzzing, ~1.54M executions, 0 mismatches, 0 crashes either side
  (`bash scripts/fuzz_all.sh`; design + numbers in `docs/fuzzing.md`).
- ✅ Milestone 9 (benchmarks, plan M8): reproducible multi-category
  C-vs-Rust suite, 7/7 plan §11 categories with per-corpus ratios
  (`CC=gcc bash scripts/bench_compare.sh`; numbers + procedure in
  `docs/methodology.md`, raw data in `results/bench-compare.json`).
- ◐ Milestone 8 (plan M7): H2O-equivalent loopback proof **PASSED** —
  self-authored HTTP/1.1 consumer relinked unchanged against the Rust
  cdylib with byte-identical transcripts + Python ctypes second consumer
  (`results/integration.log`, `bash scripts/integ_http11.sh`). Genuine-
  consumer step (H2O or Plack/Starlet/Furl per plan §16) not yet attempted.

## Layout

```
reference/            Pinned upstream (immutable) + PINNED.md + SHA256SUMS
src/                  Rust crate — core.rs (shared safe core), request.rs (M2),
                      response.rs (M3), chunked.rs (M5, stateful),
                      ffi.rs (C ABI seam, no stubs remain)
docs/                 api.md, methodology.md, compatibility.md, divergences.md,
                      fuzzing.md
results/              baseline.json, README.md (machine-readable evidence)
scripts/              run_baseline.sh (C baseline + upstream suite vs Rust),
                      difftest_request.c + diff_request.sh,
                      difftest_response.c + diff_response.sh,
                      difftest_headers.c + diff_headers.sh,
                      difftest_chunked.c + diff_chunked.sh (Layer-2 harnesses),
                      fuzz_parse.c/.sh, fuzz_chunked.c/.sh, fuzz_triage.sh,
                      fuzz_all.sh (differential fuzz campaign),
                      integ_http11.c/.sh, integ_ctypes.py (loopback integration)
```

## Commands

```bash
cargo build            # builds rlib + cdylib + staticlib
cargo test             # tests/abi.rs (layout + entry guards) + tests/request.rs +
                       # tests/response.rs + tests/headers.rs + tests/chunked.rs
                       # (parser vectors)
CC=gcc bash scripts/diff_headers.sh    # Layer-2 differential, header blocks,
                                       # 21,728 cases, tees results/difftest-headers.log
CC=gcc bash scripts/diff_chunked.sh    # Layer-2 differential, chunked decoder,
                                       # 3,506 cases, tees results/difftest-chunked.log
CC=gcc bash scripts/diff_request.sh   # Layer-2 differential: C oracle vs release cdylib,
                                       # 87,052 cases, tees results/difftest-request.log
CC=gcc bash scripts/diff_response.sh  # same for responses, 20,678 cases
CC=gcc bash scripts/bench_compare.sh  # same-session C-vs-Rust interleaved bench,
                                       # writes results/bench-compare.json (internal numbers)
CC=gcc bash scripts/integ_http11.sh   # loopback integration: relinked C consumer
                                       # + ctypes consumer, writes results/integration.log
CC=gcc scripts/run_baseline.sh   # rebuilds C baseline, verifies pin, writes results/baseline.json
(cd reference && sha256sum -c SHA256SUMS)   # verifies pinned reference integrity
# (retired) scripts/smoke_abi.c asserted M1 stub values and linked the
# staticlib, which MinGW ld cannot satisfy (MSVC EH residue) — removed;
# the cdylib link+call path is proven by every diff_*.sh harness instead.
```

Honesty policy: `results/baseline.json` contains only real measurements;
missing tools are recorded as `skipped` with a reason, never approximated.

## License

MIT — the upstream source is dual-licensed (MIT / Perl); see `reference/`
headers.