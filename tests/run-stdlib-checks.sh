#!/usr/bin/env bash
# tests/run-stdlib-checks.sh -- type-check curated stdlib files in isolation.
#
# Runs `tur check` over an allowlist of stdlib/*.tur files that are NOT
# auto-loaded by the compiler. Catches regressions in non-auto-loaded
# stdlib files (the auto-loaded ones are exercised implicitly by every
# other build).
#
# To add a file: verify `./build/tur check stdlib/<file>` succeeds, then
# append it to STDLIB_FILES below. See
# docs/stdlib-check-allowlist-plan.md for the broader plan to extend
# coverage.
set -u
cd "$(dirname "$0")/.."

TUR="./build/tur"
[ -x "$TUR" ] || { echo "tests: $TUR not built; run 'just build' first" >&2; exit 2; }

STDLIB_FILES=(
    stdlib/arrow.tur
)

PASS=0
FAIL=0
FAILED_FILES=()

for f in "${STDLIB_FILES[@]}"; do
    if "$TUR" check "$f" >/dev/null 2>&1; then
        PASS=$((PASS+1))
    else
        FAIL=$((FAIL+1))
        FAILED_FILES+=("$f")
        echo "FAIL $f"
        "$TUR" check "$f" 2>&1 | sed 's/^/    /'
    fi
done

echo
echo "stdlib-checks: $PASS passed, $FAIL failed"
if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
exit 0
