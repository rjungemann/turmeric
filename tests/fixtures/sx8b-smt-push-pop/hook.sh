#!/usr/bin/env bash
# SX8b: `tur smt` as an incremental session -- (push)/(assert)/(check-sat)/(pop).
#
# The property that makes an incremental interface worth having is not that it
# answers, but that it answers the SAME as the non-incremental one.  So the
# centre of this fixture is a DIFFERENTIAL check: every answer a stacked script
# gives must equal the answer a flat script with the same assertions in scope
# gives, run as a separate process with no stack at all.  A push/pop that
# silently kept a popped assertion, or dropped a live one, is exactly the bug
# this shape catches and a single-script test does not.
#
# Also pinned: the SX8a contract is undisturbed -- a script with no (check-sat)
# is still decided once at the end -- and the two ways a stack can be malformed
# are refusals rather than guesses.

set -u
TMP="$1"
TUR="${TUR:-./build/tur}"

# --- the stacked script ------------------------------------------------------
# x > 10 throughout. Inside the scope, x < 5 contradicts it; after the pop the
# contradiction must be gone.
cat > "$TMP/stacked.smt2" <<'EOF'
(set-logic QF_LIA)
(declare-fun x () Int)
(assert (> x 10))
(check-sat)
(push 1)
(assert (< x 5))
(check-sat)
(pop 1)
(check-sat)
EOF

# --- the three flat scripts it must agree with -------------------------------
cat > "$TMP/flat1.smt2" <<'EOF'
(set-logic QF_LIA)
(declare-fun x () Int)
(assert (> x 10))
(check-sat)
EOF

cat > "$TMP/flat2.smt2" <<'EOF'
(set-logic QF_LIA)
(declare-fun x () Int)
(assert (> x 10))
(assert (< x 5))
(check-sat)
EOF

cp "$TMP/flat1.smt2" "$TMP/flat3.smt2"

# Answers only (stdout's first word per check), so the comparison is about
# verdicts rather than model formatting.
stacked=$("$TUR" smt "$TMP/stacked.smt2" 2>/dev/null | grep -E '^(sat|unsat|unknown)$' | tr '\n' ' ')
flat=""
for f in flat1 flat2 flat3; do
    flat="$flat$("$TUR" smt "$TMP/$f.smt2" 2>/dev/null | grep -E '^(sat|unsat|unknown)$') "
done
echo "stacked: $stacked"
echo "flat:    $flat"
[ "$stacked" = "$flat" ] && echo "differential: agree" || echo "differential: DISAGREE"

# --- multi-level push/pop lands where n separate pops would -------------------
cat > "$TMP/multi.smt2" <<'EOF'
(set-logic QF_LIA)
(declare-fun x () Int)
(assert (> x 10))
(push 2)
(assert (< x 5))
(assert (> x 100))
(check-sat)
(pop 2)
(check-sat)
EOF
echo "multi: $("$TUR" smt "$TMP/multi.smt2" 2>/dev/null | grep -E '^(sat|unsat|unknown)$' | tr '\n' ' ')"

# --- the exit code is the LAST answer ----------------------------------------
# Captured into a variable immediately: `$?` read later in a string that also
# carries a command substitution reports the substitution's status, not tur's.
"$TUR" smt "$TMP/stacked.smt2" >/dev/null 2>&1
rc=$?
echo "stacked exit: $rc"

# --- SX8a's contract is undisturbed: no (check-sat) is still decided once -----
cat > "$TMP/nocheck.smt2" <<'EOF'
(set-logic QF_LIA)
(declare-fun x () Int)
(assert (> x 0))
(assert (< x 0))
EOF
"$TUR" smt "$TMP/nocheck.smt2" >"$TMP/nocheck.out" 2>/dev/null
rc=$?
echo "no check-sat: answer=$(head -1 "$TMP/nocheck.out") exit=$rc"

# --- a malformed stack is refused, never guessed at --------------------------
printf '(pop)\n' > "$TMP/badpop.smt2"
"$TUR" smt "$TMP/badpop.smt2" >/dev/null 2>"$TMP/badpop.err"
rc=$?
echo "unmatched pop: exit=$rc $(grep -o 'pop with no matching push' "$TMP/badpop.err")"

printf '(push x)\n' > "$TMP/badlevel.smt2"
"$TUR" smt "$TMP/badlevel.smt2" >/dev/null 2>"$TMP/badlevel.err"
rc=$?
echo "bad level: exit=$rc $(grep -o 'malformed push/pop level' "$TMP/badlevel.err")"

# --- interactive mode answers as commands arrive, and (exit) stops ------------
# The trailing (assert false) is after (exit): if it were executed the final
# answer would change, so its absence is what pins that (exit) ends the session.
printf '(set-logic QF_LIA)\n(declare-fun y () Int)\n(assert (> y 3))\n(check-sat)\n(push)\n(assert (< y 1))\n(check-sat)\n(pop)\n(check-sat)\n(exit)\n(assert false)\n(check-sat)\n' \
  | "$TUR" smt --interactive 2>/dev/null \
  | grep -E '^(sat|unsat|unknown)$' | tr '\n' ' ' | sed 's/^/interactive: /;s/ $//'
echo

exit 0
