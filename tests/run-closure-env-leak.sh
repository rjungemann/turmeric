#!/usr/bin/env bash
# tests/run-closure-env-leak.sh -- LeakSanitizer regression gate for the
# fat-closure-env scoped-free pass (docs/archive/history/fat-closure-env-leak.md).
#
# Every `(fn ...)` that captures a free variable heap-allocates a fat-closure
# env.  When the closure provably does not escape its defining scope, the
# emitter releases that env at scope exit; without the free, a closure built in
# a loop or a repeatedly-invoked effect handler leaks one env per construction.
#
# tests/run.sh compiles the *generated program* without ASan (only the `tur`
# binary itself is sanitized), so a runtime leak in emitted code is invisible to
# the normal suite -- the fixture carries `requires.dedicated-runner` and is
# PASS-skipped there.  This gate compiles the emitted C with
# -fsanitize=address,undefined and runs it with LeakSanitizer ON, asserting:
#
#   1. the program produces the expected output (it actually ran), and
#   2. LeakSanitizer reports no leak.
#
# Exit status: 0 if all checks pass, 1 otherwise.

set -uo pipefail
cd "$(dirname "$0")/.."

TUR="${TUR:-./build/tur}"
CC="${CC:-cc}"
FIXTURE="tests/fixtures/closure-env-no-leak/input.tur"

if [ ! -x "$TUR" ]; then
    echo "FAIL closure-env-leak -- tur binary not found at $TUR (build first)"
    exit 1
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
C_OUT="$WORK/prog.c"
BIN="$WORK/prog"

# Emit C for the fixture.
#
# TUR_RCGC_FROM_ARCHIVE=1 suppresses the emitted rc/GC replica so the real
# runtime sources below supply that half instead (the DEDUP-3 "archive" state).
# The two cannot be mixed: the replica exports rc_cb_alloc / rc_strong_decrement
# with external linkage, so linking rc.c alongside it is a duplicate-symbol
# error -- while its rc_free_queue half is `static`, leaving runtime.c's
# rc_free_queue_reset_drain_state() call in tur_catch_unwind unresolvable.
# Which copy backs the allocator is immaterial to this gate; the closure-env
# free being asserted is emitted either way.
if ! TUR_RCGC_FROM_ARCHIVE=1 "$TUR" emit-c "$FIXTURE" > "$C_OUT" 2>"$WORK/emit.err"; then
    echo "FAIL closure-env-leak -- emit-c failed"
    sed 's/^/    /' "$WORK/emit.err"
    exit 1
fi

# Compile with ASan/UBSan.  LeakSanitizer ships with ASan on Linux; on macOS it
# is unsupported and ASan aborts at startup, so probe and skip cleanly.
if ! "$CC" -g -O0 -fno-strict-aliasing -fsanitize=address,undefined \
        -Isrc/runtime -o "$BIN" "$C_OUT" \
        src/runtime/hamt.c src/runtime/runtime.c src/runtime/rc.c \
        src/runtime/gc.c src/runtime/rc_free_queue.c src/runtime/tur_string.c \
        src/runtime/symbols.c -lpthread 2>"$WORK/cc.err"; then
    echo "FAIL closure-env-leak -- C compile failed"
    sed 's/^/    /' "$WORK/cc.err"
    exit 1
fi

probe=$(ASAN_OPTIONS="detect_leaks=1" "$BIN" 2>&1)
if printf '%s' "$probe" | grep -q "detect_leaks is not supported"; then
    echo "PASS closure-env-leak (skipped: LeakSanitizer unsupported on this platform)"
    echo "closure-env-leak summary: skipped -- no LSan on this platform"
    exit 0
fi

out=$(ASAN_OPTIONS="detect_leaks=1" "$BIN" 2>&1)
rc=$?

fail=0
# (1) Output sanity: both lines must be present (the program ran to completion).
if ! printf '%s\n' "$out" | grep -qx "202"; then
    echo "FAIL closure-env-leak -- expected handler result 202 not in output"
    fail=1
fi
if ! printf '%s\n' "$out" | grep -qx "500500"; then
    echo "FAIL closure-env-leak -- expected loop result 500500 not in output"
    fail=1
fi
# (2) The real assertion: no LeakSanitizer leak report, clean exit.
if printf '%s' "$out" | grep -q "LeakSanitizer: detected memory leaks"; then
    echo "FAIL closure-env-leak -- LeakSanitizer reported a leak"
    fail=1
fi
if printf '%s' "$out" | grep -q "runtime error:"; then
    echo "FAIL closure-env-leak -- UBSan reported undefined behavior"
    fail=1
fi
if [ "$rc" -ne 0 ]; then
    echo "FAIL closure-env-leak -- nonzero exit ($rc) under ASan"
    fail=1
fi

if [ "$fail" -ne 0 ]; then
    printf '%s\n' "$out" | sed 's/^/    /'
    echo "closure-env-leak summary: 0 passed, 1 failed"
    exit 1
fi

echo "PASS closure-env-leak"
echo "closure-env-leak summary: 1 passed, 0 failed"
exit 0
