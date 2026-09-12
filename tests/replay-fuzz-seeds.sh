#!/usr/bin/env bash
# tests/replay-fuzz-seeds.sh -- replay one harness's recorded fuzz seeds.
#
# Reads tests/fuzz-seed-corpus.txt and re-runs every seed recorded for the
# named harness at smoke size.  See that file's header for why, and
# docs/upcoming/type-confusion-detection-plan.md section F2.
#
# Usage: replay-fuzz-seeds.sh <harness> <driver.py> <tur> <n>
#
# Exits non-zero if any replayed seed fails.  An empty corpus exits 0 after
# saying so -- silence would be indistinguishable from "the file went
# missing", which is the failure mode this project keeps re-learning.
set -uo pipefail
cd "$(dirname "$0")/.."

HARNESS="$1"; DRIVER="$2"; TUR="$3"; N="$4"
CORPUS="tests/fuzz-seed-corpus.txt"

if [ ! -f "$CORPUS" ]; then
  echo "  replay-fuzz-seeds: $CORPUS is missing -- recorded seeds are not being replayed" >&2
  exit 1
fi

rc=0
found=0
while read -r h seed _rest; do
  case "$h" in ''|'#'*) continue ;; esac
  [ "$h" = "$HARNESS" ] || continue
  case "$seed" in ''|*[!0-9]*)
    echo "  replay-fuzz-seeds: $HARNESS: ignoring non-numeric seed '$seed'" >&2
    continue ;;
  esac
  found=$((found + 1))
  echo "  replaying $HARNESS seed $seed"
  python3 "$DRIVER" --tur "$TUR" --n "$N" --seed "$seed" || rc=1
done < "$CORPUS"

if [ "$found" -eq 0 ]; then
  echo "  replay-fuzz-seeds: no recorded seeds for '$HARNESS' (corpus is empty)"
fi
exit $rc
