# Methodology

Reproducible commands for building, testing, benchmarking, and verifying the
pinned reference. Milestone 0 records the environment and the exact baseline
procedure; every claim in `results/` must be traceable to a command below.

## Environment (re-recorded by every `run_baseline.sh`; snapshot below)

| Tool | Version | Path |
|---|---|---|
| OS | Windows (Git-Bash / MSYS2 userland) | — |
| CPU | 12th Gen Intel i5-12400F, 12 logical processors | — |
| Power scheme | Atlas Power Scheme (custom; record on every run) | — |
| git | 2.54.0.windows.1 | `/mingw64/bin/git` |
| curl | (msys2 dist) | `/mingw64/bin/curl` |
| rustc | 1.96.1 (31fca3adb 2026-06-26) | `~/.cargo/bin/rustc` |
| cargo | 1.96.1 (356927216 2026-06-26) | `~/.cargo/bin/cargo` |
| cc / gcc / clang | GCC 16.2.0 (w64devkit, since 2026-09-09) | `D:/Tools/w64devkit/bin` (user PATH) |
| make / mingw32-make | GNU Make 4.4.1 (w64devkit) | `D:/Tools/w64devkit/bin` |
| prove (perl) | (perl present, `prove` absent — suite runs directly) | `/usr/bin/perl` |

Re-run the environment snapshot with:

```bash
git --version; rustc --version; cargo --version; curl --version | head -n1
command -v cc gcc clang make prove perl
```

## Pin verification

```bash
sha256sum -c reference/SHA256SUMS          # must print OK for all 8 files
# cross-check hashes against reference/PINNED.md
```

## Rust crate (Milestone 0 stub)

```bash
cargo build      # release for real work: cargo build --release
cargo test       # empty until Milestone 1
```

Edition: 2024 (rustc 1.96.1 supports it; task defaulted to 2021 otherwise).

## Upstream C test suite (requires cc + picotest sources)

Resolved 2026-09-09: toolchain = w64devkit GCC (`D:/Tools`, on user PATH),
`reference/picotest/` fetched at the upstream submodule pin (hashes in
`SHA256SUMS`). The suite builds **out-of-tree** (never `make test` inside
`reference/`, which would write `test-bin` into the immutable oracle):

```bash
CC=gcc scripts/run_baseline.sh   # builds target/c-baseline/test-bin, runs it, writes results
```

Platform notes (all handled inside the script, harness-only):
- MinGW has no `sys/mman.h` / `sysconf`: the script generates a
  VirtualAlloc-backed shim under `target/c-baseline/shim/`.
- `prove` absent: the TAP binary runs directly (equivalent — `prove -v`
  only executes it).
- w64devkit GCC ships no ASan/UBSan runtimes: suite builds plain `-O2`
  (sanitizer coverage pending a toolchain that ships them).

## Upstream benchmark (requires a C compiler)

```bash
mkdir -p target/c-baseline
cc -O2 -o target/c-baseline/bench reference/bench.c reference/picohttpparser.c
time target/c-baseline/bench            # 10,000,000 iterations, exits 0 on success
```

`bench.c` is upstream's standalone micro-benchmark (typical GET request,
10M iterations, asserts full-buffer consumption).

## Full baseline (what `scripts/run_baseline.sh` does)

1. Snapshot environment + tool versions + machine identity (CPU, core count,
   power scheme).
2. Verify `reference/SHA256SUMS` (`sha256sum -c`).
3. `cc -O2` build of the upstream benchmark; time N=7 trials, discard the
   first as warmup, store `runs[]` + mean/min (timer-validated every trial).
4. Build `test-bin` out-of-tree from `picohttpparser.c` + `picotest/` +
   `test.c` (+ Windowsh `mmap` shim when needed); run via `prove` if present,
   else directly; record TAP totals.
5. Write `results/baseline.json` with real values only. Missing tools →
   `"status": "skipped"` + explicit `reason`. Exit 0 on honest skip; exit 1
   on hard failure.

## Rust profile policy (pinned in `Cargo.toml`, not inherited)

- `[profile.release]`: `opt-level = 2` (matches the C `-O2` anchor, plan §13),
  `codegen-units = 1`, `lto = true`, explicit `overflow-checks = false` /
  `debug-assertions = false` for C parity.
- `[profile.bench]` mirrors release but `lto = false`: bench-profile LTO
  could constant-fold stub calls with literal args into a ~0 ns/parse lie.
- `[profile.dev]` keeps asserts/overflow-checks ON — loud bugs in unit tests.
- Evidence→profile mapping: unit tests = dev; **differential, fuzz, and any
  measurement = release-shaped profile only**. A dev-profile crash or timing
  is never evidence (overflows panic in dev but wrap in release by design).
- Panic policy: keep unwind; edition-2024 `extern "C"` is nounwind (panic
  aborts, never UB through the ABI); M2+ bodies get `catch_unwind` → `-1`,
  and the core stays panic-free (`unwrap_used`/`expect_used` denied).

## Milestone 2/3/4/5 differential (Layer 2, request + response + headers + chunked)

```bash
CC=gcc bash scripts/diff_request.sh    # builds oracle + harness, runs corpus, tees results/difftest-request.log
CC=gcc bash scripts/diff_response.sh   # same for responses, tees results/difftest-response.log
CC=gcc bash scripts/diff_headers.sh    # same for header blocks, tees results/difftest-headers.log
CC=gcc bash scripts/diff_chunked.sh    # stateful decoder: single + split + dirty-state cases,
                                       # tees results/difftest-chunked.log
```

What it does: compiles `reference/picohttpparser.c` with `-D` renames
(`c_*` oracle symbols, `-O2 -Wall`), links the **release cdylib** (the
shipped artifact, never a debug build) plus the per-entry harness into one
binary, and runs every corpus file × caps {0,1,2,3,5,16,64} × three sweeps:
full buffer cold, every prefix streaming (`last_len` = previous length),
and every strict prefix cold (`last_len` = 0 — without this, the slowloris
gate short-circuits all short prefixes and parse-path EOF handling goes
uncompared).

Chunked (`diff_chunked.sh`) differs structurally: the decoder is stateful, so
each file runs single-call, exhaustive two-way splits (quarters for >8 KB
files), three-way quarter splits, and dirty initial decoder states, under
both `consume_trailer` settings — comparing ret, decoded length,
`is_in_data`, the full decoder struct, and the entire working buffer after
every call.

Case formula (request/response/headers harnesses): cases/file = 7 × ((len + 2) + len);
total = 7 × (2×Σlen + 2×files). The chunked harness counts single, split,
and dirty-state calls instead (see its stored log).

## Compatibility gate: upstream suite vs Rust (plan M5 / repo M6)

`scripts/run_baseline.sh` section 4b compiles the **unmodified** upstream
`reference/test.c` + `reference/picotest/` (plus the harness-only `mmap`
shim where the libc lacks it) and links them against
`target/release/picohttpparser_rs.dll` — *not* the C oracle. The suite's
guard-page `mmap` input makes this an overread test as well as a behavioral
one. Results land in `results/upstream-rust.log` with totals in
`baseline.json:upstreamVsRust` (schema v5); any failure fails the script.
The gate passes iff: this suite is green, every differential log is fresh
and mismatch-free, and `docs/divergences.md` holds no OPEN/UNRESOLVED row.
Compared per case — request: ret, method, path, version, header count;
response: ret, version, status (incl. partial value on digit failure),
reason (incl. empty + space-strip); both: every name/value — as
pointer+length equality into the shared input buffer (which *is* byte
equality), including the in-progress header slot on error paths (C leaves
the scanned name behind; the Rust seam mirrors it two-phase). The harness
never dereferences output pointers, so untouched sentinel slots cannot crash
it — only genuinely divergent bytes fail.

## Benchmark rules (forward-looking, per plan §13)

Same machine, CPU governor, compiler version, `-O2`, corpus, iteration count
for both sides; warmup runs; report mean/median/stddev; never compare debug
builds. Differential harness must feed identical bytes to both parsers.

Why `-O2`, not `-O3 -march=native`: the anchor is a *portable* baseline any
machine can reproduce, not a peak claim. A labeled `-O3 -march=native` run
may be added later for "fastest possible" comparisons — never mixed into
the anchor.

Bench-seam rule: measure the **shipped artifact**. Rust numbers must come
from the `cdylib`/`staticlib` via the same C-harness pattern as
`scripts/smoke_abi.c` (identical loop, out-of-line calls both sides) — never
from an rlib-linked Rust harness, which is a different codegen context.
`scripts/bench_compare.sh` implements this: one binary times the C oracle
loop and the cdylib loop interleaved (C,R/R,C), in-process, same REQ bytes
included verbatim from `reference/bench.c`, results in
`results/bench-compare.json` with artifact hash + machine identity.

## M9 benchmark suite — multi-category procedure (plan M8, §11–§13)

The single-corpus anchor above (fixed upstream REQ × 10M,
`results/bench-compare.json`, schema v1) is a labeled upstream-marker row.
Plan §11 requires seven categories before any sentence like "Rust is within
X% of C" is written without per-corpus qualification; this section is the
procedure for the multi-category extension (additive schema v2 over v1).

### Fixed contract (harness + driver)

The multi-category driver extends `scripts/bench_compare.c`/`.sh` so one
binary measures a category at a time:

```bash
./bench_compare [category]    # category omitted => anchor REQ corpus
```

Category set (plan §11 order): `tiny`, `typical`, `large-headers`,
`response`, `chunked`, `malformed`, `streaming`. Output tokens per run:
`CATEGORY <name>`, `ITERS <n>`, `TIMER <label> <res_ns>`, and per trial
`TRIAL <i> C <ns> R <ns>`. Results land as an **additive schema-v2** JSON:
every v1 field stays byte-unchanged and present; per-category rows are
added beside it, never replacing it.

Verified against the shipped driver and a live run
(`scripts/bench_compare.sh` `CATEGORIES`, `scripts/bench_corpus.h`, and a
fresh `./bench_compare chunked` execution): the emission grammar is exactly
`CATEGORY <name>`, `ITERS <n>`, `TIMER clock_gettime(CLOCK_MONOTONIC)
res_ns=<res>`, then seven `TRIAL <i> C <ns> R <ns>` lines (i = 0..6; trial 0
is warmup and discarded, 6 counted). Category keys are `tiny`, `typical`,
`large`, `response`, `chunked`, `malformed`, `streaming` — note `large`, not
`large-headers`. Per-category iteration defaults live in `bench_corpus.h`
as `CAT_*_ITERS` (tiny 120M, typical 10M, large 1M, response 30M, chunked
60M, malformed 400M, streaming 3M logical messages × 8 prefix parses) and
are emitted in each run's `ITERS` line. v2 JSON field names (from
`results/bench-compare.json`): top level `schemaVersion: 2`, `generatedUtc`,
`rev`, `status`, `machine{cpu,cpuCount}`, `timer{name,resNs}`, `protocol`,
`c{buildCommand,ccVersion,trialsNs,meanNs,minNs}`,
`rust{artifact,artifactSha256,rustc,profile,tier,trialsNs,meanNs,minNs}`,
`ratioRustOverC_mean`, `stability`, `corpus{...}` (anchor only), and
`categories{<key>:{iters,cMeanNs,cMinNs,rMeanNs,rMinNs,ratio,stability,
cTrialsNs,rTrialsNs,tier}}`. Every v1 anchor field is preserved unchanged;
the schema is additive as contracted.

### Harness pattern and trial protocol (unchanged from the anchor)

Both implementations live in one binary and are measured in one process,
interleaved (C,R / R,C alternating trials) so turbo/thermal/background
drift is common-mode; each side calls through the same out-of-line shape
(C oracle = renamed symbols; Rust = release cdylib import — the shipped
artifact, per the bench-seam rule above). Same buffers, same `ITERS`, same
process, same trial window. Seven trials per category with trial 0
discarded as warmup (6 counted). Timer is in-process
`clock_gettime(CLOCK_MONOTONIC)` (measured resolution 100 ns on this
machine); raw trial lines are retained (`target/bench-compare/trials.txt`)
beside the mean/min/spread aggregates and `trialsNs` arrays.

### Per-category iteration scaling (rationale)

One fixed `ITERS` across all seven categories would be wrong twice: 10M on
the 1 MB chunked corpus would take hours, and a tiny request would spend
extra wall time re-saturating branch predictors that already overfit the
single-corpus anchor. Each category therefore sets `ITERS` so a single
per-side trial occupies a comparable wall-clock band (~0.1–1 s), keeping
enough samples for a stable mean while bounding total campaign time. The
chosen `ITERS` is recorded per row in the v2 JSON; per-row `ns/parse` is
always recomputable as `meanNs / ITERS`. ns/parse and ratios are comparable
only within a category — cross-category aggregation into one "Rust vs C"
figure is explicitly not produced.

Verified: tiny 120,000,000; typical 10,000,000; large 1,000,000; response
30,000,000; chunked 60,000,000; malformed 400,000,000; streaming 3,000,000
(logical messages — 8 prefix parses each). These match `bench_corpus.h`
`CAT_*_ITERS` one-for-one and the recorded `categories.<key>.iters` values
in `results/bench-compare.json`. The fixed-contract guarantee holds as
stated: both sides run the same `ITERS` in the same trial, and the value is
emitted per run and recorded per row.

### Tier discipline

The anchor comparison is **C `-O2` vs the pinned release profile**
(opt-level=2, codegen-units=1, lto, overflow/debug off, unwind) — the only
basis for any headline claim. `-O3` and `-march=native` runs are labeled
side rows only (`C_OPT`/`RUSTFLAGS`/`TIER` env, written to `JSON_OUT`
files such as `results/bench-compare-o3.json` and
`results/bench-compare-native.json`), never mixed into the anchor. A
native-C number's fair counterpart is native Rust (`gcc -O3 -march=native`
vs `-C target-cpu=native`); a native-C vs scalar-Rust difference (the 1.45
native row) quantifies the SIMD prize — it is not an anchor result.

### Honest limits (read before trusting any number)

- Single-machine Windows runs (i5-12400F, Atlas power scheme, w64devkit
  GCC 16.2.0): numbers are machine-specific, not portable claims.
- No CPU pinning, governor control, or background isolation. Interleaving
  makes drift common-mode but cannot remove it; per-trial spread is
  recorded (`cspread`/`rspread`) and `stability` flags a >5% spread.
- In-process whole-loop timer only: latency *distributions* (p50/p95/p99)
  cannot be derived from it (see §12 mapping).
- `malformed` measures the **rejection path** (early-exit cost until the
  offending byte), not steady-state full-parse throughput; `streaming`
  measures **incremental delivery** (per-call seam + decoder state carry),
  not single-shot latency; `chunked` large is dominated by bulk
  `copy_within`, so its ns/parse is not comparable to a tiny request's.
- Every figure is labeled an internal engineering number, not a
  publication claim (schema `status` field).

### Plan §11 categories → corpus source → status

| §11 category | Corpus source (this tree) | Status |
|---|---|---|
| Tiny request | `tests/corpus/request/valid/01-basic.http` (+ hand-minimal REQ) | difftest-covered; driver row live: 36 B hand-minimal GET (identical bytes to `01-basic.http`), 120M iters |
| Typical request | `CAT_TYPICAL_MSG` (~330 B POST, 9 headers, `scripts/bench_corpus.h`) — NOT the anchor REQ below | driver row live, 10M iters |
| (Anchor, not a §11 row) | fixed upstream REQ (~620 B, 11 headers, `reference/bench.c` macro) | running since Phase 0 (schema v1); kept as the labeled upstream-marker row, never mixed into category claims |
| Large headers | `tests/corpus/request/valid/24-seventy-headers.http`, `18-long-path.http`; `tests/corpus/headers/valid/08-many-headers.http` | difftest-covered; driver row live: embedded 64-header × 64-B-value message built once by `cat_large_build` (~4.9 KB), 1M iters |
| Response parsing | `tests/corpus/response/valid/*` (15 files, incl. `09-long-reason`, `15-twenty-headers`) | difftest-covered; driver row live: ~150 B status + 4 headers, 30M iters |
| Chunked decoding | `tests/corpus/chunked/valid/*` (12 files, incl. `01-single`, `08-big-1MB`) | difftest-covered; driver row live: 38 B three-data-chunk stream (19 decoded, ret == 2), 60M iters |
| Malformed input | `tests/corpus/{request,response,headers,chunked}/malformed/*` (37+19+15+19 files) | difftest-covered — rejection-path semantics (see limits); driver row live: 18 B early-reject `G@T / HTTP/1.1...` (ret == -1), 400M iters |
| Streaming | same buffers delivered across split points (difftest streaming shapes: per-prefix, `last_len` chain) | difftest-covered — incremental-delivery semantics; driver row live: 8 staged prefixes of the typical message (3M logical = 24M parse calls) |

Corpus dirs and file names verified by `ls tests/corpus/...` and the totals
in `results/difftest-*.log` (`files=60/34/31/31`). The driver's concrete
per-category corpus pick, `ITERS`, and row emission are the driver lane's
deliverable.

### Plan §12 metrics → where recorded

| §12 metric | Where recorded | Note |
|---|---|---|
| ns/parse | bench JSON: `meanNs`/`minNs` per side per row; per-row `ns/parse = meanNs / ITERS` | headline metric; per-category only |
| Throughput (req/s) | derived = `1e9 / ns_per_parse`; not separately measured | derived field, not a new measurement [driver/reporting lane] |
| Bytes/s | derived = req/s × that row's corpus length | same |
| p50/p95/p99 | **deferred** — whole-loop aggregate timing cannot yield a per-iteration distribution; needs a per-iteration sampling harness that does not exist yet (consistent with `results/README.md`) | do not retrofit |
| CPU usage | **deferred** — not meaningful for an in-cache single-thread loop on a shared desktop; needs isolated infra | |
| Peak memory | **deferred** — parser is allocation-free by construction (verified property); static footprint only | |
| Allocations per parse | 0 — established property, not a bench number | |
| Binary/library size | **deferred** — absent from schema v1 and NOT added by schema v2 (verified: no size keys in the v2 JSON); capture step remains a future reporting-lane item | |

### Plan §13 rules → honored or known gap

| §13 rule | Status | How |
|---|---|---|
| Same machine | Honored | single machine every run; identity in every JSON (`machine`) and `baseline.json` |
| CPU governor/power settings | Partial — recorded, not controlled | Atlas scheme recorded; no governor control on this Windows consumer plan (known gap) |
| Compiler versions | Honored | `ccVersion` + `rustc` per file |
| Optimization level | Honored | tier discipline above; anchor = C `-O2` vs pinned release profile |
| Same input corpus | Honored | same buffers to both implementations in-process; anchor zero-drift by construction (REQ include) |
| Same iteration count | Honored per trial | same `ITERS` both sides; category scaling recorded per row |
| Warmups | Honored | trial 0 discarded; 6 counted |
| Multiple runs | Honored | 7 trials per category |
| Report mean/median/stddev | Partial — mean/min today | verified: v2 records meanNs/minNs plus raw trialsNs per row but does NOT compute median/stddev; both remain derivable from trialsNs (and the retained target/bench-compare/*.txt) |
| Prefer raw outputs | Honored | `trials.txt` retained; `trialsNs` arrays in JSON |
| Never use debug builds | Honored | release cdylib only (`cargo build --release`); artifact sha256 recorded |

## Optimization decisions (audited, measured where stated)

- `catch_unwind` stays on all four parsing exports: the seam cost is inside
  the measured number (no separate estimate needed), and dropping it would
  trade panic→−1 for panic→abort — a behavior-policy change requiring its
  own deliberation, not a codegen tweak. Revisit only with a measured
  seam-delta experiment.
- Value-scan batching (8-at-a-time, C-mirror): A/B-tested, measured ~6%
  SLOWER, reverted — LLVM already unrolls the plain loop; manual batching
  added only scaffolding. Do not retry without a new hypothesis.
- Token table (`TOKEN_CHAR` static LUT): A/B-tested, measured ~−11%
  whole-parse, kept — proven by 132,964 differential cases + full suite.
- SWAR 8-byte prescan in `get_token_to_eol` (safe, filter-only, soundness
  proven by exhaustive `swar_filter_sound` unit test over all 65,536 byte
  pairs): A/B-tested, ratio 1.52 → **0.98** (Rust 200.1ns vs C 203.6ns on
  the anchor; 0.97 on the prior run — sub-1.0 twice, spreads ~3%), kept —
  proven by the same gates. First sub-1.0 headline; still an internal
  number, not a publication claim (single corpus).
- `rtrim` rewrite and `#[inline]` attributes: REJECTED after disassembly
  evidence — `parse_token`/`get_token_to_eol`/`rtrim` are already fully
  inlined (no symbols emitted) and `parse_headers` contains zero panic
  call sites, so both changes would be proven no-ops. Verified, not assumed.
- Labeled tiers (same-session runs, `results/bench-compare-*.json`): anchor
  **0.97**, O3-pair **1.02** (both sides faster, gap steady), native-pair
  **1.45** (C `-march=native` unlocks its pcmpestri path: 2.10s → 1.38s;
  Rust scalar+SWAR barely moves). The native row quantifies the SIMD prize
  and the anchor rule stands: never mix native-C into headline claims.
- Staticlib benchmarking stays infeasible on this toolchain (MinGW ld
  rejects the MSVC EH residue — re-probed, still fails), so every Rust
  number includes the cdylib IAT hop. Disclosed, not hidden; Rust callers
  would use the rlib (cross-crate inlining) and see a different number.
- Chunked locals/`unreachable!` rework, `Progress` direct publication,
  PGO, AVX2 tiers: deferred — the first two are unmeasurable with current
  tooling (no chunked bench shape; SROA question unresolved either way) and
  the rest need numbers that do not exist yet.
- SIMD skip-scanning (`std::arch`), `memchr` dependency: deferred. SIMD
  needs differential fuzz before shipping `unsafe` into the hot path;
  `memchr` solves single-byte search, not accept-set classification;
  struct layouts are ABI-frozen and not optimizable.
- Pinned release profile: unchanged by any finding; tiers stay labeled.

## Honesty policy

`results/baseline.json` is generated, not hand-edited. A skipped measurement
is a valid result with a reason; a fabricated number is not a result.