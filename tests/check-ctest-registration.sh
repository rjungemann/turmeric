#!/usr/bin/env bash
# check-ctest-registration.sh -- every `add_test(NAME ...)` declared under
# src/CMakeLists.txt must actually be registered with CTest.
#
# CMake only collects tests from a subdirectory if `enable_testing()` ran
# BEFORE `add_subdirectory(...)`.  When the order was the other way round,
# `tur_trail` sat in src/CMakeLists.txt for months, built, passed by hand, and
# was never run by CI -- cmake gave no diagnostic and `ctest -N` simply did not
# mention it (src-cmakelists-add-test-never-registers).  This lint makes the
# next such drop visible.
#
# Usage: bash tests/check-ctest-registration.sh [build-dir]   (default: build)
set -u
cd "$(dirname "$0")/.."

BUILD="${1:-${TUR_BUILD_DIR:-build}}"
if [ ! -f "$BUILD/CTestTestfile.cmake" ]; then
    echo "SKIP check-ctest-registration -- $BUILD is not a configured build tree"
    exit 0
fi

# Names declared under src/.  Anchor on `add_test(NAME <name>`; the
# multi-line form puts NAME on the same line as the paren in this tree.
declared=$(grep -oE 'add_test\(\s*NAME\s+[A-Za-z0-9_]+' src/CMakeLists.txt \
           | awk '{print $NF}' | sort -u)
if [ -z "$declared" ]; then
    echo "PASS check-ctest-registration (no add_test under src/)"
    exit 0
fi

registered=$(cd "$BUILD" && ctest -N 2>/dev/null | sed -n 's/^ *Test *#[0-9]*: *//p')
fail=0
for t in $declared; do
    if ! grep -qx "$t" <<< "$registered"; then
        echo "FAIL check-ctest-registration -- src/CMakeLists.txt declares test '$t' but 'ctest -N' does not list it"
        echo "     (is enable_testing() still above add_subdirectory(src) in CMakeLists.txt?)"
        fail=1
    fi
done
[ "$fail" -ne 0 ] && exit 1
n=$(wc -w <<< "$declared")
echo "PASS check-ctest-registration ($n test(s) declared under src/, all registered)"
