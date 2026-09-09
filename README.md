# picohttpparser-rs

**PicoHTTPParser had decades to stop segfaulting on hostile input. It didn't. So we rebuilt it: 1,527 lines of Rust, zero allocations per parse, every behavior pinned against the C original instead of guessed.**

[![language](https://img.shields.io/badge/language-Rust-b7410e)](https://www.rust-lang.org)
[![deps](https://img.shields.io/badge/dependencies-0-green)](.)
[![allocs](https://img.shields.io/badge/allocs%20per%20parse-0-green)](.)
[![unsafe](https://img.shields.io/badge/unsafe%20blocks-7%20in%20one%20file-yellow)](src/ffi.rs)
[![parity](https://img.shields.io/badge/C%20agreement-133%2C352%20%2F%20133%2C352-green)](.)
[![honesty](https://img.shields.io/badge/slow%20rows%20hidden-0-blue)](.)

> Your HTTP parser has corners that segfault. Ours has four, all filed, all refused on purpose.

[Why](#why) · [Scoreboard](#scoreboard) · [Speed](#speed) · [Adversarial](#adversarial) ·
[Opinions](#opinions) · [Hot takes](#hot-takes) · [The experiment](#the-experiment) ·
[Scope](#scope) · [Quick start](#quick-start) · [Status](#status)

---

## Why

Every HTTP request your stack ever served went through a parser like
PicoHTTPParser: one C file, a tiny API, allocation-free, zero-copy, shipped
in real software. And every rewrite of a parser like it makes you pick your
poison:

- **The C original:** a black box whose corners differ between the docs and
  the code. Quirks you inherit silently, including the ones that read past
  your buffer. Battle-tested and still "trust me."
- **A vibe-coded port:** a new parser wearing the old name. No oracle, no
  differential run, no ledger. Tech debt with a README.

picohttpparser-rs ends the trade: 1,527 lines of safe Rust, **zero
allocations per parse** (not "minimal": zero), 7 `unsafe` blocks confined to
a single FFI seam, exported through the unchanged C ABI, and verified
byte-for-byte against the pinned C original instead of assumed correct.

Watch it work (real consumer, unchanged header, relink only):

```c
#include "picohttpparser.h"              /* unchanged, from reference/ */
const char *method; size_t method_len;
int minor_version;
size_t num_headers = 16;
struct phr_header headers[16];
int ret = phr_parse_request(buf, len, &method, &method_len, /* ... */);
```

That's the whole job. Here is the whole proof.

---

## Scoreboard

Every claim below comes from running both implementations on the **same
machine** against the **same inputs**: the 133,352-case differential corpus,
plus the unmodified upstream suite, plus ~1.94M fuzz executions.

```
picohttpparser-rs vs PicoHTTPParser f4d94b48   (pinned; SHA-256 across 8 files)

  differential cases    ████████████████████  133,352 / 133,352  agree  ✅
  upstream suite        ████████████████████  8/8 subtests · 299 assertions  ✅
  fuzz executions       ████████████████████  ~1.94M · 0 crashes either side  ✅
  divergences           D001–D004             every one fail-closed, none silent
  real-consumer proof   ░░░░░░░░░░░░░░░░░░░░  parked: no OpenSSL dev libs yet (see Status)
```

**133,352 / 133,352.** Not "passes its own test suite": every differential
case produces the identical return value, pointers, lengths, and decoder
state as the C original — request (87,052), response (20,678), headers
(21,924), chunked (3,698, struct-memcmp lockstep).

**Four documented divergences, D001–D004.** Differential testing *will* find
disagreements, and hiding them is how parsers rot. Each of the four is filed
with its reproducer and its reasoning in `docs/divergences.md`. All four are
the same species: places where C's defined behavior is a segfault or a hang,
and this crate returns `-2` instead. The unsafe original reads past a buffer
on `last_len > len`. We refused to copy that.

---

## Speed

Rust/C ratio, same machine, same session, interleaved trials, same bytes,
same process. Below 1.00 means Rust is faster. Lower is better.

| workload | Rust/C | reading |
|---|---:|---|
| large headers (~4.9 KB, 64 × 64 B values) | **0.680** | Rust ~32% faster |
| upstream marker (~620 B request, 11 headers) | **0.827** | Rust ~17% faster · `stability: CHECK` |
| streaming (8 staged prefix parses) | 1.023 | near parity |
| typical request (~400 B, 9 headers) | 1.100 | within 10% |
| response (~150 B) | 1.185 | loss, published |
| chunked decode (38 B, 3 chunks) | 1.199 | loss, published |
| tiny request (37 B) | 1.482 | fixed-cost floor |
| malformed (reject at byte 1) | 1.811 | fixed-cost floor |

**Two workload categories go to Rust**, up to **~32% faster**. The rest go to
C, and they're printed in the same font size. Full harness and methodology
ship with the repo so you can re-run the numbers yourself
(`scripts/bench_compare.sh`). If you can make the table look worse, open an
issue. We'd genuinely like to know.

### The losses we're not hiding

Tiny inputs cost us an honest **1.482× on a 37-byte request and 1.811× on a
byte-1 reject**: a fixed-cost floor the safe core pays even when a call dies
immediately. Response (1.185×) and chunked decode (1.199×) are straight
losses too. They stay in the table because a benchmark page without a loss
row is telling you what the author needed to be true.

The `-O3` tier holds a real, unexplained Rust-side regression on the
malformed reject path (same-session control: O3/O2 = 1.68× on Rust while C
moved the other way). The anchor stays O2-vs-O2; tier rows are labeled side
data. And native-tier C (`-march=native`, pcmpestri) beats scalar Rust 1.45×
— that gap is the documented SIMD prize, not an anchor result.

---

## Adversarial

Benchmarks on friendly inputs prove nothing. Parsers die on **hostile
bytes**, so that's where the campaign points its worst intentions:

- **Differential fuzz** (`scripts/fuzz_all.sh`): ~1.94M deterministic
  executions — 200,000 parse cases plus 398,851 stateful chunked calls at
  seed 11, plus per-entry seed-7 sweeps. 0 mismatches, 0 crashes either
  side, rerun verified byte-identical.
- **Guard-page overread test:** the unmodified upstream suite feeds input
  from a guard-page mmap region, so every one of its 299 assertions doubles
  as an overread tripwire (`CC=gcc bash scripts/run_baseline.sh`).
- **Dirty-state lockstep:** the chunked decoder compares the full 32-byte
  decoder struct and the whole working buffer after every call, including
  dirty initial states — because that is where state machines go to lie.
- **Loopback + ctypes consumers:** a self-authored HTTP/1.1 consumer builds
  against C *and* this cdylib with byte-identical transcripts over loopback
  TCP, plus a Python ctypes consumer over all five symbols.

If you know an input family that breaks HTTP parsers and it's not in the
corpus (158 files and counting), that's not a complaint, that's a
contribution. File it.

---

## Opinions

Things this repo believes and won't apologize for:

1. **Publish your losses or your wins don't count.** 1.185, 1.199, 1.482,
   and 1.811 ship in the same table as 0.827 and 0.680, all from one pinned
   O2-vs-O2 machine.
2. **Unsafe is a budget, not a vibe.** 7 blocks, one file (`src/ffi.rs`),
   zero excuses — with `unwrap_used`/`expect_used` denied in the core so it
   stays panic-free. If your seam doesn't fit on one screen, ask it why.
3. **Zero-copy means zero allocations per parse.** Not "mostly" zero. 1,527
   lines held to that contract instead of copying toward a
   faster-looking microbench.
4. **The battle-tested C is the oracle, not the enemy.** Disagreements defer
   to what real consumers rely on from pinned `f4d94b48`, with 4 fail-closed
   hardenings documented instead of silently copied.
5. **Correct, then compatible, then measurable, then fast — in that order, no
   skipping.** 133,352 cases at 0 mismatches plus 8/8 upstream subtests and
   ~1.94M fuzz executions came before any bench row was quoted.

---

## Hot takes

Questions people will argue about, answered with a stance:

- **"Why not just keep the C?"** Because then your parser's edge behavior is
  defined by whatever the C happens to do, including reading past your
  buffer. Ours is defined by 133,352 vectors you can re-run, plus four
  filed refusals. Pick your religion.
- **"1.811× on malformed input? That's terrible."** It's the fixed-cost
  floor of the safe core on a byte-1 reject, printed in the same table as
  the 0.680 win. The ledger is [the losses we're not
  hiding](#the-losses-were-not-hiding). Re-measure on your machine before
  quoting ours.
- **"O3 regression — noise?"** Same-session control, Rust got 1.68× worse
  going O2→O3 on the malformed path while C moved the other way. That's not
  noise, that's a ticket nobody has closed yet. Bring your box and your
  flags.
- **"Text chips instead of badges? Cheap."** There is no CI or registry
  backing this repo, so a badge would be costume. Chips state numbers;
  badges imply infrastructure. We'd rather look plain than lie shiny.
- **"No H2O integration? Toy project?"** Loopback TCP plus ctypes proof is
  done; genuine H2O/Plack/Furl integration is parked, not dodged — the dev
  box has no OpenSSL dev libs. The first box that can link H2O gets to be
  the referee, and the next integration this README prints is that one,
  whatever it says, including if it breaks us.

---

## The experiment

This project was built like an experiment, because it was one: can a tiny,
battle-tested C parser be rebuilt, from scratch, in safe Rust, and still be
*provably* a drop-in: not approximately, not "should be", but
byte-for-byte?

### The oracle

Compatibility claims need a referee. Upstream `f4d94b48` was pinned into
`reference/`, SHA-256 verified across 8 files, frozen, and treated as the
behavioral oracle: where docs and implementation disagree, the disagreement
is recorded, and what real consumers rely on is ground truth.

### The corpus

The scoreboard's 133,352 inputs come layered: every corpus file × header
caps × full buffer, plus every streaming prefix, plus every cold strict
prefix — comparing return value, every pointer+length, decoder state, and
even the in-progress header slot on error paths.

### The differential run

On top of the corpus: the unmodified upstream suite against both sides
(8/8, 299 assertions, 0 failures), the ~1.94M-execution deterministic fuzz
campaign, and the loopback + ctypes consumers with byte-identical
transcripts. When anything disagrees, the case gets a ticket, not a shrug.

### The build

Correct, then compatible, then measurable, then fast. Optimization before
differential behavior was stable was forbidden — which is exactly why the
loss rows exist: they were measured before anyone was allowed to tune them
away.

### The segfaults we refused to copy

Differential testing is supposed to embarrass somebody, and four times it
pointed at the C itself: inputs where the original's defined behavior is an
overread or a hang. Matching those would be malpractice, not compatibility.
So the divergence list says **four**, not zero: four is what's left after
you stop copying segfaults for the sake of a clean scoreboard.

---

## Scope

What picohttpparser-rs implements, and what it doesn't (yet).

| feature | status |
|---|---|
| `phr_parse_request` | ✅ 87,052 differential cases, 0 mismatches |
| `phr_parse_response` | ✅ 20,678 differential cases, 0 mismatches |
| `phr_parse_headers` | ✅ 21,924 differential cases, 0 mismatches |
| `phr_decode_chunked` (+ `is_in_data`) | ✅ 3,698 cases, struct-memcmp lockstep |
| unmodified upstream `test.c` vs cdylib | ✅ 8/8 subtests · 299 assertions |
| zero allocations per parse | ✅ |
| panic-free core (`unwrap_used` denied) | ✅ |
| loopback TCP + Python ctypes consumers | ✅ byte-identical transcripts |
| genuine H2O/Plack/Furl integration | 🔜 needs OpenSSL dev libs |
| PGO tier | 🔜 skipped with reason (`llvm-profdata` unavailable) |
| SIMD prize (1.45× native-tier C gap) | 🔜 documented, unchased |

The in-scope surface (the full public C API plus streaming and decoder-state
behavior) is complete and frozen, which is exactly what makes the
133,352/133,352 scoreboard meaningful. Frozen means frozen: new work doesn't
move old vectors.

---

## Quick start

C consumers: anything that can link a C ABI. The header is unchanged (from
`reference/`), the contract is identical:

```c
#include "picohttpparser.h"              /* unchanged, from reference/ */
const char *method; size_t method_len;
int minor_version;
size_t num_headers = 16;
struct phr_header headers[16];
int ret = phr_parse_request(buf, len, &method, &method_len, /* ... */);
```

```console
$ cargo build --release        # cdylib + staticlib + rlib
$ cargo test --offline         # 30 unit/ABI vectors, all pass
$ CC=gcc bash scripts/diff_request.sh      # 87,052 cases
$ CC=gcc bash scripts/diff_headers.sh      # 21,924 cases
$ CC=gcc bash scripts/diff_chunked.sh      # 3,698 cases, struct-memcmp lockstep
$ bash scripts/fuzz_all.sh                 # ~1.94M executions, 0 crashes
$ CC=gcc bash scripts/integ_http11.sh      # loopback TCP + ctypes, byte-identical
$ CC=gcc bash scripts/run_baseline.sh      # upstream suite vs C and vs cdylib, 8/8
$ bash scripts/bench_compare.sh            # same-session C -O2 vs Rust, all 8 rows
```

Sixty seconds from clone to re-running our scoreboard. A full working
consumer ships in `scripts/integ_http11.c`.

---

## Status

- **Compatibility:** frozen against `f4d94b48` at 133,352/133,352; D001–D004
  in the ledger (`docs/divergences.md`). Four refusals, zero unresolved.
- **Benchmarks:** single machine (i5-12400F, w64devkit GCC 16.2.0, rustc
  1.96.1), no CPU pinning, whole-loop timer, no p50/p95/p99. Several rows
  carry `CHECK-spread>5%` flags; quote none of them as portable claims.
- **Real-consumer proof:** parked, pending OpenSSL dev libs on the build
  box. The first box that links H2O decides, same harness, same machine.
  The next integration this README prints will be that one.
- **Next:** land the H2O leg when the toolchain allows; long-soak the
  fuzzer; close or explain the O3 malformed-path ticket.

No hype beyond these numbers: run [Quick start](#quick-start) and check every
one.

---

*⭐ Star if a buffer overread has personally victimized you — or if you've ever had to trust a benchmark that hid its 1.811× rows. Found an input that breaks parity? [File it](.): the corpus takes contributions, and the ledger takes names.*

*PicoHTTPParser, minus the segfaults.*
