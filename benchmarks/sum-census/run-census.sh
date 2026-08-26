#!/usr/bin/env bash
# SR0(a): how often are small sums actually CONSTRUCTED at runtime?
#
# For each fixture: emit C, inject per-ctor counters (instrument.py), compile,
# run, and collect `fixture<TAB>ctor<TAB>count<TAB>repr`.  Fixtures that do not
# build or do not run are skipped silently -- this is a census over what
# executes, and a fixture that never runs contributes nothing either way.
#
# Usage: bash benchmarks/sum-census/run-census.sh [out.tsv] [limit]
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
TUR="${TUR:-$ROOT/build/tur}"
OUT="${1:-$ROOT/benchmarks/sum-census/census.tsv}"
LIMIT="${2:-0}"
WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
: > "$OUT"

# Skip reasons are recorded, not swallowed: a census over an unknown fraction
# of the corpus is not evidence.  Each fixture appends one line to $WORK/skips.
one() {
    local input="$1" name="$2" w="$WORK/$$-$RANDOM"
    mkdir -p "$w"
    if ! timeout 30 "$TUR" emit-c "$input" > "$w/a.c" 2>/dev/null; then
        echo "emit-c" >> "$WORK/skips"; return 0; fi
    if ! python3 "$ROOT/benchmarks/sum-census/instrument.py" "$w/a.c" "$w/b.c" 2>/dev/null; then
        echo "instrument" >> "$WORK/skips"; return 0; fi
    if ! timeout 60 cc -O2 -std=c99 -w -fno-strict-aliasing -L"$ROOT/build/src" \
        "$w/b.c" -lturi -lm -lpthread -o "$w/bin" 2>/dev/null; then
        echo "cc" >> "$WORK/skips"; return 0; fi
    TUR_CENSUS_OUT="$w/c.tsv" timeout 20 "$w/bin" >/dev/null 2>&1 || true
    if [ ! -s "$w/c.tsv" ]; then
        echo "no-ctor-calls" >> "$WORK/skips"; return 0; fi
    echo "ok" >> "$WORK/skips"
    sed "s|^|$name\t|" "$w/c.tsv"
}
export -f one; export WORK ROOT TUR

# Fixtures are minimal by construction (median 2 constructions each), so they
# measure breadth well and depth badly.  examples/ holds actual programs --
# minikanren, datalog, guestbook, snake -- and is the better workload proxy;
# census both and report them separately.
list=$(find "$ROOT/tests/fixtures" -name input.tur -not -path "*/errors/*" | sort)
list="$list
$(find "$ROOT/examples" -name '*.tur' | sort)"
[ "$LIMIT" -gt 0 ] && list=$(printf '%s\n' "$list" | head -"$LIMIT")
printf '%s\n' "$list" | while read -r f; do
    printf '%s\t%s\n' "$f" "$(basename "$(dirname "$f")")"
done | xargs -P "$(nproc)" -n2 bash -c 'one "$0" "$1"' >> "$OUT"

echo "census rows: $(wc -l < "$OUT")"
echo "fixtures contributing: $(cut -f1 "$OUT" | sort -u | wc -l)"
echo "--- coverage (why each fixture did or did not contribute) ---"
sort "$WORK/skips" | uniq -c | sort -rn
