#!/usr/bin/env bash
set -u
cd "$(dirname "$0")/.."

TUR="${TUR:-./build/tur}"
[ -x "$TUR" ] || { echo "tests: $TUR not built; run 'make' first" >&2; exit 2; }

PASS=0
FAIL=0

run_cli_case() {
    local case_dir="$1"
    local name="${case_dir#tests/cli/}"
    local expected_exit expected_stdout actual_stdout actual_stderr

    expected_exit=$(tr -d '[:space:]' < "$case_dir/expected.exit")
    expected_stdout="$case_dir/expected.stdout"
    actual_stdout="$case_dir/actual.stdout"
    actual_stderr="$case_dir/actual.stderr"

    "$TUR" test "$case_dir/cases" > "$actual_stdout" 2> "$actual_stderr"
    local rc=$?

    if [ "$rc" -ne "$expected_exit" ]; then
        FAIL=$((FAIL+1))
        echo "FAIL $name - exit $rc (expected $expected_exit)"
        if [ -s "$actual_stderr" ]; then
            sed 's/^/    /' "$actual_stderr"
        fi
        return
    fi

    if ! diff -u "$expected_stdout" "$actual_stdout" > /dev/null; then
        FAIL=$((FAIL+1))
        echo "FAIL $name - stdout mismatch"
        diff -u "$expected_stdout" "$actual_stdout" | sed 's/^/    /'
        return
    fi

    PASS=$((PASS+1))
    echo "PASS $name"
}

for d in tests/cli/*/; do
    d="${d%/}"
    [ -d "$d" ] || continue
    run_cli_case "$d"
done

echo
echo "cli summary: $PASS passed, $FAIL failed"
if [ $FAIL -ne 0 ]; then
    exit 1
fi
exit 0
