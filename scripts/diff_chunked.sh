#!/usr/bin/env bash
#
# diff_chunked.sh — Layer-2 differential test, chunked decoder (Milestone 5).
#
# Builds the C oracle (symbols renamed to c_*) and the Rust release cdylib,
# links both into scripts/difftest_chunked.c, and runs it over
# tests/corpus/chunked/. Compares ret, decoded length, is_in_data, the full
# decoder struct, and the entire working buffer after every call — across
# single calls, exhaustive two-way splits, three-way splits, both
# consume_trailer settings, and dirty initial decoder states.
# Exit 0 iff zero mismatches.
set -u
set -o pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REF="$ROOT/reference"
OUT="$ROOT/target/difftest"
CC="${CC:-gcc}"

mkdir -p "$OUT"

echo "== cargo build --release (Rust cdylib under test)"
(cd "$ROOT" && cargo build --release) || exit 1

echo "== compile C oracle (renamed symbols)"
$CC -O2 -Wall \
  -Dphr_parse_request=c_phr_parse_request \
  -Dphr_parse_response=c_phr_parse_response \
  -Dphr_parse_headers=c_phr_parse_headers \
  -Dphr_decode_chunked=c_phr_decode_chunked \
  -Dphr_decode_chunked_is_in_data=c_phr_decode_chunked_is_in_data \
  -c "$REF/picohttpparser.c" -o "$OUT/oracle.o" || exit 1

echo "== link differential harness (against the cdylib: the shipped artifact)"
# NOTE: we link the cdylib import lib, not the staticlib: MinGW ld cannot
# satisfy the staticlib's MSVC C++ EH residue (panic_unwind type_info), while
# the DLL carries its own resolved imports. DLL must sit next to the binary.
$CC -O2 -Wall -I"$REF" "$ROOT/scripts/difftest_chunked.c" "$OUT/oracle.o" \
  "$ROOT/target/release/picohttpparser_rs.dll.lib" -o "$OUT/difftest_chunked" || exit 1
cp "$ROOT/target/release/picohttpparser_rs.dll" "$OUT/"

echo "== run over corpus (teeing to results/difftest-chunked.log)"
# Run identity: a bare totals line cannot prove freshness, so the log opens
# with when/what-revision ran it (Finding 2, M6 gate audit).
{ echo "run_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)";
   _rev=$(git rev-parse --short HEAD 2>/dev/null || echo unknown);
   # -dirty iff tracked or corpus/script changes are uncommitted (target/ ignored).
   [ -n "$(git status --porcelain 2>/dev/null | grep -v '^?? target/')" ] && _rev="${_rev}-dirty";
   echo "rev=$_rev"; } | tee "$ROOT/results/difftest-chunked.log"
# shellcheck disable=SC2086
# shellcheck disable=SC2086
_DIFF_T0=$(date +%s%N)
"$OUT/difftest_chunked" $ROOT/tests/corpus/chunked/valid/* $ROOT/tests/corpus/chunked/malformed/* 2>&1 | tee -a "$ROOT/results/difftest-chunked.log"
echo "elapsed_ms=$(( ($(date +%s%N) - _DIFF_T0) / 1000000 ))" | tee -a "$ROOT/results/difftest-chunked.log"
