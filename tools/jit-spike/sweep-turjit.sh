#!/usr/bin/env bash
# Full-corpus sweep of the REAL `tur jit` subcommand (J1, findings 18) --
# the product path, not the spike harness.  Seeds J3's parity harness.
#
# Differences from sweep-full.sh, which drives the spike:
#   - No emit-c step, no normalizer, no shim: `tur jit` runs the whole
#     pipeline in one process, exactly as a user would.
#   - Fallback is a first-class outcome, counted separately: a fixture whose
#     inline C c2mir rejects takes the plan's step-6 fallback to cc
#     (TUR-W0070 on stderr).  `fallback-pass` means the cc path then produced
#     the expected output; `fallback-env` means the fallback could not link in
#     this checkout (the -lturi SDK anchoring limitation recorded in findings
#     18 -- environmental, not a JIT defect).
#
#   TURJIT=path/to/tur bash tools/jit-spike/sweep-turjit.sh
#   OUT=/somewhere ... (default build-jit/sweep-turjit)
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

TURJIT="${TURJIT:-$ROOT/build-turjit/tur}"
OUT="${OUT:-$ROOT/build-jit/sweep-turjit}"

[ -x "$TURJIT" ] || { echo "no tur at $TURJIT (build with -DTUR_JIT=ON)"; exit 1; }
# Capture, don't pipe into grep -q: under pipefail the early close SIGPIPEs
# tur and the pipeline reads as failure even on a match.
# Probed with a NONEXISTENT input: a bare `tur jit` prints usage on every
# build (P0 moved the input scan ahead of everything), so only the "carries no
# JIT engine" answer discriminates.
probe=$("$TURJIT" jit /nonexistent-tur-jit-probe.tur 2>&1 || true)
case "$probe" in
  *"carries no JIT"*)
     echo "$TURJIT does not carry the JIT engine"; exit 1 ;;
esac

mkdir -p "$OUT/w"
: > "$OUT/results.tsv"

run_one() {
  local dir="$1" name
  name=$(basename "$dir")
  local W="$OUT/w"
  local stdin_file=/dev/null
  [ -f "$dir/input.stdin" ] && stdin_file="$dir/input.stdin"
  timeout 90 "$TURJIT" jit "$dir/input.tur" \
      < "$stdin_file" > "$W/$name.out" 2> "$W/$name.err"
  local rc=$?
  local fell_back=""
  grep -q 'TUR-W0070' "$W/$name.err" && fell_back=1
  if [ "$rc" -ge 128 ]; then
    printf '%s\tFAIL\tsignal-%d\n' "$name" "$((rc - 128))"
  elif diff -q "$W/$name.out" "$dir/expected.stdout" > /dev/null 2>&1; then
    if [ -n "$fell_back" ]; then
      printf '%s\tfallback-pass\t\n' "$name"
    else
      printf '%s\tPASS\t\n' "$name"
    fi
  # GNU ld says "cannot find -lturi"; ld64 says "library 'turi' not found".
  # Matching only the former filed all 31 httpd-* fixtures as fallback-fail on
  # macOS -- a purely environmental limit reading as 31 JIT defects.
  elif [ -n "$fell_back" ] \
       && grep -qE "cannot find -lturi|library '?turi'? not found" "$W/$name.err"; then
    printf '%s\tfallback-env\t\n' "$name"
  elif [ -n "$fell_back" ]; then
    printf '%s\tfallback-fail\t\n' "$name"
  else
    printf '%s\toutput-mismatch\t\n' "$name"
  fi
  rm -f "$W/$name.out"
}
export -f run_one
export OUT TURJIT

# Same eligibility filter as sweep-full.sh.
eligible=()
for dir in tests/fixtures/*/; do
  [ -f "$dir/input.tur" ] || continue
  [ -f "$dir/expected.stdout" ] || continue
  ls "$dir" | grep -qE '^(flags|args|requires\.|expected\.status|expected\.stderr)' && continue
  eligible+=("$dir")
done
echo "eligible: ${#eligible[@]}  binary: $TURJIT"

jobs=$(command -v nproc > /dev/null && nproc \
       || sysctl -n hw.ncpu 2> /dev/null \
       || echo 4)
printf '%s\n' "${eligible[@]}" \
  | xargs -P "$jobs" -I{} bash -c 'run_one "$@"' _ {} >> "$OUT/results.tsv"

echo "--- outcome ---"
cut -f2 "$OUT/results.tsv" | sort | uniq -c | sort -rn
total=$(wc -l < "$OUT/results.tsv")
jit_pass=$(awk -F'\t' '$2=="PASS"' "$OUT/results.tsv" | wc -l)
fb_pass=$(awk -F'\t' '$2=="fallback-pass"' "$OUT/results.tsv" | wc -l)
echo "--- jit-native PASS: $jit_pass / $total;  +fallback-pass: $((jit_pass + fb_pass)) ---"
