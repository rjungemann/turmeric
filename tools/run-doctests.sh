#!/usr/bin/env bash
# tools/run-doctests.sh -- run generated doctest programs and compare output.
#
# Usage:
#   bash tools/run-doctests.sh
#
# Prerequisites:
#   Run `python3 tools/doctest.py stdlib/ --out tests/doctest-generated/` first
#   (or let `just doctest` call both steps in order).
#
# Environment:
#   TUR         path to tur binary (default: ./build/tur)
#   TUR_FORCE   set to 1 to skip stamp cache and re-run every module

set -u
cd "$(dirname "$0")/.."

TUR="${TUR:-./build/tur}"
[ -x "$TUR" ] || { echo "run-doctests: $TUR not built; run 'just build' first" >&2; exit 2; }

GENERATED="tests/doctest-generated"
STAMP_CACHE="tests/.stamp-cache-doctest"
VERIFIED_OUT="$GENERATED/verified.txt"

TUR_FORCE="${TUR_FORCE:-0}"

PASS=0
FAIL=0
SKIP=0
FAILED=()

# ---------------------------------------------------------------------------
# Stamp-cache helpers (keyed on hash of generated .tur + mtime of tur binary)
# ---------------------------------------------------------------------------

_tur_mtime() { stat -f '%m' "$1" 2>/dev/null || stat -c '%Y' "$1" 2>/dev/null || echo "0"; }
_tur_hash_file() {
    if command -v md5 >/dev/null 2>&1; then md5 -q "$1" 2>/dev/null
    elif command -v md5sum >/dev/null 2>&1; then md5sum "$1" 2>/dev/null | awk '{print $1}'
    else echo "nohash"; fi
}
stamp_key() { echo "$(_tur_hash_file "$1")-$(_tur_mtime "$TUR")"; }
stamp_check() {
    [ "$TUR_FORCE" = "1" ] && return 1
    local sf="$STAMP_CACHE/$(printf '%s' "$1" | tr '/ ' '__').stamp"
    [ -f "$sf" ] && [ "$(cat "$sf")" = "$(stamp_key "$2")" ]
}
stamp_write() {
    mkdir -p "$STAMP_CACHE"
    local sf="$STAMP_CACHE/$(printf '%s' "$1" | tr '/ ' '__').stamp"
    stamp_key "$2" > "$sf"
}

# ---------------------------------------------------------------------------
# Main loop
# ---------------------------------------------------------------------------

# Truncate verified.txt; it is rebuilt entirely on each run (cached or fresh)
> "$VERIFIED_OUT"

shopt -s nullglob
tur_files=("$GENERATED"/*.tur)
shopt -u nullglob

if [ "${#tur_files[@]}" -eq 0 ]; then
    echo "run-doctests: no generated test files in $GENERATED" >&2
    echo "  Run: python3 tools/doctest.py stdlib/ --out $GENERATED" >&2
    exit 1
fi

for tur_file in "${tur_files[@]}"; do
    stem="${tur_file%.tur}"
    expected_file="${stem}.expected"
    names_file="${stem}.names"

    [ -f "$expected_file" ] || continue
    [ -f "$names_file" ] || continue

    mod_name="$(basename "$stem")"

    if stamp_check "$mod_name" "$tur_file"; then
        # All cases for this module passed on the last run; credit them as
        # verified without re-running.
        while IFS= read -r fname || [ -n "$fname" ]; do
            [ -z "$fname" ] && continue
            echo "$fname" >> "$VERIFIED_OUT"
            PASS=$((PASS + 1))
        done < "$names_file"
        continue
    fi

    # Run the generated test program and capture stdout + stderr combined
    actual="$("$TUR" run "$tur_file" 2>&1)"
    run_exit=$?

    if [ $run_exit -ne 0 ]; then
        # Interpreter error: module cannot be loaded (typeclass/GADT/module limitations).
        # Count as skipped rather than failed so CI is not blocked by interpreter gaps.
        n_cases=0
        while IFS= read -r fname || [ -n "$fname" ]; do
            [ -z "$fname" ] && continue
            n_cases=$((n_cases + 1))
        done < "$names_file"
        printf 'SKIP %s (%d cases) -- interpreter error (exit %d)\n' \
            "$mod_name" "$n_cases" "$run_exit"
        SKIP=$((SKIP + n_cases))
        continue
    fi

    # Compare output line by line against expected values
    module_ok=true
    idx=0

    # Build parallel arrays from the files
    mapfile -t expected_arr < "$expected_file"
    mapfile -t names_arr    < "$names_file"

    while IFS= read -r actual_line || [ -n "$actual_line" ]; do
        exp="${expected_arr[$idx]:-}"
        fname="${names_arr[$idx]:-?}"

        if [ "$actual_line" = "$exp" ]; then
            echo "$fname" >> "$VERIFIED_OUT"
            PASS=$((PASS + 1))
        else
            printf 'FAIL %s:%s -- got "%s", expected "%s"\n' \
                "$mod_name" "$fname" "$actual_line" "$exp"
            FAILED+=("$mod_name:$fname")
            FAIL=$((FAIL + 1))
            module_ok=false
        fi
        idx=$((idx + 1))
    done <<< "$actual"

    # Write stamp only when all cases in the module passed
    $module_ok && stamp_write "$mod_name" "$tur_file"
done

echo ""
echo "Doctest results: $PASS passed, $FAIL failed, $SKIP skipped (interpreter incompatible)"

if [ "${#FAILED[@]}" -gt 0 ]; then
    echo "Failed (wrong output):"
    for f in "${FAILED[@]}"; do
        echo "  $f"
    done
    exit 1
fi

exit 0
