#!/usr/bin/env bash
# SX8a: `tur smt` -- the solver answering queries from outside the compile
# pipeline.
#
# Pins the four answers and, crucially, the four EXIT CODES: they are the
# contract a shell harness branches on without parsing stdout, and they are not
# the usual 0-is-success convention (`unsat` is an answer, not a success).
#
# Also pins that a script outside the accepted fragment is refused WHOLE rather
# than partially parsed.  That is the one dishonest failure mode a query
# surface can have: a partly-read assertion set has weaker hypotheses than the
# script wrote, so `unsat` from it would be a claim about work not done.

set -u
TMP="$1"
TUR="${TUR:-./build/tur}"

# --- unsat: a contradiction the arithmetic stage refutes ---------------------
cat > "$TMP/unsat.smt2" <<'EOF'
(set-logic QF_LIA)
(declare-fun x () Int)
(assert (> x 0))
(assert (< x 0))
(check-sat)
EOF

# --- sat: satisfiable, and the bounded search produces a real witness --------
cat > "$TMP/sat.smt2" <<'EOF'
(set-logic QF_LIA)
(declare-fun x () Int)
(assert (> x 0))
(check-sat)
EOF

# --- unknown: no stage decides it and the bounded search declines ------------
# Four variables is past the counterexample search's deliberately tiny scope,
# and nothing here is provable, so `unknown` is the honest answer rather than a
# failure.
cat > "$TMP/unknown.smt2" <<'EOF'
(set-logic QF_UFLIA)
(declare-fun a () Int)
(declare-fun b () Int)
(declare-fun c () Int)
(declare-fun d () Int)
(declare-fun f (Int) Int)
(assert (> (+ a b c d) 0))
(assert (> (f a) 0))
(check-sat)
EOF

# --- outside the fragment: quantifiers ---------------------------------------
cat > "$TMP/outside.smt2" <<'EOF'
(set-logic UFLIA)
(declare-fun p (Int) Bool)
(assert (forall ((x Int)) (p x)))
(check-sat)
EOF

for case in unsat sat unknown outside; do
    out=$("$TUR" smt "$TMP/$case.smt2" 2>"$TMP/$case.err")
    rc=$?
    echo "$case: answer=$(printf '%s' "$out" | head -1) exit=$rc"
done

# The witness itself, not just the word `sat`: a model that is not checked is
# indistinguishable from a guess, and the bounded search is the only thing in
# the solver allowed to answer INVALID precisely because it produces one.
"$TUR" smt "$TMP/sat.smt2" 2>/dev/null | grep -c 'define-fun x () Int' \
    | sed 's/^/sat model binds x: /'

# A refusal must say why, so a caller can tell "outside my fragment" from
# "I looked and could not decide".
grep -o 'outside the accepted fragment' "$TMP/outside.err" | head -1

exit 0
