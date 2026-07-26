---
title: Boolean-Sorted Measures in Refinement Predicates
category: Planning
description: A measure is always declared `VS_INT`, so a `bool`-returning function cannot be used as a predicate atom. Small, verified, and blocking readable domain predicates.
---

# Boolean-Sorted Measures (`RM-B`)

**Status:** RM-B0, RM-B1, RM-B3 **landed** 2026-07-26 on the `refinements`
branch. RM-B2 (position-determined sort for *abstract* measures) and the
fuzzer-vocabulary extension are **deferred** -- see the status block below.
**Depends on:** [refinement-types-plan.md](refinement-types-plan.md) (RT0--RT7,
S0--S4, all landed).
**Feeds:** [ecs-refinement-typed-apis-plan.md](ecs-refinement-typed-apis-plan.md)
gap C1.

> **Status 2026-07-26 -- RM-B1 landed (the C1 fix).** A `bool`-returning
> function is now usable as a predicate atom, and a `float`-returning measure
> is Real-sorted instead of mis-declared Int. Implementation:
> `RefineFnInfo` gained a `ret_sort` field (`refine_collect.h`);
> `rt_sort_of_kind` gained its `TY_BOOL -> VS_BOOL` arm (`elab_fns.c`);
> `rt_resolve_fn` populates `ret_sort` from the resolved callee's return type
> (fn binding / class method / constructor); and `enc_measure`
> (`refine_collect.c`) declares the measure with that sort rather than a
> hard-coded `VS_INT`. RM-B3 prints the drop reason under `TUR_REFINE_STATS=1`
> (`refine_discharge.c`).
>
> **Soundness held (the point of the plan):** the change widens only what can
> be *spelled*, not what is *congruent*. An IMPURE `bool` measure still gets a
> fresh symbol per occurrence -- verified adversarially: a guard/crossing pair
> over an inline-C `flaky?` encodes as `flaky?#0` vs `flaky?#1`, stays
> `0 proven, 1 unknown`, and errors under `--strict-refine`. A PURE `bool`
> measure is congruent and discharges through an `if`-guard.
>
> Fixtures: `tests/fixtures/refine-bool-measure` and `refine-float-measure`
> (both `--enable=refined --strict-refine`, so they compile+run *only because*
> the obligation proves). Full suite unchanged at 11 pre-existing failures
> (`2350 passed`, up 2 for the new fixtures); none of the 11 are related.
>
> **RM-B0 finding (float case):** it MIS-SORTS rather than rejects -- `(< (norm
> v) 3.25)` declared `norm` with an Int result and compared it to a Real
> literal, and reported `1 proven` on a mis-sorted VC. RM-B1's result-sort fix
> corrects it (`norm` now `... Real`). A *separate*, pre-existing coarseness
> remains and is out of C1 scope: `refine_smtlib.c` emits every ufunc ARGUMENT
> sort as a blanket `has_real ? Real : Int`, so a mixed int/real domain is
> approximated. It did not cause a wrong verdict here (the functions are
> uninterpreted); worth a separate note if a domain-sort divergence ever bites.
>
> **RM-B2 deferred, with rationale.** Position-determined sort for *abstract*
> (unresolved) measures is NOT needed for the C1-for-ECS driver -- every ECS
> predicate (`alive?`, `in-bounds?`) resolves to a `defn` in the unit and is
> covered by RM-B1. Deferring it is *strictly sound*: an abstract `bool`
> measure stays Int-sorted, the goal-sort check rejects it, and the obligation
> falls back to the runtime check exactly as today. Crucially, RM-B1 alone
> introduces NO same-name-two-sorts scenario (a resolved name has one fixed
> sort from its return type), so the congruence hazard this plan is careful
> about is *created by* RM-B2's position inference, not by what shipped. Land
> RM-B2 when an abstract bool measure is actually wanted; the design below
> stands. The fuzzer-vocabulary extension is deferred alongside it -- and
> separately, the source fuzzer's differential is currently vacuous on this
> branch (every generated program is `skip_invalid`: it fails to compile even
> gate-off), so adding bool helpers buys no signal until that skew is fixed.

## The bug

A `bool`-returning function cannot be used as a predicate:

```turmeric
(defn alive? [w : int e : int] #fx{} : bool (= w e))
(defn use-it [w : int e : #refine{ x : int | (alive? w x) }] : int e)
```

> `refine: 1 obligation(s): 0 proven, 0 refuted, 1 unknown`

and `TUR_REFINE_DUMP=1` prints nothing -- the obligation never reaches the
solver. Verified against `build-release/tur` at 0.30.8.

## Root cause

Two lines, both in `src/compiler/refine_collect.c`:

- `enc_measure` declares every measure with
  `vc_declare_ufunc(E->vc, fname, argc, VS_INT, ...)`. The result sort is
  hard-coded, for the nullary constant case as well.
- `refine_vc_build` then rejects the goal:
  ```c
  if (goal->sort != VS_BOOL) {
      if (out_reason) *out_reason = "predicate does not denote a proposition";
      return NULL;
  }
  ```

So the encoder builds an `Int`-sorted term where the checker demands a `Bool`,
and the obligation is dropped to Unknown with a reason nothing prints by
default.

Nothing is unsound about this -- the runtime check survives, as it does for
every Unknown. It is a pure expressiveness hole.

## Why it matters more than it looks

Every domain predicate a user actually wants to write is a `bool`-returning
function: `(alive? w e)`, `(in-bounds? v i)`, `(sorted? xs)`, `(open? conn)`.
The supported fragment's `measure` production reads as though these work --
the guide's grammar literally lists `(measure e ...)` as a `pred` form -- and
they silently do not.

The workaround is to return `:int` and compare:

```turmeric
(defn alive-i [w : int e : int] #fx{} : int (if (= w e) 1 0))
(defn use-it [w : int e : #refine{ x : int | (= (alive-i w x) 1) }] : int e)
```

which **does** prove (verified: `1 proven, 0 refuted, 0 unknown`, including
through an `if` guard at the call site). So the machinery is entirely there;
the only thing missing is the sort. Pushing users to the `= ... 1` spelling
also pushes them toward exactly the `:int` stand-ins `CLAUDE.md` forbids.

## Design

**Give a measure the sort its callee's return type maps to.**

`rt_sort_of_kind` (`elab_fns.c:46`) already maps `TypeKind -> VCSort` and today
answers `VS_INT` for everything but the float kinds. It needs a `VS_BOOL` arm
for `TY_BOOL`, and `enc_measure` needs the callee's return type to consult.

The plumbing for "the callee's return type" mostly exists: `RefineFnInfo` is
already filled in by the `RefineFnResolver` hook that `enc_callee_is_pure`
calls, which is how purity and declared result refinements reach the encoder.
Adding a result-sort field to that struct keeps `refine_collect.c` free of
scope and binding knowledge, which was a deliberate property of the original
design and should stay one.

Four cases, and only the third is interesting:

| callee | sort | note |
|---|---|---|
| resolves, returns `:bool` | `VS_BOOL` | the feature |
| resolves, returns `:int` / opaque / struct | `VS_INT` | unchanged |
| resolves, returns `:float` | `VS_REAL` | **new, and a latent bug of its own** -- a float-returning measure is currently declared `Int` |
| does not resolve (abstract measure) | ? | see below |

### The float case is a second defect

`(< (norm v) 1.0)` today declares `norm` as `Int`-sorted and then compares it
to a `Real` literal. Whether that produces a mis-sorted VC or a rejection is
not established; it should be checked before this lands, because if it
mis-sorts rather than rejects, that is a soundness question rather than a
completeness one and it changes the priority of this plan.

### An abstract measure has no return type to read

A name resolving to no function at all is treated as an abstract measure --
that rule is load-bearing (it is what makes `(len v)` usable without defining
`len`) and must not change. There is no declaration to read a sort from.

Proposal: keep `VS_INT` as the default for an unresolved name, and let
**position** decide when it is unambiguous -- a measure appearing where a
proposition is required (directly as the goal, or as an operand of
`and`/`or`/`not`/`=>`) is declared `VS_BOOL`. Every other position keeps
`VS_INT`.

This is a small type inference over one syntactic form, and it is well-founded
because the predicate grammar's boolean positions are all syntactically
determined. The one thing it must not do is declare *the same symbol* at two
sorts within one VC -- the hash-cons table keys off the declaration, and a
symbol that is `Int` in one hypothesis and `Bool` in the goal is two symbols
that print the same, which is exactly how a congruence bug gets written. So:
**resolve the sort of every occurrence of a name before declaring any of
them, and reject the VC (Unknown, with a reason) if a name is used at both
sorts.** Rejecting is correct here -- inconsistent use is a user error the
runtime check still covers, and guessing is how this becomes unsound.

### The predicate must still be pure

Unchanged. A `bool`-returning measure gets the same purity treatment as an
`int`-returning one: congruent only when `rt_binding_is_pure` says so, a
distinct symbol per occurrence otherwise. **This plan does not widen what may
be congruent**, only what may be *spelled*. That separation is the reason this
one is small and
[`refine-stateful-measures-plan.md`](refine-stateful-measures-plan.md) is not.

## Phases

### RM-B0 -- Establish the float case  [DONE: mis-sorts; fixed by RM-B1]

Write the two-line probe (`(< (norm v) 1.0)` with `norm : ... : float`) and
record whether it rejects or mis-sorts. If it mis-sorts, file it and fix it
here; if it rejects, it is the same completeness hole as the bool case and
rides along.

### RM-B1 -- Sort from the resolved callee  [DONE]

`RefineFnInfo` gains a result sort; `rt_sort_of_kind` gains its `TY_BOOL` arm;
`enc_measure` consults both. Covers `alive?`, `norm`, and every other measure
whose definition is in the unit.

### RM-B2 -- Position-determined sort for abstract measures  [DEFERRED -- see status block]

The two-pass declaration described above, with the same-name-two-sorts
rejection.

### RM-B3 -- Surface the drop reason  [DONE]

`refine_vc_build`'s `out_reason` strings are computed and, for this path,
reach nobody -- an obligation that fails to *encode* is indistinguishable at
the CLI from one the solver could not decide. Both report as `unknown`.

Print the reason under `TUR_REFINE_STATS=1` (not by default -- an encoding
failure is not a diagnostic, it is a completeness note). This is what would
have made the original bug a five-minute investigation instead of a source
read, and it will pay off again for the next fragment gap.

## Acceptance

- `tests/fixtures/refine-bool-measure`: the `alive?` program above, proving
  through an `if` guard at a call site. `1 proven, 0 unknown`.
- `tests/fixtures/refine-float-measure`: a `float`-returning measure compared
  against `3.25` -- **not** `3.0`, per the repo's float rule, since an
  integer-valued float literal cannot show a sort/truncation divergence.
- `tests/fixtures/errors/refine-measure-sort-conflict`: one name used at both
  `Bool` and `Int` positions; Unknown with a stated reason, check retained,
  no crash.
- `tests/refine-fuzz-src.py`: the generator's predicate vocabulary gains
  `bool`-returning helpers. The `congruence` shape is where they belong -- a
  `bool` measure over a lying `#fx{}` helper is the same trap as the `int` one
  and should be caught by the same sabotage.
- Run the full source fuzzer at n>=400 across two seeds. This touches the
  **encoder**, which is where both historical soundness bugs lived and which
  the VC-level fuzzer is structurally blind to.

## Effort

Small. RM-B1 is a struct field and two switch arms. RM-B2 is the only part
with a design decision in it, and it is bounded by one syntactic pass. RM-B0
and RM-B3 are each under an hour.

The reason it is worth writing down rather than just doing: RM-B2's
same-name-two-sorts case is a congruence hazard, and this feature's history
says congruence hazards get written by the person who was sure there wasn't
one.
