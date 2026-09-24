#!/usr/bin/env bash
# tests/check-r7rs-eval-link.sh -- r7rs-lang-plan T4: importing (scheme eval)
# links the interpreter into a compiled program, and NOT importing it links
# nothing new.
#
#   1. A `#lang r7rs` program without the import: its generated C carries no
#      `-lturi` autolink marker, and the built binary has no libturi symbol.
#   2. The same program importing (scheme eval): the binary carries the
#      embedded evaluator, and it runs from another directory with
#      TUR_STDLIB_DIR unset -- the stdlib root is baked in at build time
#      (`@TUR_STDLIB_ROOT@` in stdlib/r7rs/eval.tur's autolink marker).
#
# Usage: bash tests/check-r7rs-eval-link.sh
# Environment: TUR  path to the compiler (default: ./build/tur)

set -u
cd "$(dirname "$0")/.."
TUR_REL="${TUR:-./build/tur}"
TUR="$(cd "$(dirname "$TUR_REL")" && pwd)/$(basename "$TUR_REL")"
if [ ! -x "$TUR" ]; then
    echo "check-r7rs-eval-link: $TUR not built" >&2
    exit 2
fi
if ! command -v nm >/dev/null 2>&1; then
    echo "SKIP check-r7rs-eval-link: nm not found"
    exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
FAILED=0
fail() { echo "FAIL check-r7rs-eval-link: $*"; FAILED=1; }

export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}"

cat > "$TMP/plain.tur" <<'EOF'
#lang r7rs
(import (scheme base) (scheme write))
(write (expt 2 10))
(newline)
EOF
cat > "$TMP/with-eval.tur" <<'EOF'
#lang r7rs
(import (scheme base) (scheme write) (scheme eval))
(write (eval '(expt 2 10) (environment '(scheme base))))
(newline)
EOF

# 1. No import, nothing linked.
if "$TUR" emit-c "$TMP/plain.tur" 2>/dev/null | grep -q -- '__tur_autolink__: -lturi'; then
    fail "a program without (scheme eval) carries the -lturi autolink marker"
fi
if ! "$TUR" build "$TMP/plain.tur" -o "$TMP/plain" >/dev/null 2>"$TMP/plain.err"; then
    fail "the program without (scheme eval) did not build: $(tail -3 "$TMP/plain.err")"
elif nm "$TMP/plain" 2>/dev/null | grep -Eq ' T (turi_eval|turi_env_new|turi_r7rs_embed_eval)$'; then
    fail "a program without (scheme eval) links libturi symbols"
fi

# 2. The import links the evaluator, and the binary finds its stdlib alone.
if ! "$TUR" build "$TMP/with-eval.tur" -o "$TMP/with-eval" >/dev/null 2>"$TMP/with-eval.err"; then
    fail "the program importing (scheme eval) did not build: $(tail -3 "$TMP/with-eval.err")"
else
    if ! nm "$TMP/with-eval" 2>/dev/null | grep -Eq ' T turi_r7rs_embed_eval$'; then
        fail "a program importing (scheme eval) does not carry the embedded evaluator"
    fi
    out="$(cd / && env -u TUR_STDLIB_DIR "$TMP/with-eval" 2>"$TMP/run.err")"
    if [ "$out" != "1024" ]; then
        fail "the built program printed '$out', want 1024 ($(tail -3 "$TMP/run.err"))"
    fi
fi

if [ "$FAILED" -eq 0 ]; then
    echo "check-r7rs-eval-link: ok"
fi
exit "$FAILED"
