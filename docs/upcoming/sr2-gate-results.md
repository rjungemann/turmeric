---
title: SR2 prototype gate -- results
category: Planning
description: What happened when a multi-variant PARAMETRIC sum monomorph was forced by value behind a compile-time seam -- the plan's prerequisite line corrected, an eleven-fixture worklist that is one family (the M7 HKT carrier machinery), one silent SEGV, one elaboration gap on nullary constructors, and a pre-existing default-path ICE found while probing.
---

# SR2 prototype gate -- results

The gate for [sum-representation-plan.md](sum-representation-plan.md) SR2
(Option and Result as real sums), run 2026-08-27 following the SR1 gate's
method: force the shape by value behind a seam, count the crossings, find the
blockers, before writing any of the phase.

**Verdict: the plan's "Hard prerequisite: SR1" line is wrong as written, the
REAL prerequisite (by-value parametric sum monomorphs, call it SR2a) is
tractable and its direct-use subset already works, and the cost lives in one
place the plan already knew was expensive: the M7 HKT carrier machinery.**

## The prerequisite correction

SR1 as shipped covers **non-parametric** sums only
(`adt_sr1_sum_candidate` requires `n_type_params == 0`). Option and Result
are parametric, and a parametric sum monomorph today -- `(Opt2 int)` for
`(defdata Opt2 :copy [a] (Nah) (Yep a))` -- gets a tagged monomorph typedef
whose constructors **malloc and leak**:

```c
static int64_t ctor_Yep__int(int64_t _0) {
    tur_adt_Opt2__int *__r = malloc(sizeof(tur_adt_Opt2__int));  /* leaked */
    ...
```

So converting Option/Result to `defdata` today would take the two most-used
types in the language from by-value to heap-boxed-and-leaked **even with SR1
done** -- exactly the regression the plan's section 5 warns about, arriving
through a gap in the plan's own prerequisite arithmetic.

## The seam

`TUR_SR2_APP_SUM_BYVALUE=1` (env-only, default off, the SR1/SR4 precedent).
Three lockstep changes, all in `types.c`:

- `adt_app_is_byvalue_product` admits a multi-variant parametric monomorph
  (non-GADT, non-heap, not self-recursive, every VARIANT's substituted fields
  by-value-able) -- the app-side sibling of `adt_sr1_sum_candidate`.  The
  sig-table mirror (`record_adt_app_ctor_sigs`) and every emit-side consumer
  route through this one predicate, so they follow automatically.
- `emit_registered_adt_app_rec`'s by-value ctor branch stores the tag and the
  SR4-perf dead-byte prologue (it assumed "byval implies flat", the same
  assumption SR1 broke non-parametrically).
- `adt_app_byval_pass_by_ptr` sizes a sum as tag + WIDEST variant (it read
  `ctors[0]` only, so the pbp answer depended on declaration order -- the
  same latent SR1's sizing fix closed for non-parametric sums).

**Default off, provably inert: 2709 passed / 0 failed, zero snapshot drift.**

One exclusion was required immediately: an app over a **B4 recursive-carrier
wrapper** -- `(ExprF Expr)`, `(ReF Re)`, the fixpoint functors -- stays on
B4's machinery.  Its element rides the int64 carrier BY DESIGN (the wrapper
is the carrier), and admitting the functor spelled `tur_adt_Expr` fields
inline into a typedef that precedes the wrapper's own ("field has incomplete
type").  This is the same boundary the `g_adt_app_byvalue` comment already
draws for the flat-product path.

## What already works under the seam

The Option/Result shape used DIRECTLY -- construction, match, by-value
params, returns, nesting a sum in another sum's field -- runs correctly with
**zero allocations in the constructors**:

```c
static tur_adt_Res2__int__cstr ctor_Good__int__cstr(int64_t _0) {
    tur_adt_Res2__int__cstr __r;
    __r.tag = 1; ...            /* no malloc anywhere */
```

Probe: `(defdata Res2 :copy [a b] (Bad b) (Good a))` with a `div2` returning
`(Res2 int cstr)`, matched by the caller.  Both variants print correctly.

## Remaining damage: 11 fixtures, one family

Against the 2709-green baseline (SR1's gate had 34):

| fixture | first error |
|---|---|
| hkt-cata-* (6) | `(ExprF (fn [int] int))` etc. assigned into an int64 slot -- the cata/fmap machinery hard-assumes `(F B)` monomorphs ride the carrier |
| hkt-fmap-byvalue-sum-element | aggregate used where an integer was expected |
| class-method-hkt-tyvar-grounding | same |
| poly-combinator-application-element-inference | invalid initializer |
| conv-with-narrowed-variant-parametric | pbp/by-value param mismatch at `getv` |
| parsec-tutorial | **builds, then SEGVs at runtime -- silent** |

Every one is the M7 HKT-carrier bridge family: `fmap`/`cata` specs, functor
element slots, and closure returns that assume a `(F A)` monomorph is an
int64 carrier word.  The B4 exclusion moved the fixpoint functors out of the
way, but any OTHER parametric sum used through `Functor`/`Monad` -- which is
precisely what Option and Result are, everywhere -- hits the same
assumptions.  **This is the actual cost of SR2a**, and it is the same
machinery the plan's provenance section already priced as "four predicates
had to move in lockstep."

**The one to respect: parsec-tutorial.** `PRes` -- `(PFail)` / `(POK a int)`,
the Option shape verbatim -- driven through parser-combinator closures,
compiles cleanly under the seam and segfaults at runtime.  A closure-returned
sum monomorph crosses a `:fn` carrier as an aggregate where the reader
assumes a box pointer, and a function-pointer cast hides the disagreement
from cc -- the identical failure signature to the archived
[fat-dispatch-wide-byvalue-aggregate-argument](../archive/fat-dispatch-wide-byvalue-aggregate-argument.md),
one machinery over.  Whatever closes this family must assert VALUES in its
fixtures, not just that they build.

## The elaboration gap the codegen list does not show

A **nullary constructor with an inferable type argument does not select its
monomorph**: `(get-or (Nah) 7)` against `get-or [o : (Opt2 int) ...]` calls
the un-monomorphised carrier base `ctor_Nah()` -- a hard cc error once the
param is by value, and today (default path) a silent representation split the
carrier just happens to absorb.  `(:: (Nah) (Opt2 int))` selects
`ctor_Nah__int` correctly, so the expected-type channel exists but does not
reach bare ctor calls in argument position.

This is on SR2's critical path in a way no fixture shows: **`(none)` is the
most common nullary constructor in the language.**  If it stays
annotation-required, the migration is not "47 mechanical sites" -- every
`(none)` in argument position grows an ascription.  Scope this
elaboration fix (push the expected app type onto ctor calls, the way the
ascription path already does) before starting the codegen half.

## Found while probing, unrelated to the seam

`(vec-of (Yep 8) ...)` -- a Vec of a parametric sum monomorph -- **ICEs on
the DEFAULT path** (repr-shadow: let-bind of `(Vec (Opt2 int))` wants
heap-ptr, gets carrier-i64).  Reproduced at the pre-session merge base, so it
predates all SR work.  Filed:
[vec-of-parametric-sum-monomorph-ice](../reported/vec-of-parametric-sum-monomorph-ice.md).
It is also on SR2's path: `vec<option<T>>` is an everyday shape.

## What SR2 now costs, in order

1. **SR2a codegen**: the 11-fixture M7 worklist above.  Smaller than SR1's 34
   and clustered the same way, but in the HKT machinery, where the archived
   nested-monomorph paper trail says changes move four predicates in lockstep
   and fail quietly.  The parsec SEGV is the acceptance test.
2. **The nullary-ctor elaboration fix** -- without it `(none)` regresses
   ergonomically everywhere.
3. **The vec-of-sum ICE** -- pre-existing, but `vec<option<T>>` cannot ICE in
   a world where Option IS a sum.
4. **SR2b stdlib conversion**: Option/Result defstruct -> defdata, the 47
   sites SR0(b) verified, and the inline-C result builders
   (`tur_ok_ptr`/`tur_some_ptr`/`tur_box_*` in the preamble) which hand-build
   the CURRENT discriminated-record layout and every user of
   [the inline-C results guide](../guides/inline-c-results-guide.md) depends on.
5. **SR2c**: the `EXPERIMENTS[]` row (user-visible change), docs, and the
   `.value`-accessor API migration in `option.tur`/`result.tur`.

The seam stays in the tree, default off, as the instrument -- flipping it on
is one env var and the failure list above is the worklist.
