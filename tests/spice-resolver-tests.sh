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
assert_exit 1 "SC0: tur check (no -I) fails on intra-spice import" \
    "$TUR" check "$ENTRY"

assert_stderr_contains "searched:" \
    "SC0: diagnostic lists searched paths" \
    "$TUR" check "$ENTRY"

assert_stderr_contains "intra-spice import" \
    "SC0: diagnostic mentions intra-spice import" \
    "$TUR" check "$ENTRY"

echo
echo "summary: $PASS passed, $FAIL failed"
if [ "$FAIL" -ne 0 ]; then
    echo "failed cases:"
    for f in "${FAILED[@]}"; do echo "  - $f"; done
    exit 1
fi
exit 0
