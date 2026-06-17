#!/usr/bin/env bash
# tests/run-result-typedef-multi-module.sh -- pin
# `result-typedef-duplicated-across-modules`: a multi-module project where
# two modules export functions returning the same `(Result cstr cstr)`
# instantiation, and a third module imports both.  Each module's generated
# `.h` emits a `typedef struct Result__cstr__cstr { ... }`; without a
# per-instantiation include guard the third TU fails to compile with
# `redefinition of 'Result__cstr__cstr'`.
#
# The fix wraps every monomorphic-instantiation typedef in
# `#ifndef TUR_TY_<Name> / #define / #endif` (src/compiler/types.c
# emit_registered_struct_app_rec + emit_registered_adt_app_rec).

set -uo pipefail
cd "$(dirname "$0")/.."
REPO="$PWD"

TUR="$REPO/build/tur"
FIXTURE="$REPO/tests/fixtures/result-typedef-multi-module"
WORK="$(mktemp -d -t tur-rtmm.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

# Leaks in the Debug (ASan) build of tur are out of scope here; we are
# exercising the codegen path, not chasing compiler-internal leaks.
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}"

PASS=0
FAIL=0
pass() { PASS=$((PASS + 1)); echo "PASS $1"; }
fail() { FAIL=$((FAIL + 1)); echo "FAIL $1 -- $2"; }

if [ ! -x "$TUR" ]; then
    echo "tests: $TUR not built; run 'just build' first" >&2
    exit 2
fi

cp -R "$FIXTURE" "$WORK/proj"
PROJ="$WORK/proj"
BIN="$WORK/app"

build_out=$(cd "$WORK" && "$TUR" build "$PROJ" -o "$BIN" 2>&1)
build_rc=$?
if [ $build_rc -ne 0 ]; then
    # The specific failure we are pinning against: a `redefinition` error on
    # the Result__cstr__cstr typedef.
    if echo "$build_out" | grep -q "redefinition of 'Result__cstr__cstr'"; then
        fail "result-typedef-no-redefinition" "redefinition of Result__cstr__cstr re-emerged: $build_out"
    else
        fail "result-typedef-no-redefinition" "tur build exit=$build_rc: $build_out"
    fi
else
    pass "result-typedef-no-redefinition"
fi

# The linked binary must exist and run, returning 42.
if [ -x "$BIN" ]; then
    pass "result-typedef-binary-exists"
    "$BIN"
    run_rc=$?
    if [ "$run_rc" -eq 42 ]; then
        pass "result-typedef-binary-runs"
    else
        fail "result-typedef-binary-runs" "binary returned $run_rc, expected 42"
    fi
else
    fail "result-typedef-binary-exists" "binary not produced at $BIN"
fi

# Sanity: each module's emitted header must contain the include-guard for
# Result__cstr__cstr.  This proves the fix is in place (rather than the
# build accidentally passing because only one module emitted the typedef).
guarded_count=0
for h in "$PROJ/build/obj"/*.h; do
    [ -f "$h" ] || continue
    if grep -q "TUR_TY_Result__cstr__cstr" "$h"; then
        guarded_count=$((guarded_count + 1))
    fi
done
if [ "$guarded_count" -ge 2 ]; then
    pass "result-typedef-guard-emitted-per-module"
else
    fail "result-typedef-guard-emitted-per-module" \
        "expected TUR_TY_Result__cstr__cstr guard in >=2 module headers, got $guarded_count"
fi

echo
echo "summary: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
