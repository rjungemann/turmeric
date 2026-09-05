#!/usr/bin/env bash
# RM3 R2 parity gate (docs/upcoming/regions-plan.md).
#
# With `--enable=regions` and NO region open, every emitted spine constructor
# routes through tur_region_alloc_or_malloc, which falls back to malloc.  So
# the flag must change nothing observable until a region is actually pushed --
# that is the property R2 is built to have, and the one a later increment is
# most likely to break by accident.
#
# Asserts OUTPUT equality, not that both arms build: a routing bug that
# returned uninitialised memory would still link.
set -u
cd "$(dirname "$0")/.."
TUR="${TUR:-./build/tur}"
[ -x "$TUR" ] || { echo "run-regions-seam: $TUR not built" >&2; exit 2; }

# Fixtures whose emitted C contains a `:heap` ADT monomorph ctor -- the spine
# node R2 routes.  Kept explicit rather than swept: this gate is about the
# routing, and a fixture with no spine tests nothing.
FIXTURES="
refined-nonempty
constrained-defn-cons-return-monomorphize
zipper-basic
re-string
option-niche-crossings
hkt-stdlib-result-ok-biased

"

# `region-scope-adt-result` is deliberately NOT listed: it is hook-driven (no
# input.tur for this harness to run) and its hook already asserts both halves
# of what this gate checks -- on/off output equality AND, which this gate
# cannot see, that a region was opened at all.
#
# R4: fixtures that actually OPEN a region -- every `bt-scope` is a boundary
# now, so these run push / (checked) pop for real, and the spine nodes their
# bodies allocate land in the generation rather than on the heap.  The
# assertion is the same one and it is the stronger one here: a rewind that
# reclaimed something still live would print a wrong answer (or trap on the
# Debug poison), not merely allocate differently.
FIXTURES="$FIXTURES
sx2-trail-combinators
sx2-dfs-driver
self-recursive-goal-into-fat-sink
region-scope-value-survives
region-scope-escape-refused
region-scope-void-body
"
pass=0; fail=0; skip=0
for name in $FIXTURES; do
    d="tests/fixtures/$name"; in="$d/input.tur"
    [ -f "$in" ] || { echo "SKIP $name (absent)"; skip=$((skip+1)); continue; }
    off=$("$TUR" run "$in" 2>/dev/null)
    on=$("$TUR" run --enable=regions "$in" 2>/dev/null)
    if [ -z "$off" ] && [ -z "$on" ]; then
        echo "SKIP $name (no output either way)"; skip=$((skip+1)); continue
    fi
    if [ "$off" = "$on" ]; then
        echo "PASS $name"; pass=$((pass+1))
    else
        echo "FAIL $name -- output differs with --enable=regions"
        diff <(printf '%s\n' "$off") <(printf '%s\n' "$on") | head -10
        fail=$((fail+1))
    fi
done
echo
echo "regions-seam summary: $pass passed, $fail failed, $skip skipped"
[ "$fail" -eq 0 ]
