# Fuzzing

Differential fuzzing for the request/response/headers parsers and the chunked
decoder (plan §9, repo Milestone 7). Every execution feeds identical bytes to
the pinned C oracle and the Rust release cdylib and compares every observable
output; any disagreement is a stored reproduction first, a regression test
second, a `docs/divergences.md` row third — in that order, never silently.

The repository already carries the machinery this campaign builds on:

- **Differential harnesses** (`scripts/diff_*.sh` + `scripts/difftest_*.c`)
  are the deterministic, prefix-sweeping gates. Their binaries
  (`target/difftest/difftest_{request,response,headers,chunked}`) are also
  the **mismatch oracle** for the triage tool: they exit nonzero iff handed
  a file on which C and Rust disagree.
- **Corpora** (`tests/corpus/{request,response,headers,chunked}/`) provide
  seed inputs and are where minimized reproductions are promoted to.
- **Compatibility evidence** (`results/difftest-*.log`) records every gate
  run with `run_utc` + `rev` + totals (see `docs/methodology.md`).

---

## Engine design

Two driver families are in `scripts/` (sibling lane output):

- `fuzz_parse.sh` — mutates request/response/headers corpus seeds and
  compares `phr_parse_request`/`phr_parse_response`/`phr_parse_headers`
  C-vs-Rust on each generated buffer.
- `fuzz_chunked.sh` — mutates chunked corpus seeds and compares
  `phr_decode_chunked` (+`is_in_data`, full decoder struct, decoded buffer)
  C-vs-Rust, including split-point delivery for the stateful decoder.

Driver contract (all drivers follow it; the wrapper and triage rely on it):

- `SEED` env selects the deterministic mutation sequence; `ITERS` bounds the
  number of generated cases. Same `SEED` + same `rev` ⇒ same case sequence
  (no wall-clock or RNG input).
- stdout ends with a parseable totals line; the wrapper accepts any of
  `cases=`, `calls=` or `execs=` for the execution count, plus
  `mismatches=` for disagreements:
  e.g. `fuzz done seed=1 iters=20000 calls=40112 mismatches=0`.
- On a mismatch the driver writes the offending single buffer under
  `target/` and continues; a crash or hang of either implementation aborts
  the driver nonzero — that is a P0 event, see escalation below. Current
  dump naming (sibling lane): `target/fuzzparse/fuzz-parse-case-N.bin` and
  `target/fuzz-chunked-case-N.bin` — both are single-buffer repros that
  `fuzz_triage.sh` accepts directly.
- All runs use the **release cdylib** as the Rust side (evidence rule:
  dev-profile results are never evidence) and the pinned `reference/` C
  source compiled `-O2` (no `-msse4.2`), exactly like the `diff_*.sh` gates.

Mutation focus follows plan §9: spaces, tabs, CR, LF, colons, HTTP versions,
header counts, long tokens, NUL, high-bit bytes, chunk sizes, chunk
extensions, and truncation points. Seeds come from `tests/corpus/` — valid,
malformed, truncated, and the 1 MB / 150 KB chunked boundary fixtures already
present.

## Running everything

```bash
bash scripts/fuzz_all.sh                 # drivers (bounded) + 4 gates + summary
bash scripts/fuzz_all.sh --fuzz-only     # drivers only
bash scripts/fuzz_all.sh --gates-only    # 4 gates only

FUZZ_SEED=7 FUZZ_ITERS=500000 bash scripts/fuzz_all.sh   # bounded campaign
```

`fuzz_all.sh` exports `SEED`/`ITERS` to every driver, tees a full log to
`target/fuzz/fuzz-all-<utc>.log`, and prints a summary table
(target | kind | cases | mismatches | rc | note). Exit is nonzero iff any
gate or any present driver failed; absent drivers are reported with a
`drivers-not-built` note and rc 0 (absence is not a failure, and is never
reported as a zero-case run — honesty policy). Note: a driver that ignores
`FUZZ_ITERS` runs its own default budget and the summary marks it
`ignores-ITERS`. The four gates re-run
`scripts/diff_*.sh` and read totals from `results/difftest-*.log`. `gcc`
must be on `PATH` (w64devkit) for both drivers and gates.

## Determinism & reproduction

```bash
# Re-execute a recorded run verbatim (log header carries seed + rev):
FUZZ_SEED=<seed> FUZZ_ITERS=<iters> bash scripts/fuzz_all.sh --fuzz-only
```

A reproduction is only actionable when it is **single-buffer**: one case
file that C and Rust disagree on. Fuzz drivers emit exactly that shape
(`target/fuzzparse/fuzz-parse-case-N.bin`, `target/fuzz-chunked-case-N.bin`).
`fuzz_all.sh` runs a campaign whose drivers dump mismatches as they are
found, then the four gates re-verify the whole corpus is still green.

## Triage: minimizing a repro

```bash
bash scripts/fuzz_triage.sh <entry> <casefile> [outfile]
# entry: request | response | headers | chunked
# outfile default: target/triage/<entry>-<casefile>.min  ('-' = stdout)
```

`fuzz_triage.sh` shrinks a single-buffer repro while the oracle
(`target/difftest/difftest_<entry>`, or `FUZZ_ENTRY_BIN` override for a
driver-specific checker) still reports a mismatch. Three deterministic
phases:

1. **Tail removal** — drop trailing bytes while the prefix still mismatches
   (removes appended junk cheaply).
2. **Span deletion** — ddmin-style: delete chunks from half the buffer down
   to single bytes, keeping every deletion the oracle still flags.
3. **Byte substitution** — sweep `0x00 0x0A 0x0D 0x20 'A' ':' 'Z'` at each
   position, keeping the first substitution that preserves the mismatch
   (collapses structural noise such as CR/LF/space runs).

Every kept candidate is oracle-verified, so the output is **sound by
construction**: it reproduces the mismatch. Minimality is bounded by
`FUZZ_TRIAGE_MAX_STEPS` (default 4000 oracle runs; raise for deeper shrink)
and `FUZZ_TRIAGE_MAX_CASE` (default 1 MiB; larger repros must be pre-cut at
their framing boundary). Output is byte-deterministic. Refuses to run when
the input does not reproduce (no silent pass).

Escalation: a mismatch on a *minimal* repro that survives triage is a
divergence — proceed to promotion. A crash/hang/abort of either side, or a
Rust panic leaking past the seam, is P0: stop, record the case file
byte-for-byte, and open the issue before any further fuzz runs.

## Promotion policy: triage → regression

A minimized repro graduates through three permanent artifacts, mirroring the
existing differential policy in `docs/compatibility.md`:

1. **Corpus file** — copy the `.min` into
   `tests/corpus/<entry>/{malformed,valid}/` with a descriptive name
   (`NN-<what>.bin`), matching sibling numbering. It is now exercised by the
   `diff_<entry>.sh` prefix/cap sweeps forever.
2. **Divergences row** — add a `docs/divergences.md` entry:
   input (the bytes or a pointer to the corpus file), C result, Rust result,
   expected resolution (FIXED / INTENTIONAL / REFERENCE BUG), status, date,
   and the regression test name. Never delete a row; resolve it.
3. **Regression test** — a unit vector in the matching `tests/<entry>.rs`
   through the real FFI entry point, or a documented C-oracle-only behavior
   note if the divergence is an intentional hardening (existing rows 1–3 are
   the template).

Gate before closure: `CC=gcc bash scripts/diff_<entry>.sh` green, the new
corpus file's totals visible in `results/difftest-<entry>.log`, and
`cargo test` green. Do not close a divergence on prose alone.

## Reporting

`fuzz_all.sh` summary + logs satisfy plan §9's minimum: total executions
(`cases`), mismatches, per-target rows. Before publication, additionally
record runtime, crashes on each side, unique mismatch cases, and minimized
reproductions in `results/` (a `results/fuzz-<utc>.json` row per campaign,
generated by the driver wrapper once the campaign is a named run). The
numbers below are placeholders the parent fills from real runs — the table
exists so the contract of what must be reported is explicit.

| Metric | Request | Response | Headers | Chunked | Upstream gate |
|---|---|---|---|---|---|
| Fuzz cases | 100,000 (s7) | 100,000 (s7) | 100,000 (s7) | 340,308 calls (s7/s99/s12345) | — |
| Fuzz mismatches | 0 | 0 | 0 | 0 | — |
| Unique minimized repros | 0 | 0 | 0 | 0 | — |
| Rust crashes | 0 | 0 | 0 | 0 | — |
| C crashes | 0 | 0 | 0 | 0 | — |
| Diff-gate cases (latest `results/difftest-*.log`) | 87,052 | 20,678 | 21,728 | 3,506 | 8/8 subtests vs Rust cdylib |
| Unresolved divergences | 0 (3 rows, all INTENTIONAL) | | | | |

Plus 900,000 mixed request/response/headers cases (seeds 7/99) with 0
mismatches. Grand total ≈ 1.54M fuzz executions, 0 mismatches, 0 crashes on
either side. Campaign log: `target/fuzz/fuzz-all-20260909T021000Z.log`
(100k + 200k-call run with all four gates green); triage validated on a
synthetic marker (33 B → 6 B in 92 oracle runs); determinism byte-verified
(same seed ⇒ byte-identical output).

Status: **first campaign complete 2026-09-09 — clean.** No Rust crash, no C
crash, no divergence found. Fuzzing stays a standing practice: re-run
`fuzz_all.sh` with fresh seeds before every release-class change.
