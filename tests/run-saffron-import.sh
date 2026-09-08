#!/usr/bin/env bash
# tests/run-saffron-import.sh -- a Turmeric module can IMPORT a Saffron module.
#
# saffron-lang-plan S7 / D5, the reverse direction. D5 said a Turmeric module
# importing a Saffron one "sees `any`-typed exports and must narrow them
# (cast, is?, match) like any other `any` -- that is already how `any` works;
# nothing new is needed." That assumed the import WORKED. It did not:
#
#   ./dyn.tur:1:1: error: unexpected character '#' (0x23)
#   1 | #lang saffron
#
# The import path (elab_module.c) hardcoded READER_TURMERIC and never ran
# `#lang` detection, so the directive reached the reader as source and a
# Saffron module could not be imported at ALL. The `(load ...)` path in
# elab_toplevel.c already detected it -- the same split that let `tur fmt`
# reject every `#lang` file while every other entry point accepted them.
#
# Needs its own runner rather than a fixture: a multi-module fixture requires a
# dedicated runner anyway (cf. tests/fixtures/any-type-id-multi-module and its
# requires.dedicated-runner marker), and this is smaller than that machinery.
#
# Usage: bash tests/run-saffron-import.sh
# Environment: TUR  path to the compiler (default: ./build/tur)

set -u
cd "$(dirname "$0")/.."
TUR_REL="${TUR:-./build/tur}"
TUR="$(cd "$(dirname "$TUR_REL")" && pwd)/$(basename "$TUR_REL")"
if [ ! -x "$TUR" ]; then
    echo "run-saffron-import: $TUR not built" >&2
    exit 2
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
FAILED=0

# A Saffron module: no annotations anywhere, so both params and both returns
# are `any` by D3's default.
cat > "$TMP/dyn.tur" <<'EOF'
#lang saffron
(defmodule dyn
  (export twice greet)
  (defn twice [x] (* x 2))
  (defn greet [name] name))
EOF

# A plain TURMERIC module importing it. The exports are `any`-typed, so the
# caller narrows with `cast` like any other `any` -- which is exactly what D5
# says this direction should feel like.
cat > "$TMP/main.tur" <<'EOF'
(defmodule app
  (import dyn :refer [twice greet])
  (defn main [] : int
    (println (cast (twice (:: 21 any)) int))
    (println (cast (greet (:: "hi" any)) cstr))
    0))
EOF

expect="42
hi"

for mode in compiled interpret; do
    if [ "$mode" = compiled ]; then
        out="$(cd "$TMP" && "$TUR" run main.tur 2>/dev/null)"
    else
        out="$(cd "$TMP" && ASAN_OPTIONS=detect_leaks=0 "$TUR" --interpret main.tur 2>/dev/null)"
    fi
    if [ "$out" != "$expect" ]; then
        echo "FAIL saffron-import ($mode): expected '$expect', got '$out'"
        FAILED=1
    else
        echo "PASS saffron-import ($mode): a Turmeric module imported a Saffron one"
    fi
done

if [ $FAILED -ne 0 ]; then
    echo "run-saffron-import: FAILED"
    exit 1
fi
echo "run-saffron-import: PASS"
