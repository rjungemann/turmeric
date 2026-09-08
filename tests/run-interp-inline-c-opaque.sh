#!/usr/bin/env bash
# tests/run-interp-inline-c-opaque.sh -- `any` reflection over an inline-C
# opaque, under --interpret.
#
# interp-inline-c-opaque-segv-in-any-reflection: `type-of` on an `any` holding a
# value returned from an INLINE-C `defopaque` constructor used to SEGFAULT the
# interpreter.  The inline-C result path re-tags a TURI_INT as TURI_STRUCT when
# the declared return type is TY_ADT, and an opaque IS a TY_ADT -- so the
# payload 7 became a `TuriStruct *` of 0x7, and the first reader to dereference
# it died (UBSan: misaligned member access at 0x7; ASan: SEGV).
#
# This needs its OWN runner rather than a fixture: tests/run-turi.sh PASS-skips
# every program containing a user inline-C block (the TI7 carve-out, ~767
# fixtures), so no fixture in the tree can assert an inline-C `defopaque` under
# --interpret at all.  That carve-out is exactly why nothing caught the crash.
#
# What is asserted:
#   1. The interpreter does not crash (the fix).
#   2. The compiled path still answers `Route` (no regression there).
#   3. A genuine `defdata` round-tripped through inline-C still reaches its
#      `match` arm -- the case the struct re-tag exists for, and the one a
#      careless fix would break.
#
# KNOWN DIVERGENCE, asserted rather than hidden: interpreted, (1) answers `adt`
# where compiled answers `Route`.  The reflection declines the struct name for
# an implausible pointer and falls back to the generic ADT answer.  Naming the
# opaque needs the AdtDef, and `FnDef.return_type` does not carry one -- it
# stores `type_from_kind(return_kind)`, a bare kind, "sufficient for the
# lifetime pass" per its own comment.  Threading the full declared type through
# is a separate change; the report records it.
#
# Usage: bash tests/run-interp-inline-c-opaque.sh
# Environment: TUR  path to the compiler (default: ./build/tur)

set -u
cd "$(dirname "$0")/.."

TUR="${TUR:-./build/tur}"
if [ ! -x "$TUR" ]; then
    echo "run-interp-inline-c-opaque: $TUR not built" >&2
    exit 2
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
FAILED=0

cat > "$TMP/opaque.tur" <<'EOF'
(defopaque Route :int)
(defn route-of [n : int] : Route
  ```c
  return n;
  ```)
(defn boxed [] : any (route-of 7))
(defn main [] : int (println (type-of (boxed))) 0)
EOF

cat > "$TMP/adt.tur" <<'EOF'
(defdata Shape [] (Circle [r : int]) (Square [s : int]))
(defn passthru [x : Shape] : Shape
  ```c
  return x;
  ```)
(defn main [] : int
  (let [s (passthru (Circle 5))]
    (match s
      (Circle r) (println r)
      (Square q) (println q)))
  0)
EOF

# 1. The interpreter must not crash.  ASAN_OPTIONS forces leak detection off
#    (the tree-walker retains process-lifetime closures by design) while leaving
#    the ADDRESS sanitizer -- the half that caught this -- on.
out="$(ASAN_OPTIONS=detect_leaks=0 "$TUR" --interpret "$TMP/opaque.tur" 2>&1)"
rc=$?
if [ $rc -ne 0 ]; then
    echo "FAIL interp-inline-c-opaque: --interpret exited $rc"
    echo "$out" | head -5
    FAILED=1
elif printf '%s' "$out" | grep -qiE "SEGV|AddressSanitizer|runtime error"; then
    echo "FAIL interp-inline-c-opaque: sanitizer fired"
    echo "$out" | head -5
    FAILED=1
elif [ "$out" != "adt" ]; then
    echo "FAIL interp-inline-c-opaque: expected 'adt' (the known divergence), got '$out'"
    FAILED=1
else
    echo "PASS interp-inline-c-opaque: no crash (answers 'adt' -- known divergence)"
fi

# 2. The compiled path is unchanged.
out="$("$TUR" run "$TMP/opaque.tur" 2>/dev/null)"
if [ "$out" != "Route" ]; then
    echo "FAIL interp-inline-c-opaque: compiled expected 'Route', got '$out'"
    FAILED=1
else
    echo "PASS interp-inline-c-opaque: compiled still answers 'Route'"
fi

# 3. The struct re-tag still does its job for a real ADT.
out="$(ASAN_OPTIONS=detect_leaks=0 "$TUR" --interpret "$TMP/adt.tur" 2>&1)"
if [ "$out" != "5" ]; then
    echo "FAIL interp-inline-c-opaque: ADT round-trip expected '5', got '$out'"
    FAILED=1
else
    echo "PASS interp-inline-c-opaque: inline-C ADT round-trip still matches"
fi

if [ $FAILED -ne 0 ]; then
    echo "run-interp-inline-c-opaque: FAILED"
    exit 1
fi
echo "run-interp-inline-c-opaque: PASS"
