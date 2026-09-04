#!/usr/bin/env bash
# tests/turi/repl-spice-jit.sh -- J2 (jit-engine-plan section 3.3).
#
# Drives `tur repl --engine jit` inside fixture spices and asserts the
# IN-PROCESS spice build (MIR engine via the TurSpiceJitHook) behaves like
# the subprocess + dlopen path it replaces: load, bare/qualified calls,
# float marshalling, high-arity via the __ffi shim, compile-error surface,
# and (reload) self-heal with in-session rebinding.
#
# Self-skips (exit 0) when the tur binary carries no JIT engine
# (-DTUR_JIT=ON not configured) so the script is safe to run against any
# build; the ctest registration is additionally gated on TUR_JIT.

set -uo pipefail
cd "$(dirname "$0")/../.."

TUR="${TUR:-./build-turjit/tur}"
[ -x "$TUR" ] || TUR=./build/tur
[ -x "$TUR" ] || { echo "tests: no tur binary found" >&2; exit 2; }
case "$TUR" in
    /*) TUR_BIN="$TUR" ;;
    *)  TUR_BIN="$(cd "$(dirname "$TUR")" && pwd)/$(basename "$TUR")" ;;
esac
export TUR_BIN

# Capability probe -- capture, don't pipe into grep -q (pipefail SIGPIPE).
# Probed with a NONEXISTENT input, matching run-jit.sh and run-engine-select.sh:
# P0 moved cmd_jit's input scan ahead of everything else, so a bare `tur jit`
# prints usage on every build and never discriminates.  Only the "carries no
# JIT engine" answer does.  This matters more since the `jit` graduation
# (2026-08-17) pointed `tur repl` at engine selection: `repl --engine jit` on a
# build with no engine is now a hard error rather than a silently-ignored flag,
# so a probe that fails to skip turns into a wall of failures instead of one.
probe=$("$TUR_BIN" jit /nonexistent-tur-jit-probe.tur 2>&1 || true)
case "$probe" in
  *"carries no JIT"*)
     echo "SKIP repl-spice-jit ($TUR_BIN carries no JIT engine)"; exit 0 ;;
esac

WORK="$(mktemp -d -t tur-j2.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT
export ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}detect_leaks=0"

PASS=0
FAIL=0
pass() { PASS=$((PASS + 1)); echo "PASS $1"; }
fail() { FAIL=$((FAIL + 1)); echo "FAIL $1 -- $2"; }

# -------- fixture spice ---------------------------------------------------
PROJ="$WORK/proj"
mkdir -p "$PROJ/src"
cat > "$PROJ/build.tur" <<'EOF'
(defpackage j2-jit-fixture)
EOF
# Two modules with an intra-spice import, one in a file whose name does NOT
# match its module (the shadow-dir case), one high-arity export (the __ffi
# shim case).
cat > "$PROJ/src/sh.tur" <<'EOF'
(defmodule sh
  (export add42 scale sum12)
  (defn add42 [x :int] :int (+ x 42))
  (defn scale [x :float y :float] :float (* x y))
  (defn sum12 [a :int b :int c :int d :int e :int f :int
               g :int h :int i :int j :int k :int l :int] :int
    (+ a (+ b (+ c (+ d (+ e (+ f (+ g (+ h (+ i (+ j (+ k l)))))))))))))
EOF
cat > "$PROJ/src/other.tur" <<'EOF'
(defmodule oth
  (export triple)
  (import sh)
  (defn triple [x :int] :int (* 3 x)))
EOF

run() {
    (cd "$PROJ" && printf '%b' "$1" | "$TUR_BIN" repl --engine jit 2>&1)
}

# -------- scenario 1: in-process load (no .so artifact) -------------------
out=$(run ':quit\n')
if   grep -q "Loaded spice from .* (4 exports)" <<< "$out" \
  && ! ls "$PROJ"/.tur-repl-cache/lib-*.so >/dev/null 2>&1; then
    pass "j2-load-in-process"
else
    fail "j2-load-in-process" "$out"
fi

# -------- scenario 2: calls -- bare, qualified, float, high-arity ---------
# Float probe uses a NON-INTEGER result (1.5 * 3.0 = 4.5) so int/float
# marshalling divergence cannot hide behind a whole-number answer.
out=$(run '(add42 1)\n(oth/triple 14)\n(scale 1.5 3.0)\n(sum12 1 2 3 4 5 6 7 8 9 10 11 12)\n:quit\n')
if   grep -qx '=> 43' <<< "$out" \
  && grep -qx '=> 42' <<< "$out" \
  && grep -qx '=> 4.5' <<< "$out" \
  && grep -qx '=> 78' <<< "$out"; then
    pass "j2-calls"
else
    fail "j2-calls" "$out"
fi

# -------- scenario 3: compile error surfaces the rebuild hint -------------
P2="$WORK/bad"
mkdir -p "$P2/src"
cat > "$P2/build.tur" <<'EOF'
(defpackage j2-bad)
EOF
cat > "$P2/src/sh.tur" <<'EOF'
(defmodule sh (export f) (defn f [] :int (no-such-name)))
EOF
out=$(cd "$P2" && echo ':quit' | "$TUR_BIN" repl --engine jit 2>&1)
if   grep -q "spice rebuild failed" <<< "$out" \
  && grep -q "(reload)" <<< "$out"; then
    pass "j2-compile-error-hint"
else
    fail "j2-compile-error-hint" "$out"
fi

# -------- scenario 4: (reload) self-heal + in-session rebinding -----------
FIFO="$WORK/fifo"
mkfifo "$FIFO"
OUT4="$P2/out.log"
(cd "$P2" && "$TUR_BIN" repl --engine jit < "$FIFO") >"$OUT4" 2>&1 &
REPL_PID=$!
exec 3>"$FIFO"
sleep 1
cat > "$P2/src/sh.tur" <<'EOF'
(defmodule sh (export f) (defn f [] :int 42))
EOF
sleep 1
printf '(reload)\n(f)\n:quit\n' >&3
exec 3>&-
wait "$REPL_PID"
if   grep -q '(reload) loaded 1 export' "$OUT4" \
  && grep -qx '=> 42' "$OUT4"; then
    pass "j2-reload-self-heal"
else
    fail "j2-reload-self-heal" "$(cat "$OUT4")"
fi
rm -f "$FIFO"

echo
echo "repl-spice-jit: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
