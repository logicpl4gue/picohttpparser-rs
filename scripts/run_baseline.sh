#!/usr/bin/env bash
#
# run_baseline.sh — Milestone 0 C baseline.
#
# 1. Snapshot environment + tool versions.
# 2. Verify the pinned reference (sha256sum -c reference/SHA256SUMS).
# 3. Build the upstream benchmark with ${CC:-cc} -O2 and time N trials
#    (first trial discarded as warmup); validate the timer on every trial and
#    record runs[] + mean/min. A single timed run is not a baseline.
# 4. Build the upstream test suite out-of-tree (needs CC + picotest sources;
#    generates a sys/mman.h shim under target/ when the libc lacks POSIX mmap,
#    runs via prove when present, else executes test-bin directly) and record
#    TAP totals.
# 5. Write results/baseline.json with REAL values only.
#
# Honesty contract: missing tools are recorded as "status": "skipped" with an
# explicit reason. A guessed number is never written. Exit 0 on honest skip,
# exit 1 on hard failure (e.g. tool present but build/run failed).
#
# Overrides: CC=..., CFLAGS=... (default CC=cc, CFLAGS=-O2).
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REF="$ROOT/reference"
OUT="$ROOT/results/baseline.json"
BENCH_DIR="$ROOT/target/c-baseline"
CC="${CC:-cc}"
CFLAGS="${CFLAGS:--O2}"

# --- helper: JSON string escaper (safe for our fixed, verified inputs) ------
jstr() { printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g'; }

# --- env snapshot -----------------------------------------------------------
GEN_UTC="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
OS="$(uname -s -r -m 2>/dev/null || echo unknown)"
GIT_V="$(git --version 2>/dev/null || echo absent)"
RUSTC_V="$(rustc --version 2>/dev/null || echo absent)"
CARGO_V="$(cargo --version 2>/dev/null || echo absent)"
CURL_V="$(curl --version 2>/dev/null | head -n1 || echo absent)"
if command -v "$CC" >/dev/null 2>&1; then
  CC_PRESENT="present"
  CC_PATH="$(command -v "$CC")"
  CC_V="$($CC --version 2>/dev/null | head -n1 || echo unknown)"
else
  CC_PRESENT="missing"
  CC_PATH=""
  CC_V=""
fi
# Machine identity (plan §7 Phase-0 step 2): a number without a machine is trivia.
CPU_MODEL="$(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | cut -d: -f2- | sed 's/^ *//;s/ *$//')"
# Fallback for native-Windows toolchains first in PATH (no MSYS /proc):
if [ -z "$CPU_MODEL" ] && command -v powershell.exe >/dev/null 2>&1; then
  CPU_MODEL="$(powershell.exe -NoProfile -Command "Get-CimInstance Win32_Processor | Select-Object -ExpandProperty Name" 2>/dev/null | tr -d '\r')"
fi
[ -n "$CPU_MODEL" ] || CPU_MODEL="unknown"
CPU_COUNT="$(nproc 2>/dev/null || echo "${NUMBER_OF_PROCESSORS:-unknown}")"
POWER_SCHEME="$(powercfg //getactivescheme 2>/dev/null | sed -n 's/.*(\(.*\)).*/\1/p')"
[ -n "$POWER_SCHEME" ] || POWER_SCHEME="unknown"
MAKE_PRESENT="$(command -v make >/dev/null 2>&1 && echo present || echo missing)"
PROVE_PRESENT="$(command -v prove >/dev/null 2>&1 && echo present || echo missing)"
PERL_PRESENT="$(command -v perl >/dev/null 2>&1 && echo present || echo missing)"
PICOTEST_PRESENT="present" && [ -f "$REF/picotest/picotest.c" ] || PICOTEST_PRESENT="missing"

# --- 2. pin verification ----------------------------------------------------
( cd "$REF" && sha256sum -c SHA256SUMS >/dev/null 2>&1 ) && HASH_VERIFY="passed" || HASH_VERIFY="failed"
# report detail
HASH_DETAIL="$( cd "$REF" && sha256sum -c SHA256SUMS 2>&1 | tr '\n' ';' )"

# --- 3. benchmark -----------------------------------------------------------
# Trials, not a single run: N timed launches, first discarded as warmup
# (cold pages, CRT/DLL load, Defender scan, turbo ramp all live there).
BENCH_N=7
BENCH_STATUS="skipped"; BENCH_REASON=""
BENCH_MEAN="null"; BENCH_MIN="null"; BENCH_NSPP="null"; BENCH_WARM="null"
BENCH_RUNS="[]"; BENCH_TRIALS=0; BENCH_ITERS="null"
BENCH_CORPUS_SHA=""; TIMER_GRAN="null"
TIMER_NAME="date +%s%N"
if [ "$CC_PRESENT" = "present" ]; then
  mkdir -p "$BENCH_DIR"
  BENCH_BIN="$BENCH_DIR/bench"
  if $CC $CFLAGS -o "$BENCH_BIN" "$REF/bench.c" "$REF/picohttpparser.c" >"$BENCH_DIR/build.log" 2>&1; then
    BENCH_CORPUS_SHA="$(sha256sum "$REF/bench.c" 2>/dev/null | cut -d' ' -f1)"
    # No separate granularity probe: back-to-back `date` spawns measure MSYS
    # fork/exec + scheduler noise (~10-30 ms), not clock resolution, so such a
    # probe would launder spawn latency as "granularity". The %N unit is
    # 100 ns and every trial is timer-validated (>=16 digits, END > START);
    # run-to-run spread (~0.1 s, recorded in benchRunsSeconds) dominates any
    # plausible clock quantization by 6+ orders of magnitude.
    BENCH_STATUS="passed"
    _sum="0"; _n=0; _min=""; _runs=""
    for _i in $(seq 1 "$BENCH_N"); do
      START=$(date +%s%N); "$BENCH_BIN"; RC=$?; END=$(date +%s%N)
      if [ "$RC" -ne 0 ]; then
        BENCH_STATUS="failed"; BENCH_REASON="bench binary exited ${RC} on trial ${_i}"; break
      fi
      if ! [[ "$START" =~ ^[0-9]{16,}$ && "$END" =~ ^[0-9]{16,}$ && "$END" -gt "$START" ]]; then
        BENCH_STATUS="failed"; BENCH_REASON="timer validation failed on trial ${_i} (START=$START END=$END)"; break
      fi
      _sec=$(awk "BEGIN{printf \"%.3f\", ($END-$START)/1000000000}")
      if [ "$_i" -eq 1 ]; then
        BENCH_WARM="$_sec"   # warmup: recorded, never averaged
      else
        _sum=$(awk "BEGIN{printf \"%.3f\", $_sum + $_sec}")
        _n=$((_n + 1))
        if [ -z "$_min" ] || [ "$(awk "BEGIN{print ($_sec < $_min)}")" -eq 1 ]; then _min="$_sec"; fi
        _runs="${_runs}${_runs:+,}$_sec"
      fi
    done
    if [ "$BENCH_STATUS" = "passed" ]; then
      BENCH_RUNS="[$_runs]"; BENCH_TRIALS="$_n"
      BENCH_MEAN=$(awk "BEGIN{printf \"%.3f\", $_sum / $_n}")
      BENCH_MIN="$_min"
      BENCH_NSPP=$(awk "BEGIN{printf \"%.1f\", ($BENCH_MEAN / 10000000) * 1000000000}")
      BENCH_ITERS=10000000
    fi
  else
    BENCH_STATUS="failed"; BENCH_REASON="cc build failed, see target/c-baseline/build.log"
  fi
else
  BENCH_REASON="no C compiler in PATH (CC=${CC}: cc/gcc/clang absent)"
fi

# --- 4. upstream test suite (out-of-tree; reference/ stays immutable) --------
TEST_STATUS="skipped"; TEST_REASON=""; TEST_TOTAL=""; TEST_PASSED=""; TEST_FAILED=""; TEST_SKIPPED=""
TEST_RUNNER=""; TEST_SAN=""
# Shim: only when the host libc lacks sys/mman.h (e.g. MinGW). Implements the
# exact mmap/mprotect/munmap/sysconf shapes test.c:main uses, backed by
# VirtualAlloc. Harness-only, generated under target/ (never in reference/).
SHIM_INC=""; SHIM_SRC=""
if [ "$CC_PRESENT" = "present" ]; then
  if ! echo '#include <sys/mman.h>' | $CC -E - >/dev/null 2>&1; then
    mkdir -p "$BENCH_DIR/shim/sys"
    cat > "$BENCH_DIR/shim/sys/mman.h" <<'SHIM_H'
#ifndef WIN_MMAN_SHIM_H
#define WIN_MMAN_SHIM_H
/* Harness-only sys/mman.h shim (see script section 4). */
#include <stddef.h>
#define PROT_NONE 0x0
#define PROT_READ 0x1
#define PROT_WRITE 0x2
#define MAP_PRIVATE 0x02
#define MAP_ANON 0x20
#define MAP_FAILED ((void *)-1)
#ifdef __cplusplus
extern "C" {
#endif
void *mmap(void *addr, size_t len, int prot, int flags, int fd, long off);
int munmap(void *addr, size_t len);
int mprotect(void *addr, size_t len, int prot);
/* MinGW also lacks sysconf(3); test.c needs only _SC_PAGESIZE. */
#define _SC_PAGESIZE 30
long sysconf(int name);
#ifdef __cplusplus
}
#endif
#endif
SHIM_H
    cat > "$BENCH_DIR/shim/mman_win.c" <<'SHIM_C'
/* VirtualAlloc-backed harness shim (see script section 4). */
#include <windows.h>
#include <sys/mman.h>
static DWORD prot_to_win(int prot)
{
    if (prot == PROT_NONE)
        return PAGE_NOACCESS;
    if (prot & PROT_WRITE)
        return PAGE_READWRITE;
    return PAGE_READONLY;
}
void *mmap(void *addr, size_t len, int prot, int flags, int fd, long off)
{
    void *p;
    (void)addr; (void)flags; (void)fd; (void)off;
    p = VirtualAlloc(NULL, len, MEM_RESERVE | MEM_COMMIT, prot_to_win(prot));
    return p != NULL ? p : MAP_FAILED;
}
int munmap(void *addr, size_t len)
{
    (void)len;
    return VirtualFree(addr, 0, MEM_RELEASE) ? 0 : -1;
}
int mprotect(void *addr, size_t len, int prot)
{
    DWORD old;
    return VirtualProtect(addr, len, prot_to_win(prot), &old) ? 0 : -1;
}
long sysconf(int name)
{
    SYSTEM_INFO si;
    (void)name;
    GetSystemInfo(&si);
    return (long)si.dwPageSize;
}
SHIM_C
    SHIM_INC="-I$BENCH_DIR/shim"; SHIM_SRC="$BENCH_DIR/shim/mman_win.c"
  fi
  # Sanitizers when the toolchain ships their runtimes, else plain -O2.
  if echo 'int main(void){return 0;}' | $CC -fsanitize=address,undefined -x c - -o "$BENCH_DIR/sanprobe" >/dev/null 2>&1; then
    SAN_FLAGS="-fsanitize=address,undefined"; TEST_SAN="asan+ubsan"
  else
    SAN_FLAGS=""; TEST_SAN="unavailable"
  fi
fi
# F2 gate: a failed pin check fails every consumer of reference/ outright.
PIN_ENFORCED=0
if [ "$HASH_VERIFY" != "passed" ]; then
  TEST_STATUS="failed"; TEST_REASON="pin verification failed; refusing to build or run reference code"
  RUST_STATUS="failed"; RUST_REASON="pin verification failed; refusing to build or run reference code"
  PIN_ENFORCED=1
fi
# F3 guard: hang regressions must fail loudly, never stall. `timeout` ships
# with MSYS coreutils; without it the binaries run unwrapped (same as before).
TIMEOUT_RUN=""
if command -v timeout >/dev/null 2>&1; then
  TIMEOUT_RUN="timeout 300"
fi
if [ "$PIN_ENFORCED" = "0" ] && [ "$CC_PRESENT" = "present" ] && [ "$PICOTEST_PRESENT" = "present" ]; then
  TEST_BIN="$BENCH_DIR/test-bin"
  # shellcheck disable=SC2086
  if $CC -Wall -O2 $SAN_FLAGS $SHIM_INC -o "$TEST_BIN" "$REF/picohttpparser.c" "$REF/picotest/picotest.c" "$REF/test.c" $SHIM_SRC >"$BENCH_DIR/test-build.log" 2>&1; then
    TEST_LOG="$ROOT/results/upstream-tests.log"
    if [ "$PROVE_PRESENT" = "present" ]; then
      TEST_RUNNER="prove"
      ( cd "$BENCH_DIR" && UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 $TIMEOUT_RUN prove ./test-bin >"$TEST_LOG" 2>&1 ); RC=$?
    else
      TEST_RUNNER="direct"
      ( cd "$BENCH_DIR" && UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 $TIMEOUT_RUN ./test-bin >"$TEST_LOG" 2>&1 ); RC=$?
    fi
    TEST_TOTAL=$(grep -cE '^(ok|not ok) [0-9]+ - ' "$TEST_LOG" 2>/dev/null || true)
    TEST_FAILED=$(grep -cE '^not ok' "$TEST_LOG" 2>/dev/null || true)
    TEST_TOTAL=${TEST_TOTAL:-0}; TEST_FAILED=${TEST_FAILED:-0}
    TEST_PASSED=$((TEST_TOTAL - TEST_FAILED))
    if [ "$RC" -eq 0 ] && [ "$TEST_FAILED" -eq 0 ]; then
      TEST_STATUS="passed"
    else
      TEST_STATUS="failed"; TEST_REASON="test binary exited ${RC} with ${TEST_FAILED} failures, see results/upstream-tests.log"
    fi
  else
    TEST_STATUS="failed"; TEST_REASON="test-bin build failed, see target/c-baseline/test-build.log"
  fi
elif [ "$PIN_ENFORCED" = "0" ]; then
  MISSING=""
  [ "$CC_PRESENT" = "present" ] || MISSING="$MISSING cc"
  [ "$PICOTEST_PRESENT" = "present" ] || MISSING="$MISSING picotest"
  TEST_REASON="missing:${MISSING}"
fi
# (PIN_ENFORCED=1 keeps the failed status set above.)

# --- 4b. upstream suite vs Rust cdylib (the compatibility gate) ------------
# Compiles the UNMODIFIED upstream test.c + picotest against the Rust release
# cdylib (NOT the C oracle) and runs it: this is the literal "100%
# applicable upstream tests" gate. Guard-page mmap input makes it an
# overread test too.
RUST_STATUS="skipped"; RUST_REASON=""; RUST_TOTAL=""; RUST_PASSED=""; RUST_FAILED=""
RUST_RUNNER=""
CARGO_PRESENT="$(command -v cargo >/dev/null 2>&1 && echo present || echo missing)"
if [ "$PIN_ENFORCED" = "0" ] && [ "$CC_PRESENT" = "present" ] && [ "$PICOTEST_PRESENT" = "present" ] && [ "$CARGO_PRESENT" = "present" ]; then
  if ( cd "$ROOT" && cargo build --release >"$BENCH_DIR/cargo-build.log" 2>&1 ); then
    RUST_BIN="$BENCH_DIR/test-rust"
    # F1 hardening: delete before copy so a locked/stale DLL can never pass
    # silently — a failed copy fails the gate instead of testing old code.
    rm -f "$BENCH_DIR/picohttpparser_rs.dll"
    if ! cp "$ROOT/target/release/picohttpparser_rs.dll" "$BENCH_DIR/" 2>"$BENCH_DIR/dll-copy.log"; then
      RUST_STATUS="failed"; RUST_REASON="could not stage fresh cdylib, see target/c-baseline/dll-copy.log"
    # shellcheck disable=SC2086
    elif $CC -Wall -O2 $SHIM_INC -I"$REF" -o "$RUST_BIN" "$REF/picotest/picotest.c" "$REF/test.c" $SHIM_SRC "$ROOT/target/release/picohttpparser_rs.dll.lib" >"$BENCH_DIR/test-rust-build.log" 2>&1; then
      RUST_LOG="$ROOT/results/upstream-rust.log"
      if [ "$PROVE_PRESENT" = "present" ]; then
        RUST_RUNNER="prove"
        ( cd "$BENCH_DIR" && $TIMEOUT_RUN prove ./test-rust >"$RUST_LOG" 2>&1 ); RC=$?
      else
        RUST_RUNNER="direct"
        ( cd "$BENCH_DIR" && $TIMEOUT_RUN ./test-rust >"$RUST_LOG" 2>&1 ); RC=$?
      fi
      RUST_TOTAL=$(grep -cE '^(ok|not ok) [0-9]+ - ' "$RUST_LOG" 2>/dev/null || true)
      RUST_FAILED=$(grep -cE '^not ok' "$RUST_LOG" 2>/dev/null || true)
      RUST_TOTAL=${RUST_TOTAL:-0}; RUST_FAILED=${RUST_FAILED:-0}
      RUST_PASSED=$((RUST_TOTAL - RUST_FAILED))
      if [ "$RC" -eq 0 ] && [ "$RUST_FAILED" -eq 0 ]; then
        RUST_STATUS="passed"
      else
        RUST_STATUS="failed"; RUST_REASON="test-rust exited ${RC} with ${RUST_FAILED} failures, see results/upstream-rust.log"
      fi
    else
      RUST_STATUS="failed"; RUST_REASON="test-rust build failed, see target/c-baseline/test-rust-build.log"
    fi
  else
    RUST_STATUS="failed"; RUST_REASON="cargo build --release failed, see target/c-baseline/cargo-build.log"
  fi
elif [ "$PIN_ENFORCED" = "0" ]; then
  MISSING=""
  [ "$CC_PRESENT" = "present" ] || MISSING="$MISSING cc"
  [ "$PICOTEST_PRESENT" = "present" ] || MISSING="$MISSING picotest"
  [ "$CARGO_PRESENT" = "present" ] || MISSING="$MISSING cargo"
  RUST_REASON="missing:${MISSING}"
fi
# (PIN_ENFORCED=1 keeps the failed status set above.)

# --- 5. write results/baseline.json ----------------------------------------
mkdir -p "$ROOT/results"
if ! cat > "$OUT" <<EOF
{
  "schemaVersion": 4,
  "generatedUtc": "$(jstr "$GEN_UTC")",
  "generatedBy": "scripts/run_baseline.sh",
  "pin": {
    "repository": "h2o/picohttpparser",
    "commit": "f4d94b48b31e0abae029ebeafcfd9ca0680ede58",
    "short": "f4d94b48",
    "filesPinned": 8,
    "hashVerify": "$HASH_VERIFY",
    "hashDetail": "$(jstr "$HASH_DETAIL")"
  },
  "environment": {
    "os_uname": "$(jstr "$OS")",
    "git": "$(jstr "$GIT_V")",
    "rustc": "$(jstr "$RUSTC_V")",
    "cargo": "$(jstr "$CARGO_V")",
    "curl": "$(jstr "$CURL_V")",
    "cc": "$CC_PRESENT",
    "ccPath": "$(jstr "$CC_PATH")",
    "ccVersion": "$(jstr "$CC_V")",
    "cpu": "$(jstr "$CPU_MODEL")",
    "cpuCount": "$(jstr "$CPU_COUNT")",
    "powerScheme": "$(jstr "$POWER_SCHEME")",
    "make": "$MAKE_PRESENT",
    "prove": "$PROVE_PRESENT",
    "perl": "$PERL_PRESENT",
    "picotestSubmodule": "$PICOTEST_PRESENT"
  },
  "cBaseline": {
    "status": "$BENCH_STATUS",
    "reason": "$(jstr "$BENCH_REASON")",
    "buildCommand": "$(jstr "$CC $CFLAGS -o $BENCH_DIR/bench $REF/bench.c $REF/picohttpparser.c")",
    "benchTrials": $BENCH_TRIALS,
    "benchWarmupDiscarded": 1,
    "benchIterations": $BENCH_ITERS,
    "benchSecondsMean": $BENCH_MEAN,
    "benchSecondsMin": $BENCH_MIN,
    "benchNsPerParseMean": $BENCH_NSPP,
    "benchRunsSeconds": $BENCH_RUNS,
    "benchWarmupSeconds": $BENCH_WARM,
    "benchCorpusFile": "reference/bench.c",
    "benchCorpusSha256": "$(jstr "$BENCH_CORPUS_SHA")",
    "benchScope": "request-parse happy path only: one fixed ~620B GET x10M (upstream marker, unmodified)",
    "timer": "$(jstr "$TIMER_NAME")",
    "timerGranularityNs": null,
    "timerNote": "MSYS date; %N unit is 100ns, effective resolution unverified but immaterial: trial spread (~0.1s) dominates"
  },
  "upstreamTestSuite": {
    "status": "$TEST_STATUS",
    "reason": "$(jstr "$TEST_REASON")",
    "runner": "$(jstr "$TEST_RUNNER")",
    "sanitizers": "$(jstr "$TEST_SAN")",
    "total": ${TEST_TOTAL:-null},
    "passed": ${TEST_PASSED:-null},
    "failed": ${TEST_FAILED:-null},
    "skipped": "$(jstr "$TEST_SKIPPED")",
    "log": "results/upstream-tests.log (created only when the suite runs)"
  },
  "upstreamVsRust": {
    "status": "$RUST_STATUS",
    "reason": "$(jstr "$RUST_REASON")",
    "runner": "$(jstr "$RUST_RUNNER")",
    "total": ${RUST_TOTAL:-null},
    "passed": ${RUST_PASSED:-null},
    "failed": ${RUST_FAILED:-null},
    "log": "results/upstream-rust.log (unmodified upstream test.c linked against the Rust cdylib)"
  },
  "honestyNote": "Every value above is a real measurement or an explicit skipped-with-reason. No fabricated numbers."
}
EOF
then
  echo "ERROR: could not write $OUT" >&2
  exit 1
fi

echo "Baseline written to $OUT"
echo "  hashVerify: $HASH_VERIFY"
echo "  cBaseline:  $BENCH_STATUS ${BENCH_REASON:+($BENCH_REASON)}${BENCH_MEAN:+mean ${BENCH_MEAN}s over ${BENCH_TRIALS} trials}"
echo "  testSuite:  $TEST_STATUS ${TEST_REASON:+($TEST_REASON)}"
echo "  vsRust:     $RUST_STATUS ${RUST_REASON:+($RUST_REASON)}"
[ "$BENCH_STATUS" = "failed" ] || [ "$TEST_STATUS" = "failed" ] || [ "$RUST_STATUS" = "failed" ] && exit 1
exit 0