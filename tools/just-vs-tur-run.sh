#!/usr/bin/env bash
# just-vs-tur-run.sh -- RN8 CI parity test.
#
# For each ok-* fixture in tests/tur/run/fixtures/, run every non-default
# recipe under both `just` and `tur run` and compare exit codes + stdout.
#
# Requirements:
#   - tur binary at $TUR_BIN (default: ./build/tur)
#   - just binary on $PATH (soft-skip when absent)
#
# Usage: bash tools/just-vs-tur-run.sh
# Exit codes:
#   0   all recipes matched
#   1   at least one mismatch

set -euo pipefail

TUR_BIN="${TUR_BIN:-./build/tur}"
FIXTURES_DIR="tests/tur/run/fixtures"
PASS=0
FAIL=0
SKIP=0

if ! command -v just >/dev/null 2>&1; then
    echo "just-vs-tur-run: just not found on PATH -- skipping parity tests" >&2
    exit 0
fi

for fixture_dir in "$FIXTURES_DIR"/ok-*/; do
    jf="$fixture_dir/Justfile"
    if [ ! -f "$jf" ]; then continue; fi

    # Extract recipe names (lines that match NAME: at column 0, not 'default')
    recipes=$(grep -E '^[a-z][a-z0-9_-]*[[:space:]]*:' "$jf" \
              | sed 's/[[:space:]]*:.*//' \
              | grep -v '^default$' || true)

    for recipe in $recipes; do
        # Skip recipes that require actual tools (tur build etc.)
        # We only compare recipes that have @echo or echo bodies.
        body=$(awk "/^${recipe}[[:space:]]*:/{found=1;next} found && /^\t/{print;next} found{exit}" "$jf")
        if echo "$body" | grep -qE "tur (build|test|fmt|check|docs|install)"; then
            continue
        fi

        just_out=$(cd "$fixture_dir" && just "$recipe" 2>/dev/null) || just_rc=$?
        just_rc="${just_rc:-0}"
        tur_out=$(cd "$fixture_dir" && "$OLDPWD/$TUR_BIN" run "$recipe" 2>/dev/null) || tur_rc=$?
        tur_rc="${tur_rc:-0}"

        if [ "$just_rc" != "$tur_rc" ] || [ "$just_out" != "$tur_out" ]; then
            echo "FAIL: $fixture_dir recipe '$recipe'"
            echo "  just exit=$just_rc stdout='$just_out'"
            echo "  tur  exit=$tur_rc  stdout='$tur_out'"
            FAIL=$((FAIL + 1))
        else
            PASS=$((PASS + 1))
        fi
    done
done

echo "just-vs-tur-run: $PASS passed, $FAIL failed, $SKIP skipped"

# A missing/empty fixture tree used to make this script a silent no-op: the
# `ok-*/` glob matched nothing, every iteration `continue`d, and it exited 0
# having compared nothing.  That is indistinguishable from "parity holds" in
# CI, and it is how tests/tur/run/fixtures/ stayed absent unnoticed.
if [ "$PASS" -eq 0 ] && [ "$FAIL" -eq 0 ]; then
    echo "just-vs-tur-run: ERROR -- no recipes were compared." >&2
    echo "  Expected fixtures in $FIXTURES_DIR/ok-*/Justfile" >&2
    exit 1
fi

[ "$FAIL" -eq 0 ]
