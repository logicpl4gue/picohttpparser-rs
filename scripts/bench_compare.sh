#!/usr/bin/env bash
#
# bench_compare.sh — same-session C-vs-Rust benchmark (M8 measurement seam).
#
# Builds the C oracle (renamed symbols) and one comparison binary that times
# BOTH sides in-process, interleaved (C,R / R,C), 7 trials with trial 0
# discarded as warmup: turbo/thermal/background drift becomes common-mode
# instead of a silent bias. Corpus comes verbatim from reference/bench.c
# (included by the harness source — zero drift by construction).
#
# Output: results/bench-compare.json (schema v1, this file's own versioning),
# or $JSON_OUT for labeled P8 tiers (anchor file is never overwritten).
# These are INTERNAL engineering numbers (Phase-0 measurement), labeled with
# machine identity and artifact hashes — not publication claims.
set -u
set -o pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REF="$ROOT/reference"
OUT="$ROOT/target/bench-compare"
JSON="${JSON_OUT:-$ROOT/results/bench-compare.json}"
CC="${CC:-gcc}"
# Labeled tiers (P8): C_OPT/RUSTFLAGS/TIER override NOTHING by default — the
# pinned anchor (C -O2 vs release profile) is what CI and the gate use.
# A tier run sets all three, e.g.:
#   C_OPT=-O3 RUSTFLAGS="-C opt-level=3" TIER=o3 \
#     JSON_OUT=results/bench-compare-o3.json bash scripts/bench_compare.sh
# cargo picks $RUSTFLAGS up automatically; the recorded profile reflects it.
C_OPT="${C_OPT:--O2}"
TIER="${TIER:-anchor}"

mkdir -p "$OUT"

jstr() { printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g'; }

echo "== cargo build --release (Rust cdylib under test)"
(cd "$ROOT" && cargo build --release) || exit 1

echo "== compile C oracle (renamed symbols)"
$CC $C_OPT -Wall \
  -Dphr_parse_request=c_phr_parse_request \
  -Dphr_parse_response=c_phr_parse_response \
  -Dphr_parse_headers=c_phr_parse_headers \
  -Dphr_decode_chunked=c_phr_decode_chunked \
  -Dphr_decode_chunked_is_in_data=c_phr_decode_chunked_is_in_data \
  -c "$REF/picohttpparser.c" -o "$OUT/oracle.o" || exit 1

echo "== link comparison binary (cdylib import lib: the shipped artifact)"
$CC $C_OPT -Wall -I"$REF" "$ROOT/scripts/bench_compare.c" "$OUT/oracle.o" \
  "$ROOT/target/release/picohttpparser_rs.dll.lib" -o "$OUT/bench_compare" || exit 1
cp "$ROOT/target/release/picohttpparser_rs.dll" "$OUT/"

echo "== run interleaved trials (~1 min)"
RAW="$OUT/trials.txt"
(cd "$OUT" && ./bench_compare > trials.txt 2>&1); RC=$?
cat "$RAW"
[ "$RC" -eq 0 ] || { echo "comparison binary failed"; exit 1; }

# Machine identity (same probes as run_baseline.sh §env).
CPU_MODEL="$(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | cut -d: -f2- | sed 's/^ *//;s/ *$//')"
[ -n "$CPU_MODEL" ] || CPU_MODEL="unknown"
CPU_COUNT="$(nproc 2>/dev/null || echo "${NUMBER_OF_PROCESSORS:-unknown}")"

# Parse trials (skip trial 0 warmup), compute means/mins/spread in awk.
STATS=$(awk '
  BEGIN { ni = 0 }
  /^TRIAL [1-6] / { c[ni]= $4; r[ni]= $6; ni++ }
  END {
    if (ni == 0) { print "NOTRIALS"; exit 1 }
    cs=0; rs=0; cmin=c[0]; rmin=r[0]; cmax=c[0]; rmax=r[0]
    for (i in c) { cs+=c[i]; rs+=r[i]
      if (c[i]<cmin) cmin=c[i]; if (c[i]>cmax) cmax=c[i]
      if (r[i]<rmin) rmin=r[i]; if (r[i]>rmax) rmax=r[i] }
    printf "n=%d cmean=%.1f cmin=%.0f rmean=%.1f rmin=%.0f cspread=%.3f rspread=%.3f ratio=%.4f", \
      ni, cs/ni, cmin, rs/ni, rmin, (cmax-cmin)/(cs/ni), (rmax-rmin)/(rs/ni), (rs/ni)/(cs/ni)
  }' "$RAW") || { echo "no counted trials parsed"; exit 1; }
echo "== $STATS"
eval "$STATS"  # sets n/cmean/cmin/rmean/rmin/cspread/rspread/ratio

C_RUNS=$(awk '/^TRIAL [1-6] /{printf "%s%s", sep, $4; sep=","}' "$RAW")
R_RUNS=$(awk '/^TRIAL [1-6] /{printf "%s%s", sep, $6; sep=","}' "$RAW")
TIMER_RES=$(awk '/^TIMER /{print $3}' "$RAW" | sed 's/res_ns=//')
REV="$(git rev-parse --short HEAD 2>/dev/null || echo unknown)"
[ -n "$(git status --porcelain 2>/dev/null | grep -v '^?? target/')" ] && REV="${REV}-dirty"
DLL_SHA="$(sha256sum "$ROOT/target/release/picohttpparser_rs.dll" 2>/dev/null | cut -d' ' -f1)"
RUSTC_V="$(rustc --version 2>/dev/null || echo unknown)"
CC_V="$($CC --version 2>/dev/null | head -n1 || echo unknown)"
CORPUS_SHA="$(sha256sum "$REF/bench.c" 2>/dev/null | cut -d' ' -f1)"
STABLE="OK"
awk "BEGIN{exit !(($cspread > 0.05) || ($rspread > 0.05))}" && STABLE="CHECK-spread>5%"

mkdir -p "$ROOT/results"
cat > "$JSON" <<EOF
{
  "schemaVersion": 1,
  "generatedUtc": "$(date -u +%Y-%m-%dT%H:%M:%SZ)",
  "rev": "$(jstr "$REV")",
  "status": "internal-engineering-number (Phase-0 measurement, not a publication claim)",
  "machine": {
    "cpu": "$(jstr "$CPU_MODEL")",
    "cpuCount": "$(jstr "$CPU_COUNT")"
  },
  "timer": {
    "name": "clock_gettime(CLOCK_MONOTONIC), in-process",
    "resNs": "$TIMER_RES"
  },
  "protocol": "7 trials interleaved C,R/R,C; trial 0 discarded as warmup; 10M iters/trial; per-iteration ret==len check both sides",
  "c": {
    "buildCommand": "$(jstr "$CC $C_OPT (-Dmain=bench_c_main -Dphr_parse_request=c_phr_parse_request via bench_compare.c include of reference/bench.c) + renamed oracle object")",
    "ccVersion": "$(jstr "$CC_V")",
    "trialsNs": [$C_RUNS],
    "meanNs": $cmean,
    "minNs": $cmin
  },
  "rust": {
    "artifact": "target/release/picohttpparser_rs.dll",
    "artifactSha256": "$(jstr "$DLL_SHA")",
    "rustc": "$(jstr "$RUSTC_V")",
    "profile": "pinned(opt-level=2,CGU1,LTO,overflow-off,debug-off,unwind)${RUSTFLAGS:+ PLUS RUSTFLAGS=$RUSTFLAGS}",
    "tier": "$(jstr "$TIER")",
    "trialsNs": [$R_RUNS],
    "meanNs": $rmean,
    "minNs": $rmin
  },
  "ratioRustOverC_mean": $ratio,
  "stability": "$STABLE",
  "corpus": {
    "file": "reference/bench.c REQ macro (included verbatim, zero drift by construction)",
    "sha256": "$(jstr "$CORPUS_SHA")",
    "scope": "one fixed ~620B GET x10M, happy path only",
    "iterations": 10000000
  }
}
EOF
echo "wrote $JSON (ratio mean: $ratio, stability: $STABLE)"
