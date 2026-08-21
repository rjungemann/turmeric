#!/usr/bin/env bash
# tests/run-hamt-del-lineage.sh -- regression gate for the HAMT delete-path
# sibling refcount bug (docs/archive/history/hamt-delete-sibling-refcount-report.md).
#
# Compiles the HAMT runtime + tests/test_hamt_del_lineage.c with AddressSanitizer
# + UndefinedBehaviorSanitizer (and LeakSanitizer on Linux) and runs it.  The
# acceptance is that freeing a *whole* persistent-map lineage that includes a
# tur_hamt_del-derived map is double-free / use-after-free clean (pre-fix this
# tripped a heap-use-after-free in tur_hamt_node_release).
#
# Exit status: 0 if the test binary exits 0 with no sanitizer report, else 1.

set -uo pipefail
cd "$(dirname "$0")/.."

CC="${CC:-cc}"
SRC_RUNTIME="src/runtime"
TEST_SRC="tests/test_hamt_del_lineage.c"
BIN="$(mktemp -t tur-del-lineage-XXXXXX)"

if ! "$CC" -std=c11 -fsanitize=address,undefined -I "$SRC_RUNTIME" \
        "$TEST_SRC" "$SRC_RUNTIME/hamt.c" -o "$BIN" 2>/tmp/del-lineage-cc.log; then
    echo "FAIL hamt-del-lineage -- compile failed"
    cat /tmp/del-lineage-cc.log
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
    echo "FAIL hamt-del-lineage -- sanitizer reported an issue (rc=$rc)"
    exit 1
fi
if ! grep -q "all hamt del-lineage tests passed" <<< "$run_out"; then
    echo "FAIL hamt-del-lineage -- test did not complete"
    exit 1
fi
echo "PASS hamt-del-lineage"
exit 0
