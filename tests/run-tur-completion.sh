#!/usr/bin/env bash
# run-tur-completion.sh -- `tur completion` + the --list guarantees it rests on.
#
# Shell completion for `tur run` is driven by `tur run --list`, so this covers
# both halves together: the listing contract (aliases present, hidden recipes
# absent, unsupported features degrade instead of blanking the list, valid
# JSON) and the emitted zsh/bash scripts that consume it.
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
alias b := build

# Build "it"
build flags='-DFOO="bar"':
    @echo "build {{flags}}"

_helper:
    @echo helper

[private]
secret:
    @echo secret
EOF

# ---------------------------------------------------------------- listing ---

OUT="$("$TUR" run --list 2>/dev/null)"
if grep -q '^  b  # alias for `build`$' <<< "$OUT"; then
  pass "aliases appear in --list"
else
  fail "aliases appear in --list (got: $OUT)"
fi

if grep -qE '^  (_helper|secret)' <<< "$OUT"; then
  fail "[private] and _-prefixed recipes are hidden from --list (got: $OUT)"
else
  pass "[private] and _-prefixed recipes are hidden from --list"
fi

OUT_ALL="$("$TUR" run --list --all 2>/dev/null)"
if grep -q '^  _helper' <<< "$OUT_ALL" && grep -q '^  secret' <<< "$OUT_ALL"; then
  pass "--list --all reveals hidden recipes"
else
  fail "--list --all reveals hidden recipes (got: $OUT_ALL)"
fi

# A hidden recipe is hidden, not disabled.
OUT="$("$TUR" run secret 2>&1)"
if [ $? -eq 0 ] && grep -q "^secret$" <<< "$OUT"; then
  pass "[private] recipes remain runnable by name"
else
  fail "[private] recipes remain runnable by name (got: $OUT)"
fi

# The quote-bearing doc and parameter default must survive as valid JSON.
if "$TUR" run --list --json 2>/dev/null | python3 -c '
import json, sys
d = json.load(sys.stdin)
names = [x["name"] for x in d]
assert names == ["build", "b"], names
assert d[0]["doc"] == "Build \"it\"", d[0]["doc"]
assert d[0]["params"][0]["default"] == "-DFOO=\"bar\"", d[0]["params"]
assert d[1]["alias"] == "build", d[1]
' 2>/dev/null; then
  pass "--list --json escapes names, docs and defaults, and includes aliases"
else
  fail "--list --json escapes names, docs and defaults, and includes aliases"
fi

# --------------------------------------------------- unsupported features ---

# [confirm] is implemented now, so it is no longer a stand-in for "unsupported".
# [positional-arguments] is a real `just` attribute tur run does not implement.
cat > Justfile.unsupported <<'EOF'
good:
    @echo good

[positional-arguments]
risky:
    @echo risky
EOF

OUT="$("$TUR" run --justfile Justfile.unsupported --list 2>/dev/null)"
RC=$?
if [ "$RC" -eq 0 ] && grep -q '^  good' <<< "$OUT"; then
  pass "an unsupported attribute degrades --list instead of blanking it"
else
  fail "an unsupported attribute degrades --list instead of blanking it (rc=$RC, out=$OUT)"
fi

"$TUR" run --justfile Justfile.unsupported good >/dev/null 2>&1
if [ $? -eq 2 ]; then
  pass "an unsupported attribute is still fatal when running a recipe"
else
  fail "an unsupported attribute is still fatal when running a recipe"
fi

# ------------------------------------------------------- completion scripts --

for shell in zsh bash; do
  if "$TUR" completion "$shell" > "script.$shell" 2>/dev/null && [ -s "script.$shell" ]; then
    pass "tur completion $shell emits a script"
  else
    fail "tur completion $shell emits a script"
    continue
  fi

  if command -v "$shell" >/dev/null 2>&1; then
    if "$shell" -n "script.$shell" 2>/dev/null; then
      pass "tur completion $shell output parses under $shell"
    else
      fail "tur completion $shell output parses under $shell"
    fi
  else
    echo "SKIP: $shell not on PATH; not parse-checking its script"
  fi
done

if grep -q '^#compdef tur$' < <(head -1 script.zsh); then
  pass "zsh script carries the #compdef tag"
else
  fail "zsh script carries the #compdef tag"
fi

if grep -q '^complete -F _tur tur$' script.bash; then
  pass "bash script registers the completion"
else
  fail "bash script registers the completion"
fi

# Both scripts must source their recipe names from `tur run --list`; that is
# the whole reason the listing guarantees above are load-bearing.
for shell in zsh bash; do
  if grep -q 'run --list' "script.$shell"; then
    pass "$shell script sources recipes from 'tur run --list'"
  else
    fail "$shell script sources recipes from 'tur run --list'"
  fi
done

"$TUR" completion fish >/dev/null 2>&1
if [ $? -eq 2 ]; then
  pass "an unsupported shell is a CLI error"
else
  fail "an unsupported shell is a CLI error"
fi

# The bash script's recipe extraction is plain shell, so run it for real.
if command -v bash >/dev/null 2>&1; then
  RECIPES="$(cd "$TMP" && PATH="$(dirname "$TUR"):$PATH" bash -c '
    source ./script.bash
    COMP_WORDS=(tur run "")
    COMP_CWORD=2
    _tur
    printf "%s\n" "${COMPREPLY[@]}"
  ' 2>/dev/null)"
  if grep -qx build <<< "$RECIPES" && grep -qx b <<< "$RECIPES"; then
    pass "bash completion returns recipe names and aliases"
  else
    fail "bash completion returns recipe names and aliases (got: $RECIPES)"
  fi
fi

exit $FAIL
