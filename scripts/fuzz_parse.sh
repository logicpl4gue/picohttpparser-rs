#!/usr/bin/env bash
#
# fuzz_parse.sh — build + smoke-run the deterministic differential fuzz
# driver (scripts/fuzz_parse.c) for the stateless parsers.
#
# Builds the C oracle (symbols renamed to c_*) and the Rust release cdylib,
# links both into scripts/fuzz_parse.c, and runs a bounded mutation campaign
# over tests/corpus/{request,response,headers}/**. Any mismatch writes a
# repro to $OUT/fuzz-parse-case-N.bin and (after 5) exits nonzero.
#
# Deterministic: SEED controls the whole run. Env overrides:
#   SEED   PRNG seed            (default 1)
#   ITERS  iterations           (default 50000)
#   ENTRY  request|response|headers|all (default all)
set -u
set -o pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REF="$ROOT/reference"
OUT="$ROOT/target/fuzzparse"
CC="${CC:-gcc}"

SEED="${SEED:-1}"
ITERS="${ITERS:-50000}"
ENTRY="${ENTRY:-all}"

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

echo "== link fuzz driver (against the cdylib: the shipped artifact)"
$CC -O2 -Wall -I"$REF" "$ROOT/scripts/fuzz_parse.c" "$OUT/oracle.o" \
  "$ROOT/target/release/picohttpparser_rs.dll.lib" -o "$OUT/fuzz_parse" || exit 1
cp "$ROOT/target/release/picohttpparser_rs.dll" "$OUT/"

echo "== smoke campaign: seed=$SEED iters=$ITERS entry=$ENTRY"
SEED_FILES=$(find "$ROOT/tests/corpus/request" "$ROOT/tests/corpus/response" \
  "$ROOT/tests/corpus/headers" -type f | sort)
# FUZZ_TIMEOUT_S bounds the run so a hang stalls CI loudly instead of
# forever (P1-1); timeout's 124 surfaces distinctly from mismatch exit 1.
FUZZ_TIMEOUT_S="${FUZZ_TIMEOUT_S:-1800}"
_T0=$(date +%s%N)
# shellcheck disable=SC2086
if command -v timeout >/dev/null 2>&1; then
  FUZZ_CASE_DIR="$OUT" timeout "$FUZZ_TIMEOUT_S" "$OUT/fuzz_parse" "$SEED" "$ITERS" "$ENTRY" $SEED_FILES
else
  FUZZ_CASE_DIR="$OUT" "$OUT/fuzz_parse" "$SEED" "$ITERS" "$ENTRY" $SEED_FILES
fi
RC=$?
_T1=$(date +%s%N)
echo "elapsed_ms=$(( (_T1 - _T0) / 1000000 ))"
exit $RC
