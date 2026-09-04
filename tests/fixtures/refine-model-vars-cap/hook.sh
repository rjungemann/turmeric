#!/usr/bin/env bash
# MODEL_MAX_VARS telemetry: the cap that costs a REFUTATION, not a proof.
#
# The bounded counterexample search is the only thing in the solver allowed to
# answer INVALID, and it declines any VC with more than MODEL_MAX_VARS (3)
# variables.  So a four-parameter function with a plainly false return
# refinement stays `unknown` -- silent, runtime check kept -- where the same
# function with three parameters would report TUR-E0371 with a witness.
#
# That was invisible until 2026-09-04.  This pins the instrument rather than
# the cap: the value may well be raised on evidence one day, but a hit must
# always SAY so, and the `model vars run` subset must always separate "the cap
# turned this away" from "raising it would actually help" -- the second is the
# only number a raise can be argued from, since a VC over the cap may also
# carry a non-int variable the sort gate declines at any limit.
#
# The subject is written into $TMP rather than kept as a sibling `input.tur`,
# which is the sx8a-tur-smt / sx8b-smt-push-pop pattern and is load-bearing:
# tests/run-turi.sh discovers a fixture by its `input.tur` and runs it as a
# PROGRAM under --interpret, comparing the program's stdout against
# expected.stdout.  For a hook fixture whose expected.stdout is CLI telemetry
# rather than program output, that is a guaranteed mismatch -- which is exactly
# how this fixture failed CI the first time.  A `requires.compiled` marker would
# also have silenced it, but it would leave a directory that looks like an
# ordinary compiled fixture while meaning something else.

set -u
TMP="$1"
TUR="${TUR:-./build/tur}"

SRC="$TMP/model-vars.tur"
cat > "$SRC" <<'EOF'
;; Four integer variables and an obviously false goal: the search would find a
;; counterexample instantly at a higher cap, and declines outright at 3.
(defn f [a : int b : int c : int d : int] : #refine{r : int | (> r 1000)}
  (+ a (+ b (+ c d))))

(defn main [] : int (f 1 1 1 1))
EOF

out=$(TUR_REFINE_STATS=1 "$TUR" check "$SRC" 2>&1)

# The obligation is not decided: no proof, and no refutation either.
echo "$out" | grep -o '1 unknown' | head -1

# The cap says so, with the real width rather than a saturated one.
echo "$out" | sed -n 's/^refine:  *model vars  *peak  *\([0-9]*\) \/ \([0-9]*\).*\*\* HIT/model vars: peak=\1 limit=\2 HIT/p'

# And the subset a raise would actually help is reported separately.
echo "$out" | sed -n 's/^refine:  *model vars run  *\([0-9]*\) (of \([0-9]*\) over the cap)/would run: \1 of \2/p'

# The cap is a completeness limit, never a soundness one: the check survives.
echo "$out" | grep -c 'TUR-E0371' | sed 's/^/E0371 count: /'
exit 0
