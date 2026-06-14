---
title: Instance-method Path A spec doesn't propagate constraint-var bindings to callee specs
severity: blocks Eq Vec rewrite via constrained-poly helpers -- DOWNGRADED 2026-06-14: reproduces only in specific build orderings; the earlier bridge-side strip fix in commit (M5 emit: preserve EX_ASCRIBE for byval->carrier bridge) handles the underlying cases
date: 2026-06-14
---

## Update 2026-06-14: Likely subsumed by bridge-side fix; attempted fix had marginal regression

Subsequent investigation (same session) showed that the original
repro shapes (gap2b and g4-instance) actually compile and run
correctly with just the earlier bridge-side EX_ASCRIBE strip fix
in place.  The "no spec interned for callee" symptom may have been
specific to a partial state or build ordering.

An attempted fix that extended `elab_typeclasses.c`'s instance-method
abi_bindings to also record constraint-var bindings (derived from
`recv_def->type_params[param_idx]`) was implemented and reverted:

- Compiled cleanly, didn't change emission for any tested case.
- Caused a net regression of 1 in the suite (1566/88 with the fix
  vs 1567/87 without) -- one test that was passing started failing.

The patch is principled (records info that's logically in scope at
the spec-intern site) but apparently affects something subtler than
predicted in the spec-matching / interning chain.  Future fix
attempts should isolate the regressing test first.

## Summary

When an instance method's Path A spec body calls a sibling constrained-
polymorphic defn, no per-receiver-type spec gets interned for the
callee.  The callee's int64 carrier base ends up called with by-value
spec params (e.g. `callee(Vec__int, ...)` against `callee(int64_t,
...)`) — cc type error.

The same pattern works correctly when the OUTER caller is a regular
polymorphic defn (not an instance method) — confirmed by direct
side-by-side trace.

## Repro

```turmeric
(defn callee [A]
  [(Eq A)]
  [x : (Vec A) y : (Vec A) i : int len : int]
  : bool
  (if (= i len)
    true
    (if (eq? (:: (vec-get (:: x :int) i) A) (:: (vec-get (:: y :int) i) A))
      (callee x y (+ i 1) len)
      false)))

(defclass MyEq [a] (myeq? [x y] : bool))

(definstance MyEq [Vec]
  [(Eq A)]
  (myeq? [x y]
    (callee (:: x (Vec A)) (:: y (Vec A)) 0 (vec-len (:: x :int)))))

(defn main [] : int
  (let [a (:: (vec-of) (Vec int))
        b (:: (vec-of) (Vec int))]
    (vec-push! a 1) (vec-push! b 1)
    (if (.myeq? a b) 0 1)))
```

Build: `error: passing 'Vec__int' to parameter of incompatible type 'int64_t'`

Control (works):

```turmeric
(defn callee ...same as above...)

(defn outer [A]                          ;; regular outer, not instance
  [(Eq A)]
  [x : (Vec A) y : (Vec A) len : int]
  : bool
  (callee x y 0 len))

(defn main [] : int
  ...callers identical...
  (if (outer a b 1) 0 1))                ;; compiles + runs
```

## Trace (instrumented `emit_abi_register_call`'s composition pass)

**WORKING (regular outer)** — when callee is called from outer's
Path A spec body:

```
DBG g4 callee: outer_spec=outer__spec__bool_Vec__int_Vec__int_int64_t
  call.bindings[0]: name=A type.kind=36 (TY_TYVAR)
  outer.bindings[0]: name=A type.kind=3 (TY_INT)
```

Names line up (`A` == `A`), composition substitutes `A → int`,
arg_types[0] becomes Vec__int, by-value spec interned.

**BROKEN (instance method outer)** — when callee is called from
MyEq[Vec]'s Path A spec body:

```
DBG g4 callee: outer_spec=__inst_MyEq_myeq_qu_Vec__spec__bool_Vec__int_Vec__int
  call.bindings[0]: name=A type.kind=18 (TY_STRUCT, def=NULL — abstract tyvar)
  outer.bindings[0]: name=a type.kind=21 (TY_APP, (Vec int) — the class var resolution)
```

The outer spec tracks the CLASS VAR `a` resolution (= the full
receiver type `(Vec int)`), NOT the CONSTRAINT VAR `A` (= the element
type `int`).  When callee's `A` (which corresponds to the constraint
var) is composed against the outer's bindings, no match exists --
the call's `A → TY_STRUCT(NULL)` representation carries no name to
look up, and even if it did, the outer doesn't have a binding for
the constraint var.

## Root cause

The instance-method Path A spec interner only records the class
var(s) in its `bindings[]` array, with values like `a → (Vec int)`.
Constraint vars introduced by `[(Eq A)]` on the instance are NOT
recorded -- they're class-local tyvars that are supposed to be
resolved through the class-var's type arguments (`(Vec int)` →
element is `int`, so constraint var A in `(Eq A)` resolves to int).

But `emit_abi_register_call`'s composition pass doesn't know to
peek into the class var's type to extract constraint-var bindings.
It just looks up the call's binding name in the outer's bindings,
finds no match, and returns the call's binding type unchanged.

## Proposed fix directions

1. **At spec-intern time**: when interning an instance-method Path A
   spec, ALSO record constraint-var bindings derived from the
   class-var's resolved type args.  E.g. when interning
   `__inst_MyEq_myeq_qu_Vec__spec__bool_Vec__int_Vec__int`, record
   `{a → (Vec int), A → int}` instead of just `{a → (Vec int)}`.

2. **In the composition pass**: when the call's binding is
   `TY_STRUCT(NULL)` and the outer's only binding is the class var
   resolved to a TY_APP, walk the TY_APP's arg-chain to extract
   constraint-var values by position.  Heuristic; fragile.

(1) is the principled fix.  It needs identifying where instance-
method specs are interned in `emit_module.c` and extending the
binding recording to walk the constraint structure.  Estimated
2-4 hours of focused trace + code.

## Workaround

Don't call a constrained-polymorphic helper from inside an instance
method body.  The original `Eq Vec` body (using `(:: x :int)` +
existing carrier `vec-eq-loop` with a `^fat cmp` lambda) sidesteps
this because the carrier helpers don't need by-value specs.

## Validation under fix

`/tmp/g4-instance.tur` should compile and exit=0.
The Eq Vec rewrite that I attempted (which mirrors g4-instance's
shape) would also compile.

## Related

- `docs/upcoming/m5-residual-straddle-retirement.md` — Option D plan
  that this gap blocks.
- `docs/reported/m5-constrained-poly-wrong-instance-on-tyvar-receiver.md`
  — fixed earlier this session; addressed a different but adjacent
  dispatch bug in the same area.
