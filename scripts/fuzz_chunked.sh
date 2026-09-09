#!/usr/bin/env bash
#
# fuzz_chunked.sh — deterministic differential fuzz, chunked decoder.
#
# Builds the C oracle (symbols renamed to c_*), links scripts/fuzz_chunked.c
# against the Rust release cdylib (DLL staged beside the binary), and runs a
# bounded campaign over the chunked corpus as the seed pool.
#
# Deterministic: SEED (default 12345) + ITERS (default 20000) env-overridable.
# The driver prints SEED/ITERS at startup; a given seed reproduces exactly.
#
# Mismatches: printed inline with the operation trace; the full body of each
# failing iteration is dumped to target/fuzz-chunked-case-N.bin (N=1..5, then
# the driver stops). Exit 1 iff any mismatch was found, else 0.
set -u
set -o pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REF="$ROOT/reference"
OUT="$ROOT/target/fuzz-chunked"
CC="${CC:-gcc}"
SEED="${SEED:-12345}"
ITERS="${ITERS:-20000}"

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
# MinGW ld cannot satisfy the staticlib's MSVC EH residue; the cdylib import
# lib is the proven link path (see diff_chunked.sh). DLL sits beside the exe.
$CC -O2 -Wall -I"$REF" "$ROOT/scripts/fuzz_chunked.c" "$OUT/oracle.o" \
  "$ROOT/target/release/picohttpparser_rs.dll.lib" -o "$OUT/fuzz_chunked" || exit 1
cp "$ROOT/target/release/picohttpparser_rs.dll" "$OUT/"

echo "== fuzz campaign: SEED=$SEED ITERS=$ITERS over tests/corpus/chunked/{valid,malformed}"
echo "   (case dumps land in target/fuzz-chunked-case-N.bin; exe+log in $OUT)"
cd "$ROOT" || exit 1
"$OUT/fuzz_chunked" "$SEED" "$ITERS" \
  "$ROOT"/tests/corpus/chunked/valid/* "$ROOT"/tests/corpus/chunked/malformed/*
RC=$?
echo "exit=$RC"
exit $RC
