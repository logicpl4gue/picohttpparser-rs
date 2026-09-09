# picohttpparser-rs: a drop-in Rust HTTP parser, minus the segfaults

Rewrite the HTTP parser. Keep the C API. Keep the zero-copy behavior. Prove
nobody can tell — then publish the places where Rust is slower, and the places
where the C original is undefined behavior that we refused to copy.

That was the experiment, and it was never "Rust beats C." A rewrite is only
interesting if consumers cannot tell the difference, so that was the only
claim we allowed ourselves to make.

`◉ correctness: 133,352 differential cases, 0 mismatches` ·
`◉ upstream suite: 8/8 subtests · 299 assertions` ·
`◉ fuzz: ~1.94M executions, 0 crashes` ·
`◉ divergences: 0 unresolved` ·
`◉ license: MIT`

*(Text chips, not badge images — there is no CI or registry to back a badge,
so none is faked.)*

## Why picohttpparser

PicoHTTPParser is a single C file with a tiny API (`phr_parse_request`,
`phr_parse_response`, `phr_parse_headers`, `phr_decode_chunked`,
`phr_decode_chunked_is_in_data`): allocation-free, zero-copy, used in real
software, small enough to read in one sitting, and shipped to consumers
through a frozen struct ABI. That combination makes it an ideal subject for
a controlled engineering experiment: not "is Rust better than C," but "can a
memory-safe implementation replace a battle-tested C parser and be
indistinguishable at every observable boundary?" The reference is pinned
(`f4d94b48`, SHA-256 verified across 8 files) and treated as a behavioral
oracle: disagreements between docs and implementation are recorded, and what
real consumers rely on is treated as ground truth.

## The rules of the game

Order was fixed up front: correct, then compatible, then measurable, then
fast. Optimization before differential behavior is stable was forbidden.
Measurements run against the pinned release profile (opt-level 2 to match
the C `-O2` anchor, codegen-units 1, LTO) — the shipped cdylib, never a
debug build. Every figure below is an internal engineering number from one
machine (i5-12400F, w64devkit GCC 16.2.0, rustc 1.96.1), reported with its
limits, not as a portable claim.

## Proof, one table

Same-session C `-O2` vs Rust release cdylib, interleaved trials, same bytes,
same process. Ratio = Rust/C; **below 1.00 means Rust is faster.**

| Corpus | Rust/C | Reading |
|---|---|---|
| Upstream marker (~620 B request, 11 headers) | **0.827** | Rust ~17% faster · `stability: CHECK` |
| Large headers (~4.9 KB, 64 × 64 B values) | **0.680** | Rust ~32% faster |
| Typical request (~400 B, 9 headers) | 1.100 | within 10% |
| Streaming (8 staged prefix parses) | 1.023 | near parity |
| Response (~150 B) | 1.185 | loss, published |
| Chunked decode (38 B, 3 chunks) | 1.199 | loss, published |
| Tiny request (37 B) | 1.482 | fixed-cost floor |
| Malformed (reject at byte 1) | 1.811 | fixed-cost floor |

Reproducible via `scripts/bench_compare.sh`. Raw trials, spread flags, and
the honest limits (no CPU pinning; whole-loop timer only; per-category
ns/parse not cross-comparable) live in
[`results/EVIDENCE.md`](results/EVIDENCE.md) and
[`docs/methodology.md`](docs/methodology.md).

## Compatibility matrix

| Entry point | Differential cases vs C | Mismatches |
|---|---|---|
| `phr_parse_request` | 87,052 | 0 |
| `phr_parse_response` | 20,678 | 0 |
| `phr_parse_headers` | 21,924 | 0 |
| `phr_decode_chunked` (+ `is_in_data`) | 3,698 | 0 |
| Unmodified upstream `test.c` vs cdylib | 8/8 subtests · 299 assertions | 0 |
| **Total** | **133,352** | **0** |

Four documented divergences exist — all deliberate **fail-closed hardenings**
where C's defined behavior is a segfault or hang
(`docs/divergences.md`). The unsafe original reads past a buffer on
`last_len > len`; this crate returns `-2`.

<details>
<summary>How it's verified (and how you can re-run it)</summary>

- **Pin:** upstream `f4d94b48` frozen in `reference/` with SHA-256 manifests
  (`sha256sum -c reference/SHA256SUMS`).
- **Layer 1:** the unmodified upstream suite runs against the C oracle and
  against this cdylib — 8/8, 299 checks, 0 failures both sides. Because the
  suite feeds input from a guard-page mmap region, it doubles as an overread
  test (`CC=gcc bash scripts/run_baseline.sh`).
- **Layer 2:** identical bytes to both parsers — every corpus file × header
  caps × full buffer + every streaming prefix + every cold strict prefix —
  comparing return value, every pointer+length, decoder state, and even the
  in-progress header slot on error paths. The chunked decoder additionally
  compares the full 32-byte decoder struct and the whole working buffer after
  every call, including dirty initial states.
- **Fuzz:** deterministic differential campaign, ~1.94M executions
  (200,000 parse cases + 398,851 stateful chunked calls at seed 11, plus
  per-entry seed-7 sweeps), 0 mismatches, 0 crashes either side, rerun
  verified byte-identical (`bash scripts/fuzz_all.sh`).
- **Integration:** a self-authored HTTP/1.1 consumer builds against C *and*
  this cdylib with byte-identical transcripts over loopback TCP, plus a
  Python ctypes consumer over all five symbols
  (`CC=gcc bash scripts/integ_http11.sh`).
- **Implementation:** 1,527 LOC of Rust; 7 `unsafe` blocks confined to the
  single FFI seam file (`src/ffi.rs`); panic-free core by lint policy
  (`unwrap_used`/`expect_used` denied); 0 allocations per parse; 30 unit
  vectors green.

</details>

## Quickstart

```bash
cargo build --release        # cdylib + staticlib + rlib
cargo test --offline         # 30 unit/ABI vectors, all pass

# Differential harnesses (C oracle vs Rust):
CC=gcc bash scripts/diff_request.sh      # 87,052 cases
CC=gcc bash scripts/diff_headers.sh      # 21,924 cases
CC=gcc bash scripts/diff_chunked.sh      # 3,698 cases, struct-memcmp lockstep
```

### C consumers: relink, don't rewrite

```c
#include "picohttpparser.h"              /* unchanged, from reference/ */
const char *method; size_t method_len;
int minor_version;
size_t num_headers = 16;
struct phr_header headers[16];
int ret = phr_parse_request(buf, len, &method, &method_len, /* ... */);
```

Identical contract to upstream — a full working consumer ships in
`scripts/integ_http11.c` with byte-identical transcripts against both
implementations.

## Known honest limits

- Benchmarks: single machine, no CPU pinning, whole-loop timer (no
  p50/p95/p99). Several rows carry `CHECK-spread>5%` flags; quote none of
  them as portable claims.
- The `-O3` tier shows a real, unexplained Rust-side regression on the
  malformed reject path (same-session control: O3/O2 = 1.68× on Rust while C
  moved the other way). The anchor stays O2-vs-O2; tier rows are labeled side
  data. PGO is skipped with reason (`llvm-profdata` unavailable).
  Native-tier C (`-march=native`, pcmpestri) beats scalar Rust 1.45× — that
  gap is the documented SIMD prize, not an anchor result.
- A genuine H2O/Plack/Furl consumer integration is **not yet attempted**
  (no OpenSSL dev libs on the dev machine). Loopback proof is complete;
  real-consumer proof is the open item.

## Layout & links

```
src/          safe core (core/request/response/chunked) + ffi.rs seam (all unsafe)
reference/    pinned upstream oracle (immutable) + SHA256SUMS
docs/         api.md · compatibility.md · divergences.md · methodology.md · fuzzing.md
results/      machine-readable evidence (generated, never hand-edited)
scripts/      differential + fuzz + bench + integration harnesses
tests/        corpus (158 files) + unit vectors (request/response/headers/chunked/abi)
```

- Methodology & full procedure: [`docs/methodology.md`](docs/methodology.md)
- ABI mapping table: [`docs/api.md`](docs/api.md)
- Divergence register: [`docs/divergences.md`](docs/divergences.md)
- Evidence dashboard: [`results/EVIDENCE.md`](results/EVIDENCE.md)
- Project plan & milestone numbering:
  [`picohttpparser-rs-plan.md`](picohttpparser-rs-plan.md)

## License

MIT. Upstream PicoHTTPParser is dual-licensed (MIT / Perl) — see
`reference/` headers. Not affiliated with the upstream project.
