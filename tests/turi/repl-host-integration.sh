#!/usr/bin/env bash
# tests/turi/repl-host-integration.sh — the REPL surface a GUI host drives.
#
# Every case here comes from §5 of docs/archive/lsp-client-gaps-plan.md: a
# real client (Trowel) carried a workaround because the REPL would not answer
# the question directly. Covers OSC 133 busy/idle markers over a pipe, the
# logical working directory, and :load-string.
set -uo pipefail

REPL="${1:-./build/tur}"
PASS=0
FAIL=0

check() {
    local desc="$1" expected="$2" actual="$3"
    if echo "$actual" | grep -qF -- "$expected"; then
        echo "PASS: $desc"
        PASS=$((PASS + 1))
    else
        echo "FAIL: $desc"
        echo "  expected substring: $expected"
        echo "  got: $actual"
        FAIL=$((FAIL + 1))
    fi
}

check_absent() {
    local desc="$1" unexpected="$2" actual="$3"
    if echo "$actual" | grep -qF -- "$unexpected"; then
        echo "FAIL: $desc"
        echo "  did not expect substring: $unexpected"
        echo "  got: $actual"
        FAIL=$((FAIL + 1))
    else
        echo "PASS: $desc"
        PASS=$((PASS + 1))
    fi
}

# stdout here is a pipe, never a tty -- which is the whole point: a GUI host
# drives the REPL over pipes and that is exactly the case that used to get no
# markers at all. `cat -v` renders ESC as ^[ so the markers survive grep.
repl_raw() {
    printf '%s\n' "$@" | "$REPL" repl 2>/dev/null | cat -v
}

# ---------------------------------------------------------------------------
# §5.1 -- OSC 133 semantic-prompt markers
# ---------------------------------------------------------------------------

# Default over a pipe: still silent. Emitting escapes into a redirect that
# nobody asked for would corrupt captured output.
DEFAULT_OUT="$(repl_raw '(+ 1 2)' ':quit')"
check_absent "no OSC 133 over a pipe by default" '133;' "$DEFAULT_OUT"

# Forced on: the full idle -> busy -> done cycle, which is what lets a host
# stop latching "busy" forever as its safe default.
FORCED="$(printf '%s\n' '(+ 1 2)' ':quit' \
    | TUR_SHELL_INTEGRATION=1 "$REPL" repl 2>/dev/null | cat -v)"
check "TUR_SHELL_INTEGRATION=1 emits prompt marker (A)" ']133;A' "$FORCED"
check "TUR_SHELL_INTEGRATION=1 emits exec marker (C)"   ']133;C' "$FORCED"
check "TUR_SHELL_INTEGRATION=1 emits done marker (D)"   ']133;D;0' "$FORCED"

# D carries the status, so a host can tell a failed form from a good one
# without parsing stderr.
ERR_OUT="$(printf '%s\n' '(no-such-fn-at-all 1)' ':quit' \
    | TUR_SHELL_INTEGRATION=1 "$REPL" repl 2>/dev/null | cat -v)"
check "failed form reports D;1" ']133;D;1' "$ERR_OUT"

# An explicit off beats an explicit on.
BOTH="$(printf ':quit\n' \
    | TUR_SHELL_INTEGRATION=1 TUR_NO_SHELL_INTEGRATION=1 "$REPL" repl 2>/dev/null | cat -v)"
check_absent "TUR_NO_SHELL_INTEGRATION wins over TUR_SHELL_INTEGRATION" \
    '133;' "$BOTH"

# ---------------------------------------------------------------------------
# §5.2 -- logical working directory
# ---------------------------------------------------------------------------

TMPROOT="$(mktemp -d "${TMPDIR:-/tmp}/repl_host_XXXXXX")"
# Collapse duplicate slashes without resolving symlinks. macOS sets TMPDIR
# with a trailing slash, so the mktemp template yields ".../T//repl_host_x";
# bash canonicalizes that away when it sets $PWD, and every path assertion
# below compares against $PWD, so the raw form would never match. `cd` + the
# `pwd` builtin is logical, so this tidies the string and nothing else --
# resolving here would defeat the very thing these cases test.
TMPROOT="$(cd "$TMPROOT" && pwd)"
REAL="$TMPROOT/real"
LINK="$TMPROOT/link"
mkdir -p "$REAL/nested"
ln -sfn "$REAL" "$LINK"
ABS_REPL="$(cd "$(dirname "$REPL")" && pwd)/$(basename "$REPL")"

# getcwd() resolves symlinks; $PWD does not. A host that launched us in
# $LINK must be told $LINK, or its own path never string-matches ours.
PWD_OUT="$(cd "$LINK" && printf ':pwd\n:quit\n' | "$ABS_REPL" repl 2>/dev/null)"
check ":pwd reports the logical path, not the resolved one" "$LINK" "$PWD_OUT"
check_absent ":pwd does not leak the resolved path" "$REAL" "$PWD_OUT"

# A relative :cd stays logical rather than snapping to the resolved path.
CD_OUT="$(cd "$LINK" && printf ':cd nested\n:pwd\n:quit\n' | "$ABS_REPL" repl 2>/dev/null)"
check ":cd keeps the logical prefix" "$LINK/nested" "$CD_OUT"

# ".." pops the logical path the way a shell does.
UP_OUT="$(cd "$LINK" && printf ':cd nested\n:cd ..\n:pwd\n:quit\n' \
    | "$ABS_REPL" repl 2>/dev/null)"
check ":cd .. pops back logically" "$LINK" "$UP_OUT"

# A stale $PWD must never be believed: it is checked against the real
# directory by (device, inode) before being reported.
STALE_OUT="$(cd "$REAL" && PWD=/definitely/not/here printf ':pwd\n:quit\n' \
    | env PWD=/definitely/not/here "$ABS_REPL" repl 2>/dev/null)"
check "stale \$PWD falls back to getcwd" "$REAL" "$STALE_OUT"
check_absent "stale \$PWD is not echoed back" "/definitely/not/here" "$STALE_OUT"

# A ".." that crosses a symlink cannot be popped textually. The report must be
# where we actually are, never the textual guess.
mkdir -p "$TMPROOT/deep/inner"
ln -sfn "$TMPROOT/deep/inner" "$TMPROOT/jump"
CROSS_OUT="$(cd "$TMPROOT/jump" && printf ':cd ..\n:pwd\n:quit\n' \
    | "$ABS_REPL" repl 2>/dev/null)"
check ":cd .. across a symlink reports the truth" "$TMPROOT/deep" "$CROSS_OUT"

rm -rf "$TMPROOT"

# ---------------------------------------------------------------------------
# §5.3 -- :load-string
# ---------------------------------------------------------------------------

strip_ansi() { sed 's/\x1b\[[0-9;]*m//g'; }
repl_out() { printf '%s\n' "$@" | "$REPL" repl 2>&1 | strip_ansi; }

check ":load-string evaluates an expression" "=> 3" \
    "$(repl_out ':load-string "(+ 1 2)"')"

# The point of the command: a multi-form region on one line, no scratch file.
check ":load-string runs a multi-form region" "=> 42" \
    "$(repl_out ':load-string "(defn dbl [x :int] :int (* x 2))\n(dbl 21)"')"

check ":load-string definitions persist" "=> 15" \
    "$(repl_out ':load-string "(defn trip [x :int] :int (* x 3))"' '(trip 5)')"

check ":load-string reports errors" "unknown function or operator" \
    "$(repl_out ':load-string "(no-such-fn 1)"')"

check ":load-string rejects an unquoted argument" "requires a quoted source string" \
    "$(repl_out ':load-string (+ 1 2)')"

check ":load-string rejects an unterminated literal" "unterminated string literal" \
    "$(repl_out ':load-string "oops')"

check ":help lists :load-string" ":load-string" "$(repl_out ':help')"

# ---------------------------------------------------------------------------
# §5.4 -- TUR_STDLIB_DIR validation
# ---------------------------------------------------------------------------

# A stale value inherited from the environment used to be taken verbatim, so
# the failure surfaced as a wall of "cannot open .../macros.tur" with nothing
# naming the variable responsible.
BAD_STDLIB="$(printf '(+ 1 2)\n:quit\n' \
    | TUR_STDLIB_DIR=/nonexistent/stdlib "$REPL" repl 2>&1 | strip_ansi)"
check "bad TUR_STDLIB_DIR names itself" "ignoring TUR_STDLIB_DIR" "$BAD_STDLIB"
check "bad TUR_STDLIB_DIR still evaluates" "=> 3" "$BAD_STDLIB"

# A good one is honored silently -- an explicit override is still an override.
GOOD_STDLIB="$(printf '(+ 1 2)\n:quit\n' \
    | TUR_STDLIB_DIR="$(pwd)/stdlib" "$REPL" repl 2>&1 | strip_ansi)"
check_absent "valid TUR_STDLIB_DIR is not warned about" \
    "ignoring TUR_STDLIB_DIR" "$GOOD_STDLIB"
check "valid TUR_STDLIB_DIR still works" "=> 3" "$GOOD_STDLIB"

echo ""
echo "$PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
