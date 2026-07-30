#!/usr/bin/env bash
# tests/run-repr-trace.sh -- smoke for the representation trace
# (representation-consolidation-meta-plan, increment 0).
#
# `--emit-abi-trace` now also prints `repr-trace` lines: one per fn-typed
# parameter naming the representation the elaborator routed it onto
# (carrier / fat / cfnptr / thin-fn + reason), and one per emit-side
# representation bridge (bare-to-fat, poly-to-fat).  Consolidation
# increments diff these traces to prove "only the intended boundaries
# moved"; this smoke pins that each classification actually prints.
#
# Env:
#   TUR_BIN   compiler to test (default ./build/tur)

set -uo pipefail
cd "$(dirname "$0")/.."

export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}"
TUR="${TUR_BIN:-./build/tur}"

if [ ! -x "$TUR" ]; then
  echo "SKIP repr-trace: no compiler at $TUR"
  exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

cat > "$tmp/probe.tur" <<'EOF'
(defn use-carrier [f : (fn [int] int)] : int (f 1))
(defn use-fat [^fat f : (fn [] int)] : int (f))
(defn use-thin-fx [f : (fn [] #fx{Write} int)] : int (f))
(defn top2 [x : int] : int (+ x 2))
(defn main [] : int
  (let [k 5]
    (println (use-carrier top2))
    (println (use-fat (fn [] k))))
  0)
EOF

trace="$("$TUR" emit-c --emit-abi-trace "$tmp/probe.tur" 2>&1 >/dev/null | grep '^repr-trace')"

rc=0
check() {
  local label="$1" pattern="$2"
  if echo "$trace" | grep -qE "$pattern"; then
    echo "  ok  $label"
  else
    echo "  FAIL $label -- no line matching: $pattern"
    rc=1
  fi
}

# The probe file's own params (lines 1-3).  Stdlib params also appear in the
# trace; the leading `^repr-trace 1:`/`2:`/`3:` anchors pin OUR lines.
check "carrier param traced"        "^repr-trace 1:[0-9]+ fn-param f carrier$"
check "fat param traced"            "^repr-trace 2:[0-9]+ fn-param f fat$"
check "thin-fn + reason traced"     "^repr-trace 3:[0-9]+ fn-param f thin-fn effect-row$"
# Emit-side bridges (any location -- stdlib exercises them too).
check "bare-to-fat bridge traced"   "^repr-trace [0-9]+:[0-9]+ bridge bare-to-fat arity=[0-9]+ (typed|int64)-shim$"

if [ $rc -ne 0 ]; then
  echo "FAIL repr-trace"
  echo "--- full trace ---"
  echo "$trace"
else
  echo "PASS repr-trace"
fi
exit $rc
