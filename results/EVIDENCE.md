# Final evidence table (Milestone 10, plan §17)

Consolidated readout of the experiment at `dfdfa0c` (code under test `fb5e55e`;
`dfdfa0c` itself is docs + tier JSON only). Every figure cites the primary
record it was copied from — this file aggregates, it does not measure.
Machine: i5-12400F, Atlas Power Scheme, w64devkit GCC 16.2.0, rustc 1.96.1.
Pin: `f4d94b48` (`reference/`, SHA-256 verified). Honesty policy applies:
losses printed alongside wins; skipped items carry reasons, never guesses.

## Compatibility

| Check | Result | Primary record |
|---|---|---|
| Upstream suite vs C oracle | 8/8 subtests, 299 TAP checks, 0 failures | `results/upstream-tests.log`, `baseline.json:upstreamTestSuite` |
| Upstream suite vs Rust cdylib (unmodified `test.c`) | 8/8, 299, 0 failures | `results/upstream-rust.log`, `baseline.json:upstreamVsRust` |
| Differential request (60 files) | 87,052 cases, 0 mismatches | `results/difftest-request.log` (2026-09-09T04:39Z, `fb5e55e-dirty`) |
| Differential response (34 files) | 20,678 cases, 0 mismatches | `results/difftest-response.log` (same run) |
| Differential headers (32 files) | 21,924 cases, 0 mismatches | `results/difftest-headers.log` (same run) |
| Differential chunked (32 files) | 3,698 cases, 0 mismatches | `results/difftest-chunked.log` (same run) |
| **Differential total** | **133,352 cases, 0 mismatches** | sum of the four logs above |
| Known divergences unresolved | 0 (4 rows, all INTENTIONAL fail-closed) | `docs/divergences.md` |
| Unit vectors (`cargo test --offline`) | 30/30 pass, 0 failures | `tests/{abi,request,response,headers,chunked}.rs` |

## Fuzzing

| Check | Result | Primary record |
|---|---|---|
| Parse fuzz (req/resp/headers), seed 11 | 200,000 cases, 0 mismatches, 0 crashes either side | `target/fuzz/fuzz-all-20260909T042305Z.log` (gitignored build scratch; table in `docs/fuzzing.md`) |
| Chunked fuzz (stateful/splits/dirty), seed 11 | 398,851 calls, 0 mismatches, 0 crashes | same log |
| Per-entry sweeps, seed 7 | 100,000 × 3 parse + 200,190 + 100,060 + 40,058 chunked calls, clean | same log (≈1.94M executions aggregate) |
| Triage pipeline | validated on synthetic marker (33 B → 6 B, 92 oracle runs) | `scripts/fuzz_triage.sh` |
| Determinism | byte-identical rerun verified | `docs/fuzzing.md` |

## Performance (internal engineering numbers, single Windows machine)

Anchor: C `-O2` vs pinned release profile (opt-level=2, CGU1, LTO), cdylib
via shared C harness, 7 trials, trial 0 discarded.

| Row | Rust/C | Stability | Primary record |
|---|---|---|---|
| Anchor (upstream ~620 B REQ) | **0.9746** | CHECK-spread>5% | `results/bench-compare.json` |
| tiny | 1.4123 | CHECK | same file |
| typical | 1.1544 | CHECK | same file |
| large | 0.8166 (Rust faster) | CHECK | same file |
| response | 1.2084 | CHECK | same file |
| chunked | **1.1853** (was 1.6794 pre EXP-1 flatten) | CHECK | same file |
| malformed (reject path) | 1.9472 | CHECK | same file |
| streaming (incremental) | 1.1716 | CHECK | same file |
| O3-pair anchor (labeled tier) | 0.9502 | CHECK | `results/bench-compare-o3.json` |
| O3 malformed anomaly | **3.3907 tier record; P1 same-session control: Rust-side O3/O2 = 1.68x REAL** (identical harness; C drifted the other way) — cause open, guard A/B queued | OK | same file + `docs/methodology.md` |
| Native pair (labeled tier, historical) | 1.4499 — C pcmpestri leaps, Rust scalar+SWAR flat; the SIMD prize quantified | — | `results/bench-compare-native.json` |
| PGO pair | SKIPPED — no `llvm-profdata`, `C:` 100% full; procedure stands | — | `docs/methodology.md` |

Limits: no pinning/governor control; whole-loop timer only (no p50/p95/p99);
per-category ns/parse comparable only within a category; never mix tiers
into headline claims. See `docs/methodology.md` honest-limits section.

## Implementation

| Metric | Value | Source |
|---|---|---|
| Rust src LOC | 1,527 (`core` 359, `ffi` 601, `chunked` 287, `request` 121, `response` 107, `lib` 52) | `wc -l src/*.rs` |
| `unsafe` blocks / FFI exports | 7 blocks, 5 exports — all in `src/ffi.rs`; parser core zero-`unsafe` | `grep -c` both patterns |
| Panic policy | core panic-free (`unwrap_used`/`expect_used`/`unsafe_op_in_unsafe_fn` denied); `catch_unwind` → `-1` at seam | `Cargo.toml` lints, `src/ffi.rs` |
| Allocations per parse | 0, by construction (zero-copy offsets; pointers only at FFI boundary) | design, plan §14 |

## Integration

| Check | Result | Primary record |
|---|---|---|
| Loopback relink (all 5 entry points) | PASS — transcripts byte-identical + ctypes 5/5 green | `results/integration.log` (`fb5e55e`) |
| Genuine consumer (H2O/Plack/Starlet/Furl) | NOT attempted — H2O infeasible here (no OpenSSL dev libs, Windows unsupported); cheapest identified path is HTTP::Parser::XS swap + Furl under Strawberry Perl (not installed) | `docs/compatibility.md` |

## Plan §14 minimum-viable checklist

100% upstream compat ✓ · 0 unresolved divergences ✓ · 0 allocations ✓ ·
stable fuzz, 0 Rust crashes ✓ · anchor within ~10% of C ✓ (0.99; per-category
losses to 1.79x disclosed, not hidden) · real-consumer integration ◐ partial
(loopback proof only — the one open item).
