#!/usr/bin/env bash
#
# bench_compare.sh — same-session C-vs-Rust benchmark suite (repo M9 /
# plan M8 measurement seam, schema v2).
#
# Builds the C oracle (renamed symbols) and one comparison binary that times
# BOTH sides in-process, interleaved (C,R / R,C), 7 trials with trial 0
# discarded as warmup: turbo/thermal/background drift becomes common-mode
# instead of a silent bias. Corpus comes verbatim from reference/bench.c
# (included by the harness source — zero drift by construction).
#
# Harness contract (scripts/bench_compare.c, sibling lane):
#   ./bench_compare            anchor run: TIMER, ITERS, TRIAL i C <ns> R <ns> x7
#   ./bench_compare <category> CATEGORY <name>, ITERS <n>, TIMER,
#                              TRIAL i C <ns> R <ns> x7
#   categories: tiny | typical | large | response | chunked | malformed |
#               streaming
#
# This driver runs the anchor + all seven categories, parses each run, and
# writes results/bench-compare.json as schema v2 (additive: every existing
# top-level anchor field is preserved byte-compatible for current readers; a
# new top-level "categories" object maps name -> {iters, cMeanNs, cMinNs,
# rMeanNs, rMinNs, ratio, stability, cTrialsNs, rTrialsNs, tier}).
#
# Per-run stats reuse the established awk pattern (trial 0 = warmup,
# discarded; mean/min over trials 1..6; spread > 5% => CHECK flag).
#
# Runtime bound: each binary invocation is capped at RUN_TIMEOUT_S seconds
# (default 180; "~3 min"). If a run trips the cap the suite stops and names
# the category — a hang can never stall the lane.
#
# Tier mechanism (unchanged): C_OPT/RUSTFLAGS/TIER/JSON_OUT env vars make a
# labeled side run; tiers apply to the anchor only and are recorded per row.
#
# Output: $JSON_OUT or results/bench-compare.json. These are INTERNAL
# engineering numbers (measurement), labeled with machine identity and
# artifact hashes — not publication claims.
set -u
set -o pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REF="$ROOT/reference"
OUT="$ROOT/target/bench-compare"
JSON="${JSON_OUT:-$ROOT/results/bench-compare.json}"
CC="${CC:-gcc}"
# Harness C source. Default: the in-tree harness. BENCH_C_SRC exists so the
# driver can be exercised against a contract-faithful harness before the
# sibling lane's scripts/bench_compare.c lands — default behavior unchanged.
BENCH_C_SRC="${BENCH_C_SRC:-$ROOT/scripts/bench_compare.c}"
# Labeled tiers (P8): C_OPT/RUSTFLAGS/TIER override NOTHING by default — the
# pinned anchor (C -O2 vs release profile) is what CI and the gate use.
# A tier run sets all three, e.g.:
#   C_OPT=-O3 RUSTFLAGS="-C opt-level=3" TIER=o3 \
#     JSON_OUT=results/bench-compare-o3.json bash scripts/bench_compare.sh
# cargo picks $RUSTFLAGS up automatically; the recorded profile reflects it.
C_OPT="${C_OPT:--O2}"
TIER="${TIER:-anchor}"
RUN_TIMEOUT_S="${RUN_TIMEOUT_S:-180}"

CATEGORIES=(tiny typical large response chunked malformed streaming)
CAT_COUNT=${#CATEGORIES[@]}

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
$CC $C_OPT -Wall -I"$REF" "$BENCH_C_SRC" "$OUT/oracle.o" \
  "$ROOT/target/release/picohttpparser_rs.dll.lib" -o "$OUT/bench_compare" || exit 1
cp "$ROOT/target/release/picohttpparser_rs.dll" "$OUT/"

# --- run_one <name> <arg...> ----------------------------------------------
# Runs the harness (capped), dumps the raw output, returns the harness rc.
# rc 124 => timed out (stopping condition).
run_one() {
    local name="$1"; shift
    local raw="$OUT/$name.txt"
    local rc
    if command -v timeout >/dev/null 2>&1; then
        (cd "$OUT" && timeout "$RUN_TIMEOUT_S" ./bench_compare "$@" > "$raw" 2>&1); rc=$?
    else
        (cd "$OUT" && ./bench_compare "$@" > "$raw" 2>&1); rc=$?
    fi
    echo "== run: $name (rc=$rc)"
    cat "$raw"
    return "$rc"
}

# --- stats_one <rawfile> ---------------------------------------------------
# Parses trials (skip trial 0 warmup) and computes mean/min/spread per side.
# Sets: n cmean cmin rmean rmin cspread rspread ratio c_runs r_runs iters
# timer_res via eval on stdout. Exits nonzero if no counted trials.
stats_one() {
    local raw="$1"
    local STATS
    STATS=$(awk '
      BEGIN { ni = 0 }
      /^ITERS / { iters=$2 }
      /^TIMER .*res_ns=/ { sub(/.*res_ns=/, "", $3); tres=$3 }
      /^TRIAL [1-6] / { c[ni]= $4; r[ni]= $6; ni++ }
      END {
        if (ni == 0) { print "NOTRIALS"; exit 1 }
        cs=0; rs=0; cmin=c[0]; rmin=r[0]; cmax=c[0]; rmax=r[0]
        for (i in c) { cs+=c[i]; rs+=r[i]
          if (c[i]<cmin) cmin=c[i]; if (c[i]>cmax) cmax=c[i]
          if (r[i]<rmin) rmin=r[i]; if (r[i]>rmax) rmax=r[i] }
        printf "n=%d cmean=%.1f cmin=%.0f rmean=%.1f rmin=%.0f cspread=%.3f rspread=%.3f ratio=%.4f iters=%s tres=%s", \
          ni, cs/ni, cmin, rs/ni, rmin, (cmax-cmin)/(cs/ni), (rmax-rmin)/(rs/ni), (rs/ni)/(cs/ni), iters, tres
      }' "$raw") || { echo "no counted trials parsed from $raw"; return 1; }
    eval "$STATS"  # n cmean cmin rmean rmin cspread rspread ratio iters tres
    c_runs=$(awk '/^TRIAL [1-6] /{printf "%s%s", sep, $4; sep=","}' "$raw")
    r_runs=$(awk '/^TRIAL [1-6] /{printf "%s%s", sep, $6; sep=","}' "$raw")
    [ -n "$n" ] || { echo "empty stats from $raw"; return 1; }
    return 0
}

# --- machine identity (same probes as run_baseline.sh §env) ----------------
CPU_MODEL="$(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | cut -d: -f2- | sed 's/^ *//;s/ *$//')"
[ -n "$CPU_MODEL" ] || CPU_MODEL="unknown"
CPU_COUNT="$(nproc 2>/dev/null || echo "${NUMBER_OF_PROCESSORS:-unknown}")"

# --- anchor run -------------------------------------------------------------
echo "== anchor run (interleaved trials)"
run_one trials || { echo "anchor run failed"; exit 1; }
stats_one "$OUT/trials.txt" || exit 1
A_C_RUNS="$c_runs"; A_R_RUNS="$r_runs"
A_CM=$cmean; A_CN=$cmin; A_RM=$rmean; A_RN=$rmin
A_RATIO=$ratio; A_CSP=$cspread; A_RSP=$rspread; A_ITERS=$iters; A_TRES=$tres
A_STAB="OK"
awk "BEGIN{exit !(($A_CSP > 0.05) || ($A_RSP > 0.05))}" && A_STAB="CHECK-spread>5%"
echo "== anchor: $n trials cmean=${A_CM} rmean=${A_RM} ratio=${A_RATIO} stability=${A_STAB}"

# --- category runs ----------------------------------------------------------
declare -A CAT_CM CAT_CN CAT_RM CAT_RN CAT_RAT CAT_STAB CAT_ITERS \
       CAT_C_RUNS CAT_R_RUNS
CAT_JSON=""
for cat in "${CATEGORIES[@]}"; do
    if ! run_one "$cat" "$cat"; then
        echo "ERROR: category '$cat' run failed (rc above)."
        exit 1
    fi
    # Integrity: the harness contract emits CATEGORY <name> on category runs.
    if ! grep -q "^CATEGORY $cat" "$OUT/$cat.txt"; then
        echo "ERROR: harness did not emit 'CATEGORY $cat' — the linked "
        echo "  bench_compare harness lacks category support (sibling lane's"
        echo "  scripts/bench_compare.c must land before full-suite runs)."
        exit 1
    fi
    stats_one "$OUT/$cat.txt" || exit 1
    CAT_CM[$cat]=$cmean; CAT_CN[$cat]=$cmin; CAT_RM[$cat]=$rmean; CAT_RN[$cat]=$rmin
    CAT_RAT[$cat]=$ratio; CAT_ITERS[$cat]=$iters
    CAT_C_RUNS[$cat]=$c_runs; CAT_R_RUNS[$cat]=$r_runs
    s="OK"; awk "BEGIN{exit !(($cspread > 0.05) || ($rspread > 0.05))}" && s="CHECK-spread>5%"
    CAT_STAB[$cat]=$s
    echo "== $cat: ${n} trials cmean=${CAT_CM[$cat]} rmean=${CAT_RM[$cat]} ratio=${CAT_RAT[$cat]} stability=${CAT_STAB[$cat]}"
done

# --- assemble JSON ----------------------------------------------------------
REV="$(git rev-parse --short HEAD 2>/dev/null || echo unknown)"
[ -n "$(git status --porcelain 2>/dev/null | grep -v '^?? target/')" ] && REV="${REV}-dirty"
DLL_SHA="$(sha256sum "$ROOT/target/release/picohttpparser_rs.dll" 2>/dev/null | cut -d' ' -f1)"
RUSTC_V="$(rustc --version 2>/dev/null || echo unknown)"
CC_V="$($CC --version 2>/dev/null | head -n1 || echo unknown)"
CORPUS_SHA="$(sha256sum "$REF/bench.c" 2>/dev/null | cut -d' ' -f1)"
CAT_ROW=""
for cat in "${CATEGORIES[@]}"; do
    row=$(printf '    "%s": {
      "iters": %s,
      "cMeanNs": %s,
      "cMinNs": %s,
      "rMeanNs": %s,
      "rMinNs": %s,
      "ratio": %s,
      "stability": "%s",
      "cTrialsNs": [%s],
      "rTrialsNs": [%s],
      "tier": "%s"
    }' \
        "$cat" "${CAT_ITERS[$cat]}" "${CAT_CM[$cat]}" "${CAT_CN[$cat]}" "${CAT_RM[$cat]}" "${CAT_RN[$cat]}" \
        "${CAT_RAT[$cat]}" "${CAT_STAB[$cat]}" "${CAT_C_RUNS[$cat]}" "${CAT_R_RUNS[$cat]}" "$TIER")
    [ -n "$CAT_ROW" ] && CAT_ROW="$CAT_ROW,"
    CAT_ROW="$CAT_ROW$row"
done

mkdir -p "$ROOT/results"
cat > "$JSON" <<EOF
{
  "schemaVersion": 2,
  "generatedUtc": "$(date -u +%Y-%m-%dT%H:%M:%SZ)",
  "rev": "$(jstr "$REV")",
  "status": "internal-engineering-number (measurement, not a publication claim)",
  "machine": {
    "cpu": "$(jstr "$CPU_MODEL")",
    "cpuCount": "$(jstr "$CPU_COUNT")"
  },
  "timer": {
    "name": "clock_gettime(CLOCK_MONOTONIC), in-process",
    "resNs": "${A_TRES:-100}"
  },
  "protocol": "anchor + 7 categories; each: 7 trials interleaved C,R/R,C; trial 0 discarded as warmup; per-iteration result check both sides",
  "c": {
    "buildCommand": "$(jstr "$CC $C_OPT (-Dmain=bench_c_main -Dphr_parse_request=c_phr_parse_request via bench_compare.c include of reference/bench.c) + renamed oracle object")",
    "ccVersion": "$(jstr "$CC_V")",
    "trialsNs": [$A_C_RUNS],
    "meanNs": $A_CM,
    "minNs": $A_CN
  },
  "rust": {
    "artifact": "target/release/picohttpparser_rs.dll",
    "artifactSha256": "$(jstr "$DLL_SHA")",
    "rustc": "$(jstr "$RUSTC_V")",
    "profile": "pinned(opt-level=2,CGU1,LTO,overflow-off,debug-off,unwind)${RUSTFLAGS:+ PLUS RUSTFLAGS=$RUSTFLAGS}",
    "tier": "$(jstr "$TIER")",
    "trialsNs": [$A_R_RUNS],
    "meanNs": $A_RM,
    "minNs": $A_RN
  },
  "ratioRustOverC_mean": $A_RATIO,
  "stability": "$A_STAB",
  "corpus": {
    "file": "reference/bench.c REQ macro (included verbatim, zero drift by construction)",
    "sha256": "$(jstr "$CORPUS_SHA")",
    "scope": "one fixed ~620B GET x10M, happy path only",
    "iterations": ${A_ITERS:-10000000}
  },
  "categories": {
$CAT_ROW
  }
}
EOF
echo "wrote $JSON (schema v2; anchor ratio mean: $A_RATIO, stability: $A_STAB; ${CAT_COUNT} category rows recorded)"
