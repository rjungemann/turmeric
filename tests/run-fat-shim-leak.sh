#!/usr/bin/env bash
# tests/run-fat-shim-leak.sh -- LeakSanitizer regression gate for the fat-sink
# shim box (docs/archive/fat-sink-shim-box-leaks-per-call.md).
#
# EX_FN_TO_FAT boxes a bare fn into a { shim, orig } fat handle.  At a `^fat`
# parameter nothing freed it, so every call mallocd a box that became
# unreachable the moment the callee returned -- 109 MiB over 4e6 iterations.
# The box is now the shared file-scope one whenever the sink provably neither
# retains nor drops its argument.
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
# A per-call box IS an LSan leak, so this gate fails loudly on a regression --
# unlike an RSS measurement, which needs a trip count large enough to see.
#
# Exit status: 0 if all checks pass, 1 otherwise.

set -uo pipefail
cd "$(dirname "$0")/.."

TUR="${TUR:-./build/tur}"
CC="${CC:-cc}"
FIXTURE="tests/fixtures/fat-sink-shim-no-leak/input.tur"

if [ ! -x "$TUR" ]; then
    echo "FAIL fat-shim-leak -- tur binary not found at $TUR (build first)"
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
    echo "FAIL fat-shim-leak -- emit-c failed"
    sed 's/^/    /' "$WORK/emit.err"
    exit 1
fi

# Compile with ASan/UBSan.  LeakSanitizer ships with ASan on Linux; on macOS it
# is unsupported and ASan aborts at startup, so probe and skip cleanly.
#
# src/runtime/trail.c is in the list even though this fixture never touches the
# trail -- stdlib/trail.tur is auto-loaded into every program, so the emitted C
# carries its inline-C bodies and their calls into tur_bt_cell_*.  GCC at -O0
# emits those unused statics and the link needs the definitions; Apple clang
# drops them, so macOS cannot catch a regression here.  Same note as
# tests/run-closure-env-leak.sh.
if ! "$CC" -g -O0 -fno-strict-aliasing -fsanitize=address,undefined \
        -Isrc/runtime -o "$BIN" "$C_OUT" \
        src/runtime/hamt.c src/runtime/runtime.c src/runtime/rc.c \
        src/runtime/gc.c src/runtime/rc_free_queue.c src/runtime/tur_string.c \
        src/runtime/symbols.c src/runtime/trail.c \
        src/runtime/region.c src/runtime/arena.c -lpthread 2>"$WORK/cc.err"; then
    echo "FAIL fat-shim-leak -- C compile failed"
    sed 's/^/    /' "$WORK/cc.err"
    exit 1
fi

probe=$(ASAN_OPTIONS="detect_leaks=1" "$BIN" 2>&1)
if grep -q "detect_leaks is not supported" <<< "$probe"; then
    echo "PASS fat-shim-leak (skipped: LeakSanitizer unsupported on this platform)"
    echo "fat-shim-leak summary: skipped -- no LSan on this platform"
    exit 0
fi

out=$(ASAN_OPTIONS="detect_leaks=1" "$BIN" 2>&1)
rc=$?

fail=0
# (1) Output sanity: the program ran to completion.
if ! grep -qx "6000" <<< "$out"; then
    echo "FAIL fat-shim-leak -- expected loop result 6000 not in output"
    fail=1
fi
# (2) The real assertion: no LeakSanitizer leak report, clean exit.
if grep -q "LeakSanitizer: detected memory leaks" <<< "$out"; then
    echo "FAIL fat-shim-leak -- LeakSanitizer reported a leak"
    fail=1
fi
if grep -q "runtime error:" <<< "$out"; then
    echo "FAIL fat-shim-leak -- UBSan reported undefined behavior"
    fail=1
fi
if [ "$rc" -ne 0 ]; then
    echo "FAIL fat-shim-leak -- nonzero exit ($rc) under ASan"
    fail=1
fi

if [ "$fail" -ne 0 ]; then
    printf '%s\n' "$out" | sed 's/^/    /'
    echo "fat-shim-leak summary: 0 passed, 1 failed"
    exit 1
fi

echo "PASS fat-shim-leak"
echo "fat-shim-leak summary: 1 passed, 0 failed"
exit 0
