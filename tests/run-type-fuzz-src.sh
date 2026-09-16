#!/usr/bin/env bash
# tests/run-type-fuzz-src.sh -- typed-boundary fuzz smoke.
#
# Generates correct-by-construction programs that route known values through
# random wrappers (structs, ADTs, Option/Result, Vec, closures) and random
# boundary crossings, and asserts: `tur check` accepting a program implies the
# C compiles, links, runs, and prints exactly the predicted output.  See the
# header of tests/type-fuzz-src.py for the property list, the known-open-report
# avoid table, and the procedure that proves the harness can fail.
#
# This is the SMOKE size (fast enough for ctest).  For a real session, run the
# Python driver directly with a larger --n and a fresh --seed.
#
# Since the seam axis (2026-09-16) a share of the cases route their payload
# through a RUNTIME SEAM instead -- a session/router channel, a generator frame,
# a future slot, a fiber slot, a tagged `any` box, a transactional cell -- i.e.
# a place the value is parked by one piece of emitted code and read back by
# another.  Those are the boundaries the crossing pool above structurally cannot
# reach, and where a `double` gets cast into an int64 slot and truncated.
#
# Env:
#   TUR_BIN               compiler to test        (default ./build/tur)
#   TYPE_FUZZ_N           cases                   (default 40)
#   TYPE_FUZZ_SEED        seed                    (default 1)
#   TYPE_FUZZ_SEAM_FRAC   share of seam cases     (default: the script's 0.30)
#
# Reading the axis by hand:
#   python3 tests/type-fuzz-src.py --seam-matrix     # seam x payload table
#   python3 tests/type-fuzz-src.py --seam session    # one seam, many shapes

set -uo pipefail
cd "$(dirname "$0")/.."

# The generated programs are throwaway; leak-checking the spawned binaries is
# not what this harness measures.  The compiler path stays leak-checked.
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}"

TUR="${TUR_BIN:-./build/tur}"
N="${TYPE_FUZZ_N:-40}"
SEED="${TYPE_FUZZ_SEED:-1}"
SEAM_FRAC="${TYPE_FUZZ_SEAM_FRAC:-}"

SEAM_ARGS=()
if [ -n "$SEAM_FRAC" ]; then
  SEAM_ARGS=(--seam-frac "$SEAM_FRAC")
fi

if ! command -v python3 >/dev/null 2>&1; then
  echo "SKIP type-fuzz-src: python3 unavailable"
  echo "TUR_SKIP: python3 unavailable"
  exit 0
fi
if [ ! -x "$TUR" ]; then
  echo "SKIP type-fuzz-src: no compiler at $TUR"
  echo "TUR_SKIP: no compiler at $TUR"
  exit 0
fi

rc=0

# Plumbing first: the classifier has to see a pass, a wrong-output, a crash,
# and a reject before any generated verdict means anything.
python3 tests/type-fuzz-src.py --self-test --tur "$TUR" || rc=1

python3 tests/type-fuzz-src.py --tur "$TUR" --n "$N" --seed "$SEED" \
        "${SEAM_ARGS[@]+"${SEAM_ARGS[@]}"}" || rc=1

# type-confusion-detection-plan F2: replay any seeds a nightly fuzz run
# recorded for this harness.  The smoke run above is pinned to seed 1 and
# therefore searches nothing; the corpus is what carries forward the seeds
# that actually found something.  Empty corpus = a no-op that says so.
bash tests/replay-fuzz-seeds.sh type tests/type-fuzz-src.py "$TUR" "$N" || rc=1

if [ $rc -ne 0 ]; then
  echo "FAIL type-fuzz-src"
else
  echo "PASS type-fuzz-src"
fi
exit $rc
