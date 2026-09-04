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

set -u
TMP="$1"
TUR="${TUR:-./build/tur}"
SRC="tests/fixtures/refine-model-vars-cap/input.tur"

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
