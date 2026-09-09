# Divergences

Known disagreements between the Rust replacement and the pinned upstream
(PicoHTTPParser `f4d94b48`). Request parser shipped in Milestone 2 with
**zero behavioral divergences** (87,052 differential cases, 0 mismatches);
the entries below are deliberate fail-closed hardenings outside C's defined
behavior, kept visible per policy.

Status values per plan §19: `OPEN`, `FIXED`, `INTENTIONAL`, `REFERENCE BUG`,
`UNRESOLVED`.

| # | Input | C result | Rust result | Expected resolution | Status | Date discovered | Regression test |
|---|---|---|---|---|---|---|---|
| 1 | Any call with a null output pointer, null `buf` with `len != 0`, or null `headers` with capacity > 0 | Segfault (UB) **once a header write is reached**; defined results (blank-line success, `-2` on empty/incomplete/gate-failing input) otherwise — all converted to `-1` | `-1` (on the `buf`-null path the guard fires before zeroing, so `*num_headers` keeps its entry value) | INTENTIONAL (never replicate crashes; coarse fail-closed guard) | INTENTIONAL | 2026-09-09 | `tests/abi.rs::request_null_guards_return_error` |
| 2 | Chunked decoder with a forged `_state` outside 0–7 | Aborts (assert build) or hangs reswitching forever (NDEBUG build) | `-1` via `unreachable!` + the seam's panic guard; no data copied, only `_total_read` advanced at entry | INTENTIONAL (a library must never hang/abort the host on bad input) | INTENTIONAL | 2026-09-09 | Unreachable from any zero-init + transition sequence (state lockstep memcmp'd in every differential case) |
| 3 | Chunked entry null corners: `buf` NULL with `*bufsz == 0`; `phr_decode_chunked_is_in_data(NULL)` | `-2` without dereferencing `buf`; segfault on the NULL query | `-2` (null empty input reads as empty); `0` on the NULL query | INTENTIONAL (fail-closed superset: every other null still `-1`) | INTENTIONAL | 2026-09-09 | `tests/chunked.rs` null-corner asserts (added M5 review) |

Policy: never hide a compatibility problem. A `REFERENCE BUG`/`INTENTIONAL`
entry stays visible here even after resolution.