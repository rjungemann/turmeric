#!/usr/bin/env bash
# check-reported-index.sh -- docs/reported/README.md must list every open
# finding, and must list nothing else.
#
# The index exists so a triage pass reads one file instead of two dozen, which
# only works if it is true. It has drifted in both directions:
#
#   - 2026-08-20: four reports were resolved and archived, but their rows were
#     left behind, so triage read four open findings that were not open. Two of
#     those archives were themselves hiding live work.
#   - mir-aarch64-fp-aggregate-abi (severity HIGH) sat in docs/reported/ with no
#     row at all from the day it was filed.
#
# Both are cheap to catch and expensive to miss, so they are checked rather than
# remembered. This is a lint over prose, not a gate on the compiler: it can only
# fail on a docs edit.
set -u

cd "$(dirname "$0")/.."

INDEX=docs/reported/README.md
fail=0

if [ ! -f "$INDEX" ]; then
    echo "FAIL check-reported-index -- $INDEX is missing"
    exit 1
fi

# Every open report needs a link from the index. Match the bare filename in a
# markdown link target: the index links siblings as `(slug.md)` and archived
# reports as `(../archive/slug.md)`, so anchor on the opening paren.
for f in docs/reported/*.md; do
    b=$(basename "$f")
    [ "$b" = "README.md" ] && continue
    if ! grep -qF "($b)" "$INDEX"; then
        echo "FAIL check-reported-index -- docs/reported/$b has no row in $INDEX"
        fail=1
    fi
done

# Every sibling link in the index needs a file behind it. A link with a path
# separator points outside this directory (../archive/...) and is not checked.
links=$(grep -o ']([A-Za-z0-9._-]*\.md)' "$INDEX" | sed 's/^](//; s/)$//' | sort -u)
for l in $links; do
    [ "$l" = "README.md" ] && continue
    if [ ! -f "docs/reported/$l" ]; then
        if [ -f "docs/archive/$l" ]; then
            echo "FAIL check-reported-index -- $INDEX links $l, which was archived to docs/archive/$l"
            echo "     Remove the row and leave a resolution note in its place."
        else
            echo "FAIL check-reported-index -- $INDEX links $l, which does not exist"
        fi
        fail=1
    fi
done

# docs/reported/history/ is forbidden (see CLAUDE.md); the PreToolUse hook
# blocks creating one, but a clone that predates the hook could carry one.
if [ -d docs/reported/history ]; then
    echo "FAIL check-reported-index -- docs/reported/history/ exists; it is forbidden."
    echo "     Resolved reports go to docs/archive/, their paper trail to docs/archive/history/."
    fail=1
fi

if [ "$fail" -ne 0 ]; then
    exit 1
fi

n=$(ls docs/reported/*.md | grep -cv 'README.md$')
echo "PASS check-reported-index ($n open reports, all indexed)"
