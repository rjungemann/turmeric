#!/usr/bin/env bash
# tests/run-hamt-eq-ctx.sh -- prereq 2a gate: context-carrying HAMT comparator.
#
# Compiles the HAMT runtime + tests/test_hamt_eq_ctx.c with AddressSanitizer +
# UndefinedBehaviorSanitizer (and LeakSanitizer on Linux) and runs it.  The
# acceptance is that the tur_hamt_*_eq_ctx family threads `ctx` to every compare,
# does content-keyed get/has/set/del, nests correctly against the plain _eq
# family, and is leak/double-free/UAF clean.
#
# Exit status: 0 if the test binary exits 0 with no sanitizer report, else 1.

set -uo pipefail
cd "$(dirname "$0")/.."

CC="${CC:-cc}"
SRC_RUNTIME="src/runtime"
TEST_SRC="tests/test_hamt_eq_ctx.c"
BIN="$(mktemp -t tur-eqctx-XXXXXX)"

# region-lock-hardening: hamt.c notes stored words through region.c, so the
# standalone compile links it (and arena.c under it) exactly as every library
# that carries hamt.c does.
if ! "$CC" -std=c11 -fsanitize=address,undefined -I "$SRC_RUNTIME" \
        "$TEST_SRC" "$SRC_RUNTIME/hamt.c" "$SRC_RUNTIME/region.c" "$SRC_RUNTIME/arena.c" -pthread -o "$BIN" 2>/tmp/eqctx-cc.log; then
    echo "FAIL hamt-eq-ctx -- compile failed"
    cat /tmp/eqctx-cc.log
    rm -f "$BIN"
    exit 1
fi

run_out=$(ASAN_OPTIONS="detect_leaks=1:exitcode=23" \
          UBSAN_OPTIONS="halt_on_error=1:exitcode=24" "$BIN" 2>&1)
rc=$?

if grep -q "detect_leaks is not supported" <<< "$run_out"; then
    run_out=$(ASAN_OPTIONS="exitcode=23" UBSAN_OPTIONS="halt_on_error=1:exitcode=24" \
              "$BIN" 2>&1)
    rc=$?
    echo "(note: LeakSanitizer unsupported here; ran ASan/UBSan only)"
fi

echo "$run_out"
rm -f "$BIN"

if [ "$rc" -ne 0 ]; then
    echo "FAIL hamt-eq-ctx -- sanitizer reported an issue (rc=$rc)"
    exit 1
fi
if ! grep -q "all eq-ctx tests passed" <<< "$run_out"; then
    echo "FAIL hamt-eq-ctx -- test did not complete"
    exit 1
fi
echo "PASS hamt-eq-ctx"
exit 0
