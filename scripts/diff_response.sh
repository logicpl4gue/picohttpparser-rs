#!/usr/bin/env bash
#
# diff_response.sh — Layer-2 differential test, response parser (Milestone 3).
#
# Builds the C oracle (symbols renamed to c_*) and the Rust release cdylib,
# links both into scripts/difftest_response.c, and runs it over
# tests/corpus/response/. Compares ret, version, status, reason, header
# count, and every name/value (pointer+length into the shared buffer),
# including the in-progress slot on error paths. Exit 0 iff zero mismatches.
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
$CC -O2 -Wall -I"$REF" "$ROOT/scripts/difftest_response.c" "$OUT/oracle.o" \
  "$ROOT/target/release/picohttpparser_rs.dll.lib" -o "$OUT/difftest_response" || exit 1
cp "$ROOT/target/release/picohttpparser_rs.dll" "$OUT/"

echo "== run over corpus (teeing to results/difftest-response.log)"
# Run identity: a bare totals line cannot prove freshness, so the log opens
# with when/what-revision ran it (Finding 2, M6 gate audit).
{ echo "run_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)";
   _rev=$(git rev-parse --short HEAD 2>/dev/null || echo unknown);
   # -dirty iff tracked or corpus/script changes are uncommitted (target/ ignored).
   [ -n "$(git status --porcelain 2>/dev/null | grep -v '^?? target/')" ] && _rev="${_rev}-dirty";
   echo "rev=$_rev"; } | tee "$ROOT/results/difftest-response.log"
# shellcheck disable=SC2086
# shellcheck disable=SC2086
"$OUT/difftest_response" $ROOT/tests/corpus/response/valid/* $ROOT/tests/corpus/response/malformed/* 2>&1 | tee -a "$ROOT/results/difftest-response.log"
