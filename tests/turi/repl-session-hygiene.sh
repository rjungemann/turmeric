#!/usr/bin/env bash
# tests/turi/repl-session-hygiene.sh -- PS1 + PS4
# (docs/upcoming/playground-session-hygiene-plan.md, filed from
#  docs/reported/doc-lookup-poisons-the-playground-eval-session.md).
#
# Two session-hygiene rules, both testable at `tur repl` with no browser --
# which is the point: the report reads as a playground bug, but the mechanism
# is in shared elaboration and reproduces here.
#
#   PS1  A failed turn must not poison the session.  It used to discard the
#        elaboration session, dropping the REPL onto the whole-program path
#        where `stdlib_prefix` marks every prior USER turn as stdlib -- so the
#        next redefinition of the user's own `main` was refused as "already
#        defined by an auto-loaded stdlib module", naming a cause that does not
#        exist and prescribing a rename that cannot help.
#
#   PS4  Re-entering a top-level definition replaces the previous one.  `defn`,
#        `defclass` and `defopaque` already did; `defeffect` refused, so the
#        shipped effects example broke the session on its second Run.
#
# Both halves also assert the guards were NARROWED, not deleted: a FILE that
# declares the same effect twice, or that shadows a real stdlib name, still
# errors on the compiled path.
set -euo pipefail

TUR="${1:-./build/tur}"
PASS=0
FAIL=0
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

repl_out() {
    printf '%s\n' "$@" | "$TUR" repl 2>&1 | sed 's/\x1b\[[0-9;]*m//g'
}

check_has() {
    local desc="$1" expected="$2" actual="$3"
    if grep -qF -- "$expected" <<< "$actual"; then
        echo "PASS: $desc"; PASS=$((PASS + 1))
    else
        echo "FAIL: $desc"
        echo "  expected substring: $expected"
        echo "  got: $actual"
        FAIL=$((FAIL + 1))
    fi
}

check_lacks() {
    local desc="$1" unexpected="$2" actual="$3"
    if grep -qF -- "$unexpected" <<< "$actual"; then
        echo "FAIL: $desc"
        echo "  must NOT contain: $unexpected"
        echo "  got: $actual"
        FAIL=$((FAIL + 1))
    else
        echo "PASS: $desc"; PASS=$((PASS + 1))
    fi
}

# --- PS1: a failed turn leaves the session usable -------------------------
# Turn 3 fails at run time; turn 4 redefines `main` and turn 5 calls it.  The
# redefinition must succeed AND take effect -- asserting the new body's output
# ("bye"), not merely the absence of an error, is what distinguishes a real
# redefinition from the old definition still being in place.
ps1="$(repl_out \
    '(defn main [] : int (println "hi") 0)' \
    '(main)' \
    '(undefined-thing-xyz)' \
    '(defn main [] : int (println "bye") 0)' \
    '(main)')"
check_has   "PS1: redefinition after a failed turn takes effect" "bye" "$ps1"
check_lacks "PS1: no stdlib-module claim for the user's own name" \
            "already defined by an auto-loaded stdlib module" "$ps1"

# --- PS4: defeffect is re-enterable (the shipped effects example) ---------
ps4="$(repl_out \
    '(defeffect Ask [] :int)' \
    '(defn use-ask [] :int (+ 1 (perform (Ask))))' \
    '(println (handle (use-ask) (Ask [] k) (resume k 41)))' \
    '(defeffect Ask [] :int)' \
    '(defn use-ask [] :int (+ 1 (perform (Ask))))' \
    '(println (handle (use-ask) (Ask [] k) (resume k 41)))')"
check_lacks "PS4: a second defeffect turn is accepted" \
            "defeffect: 'Ask' is already defined" "$ps4"
# Both runs answer, so the replacement effect really is handled.
if [ "$(grep -c '^42$' <<< "$ps4")" = "2" ]; then
    echo "PASS: PS4: both runs of the effects example answer 42"; PASS=$((PASS + 1))
else
    echo "FAIL: PS4: both runs of the effects example answer 42"
    echo "  got: $ps4"; FAIL=$((FAIL + 1))
fi

# --- PS4: defstruct refuses, but says what actually happened --------------
# Not made re-enterable: a struct VALUE carries its AdtDef, so a value built by
# an earlier turn and matched after a redefinition would be read against a
# different layout.  The message must name the session and the action that
# resolves it, not a stdlib module the type is not in.
ps4s="$(repl_out '(defstruct P [a : int])' '(defstruct P [a : int])')"
check_has   "PS4: defstruct refusal names the session" \
            "already defined earlier in this session" "$ps4s"
check_lacks "PS4: defstruct refusal does not blame a stdlib module" \
            "an auto-loaded stdlib module or earlier form in this file" "$ps4s"

# --- The guards are narrowed, not deleted ---------------------------------
printf '(defeffect Ask [] :int)\n(defeffect Ask [] :int)\n(defn main [] : int 0)\n' \
    > "$WORK/dup-effect.tur"
check_has "file: a duplicate defeffect still errors" \
          "defeffect: 'Ask' is already defined" \
          "$("$TUR" check "$WORK/dup-effect.tur" 2>&1 || true)"

printf '(defn vec-len [v : int] : int 999)\n(defn main [] : int 0)\n' \
    > "$WORK/shadow-stdlib.tur"
check_has "file: shadowing a real stdlib name still errors (MF3)" \
          "already defined by an auto-loaded stdlib module" \
          "$("$TUR" check "$WORK/shadow-stdlib.tur" 2>&1 || true)"

echo "repl-session-hygiene summary: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
