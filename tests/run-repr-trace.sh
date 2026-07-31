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

# Value-position bridges: a Vec of by-value (Option int) elements crosses
# concrete->carrier on push (heap reinterpret of the Vec handle) and
# carrier->concrete on the ascribed vec-get read (aggregate).  A by-value
# aggregate through a bare `:fn` poly-carrier param heap-boxes on the way
# in (agg-box) and derefs back on the way out (agg-unbox).
cat > "$tmp/vprobe.tur" <<'EOF'
(defn opt-get [o : (Option int)] : int (unwrap-or o -1))
(defn call-it [f : fn x : (Option int)] : int (f x))
(defn main [] : int
  (let [vo (:: (vec-new) (Vec (Option int)))]
    (vec-push! vo (:: (some 5) (Option int)))
    (let [a (:: (vec-get vo 0) (Option int))]
      (println (unwrap-or a -1))))
  (println (call-it opt-get (some 7)))
  0)
EOF

trace="$("$TUR" emit-c --emit-abi-trace "$tmp/probe.tur" 2>&1 >/dev/null | grep '^repr-trace')"
vtrace="$("$TUR" emit-c --emit-abi-trace "$tmp/vprobe.tur" 2>&1 >/dev/null | grep '^repr-trace')"

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

# Value-position bridge classifications, from the second probe.
vcheck() {
  local label="$1" pattern="$2"
  if echo "$vtrace" | grep -qE "$pattern"; then
    echo "  ok  $label"
  else
    echo "  FAIL $label -- no line matching: $pattern"
    rc=1
  fi
}
vcheck "heap reinterpret crossing traced" "^repr-trace bridge concrete->carrier heap-reinterpret "
vcheck "aggregate crossing traced"        "^repr-trace bridge carrier->concrete aggregate \(type-app Option int\)$"
vcheck "agg-box traced"                   "^repr-trace bridge agg-box tur_adt_Option__int$"
vcheck "agg-unbox traced"                 "^repr-trace bridge agg-unbox tur_adt_Option__int$"

# Increment 4 stage 2: the repr_of shadow log.  Two smokes: (a) a known
# mid-migration shape (a phantom-parametric by-value app bound from a carrier
# producer) fires a `repr-shadow` disagreement line -- proving the shadow
# instrument is alive; (b) a fully-consolidated shape (increment 3's vec
# element protocol) fires none -- proving the spec does not false-positive on
# clean code.  When stage 3 migrates the binding site, (a) starts failing:
# that is the signal to move the smoke to the next unmigrated shape (or
# retire it if the log is empty corpus-wide).
cat > "$tmp/shadow-dirty.tur" <<'EOF'
(defstruct ArrShadow [a] [raw : int])
(defn get-raw [w : (ArrShadow int)] : int (.raw w))
(defn main [] : int
  (let [w (:: (make-struct ArrShadow 7) (ArrShadow int))]
    (println (get-raw w)))
  0)
EOF
strace="$("$TUR" emit-c --emit-abi-trace "$tmp/shadow-dirty.tur" 2>&1 >/dev/null | grep '^repr-shadow' || true)"
if echo "$strace" | grep -q "^repr-shadow binding let-bind .*want=byval-agg got=carrier-i64"; then
  echo "  ok  shadow disagreement fires on mid-migration shape"
else
  echo "  FAIL shadow disagreement -- expected a 'repr-shadow binding let-bind ... want=byval-agg got=carrier-i64' line, got:"
  echo "$strace"
  rc=1
fi

cat > "$tmp/shadow-clean.tur" <<'EOF'
(defstruct FzShadow [a : int])
(defn main [] : int
  (let [v (:: (vec-new) (Vec FzShadow))]
    (vec-push! v (FzShadow 31))
    (let [b (:: (vec-get v 0) FzShadow)]
      (println (.a b))))
  0)
EOF
ctrace="$("$TUR" emit-c --emit-abi-trace "$tmp/shadow-clean.tur" 2>&1 >/dev/null | grep '^repr-shadow' || true)"
if [ -z "$ctrace" ]; then
  echo "  ok  no shadow noise on consolidated shape"
else
  echo "  FAIL shadow noise -- expected no repr-shadow lines on the clean shape, got:"
  echo "$ctrace"
  rc=1
fi

if [ $rc -ne 0 ]; then
  echo "FAIL repr-trace"
  echo "--- fn-param trace ---"
  echo "$trace"
  echo "--- value-position trace ---"
  echo "$vtrace"
else
  echo "PASS repr-trace"
fi
exit $rc
