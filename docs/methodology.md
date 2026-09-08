# Methodology

Reproducible commands for building, testing, benchmarking, and verifying the
pinned reference. Milestone 0 records the environment and the exact baseline
procedure; every claim in `results/` must be traceable to a command below.

## Environment (recorded at baseline time, 2026-09-08)

| Tool | Version | Path |
|---|---|---|
| OS | Windows (Git-Bash / MSYS2 userland) | — |
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
sha256sum -c reference/SHA256SUMS          # must print OK for all 6 files
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

1. Snapshot environment + tool versions.
2. Verify `reference/SHA256SUMS` (`sha256sum -c`).
3. `cc -O2` build of the upstream benchmark; time one run.
4. If `make` + `prove` + `picotest` exist: run `make test`.
5. Write `results/baseline.json` with real values only. Missing tools →
   `"status": "skipped"` + explicit `reason`. Exit 0 on honest skip; exit 1
   on hard failure.

## Benchmark rules (forward-looking, per plan §13)

Same machine, CPU governor, compiler version, `-O2`, corpus, iteration count
for both sides; warmup runs; report mean/median/stddev; never compare debug
builds. Differential harness must feed identical bytes to both parsers.

## Honesty policy

`results/baseline.json` is generated, not hand-edited. A skipped measurement
is a valid result with a reason; a fabricated number is not a result.