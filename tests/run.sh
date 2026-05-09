#!/usr/bin/env bash
# tests/run.sh — fixture runner for tur (phase 0).
#
# Layout:
#   tests/fixtures/<name>/                 — happy-path fixture
#     input.tur (or <name>.tur)
#     expected.stdout
#     expected.c          (optional codegen snapshot)
#
#   tests/fixtures/errors/<name>/          — negative fixture
#     input.tur
#     expected.diag       (substring(s) that must appear in stderr; one per line)
#
# Pass: exits 0 with "PASS <name>". Any failure exits 1.

set -u
cd "$(dirname "$0")/.."

TUR="./build/tur"
[ -x "$TUR" ] || { echo "tests: $TUR not built; run 'make' first" >&2; exit 2; }

PASS=0
FAIL=0
FAILED=()

run_happy() {
    local dir="$1"
    local name="${dir#tests/fixtures/}"
    local input
    if   [ -f "$dir/input.tur" ]; then input="$dir/input.tur"
    elif [ -f "$dir/$(basename "$dir").tur" ]; then input="$dir/$(basename "$dir").tur"
    else echo "SKIP $name (no input)" ; return; fi

    local out_dir="$dir"
    local actual_stdout="$out_dir/actual.stdout"
    local actual_c="$out_dir/actual.c"

    "$TUR" emit-c "$input" > "$actual_c" 2> "$out_dir/actual.stderr"
    if [ $? -ne 0 ]; then
        FAIL=$((FAIL+1)); FAILED+=("$name (emit-c failed)")
        echo "FAIL $name — tur emit-c failed"
        cat "$out_dir/actual.stderr"
        return
    fi

    local exe
    exe=$(mktemp -t tur-test-XXXXXX)
    "$TUR" build "$input" -o "$exe" 2> "$out_dir/actual.stderr"
    if [ $? -ne 0 ]; then
        FAIL=$((FAIL+1)); FAILED+=("$name (build failed)")
        echo "FAIL $name — tur build failed"
        cat "$out_dir/actual.stderr"
        rm -f "$exe"
        return
    fi

    "$exe" > "$actual_stdout"
    local rc=$?
    rm -f "$exe"

    if [ -f "$dir/expected.stdout" ]; then
        if ! diff -u "$dir/expected.stdout" "$actual_stdout" > /dev/null; then
            FAIL=$((FAIL+1)); FAILED+=("$name (stdout mismatch)")
            echo "FAIL $name — stdout mismatch"
            diff -u "$dir/expected.stdout" "$actual_stdout" | sed 's/^/    /'
            return
        fi
    fi

    if [ -f "$dir/expected.c" ]; then
        if ! diff -u "$dir/expected.c" "$actual_c" > /dev/null; then
            FAIL=$((FAIL+1)); FAILED+=("$name (codegen mismatch)")
            echo "FAIL $name — codegen mismatch"
            diff -u "$dir/expected.c" "$actual_c" | sed 's/^/    /'
            return
        fi
    fi

    if [ "$rc" -ne 0 ]; then
        FAIL=$((FAIL+1)); FAILED+=("$name (exit $rc)")
        echo "FAIL $name — program exited $rc"
        return
    fi

    PASS=$((PASS+1))
    echo "PASS $name"
}

run_negative() {
    local dir="$1"
    local name="${dir#tests/fixtures/}"
    local input="$dir/input.tur"
    [ -f "$input" ] || { echo "SKIP $name (no input)"; return; }

    "$TUR" emit-c "$input" > /dev/null 2> "$dir/actual.stderr"
    local rc=$?
    if [ $rc -eq 0 ]; then
        FAIL=$((FAIL+1)); FAILED+=("$name (expected error, got success)")
        echo "FAIL $name — expected error, but tur exited 0"
        return
    fi

    if [ -f "$dir/expected.diag" ]; then
        local missing=0
        while IFS= read -r needle; do
            [ -z "$needle" ] && continue
            if ! grep -F -q "$needle" "$dir/actual.stderr"; then
                echo "FAIL $name — expected diagnostic substring not found:"
                echo "    $needle"
                missing=1
            fi
        done < "$dir/expected.diag"
        if [ $missing -ne 0 ]; then
            FAIL=$((FAIL+1)); FAILED+=("$name (diagnostic mismatch)")
            echo "    actual stderr:"
            sed 's/^/      /' "$dir/actual.stderr"
            return
        fi
    fi

    PASS=$((PASS+1))
    echo "PASS $name"
}

# Happy fixtures: tests/fixtures/* except tests/fixtures/errors
shopt -s nullglob
for d in tests/fixtures/*/; do
    d="${d%/}"
    [ "$d" = "tests/fixtures/errors" ] && continue
    [ -d "$d" ] || continue
    run_happy "$d"
done

# Error fixtures
for d in tests/fixtures/errors/*/; do
    d="${d%/}"
    [ -d "$d" ] || continue
    run_negative "$d"
done

echo
echo "summary: $PASS passed, $FAIL failed"
if [ $FAIL -ne 0 ]; then
    for f in "${FAILED[@]}"; do echo "  - $f"; done
    exit 1
fi
exit 0
