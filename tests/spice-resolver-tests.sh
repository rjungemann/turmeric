#!/usr/bin/env bash
# tests/spice-resolver-tests.sh -- per-file subcommand include-path tests.
#
# Exercises the SC0/SC1 surface of the spice-aware-check plan:
#
#   SC0: `tur check <file>` on a missing intra-spice import emits the
#        multi-line "searched: ... hint: ..." diagnostic.
#   SC1: `tur check -I <src> <file>` resolves intra-spice imports and exits 0.
#
# Later SC phases extend this script (SC2 wires emit-c/emit-h/format/run;
# SC4 adds auto-discovery and the --no-auto-spice escape hatch). New cases
# should follow the assert_* helpers below.

set -u
cd "$(dirname "$0")/.."

TUR="./build/tur"
[ -x "$TUR" ] || { echo "tests: $TUR not built; run 'just build' first" >&2; exit 2; }

FIXTURE="tests/fixtures/spice-resolver-ok"
SRC_ROOT="$FIXTURE/src"
ENTRY="$SRC_ROOT/foo/b.tur"

# SC5 fixtures: a consumer spice that depends on a sibling producer spice
# via :spices :path.  See tests/fixtures/spice-resolver-deps/build.tur.
DEPS_FIXTURE="tests/fixtures/spice-resolver-deps"
DEPS_ENTRY="$DEPS_FIXTURE/src/app/main.tur"

PASS=0
FAIL=0
FAILED=()

# assert_exit <expected-exit> <case-label> <cmd...>
# Runs the command; if exit code matches, PASS; else FAIL and dump stderr.
assert_exit() {
    local want="$1" label="$2"; shift 2
    local out err rc
    out=$(mktemp); err=$(mktemp)
    "$@" >"$out" 2>"$err"
    rc=$?
    if [ "$rc" -eq "$want" ]; then
        echo "PASS $label"
        PASS=$((PASS + 1))
    else
        echo "FAIL $label -- expected exit $want, got $rc"
        echo "  cmd: $*"
        echo "  stdout:" ; sed 's/^/    /' "$out"
        echo "  stderr:" ; sed 's/^/    /' "$err"
        FAIL=$((FAIL + 1))
        FAILED+=("$label")
    fi
    rm -f "$out" "$err"
}

# assert_stderr_contains <needle> <case-label> <cmd...>
# Runs the command (ignoring exit code) and asserts stderr contains the
# literal substring `needle`.
assert_stderr_contains() {
    local needle="$1" label="$2"; shift 2
    local err
    err=$(mktemp)
    "$@" >/dev/null 2>"$err"
    if grep -F -q "$needle" "$err"; then
        echo "PASS $label"
        PASS=$((PASS + 1))
    else
        echo "FAIL $label -- stderr missing substring:"
        echo "    $needle"
        echo "  cmd: $*"
        echo "  stderr:"; sed 's/^/    /' "$err"
        FAIL=$((FAIL + 1))
        FAILED+=("$label")
    fi
    rm -f "$err"
}

# SC1: `tur check -I src <file>` resolves intra-spice imports and exits 0.
assert_exit 0 "SC1: tur check -I <src> resolves intra-spice import" \
    "$TUR" check -I "$SRC_ROOT" "$ENTRY"

# SC1: short -I form (`-I<path>`) also works.
assert_exit 0 "SC1: tur check -I<src> (no space) also works" \
    "$TUR" check -I"$SRC_ROOT" "$ENTRY"

# SC0: `tur check <file>` without -I fails with the new searched/hint diag.
# Pass --no-auto-spice so SC4 auto-discovery doesn't quietly satisfy the
# import; the test exists to prove the diagnostic still triggers for users
# who explicitly opt out.
assert_exit 1 "SC0: tur check (--no-auto-spice, no -I) fails on intra-spice import" \
    "$TUR" --no-auto-spice check "$ENTRY"

assert_stderr_contains "searched:" \
    "SC0: diagnostic lists searched paths" \
    "$TUR" --no-auto-spice check "$ENTRY"

assert_stderr_contains "intra-spice import" \
    "SC0: diagnostic mentions intra-spice import" \
    "$TUR" --no-auto-spice check "$ENTRY"

# SC2: `tur emit-c -I <src> <file>` succeeds and emits valid C to stdout.
assert_exit 0 "SC2: tur emit-c -I <src> resolves intra-spice import" \
    "$TUR" emit-c -I "$SRC_ROOT" "$ENTRY"

# SC2: `tur emit-c` without -I (and without auto-discovery) fails.
assert_exit 1 "SC2: tur emit-c (--no-auto-spice, no -I) fails on intra-spice import" \
    "$TUR" --no-auto-spice emit-c "$ENTRY"

# SC2: `tur emit-h -I <src> <file>` succeeds.
assert_exit 0 "SC2: tur emit-h -I <src> resolves intra-spice import" \
    "$TUR" emit-h -I "$SRC_ROOT" "$ENTRY"

# SC2: `tur emit-h` without -I (and without auto-discovery) fails.
assert_exit 1 "SC2: tur emit-h (--no-auto-spice, no -I) fails on intra-spice import" \
    "$TUR" --no-auto-spice emit-h "$ENTRY"

# SC2: `tur emit-c --output-dir <dir> -I <src> <file>` succeeds and writes
# files to the output directory.
EMIT_OUT=$(mktemp -d)
assert_exit 0 "SC2: tur emit-c --output-dir -I <src> succeeds" \
    "$TUR" emit-c -I "$SRC_ROOT" --output-dir "$EMIT_OUT" "$ENTRY"
if [ -f "$EMIT_OUT/b.h" ] && [ -f "$EMIT_OUT/b.c" ]; then
    echo "PASS SC2: emit-c --output-dir produced b.h and b.c"
    PASS=$((PASS + 1))
else
    echo "FAIL SC2: emit-c --output-dir missing b.h or b.c in $EMIT_OUT"
    ls -la "$EMIT_OUT" || true
    FAIL=$((FAIL + 1))
    FAILED+=("SC2: emit-c --output-dir output present")
fi
rm -rf "$EMIT_OUT"

# SC2: `tur run -I <src> <file>` succeeds and the produced binary exits with
# the value `main` returns (42 in our fixture).
assert_exit 42 "SC2: tur run -I <src> compiles and executes intra-spice import" \
    "$TUR" run -I "$SRC_ROOT" "$ENTRY"

# SC4: with auto-discovery, `tur check <file>` (no -I) inside a spice now
# succeeds because find_spice_root walks up from b.tur to the build.tur
# at the fixture root and adds <root>/src to the include path.
assert_exit 0 "SC4: tur check auto-discovers enclosing spice src/" \
    "$TUR" check "$ENTRY"

assert_exit 0 "SC4: tur emit-c auto-discovers enclosing spice src/" \
    "$TUR" emit-c "$ENTRY"

assert_exit 0 "SC4: tur emit-h auto-discovers enclosing spice src/" \
    "$TUR" emit-h "$ENTRY"

assert_exit 42 "SC4: tur run auto-discovers enclosing spice src/" \
    "$TUR" run "$ENTRY"

# SC4 escape hatch: --no-auto-spice restores the pre-SC4 behavior, so
# the check should fail again with the SC0 diagnostic.
assert_exit 1 "SC4: --no-auto-spice opts out of auto-discovery (check fails again)" \
    "$TUR" --no-auto-spice check "$ENTRY"

assert_stderr_contains "intra-spice import" \
    "SC4: --no-auto-spice falls through to SC0 hint" \
    "$TUR" --no-auto-spice check "$ENTRY"

# SC6: --json output exercises the same auto-discovery code path the LSP
# server uses via tur_check_only().  No -I, no --no-auto-spice; expect a
# successful JSON envelope and exit 0.
assert_exit 0 "SC6: tur --json check inherits auto-discovery (LSP-equivalent path)" \
    "$TUR" --json check "$ENTRY"

# SC5: with cross-spice deps wired via auto-discovery, `tur check` on a
# consumer spice resolves `import helper/util` (from the sibling
# spice-resolver-dep) without any explicit -I.
assert_exit 0 "SC5: tur check resolves :spices :path dep via auto-discovery" \
    "$TUR" check "$DEPS_ENTRY"

# SC5: same for emit-c.
assert_exit 0 "SC5: tur emit-c resolves :spices :path dep via auto-discovery" \
    "$TUR" emit-c "$DEPS_ENTRY"

# SC5: tur run also resolves the dep and produces a working binary.
# The dep's `bonus` returns 100, so main's exit code is 100.
assert_exit 100 "SC5: tur run with :spices :path dep returns dep's value (100)" \
    "$TUR" run "$DEPS_ENTRY"

# SC5: --no-auto-spice disables the manifest read too, so the same check
# falls back to the SC0 diagnostic for the missing helper/util.
assert_exit 1 "SC5: --no-auto-spice disables :spices auto-resolution as well" \
    "$TUR" --no-auto-spice check "$DEPS_ENTRY"

# SC4: works the same way when invoked from a deeply-nested unrelated
# cwd, since find_spice_root canonicalizes via realpath() and walks
# ancestors of the *file*, not the cwd.
ABS_TUR="$PWD/$TUR"
ABS_ENTRY="$PWD/$ENTRY"
NESTED=$(mktemp -d)
mkdir -p "$NESTED/a/b/c/d"
( cd "$NESTED/a/b/c/d" && "$ABS_TUR" check "$ABS_ENTRY" ) >/dev/null 2>&1
rc=$?
if [ "$rc" -eq 0 ]; then
    echo "PASS SC4: auto-discovery works from a deeply-nested unrelated cwd"
    PASS=$((PASS + 1))
else
    echo "FAIL SC4: auto-discovery from deeply-nested cwd -- expected exit 0, got $rc"
    FAIL=$((FAIL + 1))
    FAILED+=("SC4: auto-discovery from nested cwd")
fi
rm -rf "$NESTED"

# SN3: regression guard against the launcher's cwd-relative stdlib path.
# Before SN1, running `tur check` from any cwd other than the repo root
# produced ~18 lines of `tur: cannot open 'stdlib/X.tur'` stderr noise
# on every invocation, even on success. After SN1+SN2 those should be
# silent. Running from `/tmp` (a directory with no stdlib/ subtree)
# exercises the exe-relative resolver and reaffirms the fix.
SN3_TMP=$(mktemp -d)
SN3_TUR_ABS="$PWD/$TUR"
SN3_ENTRY_ABS="$PWD/$ENTRY"
SN3_STDERR=$(mktemp)
( cd "$SN3_TMP" && "$SN3_TUR_ABS" check "$SN3_ENTRY_ABS" ) >/dev/null 2>"$SN3_STDERR"
rc=$?
SN3_LINES=$(wc -l <"$SN3_STDERR" | tr -d ' ')
if [ "$rc" -eq 0 ] && [ "$SN3_LINES" -eq 0 ]; then
    echo "PASS SN3: tur check from non-repo cwd has empty stderr on success"
    PASS=$((PASS + 1))
else
    echo "FAIL SN3: tur check from non-repo cwd"
    echo "  cwd:    $SN3_TMP"
    echo "  exit:   $rc (want 0)"
    echo "  stderr lines: $SN3_LINES (want 0)"
    echo "  stderr content:"
    sed 's/^/    /' "$SN3_STDERR"
    FAIL=$((FAIL + 1))
    FAILED+=("SN3: empty stderr on success from non-repo cwd")
fi
rm -rf "$SN3_TMP" "$SN3_STDERR"

echo
echo "summary: $PASS passed, $FAIL failed"
if [ "$FAIL" -ne 0 ]; then
    echo "failed cases:"
    for f in "${FAILED[@]}"; do echo "  - $f"; done
    exit 1
fi
exit 0
