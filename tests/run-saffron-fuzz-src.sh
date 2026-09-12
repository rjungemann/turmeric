#!/usr/bin/env bash
# tests/run-saffron-fuzz-src.sh -- Saffron dynamic-surface fuzz smoke.
#
# Generates correct-by-construction `#lang saffron` programs that route known
# scalars through random `any` plumbing (wrappers, routes, terminal dynamic
# operations) and asserts a three-way property: `tur check` accepting a
# program implies the C compiles, links, runs and prints the predicted
# output, AND `tur --interpret` prints the same.  See the header of
# tests/saffron-fuzz-src.py for the property list, the known-open-finding
# avoid table, and the procedure that proves the harness can fail.
#
# This is the SMOKE size (fast enough for ctest).  For a real session, run
# the Python driver directly with a larger --n and a fresh --seed.
#
# Env:
#   TUR_BIN             compiler to test        (default ./build/tur)
#   SAFFRON_FUZZ_N      cases                   (default 40)
#   SAFFRON_FUZZ_SEED   seed                    (default 1)

set -uo pipefail
cd "$(dirname "$0")/.."

# The generated programs are throwaway; leak-checking the spawned binaries is
# not what this harness measures.  The compiler path stays leak-checked.
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}"

TUR="${TUR_BIN:-./build/tur}"
N="${SAFFRON_FUZZ_N:-40}"
SEED="${SAFFRON_FUZZ_SEED:-1}"

if ! command -v python3 >/dev/null 2>&1; then
  echo "SKIP saffron-fuzz-src: python3 unavailable"
  echo "TUR_SKIP: python3 unavailable"
  exit 0
fi
if [ ! -x "$TUR" ]; then
  echo "SKIP saffron-fuzz-src: no compiler at $TUR"
  echo "TUR_SKIP: no compiler at $TUR"
  exit 0
fi

rc=0

# Plumbing first: the classifier has to see a pass, a wrong-output, a crash,
# a reject, and both interpreter-arm verdicts before any generated verdict
# means anything.
python3 tests/saffron-fuzz-src.py --self-test --tur "$TUR" || rc=1

python3 tests/saffron-fuzz-src.py --tur "$TUR" --n "$N" --seed "$SEED" || rc=1

# type-confusion-detection-plan F2: replay any seeds a nightly fuzz run
# recorded for this harness.  The smoke run above is pinned to seed 1 and
# therefore searches nothing; the corpus is what carries forward the seeds
# that actually found something.  Empty corpus = a no-op that says so.
bash tests/replay-fuzz-seeds.sh saffron tests/saffron-fuzz-src.py "$TUR" "$N" || rc=1

if [ $rc -ne 0 ]; then
  echo "FAIL saffron-fuzz-src"
else
  echo "PASS saffron-fuzz-src"
fi
exit $rc
