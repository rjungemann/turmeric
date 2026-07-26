---
title: Refinements Over Mutable State
category: Planning
description: A measure must be provably pure to be congruent, which makes every predicate about a mutable world unprovable. Two candidate designs that recover it without reopening the congruence hole.
---

# Refinements Over Mutable State (`RM-S`)

**Status:** RM-S0 (dogfooding) **done 2026-07-26**; the A-vs-B decision is
**awaiting sign-off** (RM-S1/RM-S2 do not start without it). This plan states
the problem and two candidate answers; RM-S0 wrote both surfaces by hand in
`tur-ecs`, ran the epoch one, and produced a recommendation -- see the status
block below.
**Depends on:** [refinement-types-plan.md](refinement-types-plan.md).
**Feeds:** [ecs-refinement-typed-apis-plan.md](ecs-refinement-typed-apis-plan.md)
gap C2 -- hard-blocking for that plan's RE1.

> **Status 2026-07-26 -- RM-S0 done, recommending Candidate B; decision open.**
> Both surfaces were written and read in `tur-ecs` (full artifact:
> `turmeric-spices/docs/ecs-rms0-stateful-refinement-dogfood.md`). RM-S0 was run
> *after* C1/RM-B1 landed, so the epoch surface is now testable rather than
> hypothetical. Empirical results (`TUR_REFINE_STATS=1 --enable=refined`):
>
> | case | result |
> |---|---|
> | A.1 epoch as a **pure struct field**, clean guard->read | **1 proven** |
> | A.2 direct `set!` between guard and read | 1 unknown (sound invalidation) |
> | A.3 epoch behind the **real `tur-ecs` inline-C handle** | 1 unknown (the wall) |
>
> **Reading:** the epoch surface does **not** read acceptably for `tur-ecs`, and
> not because of call-site verbosity. It fails on two structural points a lint
> cannot fix: (a) it does not discharge against the *shipped* world (A.3) -- the
> state is shared heap behind a handle, so `world-epoch` is inline C and impure;
> making it discharge means restructuring `sized-world.tur` into by-value
> struct-field state and giving up the shared-world write-propagation the
> scheduler relies on; and (b) its soundness rests on an **unenforced** bump
> discipline (bad point 1), whose failure is an elided use-after-despawn check.
>
> Per this plan's own decision rule, that points to **Candidate B** (scoped
> congruence windows via the linear caps `ecs/cap` already ships). B's failure
> mode is "region smaller than it could be" (loses proofs, keeps checks --
> sound); A's is "check elided that shouldn't be" (unsound). The counterweight
> is size: B is a real language addition (declared mutation rows + region form +
> a third hypothesis-invalidation site), staged as B1 (checked mutation rows,
> no VC interaction) -> B2 (region form + cap borrow) -> B3 (the congruence-window
> link, behind `--enable=refined`, fuzzed with the `stateful` sabotage). A-with-
> a-lint is small but buys no working ECS aliveness proof here, so it is not a
> cheaper win -- it is not a win.
>
> **Decision (2026-07-26): build Candidate B, staged.** RM-S1 (epoch + lint)
> is not pursued.
>
> **B1 done 2026-07-26 -- and it needs no new compiler row.** RM-S0's follow-up
> probing established that the "a mutator is uncallable inside a frozen region"
> property is *already* expressible with the shipped substructural machinery --
> a `:linear` capability, the same mechanism `defsystem` uses. So B1 ships
> spice-side as `ecs/freeze` (`turmeric-spices/spices/ecs/src/ecs/freeze.tur`):
> a `:linear` `DespawnCap<W>` gates a despawn, and `with-frozen` borrows the cap
> for a region so any cap-gated despawn inside the body fails to elaborate
> (`TUR-E0101`, a compile error). Verified: `tests/freeze-region.tur` (positive)
> and `tests/errors/freeze-despawn-in-region.tur` (negative). This is a
> *refinement of this plan's RM-S2 item 1*: the mutation gate is "declared and
> checked" as the plan demands, but realized as a linear cap rather than a new
> effect row -- lower risk, reuses proven machinery, and gives B3 a concrete
> type-level fact (cap frozen in this scope) to key congruence off. Two caveats,
> both carried into B2: (a) non-forgeability is a usage discipline at B1 (mint
> the cap once at world construction; do not re-mint mid-frame) -- B2's region
> *form* makes it structural; (b) the ergonomic region HOF hit a codegen bug
> (`docs/reported/poly-result-hof-capturing-closure-sigbus.md`: a capturing
> closure through a HOF with a *type-variable result* SIGBUSes), worked around
> by fixing `with-frozen`'s body result to `int`. B2 should make the region a
> first-class form (not a polymorphic HOF), or that bug must be fixed first.
>
> **Next:** B2 (region form + capability borrow as a language construct) then
> B3 (the VC congruence-window link, behind `--enable=refined`, with the
> `stateful` fuzzer sabotage). RE1's `errors/refine-stateful-mutation-invalidates`
> stays the fixture to write before B3 touches the VC.

## The problem

Refinement predicates are useful about immutable things and useless about
mutable ones. Not partly useless -- entirely, and by construction.

The rule is in the guide: *"A measure must be provably pure."* Two occurrences
of `(size-of v)` denote one value only if `size-of` is a mathematical function
of `v`. The purity walk is default-deny, and it correctly refuses inline C, a
read of a `^mut` binding, and a field read behind `rc<T>` / `ref<T>`. So:

```turmeric
(defn sized-alive? [s : WorldState e : Entity] : bool
  ;; reads gens[idx] out of a malloc'd control block through inline C
  ...)

(if (sized-alive? s e)
  (get-Pos! cap w e)     ;; the crossing does NOT discharge
  ...)
```

Verified: `0 proven, 1 unknown`. The guard is recovered as a path condition,
the argument is the same expression, and the two occurrences of
`sized-alive?` still get distinct symbols -- so the hypothesis and the goal
are about unrelated opaque values.

**And that is the correct answer today.** `(- (tick) (tick))` is `-1`, not `0`;
the same reasoning applies to a world that is despawned between the guard and
the read. Three separate soundness bugs in this feature came from assuming a
call was congruent when it was not, and the fix each time was to assume less.
Loosening the rule for the ECS's convenience would be the fourth.

### What is NOT wrong

Probes worth recording so nobody re-runs them looking for a hole:

- A `set!` in the caller's body **abandons** the crossing (verified: Unknown).
- A `^mut` parameter is **by value** in Turmeric -- a callee's `set!` is not
  observable by the caller (verified by print). So an intervening
  `(clobber! w)` cannot stale a caller's hypothesis about a struct it passed,
  and the fact that such a crossing reports *proven* is correct rather than a
  bug.
- A `^borrow` parameter's field read **is** congruent -- the decline keys off
  the receiver's type kind (`TY_REF` / `TY_RC` / `TY_PTR_VOID`), and `^borrow`
  is an annotation, not a reference type. This is sound for the same reason
  by-value is: the callee holds no second handle to mutate through.

The mutation channels are closed. The task is to open a *narrow, checked* one,
not to widen the purity walk.

## What "congruent" has to mean instead

The unstated assumption in "a measure must be pure" is that the only thing a
predicate can be a function of is its arguments. For a stateful predicate that
is false: `alive?` is a function of its arguments **and of the world's despawn
history**, and the history is not a value anywhere in the program.

So there are exactly two honest moves: make the history a value, or bound the
window over which it cannot change.

---

## Candidate A -- Epoch arguments (make the state a value)

Give the mutable thing a monotonically increasing **epoch**, bumped by every
mutation, and make the measure a function of it:

```turmeric
(defn world-epoch [^borrow w : GameWorld] #fx{} : int (.epoch w))

(defn get-Pos! [^borrow cap : (ReadCap Pos)
                ^borrow w   : GameWorld
                e : #refine{ x : Entity | (alive-at? (world-epoch w) x) }]
             : Pos ...)
```

`alive-at?` is now a function of two values, and two occurrences are congruent
exactly when the epoch has not moved. A despawn bumps `.epoch`, so the guard's
fact and the read's goal mention different epochs and the proof correctly
fails.

**What is good about it:** it needs *no new compiler feature at all*. The
epoch is an ordinary immutable field read, which probe 5 of the ECS plan shows
is already congruent through a `^borrow`. It is entirely a library discipline.

**What is bad about it:**

1. **Nothing enforces the bump.** A mutator that forgets to increment the epoch
   makes the compiler prove a false thing. The whole soundness argument moves
   into a library's hands, silently, which is the shape of every escape hatch
   this feature has refused so far. It is materially worse than `#fx{}`-lying,
   because there is not even a declaration to point at.
2. **The epoch has to live somewhere the compiler can read purely.** In
   `tur-ecs` the world's mutable state is behind an `:int` handle to a malloc'd
   block, so `world-epoch` would be inline C and therefore impure -- the exact
   wall this is trying to climb. It works only after the state moves into
   struct fields (a large change to `sized-world.tur`).
3. **It is coarse.** Any mutation invalidates every fact, so an ECS that
   spawns during iteration loses all its proofs, including ones about entities
   the spawn cannot have touched.

Point 1 is the one that decides it. This is the "trusted purity annotation"
idea wearing a different hat, and the parent plan's history says trusted
purity is how the miscompiles got in.

---

## Candidate B -- Scoped congruence windows (bound the mutation)

Do not make the state a value; make the *absence of mutation* checkable, and
let a measure be congruent inside a region where mutation is impossible.

Turmeric already has the mechanism: **substructural types**. `ecs/cap.tur`
ships linear `WriteCap<T>` / `ReadCap<T>`, and `defsystem` already uses
linearity to make "writing a component you did not declare" a compile error.
The same shape gives a despawn capability:

```turmeric
;; despawn! consumes the linear cap, so no despawn can happen inside a
;; region that borrowed it away.
(with-frozen-entities w [tok]
  (if (alive? w e)
    (get-Pos! cap w e)     ;; congruent: the region owns the despawn cap
    ...))
```

Inside `with-frozen-entities`, the despawn capability is borrowed by the
region, so `world-despawn!` is not callable, so `alive?` genuinely *is* a
function of its arguments there.

**What is good about it:** the enforcement is the type system's, not a
library's. There is no bump to forget; if you can call the mutator, you are
not in the region.

**What is bad about it:** the compiler does not currently connect "this
capability is unavailable in this scope" to "this measure is congruent in this
scope". That connection is the actual work, and it is not small:

- The purity walk answers a question about a *function's body*, statelessly.
  This asks a question about a *program point*. `rt_binding_is_pure` has no
  notion of where the call is.
- The link between a cap type and the set of functions it gates is a library
  fact today (`get-Pos` takes a `ReadCap Pos` because the macro emits it), not
  something the elaborator knows. Teaching the encoder "these calls mutate
  this thing" needs a declared relation -- something like a `#reads`/`#writes`
  row on the function, which is a real language addition.
- Region entry/exit is a third place refinement hypotheses would have to be
  invalidated, alongside `set!` and the `do`-split rule.

**What is honest about it:** the bad list is all *work*, not *risk*. Every
failure mode is "the region is smaller than it could be", which loses proofs
and keeps checks. Candidate A's failure mode is "a check was elided that should
not have been".

---

## The recommendation, and the phase that tests it

Candidate B is the right shape and Candidate A is the right size, and the
decision should not be made on either of those. It should be made on **whether
anyone writes the annotations**.

So RM-S0 is not an implementation phase.

### RM-S0 -- Write both surfaces by hand, in `tur-ecs`, and read them  [DONE 2026-07-26 -- recommends B; see status block + turmeric-spices/docs/ecs-rms0-stateful-refinement-dogfood.md]

Take the accessor family from
[the ECS plan's RE1](ecs-refinement-typed-apis-plan.md#re1----strict-aliveness-needs-c2-wants-c1)
and write its call sites twice -- once with epoch arguments threaded, once
inside a hypothetical frozen region. No compiler changes; the epoch version
partly runs today, the region version is a sketch that does not compile.

The question RM-S0 answers is the dogfooding plan's "distant third": *is the
annotation burden tolerable?* For this feature it is not third at all, it is
first, because both candidates cost the user something at every call site and
only one of them buys enforcement for it.

If the epoch version reads acceptably, Candidate B's cost is hard to justify
and the answer is A-with-a-lint. If it does not, B is the only one worth
building.

### RM-S1 -- (A) Epoch discipline + a lint that makes forgetting loud  [NOT PURSUED -- RM-S0 chose B]

Only if RM-S0 says so. The minimum viable version is not the epoch itself --
that is library code today -- but a diagnostic: a function that mutates a
struct carrying a field the codebase treats as an epoch, and does not bump it,
should warn. Which requires the epoch to be *declared* as one, which is a
small language addition (`^epoch` on a struct field) and turns "silently
trusted" into "declared and checked".

Without that, do not do A. A trusted purity claim with no declaration to point
at is the thing this feature has spent its whole history refusing.

### RM-S2 -- (B) Declared mutation rows + scope-bounded congruence

Only if RM-S0 says so. Three pieces, in order, each independently useful:

1. **A declared relation between a function and the state it mutates.**
   **[B1 DONE 2026-07-26 -- realized as a linear capability, not an effect
   row.]** Likely spelled as an extension of the effect row -- **but B1 found
   the shipped `:linear` capability machinery already delivers a "declared and
   checked" mutation gate** (`ecs/freeze`'s `DespawnCap<W>` + `with-frozen`;
   despawn inside a frozen region is `TUR-E0101` at compile time). That
   satisfies this item without a new row, and keeps the parent plan's rule --
   the gate is *declared* (a `^linear` cap in the signature) and *checked* (the
   linearity checker), never inferred from `set!`/inline C. A new effect row
   remains an option if B2/B3 need mutation facts the cap cannot carry, but is
   not the starting point.
2. **A region form** whose entry borrows the capability and whose body is a
   congruence window.
3. **Hypothesis invalidation at region boundaries**, joining `set!` and the
   `do`-split rule as the third invalidation site. These three should be one
   shared predicate with one comment naming the failure mode -- `rt_peel_
   contract` had to be reached independently three times before it was
   centralised, and this is the same pattern arriving early enough to avoid
   that.

## Acceptance (whichever candidate)

The fixtures are the same either way, and the **negative** ones are the point:

- `refine-stateful-guard-discharges`: guard, then read, no mutation between.
  Proves.
- `errors/refine-stateful-mutation-invalidates`: guard, **mutate**, then read.
  Must NOT prove. Write this one first. It is the fixture that catches the
  design being implemented as an escape hatch, and neither candidate is
  acceptable without it green.
- `errors/refine-stateful-aliased-mutation`: the mutation happens through a
  *second* handle to the same object. This is the aliasing question the
  `rc<T>` / `ref<T>` decline already answers for field reads, and any new
  congruence route has to answer it again rather than inherit the answer.
- Source-level differential fuzzing, with a `stateful` generator shape and a
  sabotage: make the invalidation a no-op and confirm the fuzzer reports
  soundness bugs where the shipped build reports zero. Per the parent plan's
  standing lesson -- both historical soundness bugs lived in the encoder, below
  the VC fuzzer's reach -- this is the only harness that covers this work.

## What this plan will not do

- **No trusted-purity attribute.** No `#[pure]`, no `:measure`, no way for a
  user to assert congruence the compiler cannot check. The cost of a wrong
  purity claim is an elided check; the cost of declining is a kept one. The
  asymmetry is the whole design.
- **No heap model.** Nothing here adds a memory theory to the VC. Both
  candidates work by making the state question disappear before encoding --
  A by turning it into an argument, B by proving it cannot change.
- **No inference.** Neither an epoch's bump sites nor a region's extent is
  inferred. Checking, not inference, is the founding decision of the
  refinement design and this plan does not reopen it.

## References

- [refinement-types-plan.md](refinement-types-plan.md) -- particularly
  "The purity gap was a real miscompile", "Purity, take two: the effect row was
  never evidence", and "The third congruence door"
- [refinement-types-guide.md](../../guides/refinement-types-guide.md) -- the
  purity rules as shipped
- [substructural-types-guide.md](../../guides/substructural-types-guide.md) --
  the linear/borrow machinery Candidate B leans on
- [ecs-refinement-typed-apis-plan.md](ecs-refinement-typed-apis-plan.md) -- the
  consumer
- [refined-dogfooding-plan.md](../hold/refined-dogfooding-plan.md) -- RM-S0 is
  a slice of it
