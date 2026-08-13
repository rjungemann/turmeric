#!/usr/bin/env bash
# tests/check-monomorph-name-collision.sh
#
# Behavioural half of the guard for
# docs/archive/history/concrete-codegen-layout-kind-enumerations-drift.md.
#
# tests/check-typekind-mangle-exhaustive.sh reads the SOURCE of the two
# TypeKind switches. This one reads what they actually EMIT: it compiles a
# handful of programs that instantiate one parametric ADT at two different
# type arguments, and asserts that the two instantiations do not land on one
# monomorph C name.
#
# Why both. Every monomorph typedef and constructor is wrapped in
# `#ifndef TUR_TY_<name>` / `#ifndef TUR_FN_<name>`, so when two distinct
# types mangle to the same name the SECOND definition is preprocessed away
# and the second type silently adopts the first's layout -- an int64 field
# read back as a double, with no diagnostic. Two properties catch that:
#
#   A. No name is defined twice with two different bodies. Generic: it holds
#      for any emitted program, and it is exactly the condition the #ifndef
#      guards hide.
#   B. Each repro's two instantiations produce the two names listed with it.
#      Needed because A alone cannot see a merge whose two layouts happen to
#      coincide -- `(Box (| int float))` and `(Box (| int cstr))` both carry a
#      `tur_tagged_t` field, so they merged for months with identical bodies,
#      while every name-keyed specialization downstream still collided.
#
# Needs a built ./build/tur (override with TUR=). Skips if absent.

set -uo pipefail
cd "$(dirname "$0")/.."

TUR="${TUR:-./build/tur}"
if [ ! -x "$TUR" ]; then
    echo "SKIP monomorph-name-collision -- $TUR not built"
    exit 0
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tur-monomorph-collision.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
FAIL=0

# ---------------------------------------------------------------------------
# repro <slug> <extra-tur-flags> <expected-name>... << 'EOF' <program> EOF
# ---------------------------------------------------------------------------
repro() {
    local slug="$1"; shift
    local flags="$1"; shift
    local expected=("$@")
    local src="$WORK/$slug.tur"
    local out="$WORK/$slug.c"
    cat > "$src"

    # shellcheck disable=SC2086  # $flags is an intentional word list
    if ! "$TUR" emit-c $flags "$src" > "$out" 2> "$WORK/$slug.err"; then
        echo "FAIL monomorph-name-collision/$slug -- emit-c failed:"
        sed 's/^/    /' "$WORK/$slug.err" | head -20
        FAIL=1
        return
    fi

    python3 - "$slug" "$out" "${expected[@]}" <<'PY'
import re, sys
slug, path = sys.argv[1], sys.argv[2]
expected = sys.argv[3:]
src = open(path).read()
rc = 0

# (A) every `#ifndef TUR_TY_<name>` / `#ifndef TUR_FN_<name>` block that names
# the same guard must carry the same body. A differing second body is the
# definition the preprocessor throws away.
bodies = {}
for m in re.finditer(r'#ifndef (TUR_(?:TY|FN)_\w+)\n#define \1\n(.*?)\n#endif',
                     src, flags=re.S):
    guard, body = m.group(1), m.group(2).strip()
    prev = bodies.setdefault(guard, body)
    if prev != body:
        print(f"FAIL monomorph-name-collision/{slug} -- {guard} defined twice "
              f"with different bodies:")
        for line in ("--- first\n" + prev + "\n--- second\n" + body).split('\n'):
            print("     " + line)
        rc = 1
        break

# (B) the instantiations this repro is about each got their own name.
for name in expected:
    if not re.search(r'\b' + re.escape(name) + r'\b', src):
        print(f"FAIL monomorph-name-collision/{slug} -- expected monomorph "
              f"`{name}` is absent; the two instantiations merged onto one name.")
        rc = 1

if rc == 0:
    print(f"PASS monomorph-name-collision/{slug} "
          f"({len(bodies)} guarded definitions, {len(expected)} names checked)")
sys.exit(rc)
PY
    [ $? -ne 0 ] && FAIL=1
    return 0
}

# Finding 1, the report's own repro: two function types under one `opaque`
# token routed an (int64_t)-cast closure handle through a `double` field.
repro fn-payload "" tur_adt_Box__fn1_int__float tur_adt_Box__fn1_int__int <<'EOF'
(defdata Box [a] (MkBox a))
(defn use-half [b : (Box (fn [int] float))] : float (match b (MkBox f) (f 15)))
(defn use-inc  [b : (Box (fn [int] int))]   : int   (match b (MkBox f) (f 41)))
(defn main [] : int 0)
EOF

# The reference family: `ref`/`rc`/`weak`/`ptr` tokens used to drop their inner
# type, so `(Box rc<int>)` and `(Box rc<float>)` merged through arms that
# looked fine.
repro rc-inner "" tur_adt_Box__rc_int tur_adt_Box__rc_float <<'EOF'
(defdata Box [a] (MkBox a))
(defn take-ri [b : (Box rc<int>)]   : int 1)
(defn take-rf [b : (Box rc<float>)] : int 2)
(defn main [] : int 0)
EOF

# Finding 2: `type_eq` compares union members, the mangler dropped them.
repro union-members "" tur_adt_Box__union2_int_float tur_adt_Box__union2_int_cstr <<'EOF'
(defdata Box [a] (MkBox a))
(defn take-u1 [b : (Box (| int float))] : int (match b (MkBox v) 1))
(defn take-u2 [b : (Box (| int cstr))]  : int (match b (MkBox v) 2))
(defn main [] : int 0)
EOF

# Finding 2, the layout-visible one: a contract IS its base type in C, so two
# contracts over different bases are two different layouts. They shared one
# `tur_adt_Box__contract` typedef whose surviving field was `int64_t`, while
# the float arm's match read it back with a `(double)` conversion.
#
# The expected names are the BASE ones, not `..._contract_int` /
# `..._contract_float`. A refinement in type-argument position is now peeled to
# its base (rt_peel_type_arg_contract, TUR-W0380), so `(Box #refine{x : int})`
# is `(Box int)` before it ever reaches the mangler. The property this repro
# defends is unchanged and still the whole point -- two contracts over
# different bases must be two distinct monomorphs carrying the right field
# widths -- and it still holds: `tur_adt_Box__int` gets `int64_t _0`,
# `tur_adt_Box__float` gets `double _0`. Only the spelling of the names moved.
repro contract-base "" \
      tur_adt_Box__int tur_adt_Box__float <<'EOF'
(defdata Box [a] (MkBox a))
(defn take-ci [b : (Box #refine{x : int   | (> x 0)})]   : int (match b (MkBox v) 1))
(defn take-cf [b : (Box #refine{y : float | (> y 0.0)})] : int (match b (MkBox v) 2))
(defn main [] : int 0)
EOF

# macos-int-conversion-carrier-pointer-straddles: a `(c-fn ...)` and an
# ordinary `(fn ...)` of the same signature are type_eq-DISTINCT whenever the
# latter is a capturing (boxed) closure -- types.c:118-122 refuses to equate
# them, because a fat closure must never flow into a raw C callback sink.  The
# mangle did not carry `cfnptr`, so both landed on `fn1_int__int`: two registry
# entries, one C name, and the second `#ifndef` block preprocessed away.  This
# is a property-(A) repro -- the two guarded bodies differed (`void *` vs
# `tur_fnptr_int64_t_int64_t_t` in the ctor slot) with no diagnostic from any
# compiler.  Note the two views must NOT meet at a call site; the checker
# rejects that on its own, which is why this needs two separate functions.
repro cfnptr-vs-boxed "" tur_adt_Option__fn1_int__int tur_adt_Option__fnc1_int__int <<'EOF'
(defn bare-inc [x : int] : int (+ x 1))
(defn use-c [v : int] : int
  (let [o (:: (some bare-inc) (Option (c-fn [int] int)))]
    (if (some? o) ((unwrap o) v) (- 0 1))))
(defn use-t [v : int] : int
  (let [c 41
        o (some (fn [x : int] : int (+ x c)))]
    (if (some? o) ((unwrap o) v) (- 0 1))))
(defn main [] : int 0)
EOF

echo
if [ "$FAIL" -eq 0 ]; then
    echo "monomorph-name-collision: all checks passed"
else
    echo "monomorph-name-collision: FAILED -- see docs/archive/history/concrete-codegen-layout-kind-enumerations-drift.md"
fi
exit "$FAIL"
