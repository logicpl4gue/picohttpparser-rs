# results/

Machine-readable evidence for the experiment. Raw numbers live here, generated
by scripts — **not hand-edited, never fabricated**.

| File | Content |
|---|---|
| `baseline.json` | Milestone 0 C baseline: environment, pin verification, upstream test-suite and benchmark outcomes. Missing tools are `"status": "skipped"` with an explicit `reason`. |
| `latest.json` (future) | Latest full result set (compat + fuzz + bench). |
| `baseline.json` archive policy | Regenerate with `scripts/run_baseline.sh`; keep old files under `results/archive/` when the pinned revision or machine changes. |
| `upstream-tests.log` / `upstream-rust.log` | Upstream `test.c` TAP output vs C oracle / vs Rust cdylib (8/8 subtests each). |
| `difftest-request.log`, `-response.log`, `-headers.log`, `-chunked.log` | Layer-2 differential totals + run identity (`run_utc`, `rev`). |
| `bench-compare.json` (+ `-o3`, `-native` tiers) | Same-session C-vs-Rust numbers with artifact hashes (schema v1, own versioning). |
| `integration.log` | M8 loopback transcripts (oracle + Rust, must be byte-identical) + ctypes transcript + provenance (rev, dirtiness, artifact hashes). |
| `EVIDENCE.md` | M10 final evidence table (plan §17): compat + fuzz + bench + impl + integration totals with primary-record citations. Hand-maintained aggregation — every figure must match the machine record it cites. |

Rules:

1. A skipped measurement with a reason is a valid result. A guessed number is
   not — `"status": "skipped"` is the only honest placeholder.
2. Every recorded number must be reproducible from a command listed in
   `docs/methodology.md`.
3. Report losses and wins; the goal is a controlled experiment, not a
   claim-file.

## `baseline.json` schema (v6; v6 adds top-level `rev` so the M0 record maps to a commit)

Top-level: `schemaVersion` (number), `generatedUtc` (string),
`generatedBy` (string), `pin` {`repository`, `commit`, `short`,
`filesPinned` (number), `hashVerify` (`passed`/`failed`), `hashDetail`},
`environment` {tool versions, `cpu`, `cpuCount`, `powerScheme`, all strings},
`cBaseline` {`status`, `reason`, `buildCommand`, `benchTrials` (number),
`benchWarmupDiscarded` (number), `benchIterations` (number|null),
`benchSecondsMean`/`Min`/`Median`/`Stddev` (number|null), `benchNsPerParseMean` (number|null),
`benchRunsSeconds` (array), `benchWarmupSeconds` (number|null),
`benchCorpusFile`, `benchCorpusSha256`, `benchScope`, `timer`,
`timerGranularityNs` (number|null), `timerNote`},
`upstreamTestSuite` and `upstreamVsRust` {`status`, `reason`, `runner`,
`sanitizers` (suite only), `total`/`passed`/`failed` (number|null), `skipped`,
`log`}, `honestyNote`.

History: v1 = M0 baseline; v2 = test-suite counts; v3 = bench trials +
machine identity + corpus metadata; v4 = `upstreamVsRust` (gate: unmodified
upstream `test.c` linked against the Rust cdylib); v5 = median/stddev +
parsed (not hardcoded) iteration count; v6 = top-level `rev` (commit the
M0 record was generated from). Percentiles (plan §12 p50/p95/p99)
are deliberately absent: they apply to per-iteration latency harnesses, not
whole-loop aggregate timing.

## `bench-compare.json` schema (v3, own versioning; v2 added categories, v3 adds medians/paired-ratios/os+powerScheme/harness-hashes/assertState)

Same-session C-vs-Rust record from `scripts/bench_compare.sh`: `rev`,
`status` (always labeled internal-engineering-number), `machine` (`cpu`,
`cpuCount`, `os`, `powerScheme`), `timer` (in-process `clock_gettime`,
measured resolution), `protocol`, `c` {`buildCommand`, `ccVersion`,
`assertState` (NDEBUG-set/unset — the C per-iteration check costs a branch
unless stripped), trials, mean/min/median}, `rust` {artifact path + sha256,
rustc, pinned profile flags, trials, mean/min/median}, `ratioRustOverC_mean`
plus `pairedRatioMean`/`pairedRatioStddev` (per-trial rᵢ/cᵢ keeps the
interleaving pairing the pooled ratio throws away), `stability` (`OK` or
`CHECK` with reason), `corpus` {file = bench.c REQ macro included verbatim,
sha, scope, iterations}, `harness` {source + corpus-header sha256}, plus
`tier` (`anchor` by default). Per-category rows carry the same median +
paired-ratio fields. v1 lacked medians, paired ratios, machine
`os`/`powerScheme`, harness hashes, and `assertState`. Labeled P8 tiers live
beside it (`bench-compare-o3.json`, `bench-compare-native.json`, via
`C_OPT`/`RUSTFLAGS`/`TIER`/`JSON_OUT` env on the same script) — the anchor
file is never overwritten by a tier run. Quoted headline ratios use 2
decimals; the JSON keeps full precision because raw trials are retained.