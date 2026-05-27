#!/usr/bin/env bash
# tests/turi/repl-spice-watch.sh -- RP6 integration test.
#
# Scenarios:
#   1. `tur repl --watch` without source changes is functionally
#      identical to plain `tur repl` (no extra output, no reload).
#   2. With --watch, mutating a source file mid-session triggers
#      an auto-reload right before the next eval, so the next call
#      sees the new code.
#   3. Without --watch (default), the same mutation does NOT auto-
#      reload: the next call still returns the old value.
#   4. `--watch` survives `--help` parsing (no false positive).
#
# Scenarios 2/3 drive the REPL through a FIFO so input is fed
# line-by-line; the source mutation lands between the user's two
# inputs while the REPL is blocked in readline.

set -uo pipefail
cd "$(dirname "$0")/../.."

TUR="${TUR:-./build/tur}"
[ -x "$TUR" ] || { echo "tests: $TUR not built; run 'just build' first" >&2; exit 2; }
export TUR_BIN
case "$TUR" in
    /*) TUR_BIN="$TUR" ;;
    *)  TUR_BIN="$(cd "$(dirname "$TUR")" && pwd)/$(basename "$TUR")" ;;
esac

WORK="$(mktemp -d -t tur-rp6.XXXXXX)"
trap 'rm -rf "$WORK"; rm -f "$WORK.fifo"' EXIT

PASS=0
FAIL=0
pass() { PASS=$((PASS + 1)); echo "PASS $1"; }
fail() { FAIL=$((FAIL + 1)); echo "FAIL $1 -- $2"; }

make_project() {
    local root="$1"
    mkdir -p "$root/src"
    cat > "$root/build.tur" <<'EOF'
(defpackage rp6-fixture)
EOF
}

write_lib() {
    local root="$1"
    local body="$2"
    cat > "$root/src/lib.tur" <<EOF
(defmodule sh
  (export answer)
  (defn answer [] :int ${body}))
EOF
}

# Drive a REPL session through a FIFO; mutate the source between the
# two `(answer)` inputs. The function dumps the REPL output to stdout.
run_edit_session() {
    local root="$1"; shift
    local fifo="$WORK.fifo"
    local out="$root/out.log"
    rm -f "$fifo"
    mkfifo "$fifo"
    (cd "$root" && "$TUR_BIN" "$@" < "$fifo") >"$out" 2>&1 &
    local pid=$!
    exec 3>"$fifo"
    printf '(answer)\n' >&3
    sleep 1
    write_lib "$root" 99
    sleep 1
    printf '(answer)\n' >&3
    printf ':quit\n' >&3
    exec 3>&-
    wait "$pid"
    rm -f "$fifo"
    cat "$out"
}

# -------- scenario 1: --watch with no source changes ---------------------
PROJ1="$WORK/p1"
make_project "$PROJ1"
write_lib "$PROJ1" 42
out=$(cd "$PROJ1" && printf '(answer)\n:quit\n' \
      | "$TUR_BIN" repl --watch 2>&1)
if echo "$out" | grep -qx '=> 42' \
    && ! echo "$out" | grep -q 'rebuilt'; then
    pass "rp6-watch-no-changes"
else
    fail "rp6-watch-no-changes" \
         "expected '=> 42' and no 'rebuilt' line; got: $out"
fi

# -------- scenario 2: --watch auto-rebuilds on source edit ---------------
PROJ2="$WORK/p2"
make_project "$PROJ2"
write_lib "$PROJ2" 42
out=$(run_edit_session "$PROJ2" repl --watch)
if   echo "$out" | grep -qx '=> 42' \
  && echo "$out" | grep -q '(reload) rebuilt 1 export' \
  && echo "$out" | grep -qx '=> 99'; then
    pass "rp6-watch-edit-auto-reload"
else
    fail "rp6-watch-edit-auto-reload" \
         "expected => 42, rebuild line, => 99; got: $out"
fi

# -------- scenario 3: without --watch, same edit is NOT auto-reloaded ----
PROJ3="$WORK/p3"
make_project "$PROJ3"
write_lib "$PROJ3" 42
out=$(run_edit_session "$PROJ3" repl)
# Both (answer) calls should return 42 -- no auto-rebuild fired.
got_42=$(echo "$out" | grep -cx '=> 42')
if [ "$got_42" = "2" ] && ! echo "$out" | grep -q 'rebuilt'; then
    pass "rp6-no-watch-skips-auto-reload"
else
    fail "rp6-no-watch-skips-auto-reload" \
         "expected '=> 42' twice and no rebuild; got: $out"
fi

# -------- scenario 4: --watch doesn't confuse the help dispatch ----------
if "$TUR" repl --watch --help 2>&1 | grep -q 'tur repl'; then
    pass "rp6-watch-with-help"
else
    fail "rp6-watch-with-help" "expected help output to mention 'tur repl'"
fi

# Unknown flag still errors.
if ! "$TUR" repl --nope 2>&1 | grep -q "unknown option"; then
    fail "rp6-watch-unknown-flag" "no 'unknown option' diagnostic"
else
    pass "rp6-watch-unknown-flag"
fi

echo
echo "repl-spice-watch: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
