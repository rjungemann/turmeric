#!/usr/bin/env bash
# The counterexample search's two caps: the cap that costs a REFUTATION, not a
# proof.
#
# The bounded counterexample search is the only thing in the solver allowed to
# answer INVALID.  It declines a VC two ways: wider than MODEL_MAX_VARS (8), or
# with `n_cand ** n_vars` over the MODEL_MAX_EVALS budget (131072).  Either way
# a plainly false return refinement stays `unknown` -- silent, runtime check
# kept -- where a narrower function reports TUR-E0371 with a witness.
#
# Until 2026-09-05 the width cap was 3, and a FOUR-parameter function tripped
# it; that shape is now refuted (see errors/refine-model-search-four-vars) and
# the two subjects here sit past the raised limits instead.  This pins the
# INSTRUMENT rather than the values: a hit must always SAY so, the `model vars
# run` subset must separate "the cap turned this away" from "raising it would
# actually help", and the budget decline must be counted on its own row.
#
# The subjects are written into $TMP rather than kept as a sibling `input.tur`,
# which is the sx8a-tur-smt / sx8b-smt-push-pop pattern and is load-bearing:
# tests/run-turi.sh discovers a fixture by its `input.tur` and runs it as a
# PROGRAM under --interpret, comparing the program's stdout against
# expected.stdout.  For a hook fixture whose expected.stdout is CLI telemetry
# rather than program output, that is a guaranteed mismatch.

set -u
TMP="$1"
TUR="${TUR:-./build/tur}"

# --- 1. width: nine integer variables, over MODEL_MAX_VARS ------------------
SRC="$TMP/model-vars.tur"
cat > "$SRC" <<'EOT'
(defn f [a : int b : int c : int d : int e : int g : int h : int i : int j : int]
        : #refine{r : int | (> r 1000)}
  (+ a (+ b (+ c (+ d (+ e (+ g (+ h (+ i j)))))))))

(defn main [] : int (f 1 1 1 1 1 1 1 1 1))
EOT

out=$(TUR_REFINE_STATS=1 "$TUR" check "$SRC" 2>&1)

# The obligation is not decided: no proof, and no refutation either.
echo "$out" | grep -o '1 unknown' | head -1

# The cap says so, with the real width rather than a saturated one.
echo "$out" | sed -n 's/^refine:  *model vars  *peak  *\([0-9]*\) \/ \([0-9]*\).*\*\* HIT/model vars: peak=\1 limit=\2 HIT/p'

# And the subset a raise would actually help is reported separately.
echo "$out" | sed -n 's/^refine:  *model vars run  *\([0-9]*\) (of \([0-9]*\) over the cap)/would run: \1 of \2/p'

# The cap is a completeness limit, never a soundness one: the check survives.
echo "$out" | grep -c 'TUR-E0371' | sed 's/^/E0371 count: /'

# --- 2. budget: six variables inside the width cap, but the literal 1000 --
# widens the candidate set to 8 values and 8**6 = 262144 evaluations is over
# the budget, so the search declines on the OTHER row.
SRC2="$TMP/model-evals.tur"
cat > "$SRC2" <<'EOT'
(defn g [a : int b : int c : int d : int e : int f : int]
        : #refine{r : int | (> r 1000)}
  (+ a (+ b (+ c (+ d (+ e f))))))

(defn main [] : int (g 1 1 1 1 1 1))
EOT

out2=$(TUR_REFINE_STATS=1 "$TUR" check "$SRC2" 2>&1)
echo "$out2" | grep -o '1 unknown' | head -1
# Inside the width cap: no HIT on the model vars row ...
echo "$out2" | sed -n 's/^refine:  *model vars  *peak  *\([0-9]*\) \/ \([0-9]*\) *$/model vars: peak=\1 limit=\2 no hit/p'
# ... and one on the budget row.
echo "$out2" | sed -n 's/^refine:  *model evals out  *\([0-9]*\) (budget \([0-9]*\) evaluations)/evals out: \1 budget=\2/p'
echo "$out2" | grep -c 'TUR-E0371' | sed 's/^/E0371 count: /'
exit 0
