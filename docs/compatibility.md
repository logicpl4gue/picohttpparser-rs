# Compatibility

Tracking document for upstream-test and differential-test results. Every row
must be backed by a recorded run; no silent exclusions.

Status values: `PASS`, `FAIL`, `SKIPPED` (+ reason), `PENDING`.

## Layer 1 — upstream test suite (`reference`, `make test`)

| Date | Total | Passed | Failed | Skipped | Skip reason |
|---|---|---|---|---|---|
| 2026-09-09 | 8 | 8 | 0 | 0 | — (299 assertions; `prove` absent so the TAP binary runs directly; sanitizers unavailable in this GCC — see `results/baseline.json`) |

Target: 100% of applicable upstream tests passing on the Rust replacement.

## Layer 2 — differential testing (C vs Rust)

| Date | Cases | Mismatches | Unresolved | Notes |
|---|---|---|---|---|
| 2026-09-09 | 87,052 | 0 | 0 | Request parser. `scripts/diff_request.sh`: 60 corpus files × caps {0,1,2,3,5,16,64} × full + every prefix streaming (`last_len` = prev len) + every strict prefix cold (`last_len` = 0). Oracle = pinned C at `-O2` (renamed symbols); Rust = release cdylib (shipped artifact). Compared: ret, method, path, version, count, every name/value as pointer+length equality into the shared buffer, **including the in-progress header slot on error paths** (two-phase name/value mirror). Stored log: `results/difftest-request.log`. Repro: `CC=gcc bash scripts/diff_request.sh`. |
| 2026-09-09 | 20,678 | 0 | 0 | Response parser. `scripts/diff_response.sh`: 34 corpus files × same caps × full + streaming prefixes + cold prefixes. Compared: ret, version, **status (incl. partial value on digit failure)**, reason (incl. empty + space-strip), count, every name/value as pointer+length equality incl. in-progress slot. Stored log: `results/difftest-response.log`. Repro: `CC=gcc bash scripts/diff_response.sh`. |
| 2026-09-09 | 21,924 | 0 | 0 | Standalone header parser. `scripts/diff_headers.sh`: 32 corpus files (incl. 64/65-header cap-boundary blocks) × same caps × full + streaming prefixes + cold prefixes. Compared: ret, count, every name/value as pointer+length equality incl. in-progress slot. Stored log: `results/difftest-headers.log`. Repro: `CC=gcc bash scripts/diff_headers.sh`. |
| 2026-09-09 | 3,698 | 0 | 0 | Chunked decoder. `scripts/diff_chunked.sh`: 32 corpus files (incl. 1 MB chunk, 150 KB bomb + truncated cut, exact 100 KiB / ratio boundary pairs, pipelined message, trailer leftover, lowercase hex) × trailer {0,1} × single call + exhaustive two-way splits (quarters for >8 KB files) + three-way quarter splits + dirty initial decoder states. Compared per call: ret, decoded length, is_in_data, full decoder struct, entire working buffer. Stored log: `results/difftest-chunked.log`. Repro: `CC=gcc bash scripts/diff_chunked.sh`. |

## Compatibility Gate (plan M5 / repo M6) — PASSED 2026-09-09

| Check | Result | Evidence |
|---|---|---|
| Upstream suite vs C oracle | 8/8 subtests (7 parser groups + guard-page harness check), 299 TAP checks (291 parser assertions), 0 failures | `results/upstream-tests.log` |
| Upstream suite vs Rust (unmodified `test.c` linked against the release cdylib) | same 8/8, 299/291, 0 failures | `results/upstream-rust.log`, `baseline.json:upstreamVsRust`; repro `CC=gcc bash scripts/run_baseline.sh` |
| Differential, request | 87,052 cases, 0 mismatches | `results/difftest-request.log` |
| Differential, response | 20,678 cases, 0 mismatches | `results/difftest-response.log` |
| Differential, headers | 21,924 cases, 0 mismatches | `results/difftest-headers.log` |
| Differential, chunked | 3,698 cases, 0 mismatches | `results/difftest-chunked.log` |
| Known divergences unresolved | 0 (4 rows, all INTENTIONAL fail-closed hardenings) | `docs/divergences.md` |

Total: 133,352 differential cases + 299 upstream assertions × 2 targets,
zero failures, zero unresolved divergences. Optimization (plan M8+) may
begin: behavior is pinned.

## Fuzz Campaign (plan M6 / repo M7) — CLEAN 2026-09-09

| Check | Result | Evidence |
|---|---|---|
| Fuzz, request/response/headers | 200,000 mutated cases (seed 11) + per-entry 100,000 × 3 (seed 7), 0 mismatches | `target/fuzz/fuzz-all-20260909T042305Z.log` (consolidated run; per-entry rows in `docs/fuzzing.md`) |
| Fuzz, chunked (stateful, splits, dirty decoders) | 398,851 calls (seed 11), 0 mismatches | same log |
| Crashes, either side | 0 | same log |
| Triage pipeline | validated on synthetic marker (33 B → 6 B, 92 oracle runs) | `scripts/fuzz_triage.sh` |
| Determinism | byte-identical rerun verified | — |

Design, reproduction procedure, and promotion policy in `docs/fuzzing.md`.
No mismatch needed triage; no corpus promotion, no new divergence row.

## Loopback integration (plan M7 / repo M8) — PARTIAL 2026-09-09

| Check | Result | Evidence |
|---|---|---|
| H2O-equivalent consumer relink | same source builds vs C oracle and vs Rust cdylib; loopback HTTP/1.1 exchange transcripts byte-identical | `results/integration.log`; repro `CC=gcc bash scripts/integ_http11.sh` |
| Python ctypes second consumer | all five entry points parse + assert green | same log |
| Genuine consumer (H2O/Plack/Starlet/Furl) | NOT attempted — full H2O server build infeasible on this machine (no OpenSSL dev libs; Windows unsupported upstream) | — |

Scope (honest): single request/response shape over loopback TCP, all five
entry points exercised, split delivery forced by small fixed recv sizes;
no keep-alive/second exchange, no large bodies, no malformed traffic, no
concurrency. Error-path and scale coverage stays with the differential and
fuzz campaigns. Procedure in `docs/methodology.md`.

## Regression corpus (`tests/corpus/request|response|headers|chunked/`)

60 request files (23 valid + 37 malformed), 34 response files (15 valid +
19 malformed), 32 header files (17 valid + 15 malformed), and 32 chunked
files (13 valid + 19 malformed), exercised by
the differential harnesses above. Every future
mismatch gains a minimal corpus file here plus a row in
`docs/divergences.md` until resolved.

## Policy

- Any mismatch becomes: a stored reproduction, a regression test, and an
  entry in `docs/divergences.md` until resolved.
- If the original appears buggy, preserve compatibility first unless there is
  a compelling safety reason not to (see plan §19).