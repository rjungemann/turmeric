#!/usr/bin/env bash
# tests/run-refine-wasm.sh -- RT5a: the refinement solver on wasm32.
#
# Two questions, and only the second one is interesting:
#
#   1. Do the refine sources COMPILE under Emscripten, with the same flags the
#      real `tur_wasm` target uses?
#   2. Do they produce the SAME ANSWERS at 32-bit pointers?
#
# A compile check alone would miss the thing most likely to actually differ:
# S2 is Fourier-Motzkin over exact rationals with `__builtin_*_overflow`
# guards, and the hash-cons table, the constant folder, and the model search
# all key off integer widths.  So this links the solver unit test to wasm and
# runs its checks under node.
#
# Skips cleanly when emcc is absent, so it is safe to wire into ctest on hosts
# that have no Emscripten.  To get one:
#
#   git clone --depth 1 https://github.com/emscripten-core/emsdk /tmp/emsdk
#   /tmp/emsdk/emsdk install latest && /tmp/emsdk/emsdk activate latest
#   . /tmp/emsdk/emsdk_env.sh
#
# NOTE: this script deliberately does NOT build the `tur_wasm` cmake target.
# That target has a POST_BUILD step copying the module into web/public/, which
# are TRACKED files -- so running it to check that something compiles silently
# restages the deployed web bundle, with whatever Emscripten version happens to
# be installed. Compiling the translation units directly keeps verification
# free of side effects. Build tur_wasm when you mean to deploy.
#
# See docs/archive/refinement-types-plan.md (phase RT5a).

set -uo pipefail
cd "$(dirname "$0")/.."

if ! command -v emcc >/dev/null 2>&1; then
  echo "SKIP refine-wasm: emcc not on PATH (source emsdk_env.sh to enable)"
  exit 0
fi
if ! command -v node >/dev/null 2>&1; then
  echo "SKIP refine-wasm: node not on PATH"
  exit 0
fi

OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT
rc=0

# 1. Compile each refine translation unit with the `tur_wasm` target's flags.
#    -pedantic is load-bearing: it is what caught the __int128 the exact
#    rational arithmetic originally used.
FLAGS=(-Isrc -Isrc/compiler -Isrc/passes -Isrc/runtime -Isrc/async -Isrc/web
       -O2 -Wall -Wextra -Wno-unused-parameter -std=c11 -pedantic -D_DEFAULT_SOURCE)

for f in src/compiler/refine_*.c; do
  if ! out=$(emcc -c "$f" "${FLAGS[@]}" -o "$OUT/obj.o" 2>&1) || [ -n "$out" ]; then
    echo "FAIL refine-wasm: $f"
    echo "$out" | head -20
    rc=1
  fi
done
[ $rc -eq 0 ] && echo "PASS refine-wasm: $(ls src/compiler/refine_*.c | wc -l) sources compile clean at wasm32"

# 2. Link the solver unit test to wasm and run its checks under node.
if ! emcc tests/unit/refine_solver.c src/compiler/refine_*.c \
          src/runtime/arena.c src/runtime/buf.c \
          "${FLAGS[@]}" -sEXIT_RUNTIME=1 -sINITIAL_MEMORY=67108864 \
          -o "$OUT/refsolver.js" 2>"$OUT/link.err"; then
  echo "FAIL refine-wasm: link"
  head -20 "$OUT/link.err"
  exit 1
fi

result=$(node "$OUT/refsolver.js" 2>&1); run_rc=$?
echo "  wasm32: $result"
if [ $run_rc -ne 0 ] || ! grep -q "0 failure" <<< "$result"; then
  echo "FAIL refine-wasm: solver checks did not pass at wasm32"
  rc=1
else
  echo "PASS refine-wasm: solver checks agree with the native build at wasm32"
fi

exit $rc
