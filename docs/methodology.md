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

## Milestone 2 differential (Layer 2, request parser)

```bash
CC=gcc bash scripts/diff_request.sh   # builds oracle + harness, runs corpus, tees results/difftest-request.log
```

What it does: compiles `reference/picohttpparser.c` with `-D` renames
(`c_*` oracle symbols, `-O2 -Wall`), links the **release cdylib** (the
shipped artifact, never a debug build) plus `scripts/difftest_request.c`
into one binary, and runs 57 corpus files × caps {0,1,2,3,5,16,64} × full
buffer + every prefix (streaming, `last_len` = previous length).

Case formula: cases/file = 7 × (len + 2); total = 7 × (Σlen + 2×files).
Compared per case: ret, method, path, version, header count, every
name/value — as pointer+length equality into the shared input buffer (which
*is* byte equality), including the in-progress header slot on error paths
(C leaves the scanned name behind; the Rust seam mirrors it two-phase).
The harness never dereferences output pointers, so untouched sentinel slots
cannot crash it — only genuinely divergent bytes fail.

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

## Honesty policy

`results/baseline.json` is generated, not hand-edited. A skipped measurement
is a valid result with a reason; a fabricated number is not a result.