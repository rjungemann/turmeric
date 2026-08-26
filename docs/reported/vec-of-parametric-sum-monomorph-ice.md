# A Vec of a parametric sum monomorph ICEs at the let binder (repr-shadow disagreement)

**Severity: medium** -- an ICE on an everyday shape (`vec<(Opt2 int)>` -- and
therefore `vec<option<T>>` the moment Option becomes a real sum), on the
DEFAULT path.  The shadow ICE is doing its job: two sites genuinely disagree
about the representation, and without the shadow this would be whatever
miscompile the disagreement implies.

**Status:** OPEN.  Filed 2026-08-27, found while probing the SR2 gate
([sr2-gate-results](../upcoming/sr2-gate-results.md)); reproduced at the
pre-session merge base (`2c869636`), so it predates all SR work.

## Repro

```turmeric
(defdata Opt2 :copy [a] (Nah) (Yep a))
(defn get-or [o : (Opt2 int) d : int] : int
  (match o (Yep v) v (Nah) d))
(defn main [] : int
  (let [v (vec-of (Yep 8) (:: (Nah) (Opt2 int)))]
    (println (get-or (vec-get v 0) -1)))
  0)
```

```
tur: internal error (ICE): a representation decision disagrees with repr_of at binding.
  repr-shadow binding let-bind type=(type-app Vec (type-app Opt2 int))
  want=heap-ptr got=carrier-i64 cty=int64_t own=int64_t
```

## What is known

- The disagreement is at the `(Vec (Opt2 int))` LET BINDER: `repr_of` says the
  Vec monomorph is a typed heap pointer (`tur_adt_Vec__Opt2__int *`), the
  binder emission says int64 carrier.
- A Vec of a parametric PRODUCT monomorph (`(Vec (Option int))`,
  `(Vec (Pair2 int int))`) does not ICE -- the vec-app element fixtures cover
  those.  The sum monomorph is what disqualifies some predicate on the
  binder path (likely one gated on `adt_app_is_byvalue_product` /
  `type_has_concrete_codegen_layout`, both of which answer differently for a
  multi-variant app), while the Vec-side registration takes the typed-pointer
  path regardless.
- `TUR_REPR_NO_SHADOW_ICE=1` downgrades to a warning, per the ICE text, for
  anyone who needs to limp past it.

## Why it matters beyond itself

`vec<option<T>>` is an everyday shape, and SR2 (Option as a real sum) makes
every such Vec hit this binder.  The SR2 gate lists this as item 3 of the
phase's cost, ahead of the stdlib conversion.

## Fix direction

Find the binder-side predicate that classifies the app element and align it
with the Vec registration's answer (the repr-decision plan's standing advice:
consult `repr_of(type, position)` at the disagreeing site rather than
re-deriving).  A fixture pinning `vec-of`/`vec-get` over a two-variant
parametric sum belongs in the same change.
