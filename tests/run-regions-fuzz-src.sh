#!/usr/bin/env bash
# tests/run-regions-fuzz-src.sh -- source-level differential fuzz smoke for
# region brackets (RM3).
#
# Generates programs that build nodes inside `with-region` / `bt-scope`, let
# them escape (or not) through every route the lock knows about -- the result
# type, a store, an erasure, a stored closure, a nested bracket -- and reads
# every value back after the pop.  Each program runs on the default arm
# (compiled with -fsanitize=address so the arena poison is live) and under
# TUR_REGIONS=0; both must print the predicted stdout, and the default arm's
# TUR_REGION_STATS counts must match the generator's rewind / retire model.
# See the header of tests/regions-fuzz-src.py for the two properties and the
# sabotages that prove the harness can fail.
#
# This is the SMOKE size (fast enough for ctest).  For a real session, run the
# Python driver directly with a larger --n and a fresh --seed.
#
# Env:
#   TUR_BIN            compiler to test        (default ./build/tur)
#   REGIONS_FUZZ_N     programs                (default 12)
#   REGIONS_FUZZ_SEED  seed                    (default 1)

set -uo pipefail
cd "$(dirname "$0")/.."

export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}"

TUR="${TUR_BIN:-./build/tur}"
N="${REGIONS_FUZZ_N:-12}"
SEED="${REGIONS_FUZZ_SEED:-1}"

if ! command -v python3 >/dev/null 2>&1; then
  echo "SKIP regions-fuzz-src: python3 unavailable"
  echo "TUR_SKIP: python3 unavailable"
  exit 0
fi
if [ ! -x "$TUR" ]; then
  echo "SKIP regions-fuzz-src: no compiler at $TUR"
  echo "TUR_SKIP: no compiler at $TUR"
  exit 0
fi

rc=0
python3 tests/regions-fuzz-src.py --self-test --tur "$TUR" || rc=1
python3 tests/regions-fuzz-src.py --tur "$TUR" --n "$N" --seed "$SEED" || rc=1

if [ $rc -ne 0 ]; then
  echo "FAIL regions-fuzz-src"
else
  echo "PASS regions-fuzz-src"
fi
exit $rc
