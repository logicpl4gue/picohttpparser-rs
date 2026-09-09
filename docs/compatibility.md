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

## Regression corpus (`tests/corpus/request/`, `tests/corpus/response/`)

60 request files (23 valid + 37 malformed) and 34 response files (15 valid +
19 malformed), exercised by the differential harnesses above. Every future
mismatch gains a minimal corpus file here plus a row in
`docs/divergences.md` until resolved.

## Policy

- Any mismatch becomes: a stored reproduction, a regression test, and an
  entry in `docs/divergences.md` until resolved.
- If the original appears buggy, preserve compatibility first unless there is
  a compelling safety reason not to (see plan §19).