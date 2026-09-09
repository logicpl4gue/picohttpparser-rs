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

## `baseline.json` schema (v4)

Top-level: `schemaVersion` (number), `generatedUtc` (string),
`generatedBy` (string), `pin` {`repository`, `commit`, `short`,
`filesPinned` (number), `hashVerify` (`passed`/`failed`), `hashDetail`},
`environment` {tool versions, `cpu`, `cpuCount`, `powerScheme`, all strings},
`cBaseline` {`status`, `reason`, `buildCommand`, `benchTrials` (number),
`benchWarmupDiscarded` (number), `benchIterations` (number|null),
`benchSecondsMean`/`Min` (number|null), `benchNsPerParseMean` (number|null),
`benchRunsSeconds` (array), `benchWarmupSeconds` (number|null),
`benchCorpusFile`, `benchCorpusSha256`, `benchScope`, `timer`,
`timerGranularityNs` (number|null), `timerNote`},
`upstreamTestSuite` and `upstreamVsRust` {`status`, `reason`, `runner`,
`sanitizers` (suite only), `total`/`passed`/`failed` (number|null), `skipped`,
`log`}, `honestyNote`.

History: v1 = M0 baseline; v2 = test-suite counts; v3 = bench trials +
machine identity + corpus metadata; v4 = `upstreamVsRust` (gate: unmodified
upstream `test.c` linked against the Rust cdylib).