#!/usr/bin/env bash
# run-tur-just-synonym.sh -- `tur just` is a synonym for `tur run`.
#
# The alias resolves to the canonical `run` command before dispatch, so both
# halves of `tur run` must answer to it: the Justfile task runner and the
# classic compile-and-run path for a .tur file. `tur jit` must keep its own
# meaning (it is not a prefix of `just`), and the abbreviation resolver must
# not start reporting `ru` as ambiguous now that `just` shares its target.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TUR="${TUR:-$ROOT/build/tur}"

FAIL=0
pass() { echo "PASS: $1"; }
fail() { echo "FAIL: $1"; FAIL=1; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
cd "$TMP"

cat > Justfile <<'EOF'
greet:
    echo "hello from greet"
EOF

# 1. Task-runner mode: a named recipe runs under `tur just`.
OUT="$("$TUR" just greet 2>&1)"
RC=$?
if [ "$RC" -eq 0 ] && grep -q "hello from greet" <<< "$OUT"; then
  pass "tur just <recipe> runs the recipe"
else
  fail "tur just <recipe> runs the recipe (rc=$RC, out=$OUT)"
fi

# 2. --list works the same through either spelling.
RUN_LIST="$("$TUR" run --list 2>&1)"
JUST_LIST="$("$TUR" just --list 2>&1)"
if [ -n "$RUN_LIST" ] && [ "$RUN_LIST" = "$JUST_LIST" ]; then
  pass "tur just --list matches tur run --list"
else
  fail "tur just --list matches tur run --list (run=$RUN_LIST just=$JUST_LIST)"
fi

# 3. Classic path: a .tur file argument compiles and executes.
cat > hello.tur <<'EOF'
(defn main [] : int
  (println "hello from just")
  0)
EOF
OUT3="$("$TUR" just hello.tur 2>&1)"
RC3=$?
if [ "$RC3" -eq 0 ] && grep -q "hello from just" <<< "$OUT3"; then
  pass "tur just <file.tur> compiles and runs the file"
else
  fail "tur just <file.tur> compiles and runs the file (rc=$RC3, out=$OUT3)"
fi

# 4. Abbreviations resolve: `ju` -> run's help, and `ru` is still unambiguous.
for tok in ju just ru run; do
  OUT4="$("$TUR" "$tok" --help 2>&1)"
  if grep -q "tur run  *list recipes" <<< "$OUT4"; then
    pass "tur $tok --help prints the run help"
  else
    fail "tur $tok --help prints the run help (out=$OUT4)"
  fi
done

# 5. `tur jit` keeps its own meaning.
OUT5="$("$TUR" jit --help 2>&1)"
if grep -q "usage: tur jit" <<< "$OUT5"; then
  pass "tur jit is unaffected by the just alias"
else
  fail "tur jit is unaffected by the just alias (out=$OUT5)"
fi

echo
if [ "$FAIL" -eq 0 ]; then
  echo "tur just synonym: all checks passed"
else
  echo "tur just synonym: FAILURES"
fi
exit "$FAIL"
