#!/usr/bin/env bash
#
# fuzz_triage.sh — minimizer for single-buffer differential mismatch repros.
#
# Usage: fuzz_triage.sh <entry> <casefile> [outfile]
#
#   entry     one of: request | response | headers | chunked
#   casefile  a single-buffer repro: a file that makes the C oracle and the
#             Rust replacement disagree (a fuzz driver's dump, or any file)
#   outfile   where the minimal repro is written (default:
#             target/triage/<entry>-<casefile-basename>.min; use '-' for
#             stdout only)
#
# Oracle: the mismatching property is "difftest binary for <entry> exits
# nonzero when handed exactly this one file". The difftest binaries are the
# same comparison the fuzz drivers use, so a fuzz mismatch dump can be
# re-checked byte-for-byte against the identical oracle. To point at a
# sibling fuzz driver's own checker instead, set:
#     FUZZ_ENTRY_BIN=<path-or-command>   (per-entry override, see below)
# The override receives the candidate file as its single argument and must
# exit 0 iff the candidate DOES reproduce a mismatch.
#
# Minimization (deterministic, dependency-free):
#   Phase 1 coarse tail removal: while the whole tail can be dropped and the
#          prefix still mismatches, drop it (cheap win on trailing junk).
#   Phase 2 ddmin-style chunk deletion: granularity n/2 -> 1, delete each
#          span, keep the deletion whenever the oracle still mismatches.
#   Every candidate shrink is verified against the oracle before being kept,
#   so the result is sound by construction (it reproduces the mismatch); it
#   is minimal only up to the step budget — see FUZZ_TRIAGE_MAX_STEPS.
#   Phase 3 single-byte substitution sweep: try 0x00 / 0x0A / 0x0D / 0x20 /
#          'A' / ':' / 'Z' at each byte and keep the first still-mismatching
#          substitute. Byte values were chosen to collapse structural noise
#          (CR/LF/space/colon dominate HTTP framing).
#
# Determinism: byte-identical input + same entry + same oracle binary +
# same env => byte-identical output. No RNG anywhere in this script.
set -u
set -o pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIFF_DIR="$ROOT/target/difftest"
TRIAGE_DIR="$ROOT/target/triage"
MAX_STEPS="${FUZZ_TRIAGE_MAX_STEPS:-4000}"
MAX_CASE="${FUZZ_TRIAGE_MAX_CASE:-1048576}" # refuse absurd inputs outright

usage() {
    sed -n '2,12p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//' >&2
    echo "  FUZZ_TRIAGE_MAX_STEPS=$MAX_STEPS FUZZ_TRIAGE_MAX_CASE=$MAX_CASE" >&2
    exit 2
}

[ $# -ge 2 ] || usage
ENTRY="$1"
CASE="$2"
OUTFILE="${3:-}"
[ -f "$CASE" ] || { echo "error: casefile not readable: $CASE" >&2; exit 2; }
case "$ENTRY" in
    request|response|headers|chunked) ;;
    *) echo "error: entry must be request|response|headers|chunked (got '$ENTRY')" >&2; usage ;;
esac

# --- locate the oracle binary ---------------------------------------------
BIN=""
if [ -n "${FUZZ_ENTRY_BIN:-}" ]; then
    # Per-entry map or a bare path/command; '%e' is replaced by the entry name.
    BIN="${FUZZ_ENTRY_BIN//%e/$ENTRY}"
    command -v "$BIN" >/dev/null 2>&1 || BIN="$(command -v "$(basename "$BIN")" 2>/dev/null || echo "$BIN")"
else
    for cand in "$DIFF_DIR/difftest_$ENTRY" "$DIFF_DIR/difftest_$ENTRY.exe"; do
        [ -x "$cand" ] && { BIN="$cand"; break; }
    done
    [ -n "$BIN" ] || {
        echo "error: oracle binary not found: $DIFF_DIR/difftest_$ENTRY(.exe)" >&2
        echo "  build it with: CC=gcc bash scripts/diff_$ENTRY.sh" >&2
        exit 2
    }
fi

# --- oracle: does THIS file reproduce a mismatch? --------------------------
# Exit 0 => mismatch reproduced; 1 => no mismatch; 2 => oracle error.
mismatches() {
    local f="$1"
    if [ -n "${FUZZ_ENTRY_BIN:-}" ]; then
        "$BIN" "$f" >/dev/null 2>&1
        return $?
    fi
    local rc
    "$BIN" "$f" >/dev/null 2>&1
    rc=$?
    case "$rc" in
        1) return 0 ;;  # difftest: mismatches>0
        0) return 1 ;;  # difftest: clean
        *) return 2 ;;  # usage / unreadable / oracle fault
    esac
}

# --- size guard ------------------------------------------------------------
SIZE=$(wc -c < "$CASE" 2>/dev/null | tr -d ' ')
[ -n "$SIZE" ] && [ "$SIZE" -gt 0 ] 2>/dev/null || { echo "error: empty casefile" >&2; exit 2; }
if [ "$SIZE" -gt "$MAX_CASE" ]; then
    echo "error: casefile is ${SIZE} bytes; FUZZ_TRIAGE_MAX_CASE=${MAX_CASE}" >&2
    echo "  shrink the interest region first (e.g. split at the framing boundary)." >&2
    exit 2
fi

# --- initial reproduction check -------------------------------------------
mismatches "$CASE"
rc=$?
if [ "$rc" -eq 1 ]; then
    echo "error: '$CASE' does NOT reproduce a mismatch against difftest_$ENTRY" >&2
    echo "  (oracle exited clean). Triage refuses to shrink a non-repro." >&2
    exit 1
elif [ "$rc" -eq 2 ]; then
    echo "error: oracle failed while checking '$CASE' (exit 2) — check the binary and entry." >&2
    exit 2
fi
echo "== '$CASE' reproduces (${SIZE} bytes); minimizing with entry '$ENTRY'" >&2
echo "== oracle: $BIN   (steps budget ${MAX_STEPS})" >&2

# --- workspace -------------------------------------------------------------
mkdir -p "$TRIAGE_DIR"
WORK="$TRIAGE_DIR/work-$ENTRY-$$"
mkdir -p "$WORK" || { echo "error: cannot mkdir $WORK" >&2; exit 2; }
trap 'rm -rf "$WORK"' EXIT
CUR="$WORK/cur"; cp "$CASE" "$CUR"
STEPS=0

keep() { # cp candidate into CUR if it still mismatches; echo 0 on keep
    local cand="$1"
    STEPS=$((STEPS + 1))
    if [ "$STEPS" -gt "$MAX_STEPS" ]; then
        echo "note: step budget ($MAX_STEPS) exhausted — keeping best-so-far." >&2
        return 1
    fi
    if mismatches "$cand"; then
        cp "$cand" "$CUR"
        return 0
    fi
    return 1
}

# byte helpers
nbytes() { wc -c < "$1" | tr -d ' '; }

# --- Phase 1: coarse tail removal ------------------------------------------
echo "== phase 1: tail removal" >&2
P1_START=$(nbytes "$CUR")
P1_DROPS=0
while :; do
    n=$(nbytes "$CUR")
    [ "$n" -gt 1 ] || break
    tailc="$WORK/tail"; head -c $((n - 1)) "$CUR" > "$tailc"
    if keep "$tailc"; then
        P1_DROPS=$((P1_DROPS + 1))
        continue
    fi
    break
done
[ "$P1_DROPS" -gt 0 ] && printf '  tail removal: %s -> %s bytes (%s drops)\n' "$P1_START" "$(nbytes "$CUR")" "$P1_DROPS" >&2

# --- Phase 2: ddmin-style span deletion ------------------------------------
echo "== phase 2: span deletion" >&2
DELTA_BUF="$WORK/del"
n=$(nbytes "$CUR")
g=$((n / 2)); [ "$g" -lt 1 ] && g=1
while [ "$g" -ge 1 ]; do
    start=0; progressed=0
    while [ "$start" -lt "$n" ]; do
        e=$((start + g)); [ "$e" -gt "$n" ] && e=$n
        # candidate = bytes before start + bytes from e on
        head -c "$start" "$CUR" > "$DELTA_BUF"
        tail -c $((n - e)) "$CUR" >> "$DELTA_BUF"
        if keep "$DELTA_BUF"; then
            n=$(nbytes "$CUR")
            printf '  deleted [%s,%s) (g=%s): now %s bytes\n' "$start" "$e" "$g" "$n" >&2
            progressed=1
            # restart granularity from the new size, classic ddmin speed-up
            g=$((n / 2)); [ "$g" -lt 1 ] && g=1
            start=0
            [ "$STEPS" -gt "$MAX_STEPS" ] && break 2
            continue
        fi
        start=$((start + g))
        [ "$STEPS" -gt "$MAX_STEPS" ] && break 2
    done
    [ "$progressed" -eq 1 ] && continue
    [ "$g" -eq 1 ] && break
    g=$(( (g + 1) / 2 ))  # ceil so g==1 is reached
done

# --- Phase 3: single-byte substitution sweep -------------------------------
echo "== phase 3: byte substitution" >&2
SUBST_BUF="$WORK/sub"
SUBSTS=(0 10 13 32 65 58 90)
n=$(nbytes "$CUR")
for ((i = 0; i < n && STEPS <= MAX_STEPS; i++)); do
    for v in "${SUBSTS[@]}"; do
        head -c "$i" "$CUR" > "$SUBST_BUF"
        printf "\\$(printf '%03o' "$v")" >> "$SUBST_BUF"
        tail -c $((n - i - 1)) "$CUR" >> "$SUBST_BUF"
        if keep "$SUBST_BUF"; then
            printf '  byte %s -> %s: now %s bytes\n' "$i" "$v" "$(nbytes "$CUR")" >&2
            n=$(nbytes "$CUR")
            break
        fi
    done
done

# --- emit ------------------------------------------------------------------
FINAL_SIZE=$(nbytes "$CUR")
# Re-verify the minimized artifact one final time before it leaves the tool.
if ! mismatches "$CUR"; then
    echo "error: minimized artifact no longer reproduces — internal bug, not emitted" >&2
    exit 1
fi
if [ -z "$OUTFILE" ]; then
    OUTFILE="$TRIAGE_DIR/$ENTRY-$(basename "$CASE")"
    [ "${OUTFILE##*.}" = "min" ] || OUTFILE="${OUTFILE}.min"
fi
if [ "$OUTFILE" = "-" ]; then
    cat "$CUR"
else
    cp "$CUR" "$OUTFILE"
    echo "== minimal repro (${FINAL_SIZE} bytes, ${STEPS} oracle runs) -> $OUTFILE" >&2
fi
exit 0
