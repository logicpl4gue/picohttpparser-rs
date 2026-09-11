# picohttpparser-rs

**PicoHTTPParser had decades to stop segfaulting on hostile input. It didn't. So we rebuilt it: 1,527 lines of Rust, zero allocations per parse, every behavior pinned against the C original instead of guessed.**

`◉ C agreement: 133,352 / 133,352` · `◉ upstream suite: 8/8 · 299 assertions` · `◉ fuzz: ~1.94M executions, 0 crashes` · `◉ unsafe: 7 blocks, one file` · `◉ slow rows hidden: 0` · `◉ license: MIT`

> Your HTTP parser has corners that segfault. Ours has four, all filed, all refused on purpose.

[Scoreboard](#scoreboard) · [Speed](#speed) · [Adversarial](#adversarial) · [The fight](#the-fight) · [Opinions](#opinions) · [Hot takes](#hot-takes) · [Scope](#scope) · [Quick start](#quick-start)

---

## Scoreboard

Same machine, same inputs, both implementations. Not "passes its own suite": every case reproduces the C original's return value, pointers, lengths, and decoder state exactly.

```
picohttpparser-rs vs PicoHTTPParser f4d94b48   (pinned; SHA-256 across 8 files)

  differential cases    ████████████████████  133,352 / 133,352  agree
  upstream suite        ████████████████████  8/8 subtests · 299 assertions
  fuzz executions       ████████████████████  ~1.94M · 0 crashes either side
  divergences           D001–D004             every one fail-closed, none silent
  real-consumer proof   ░░░░░░░░░░░░░░░░░░░░  parked: no OpenSSL dev libs (see Scope)
```

**133,352 / 133,352.** Not "passes its own test suite": every case reproduces the C original's return value, pointers, lengths, and decoder state exactly.

Request 87,052 · response 20,678 · headers 21,924 · chunked 3,698 (struct-memcmp lockstep). The four divergences are one species: inputs where C's defined behavior is an overread or a hang, where this crate returns `-2`. Matching a segfault is malpractice, not compatibility. Reproducers and reasoning in `docs/divergences.md`.

---

## Speed

Rust/C ratio, same session, interleaved trials, same bytes. Below 1.00 means Rust is faster, and the losses ship in the same font size as the wins.

| workload | Rust/C | reading |
|---|---:|---|
| large headers (~4.9 KB) | **0.680** | Rust ~32% faster |
| upstream marker (~620 B, 11 headers) | **0.827** | Rust ~17% faster · `stability: CHECK` |
| streaming (8 staged prefixes) | 1.023 | near parity |
| typical request (~400 B) | 1.100 | within 10% |
| response (~150 B) | 1.185 | loss, published |
| chunked decode (38 B, 3 chunks) | 1.199 | loss, published |
| tiny request (37 B) | 1.482 | fixed-cost floor |
| malformed (reject at byte 1) | 1.811 | fixed-cost floor |

Tiny inputs pay an honest fixed-cost floor (1.482× / 1.811×); response and chunked are straight losses. A benchmark page without a loss row is telling you what the author needed to be true. Labeled side data: `-O3` regresses Rust 1.68× on the malformed path while C improves, and native-tier C (`-march=native`, pcmpestri) beats scalar Rust 1.45×, the documented SIMD prize. Repro: `scripts/bench_compare.sh`.

---

## Adversarial

Friendly inputs prove nothing, so the campaign points at hostile bytes:

- **Differential fuzz** (`scripts/fuzz_all.sh`): ~1.94M executions, 0 mismatches, 0 crashes, rerun byte-identical.
- **Guard-page overread test:** the upstream suite feeds input from a guard-page mmap region, so all 299 assertions double as overread tripwires.
- **Dirty-state lockstep:** full 32-byte decoder struct + working buffer compared after every chunked call, dirty initial states included.
- **Loopback + ctypes consumers:** byte-identical transcripts over loopback TCP, plus a Python ctypes consumer over all five symbols.

Know an input family that breaks HTTP parsers and isn't in the corpus (158 files)? That's a contribution. File it.

---

## The fight

| Alternative | Steelman (why smart teams pick it) | Where this crate disagrees |
|---|---|---|
| Just keep the battle-tested C | Allocation-free, zero-copy, shipped in real software. Correct by survival. | Agreed, which is why it stayed as oracle instead of being replaced on vibes. The only claim here is indistinguishability: 133,352 cases, 0 mismatches. |
| Just rewrite it in the safe language of the month | A fresh port can delete whole bug classes on day one. | A port without an oracle is a new parser wearing the old name. Ours replays every byte against pinned `f4d94b48`: 87,052 request + 20,678 response + 21,924 headers + 3,698 chunked, 0 mismatches. |
| Just use a full HTTP framework | Routing, TLS, and H2 for free; hand-rolled parsing looks like yak-shaving. | If you need a framework, use one. If you need this seam, relink it: identical contract, byte-identical loopback transcripts against C and Rust. |
| Just claim Rust beats C and post the wins | Two green rows (0.827, 0.680) would make a great headline. | Headline-benchmarking is how rewrites gaslight readers. The losses ship in the same table: 1.185, 1.199, 1.482, 1.811. |

---

## Opinions

1. **Publish your losses or your wins don't count.**
2. **Unsafe is a budget, not a vibe.** 7 blocks, one file (`src/ffi.rs`), `unwrap_used` denied in the core.
3. **Zero-copy means zero allocations per parse.** Not "mostly" zero.
4. **The battle-tested C is the oracle, not the enemy.** Four fail-closed hardenings, documented, never silently copied.
5. **Correct, then compatible, then measurable, then fast.** In that order. No skipping.

Five beliefs, each defensible in one breath. Argue with any of them in issues.

---

## Hot takes

Questions people will argue about, answered with a stance:

- **"Why not just keep the C?"** Because then your edge behavior is whatever the C happens to do, including reading past your buffer. Ours is defined by 133,352 vectors you can re-run, plus four filed refusals. Pick your religion.
- **"1.811× on malformed input? Embarrassing."** It's the fixed-cost floor of the safe core on a byte-1 reject, printed in the same table as the 0.680 win. Re-measure on your machine before quoting ours.
- **"O3 regression, noise?"** Same-session control: Rust got 1.68× worse going O2 to O3 on the malformed path while C moved the other way. That's not noise, that's an open ticket. Bring your box and your flags.
- **"Text chips instead of badges? Cheap."** There is no CI or registry behind this repo, so a badge would be costume. Chips state numbers; badges imply infrastructure. Plain beats shiny when shiny would be lying.
- **"No H2O integration? Toy project?"** Loopback TCP plus ctypes proof is done; genuine H2O/Plack/Furl integration is parked, not dodged. The first box with OpenSSL dev libs gets to be the referee, and the next integration this README prints is that one, whatever it says, including if it breaks us.

---

## Scope

| feature | status |
|---|---|
| `phr_parse_request` | ✅ 87,052 cases, 0 mismatches |
| `phr_parse_response` | ✅ 20,678 cases, 0 mismatches |
| `phr_parse_headers` | ✅ 21,924 cases, 0 mismatches |
| `phr_decode_chunked` (+ `is_in_data`) | ✅ 3,698 cases, struct-memcmp lockstep |
| upstream `test.c` vs cdylib | ✅ 8/8 · 299 assertions |
| zero allocations per parse · panic-free core | ✅ |
| loopback TCP + ctypes consumers | ✅ byte-identical |
| H2O/Plack/Furl integration · PGO · SIMD prize | 🔜 parked, each with a filed reason |

Frozen means frozen: new work doesn't move old vectors. **[YOU DECIDE] Which real-consumer proof lands first**, H2O for server-grade pressure or Plack/Furl for ecosystem parity? Vote in issues.

---

## Quick start

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
```

Sixty seconds from clone to re-running the scoreboard. Limits that apply: single machine (i5-12400F, GCC 16.2.0, rustc 1.96.1), no CPU pinning, whole-loop timer, no p50/p95/p99. Quote none of it as portable.

---

*⭐ Star if a buffer overread has personally victimized you, or if you've ever trusted a benchmark that hid its 1.811× rows. Found an input that breaks parity? File it: the corpus takes contributions, and the ledger takes names.*

*PicoHTTPParser, minus the segfaults.*
