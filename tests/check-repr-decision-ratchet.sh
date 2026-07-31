#!/usr/bin/env bash
# tests/check-repr-decision-ratchet.sh -- increment 4 stage 4
# (docs/upcoming/repr-decision-function-plan.md).
#
# Every bug in the representation-consolidation campaign's archive is two
# sites re-deriving the same representation decision and disagreeing.  The
# campaign consolidated the *existing* sites (shared predicates, repr_of,
# the shadow log driven to corpus silence); this ratchet keeps NEW code
# from quietly reopening the seam.
#
# It pins the per-file call-site count of each representation-DECISION
# predicate against tests/repr-decision-baseline.txt:
#
#   - A count INCREASE fails: a new site is re-deriving a representation
#     decision inline.  Either consult `repr_of` / the appropriate shared
#     predicate at a chokepoint instead, or -- if the new site genuinely is
#     a new chokepoint -- update the baseline IN THE SAME COMMIT, with the
#     reasoning in the commit message.
#   - A count DECREASE passes with a note: tighten the baseline in the
#     same spirit (a consolidation should bank its progress).
#
# Regenerate the baseline: bash tests/check-repr-decision-ratchet.sh --update
#
# Source-text check, no build needed.  Exit 0 = counts within baseline.

set -uo pipefail
cd "$(dirname "$0")/.."

BASELINE=tests/repr-decision-baseline.txt
PREDICATES="type_uses_carrier_abi type_is_wide_byval_adt type_is_boxed_container_elem fn_param_type_is_fat_normalized type_has_concrete_codegen_layout"

census() {
  for p in $PREDICATES; do
    for f in src/compiler/*.c; do
      # Count call sites: the identifier followed by '(' -- skips the
      # definition line too few times to matter (definitions are 1 per
      # predicate and stable; they are part of the pinned count).
      n=$(grep -o "${p}(" "$f" | wc -l | tr -d ' ')
      [ "$n" != "0" ] && echo "$p $(basename $f) $n"
    done
  done
}

if [ "${1:-}" = "--update" ]; then
  census > "$BASELINE"
  echo "repr-decision-ratchet: baseline updated ($(wc -l < "$BASELINE" | tr -d ' ') rows)"
  exit 0
fi

if [ ! -f "$BASELINE" ]; then
  echo "FAIL repr-decision-ratchet: no baseline at $BASELINE (run with --update)"
  exit 1
fi

rc=0
current="$(census)"
# Increases: rows in current above baseline.
while read -r p f n; do
  [ -z "$p" ] && continue
  base=$(awk -v p="$p" -v f="$f" '$1==p && $2==f {print $3}' "$BASELINE")
  base=${base:-0}
  if [ "$n" -gt "$base" ]; then
    echo "FAIL repr-decision-ratchet: $p in $f grew $base -> $n"
    rc=1
  elif [ "$n" -lt "$base" ]; then
    echo "note repr-decision-ratchet: $p in $f shrank $base -> $n (tighten the baseline)"
  fi
done <<< "$current"
# Files that vanished entirely from the census are decreases; fine.

if [ $rc -ne 0 ]; then
  echo ""
  echo "A representation decision is being re-derived at a new site."
  echo "Prefer consulting repr_of(type, position) or the shared predicate"
  echo "at an existing chokepoint (see docs/upcoming/repr-decision-function-plan.md"
  echo "and docs/guides/value-representations-guide.md).  If the new site IS a"
  echo "deliberate new chokepoint, update tests/repr-decision-baseline.txt in"
  echo "the same commit:  bash tests/check-repr-decision-ratchet.sh --update"
else
  echo "PASS repr-decision-ratchet ($(echo "$current" | wc -l | tr -d ' ') pinned rows)"
fi
exit $rc
