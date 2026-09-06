#!/usr/bin/env bash
# solver-integer-tail-plan Phase 3(b): SMT-LIB `div` / `mod` are EUCLIDEAN
# (0 <= mod < |n|, the quotient absorbing the divisor's sign); the VC's
# VC_DIV / VC_MOD are C's TRUNCATING pair, which is what the compiler's `/`
# and `mod` lower to and what the bounded model search evaluates.  The
# SMT-LIB seam has to translate at BOTH ends, and this fixture pins both:
#
#   reader     `tur smt` builds the Euclidean value from the truncating pair
#              plus an ite (tr_euclid_divmod).  Before: `(< (mod x 3) 0)`, which
#              has no model in SMT-LIB, came back `sat` with x = -2.
#   serializer the compiler's `(/ n 2)` obligation, dumped as SMT-LIB, must
#              spell the TRUNCATING value in Euclidean terms -- pinned by
#              feeding the dump straight back into `tur smt`, which must prove
#              it (unsat), not merely not-refute it.
#
# Every case reads its VALUE (the model's binding, the exact answer), never
# just "it ran": the pre-fix reader ran all of these and answered wrong.
set -u
TMP="$1"
TUR="${TUR:-./build/tur}"

# --- sat: a negative dividend still has a NON-NEGATIVE remainder -------------
cat > "$TMP/neg_dividend.smt2" <<'EOF'
(set-logic QF_LIA)
(declare-fun x () Int)
(assert (= (mod x 2) 1))
(assert (< x 0))
(check-sat)
EOF
# --- unsat: a Euclidean remainder is never negative (was: sat, x = -2) -------
cat > "$TMP/nonneg_rem.smt2" <<'EOF'
(set-logic QF_LIA)
(declare-fun x () Int)
(assert (< (mod x 3) 0))
(check-sat)
EOF
# --- unsat, folded: (div -7 2) is -4, the truncating -3 is a contradiction ---
cat > "$TMP/trunc_quotient.smt2" <<'EOF'
(set-logic QF_LIA)
(assert (= (div (- 7) 2) (- 3)))
(check-sat)
EOF
# --- sat: a negative DIVISOR -- 7 = (-2)(-3) + 1 -----------------------------
cat > "$TMP/neg_divisor.smt2" <<'EOF'
(set-logic QF_LIA)
(declare-fun x () Int)
(assert (= x 7))
(assert (= (div x (- 2)) (- 3)))
(assert (= (mod x (- 2)) 1))
(check-sat)
EOF
# --- unsat, needs the axioms AND the integer equality phase ------------------
cat > "$TMP/parity.smt2" <<'EOF'
(set-logic QF_LIA)
(declare-fun x () Int)
(assert (= (mod x 2) 0))
(assert (= (mod (+ x 1) 2) 0))
(check-sat)
EOF
# --- refused whole: a non-literal divisor is nonlinear and has no axiom ------
# (QF_LIA on purpose: the refusal must come from the divisor, not the logic.)
cat > "$TMP/var_divisor.smt2" <<'EOF'
(set-logic QF_LIA)
(declare-fun x () Int)
(declare-fun y () Int)
(assert (= (div x y) 3))
(check-sat)
EOF
# --- refused whole: SMT-LIB leaves division by zero uninterpreted ------------
cat > "$TMP/by_zero.smt2" <<'EOF'
(set-logic QF_LIA)
(declare-fun x () Int)
(assert (= (mod x 0) 1))
(check-sat)
EOF

for case in neg_dividend nonneg_rem trunc_quotient neg_divisor parity var_divisor by_zero; do
    out=$("$TUR" smt "$TMP/$case.smt2" 2>"$TMP/$case.err")
    rc=$?
    echo "$case: answer=$(printf '%s' "$out" | head -1) exit=$rc"
done

# The witnesses themselves -- the whole defect was a wrong model.
"$TUR" smt "$TMP/neg_dividend.smt2" 2>/dev/null | grep -o 'define-fun x () Int -[0-9]*' \
    | sed 's/^/neg_dividend model: /; s/-[0-9]*$/negative/'
"$TUR" smt "$TMP/neg_divisor.smt2" 2>/dev/null | grep -o 'define-fun x () Int 7' \
    | sed 's/^/neg_divisor model: /'
grep -o 'non-literal divisor is outside the accepted fragment' "$TMP/var_divisor.err" | head -1 | sed 's/^/var_divisor: /'
grep -o 'by zero is outside the accepted fragment' "$TMP/by_zero.err" | head -1 | sed 's/^/by_zero: /'

# --- the serializer, round-tripped ------------------------------------------
# `half` is refine-int-divmod-by-literal's shape: n >= 0 |- 0 <= (/ n 2) <= n,
# proved by the compiler.  Its dumped VC asserts the hypotheses and the
# NEGATED goal, so `tur smt` must answer unsat -- and it can only do that if
# the dump spells `(/ n 2)` as the truncating value the axioms describe.  A
# bare `(div n 2)` would still prove here (n >= 0 is where the two agree);
# `quot-neg` is the case that separates them: n < 0 |- (/ n 2) <= 0 holds for
# truncation, and a Euclidean reading of the dumped `div` proves it too, but
# `rem-neg` -- n < 0 |- -1 <= (mod n 2) <= 0 -- is FALSE under a Euclidean
# `mod` (the remainder would be 0 or 1), so a dump that wrote a bare
# `(mod n 2)` comes back sat/unknown here rather than unsat.
cat > "$TMP/rt.tur" <<'EOF'
(defn half [n : #refine{ x : int | (>= x 0) }]
           : #refine{ r : int | (and (>= r 0) (<= r n)) }
  (/ n 2))
(defn rem-neg [n : #refine{ x : int | (< x 0) }]
              : #refine{ r : int | (and (<= r 0) (>= r -1)) }
  (mod n 2))
(defn main [] : int (+ (half 9) (rem-neg -7)))
EOF
"$TUR" check --dump-refine=json "$TMP/rt.tur" > "$TMP/rt.json" 2>/dev/null
python3 - "$TMP/rt.json" "$TMP" <<'PY'
import json, sys
recs = json.load(open(sys.argv[1]))
recs = recs.get("obligations", recs) if isinstance(recs, dict) else recs
n = 0
for r in recs:
    smt = r.get("vc_smtlib")
    if not smt or "div" not in smt and "mod" not in smt:
        continue
    open("%s/rt%d.smt2" % (sys.argv[2], n), "w").write(smt)
    n += 1
print("round-trip records with div/mod: %d" % n)
PY
for f in "$TMP"/rt[0-9].smt2; do
    [ -f "$f" ] || continue
    ans=$("$TUR" smt "$f" 2>/dev/null | head -1)
    echo "round-trip $(basename "$f"): $ans"
done
# The spelling itself: every `(div n 2)` / `(mod n 2)` sits inside the
# truncating idiom, never bare.  Count the idioms, then count the div/mod
# occurrences the idioms do not account for (each idiom holds two).
all_txt=$(cat "$TMP"/rt[0-9].smt2 2>/dev/null)
n_idiom=$(printf '%s' "$all_txt" | grep -oE '\(ite \(>= n 0\) \((div|mod) n 2\) \(- \((div|mod) \(- n\) 2\)\)\)' | wc -l)
n_ops=$(printf '%s' "$all_txt" | grep -oE '\((div|mod) ' | wc -l)
echo "round-trip truncating idioms: $n_idiom"
echo "round-trip div/mod outside an idiom: $((n_ops - 2 * n_idiom))"
exit 0
