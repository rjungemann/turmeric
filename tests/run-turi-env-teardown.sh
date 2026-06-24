#!/usr/bin/env bash
# tests/run-turi-env-teardown.sh -- env-teardown leak gate.
#
# turi-env-owned-value-arena-pool-plan: runs the tur_env_teardown harness with
# LeakSanitizer FORCED ON.  The harness loops turi_env_new / eval (closures,
# structs, ADTs, error values) / turi_env_free; a clean Phase-1 value pool means
# each turi_env_free reclaims every escaping payload, so a full create/eval/free
# cycle must leak nothing.  This is the inverse of the interpreter's historical
# detect_leaks=0 default (see docs/asan-debug-leaks-plan.md) and the regression
# guard for the value-arena pool.
#
# Usage:
#   bash tests/run-turi-env-teardown.sh
#
# Environment:
#   TUR_ENV_TEARDOWN  path to the harness binary (default: ./build/tur_env_teardown)

set -u
cd "$(dirname "$0")/.."

BIN="${TUR_ENV_TEARDOWN:-./build/tur_env_teardown}"
if [ ! -x "$BIN" ]; then
    echo "run-turi-env-teardown: $BIN not built; run" >&2
    echo "  cmake --build build --target tur_env_teardown" >&2
    exit 2
fi

# Force leak detection on regardless of the caller's ASAN_OPTIONS.
if ASAN_OPTIONS=detect_leaks=1 "$BIN"; then
    echo "run-turi-env-teardown: PASS (no leaks across env teardown)"
    exit 0
else
    echo "run-turi-env-teardown: FAIL (leaks detected or harness error)" >&2
    exit 1
fi
