---
title: Class-method-level HKT tyvar (a second type constructor introduced by
  the method, not the class) doesn't ground through the method's own body
severity: MEDIUM. Blocks Traversable-shaped classes -- those whose methods
  introduce a *second* HKT tyvar beyond the class's `^f`. Foldable /
  Traversable / Distributive cannot be expressed with typed by-value
  signatures until this is resolved.
status: OPEN. Filed 2026-07-02 during the `TUR_M7_HKT` retirement
  investigation (docs/upcoming/tur-m7-hkt-flag-retirement-plan.md).
  Independent of the retirement.
---

# Method-introduced HKT tyvar doesn't ground through the method body

## Symptom

When a typeclass method's signature introduces its own tyvar in
result position -- typically the "target functor" `b` in a Traversable-style
`traverse : t a -> (a -> f b) -> f (t b)` -- the instance body's `match`
elaborator can't unify two occurrences of the method-level tyvar against each
other, even when they come from the same helper.

Minimal repro (compiled against `./build/tur` at HEAD, 2026-07-02):

```turmeric
(defmodule main)

(defdata Opt [a] (None) (Some a))

;; `b` here is introduced by the method, not by the class.
(defclass Traversable2 [^t]
  (trav [ta : (t a) g : (fn [a] (Opt b))] : (Opt (t b))))

(defn helper [inner : (Opt int)] : (Opt (Opt int))
  (match inner
    (None)   (None)
    (Some y) (Some (Some y))))

(definstance Traversable2 [Opt]
  (trav [ta g]
    (match ta
      (None)   (Some (None))
      (Some x) (helper (g x)))))
```

```
error [TUR-E0001]: match: arm types are incompatible --
  expected app (from earlier arm), got app
```

Both arms are structurally `(Opt (Opt int))` -- the first via `(Some (None))`
of type `(Opt (t b))` with `t = Opt` and `b` ungrounded; the second via
`helper` returning a concrete `(Opt (Opt int))`. The diagnostic's "expected
app, got app" is the tell: two `TypeApp` nodes with the same head but
different (ungrounded vs. ground) inner argument.

## Why this is distinct from the `pure`/`empty` issue

The `pure`/`empty` gap
([return-directed-methods-pure-empty-inference.md](return-directed-methods-pure-empty-inference.md))
is about a class-level tyvar in result position with no argument to ground
it. This one is different:

- The tyvar `b` is *method-level*, not class-level -- it does not appear in
  the class parameter list `[^t]`.
- It appears in argument position (inside `g`'s type) as well as result
  position, so the argument scan *could* ground it, but only if the scan
  descends into the argument's `result_full_type`.
- The elaboration failure happens *inside the instance body*, not at the
  call site -- so this is not about propagating a caller's `expected_type`.

## Root cause (direction, not yet pinpointed)

The instance body elaborator types `(Some (None))` with the method's
declared result `(Opt (t b))`, keeping `b` as the method's tyvar. When the
second arm returns `helper (g x)` with type `(Opt (Opt int))`, the arm-join
unification tries to reconcile the two but treats `b` as un-unifiable in
this context (the class-level substitution `t -> Opt` has been applied, but
the method-level substitution for `b` never gets bound because there is no
explicit call to unify the method result against a concrete type at this
point).

Fix directions:

1. During arm-join unification in an instance body, treat unresolved
   method-level tyvars as unification variables, not as rigid types. The
   later arm's concrete `(Opt int)` should ground `b := int`.
2. Alternatively, when the instance is elaborated for a concrete call site
   (which is where by-value HKT dispatch monomorphizes anyway), pre-bind
   method-level tyvars from the call site's argument types before typing
   the body. This aligns with the end-to-end monomorphization plan
   ([../upcoming/end-to-end-monomorphization-plan.md](../upcoming/end-to-end-monomorphization-plan.md)).

## Impact

- Traversable cannot be added to stdlib with a typed by-value signature.
  The existing Traversable fixture (`tests/fixtures/hkt-instances/`) works
  only because its `traverse` method is declared `: int` (carrier-erased),
  which is exactly the shape the M7 migration is trying to eliminate.
- Foldable's `foldMap` and Distributive's `distribute` have the same shape
  and will hit the same wall.
- Downstream: any class whose method wants "a functor to another functor"
  -- monad transformers, natural transformations expressed as methods --
  is blocked on this.

## Related

- [return-directed-methods-pure-empty-inference.md](return-directed-methods-pure-empty-inference.md)
  -- adjacent HKT inference gap; distinct root cause.
- [poly-combinator-application-element-inference.md](poly-combinator-application-element-inference.md)
  -- similar "tyvar hidden inside a nested type" gap, but at a plain-defn
  call site rather than an instance body.
- [../upcoming/tur-m7-hkt-flag-retirement-plan.md](../upcoming/tur-m7-hkt-flag-retirement-plan.md)
  -- the flag retirement is independent of this fix.
- `tests/fixtures/hkt-instances/input.tur` -- the existing Traversable
  fixture using carrier-erased `: int` signatures; a natural regression
  target once this is fixed.
