#!/usr/bin/env bash
# Validate the COMMITTED S2 split artifacts (src/runtime/generated/, produced
# by tools/gen-runtime-split.py) end to end -- the production arrangement,
# distinct from s2-proof-run.sh which re-splits per fixture:
#
#   * ONE runtime .so built from the committed all-gates tur_rt_split.c
#     (stands in for the runtime being compiled into tur, task-4 of 19.4);
#   * each fixture's emitted TU is cut at the preamble marker and the
#     COMMITTED declarations region is spliced ahead of the program half --
#     exactly what cmd_jit will do under a hash match.
#
# The per-fixture preamble differs from the all-gates union (gates), so this
# also validates the superset property: any program half must bind cleanly
# against the union decls + union runtime.
#
#   TUR=... SPIKE=... bash tools/jit-spike/s2-artifacts-run.sh [fixture...]
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

TUR="${TUR:-$ROOT/build-turjit/tur}"
SPIKE="${SPIKE:-$ROOT/build-jit/tools/jit-spike/tur-jit-spike}"
OUT="${OUT:-$ROOT/build-jit/s2-artifacts}"
GEN="${GEN:-$ROOT/src/runtime/generated}"
CC_SO="${CC_SO:-cc}"
MARK='/* ==== tur: end of fixed runtime preamble ==== */'

[ -x "$TUR" ]   || { echo "no tur at $TUR"; exit 1; }
[ -x "$SPIKE" ] || { echo "no spike at $SPIKE (build -DTUR_JIT_SPIKE=ON)"; exit 1; }
[ -f "$GEN/tur_rt_split.c" ] || { echo "no artifacts in $GEN (run tools/gen-runtime-split.py)"; exit 1; }

case "$(uname -s)" in
  Darwin) SOEXT=dylib; SOFLAGS="-dynamiclib -undefined dynamic_lookup" ;;
  *)      SOEXT=so;    SOFLAGS="-shared -fPIC -Wl,-Bsymbolic" ;;
esac

FIXTURES=("$@")
if [ ${#FIXTURES[@]} -eq 0 ]; then
  FIXTURES=(arith hamt-basic cps-backend-effect stm-stress dynvar-nested
            module-defer-basic gc-registry-growth
            self-recursive-carrier-struct-return)
fi

mkdir -p "$OUT"

# One runtime .so for every fixture -- built from the committed artifact.
if ! $CC_SO $SOFLAGS -O2 -I "$ROOT/src/runtime" \
     -o "$OUT/tur_rt_split.$SOEXT" "$GEN/tur_rt_split.c" \
     > "$OUT/rt.so.log" 2>&1; then
  echo "runtime .$SOEXT compile FAILED -- $(grep -m3 error: "$OUT/rt.so.log")"
  exit 1
fi

pass=0; fail=0
for name in "${FIXTURES[@]}"; do
  d="tests/fixtures/$name"
  if [ ! -f "$d/input.tur" ]; then echo "$name: NO INPUT"; continue; fi

  if ! "$TUR" emit-c "$d/input.tur" > "$OUT/$name.c" 2> "$OUT/$name.emit.err"; then
    echo "$name: emit-c FAILED"; fail=$((fail + 1)); continue
  fi
  # Program half: everything after the marker; splice the COMMITTED decls
  # region ahead of it (cmd_jit's exact assembly under a hash match).
  if ! grep -qF "$MARK" "$OUT/$name.c"; then
    echo "$name: no preamble marker"; fail=$((fail + 1)); continue
  fi
  { cat "$GEN/tur_rt_split_decls.h";
    awk -v mark="$MARK" 'found { print } index($0, mark) { found = 1 }' \
        "$OUT/$name.c"; } > "$OUT/$name.prog.c"

  # -I src/runtime: the union decls carry `#include "hamt.h"` (g_needs_hamt
  # is forced on in the artifacts) even for programs that never gated it in.
  out=$(TUR_JIT_PRELIB="$OUT/tur_rt_split.$SOEXT" timeout 120 \
          "$SPIKE" --eager --quiet -I "$ROOT/src/runtime" \
          "$OUT/$name.prog.c" 2> "$OUT/$name.run.err")
  rc=$?
  if [ "$out" = "$(cat "$d/expected.stdout")" ]; then
    echo "$name: PASS"
    pass=$((pass + 1))
  else
    echo "$name: FAIL (rc=$rc) -- $(head -c 200 "$OUT/$name.run.err" | tr '\n' ' ')"
    fail=$((fail + 1))
  fi
done

echo "--- committed-artifact split: $pass passed, $fail failed ---"
[ "$fail" -eq 0 ]
