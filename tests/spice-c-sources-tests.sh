#!/usr/bin/env bash
# tests/spice-c-sources-tests.sh -- spices-c-sources-plan.
#
# Exercises :c-sources / :c-includes under :build-opts:
#
#   - A single spice that vendors a hand-written .c plus a private header dir,
#     used by both the aux .c and an inline-C block in a .tur module, builds
#     and runs (the binary exits with the value the vendored C returns).
#   - :c-includes -I reaches the inline-C compile (the .tur block #includes the
#     private header).
#   - Cross-spice: a consumer that :spices-depends on a vendor spice links the
#     vendor's vendored .c into its binary.
#   - Parse-time validation: a missing source, a non-.c extension, and an
#     absolute path each fail the build with a DIAG_ERROR (not a segfault).
#   - Manifest round-trip: :c-sources / :c-includes survive read -> write
#     (via `tur add-cmake`, which rewrites build.tur through pkg_manifest_write).
#
# Owned by its own ctest target; the fixtures carry requires.dedicated-runner
# (and live under tests/fixtures/spices/, which the by-value run.sh does not
# descend into) so the main suite does not also try to build them.

set -u
cd "$(dirname "$0")/.."

TUR="${TUR:-./build/tur}"
[ -x "$TUR" ] || { echo "tests: $TUR not built; run 'tur run build' first" >&2; exit 2; }

# Leak detection is irrelevant to these CLI-level assertions and the
# build-failure paths intentionally exit before freeing everything; turn it off
# so a partial-free on an error path does not mask a real test failure.
export ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}detect_leaks=0"

SPICES="tests/fixtures/spices"

PASS=0
FAIL=0
FAILED=()

# assert_build_runs <expected-binary-exit> <label> <spice-dir> <binary-name>
# Builds the spice, runs the produced binary, and asserts its exit code.
assert_build_runs() {
    local want="$1" label="$2" dir="$3" binname="$4"
    rm -rf "$dir/build"
    local berr; berr=$(mktemp)
    "$TUR" build "$dir" >/dev/null 2>"$berr"
    local brc=$?
    if [ "$brc" -ne 0 ]; then
        echo "FAIL $label -- build failed (exit $brc)"
        echo "  stderr:"; sed 's/^/    /' "$berr"
        FAIL=$((FAIL + 1)); FAILED+=("$label"); rm -f "$berr"; rm -rf "$dir/build"; return
    fi
    rm -f "$berr"
    local bin="$dir/build/bin/$binname"
    if [ ! -x "$bin" ]; then
        echo "FAIL $label -- expected binary '$bin' not produced"
        FAIL=$((FAIL + 1)); FAILED+=("$label"); rm -rf "$dir/build"; return
    fi
    "$bin" >/dev/null 2>&1
    local rrc=$?
    rm -rf "$dir/build"
    if [ "$rrc" -eq "$want" ]; then
        echo "PASS $label"
        PASS=$((PASS + 1))
    else
        echo "FAIL $label -- binary exited $rrc, want $want"
        FAIL=$((FAIL + 1)); FAILED+=("$label")
    fi
}

# assert_build_fails <needle> <label> <spice-dir>
# Asserts the build exits non-zero AND prints `needle` to stderr.
assert_build_fails() {
    local needle="$1" label="$2" dir="$3"
    rm -rf "$dir/build"
    local berr; berr=$(mktemp)
    "$TUR" build "$dir" >/dev/null 2>"$berr"
    local brc=$?
    rm -rf "$dir/build"
    if [ "$brc" -ne 0 ] && grep -F -q "$needle" "$berr"; then
        echo "PASS $label"
        PASS=$((PASS + 1))
    else
        echo "FAIL $label -- exit $brc (want non-zero) / stderr missing: $needle"
        echo "  stderr:"; sed 's/^/    /' "$berr"
        FAIL=$((FAIL + 1)); FAILED+=("$label")
    fi
    rm -f "$berr"
}

# --- Single-spice: vendored .c + private header reached by aux .c and inline-C.
assert_build_runs 42 \
    "single spice: vendored :c-sources + :c-includes build and run (exit 42)" \
    "$SPICES/spice-with-aux-c" "spice-with-aux-c"

# --- Cross-spice: consumer links the vendor spice's :c-sources.
assert_build_runs 7 \
    "cross-spice: consumer links dep's :c-sources (exit 7)" \
    "$SPICES/consumer-of-aux" "consumer-of-aux"

# --- Error cases: each must DIAG_ERROR and fail the build, not segfault.
assert_build_fails "not found" \
    "error: missing :c-sources file fails the build" \
    "$SPICES/aux-c-missing-source"

assert_build_fails "must have a .c, .cc, or .cpp extension" \
    "error: non-.c :c-sources extension fails the build" \
    "$SPICES/aux-c-bad-extension"

assert_build_fails "must be a relative path" \
    "error: absolute :c-sources path fails the build" \
    "$SPICES/aux-c-absolute-path"

# --- Manifest round-trip: keys survive read -> write.
RT=$(mktemp -d)
mkdir -p "$RT/c/inc"; : > "$RT/c/foo.c"; : > "$RT/c/inc/foo.h"
cat > "$RT/build.tur" <<'EOF'
(defpackage rt-test
  :name "rt-test"
  :build-opts #{
    :c-includes ["c/inc"]
    :c-sources  ["c/foo.c"]
  })
EOF
RT_ABS_TUR="$PWD/$TUR"
( cd "$RT" && "$RT_ABS_TUR" add-cmake https://example.com/libfoo.git ) >/dev/null 2>&1
if grep -F -q ':c-includes ["c/inc"]' "$RT/build.tur" \
   && grep -F -q ':c-sources ["c/foo.c"]' "$RT/build.tur"; then
    echo "PASS round-trip: :c-sources / :c-includes survive read -> write"
    PASS=$((PASS + 1))
else
    echo "FAIL round-trip: keys lost after rewrite"
    echo "  build.tur:"; sed 's/^/    /' "$RT/build.tur"
    FAIL=$((FAIL + 1)); FAILED+=("round-trip")
fi
rm -rf "$RT"

echo
echo "summary: $PASS passed, $FAIL failed"
if [ "$FAIL" -ne 0 ]; then
    echo "failed cases:"
    for f in "${FAILED[@]}"; do echo "  - $f"; done
    exit 1
fi
exit 0
