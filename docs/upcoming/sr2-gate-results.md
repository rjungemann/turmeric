---
title: SR2 prototype gate -- results
category: Planning
description: What happened when a multi-variant PARAMETRIC sum monomorph was forced by value behind a compile-time seam -- the plan's prerequisite line corrected, an eleven-fixture worklist worked down to three, the silent SEGV traced to a default-path ABI bug and fixed, the two remaining causes named, and a pre-existing default-path ICE found while probing.
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

**Update 2026-08-27: SR2a's codegen is mostly paid.** The worklist below is 8
of 11 green, and what closed was not new machinery -- four existing conventions
each had a `TY_ADT` case and no `TY_APP` one. Two causes remain, named in
**Remaining damage**; one of them (a bare ctor call with no expected type) is
the elaboration item this document already flagged as SR2's real ergonomic
risk, now shown to be broader than nullary constructors.

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

One exclusion was required immediately: an app over a **recursive-carrier
wrapper** -- `(ExprF Expr)`, `(ReF Re)`, the fixpoint functors -- stays on
B4's machinery.  Its element rides the int64 carrier BY DESIGN (the wrapper
is the carrier), and admitting the functor spelled `tur_adt_Expr` fields
inline into a typedef that precedes the wrapper's own ("field has incomplete
type").  This is the same boundary the `g_adt_app_byvalue` comment already
draws for the flat-product path.

The exclusion first reused B4's `adt_is_byval_recursive_carrier_wrapper`, which
was too narrow: it requires a SOLE field because it promises something else
entirely ("an 8-byte wrapper whose by-value form IS its carrier int64", a
promise its own caller relies on).  `Expr = (Roll :int (ExprF Expr))` carries a
tag alongside the carrier field and failed that test.  The question here is
fixpoint PARTNERSHIP -- `adt_is_fixpoint_partner_of`.

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

## Remaining damage: 3 fixtures, two causes (was 11, one family)

Against the 2709-green baseline (SR1's gate had 34).  **Worked 2026-08-27; 8 of
the 11 now build and print correct values under the seam.**

| fixture | state |
|---|---|
| hkt-cata-* (6) | **GREEN** |
| hkt-fmap-byvalue-sum-element | **GREEN** |
| parsec-tutorial | **GREEN** -- and it was a default-path bug, see below |
| class-method-hkt-tyvar-grounding | cause A |
| conv-with-narrowed-variant-parametric | cause A |
| poly-combinator-application-element-inference | cause B |

The original entry read "every one is the M7 HKT-carrier bridge family."  That
was right about the shape and wrong about the count: seven of the eight closed
by teaching four existing conventions that a concrete parametric monomorph is
one of their members, not by new machinery.

- **The typed fatshim** (`thunk_type_has_concrete_c_abi`) had no `TY_APP` case,
  so slot 0 of a fatbox kept the generic int64 shim while the call site cast it
  to the aggregate signature.  Default-path SEGV past 16 bytes; see below.
- **The match's two ends** read types that a by-value HKT instance-method spec
  leaves erased -- the scrutinee's `(ReF a)` and the result's bare `ReF` -- while
  the element that grounds them lives in the emit ctx.  Both now ground from the
  active spec, the result through `emit_hkt_spec_app_result`, the result-type
  twin of the existing `emit_hkt_spec_ctor_suffix`.
- **The b4box closure-slot convention** (`type_is_wide_byval_adt`) answered for
  `TY_ADT` only, so `(ExprF (fn [int] int))` -- 24 bytes, pass-by-pointer at the
  callee -- was spelled BY VALUE in the fat-dispatch cast.  Now
  `type_is_b4box_closure_slot`, deliberately separate: the old predicate also
  drives ADT FIELD layout, where a monomorph must stay inline, and folding the
  two questions together broke six DEFAULT-path fixtures.
- **The fixpoint exclusion** reused B4's narrow `adt_is_byval_recursive_carrier_
  wrapper`, which requires a sole field because it promises something else
  ("an 8-byte wrapper whose by-value form IS its carrier int64").  `Expr = (Roll
  :int (ExprF Expr))` failed that test, so `(ExprF Expr)` was admitted and its
  functor monomorph laid `tur_adt_Expr` out inline in a typedef that precedes
  the wrapper's own.  `adt_is_fixpoint_partner_of` asks the right question.

**The pattern across all four: a predicate that had a `TY_ADT` case and no
`TY_APP` one.**  That is the shape to look for in whatever is left.

### Cause A -- a bare ctor call has no expected type

`class-method-hkt-tyvar-grounding` and `conv-with-narrowed-variant-parametric`
are the SAME defect as the nullary-constructor gap the next section already
describes, and that section understates it: it is not only about nullary ctors.

- `(Empty)` bound in a `let` and later passed to `getv [b : (Box int)]` emits
  the un-monomorphised `ctor_Empty()`.  Nullary, as described.
- `(Some (None))` inside a `trav` instance body emits `ctor_None__Opt__int` for
  the INNER `(None)`: with no expected type of its own it falls back to the
  active spec's RESULT family, which is the OUTER `(Opt (Opt int))`.  The outer
  ctor knows its field is `(Opt int)` and nothing carries that down.

One channel closes both: push the expected type onto a constructor call's own
type and onto each of its argument slots, the way the ascription path
(`(:: (Nah) (Opt2 int))`) already does.  **Scope it once, for both.**

### Cause B -- the fat RESULT slot has two consumers that disagree

`poly-combinator-application-element-inference` builds and SEGVs.  A fatbox for
`s-fail : (fn [int] (PRes cstr))` holds a typed shim in slot 0 returning the
24-byte aggregate -- correct for a typed consumer, and what closed
parsec-tutorial.  But the closure lifted out of the POLYMORPHIC combinator
`or-parser` keeps that combinator's ungrounded `(PRes A)`, so it dispatches slot
0 through `int64_t (*)(void *, int64_t)`.  Past 16 bytes sret shifts every
argument and the program jumps to 0.

Both casts are right about their own side; the SLOT cannot be both.  The
argument-position answer already exists and is uniform -- the b4box convention,
where a wide by-value aggregate crosses as an int64 box and every reader derefs
(the archived
[fat-dispatch-wide-byvalue-aggregate-argument](../archive/fat-dispatch-wide-byvalue-aggregate-argument.md)).
Applying the same rule to RESULTS is the obvious candidate and is NOT a local
change: it inverts what the typed fatshim currently does, so it must move the
typed call sites to deref in the same commit.  Decide it deliberately.

The readback half is already in place for one crossing: a direct thunk call
whose thunk returns the carrier now derefs at the call site, keyed on the
thunk's emitted return spelling so an already-grounded thunk is untouched.

## The elaboration gap the codegen list does not show

This is **Cause A** above, seen from the ergonomic side rather than the fixture
side.  It is not confined to nullary constructors -- `(Some (None))` misses for
the same reason -- but the nullary case is what makes it urgent.

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

1. **SR2a codegen**: mostly PAID.  8 of the 11 worklist fixtures are green and
   the default path stayed at 2710/0 throughout.  What is left is **Cause B**
   alone -- one deliberate decision about whether a fat RESULT slot speaks the
   aggregate or the b4box carrier, which inverts the typed fatshim and must
   move its call sites in the same commit.  One fixture
   (poly-combinator-application-element-inference) is the acceptance test, and
   it SEGVs rather than failing to build, so assert values.
2. **The bare-ctor expected-type fix** (**Cause A**) -- now the largest
   remaining item, and it grew: two of the three surviving fixtures are this,
   not just `(none)` ergonomics.  Without it `(none)` regresses everywhere AND
   a nested ctor inside an HKT instance body picks the wrong family.
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

**Regression cover.**  The fatshim fix is a DEFAULT-path bug and is pinned by
`tests/fixtures/fat-dispatch-parametric-monomorph-return` (all three SysV
return widths, values asserted) plus a row in `tests/run-sr4-seam.sh`.

The seam-only work would otherwise have no cover at all -- nothing in the
ordinary suite compiles a parametric sum by value, so the eight fixtures above
would be a claim this document makes and nothing enforces.  `tests/run-sr2-
seam.sh` (ctest target `tur_sr2_seam`) is that cover: a canary asserting the
seam actually bites, then build + run + stdout compare for all eight.  Add the
remaining three as their causes close.
