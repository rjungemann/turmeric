#!/usr/bin/env bash
# tests/check-sf-let-bind-inner-call.sh -- regression guard for
# docs/archive/history/let-bound-sf-loses-outer-arg-type-when-inner-captures.md.
#
# A signal-function (SF) is a two-level closure
# (fn [sig] (fn [t] ... (sig t) ...)).  When the inner closure captures and
# calls the outer `^fat` parameter `sig`, let-binding the SF result and piping
# a typed `^fat` value into it used to mis-elaborate the outer argument type as
# the inner `t`'s :float instead of the declared (fn [float] float).  The
# binding `sf` was wrongly marked as a closure *value* (closure_fn_binding),
# which swapped in the inner env+arg thunk type and dropped sf's real first
# parameter, so (sf input) was rejected with a spurious TUR-E0001:
#
#   error [TUR-E0001]: function 'sf' arg 1: expected float, got (fn [float] : float)
#
# The fix (src/compiler/elab_forms.c) routes a call that yields a thin
# (non-boxed) function pointer through returns_closure_fn_binding instead of
# closure_fn_binding, so `sf` keeps its declared outer signature while a chained
# call still sees its result as a closure.
#
# This is a `tur check` (type-check) guard.  The native codegen for this exact
# two-level capturing-closure-return shape is a separate, pre-existing defect
# tracked in
# docs/archive/history/sf-two-level-closure-return-miscompiles-out-binding.md, so this
# guard deliberately stops at `tur check` rather than building/running.
set -euo pipefail
cd "$(dirname "$0")/.."

TUR="${TUR:-./build/tur}"
[ -x "$TUR" ] || { echo "tests/check-sf-let-bind-inner-call.sh: $TUR not built" >&2; exit 2; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# Case A -- inner closure does NOT call the captured `sig`.  Always type-checked.
cat > "$WORK/sf_no_call.tur" <<'EOF'
(defn make-sf-no-call []
  (fn [^fat sig : (fn [float] float)]
    (fn [t : float] : float 0.0)))

(defn drive-a [^fat input : (fn [float] float)] : float
  (let [sf  (make-sf-no-call)
        out (sf input)]
    (out 0.0)))
EOF

# Case B -- inner closure DOES call (sig t).  Regressed; must type-check now.
cat > "$WORK/sf_with_call.tur" <<'EOF'
(defn make-sf-with-call []
  (fn [^fat sig : (fn [float] float)]
    (fn [t : float] : float (sig t))))

(defn drive-b [^fat input : (fn [float] float)] : float
  (let [sf  (make-sf-with-call)
        out (sf input)]
    (out 0.0)))
EOF

status=0

if ! "$TUR" check "$WORK/sf_no_call.tur" > "$WORK/a.out" 2>&1; then
    echo "FAIL sf-let-bind-inner-call: case A (no inner call) failed to type-check:"
    sed 's/^/    /' "$WORK/a.out"
    status=1
fi

if ! "$TUR" check "$WORK/sf_with_call.tur" > "$WORK/b.out" 2>&1; then
    echo "FAIL sf-let-bind-inner-call: case B (inner calls sig) failed to type-check:"
    sed 's/^/    /' "$WORK/b.out"
    echo "    (regression of let-bound-sf-loses-outer-arg-type-when-inner-captures.md)"
    status=1
fi

if [ "$status" -ne 0 ]; then
    exit 1
fi

echo "PASS sf-let-bind-inner-call"
