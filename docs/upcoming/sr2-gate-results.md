---
title: SR2 prototype gate -- results
category: Planning
description: What happened when a multi-variant PARAMETRIC sum monomorph was forced by value behind a compile-time seam -- the plan's prerequisite line corrected, a ten-fixture worklist that is one family (the M7 HKT carrier machinery), one silent SEGV that turned out to be a default-path ABI bug and is now fixed, one elaboration gap on nullary constructors, and a pre-existing default-path ICE found while probing.
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

## Remaining damage: 10 fixtures, one family

Against the 2709-green baseline (SR1's gate had 34):

| fixture | first error |
|---|---|
| hkt-cata-* (6) | `(ExprF (fn [int] int))` etc. assigned into an int64 slot -- the cata/fmap machinery hard-assumes `(F B)` monomorphs ride the carrier |
| hkt-fmap-byvalue-sum-element | aggregate used where an integer was expected |
| class-method-hkt-tyvar-grounding | same |
| poly-combinator-application-element-inference | invalid initializer |
| conv-with-narrowed-variant-parametric | pbp/by-value param mismatch at `getv` |
| ~~parsec-tutorial~~ | **FIXED 2026-08-27** -- see below |

Every one is the M7 HKT-carrier bridge family: `fmap`/`cata` specs, functor
element slots, and closure returns that assume a `(F A)` monomorph is an
int64 carrier word.  The B4 exclusion moved the fixpoint functors out of the
way, but any OTHER parametric sum used through `Functor`/`Monad` -- which is
precisely what Option and Result are, everywhere -- hits the same
assumptions.  **This is the actual cost of SR2a**, and it is the same
machinery the plan's provenance section already priced as "four predicates
had to move in lockstep."

**The one to respect: parsec-tutorial -- CLOSED 2026-08-27, and it was not
the seam's bug.** `PRes` -- `(PFail)` / `(POK a int)`, the Option shape
verbatim -- driven through parser-combinator closures, compiled cleanly under
the seam and segfaulted at runtime.  The guess above (a sum monomorph crossing
a `:fn` carrier as an aggregate where the reader assumes a box pointer) named
the right family and the wrong member.  The actual cause was ABI SELECTION:
`thunk_type_has_concrete_c_abi` had no `TY_APP` case, so a concrete parametric
monomorph reported "no C ABI", the typed fatshim was declined, and slot 0 kept
the generic `int64_t (*)(void *, int64_t...)` shim that the dispatch site then
cast to the aggregate signature.

**That is a DEFAULT-PATH bug**, reproducible at the pre-session merge base with
a bare 24-byte parametric product and no seam anywhere:

```turmeric
(defdata Big3 :copy [a] (Big3 [x : a y : a z : a]))
(defn mk3 [n : int] : (Big3 int) (Big3 n (* n 2) (* n 3)))
(defn apply3 [^fat f : (fn [int] (Big3 int)) n : int] : int
  (match (f n) (Big3 x y z) (+ x (+ y z))))
(defn main [] : int (println (apply3 mk3 5)) 0)   ; expect 30 -> SEGV
```

It hid because the generic shim is a transparent forwarding tail-call, so
aggregates returned in registers survived it by luck; only past the SysV
16-byte threshold does sret shift the arguments and the shim read the caller's
sret destination as its env.  Fixed, with all three widths pinned by value in
`tests/fixtures/fat-dispatch-parametric-monomorph-return`:
[fat-dispatch-parametric-monomorph-generic-shim](../archive/fat-dispatch-parametric-monomorph-generic-shim.md).

The lesson the entry above already stated stands, and paid: **assert VALUES in
these fixtures, not just that they build** -- a function-pointer cast is
precisely what cc cannot see through, and the two narrow widths pass a
build-only check while the wide one jumps to 0.

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

1. **SR2a codegen**: the 10-fixture M7 worklist above (was 11; the parsec SEGV
   is closed and was a default-path ABI-selection bug, not seam damage).
   Smaller than SR1's 34 and clustered the same way, but in the HKT machinery,
   where the archived nested-monomorph paper trail says changes move four
   predicates in lockstep and fail quietly.  What remains is one shape: the
   cata/fmap machinery assumes an `(F A)` monomorph IS an int64 carrier word,
   which is a representation assumption rather than an ABI-selection hole, so
   the parsec fix does not generalise to it.
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
