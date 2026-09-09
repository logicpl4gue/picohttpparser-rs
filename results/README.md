# results/

Machine-readable evidence for the experiment. Raw numbers live here, generated
by scripts — **not hand-edited, never fabricated**.

| File | Content |
|---|---|
| `baseline.json` | Milestone 0 C baseline: environment, pin verification, upstream test-suite and benchmark outcomes. Missing tools are `"status": "skipped"` with an explicit `reason`. |
| `latest.json` (future) | Latest full result set (compat + fuzz + bench). |
| `baseline.json` archive policy | Regenerate with `scripts/run_baseline.sh`; keep old files under `results/archive/` when the pinned revision or machine changes. |

Rules:

1. A skipped measurement with a reason is a valid result. A guessed number is
   not — `"status": "skipped"` is the only honest placeholder.
2. Every recorded number must be reproducible from a command listed in
   `docs/methodology.md`.
3. Report losses and wins; the goal is a controlled experiment, not a
   claim-file.

## `baseline.json` schema (v5)

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
parsed (not hardcoded) iteration count. Percentiles (plan §12 p50/p95/p99)
are deliberately absent: they apply to per-iteration latency harnesses, not
whole-loop aggregate timing.

## `bench-compare.json` schema (v1, own versioning)

Same-session C-vs-Rust record from `scripts/bench_compare.sh`: `rev`,
`status` (always labeled internal-engineering-number), `machine`, `timer`
(in-process `clock_gettime`, measured resolution), `protocol`, `c`
{`buildCommand`, `ccVersion`, trials, mean/min}, `rust` {artifact path +
sha256, rustc, pinned profile flags, trials, mean/min}, `ratioRustOverC_mean`,
`stability` (`OK` or `CHECK` with reason), `corpus` {file = bench.c REQ
macro included verbatim, sha, scope, iterations}, plus `tier` (`anchor` by
default). Labeled P8 tiers live beside it (`bench-compare-o3.json`,
`bench-compare-native.json`, via `C_OPT`/`RUSTFLAGS`/`TIER`/`JSON_OUT` env
on the same script) — the anchor file is never overwritten by a tier run.