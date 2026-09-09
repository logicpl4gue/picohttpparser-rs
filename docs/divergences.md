# Divergences

Known disagreements between the Rust replacement and the pinned upstream
(PicoHTTPParser `f4d94b48`). Request parser shipped in Milestone 2 with
**zero behavioral divergences** (43,372 differential cases, 0 mismatches);
the entries below are deliberate fail-closed hardenings outside C's defined
behavior, kept visible per policy.

Status values per plan §19: `OPEN`, `FIXED`, `INTENTIONAL`, `REFERENCE BUG`,
`UNRESOLVED`.

| # | Input | C result | Rust result | Expected resolution | Status | Date discovered | Regression test |
|---|---|---|---|---|---|---|---|
| 1 | Any call with a null output pointer, null `buf` with `len != 0`, or null `headers` with capacity > 0 | Segfault (UB) | `-1`, outputs already zeroed stay zeroed | INTENTIONAL (never replicate crashes) | INTENTIONAL | 2026-09-09 | `tests/abi.rs::request_null_guards_return_error` |

Policy: never hide a compatibility problem. A `REFERENCE BUG`/`INTENTIONAL`
entry stays visible here even after resolution.