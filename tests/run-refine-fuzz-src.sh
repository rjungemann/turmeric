#!/usr/bin/env bash
# tests/run-refine-fuzz-src.sh -- source-level differential fuzz smoke for
# refinement types.
#
# Compiles and runs generated Turmeric programs twice -- once with static
# discharge suppressed (TUR_REFINE_NO_DISCHARGE=1, the reference leg) and once
# normally -- and asserts that discharge never turns a program the runtime
# contract caught into one that exits 0.  See the header of
# tests/refine-fuzz-src.py for the full property list, for why the reference
# leg needs a dedicated test seam since refinement types graduated in v0.33.0,
# and for the procedure that proves this harness can fail.
#
# This is the SMOKE size (fast enough for ctest).  For a real session, run the
# Python driver directly with a larger --n and a fresh --seed.
#
# Env:
#   TUR_BIN            compiler to test        (default ./build/tur)
#   REFINE_FUZZ_N      cases                   (default 60)
#   REFINE_FUZZ_SEED   seed                    (default 1)

set -uo pipefail
cd "$(dirname "$0")/.."

# The generated programs are throwaway; leak-checking the spawned binaries is
# not what this harness measures.  The compiler path stays leak-checked.
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}"

TUR="${TUR_BIN:-./build/tur}"
N="${REFINE_FUZZ_N:-60}"
SEED="${REFINE_FUZZ_SEED:-1}"

if ! command -v python3 >/dev/null 2>&1; then
  echo "SKIP refine-fuzz-src: python3 unavailable"
  exit 0
fi
if [ ! -x "$TUR" ]; then
  echo "SKIP refine-fuzz-src: no compiler at $TUR"
  exit 0
fi

rc=0

# Plumbing first: the classifier has to get the pinned regression fixtures
# right before any generated verdict means anything.
python3 tests/refine-fuzz-src.py --self-test --tur "$TUR" || rc=1

python3 tests/refine-fuzz-src.py --tur "$TUR" --n "$N" --seed "$SEED" || rc=1

if [ $rc -ne 0 ]; then
  echo "FAIL refine-fuzz-src"
else
  echo "PASS refine-fuzz-src"
fi
exit $rc
