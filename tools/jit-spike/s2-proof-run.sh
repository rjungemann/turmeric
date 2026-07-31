#!/usr/bin/env bash
# Drive the S2 split proof (findings 19) end to end over the subsystem
# fixtures, and time the split against the full TU.
#
# Reconstructs the ad-hoc Linux driver of 19.2 as a committed script so the
# proof is replayable on any host (this is what 19.4's "the proof script and
# the TUR_JIT_PRELIB hook stay in-tree" is for).
#
#   TUR=... SPIKE=... bash tools/jit-spike/s2-proof-run.sh [fixture...]
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

TUR="${TUR:-$ROOT/build-turjit/tur}"
SPIKE="${SPIKE:-$ROOT/build-jit-spike/tools/jit-spike/tur-jit-spike}"
OUT="${OUT:-$ROOT/build-jit/s2-proof}"
CC_SO="${CC_SO:-cc}"

[ -x "$TUR" ]   || { echo "no tur at $TUR"; exit 1; }
[ -x "$SPIKE" ] || { echo "no spike at $SPIKE (build -DTUR_JIT_SPIKE=ON)"; exit 1; }

# ld64 has no -Bsymbolic; the two-level namespace it uses by default already
# self-binds a dylib's intra-library calls, which is what -Bsymbolic buys on
# ELF.  Same 19.3-seam-3 property, reached by the platform default.
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
pass=0; fail=0

for name in "${FIXTURES[@]}"; do
  d="tests/fixtures/$name"
  if [ ! -f "$d/input.tur" ]; then echo "$name: NO INPUT"; continue; fi

  if ! "$TUR" emit-c "$d/input.tur" > "$OUT/$name.c" 2> "$OUT/$name.emit.err"; then
    echo "$name: emit-c FAILED"; fail=$((fail + 1)); continue
  fi
  if ! python3 tools/jit-spike/s2-split-proof.py \
         "$OUT/$name.c" "$OUT/$name" > "$OUT/$name.split.log" 2>&1; then
    echo "$name: split FAILED -- $(tail -1 "$OUT/$name.split.log")"
    fail=$((fail + 1)); continue
  fi

  # (a) the runtime half, compiled ONCE by the system cc into a shared lib
  if ! $CC_SO $SOFLAGS -O2 -o "$OUT/$name.rt.$SOEXT" "$OUT/$name.rt.c" \
       > "$OUT/$name.so.log" 2>&1; then
    echo "$name: runtime .$SOEXT compile FAILED -- $(grep -m2 error: "$OUT/$name.so.log")"
    fail=$((fail + 1)); continue
  fi

  # (b) the program half, through c2mir with the runtime host-resident
  out=$(TUR_JIT_PRELIB="$OUT/$name.rt.$SOEXT" timeout 120 \
          "$SPIKE" --eager --quiet "$OUT/$name.prog.c" 2> "$OUT/$name.run.err")
  rc=$?
  if [ "$out" = "$(cat "$d/expected.stdout")" ]; then
    echo "$name: PASS  (rt $(wc -l < "$OUT/$name.rt.c") lines, prog $(wc -l < "$OUT/$name.prog.c") lines)"
    pass=$((pass + 1))
  else
    echo "$name: FAIL (rc=$rc) -- $(head -c 200 "$OUT/$name.run.err" | tr '\n' ' ')"
    fail=$((fail + 1))
  fi
done

echo "--- split proof: $pass passed, $fail failed ---"

# 19.2's latency table: full TU vs program half, best of 5, on arith.
lat() {
  TUR_JIT_PRELIB="${2:-}" "$SPIKE" --eager --repeat 5 "$1" 2>&1 \
    | grep -E '^jit-spike: c2mir'
}
if [ -f "$OUT/arith.prog.c" ]; then
  echo "--- latency: full TU (status quo) ---"
  lat "$OUT/arith.c"
  echo "--- latency: split (program half only) ---"
  lat "$OUT/arith.prog.c" "$OUT/arith.rt.$SOEXT"
fi
