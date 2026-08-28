# A Vec of a parametric sum monomorph ICEs at the let binder (repr-shadow disagreement)

**Severity: medium** -- an ICE on an everyday shape (`vec<(Opt2 int)>` -- and
therefore `vec<option<T>>` the moment Option becomes a real sum), on the
DEFAULT path.  The shadow ICE is doing its job: two sites genuinely disagree
about the representation, and without the shadow this would be whatever
miscompile the disagreement implies.

**Status: RESOLVED 2026-08-27.**  Filed the same day while probing the SR2 gate
([sr2-gate-results](../upcoming/sr2-gate-results.md)); reproduced at the
pre-session merge base (`2c869636`), so it predated all SR work.  See
**Resolution** at the bottom.

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

## Resolution (2026-08-27)

The guess in **What is known** was right, and the SR2 seam proved it before any
fix was written: under `TUR_SR2_APP_SUM_BYVALUE=1` the repro compiles and runs
correctly, because the seam makes `(Opt2 int)` a by-value monomorph and the
disqualifying predicate starts answering yes.  That is a diagnosis, not a fix --
the ICE was on the default path, where the sum still rides the carrier.

The predicate is the type-ARGUMENT test shared by `type_app_is_concrete_adt` and
`adt_app_is_byvalue_product` (`adt_app_type_arg_is_concrete`).  It asked two
questions -- "does this argument have a concrete C layout" and "is it a by-value
monomorph" -- and a carrier-riding multi-variant sum answers no to both.  But
`tur_adt_Opt2__int` is a registered typedef either way, and the question at that
site is only whether the OUTER app names a real monomorph.  So the test now also
accepts an argument that is itself a concrete ADT app, whatever its
representation:

```c
return a->kind == TY_APP && type_app_is_concrete_adt(a);
```

A Vec of a parametric PRODUCT monomorph never hit this, because a product IS
by-value and passed the second question -- which is exactly why the shape looked
covered by the existing vec-app element fixtures.

**On the lockstep warning.**  `type_app_is_concrete_adt`'s comment warns that
widening its argument loop alone once cost eight fixtures ("`some__spec__..._
Vec__int` returns the by-value aggregate while its body still calls the carrier
`ctor_Option`").  That warning is why the two loops now share one helper, so
this widening moved both together; the suite is green on the default path and
under the seam.

Pinned by `tests/fixtures/vec-of-parametric-sum-monomorph`, which reads BOTH
element kinds -- a mis-decoded element would let the first through by luck, and
the `Nah` arm is where a wrong representation shows up as a plausible number
rather than a crash.

Suite after: **2711 passed, 0 failed** on the default path; **2710 / 0** under
`TUR_SR2_APP_SUM_BYVALUE=1` (the count differs by this fixture, added after that
run).
