#!/usr/bin/env bash
# FULL-corpus JIT sweep -- docs/archive/jit-engine-j0-findings.md section 8.4.2
#
# Runs every eligible fixture, in parallel, with no sampling.  This is the
# script that produced the 84.8% figure, and it exists because the stride
# sample in sweep-fixtures.sh cannot produce a trustworthy coverage number:
# tests/fixtures/ is alphabetical, so consecutive entries are near-duplicates
# and a stride draws correlated clusters.  Replaying the stride against these
# full results swings the answer 10.7 points by starting offset alone -- which
# is exactly how this document ended up with both an 89% and a 78%.
#
# Prefer this over sweep-fixtures.sh whenever a number is going to be quoted.
# ~9 minutes on 4 cores.
#
#   bash tools/jit-spike/sweep-full.sh                 # eager (default)
#   GENMODE= bash tools/jit-spike/sweep-full.sh        # lazy
#   OUT=/somewhere bash tools/jit-spike/sweep-full.sh
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

TUR="${TUR:-$ROOT/build/tur}"
SPIKE="${SPIKE:-$ROOT/build-jit/tools/jit-spike/tur-jit-spike}"
OUT="${OUT:-$ROOT/build-jit/spike-full}"
SHIM="${SHIM:-$ROOT/tools/jit-spike/subset-shim.h}"
NORMALIZE="$ROOT/tools/jit-spike/normalize-c11-subset.py"
# `${VAR-default}`, NOT `${VAR:-default}`: the colon form substitutes on EMPTY
# as well as unset, so `GENMODE= bash sweep-full.sh` would silently run eager
# and report a lazy result.  That happened once; it is not obvious from the
# output, because a bogus "lazy == eager, zero differences" reads as a finding.
GENMODE="${GENMODE---eager}"

[ -x "$TUR" ]   || { echo "no tur at $TUR"; exit 1; }
[ -x "$SPIKE" ] || { echo "no spike harness at $SPIKE (-DTUR_JIT_SPIKE=ON)"; exit 1; }

mkdir -p "$OUT/w"
: > "$OUT/results.tsv"

run_one() {
  local dir="$1" name
  name=$(basename "$dir")
  local W="$OUT/w"
  if ! timeout 30 "$TUR" emit-c "$dir/input.tur" > "$W/$name.c" 2>/dev/null; then
    printf '%s\temit-fail\t\n' "$name"; rm -f "$W/$name.c"; return
  fi
  python3 "$NORMALIZE" -I src/runtime "$W/$name.c" -o "$W/$name.subset.c" 2> "$W/$name.norm.err"
  local stdin_file=/dev/null
  [ -f "$dir/input.stdin" ] && stdin_file="$dir/input.stdin"
  timeout 60 "$SPIKE" -I src -I src/runtime -O 2 --quiet $GENMODE --shim "$SHIM" \
      "$W/$name.subset.c" < "$stdin_file" > "$W/$name.out" 2> "$W/$name.err"
  local rc=$?
  if [ "$rc" -ge 128 ]; then
    printf '%s\tFAIL\tsignal-%d\n' "$name" "$((rc - 128))"
  elif ! grep -qvE 'warning' "$W/$name.err"; then
    if diff -q "$W/$name.out" "$dir/expected.stdout" > /dev/null 2>&1; then
      printf '%s\tPASS\t\n' "$name"
    else
      printf '%s\toutput-mismatch\t\n' "$name"
    fi
  else
    local why
    why=$(grep -vE 'warning|c2mir FAILED' "$W/$name.err" | head -1 \
          | sed -E 's/^[^:]*:[0-9]+:[0-9]+: //' | cut -c1-80)
    [ -z "$why" ] && why=$(head -1 "$W/$name.err" | cut -c1-80)
    printf '%s\tFAIL\t%s\n' "$name" "$why"
  fi
  rm -f "$W/$name.c" "$W/$name.subset.c" "$W/$name.out"
}
export -f run_one
export OUT SPIKE SHIM GENMODE TUR NORMALIZE

# Same eligibility filter as sweep-fixtures.sh, minus the stride.
eligible=()
for dir in tests/fixtures/*/; do
  [ -f "$dir/input.tur" ] || continue
  [ -f "$dir/expected.stdout" ] || continue
  ls "$dir" | grep -qE '^(flags|args|requires\.|expected\.status|expected\.stderr)' && continue
  eligible+=("$dir")
done
echo "eligible: ${#eligible[@]}  mode: ${GENMODE:-lazy}"
# Corpus order matters for the offset analysis below; results.tsv is written by
# parallel workers and is therefore unordered, so keep the ordering separately.
printf '%s\n' "${eligible[@]}" > "$OUT/eligible.txt"

# `nproc` is coreutils and is absent on a stock macOS; `sysctl -n hw.ncpu` is
# the BSD spelling.  Fall back to 4 rather than to 1 -- a serial full sweep is
# ~35 minutes and reads as a hang.
jobs=$(command -v nproc > /dev/null && nproc \
       || sysctl -n hw.ncpu 2> /dev/null \
       || echo 4)
printf '%s\n' "${eligible[@]}" \
  | xargs -P "$jobs" -I{} bash -c 'run_one "$@"' _ {} >> "$OUT/results.tsv"

echo "--- outcome ---"
cut -f2 "$OUT/results.tsv" | sort | uniq -c | sort -rn
echo "--- failure reasons ---"
awk -F'\t' '$2=="FAIL"{print $3}' "$OUT/results.tsv" \
  | sed -E 's/[0-9]+/N/g' | sort | uniq -c | sort -rn | head -20
echo "--- total: $(wc -l < "$OUT/results.tsv") ---"

# The point of the whole exercise: show how far a stride sample of these same
# results can drift, so nobody quotes one again.
python3 - "$OUT" <<'PY'
import sys, collections
out = sys.argv[1]
order = [l.strip().rstrip('/').split('/')[-1] for l in open(out + '/eligible.txt')]
res = {}
for line in open(out + '/results.tsv'):
    f = line.rstrip('\n').split('\t')
    if len(f) >= 2:
        res[f[0]] = f[1]
rows = [res[n] for n in order if n in res]
n_pass = sum(1 for o in rows if o == 'PASS')
print('full corpus: %d / %d = %.1f%%' % (n_pass, len(rows), 100.0 * n_pass / len(rows)))
rates = []
for off in range(10):
    sub = rows[off::10]
    p = sum(1 for o in sub if o == 'PASS')
    rates.append(100.0 * p / len(sub))
print('stride-10 subsamples: ' + '  '.join('%.1f%%' % r for r in rates))
print('spread: %.1f%% .. %.1f%%  (%.1f points)'
      % (min(rates), max(rates), max(rates) - min(rates)))
PY
