#!/usr/bin/env bash
# Indicative J0 coverage sample -- docs/upcoming/jit-engine-plan.md
#
# NOT the J3 parity sweep.  This walks an evenly-spaced sample of the fixture
# corpus through emit-c -> normalize -> c2mir -> MIR-gen -> run and tallies why
# things fail, so the J0 findings doc can say what fraction of real programs the
# engine already executes and which constructs account for the rest.  J3 wires
# a proper harness worker and runs everything.
#
#   bash tools/jit-spike/sweep-fixtures.sh [sample-size]   # default 200
#
# Env overrides: TUR, SPIKE, OUT, OPT.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

TUR="${TUR:-$ROOT/build/tur}"
SPIKE="${SPIKE:-$ROOT/build-jit/tools/jit-spike/tur-jit-spike}"
OUT="${OUT:-$ROOT/build-jit/spike-sweep}"
OPT="${OPT:-2}"
NORMALIZE="$ROOT/tools/jit-spike/normalize-c11-subset.py"
SHIM="${SHIM:-$ROOT/tools/jit-spike/subset-shim.h}"
SAMPLE="${1:-200}"

[ -x "$TUR" ]   || { echo "no tur at $TUR"; exit 1; }
[ -x "$SPIKE" ] || { echo "no spike harness at $SPIKE"; exit 1; }

mkdir -p "$OUT"
: > "$OUT/results.tsv"

total=$(ls -d tests/fixtures/*/ | wc -l)
stride=$(( total / SAMPLE + 1 ))
count=0

for dir in tests/fixtures/*/; do
  name=$(basename "$dir")
  [ -f "$dir/input.tur" ] || continue
  [ -f "$dir/expected.stdout" ] || continue
  # Fixtures needing CLI flags, args, skip markers, or a stderr/status contract
  # are out of sample scope -- the spike harness has no equivalent plumbing.
  ls "$dir" | grep -qE '^(flags|args|requires\.|expected\.status|expected\.stderr)' && continue
  count=$((count + 1))
  (( count % stride == 0 )) || continue

  if ! timeout 30 "$TUR" emit-c "$dir/input.tur" > "$OUT/$name.c" 2>/dev/null; then
    printf '%s\temit-fail\t\n' "$name" >> "$OUT/results.tsv"
    continue
  fi
  python3 "$NORMALIZE" -I src/runtime "$OUT/$name.c" -o "$OUT/$name.subset.c" \
      2> "$OUT/$name.norm.err"

  stdin_file=/dev/null
  [ -f "$dir/input.stdin" ] && stdin_file="$dir/input.stdin"
  timeout 60 "$SPIKE" -I src -I src/runtime -O "$OPT" --quiet --shim "$SHIM" \
      "$OUT/$name.subset.c" < "$stdin_file" \
      > "$OUT/$name.stdout" 2> "$OUT/$name.err"
  rc=$?

  # Only stdout is the contract (same as tests/run.sh); a fixture's program may
  # legitimately exit non-zero.  rc >= 128 means a signal, which never is.
  if [ "$rc" -ge 128 ]; then
    printf '%s\tFAIL\tsignal-%d\n' "$name" "$((rc - 128))" >> "$OUT/results.tsv"
  elif ! grep -qvE 'warning' "$OUT/$name.err"; then
    if diff -q "$OUT/$name.stdout" "$dir/expected.stdout" > /dev/null 2>&1; then
      printf '%s\tPASS\t\n' "$name" >> "$OUT/results.tsv"
    else
      printf '%s\toutput-mismatch\t\n' "$name" >> "$OUT/results.tsv"
    fi
  else
    why=$(grep -vE 'warning|^tur-jit-spike: c2mir failed' "$OUT/$name.err" \
          | head -1 | sed -E 's/^[^:]*:[0-9]+:[0-9]+: //' | cut -c1-90)
    [ -z "$why" ] && why=$(head -1 "$OUT/$name.err" | cut -c1-90)
    printf '%s\tFAIL\t%s\n' "$name" "$why" >> "$OUT/results.tsv"
  fi
  rm -f "$OUT/$name.c" "$OUT/$name.subset.c"
done

echo "--- outcome ---"
cut -f2 "$OUT/results.tsv" | sort | uniq -c | sort -rn
echo "--- failure reasons ---"
awk -F'\t' '$2=="FAIL"{print $3}' "$OUT/results.tsv" \
  | sed -E 's/[0-9]+/N/g' | sort | uniq -c | sort -rn | head -25
echo "--- sampled: $(wc -l < "$OUT/results.tsv") of $count eligible fixtures ---"
