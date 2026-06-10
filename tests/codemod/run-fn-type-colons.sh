#!/usr/bin/env bash
# Runner for tools/rewrite_fn_type_colons.py against the test corpus.
#
# Each fixture directory under tests/codemod/fn-type-colons/ contains a
# before/after pair sharing one of these extensions:
#   before.tur        / after.tur        -- s-expression source
#   before.tur.sweet  / after.tur.sweet  -- sweet-exp source
#   before.md         / after.md         -- markdown with fenced blocks
#
# The script copies the before file to a temp file (preserving the
# extension so the tool routes it correctly), runs the rewriter, and diffs
# against the after file.  Idempotency is checked by re-running the tool on
# the output and asserting no further change.

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
    # Locate the before.* file and its matching extension.
    before=""
    ext=""
    for cand in "$dir"before.tur.sweet "$dir"before.tur "$dir"before.md; do
        if [ -f "$cand" ]; then
            before="$cand"
            ext="${cand#"$dir"before}"
            break
        fi
    done
    if [ -z "$before" ]; then
        echo "SKIP $name (no before.* file)"
        continue
    fi
    after="$dir/after$ext"
    if [ ! -f "$after" ]; then
        echo "SKIP $name (missing after$ext)"
        continue
    fi
    work="$tmp/$name$ext"
    cp "$before" "$work"
    if ! python3 "$TOOL" --write "$work" >/dev/null 2>"$tmp/$name.err"; then
        echo "FAIL $name -- rewriter exited non-zero"
        cat "$tmp/$name.err"
        fail=$((fail + 1))
        failed+=("$name")
        continue
    fi
    if ! diff -u "$after" "$work" >/dev/null 2>&1; then
        echo "FAIL $name -- output diverges from after$ext"
        diff -u "$after" "$work"
        fail=$((fail + 1))
        failed+=("$name")
        continue
    fi
    # Idempotency: re-running on the output must be a no-op.
    if ! python3 "$TOOL" --check "$work" >/dev/null 2>&1; then
        echo "FAIL $name -- not idempotent (--check flagged after$ext)"
        fail=$((fail + 1))
        failed+=("$name")
        continue
    fi
    echo "PASS $name"
    pass=$((pass + 1))
done

echo
echo "summary: $pass passed, $fail failed"
if [ "$fail" -gt 0 ]; then
    printf '  - %s\n' "${failed[@]}"
    exit 1
fi
exit 0
