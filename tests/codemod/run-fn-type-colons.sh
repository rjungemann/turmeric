#!/usr/bin/env bash
# Runner for tools/rewrite_fn_type_colons.py against the test corpus.
#
# Each fixture directory under tests/codemod/fn-type-colons/ contains:
#   before.tur   -- input
#   after.tur    -- expected output (byte-for-byte)
#
# The script copies before.tur to a temp file, runs the rewriter, and
# diffs against after.tur.

set -u

cd "$(dirname "$0")/../.."

CORPUS_DIR="tests/codemod/fn-type-colons"
TOOL="tools/rewrite_fn_type_colons.py"

[ -d "$CORPUS_DIR" ] || { echo "missing $CORPUS_DIR" >&2; exit 2; }
[ -f "$TOOL" ]       || { echo "missing $TOOL"       >&2; exit 2; }

pass=0
fail=0
failed=()

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

for dir in "$CORPUS_DIR"/*/; do
    name="$(basename "$dir")"
    before="$dir/before.tur"
    after="$dir/after.tur"
    if [ ! -f "$before" ] || [ ! -f "$after" ]; then
        echo "SKIP $name (missing before.tur or after.tur)"
        continue
    fi
    work="$tmp/$name.tur"
    cp "$before" "$work"
    if ! python3 "$TOOL" --write "$work" >/dev/null 2>"$tmp/$name.err"; then
        echo "FAIL $name -- rewriter exited non-zero"
        cat "$tmp/$name.err"
        fail=$((fail + 1))
        failed+=("$name")
        continue
    fi
    if diff -u "$after" "$work" >/dev/null 2>&1; then
        echo "PASS $name"
        pass=$((pass + 1))
    else
        echo "FAIL $name -- output diverges from after.tur"
        diff -u "$after" "$work"
        fail=$((fail + 1))
        failed+=("$name")
    fi
done

echo
echo "summary: $pass passed, $fail failed"
if [ "$fail" -gt 0 ]; then
    printf '  - %s\n' "${failed[@]}"
    exit 1
fi
exit 0
