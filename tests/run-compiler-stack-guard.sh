#!/usr/bin/env bash
#
# The compiler's stack BACKSTOP (TUR-E0712).
#
# There is no depth cap any more -- emission runs on a large explicitly-sized
# stack (tur_run_on_big_stack, src/compiler/stack_guard.h), so how deeply a
# program may nest follows the stack rather than a constant.  What is left is a
# real-stack-headroom check that fires when the walk genuinely runs out.
#
# That backstop is hard to assert from an ordinary fixture: on the default
# stack the nesting needed to trip it is impractically large, and a fixture
# tuned to sit just under a cliff is exactly the stack-size canary the retired
# one became.  So shrink the stack instead and keep the program small --
# TUR_STACK_MB is the knob, and driving it is the point of this harness.
#
# Asserts both directions, which is what makes it a test rather than a smoke
# check: the same source compiles on a normal stack and is refused on a tiny
# one.
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TUR="${TUR_BIN:-$ROOT/build/tur}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

fails=0
ok()  { echo "ok -- $1"; }
bad() { echo "FAIL -- $1"; fails=$((fails + 1)); }

if [ ! -x "$TUR" ]; then
    echo "TUR_SKIP: no tur binary at $TUR"
    exit 0
fi

# ~400 nested (+ ...) levels: comfortable on the default stack, far past what
#8 MiB of ASan-inflated frames can hold.
SRC="$WORK/deep.tur"
{
    printf '(defn main [] : int\n  (println '
    for _ in $(seq 1 400); do printf '(+ '; done
    printf '1'
    for _ in $(seq 1 400); do printf ' 1)'; done
    printf ')\n  0)\n'
} > "$SRC"

# 1. Default stack: must compile.
if out="$("$TUR" emit-c "$SRC" 2>&1 >/dev/null)"; then
    ok "400-deep nesting compiles on the default stack"
else
    bad "400-deep nesting should compile on the default stack; got: $(echo "$out" | head -2)"
fi

# 2. Tiny stack: must raise TUR-E0712 rather than crashing.  A stack-overflow
#    abort (the pre-fix behaviour) exits 134 and prints no diagnostic, so
#    checking for the code distinguishes "guarded" from "crashed".
out="$(TUR_STACK_MB=8 "$TUR" emit-c "$SRC" 2>&1 >/dev/null)"; rc=$?
if [ "$rc" = "134" ] || [ "$rc" = "139" ]; then
    bad "tiny stack aborted (rc=$rc) instead of diagnosing -- the guard lost the race"
elif [ "$rc" = "0" ]; then
    bad "tiny stack compiled; the backstop did not fire"
elif echo "$out" | grep -q 'TUR-E0712'; then
    ok "tiny stack raises TUR-E0712 (rc=$rc)"
else
    bad "tiny stack failed without TUR-E0712: $(echo "$out" | head -2)"
fi

# 3. The message must name the real quantity.  The retired text said "exceeds
#    the emitter's depth limit (40)" -- a number that no longer exists.
#
#    Either phase may be the one to catch it, and which one depends on the
#    stack size and the shape of the input: the elaborator recurses first, so
#    on a very small stack it reports ("the compiler's stack"); with more room
#    the walk reaches emission and the emitter reports ("the emitter's
#    stack").  Both are correct, so assert the shape, not the phase.
if echo "$out" | grep -qE "exhausted the (compiler|emitter)'s stack at depth"; then
    ok "diagnostic names the exhausted stack, not a phantom depth limit"
else
    bad "diagnostic should name the exhausted stack; got: $(echo "$out" | grep TUR-E0712 | head -1)"
fi

# 4. It must tell the user the knob that fixes it.
if echo "$out" | grep -q 'TUR_STACK_MB'; then
    ok "diagnostic points at TUR_STACK_MB"
else
    bad "diagnostic should mention TUR_STACK_MB"
fi

if [ "$fails" -eq 0 ]; then
    echo "summary: emit stack guard -- all checks passed"
    exit 0
fi
echo "summary: emit stack guard -- $fails failed"
exit 1
