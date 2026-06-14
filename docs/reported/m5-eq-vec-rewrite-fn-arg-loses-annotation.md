---
title: Eq Vec instance body cannot pass a typed (fn [A A] bool) arg to a (Vec A)-receiving helper
severity: blocking for the Option D Eq Vec rewrite; latent elaborator gap independent of M5
date: 2026-06-14
---

## Summary

Inside a `(definstance Eq [Vec] [(Eq A)] (eq? [x y] ...))` body, calling
a polymorphic helper `(defn h [A] [v : (Vec A) ... ^fat cmp : (fn [A A]
bool)] ...)` with a lambda argument fails to elaborate: the cmp formal's
recorded type is `(fn [] : ?)` (zero arity, unknown result) at the call
site, never the declared `(fn [A A] bool)`.

The same helper called from a non-instance polymorphic defn elaborates
and compiles cleanly, so the gap is specific to the typeclass-instance
body's elaboration context, not the helper signature.

## Repro

```turmeric
;; in stdlib/vec.tur
(defn vec-eq-loop-byval [A]
  [x : (Vec A) y : (Vec A) i : int len : int
   ^fat cmp : (fn [A A] bool)]
  : bool
  (if (= i len)
    true
    (if (cmp (:: (vec-get-byval x i) A) (:: (vec-get-byval y i) A))
      (vec-eq-loop-byval x y (+ i 1) len cmp)
      false)))

(definstance Eq [Vec]
  [(Eq A)]
  (eq? [x y]
    (let [xv (:: x (Vec A))
          yv (:: y (Vec A))
          lx (.len xv)
          ly (.len yv)]
      (if (= lx ly)
        (vec-eq-loop-byval xv yv 0 lx (fn [a b] (eq? a b)))
        false))))
```

## Observed

```
error [TUR-E0001]: function 'vec-eq-loop-byval' arg 5: expected (fn [] : ?),
got (fn [int int] : bool)
        (vec-eq-loop-byval xv yv 0 lx (fn [a b] (eq? a b)))
                                      ^^^^^^^^^^^^^^^^^^^^
```

The expected type `(fn [] : ?)` says the elaborator lost both vec-eq-loop-
byval's cmp param's arity (`[A A]` → `[]`) and result (`bool` → `?`)
during the call's arg-type check.

## Control

The same helper called from a non-instance, non-typeclass polymorphic
defn elaborates correctly:

```turmeric
(defn use-byval [A] [v1 : (Vec A) v2 : (Vec A) ^fat cmp : (fn [A A] bool)] : bool
  (vec-eq-loop-byval v1 v2 0 (.len v1) cmp))
;; compiles and runs cleanly.
```

So the helper's signature is correctly stored on the binding; the gap
is in how the typeclass-instance body re-reads / unifies that signature
against the call's lambda arg.

## Alternative repro -- constrained-poly helper segfaults

Trying the alternative shape -- make the helper itself constrained-poly
on `(Eq A)` and call `eq?` directly instead of taking a cmp arg --
SEGVs the elaborator:

```turmeric
(defn vec-eq-loop-byval [A]
  [(Eq A)]
  [x : (Vec A) y : (Vec A) i : int len : int]
  : bool
  (if (= i len)
    true
    (if (eq? (:: (vec-get-byval x i) A) (:: (vec-get-byval y i) A))
      (vec-eq-loop-byval x y (+ i 1) len)
      false)))
```

```
SUMMARY: AddressSanitizer: SEGV elab_typeclasses.c:3388 in elab_method_call
```

Two distinct elaborator gaps, both blocking the Option D Eq Vec
rewrite from `docs/upcoming/m5-residual-straddle-retirement.md`.

## Severity

Hard compile error / hard crash; does not affect any current stdlib
shape (Eq Vec retains the carrier-bridging body it had before).
Blocks the M4c-pre-ext straddle retirement.

## Root cause hypothesis

For the first form (`^fat cmp : (fn [A A] bool)`):
- The elaborator's instance-method scope binds `a` (Eq's class var) to
  Vec and registers `A` (the constraint var) as a fresh tyvar.
- vec-eq-loop-byval's own `[A]` tyvar SHADOWS the instance's A in some
  lookup, so when checking the cmp arg, the elaborator looks up the
  callee's cmp.arg_full_types and finds them empty (the previous A
  binding got blown away).

For the second form (constrained-poly helper calling `eq?`):
- elab_method_call at line 3388 dispatches the inner `(eq? ...)` and
  walks a null pointer -- likely a missing dict context when the
  enclosing fn is a constrained-poly defn rather than an instance.

Both need elab-side investigation.

## Workaround

None.  Keep the existing Eq Vec body (`(:: x :int)` ascription +
carrier vec-eq-loop).  The M5 residual-straddle retirement plan stays
blocked on this gap.

## Validation under fix

The fixture `tests/fixtures/m5-byval-marker-spec-emit/` already covers
the working case (helper invoked from a non-instance polymorphic defn);
a fix to either form should make the analogous instance-body shape
build and pass.

## Related

- `docs/upcoming/m5-residual-straddle-retirement.md` -- Option D plan
  that this gap blocks.
- `docs/upcoming/end-to-end-monomorphization-plan.md` -- M5 phase.
- `docs/archive/history/m5-constrained-poly-spec-wrong-dispatch-for-parametric-receiver.md`
  -- the elab fix for the dispatch side; this is a different elab gap
  on the typed-fn-arg side.
