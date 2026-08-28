---
title: SR2 prototype gate -- results
category: Planning
description: What happened when a multi-variant PARAMETRIC sum monomorph was forced by value behind a compile-time seam -- the plan's prerequisite line corrected, an eleven-fixture worklist worked to zero, the silent SEGV traced to a default-path ABI bug, the whole suite green under the seam, and the cost measured (3.6x faster, 71x less memory, one leaked allocation per construction eliminated). SR2a is done; what remains is the stdlib conversion.
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

**Update 2026-08-27: SR2a's codegen is PAID.** The eleven-fixture worklist is
all green, and so is everything else -- `TUR_SR2_APP_SUM_BYVALUE=1 bash
tests/run.sh` reports 2711 passed, 0 failed, with the default path unchanged
throughout. What closed it was not new machinery: almost every blocker was an
existing predicate with a `TY_ADT` case and no `TY_APP` one. The two items this
document flagged as separate costs -- the nullary-ctor elaboration gap and the
`vec-of` ICE -- are also closed. What remains of SR2 is the stdlib conversion
and the experiment row.

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

## The worklist, worked: 11 of 11

Against the 2709-green baseline (SR1's gate had 34).  **Worked 2026-08-27: all
eleven build and print correct values under the seam, and so does everything
else -- `TUR_SR2_APP_SUM_BYVALUE=1 bash tests/run.sh` is 2711 passed, 0
failed.**  SR2a's codegen is done.

The original entry read "every one is the M7 HKT-carrier bridge family."  Right
about the shape, wrong about the cost.  Almost all of it closed by teaching
existing conventions that a concrete parametric monomorph is one of their
members -- **each was a predicate with a `TY_ADT` case and no `TY_APP` one**:

- **The typed fatshim** (`thunk_type_has_concrete_c_abi`) -- slot 0 of a fatbox
  kept the generic int64 shim while the call site cast it to the aggregate
  signature.  A DEFAULT-path SEGV past 16 bytes; see below.
- **The match's three parts** -- the scrutinee's `(ReF a)`, the result's bare
  `ReF`, and a binder's erased tyvar -- all read types a by-value HKT
  instance-method spec leaves erased, while the element that grounds them lives
  in the emit ctx or in the scrutinee's own monomorph.
- **The b4box closure-slot convention** (`type_is_wide_byval_adt`) --
  `(ExprF (fn [int] int))`, 24 bytes and pass-by-pointer at the callee, was
  spelled BY VALUE in the fat-dispatch cast.  Now `type_is_b4box_closure_slot`,
  deliberately separate: the old predicate also drives ADT FIELD layout, where a
  monomorph must stay inline, and folding the two questions together broke six
  DEFAULT-path fixtures.
- **The fixpoint exclusion** reused B4's narrow
  `adt_is_byval_recursive_carrier_wrapper`, which requires a sole field because
  it promises something else.  `adt_is_fixpoint_partner_of` asks the right
  question.
- **The type-ARGUMENT test** answered "does this have a layout / is it by-value"
  where the question was "does it NAME a monomorph".

Two were elaboration, not codegen, and are **Cause A** below.  One was a
specialization gate.  Details in the git log; the two findings worth carrying
forward are these:

### The acceptance test was a default-path bug

parsec-tutorial compiled cleanly under the seam and SEGVed.  The guess recorded
here first -- a sum monomorph crossing a `:fn` carrier as an aggregate where the
reader assumes a box pointer -- named the right family and the wrong member.
The cause was ABI SELECTION, and it had nothing to do with the seam:

```turmeric
(defdata Big3 :copy [a] (Big3 [x : a y : a z : a]))
(defn mk3 [n : int] : (Big3 int) (Big3 n (* n 2) (* n 3)))
(defn apply3 [^fat f : (fn [int] (Big3 int)) n : int] : int
  (match (f n) (Big3 x y z) (+ x (+ y z))))
(defn main [] : int (println (apply3 mk3 5)) 0)   ; expect 30 -> SEGV
```

Reproducible at the pre-session merge base.  It hid because the generic shim is
a transparent forwarding tail-call, so aggregates returned in registers survived
it by luck; only past the SysV 16-byte threshold does sret shift the arguments.
Fixed and pinned at all three widths:
[fat-dispatch-parametric-monomorph-generic-shim](../archive/fat-dispatch-parametric-monomorph-generic-shim.md).

The lesson this document already stated stands, and paid: **assert VALUES, not
just that it builds** -- a function-pointer cast is what cc cannot see through,
and the two narrow widths pass a build-only check while the wide one jumps to 0.

### Cause A was one channel in three positions

`class-method-hkt-tyvar-grounding` and `conv-with-narrowed-variant-parametric`
were the SAME defect as the nullary-constructor gap the next section describes,
and that section understated it -- it is not confined to nullary ctors, and it
appears in three places, all now closed:

- **Argument position.** `(getv (Empty))` against `getv [b : (Box int)]`.  The
  parameter slot already carried a bidirectional channel for lambda arguments;
  a bare constructor argument now uses it too.
- **Nested ctor field.** The inner `(None)` of `(Some (None))` fell back to the
  active spec's RESULT family, which is the OUTER one.  The enclosing call is
  the only site that knows the field type, so it now supplies it.
- **Unannotated let.** `(let [e (Empty)] ... (getv e))`.  Annotating the binding
  already worked, so only the channel was missing; a bounded, depth-capped
  look-ahead over the let's own text finds the same answer.

### Cause B dissolved

The last fixture looked like a genuine design decision: a fatbox slot 0 holding
a typed shim that returns the aggregate (right for a typed consumer, and what
closed parsec) versus a generic carrier cast from a polymorphic combinator's
lifted closure.  Inverting the fat RESULT ABI would have been a large, deliberate
change.

It was not needed.  That generic dispatch is not something the combinator is
entitled to keep -- `(or-parser s-fail s-ok)` grounds `A := cstr`, and the
compiler already clones a generic fn's lifted closure per instantiation for
exactly this reason.  Three one-line gates had to admit it: the `inner_app`
trigger (which keys on an UNANNOTATED closure result, and this closure wrote its
type down), the spec dedup (whose C signature is identical across
instantiations, so both interned to one -- `match_bindings` exists for that), and
the inner-clone guard's list of triggers.

**Worth remembering: "two consumers disagree about a slot" can mean the slot is
fine and one consumer should not be generic.**

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

## Found while probing, unrelated to the seam -- CLOSED

`(vec-of (Yep 8) ...)` -- a Vec of a parametric sum monomorph -- **ICEd on the
DEFAULT path** (repr-shadow: let-bind of `(Vec (Opt2 int))` wants heap-ptr, gets
carrier-i64).  Reproduced at the pre-session merge base, so it predated all SR
work, and it is on SR2's path either way: `vec<option<T>>` is an everyday shape.

The seam diagnosed it before a fix was written -- the repro compiles and runs
correctly under `TUR_SR2_APP_SUM_BYVALUE=1`, because the seam makes `(Opt2 int)`
by-value and the disqualifying predicate starts answering yes.  The predicate is
the same type-ARGUMENT test listed above, and the fix is the same one:
`(Vec (Opt2 int))` names a real monomorph whether or not its element is by
value.  Pinned by `tests/fixtures/vec-of-parametric-sum-monomorph`, archived as
[vec-of-parametric-sum-monomorph-ice](../archive/vec-of-parametric-sum-monomorph-ice.md).

## What SR2 now costs, in order

1. ~~**SR2a codegen**~~ -- **DONE.**  Full suite green under the seam, default
   path unchanged, and `tests/run-sr2-seam.sh` (ctest `tur_sr2_seam`) keeps a
   fast subset of it honest.
2. ~~**The bare-ctor expected-type fix**~~ -- **DONE**, in all three positions
   the gap appears: argument, nested ctor field, unannotated let.  `(none)` no
   longer needs an ascription to select its monomorph.
3. ~~**The vec-of-sum ICE**~~ -- **DONE.**
4. **SR2b stdlib conversion**: Option/Result defstruct -> defdata, the 47
   sites SR0(b) verified, and the inline-C result builders
   (`tur_ok_ptr`/`tur_some_ptr`/`tur_box_*` in the preamble) which hand-build
   the CURRENT discriminated-record layout and every user of
   [the inline-C results guide](../guides/inline-c-results-guide.md) depends on.
   **This is now the whole remaining cost, and it is the part the gate never
   measured** -- everything above was about whether the representation WORKS,
   and this is about moving the two most-used types onto it.
5. **SR2c**: ~~the `EXPERIMENTS[]` row~~ -- **LANDED 2026-08-27** as
   `--enable=parametric-sum-byvalue` (beta, expires 0.42.0), wired to the same
   `sr2_app_sum_byvalue` gate the env seam drives, with the TUR-W0061 lifecycle
   notice firing the first time the gate decides anything.  Exercised in the
   ordinary suite by `tests/fixtures/parametric-sum-byvalue-enable` (per-fixture
   `flags`), so every default run compiles a parametric sum by value through
   the user-facing channel.  Still open in SR2c: docs, and the
   `.value`-accessor API migration in `option.tur`/`result.tur` -- both of
   which only make sense alongside SR2b.

## What it costs -- measured

SR1's flip was justified by numbers and SR4's was measured and DECLINED (1.13x
slower for 2.2x less memory), so SR2a gets the same treatment.  Measured
2026-08-27 on the same box as the rest of this document; three runs each,
Release-equivalent fixture builds (emitted programs carry no sanitizer).

| benchmark | seam off | seam on | delta |
|---|---|---|---|
| narrow sum, 3e6 construct+match | 0.097-0.112 s, 86-93 MB peak RSS | 0.029 s, 1.2 MB | **3.6x faster, 71x less memory** |
| WIDE sum (6 payload words), 3e6 | 0.135-0.150 s, 172-188 MB | 0.042 s, 1.2 MB | **3.2x faster, 145x less memory** |
| leak check, 1000 ctor calls | 16,000 bytes in 1,000 allocations | zero | one leaked allocation PER CONSTRUCTION |
| compile time (parsec `emit-c`) | 0.107-0.109 s | 0.111-0.114 s | ~4% slower |

The wide sum was included expecting by-value to LOSE -- 6 payload words means
every pass copies 56 bytes or goes pass-by-pointer, where the carrier passes
one word.  It does not lose, and the leak row says why: **the comparison is
against a carrier path that mallocs per construction and never frees.**  That is
this document's own opening finding, now with a number on it.  Against a
hypothetical carrier that freed correctly the gap would be smaller; against the
carrier as it exists, by-value wins on both axes at every width tried.

Note what this does NOT measure: recursive sums, which the seam excludes by
design and which SR4 measured separately and declined.  The favorable result
here is for the population the seam actually admits.

## The open question: should the seam flip?

The gate's job was to answer "what would SR2a cost", and the answer came out
lower than the plan assumed -- on correctness (2710/0 both ways) and now on
performance.  That makes the flip a live decision rather than a distant one.  It
is still a decision, not a formality:

- **CLAUDE.md's rule applies, and is now satisfied.**  The representation ships
  behind `--enable=parametric-sum-byvalue` (beta, `expires_at` 0.42.0).  The
  soak is not a formality: graduating before SR2b lands would fix the ABI
  against the one migration most likely to want it changed.
- **SR2b is still unwritten**, and it is the part no gate measured: moving
  Option and Result themselves, plus the inline-C builders every user of the
  results guide depends on.  Flipping the representation before that lands
  changes the cost of doing it.

The seam stays in the tree, default off, as the instrument -- flipping it on is
one env var, and there is no longer a failure list behind it.

**Regression cover.**  The fatshim fix is a DEFAULT-path bug and is pinned by
`tests/fixtures/fat-dispatch-parametric-monomorph-return` (all three SysV return
widths, values asserted) plus a row in `tests/run-sr4-seam.sh`.  The seam-only
work is covered by `tests/run-sr2-seam.sh` (ctest `tur_sr2_seam`): a canary
asserting the seam actually bites, then build + run + stdout compare for the
whole worklist.  That harness is a fast subset chosen for signal -- when the
seam moves, `TUR_SR2_APP_SUM_BYVALUE=1 bash tests/run.sh` is the real check, and
it is what caught the last defect (an emitted-C warning the subset does not
look for).
