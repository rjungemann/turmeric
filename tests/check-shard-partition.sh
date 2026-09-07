#!/usr/bin/env bash
# check-shard-partition.sh -- the shards must be an exact partition of the suite.
#
# tests/run.sh can split the corpus with TUR_TEST_SHARD="i/n", and CI uses it:
# the Windows leg runs three shards because one runner could not finish the
# suite (28-52 min, and on 2026-09-06 a runner died mid-run).  That only works
# if the shards together are EXACTLY the unsharded selection.
#
# The obvious check -- compare fixture counts -- cannot establish that.  A run
# that drops two fixtures and double-counts three has a HIGHER total than the
# truth, so "the union is not smaller" says nothing about what is missing.  A
# count is an aggregate, and an aggregate cannot prove set equality.  So this
# compares NAMES:
#
#   missing    in the unsharded selection, in no shard   -- coverage lost
#   extra      in a shard, not in the unsharded run      -- shouldn't be possible
#   duplicate  in more than one shard                    -- work done twice
#
# Only `missing` is a correctness hole; the other two are waste, and are
# reported because they mean the shard arithmetic is not doing what it claims.
#
# Cheap by construction: TUR_TEST_LIST=1 prints the selection and exits without
# running a fixture, so this is a few seconds regardless of corpus size.
set -u
cd "$(dirname "$0")/.."

TUR="${TUR:-./build/tur}"
[ -x "$TUR" ] || { echo "tests: $TUR not built; run 'just build' first" >&2; exit 2; }

WORK="$(mktemp -d -t tur-shard-check-XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

fail=0

# `FIXTURE <name>` only: the harness prints ratchet/parity status lines on the
# same stream, and an untagged read counted four of them as fixtures.
list_fixtures() {
    TUR="$TUR" TUR_TEST_LIST=1 bash tests/run.sh | sed -n 's/^FIXTURE //p'
}

list_fixtures | LC_ALL=C sort > "$WORK/full"
full_n=$(wc -l < "$WORK/full" | tr -d '[:space:]')
if [ "$full_n" -eq 0 ]; then
    echo "FAIL check-shard-partition -- the unsharded selection is empty"
    exit 1
fi

# Several shard counts: an off-by-one in the index arithmetic can partition
# correctly for one n and not another (n=1 exercises the disabled path, and an
# n that does not divide the corpus exercises the uneven remainder).
for n in 1 2 3 4 7; do
    : > "$WORK/union"
    for i in $(seq 1 "$n"); do
        TUR_TEST_SHARD="$i/$n" list_fixtures >> "$WORK/union"
    done
    LC_ALL=C sort "$WORK/union" > "$WORK/union.sorted"
    LC_ALL=C sort -u "$WORK/union" > "$WORK/union.uniq"

    missing=$(LC_ALL=C comm -23 "$WORK/full" "$WORK/union.uniq" | wc -l | tr -d '[:space:]')
    extra=$(LC_ALL=C comm -13 "$WORK/full" "$WORK/union.uniq" | wc -l | tr -d '[:space:]')
    dupes=$(LC_ALL=C uniq -d "$WORK/union.sorted" | wc -l | tr -d '[:space:]')

    if [ "$missing" -eq 0 ] && [ "$extra" -eq 0 ] && [ "$dupes" -eq 0 ]; then
        echo "PASS check-shard-partition (n=$n, $full_n fixtures, exact partition)"
        continue
    fi

    fail=1
    echo "FAIL check-shard-partition -- n=$n is not an exact partition"
    if [ "$missing" -gt 0 ]; then
        echo "  $missing fixture(s) in NO shard -- coverage would be silently lost:"
        LC_ALL=C comm -23 "$WORK/full" "$WORK/union.uniq" | sed 's/^/    /' | head -20
    fi
    if [ "$extra" -gt 0 ]; then
        echo "  $extra fixture(s) in a shard but not in the unsharded selection:"
        LC_ALL=C comm -13 "$WORK/full" "$WORK/union.uniq" | sed 's/^/    /' | head -20
    fi
    if [ "$dupes" -gt 0 ]; then
        echo "  $dupes fixture(s) in more than one shard -- run twice, counted twice:"
        LC_ALL=C uniq -d "$WORK/union.sorted" | sed 's/^/    /' | head -20
    fi
done

exit "$fail"
