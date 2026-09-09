#!/usr/bin/env bash
#
# integ_http11.sh — M8 integration proof runner (repo M8 = plan M7).
#
# Builds the SAME unmodified consumer source (scripts/integ_http11.c) twice
# — once against reference/picohttpparser.c (oracle), once against the Rust
# release cdylib — runs a real HTTP/1.x exchange over loopback TCP under
# each, and requires byte-identical transcripts. Then drives all five entry
# points from Python via ctypes as a second, independent consumer.
#
# Writes results/integration.log (both transcripts + verdicts + versions).
# Exit nonzero on any build/run/diff failure.
set -u
set -o pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REF="$ROOT/reference"
OUT="$ROOT/target/integ"
CC="${CC:-gcc}"

# Per-run isolation (PID-namespaced): concurrent invocations must not share
# transcript files, the staged DLL, or the evidence log inputs.
RUN="$OUT/run-$$"
mkdir -p "$RUN"

echo "== cargo build --release (Rust cdylib under test)"
(cd "$ROOT" && cargo build --release) || exit 1

echo "== build consumer vs C oracle (no source changes, different link)"
$CC -O2 -Wall -I"$REF" "$ROOT/scripts/integ_http11.c" "$REF/picohttpparser.c" \
  -lws2_32 -o "$RUN/integ_oracle.exe" || exit 1

echo "== build SAME consumer vs Rust cdylib (no source changes, different link)"
$CC -O2 -Wall -I"$REF" "$ROOT/scripts/integ_http11.c" \
  "$ROOT/target/release/picohttpparser_rs.dll.lib" -lws2_32 -o "$RUN/integ_rust.exe" || exit 1
# Checked copy: a locked/stale DLL must fail loudly, never test old code.
cp "$ROOT/target/release/picohttpparser_rs.dll" "$RUN/" || { echo "DLL stage FAILED"; exit 1; }

echo "== run exchange under both builds"
(cd "$RUN" && ./integ_oracle.exe > oracle.txt 2>&1); RC1=$?
(cd "$RUN" && ./integ_rust.exe > rust.txt 2>&1); RC2=$?
echo "oracle rc=$RC1, rust rc=$RC2"
[ "$RC1" -eq 0 ] || { echo "oracle consumer FAILED"; exit 1; }
[ "$RC2" -eq 0 ] || { echo "rust consumer FAILED"; exit 1; }
if cmp -s "$RUN/oracle.txt" "$RUN/rust.txt"; then
  echo "transcripts: BYTE-IDENTICAL"
else
  echo "transcripts DIFFER:"
  diff "$RUN/oracle.txt" "$RUN/rust.txt" | head -20
  exit 1
fi

echo "== second consumer: Python ctypes (all five entry points)"
python3 "$ROOT/scripts/integ_ctypes.py" > "$RUN/ctypes.txt" 2>&1; RC3=$?
cat "$RUN/ctypes.txt"
[ "$RC3" -eq 0 ] || { echo "ctypes consumer FAILED"; exit 1; }

{
  echo "run_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "rev=$(git rev-parse --short HEAD 2>/dev/null || echo unknown)"
  echo "dirty_files=$(git status --porcelain 2>/dev/null | grep -v '^?? target/' | wc -l | tr -d ' ')"
  echo "dll_sha256=$(sha256sum "$ROOT/target/release/picohttpparser_rs.dll" 2>/dev/null | cut -d' ' -f1)"
  echo "oracle_exe_sha256=$(sha256sum "$RUN/integ_oracle.exe" 2>/dev/null | cut -d' ' -f1)"
  echo "rust_exe_sha256=$(sha256sum "$RUN/integ_rust.exe" 2>/dev/null | cut -d' ' -f1)"
  echo "cc=$($CC --version 2>/dev/null | head -n1)"
  echo "rustc=$(rustc --version 2>/dev/null || echo unknown)"
  echo "python=$(python3 --version 2>&1)"
  echo "--- oracle transcript ---"
  cat "$RUN/oracle.txt"
  echo "--- rust transcript: byte-identical (diffed above) ---"
  echo "--- ctypes transcript ---"
  cat "$RUN/ctypes.txt"
  echo "INTEGRATION verdict=PASS (C consumer relink + ctypes consumer)"
} > "$RUN/integration.log"
# Atomic publish: concurrent runs never interleave the evidence file.
mv "$RUN/integration.log" "$ROOT/results/integration.log"
echo "wrote results/integration.log"
