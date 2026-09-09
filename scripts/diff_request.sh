#!/usr/bin/env bash
#
# diff_request.sh — Layer-2 differential test, request parser (Milestone 2).
#
# Builds the C oracle (symbols renamed to c_*) and the Rust release cdylib,
# links both into scripts/difftest_request.c, and runs it over
# tests/corpus/request/. The harness compares every observable output of both
# implementations on full buffers plus every prefix streaming plus every
# strict prefix cold (last_len = 0) across header caps {0,1,2,3,5,16,64}.
# Exit 0 iff zero mismatches.
#
# Honesty contract: the harness prints files/cases/mismatches; any mismatch
# is a stored reproduction (the corpus file + cap + prefix in the log).
set -u
set -o pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REF="$ROOT/reference"
OUT="$ROOT/target/difftest"
CC="${CC:-gcc}"

mkdir -p "$OUT"

echo "== cargo build --release (Rust staticlib under test)"
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
$CC -O2 -Wall -I"$REF" "$ROOT/scripts/difftest_request.c" "$OUT/oracle.o" \
  "$ROOT/target/release/picohttpparser_rs.dll.lib" -o "$OUT/difftest_request" || exit 1
cp "$ROOT/target/release/picohttpparser_rs.dll" "$OUT/"

echo "== run over corpus (teeing to results/difftest-request.log)"
# Run identity: a bare totals line cannot prove freshness, so the log opens
# with when/what-revision ran it (Finding 2, M6 gate audit).
{ echo "run_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)";
   _rev=$(git rev-parse --short HEAD 2>/dev/null || echo unknown);
   # -dirty iff tracked or corpus/script changes are uncommitted (target/ ignored).
   [ -n "$(git status --porcelain 2>/dev/null | grep -v '^?? target/')" ] && _rev="${_rev}-dirty";
   echo "rev=$_rev"; } | tee "$ROOT/results/difftest-request.log"
# shellcheck disable=SC2086
# shellcheck disable=SC2086
"$OUT/difftest_request" $ROOT/tests/corpus/request/valid/* $ROOT/tests/corpus/request/malformed/* 2>&1 | tee -a "$ROOT/results/difftest-request.log"
