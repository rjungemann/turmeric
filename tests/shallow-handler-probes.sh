#!/usr/bin/env bash
# tests/shallow-handler-probes.sh -- F2.6 acceptance probes for shallow effect
# handlers (`handle-shallow`, docs/archive/compiled-shallow-handlers-plan.md).
#
# These exercise shallow shapes that leave the CPS subset (a resume that
# re-performs), which lower to the fiber fallback.  As of the F2.4 tail the
# compiled fiber path implements full shallow semantics (via the
# `shallow_consumed` bubble-up), so compiled and interpreter must AGREE; the
# probe asserts that agreement plus the unhandled-escape case.  The distinguishing
# 105-vs-10 result is also pinned as an ordinary fixture (effect-shallow-handler,
# both harnesses); single-perform shapes live in effect-shallow-basic /
# effect-shallow-multishot.

set -u
cd "$(dirname "$0")/.."

TUR="${TUR_BIN:-./build/tur}"
[ -x "$TUR" ] || { echo "shallow-probes: $TUR not built" >&2; exit 2; }

# The interpreter intentionally never frees its process-lifetime closures
# (CLAUDE.md), so run it with leak detection off, like the other interp harnesses.
export ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}detect_leaks=0"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tur-shallow-probes.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

PASS=0 FAIL=0
ok()   { PASS=$((PASS+1)); printf 'PASS %s\n' "$1"; }
bad()  { FAIL=$((FAIL+1)); printf 'FAIL %s -- %s\n' "$1" "$2"; }

# A body that performs Op twice; an inner handler (deep or shallow) resuming 5;
# an outer deep handler resuming 100.  Deep inner catches both performs (5+5=10);
# shallow inner catches the first (5), the re-perform escapes to the outer (100),
# for 5+100=105.  $1 = handle | handle-shallow.
nest_program() {
  cat <<EOF
(defeffect Op [] :int)
(defn twice [] : int (+ (perform (Op)) (perform (Op))))
(defn inner [] : int ($1 (twice) (Op [] k) (resume k 5)))
(defn run [] : int (handle (inner) (Op [] k) (resume k 100)))
(defn main [] : int (println (run)) 0)
EOF
}

# --- Probe 1: interpreter shallow re-perform escapes to the enclosing handler.
nest_program handle-shallow > "$WORK/nest_shallow.tur"
out="$("$TUR" --interpret "$WORK/nest_shallow.tur" 2>/dev/null)"
[ "$out" = "105" ] && ok "interp-nested-shallow-escapes-to-outer (105)" \
                    || bad "interp-nested-shallow-escapes-to-outer" "got '$out', want 105"

# --- Probe 2: the deep counterpart re-catches both performs (contrast).
nest_program handle > "$WORK/nest_deep.tur"
out="$("$TUR" --interpret "$WORK/nest_deep.tur" 2>/dev/null)"
[ "$out" = "10" ] && ok "interp-nested-deep-recatches (10)" \
                  || bad "interp-nested-deep-recatches" "got '$out', want 10"

# --- Probe 3: a shallow resume whose body re-performs with NO enclosing handler
# is unhandled (the handler was removed).
cat > "$WORK/single_shallow.tur" <<'EOF'
(defeffect Op [] :int)
(defn twice [] : int (+ (perform (Op)) (perform (Op))))
(defn run [] : int (handle-shallow (twice) (Op [] k) (resume k 5)))
(defn main [] : int (println (run)) 0)
EOF
err="$("$TUR" --interpret "$WORK/single_shallow.tur" 2>&1 >/dev/null)"
printf '%s' "$err" | grep -qi "unhandled effect" \
  && ok "interp-shallow-reperform-unhandled" \
  || bad "interp-shallow-reperform-unhandled" "expected 'unhandled effect', got: $err"

# --- Probe 4: F2.4 tail -- the compiled fiber path implements full shallow
# semantics, so a multi-perform shallow handler compiles and AGREES with the
# interpreter (nested inner-shallow/outer-deep -> 105, not the deep 10).
if CC=cc "$TUR" build "$WORK/nest_shallow.tur" -o "$WORK/nest_shallow.bin" 2>/dev/null; then
  cout="$("$WORK/nest_shallow.bin" 2>/dev/null)"
  [ "$cout" = "105" ] && ok "compiled-nested-shallow-matches-interp (105)" \
                      || bad "compiled-nested-shallow-matches-interp" "got '$cout', want 105"
else
  bad "compiled-nested-shallow-matches-interp" "compiled build failed (shallow fiber path should compile now)"
fi

# --- Probe 5: `^multishot` + `handle-shallow` compose on the interpreter
# (single perform: resume twice -> 10 + 20 = 30).
cat > "$WORK/ms_shallow.tur" <<'EOF'
(defeffect Ask [] :int)
(defn use-ask [] : int (perform (Ask)))
(defn run [] : int
  (handle-shallow (use-ask) (Ask [] ^multishot k) (+ (resume k 10) (resume k 20))))
(defn main [] : int (println (run)) 0)
EOF
out="$("$TUR" --interpret "$WORK/ms_shallow.tur" 2>/dev/null)"
[ "$out" = "30" ] && ok "interp-multishot-shallow-composes (30)" \
                  || bad "interp-multishot-shallow-composes" "got '$out', want 30"

# --- Probe 6: no false TUR-W0033.  A deep outer clause over a shallow inner
# handler for the same effect IS reachable (the effect escapes the shallow
# handler), so the reachability pass must not warn it unreachable.
w33="$(CC=cc "$TUR" build "$WORK/nest_shallow.tur" -o "$WORK/nest_shallow.b2" 2>&1 >/dev/null)"
if printf '%s' "$w33" | grep -qiE "W0033|unreachable"; then
  bad "no-false-unreachable-warning-on-shallow-outer" "unexpected TUR-W0033: $w33"
else
  ok "no-false-unreachable-warning-on-shallow-outer"
fi

printf 'shallow-handler probes: %d passed, %d failed\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
