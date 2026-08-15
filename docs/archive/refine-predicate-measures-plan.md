---
title: Boolean-Sorted Measures in Refinement Predicates
category: Planning
description: A measure is always declared `VS_INT`, so a `bool`-returning function cannot be used as a predicate atom. Small, verified, and blocking readable domain predicates.
---

# Boolean-Sorted Measures (`RM-B`)

**Status:** LANDED (RM-B0..RM-B3). RM-B0 came back worse than the plan
assumed -- the float case **mis-sorts**, and the mis-sort is a soundness bug,
not a completeness hole. Written up in
[docs/archive/history/refine-float-measure-missort.md](history/refine-float-measure-missort.md).
**Depends on:** [refinement-types-plan.md](../upcoming/v1/refinement-types-plan.md) (RT0--RT7,
S0--S4, all landed).
**Feeds:** [ecs-refinement-typed-apis-plan.md](../upcoming/v1/ecs-refinement-typed-apis-plan.md)
gap C1.

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

### The float case is a second defect -- and it MIS-SORTS

`(< (norm v) 1.0)` today declares `norm` as `Int`-sorted and then compares it
to a `Real` literal. Whether that produces a mis-sorted VC or a rejection is
not established; it should be checked before this lands, because if it
mis-sorts rather than rejects, that is a soundness question rather than a
completeness one and it changes the priority of this plan.

**RM-B0 answer: it mis-sorts, and it is unsound.** `refine_solver_arith.c`
tightens a strict bound when every variable is integer-sorted (`e < 0` becomes
`e <= -1`) -- valid over the integers, false over the reals. With `norm`
wrongly declared `Int`, `(< (norm v) 4.0) |- (<= (norm v) 3.0)` *proved*, the
return check was elided, and a program whose `norm` was `3.75` returned a value
violating its own refinement with no panic. Full write-up and repro in
[docs/archive/history/refine-float-measure-missort.md](history/refine-float-measure-missort.md);
pinned by `tests/fixtures/errors/refine-float-measure-not-tightened`.

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

### RM-B0 -- Establish the float case -- DONE

It mis-sorts, and the mis-sort is unsound. See above and the archived report.

### RM-B1 -- Sort from the resolved callee -- DONE

`RefineFnInfo` gains a result sort; `rt_sort_of_kind` gains its `TY_BOOL` arm;
`enc_measure` consults both. Covers `alive?`, `norm`, and every other measure
whose definition is in the unit.

`ret_sort` is filled at all three of `rt_resolve_fn`'s exits -- data
constructor (`VS_INT`, an aggregate handle), typeclass method (the class
signature's `return_type`, the one promise true of every instance), and
ordinary binding (`b->type.as.fn.result_kind`, which is the declared return
type already peeled to its base, so a `: #refine{ r : float | q }` return is a
Real and `q` still travels separately in `ret_pred`). `VS_INT` is `VCSort`'s
zero value, so a resolver that never sets the field behaves exactly as before.

### RM-B2 -- Position-determined sort for abstract measures -- DONE

The two-pass declaration described above, with the same-name-two-sorts
rejection.

One correction to the design as written: positions are **three**-valued, not
two. `=` demands neither sort of its operands -- `(= (alive? w x) (alive? w y))`
is as legitimate as `(= (len v) n)` -- so equality operands are NEUTRAL and
`enc_cmp` enforces only that the two *sides* agree. Treating them as value
positions would have made that ordinary predicate a sort conflict. The
value-demanding positions are arithmetic and the strict/non-strict orderings;
the proposition-demanding ones are `and`/`or`/`not`/`=>` and the goal; a
measure's own arguments demand nothing (an abstract measure has no parameter
types to read).

The subject's position comes from `ob->base_sort`, which is what lets the
prescan see a *call-site* conflict: a callee's `p` and a caller's `p` are one
symbol in the encoder's flat namespace, and they need not be the same function.

Only an **unresolved** name can conflict, and the rejection is scoped to that.
A resolved measure has exactly one sort no matter where it appears, so a
resolved measure in the wrong kind of position is a local error, handled
locally by the operand guards below -- which drop just that hypothesis rather
than the whole VC. Nuking the VC there would trade a sound imprecision for a
strictly worse one.

Landing this also required operand-sort **guards** in the encoder itself.
`vc_mk` will happily build `(+ <bool> 1)` with an arithmetic sort, or an `and`
node over integer kids -- it computes the result sort without checking the
operands. Those now fail the encode (`enc_want_value` / `enc_want_prop`), which
is what keeps a propagated return refinement that misuses a name from
manufacturing a mis-sorted hypothesis rather than simply not being asserted.

### RM-B3 -- Surface the drop reason -- DONE

`refine_vc_build`'s `out_reason` strings are computed and, for this path,
reach nobody -- an obligation that fails to *encode* is indistinguishable at
the CLI from one the solver could not decide. Both report as `unknown`.

Print the reason under `TUR_REFINE_STATS=1` (not by default -- an encoding
failure is not a diagnostic, it is a completeness note). This is what would
have made the original bug a five-minute investigation instead of a source
read, and it will pay off again for the next fragment gap.

## Acceptance

- `tests/fixtures/refine-bool-measure`: the `alive?` program above, proving
  through an `if` guard at a call site. `1 proven, 0 unknown`. **Done.**
- `tests/fixtures/refine-float-measure`: a `float`-returning measure compared
  against `3.25` -- **not** `3.0`, per the repo's float rule, since an
  integer-valued float literal cannot show a sort/truncation divergence.
  **Done.**
- `tests/fixtures/errors/refine-float-measure-not-tightened`: **added, and it is
  the one that matters.** RM-B0 turned this plan into a soundness fix, so the
  mis-sort needs its own regression guard -- the `3.0`/`4.0` pair whose
  *integrality* is what armed the tightening. The positive float fixture alone
  would not have caught it: with `3.25` the tightening never fires, so that
  program proved correctly both before and after.
- `tests/fixtures/errors/refine-measure-sort-conflict`: one name used at both
  `Bool` and `Int` positions; Unknown with a stated reason, check retained,
  no crash. **Done.** Worth recording what the fixture taught: the two-sorts
  case is essentially unreachable from *well-typed* source, because the
  language's own type checker rejects `(< (ready? r) 3)` and `(and (level r)
  ...)` first. It is reachable exactly where the type checker cannot see it --
  an abstract measure with no declaration, or two different functions sharing a
  name in the encoder's flat namespace. That is the hazard, and it is why the
  rule rejects rather than resolves.
- `tests/refine-fuzz-src.py`: the generator's predicate vocabulary gains
  `bool`-returning helpers. The `congruence` shape is where they belong -- a
  `bool` measure over a lying `#fx{}` helper is the same trap as the `int` one
  and should be caught by the same sabotage. **Done**, with one finding: a liar
  in *predicate* position is stopped a layer earlier by `TUR-E0375` (both gates
  reject it, so the case lands in `skip_invalid`). That is the correct
  behaviour and the shape is still worth generating -- the property it fuzzes
  is that E0375 and the congruence denial *together* keep it sound, so if
  either stopped firing these cases would reach the encoder.
- Run the full source fuzzer at n>=400 across two seeds. This touches the
  **encoder**, which is where both historical soundness bugs lived and which
  the VC-level fuzzer is structurally blind to. **Done:**

  ```
  n=400 seed=1 mode=both -> 0 SOUNDNESS, 0 other BUG, 9 suspicious (report-only)
  n=400 seed=2 mode=both -> 0 SOUNDNESS, 0 other BUG, 17 suspicious (report-only)
  ```

  `bash tests/run.sh` is green at 2369 passed / 0 failed with the four
  fixtures above added.

## Effort

Small. RM-B1 is a struct field and two switch arms. RM-B2 is the only part
with a design decision in it, and it is bounded by one syntactic pass. RM-B0
and RM-B3 are each under an hour.

The reason it is worth writing down rather than just doing: RM-B2's
same-name-two-sorts case is a congruence hazard, and this feature's history
says congruence hazards get written by the person who was sure there wasn't
one.

**Retrospect.** The estimate held -- but the value of the plan turned out to be
RM-B0, the phase that looked like a formality. "Write the two-line probe and
record whether it rejects or mis-sorts" found a live soundness bug in a feature
whose two previous soundness bugs were both in this same file, and the bug was
sitting one character away from the completeness hole the plan was actually
about (`VS_INT` in `vc_declare_ufunc`). Neither the fixture suite nor the
source fuzzer had reached it, because no shape ever put a float-returning
*measure* inside a predicate. Worth generalising: when a plan says "check X
before this lands, because the answer changes the priority", that check is the
highest-value phase in it, not the cheapest.
