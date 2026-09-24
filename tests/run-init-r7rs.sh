#!/usr/bin/env bash
# tests/run-init-r7rs.sh -- r7rs-lang-plan R9: `tur init --r7rs` scaffolds a
# project that BUILDS, RUNS and TESTS.
#
# As with run-init-saffron.sh the assertion is the build, not the text.  The
# first draft of this scaffold found three defects that a text check would
# have passed over, all in `tur build .` (project mode) and none in the
# single-file path every fixture uses:
#
#   - a `#lang r7rs` program has no `(defn main`, and the build's bin/lib
#     decision is a text scan for exactly that, so the binary scaffold built a
#     shared library and no executable (file_has_main_defn);
#   - the library scaffold was refused outright -- the prelude is a `#lang
#     r7rs` file in the same form stream, and "a file with a define-library
#     may hold nothing else" counted the prelude's definitions;
#   - the per-module C emission lacked the forward-declaration band for
#     globals, so the prelude's handler stack was used before its declaration
#     (cc: 'r7rs-handlers__' undeclared) in both shapes.
#
# So: both shapes are built, the binary is RUN, both test files run under
# `tur test`, the scaffold is already `tur fmt`-clean, and the default
# scaffold is unchanged.  Skips cleanly (exit 0) when the binary is missing.

set -uo pipefail
cd "$(dirname "$0")/.."
ROOT="$PWD"

TUR="${TUR:-$ROOT/build/tur}"
[ -x "$TUR" ] || [ ! -x "${TUR}.exe" ] || TUR="${TUR}.exe"
if [ ! -x "$TUR" ]; then
    echo "SKIP: $TUR not built"
    echo "TUR_SKIP: $TUR not built"
    exit 0
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
fails=0

# --- 1. --r7rs --bin: scaffolds, builds, runs, tests -------------------------
mkdir -p "$WORK/bin" && cd "$WORK/bin"
"$TUR" init --no-git --r7rs demo >/dev/null 2>&1
if [ "$(head -1 src/main.tur)" != '#lang r7rs' ]; then
    echo "FAIL --r7rs --bin: src/main.tur has no '#lang r7rs' first line"
    fails=$((fails + 1))
fi
out="$("$TUR" build . 2>&1)"
if [ ! -x build/bin/demo ]; then
    echo "FAIL --r7rs --bin: build produced no binary"
    sed 's/^/       /' <<<"$out" | grep -v W0060 | head -6
    fails=$((fails + 1))
elif ! grep -q 'Hello from demo!' < <(./build/bin/demo 2>/dev/null); then
    echo "FAIL --r7rs --bin: the built binary did not greet"
    fails=$((fails + 1))
else
    echo "ok   --r7rs --bin: scaffolds, builds, runs"
fi
out="$("$TUR" test tests 2>&1)"
if grep -q '1 passed, 0 failed' <<<"$out"; then
    echo "ok   --r7rs --bin: tur test passes"
else
    echo "FAIL --r7rs --bin: tur test"
    sed 's/^/       /' <<<"$out" | grep -v W0060 | tail -6
    fails=$((fails + 1))
fi
if "$TUR" fmt --check src tests >/dev/null 2>&1; then
    echo "ok   --r7rs --bin: the scaffold is tur fmt-clean"
else
    echo "FAIL --r7rs --bin: tur fmt would change the scaffold"
    "$TUR" fmt --diff src tests 2>&1 | head -10 | sed 's/^/       /'
    fails=$((fails + 1))
fi

# --- 2. --r7rs --lib: a define-library that builds and tests -----------------
mkdir -p "$WORK/lib" && cd "$WORK/lib"
"$TUR" init --no-git --lib --r7rs mylib >/dev/null 2>&1
if ! grep -q '^(define-library (mylib)' src/mylib.tur; then
    echo "FAIL --r7rs --lib: src/mylib.tur is not a define-library"
    fails=$((fails + 1))
fi
out="$("$TUR" build . 2>&1)"
if grep -qE 'error' <<<"$out"; then
    echo "FAIL --r7rs --lib: build reported an error"
    sed 's/^/       /' <<<"$out" | grep -v W0060 | head -6
    fails=$((fails + 1))
else
    echo "ok   --r7rs --lib: builds"
fi
out="$("$TUR" test tests 2>&1)"
if grep -q '1 passed, 0 failed' <<<"$out" && grep -q 'tests: ok' <<<"$out"; then
    echo "ok   --r7rs --lib: tur test imports the library and passes"
else
    echo "FAIL --r7rs --lib: tur test"
    sed 's/^/       /' <<<"$out" | grep -v W0060 | tail -6
    fails=$((fails + 1))
fi
if "$TUR" fmt --check src tests >/dev/null 2>&1; then
    echo "ok   --r7rs --lib: the scaffold is tur fmt-clean"
else
    echo "FAIL --r7rs --lib: tur fmt would change the scaffold"
    fails=$((fails + 1))
fi

# --- 3. two languages at once is refused ---------------------------------------
mkdir -p "$WORK/both" && cd "$WORK/both"
if "$TUR" init --no-git --r7rs --saffron both >/dev/null 2>&1 || [ -e build.tur ]; then
    echo "FAIL --r7rs --saffron: accepted, or scaffolded anyway"
    fails=$((fails + 1))
else
    echo "ok   --r7rs --saffron: refused, nothing written"
fi

# --- 4. the DEFAULT scaffold is unchanged ---------------------------------------
mkdir -p "$WORK/plain" && cd "$WORK/plain"
"$TUR" init --no-git plain >/dev/null 2>&1
if [[ "$(head -1 src/main.tur)" == '#lang'* ]] || ! grep -q ':int' src/main.tur; then
    echo "FAIL default: a plain scaffold must stay annotated Turmeric"
    fails=$((fails + 1))
else
    echo "ok   default: still annotated Turmeric, no #lang line"
fi

cd "$ROOT"
if [ "$fails" -ne 0 ]; then
    echo "run-init-r7rs: $fails check(s) failed"
    exit 1
fi
echo "run-init-r7rs: all checks passed"
