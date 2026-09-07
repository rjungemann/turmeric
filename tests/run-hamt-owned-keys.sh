#!/usr/bin/env bash
# tests/run-hamt-owned-keys.sh -- WKC2 boxed-key ownership gate.
#
# Compiles the HAMT runtime + tests/test_hamt_owned_keys.c with
# AddressSanitizer + UndefinedBehaviorSanitizer (and LeakSanitizer on Linux)
# and runs it.  The acceptance for WKC2 (float32-map-key-carrier-plan) is that
# a wide-keyed map build+drop is leak-clean and double-free / use-after-free
# clean: every boxed key is freed exactly once even across structural sharing.
#
# Exit status: 0 if the test binary exits 0 with no sanitizer report, else 1.

set -uo pipefail
cd "$(dirname "$0")/.."

CC="${CC:-cc}"
SRC_RUNTIME="src/runtime"
TEST_SRC="tests/test_hamt_owned_keys.c"
BIN="$(mktemp -t tur-wkc2-XXXXXX)"

# LeakSanitizer is Linux-only; on platforms without it ASan still catches
# double-free / use-after-free, so we keep going but skip the leak assertion.
ASAN_OPTIONS="detect_leaks=1:exitcode=23"
probe_bin="$(mktemp -t tur-wkc2-probe-XXXXXX)"
# region-lock-hardening: hamt.c notes stored words through region.c, so the
# standalone compile links it (and arena.c under it) exactly as every library
# that carries hamt.c does.
if ! "$CC" -std=c11 -fsanitize=address,undefined -I "$SRC_RUNTIME" \
        "$TEST_SRC" "$SRC_RUNTIME/hamt.c" "$SRC_RUNTIME/region.c" "$SRC_RUNTIME/arena.c" -pthread -o "$BIN" 2>/tmp/wkc2-cc.log; then
    echo "FAIL hamt-owned-keys -- compile failed"
    cat /tmp/wkc2-cc.log
    rm -f "$BIN" "$probe_bin"
    exit 1
fi
rm -f "$probe_bin"

# Detect whether LSan is supported here (it aborts at startup if not).
if ASAN_OPTIONS="detect_leaks=1:exitcode=23" "$BIN" >/dev/null 2>&1; then
    : # ran clean with leak detection on
elif out=$(ASAN_OPTIONS="detect_leaks=1:exitcode=23" "$BIN" 2>&1) && [ -z "$out" ]; then
    :
fi

run_out=$(ASAN_OPTIONS="$ASAN_OPTIONS" UBSAN_OPTIONS="halt_on_error=1:exitcode=24" \
          "$BIN" 2>&1)
rc=$?

if grep -q "detect_leaks is not supported" <<< "$run_out"; then
    # Retry without LSan: ASan/UBSan still validate no double-free / UAF.
    run_out=$(ASAN_OPTIONS="exitcode=23" UBSAN_OPTIONS="halt_on_error=1:exitcode=24" \
              "$BIN" 2>&1)
    rc=$?
    echo "(note: LeakSanitizer unsupported here; ran ASan/UBSan only)"
fi

echo "$run_out"
rm -f "$BIN"

if [ "$rc" -ne 0 ]; then
    echo "FAIL hamt-owned-keys -- sanitizer reported an issue (rc=$rc)"
    exit 1
fi
if ! grep -q "all WKC2 tests passed" <<< "$run_out"; then
    echo "FAIL hamt-owned-keys -- test did not complete"
    exit 1
fi
echo "PASS hamt-owned-keys"
exit 0
