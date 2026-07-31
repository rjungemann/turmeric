#!/usr/bin/env bash
# Phase J0 spike driver -- docs/upcoming/jit-engine-plan.md
#
# For each fixture: `tur emit-c` -> C11-subset normalize -> in-process
# c2mir + MIR-gen -> run -> diff against the fixture's COMPILED expectation
# (expected.stdout, per plan section 1.4: the JIT is a compiled target).
#
#   cmake -S . -B build-jit -DCMAKE_BUILD_TYPE=Release -DTUR_JIT_SPIKE=ON
#   cmake --build build-jit -j --target tur-jit-spike
#   bash tools/jit-spike/run-spike.sh                  # J0 exit-criteria set
#   bash tools/jit-spike/run-spike.sh vec-basic map-lisp-basic   # or any set
#
# Env overrides: TUR (compiler), SPIKE (harness), OUT (work dir), REPEAT, OPT.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

TUR="${TUR:-$ROOT/build/tur}"
SPIKE="${SPIKE:-$ROOT/build-jit/tools/jit-spike/tur-jit-spike}"
OUT="${OUT:-$ROOT/build-jit/spike-work}"
REPEAT="${REPEAT:-5}"
OPT="${OPT:-2}"
NORMALIZE="$ROOT/tools/jit-spike/normalize-c11-subset.py"
# Prepended to every TU.  Papers over three c2mir subset gaps the normalizer
# does not handle; read the header, it documents each one as an open finding.
SHIM="${SHIM:-$ROOT/tools/jit-spike/subset-shim.h}"

# The J0 exit-criteria set from the plan: a hello-grade program, a HAMT-using
# program, and an effects/CPS program.  `tests/fixtures/hello` carries only an
# expected.stdout with no input, so `arith` stands in as the hello-grade case.
DEFAULT_FIXTURES=(arith hamt-basic cps-backend-effect)

[ -x "$TUR" ]   || { echo "no tur at $TUR (cmake --build build)"; exit 1; }
[ -x "$SPIKE" ] || { echo "no spike harness at $SPIKE (-DTUR_JIT_SPIKE=ON)"; exit 1; }

mkdir -p "$OUT"
fixtures=("$@")
[ ${#fixtures[@]} -eq 0 ] && fixtures=("${DEFAULT_FIXTURES[@]}")

pass=0 fail=0
for name in "${fixtures[@]}"; do
  dir="tests/fixtures/$name"
  input="$dir/input.tur"
  [ -f "$input" ] || input="$dir/$name.tur"
  if [ ! -f "$input" ]; then
    echo "SKIP $name -- no input"
    continue
  fi

  if ! "$TUR" emit-c "$input" > "$OUT/$name.c" 2> "$OUT/$name.emit.err"; then
    echo "FAIL $name -- emit-c"
    head -5 "$OUT/$name.emit.err"
    fail=$((fail + 1))
    continue
  fi

  python3 "$NORMALIZE" -I src/runtime "$OUT/$name.c" -o "$OUT/$name.subset.c" --report \
    2> "$OUT/$name.norm.err"
  norm_rc=$?

  "$SPIKE" -I src -I src/runtime -O "$OPT" --repeat "$REPEAT" --shim "$SHIM" \
    "$OUT/$name.subset.c" > "$OUT/$name.stdout" 2> "$OUT/$name.spike.err"
  rc=$?

  if [ -f "$dir/expected.stdout" ] && \
     diff -q "$OUT/$name.stdout" "$dir/expected.stdout" > /dev/null 2>&1; then
    echo "PASS $name"
    grep -E '^  (c2mir|link\+gen|execute)' "$OUT/$name.spike.err" | sed 's/^/     /'
    pass=$((pass + 1))
  else
    echo "FAIL $name -- exit=$rc normalize_rc=$norm_rc"
    echo "  --- got";  head -20 "$OUT/$name.stdout"  | sed 's/^/  /'
    echo "  --- want"; head -20 "$dir/expected.stdout" | sed 's/^/  /'
    echo "  --- stderr"; head -20 "$OUT/$name.spike.err" | sed 's/^/  /'
    fail=$((fail + 1))
  fi
  cat "$OUT/$name.norm.err" | sed 's/^/     norm: /'
done

echo "summary: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
