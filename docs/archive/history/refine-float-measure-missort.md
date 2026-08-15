---
title: A float-returning measure was declared Int-sorted, and integer tightening then proved false goals
category: Bug report (resolved)
description: RM-B0. The refinement encoder hard-coded VS_INT for every measure result sort; S2's integer tightening turned that into a false proof and elided a runtime check that should have fired.
---

# Float-returning measure mis-sorted (`RM-B0`) -- RESOLVED

**Severity:** soundness. A refinement check the compiler was responsible for
was elided on the strength of a false proof, and the resulting program returned
a value violating its own declared refinement with no panic and no diagnostic.

**Status:** fixed in the same change that landed
[refine-predicate-measures-plan.md](../refine-predicate-measures-plan.md)
(phases RM-B0..RM-B3). Verified against `build/tur` at 0.30.8.

This report exists because the plan's RM-B0 phase asked for exactly one thing
to be established before the rest could be prioritised -- *does the float case
reject, or does it mis-sort?* -- and the answer changed the plan from a
completeness fix to a soundness fix.

## Repro

```turmeric
(defn norm [v : float] #fx{} : float
  (* v 0.5))

(defn f [v : float] : #refine{ r : float | (<= (norm r) 3.0) }
  :pre (< (norm v) 4.0)
  v)

(defn main [] : int
  (println (f 7.5))
  0)
```

`norm(7.5)` is `3.75`, so the return refinement `(<= (norm r) 3.0)` is false and
the program must panic. Before the fix:

```
$ tur build repro.tur -o repro --enable=refined && ./repro
7.5                                  # no panic -- the check was elided

$ TUR_REFINE_STATS=1 TUR_REFINE_DUMP=1 tur check repro.tur --enable=refined
(declare-fun norm (Real) Int)        # <-- Int result for a float-returning fn
(assert (< (norm v) 4))
(assert (not (<= (norm v) 3)))
refine: 1 obligation(s): 1 proven, 0 refuted, 0 unknown
```

With `--enable=refined` off, the same program panics as it should
(`Return contract violated`), which is what makes this a miscompile rather than
a missed proof.

## Root cause

`src/compiler/refine_collect.c`, `enc_measure`: the result sort was hard-coded.

```c
uint32_t fn = vc_declare_ufunc(E->vc, fname, argc, VS_INT, f, false);
                                                  /* ^^^^^^ always */
```

`rt_sort_of_kind` (`src/compiler/elab_fns.c`) already mapped `TypeKind ->
VCSort` correctly for *variables*, so a `float` parameter was `VS_REAL` while a
`float`-returning **measure** was `VS_INT` -- the two disagreed inside one VC.

That alone would be mere imprecision. What made it unsound is
`src/compiler/refine_solver_arith.c:251`, which tightens a strict bound when
every variable in the expression is integer-sorted:

> `e < 0` becomes `e <= -1` when every variable is integer-sorted

Sound over the integers, false over the reals. With `norm` wrongly declared
`Int`, `norm(v) < 4` tightened to `norm(v) <= 3`, which *is* the goal -- so S2
returned `RT_VALID`, `refine_discharge_one` reported `proven`, and the return
contract check was never emitted. `norm(v) = 3.75` is the counterexample the
check would have caught.

The bool case (`docs/archive/refine-predicate-measures-plan.md`'s original
motivation) shared the same line but was only a completeness hole: an
`Int`-sorted goal was rejected by `refine_vc_build` as "predicate does not
denote a proposition", which is the safe direction.

## Fix

`RefineFnInfo` gained a `ret_sort` field, filled by `rt_resolve_fn` from the
callee's declared return type (`b->type.as.fn.result_kind`, or the class
signature's `return_type` for a typeclass method, or `VS_INT` for a data
constructor). `rt_sort_of_kind` gained its `TY_BOOL -> VS_BOOL` arm.
`enc_measure` declares the symbol at that sort instead of `VS_INT`.

`VS_INT` is the zero value of `VCSort`, so a resolver that never touches the
new field keeps the old behaviour -- the change is additive at every seam.

## Regression cover

- `tests/fixtures/errors/refine-float-measure-not-tightened` -- the repro above,
  under `--strict-refine`. It must report `TUR-W0372` (unknown, check kept). If
  the mis-sort ever returns, the obligation proves and the fixture fails by
  exiting 0.
- `tests/fixtures/refine-float-measure` -- the positive direction: a Real-sorted
  measure discharged by ordinary linear real arithmetic.
- `tests/refine-fuzz-src.py` already generates float-mode programs against the
  gate-on/gate-off soundness property; this class of bug is exactly what that
  property is for, and it is worth noting that it went undetected because no
  generated shape put a float-returning *measure* inside a predicate.
