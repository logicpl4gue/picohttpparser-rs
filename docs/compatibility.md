# Compatibility

Tracking document for upstream-test and differential-test results. Every row
must be backed by a recorded run; no silent exclusions.

Status values: `PASS`, `FAIL`, `SKIPPED` (+ reason), `PENDING`.

## Layer 1 — upstream test suite (`reference`, `make test`)

| Date | Total | Passed | Failed | Skipped | Skip reason |
|---|---|---|---|---|---|
| *(none yet)* | — | — | — | — | Requires C compiler + `make` + `prove` + `picotest` submodule; none available at baseline time (see `results/baseline.json`) |

Target: 100% of applicable upstream tests passing on the Rust replacement.

## Layer 2 — differential testing (C vs Rust)

| Date | Cases | Mismatches | Unresolved | Notes |
|---|---|---|---|---|
| 2026-09-09 | 43,372 | 0 | 0 | Request parser. `scripts/diff_request.sh`: 57 corpus files × caps {0,1,2,3,5,16,64} × full + every prefix (streaming, `last_len` = prev len). Oracle = pinned C at `-O2` (renamed symbols); Rust = release cdylib (shipped artifact). Compared: ret, method, path, version, count, every name/value as pointer+length equality into the shared buffer, **including the in-progress header slot on error paths** (two-phase name/value mirror). Stored log: `results/difftest-request.log`. Repro: `CC=gcc bash scripts/diff_request.sh`. |

## Regression corpus (`tests/regression/`, `tests/compatibility/`, `tests/malformed/`)

Not started — empty until Milestone 2.

## Policy

- Any mismatch becomes: a stored reproduction, a regression test, and an
  entry in `docs/divergences.md` until resolved.
- If the original appears buggy, preserve compatibility first unless there is
  a compelling safety reason not to (see plan §19).