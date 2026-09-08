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