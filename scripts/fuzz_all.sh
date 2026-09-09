#!/usr/bin/env bash
#
# fuzz_all.sh — one-shot fuzz campaign wrapper: runs every available fuzz
# driver (bounded defaults) then all four differential gates, and prints a
# summary table (cases / mismatches per target + exit code).
#
# Usage: fuzz_all.sh [--fuzz-only|--gates-only]
#
# Fuzz drivers are the deterministic differential drivers in scripts/:
#   scripts/fuzz_parse.sh     (request/response/headers corpus mutator)
#   scripts/fuzz_chunked.sh   (chunked-decoder corpus mutator)
# Any other scripts/fuzz_*.sh present is picked up automatically (a script
# named fuzz_<target>.sh whose stdout ends in a parseable
# "cases=… mismatches=…" totals line). Missing drivers are reported SKIPPED,
# never fabricated as zero.
#
# Bounded defaults: FUZZ_SEED (default 1) and FUZZ_ITERS (default 20000) are
# exported to every driver as SEED/ITERS. A driver that honours those env
# vars is bounded by them; one that does not runs its own defaults and the
# summary notes "ignores-ITERS".
#
# Determinism: FUZZ_SEED fixed => every driver run is reproducible; the log
# records rev + utc + seed + iters so a run can be re-executed verbatim.
set -u
set -o pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LOG_DIR="$ROOT/target/fuzz"
RESULTS_DIR="$ROOT/results"
FUZZ_SEED="${FUZZ_SEED:-1}"
FUZZ_ITERS="${FUZZ_ITERS:-20000}"
MODE="all"
[ $# -ge 1 ] && MODE="${1#--}"
case "$MODE" in all|fuzz-only|gates-only) ;; *) echo "usage: fuzz_all.sh [--fuzz-only|--gates-only]" >&2; exit 2 ;; esac
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
REV="$(git rev-parse --short HEAD 2>/dev/null || echo unknown)"
[ -n "$(git status --porcelain 2>/dev/null | grep -v '^?? target/')" ] && REV="${REV}-dirty"
LOG="$LOG_DIR/fuzz-all-$STAMP.log"
SUMMARY="$LOG_DIR/summary-$STAMP.tmp"

mkdir -p "$LOG_DIR"
: > "$SUMMARY"
add_row() { printf '%s|%s|%s|%s|%s|%s\n' "$1" "$2" "$3" "$4" "$5" "$6" >> "$SUMMARY"; }

run_all() {
    echo "run_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "rev=$REV seed=$FUZZ_SEED iters=$FUZZ_ITERS mode=$MODE"

    # --- 1. fuzz drivers ----------------------------------------------------
    echo
    echo "== fuzz drivers"
    DRIVERS=()
    if [ "$MODE" != "gates-only" ]; then
        for f in "$ROOT"/scripts/fuzz_*.sh; do
            [ -e "$f" ] || continue
            b="$(basename "$f")"
            case "$b" in fuzz_all.sh | fuzz_triage.sh) continue ;; esac
            DRIVERS+=("$b")
        done
        if [ "${#DRIVERS[@]}" -eq 0 ]; then
            echo "  (no fuzz drivers present yet — run the sibling fuzz lane first)"
            add_row "none" "fuzz" "-" "-" 0 "drivers-not-built"
        fi
    fi
    for d in "${DRIVERS[@]}"; do
        tgt="${d#fuzz_}"; tgt="${tgt%.sh}"
        out="$LOG_DIR/$tgt-$STAMP.log"
        echo "  -- $d (seed=$FUZZ_SEED iters=$FUZZ_ITERS)"
        SEED="$FUZZ_SEED" ITERS="$FUZZ_ITERS" bash "$ROOT/scripts/$d" >"$out" 2>&1
        rc=$?
        # Parse the driver's own totals line defensively; drivers may label
        # executions cases= / calls= / execs= — accept any, prefer last.
        cases=$(grep -oE '(cases|calls|execs)=[0-9]+' "$out" 2>/dev/null | tail -1 | cut -d= -f2)
        mism=$(grep -oE 'mismatches=[0-9]+' "$out" 2>/dev/null | tail -1 | cut -d= -f2)
        if [ -z "$cases" ] && [ "$rc" -eq 0 ]; then
            note="ignores-ITERS"
        else
            note=""
        fi
        [ "$rc" -eq 0 ] || note="$note rc=$rc"
        add_row "$tgt" "fuzz" "${cases:---}" "${mism:---}" "$rc" "$note"
        echo "      rc=$rc cases=${cases:---} mismatches=${mism:---}"
    done

    # --- 2. differential gates ---------------------------------------------
    echo
    echo "== differential gates"
    if [ "$MODE" != "fuzz-only" ]; then
        for t in request response headers chunked; do
            log="$RESULTS_DIR/difftest-$t.log"
            echo "  -- diff_$t.sh"
            CC="${CC:-gcc}" bash "$ROOT/scripts/diff_$t.sh" >/dev/null 2>&1
            rc=$?
            cases=$(grep -oE 'cases=[0-9]+' "$log" 2>/dev/null | tail -1 | cut -d= -f2)
            mism=$(grep -oE 'mismatches=[0-9]+' "$log" 2>/dev/null | tail -1 | cut -d= -f2)
            add_row "$t" "gate" "${cases:---}" "${mism:---}" "$rc" "log=$(basename "$log")"
            echo "      rc=$rc cases=${cases:---} mismatches=${mism:---}"
        done
    fi

    # --- 3. summary ---------------------------------------------------------
    echo
    echo "===================== FUZZ CAMPAIGN SUMMARY ====================="
    printf '%-10s %-6s %10s %11s %4s  %s\n' "TARGET" "KIND" "CASES" "MISMATCHES" "RC" "NOTE"
    while IFS='|' read -r t kind cases mism rc note; do
        printf '%-10s %-6s %10s %11s %4s  %s\n' "$t" "$kind" "${cases:---}" \
            "${mism:---}" "$rc" "$note"
    done < "$SUMMARY"
    echo "================================================================="
    echo "seed=$FUZZ_SEED iters=$FUZZ_ITERS rev=$REV"
}

FAIL=0
run_all | tee "$LOG"
# Exit nonzero iff any gate failed or any present driver failed.
while IFS='|' read -r t kind cases mism rc note; do
    [ "$rc" -eq 0 ] || FAIL=1
done < "$SUMMARY"
rm -f "$SUMMARY"
echo "full log: $LOG"
exit $FAIL
