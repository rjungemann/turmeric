# Plan: Reflected Measures (`^reflect`)

> **Status:** Deferred -- placeholder for later elaboration. Not started, and
> deliberately not scheduled; see "Why not now" for the bar it has to clear.
> **Last Updated:** 2026-08-02
> **Type:** Compiler / Refinement types
> **Depends on:** [refinement-types-plan.md](../../archive/refinement-types-plan.md)
> (RT0--RT7 + S0--S4, all landed), the `refined` graduation (0.33.0), and
> [refine-predicate-measures-plan.md](../../archive/refine-predicate-measures-plan.md)
> (RM-B, landed).

## Goal

Let a recursive measure *mean* something inside a refinement predicate, instead
of being an opaque symbol the solver can only compare to itself.

Today every named measure is an uninterpreted function. That is a deliberate
language rule, stated at `src/compiler/refine_collect.c:422-425`:

> An unrecognised head becomes a named measure -- an uninterpreted function
> symbol reasoned about by congruence closure, never unfolded. This is the
> language rule that keeps S1 tractable.

Congruence alone carries a surprising amount: `(= (len xs) 3)` as a hypothesis
does discharge `(> (len xs) 0)`, because EUF hands the term to LIA. What it
cannot do is connect a measure to the *structure* of its argument:

```turmeric
(defn len [xs : List] : int
  (match xs
    [(Nil)      0]
    [(Cons _ t) (+ 1 (len t))]))

(defn head-of [xs : #refine{ v : List | (> (len v) 0) }] : int ...)

(head-of (Cons 1 (Nil)))
```

The obligation is `(> (len (Cons 1 (Nil))) 0)`. `len` is opaque, the argument is
a constructor term, and nothing relates them -- so this falls to `TUR-W0372`
and keeps its runtime check. Two unfolding steps would settle it:
`len(Cons 1 Nil) = 1 + len(Nil)`, then `len(Nil) = 0`, then `1 > 0`.

`(sorted? xs)`, `(in-bounds? v i)`, `(elems s)` all fail the same way, and they
are the predicates users actually want to write.

---

## What this is NOT -- read before elaborating

Two documents in this tree list termination checking as an explicit non-goal,
and **this plan does not reopen either of them**:

- [refinement-types-plan.md](../../archive/refinement-types-plan.md) line 1907, under
  "Non-goals for this prototype": *"Termination checking or total-correctness
  verification."*
- [loop-invariants-plan.md](loop-invariants-plan.md) lines 156-159: *"A
  refinement says nothing about whether the loop finishes... A non-terminating
  loop with a true invariant is perfectly well-typed. Ranking functions /
  decreasing measures. Same reason."*

Both are about **user program** termination -- proving that a `while` loop or an
arbitrary `defn` halts, i.e. total correctness. That remains out of scope,
permanently, and it is the right call for a systems language: 223 of the
1448 `defn`s in `stdlib/` are self-recursive, and the concentration is
`re.tur` (27), `logic.tur` (11), `httpd.tur` (8), `parsec.tur` (6),
`reactor.tur` (6) -- backtracking, a logic engine, a server, an event loop.
A `reactor` loop that provably terminates is a bug report. Nothing here should
ever make a non-terminating program fail to compile.

This plan is about a much smaller obligation: **a function the user has opted
in to reflecting, and only such a function, must be shown total before its
defining equation is admitted as an axiom.** No annotation, no obligation.
Every other function in the language is untouched.

The distinction matters because the two are judged on different merits. "Should
Turmeric prove programs halt?" has been answered no, twice. "May a measure's
definition enter the logic, and what does that require?" has not been asked
anywhere in the tree.

### Why "reflected", not "termination checking"

Naming this after the checker invites it to be evaluated as the feature that
was already declined. Name it after what it delivers -- a measure that is
*definable* rather than opaque -- and totality reads as what it is: an
implementation obligation on an opt-in annotation, not a new global discipline.
Liquid Haskell's `{-@ reflect @-}` is the direct precedent and the same
trade-off.

---

## Sketch

Everything below is a sketch to be firmed up at elaboration time, not a
decision.

### The admission criterion

`^reflect` is admitted only if the function is **total**, which is two
obligations, not one:

1. **Termination.** Structural recursion: every self- or mutual call passes a
   syntactic subterm of a parameter in some fixed argument position. This is
   the cheap 90%; anything else declines.
2. **Coverage.** The body is defined on every input -- in practice, exhaustive
   `match`. A partial function is not a function, and an equation asserting a
   value where none exists is as unsound as a diverging one.

Obligation 2 is the one most likely to be underestimated. Whether the
elaborator already checks `match` exhaustiveness, and whether that check is
strong enough to lean on, is an open question below and should be settled
*first* -- it may be the larger half of the work.

### The encoding: ground instantiation, not quantified axioms

The supported fragment is quantifier-free
(`docs/guides/refinement-types-guide.md:353-354`: "quantifier-free linear
integer/real arithmetic with equality and uninterpreted functions"), so the
defining equation **cannot** be asserted as `forall x. len(x) = ...`. That
would need a quantifier instantiation engine and is exactly the solver cliff
the parent design exists to stay on the cheap side of.

Instead: for each application term `f(t)` already present in the VC, where `f`
is reflected, assert the **ground instance** of its defining equation with
parameters substituted by `t`, repeating to depth `k`. New terms introduced by
an unfolding are themselves candidates for the next round, bounded by `k`.

This keeps the logic quantifier-free, leaves EUF and LIA untouched, and reuses
the substitution machinery `enc_measure` already has for return-refinement
propagation (`refine_collect.c:526-534`). It is the same discipline as Dafny's
fuel: the axiom is real, the instantiation is bounded, and running out of
budget costs completeness rather than soundness.

Note this is also where the totality obligation earns its keep concretely. A
ground unfolding of `f(x) = 1 + f(x)` at any term `t` asserts
`f(t) = 1 + f(t)`, which LIA reduces to `0 = 1`. An inconsistent hypothesis set
discharges *every* obligation in the file, including false ones. This is not a
completeness hole that degrades to a runtime check -- it is silent, total
unsoundness, and it is the reason the criterion is a gate rather than a nicety.

### Two tiers, always-sound fallback

| Function | Encoding |
| --- | --- |
| `^reflect` + proven total | uninterpreted symbol **plus** ground defining-equation instances to depth `k` |
| everything else (incl. `^reflect` that fails the check) | exactly today: uninterpreted, congruence, runtime check kept |

A `^reflect` that fails its totality check is a diagnostic on the *definition*
(proposed `TUR-E0381`, the next free code after `TUR-W0380`), not a silent
downgrade -- an annotation that quietly does nothing is how a reader ends up
believing a predicate is enforced when it is not, which is the same reasoning
that produced `TUR-W0380`. Budget exhaustion during unfolding stays a warning
(`TUR-W0382`) on the same "fails toward a runtime check" principle as
`TUR-W0372`.

**No program that compiles today changes behavior.** That property is the whole
shippability argument, and it is inherited from the parent plan's runtime
fallback -- the thing that made a partial solver shippable at all
(`docs/guides/advanced-type-system-rationale.md:475`).

### Why opt-in rather than automatic

Auto-reflecting every pure recursive measure would silently change compile cost
on existing code, which the graduation went to some trouble to measure and hold
at 1.004x. Opt-in also matches the existing precedent for a per-function
refinement annotation (`#reads`) and gives the author a place to hang the
totality obligation. `^reflect` fits the caret-attribute convention already used
by `^capability`, `^extends`, `^private`, `^mut`, `^unique`.

### Where the checker would live

The language has never had a termination analysis -- confirmed absent from
`src/` -- but it has three adjacent things to build on:

- `rt_classify_binding` / `rt_classify_expr` (`src/compiler/elab_fns.c:346-470`)
  is already a cycle-aware interprocedural walk over callee bodies with the
  right memoization discipline (an explicit call stack, a `min_open` guard that
  refuses to memoize a result derived from an in-progress frame, and node/depth
  budgets that degrade to `UNKNOWN`). Its recursion case is the exact spot where
  the greatest fixpoint *assumes* productive recursion rather than proving it:

  ```c
  /* Recursion: assume pure for now.  Impurity only ever enters
   * through a concrete leaf, so the greatest fixpoint is the right
   * one -- but record that we leaned on frame `i`. */
  ```

- `effect_check.c:1286-1315` is a real worklist fixpoint over the call graph
  that handles mutual recursion by iteration. Best structural template.
- `rec_name_occurs_unguarded` (`src/passes/kind_check.c:208`) is a ~10-line
  guardedness check at the type level, and precedent for shipping a
  deliberately narrow version of exactly this idea --
  [hkt-deferred-tasks.md](../../archive/hkt-deferred-tasks.md) line 274 proposed
  a general `type_is_guarded_recursive()` and the narrow one is what landed.

There is **no shared call-graph data structure**; each of the three builds its
own ad-hoc traversal. Sizing this work should assume building one, or extending
`rt_classify_binding` with a second output field rather than a fourth lattice
value -- purity and totality are independent axes and collapsing them would be
a mistake.

---

## What it unlocks beyond the headline

Three existing rough edges are downstream of the same missing fact, and are
worth counting toward the value even though none would justify the work alone:

1. **`ENC_MAX_PROPAGATE 4`** (`refine_collect.c:168`) plus the `propagating[]`
   name-stack is a *compiler* termination hack standing in for a *logic*
   termination argument -- an arbitrary depth-4 cutoff that silently drops
   facts past it. A decreasing-argument proof turns that into a well-founded
   budget.
2. **Counterexample search is disabled by any measure at all.**
   `refine_solver.c:300` -- `if (vc->n_ufuncs > 0) return NULL;` -- so every
   `TUR-E0371` involving a measure loses its counterexample. A reflected
   measure can be evaluated at a candidate model. This is an error-message win,
   not only a proving-power one.
3. **RT4's justification becomes true as stated.** The return-refinement
   propagation at `refine_collect.c:499-535` is justified by *"either because it
   was proved statically or because the runtime check would have panicked
   otherwise"* -- a partial-correctness argument, vacuous for a call that never
   returns. Practical exposure is low (the hypothesis is about a term that never
   has a value), but a total callee is what would make the comment's reasoning
   actually hold.

---

## Why not now

1. **No measured demand.** As of 2026-08-02 there are **85** fixtures using
   `#refine{}` and **zero** whose predicate calls a recursive user function.
   `stdlib/refine.tur` is nine scalar `deftype` aliases -- `Nat`, `Pos`,
   `NonZero`, `Neg`, `Byte`, `Percent`, `NonNegFloat`, `PosFloat`, `UnitFloat`
   -- with no container measure at all. That is the same finding
   [loop-invariants-plan.md](loop-invariants-plan.md) recorded for loops, from
   the same cause: the refinement layer has not yet grown a
   structure-indexed type, and until it does, opacity costs nothing anyone has
   felt. The gap here was found by reasoning about the design, not by hitting
   it.

2. **It needs a decision the author has not been asked for.** See "What this is
   NOT". The carve-out is defensible, but it should be made deliberately and in
   writing before any code is cut, not discovered halfway through review.

3. **The cost is concentrated in a checker the language has never had**, and
   the coverage half may be larger than the termination half. Neither has been
   sized against the real elaborator.

## The trigger

Start this when a real program wants it. Concretely, any one of:

- A structure-indexed type lands in `stdlib/refine.tur` -- a bounded index, a
  non-empty container, a sortedness predicate -- and someone tries to prove
  something about it and cannot.
- The ECS refinement work wants a measure over a component set or an entity
  generation and hits the opacity wall.
- A report is filed with a concrete measure the author wanted unfolded, showing
  the `TUR-W0372` it produced.

Until then this file is the record, and "no measured demand" is the answer.

---

## Open questions for elaboration

1. **Does the elaborator check `match` exhaustiveness today, and is it strong
   enough to lean on?** Settle this first. If it does not, the coverage half of
   the totality obligation is a prerequisite project, and the sizing above is
   wrong.

2. **Syntax and placement of `^reflect`.** In the signature position alongside
   `#fx{...}` / `#reads`, or an attribute on the `deftype` that uses the
   measure? The former matches `#reads`; the latter keeps the annotation next to
   the thing that motivated it.

3. **What `k` is, and who sets it.** A fixed default with a per-call override,
   or derived from the measure's own decreasing argument? A default that is too
   small makes the feature look broken; too large makes compile time a
   surprise. Measure before choosing.

4. **Mutual recursion.** Structural recursion across an SCC needs a shared
   decreasing argument or a lexicographic order. The honest first cut is to
   admit self-recursion only and decline mutual, with the fixpoint template
   available if that proves too narrow.

5. **Interaction with `--strict-refine`.** A `^reflect` that fails its totality
   check is `TUR-E0381` regardless. But should a *budget exhaustion* warning be
   promoted to an error under strict mode, the way `TUR-W0372` is? Probably yes,
   for consistency; confirm it does not make the depth choice load-bearing on
   build success.

6. **Float measures.** RM-B0's sort handling mis-sorted the float case and it
   was a soundness bug, not a completeness hole (see
   [refine-float-measure-missort.md](../../archive/history/refine-float-measure-missort.md)).
   Reflection over a float-returning measure lands in the same neighborhood and
   should be assumed guilty until tested.

7. **The experiment row.** Per `CLAUDE.md`, an in-flight compiler feature ships
   behind `--enable=<name>` with a fully populated `EXPERIMENTS[]` row in
   `src/runtime/experiments.c` pointing `plan_path` at this file. Name it
   `reflected-measures`. Set `introduced` / `expires_at` when work starts, not
   now.

## Explicitly not in scope

- **Program termination / total correctness.** See "What this is NOT". A
  non-terminating function that is never `^reflect`ed is legal, unremarkable,
  and frequently correct.
- **Ranking functions or well-founded orders written by the user.** Structural
  recursion or decline. A user-supplied termination measure is a much larger
  surface and is not needed for the motivating cases.
- **Coinduction / productivity.** Infinite structures are not supported by the
  refinement layer and nothing here changes that.
- **Automatic reflection.** Opt-in only; see above.
- **Refinement inference.** Inherited non-goal from the parent plan; permanent,
  not deferred.
- **Fixing the `#reads` trusted grant.** `#reads` is "a promise, not a checked
  fact" (`docs/guides/stateful-refinements-guide.md:142`) and is the current
  design's one "fails toward the wrong answer" hole. Totality does not fix it --
  a checked read-effect system would. Keep the two separate.

---

## References

- [refinement-types-plan.md](../../archive/refinement-types-plan.md) -- the parent
  plan; "Why checking, not inference" is the constraint this one inherits, and
  line 1907 is the non-goal this plan carves out from.
- [refine-predicate-measures-plan.md](../../archive/refine-predicate-measures-plan.md)
  -- RM-B; made bool-returning measures usable as predicate atoms, which is
  what makes reflecting them worth anything.
- [loop-invariants-plan.md](loop-invariants-plan.md) -- the sibling deferred
  plan; shares the non-goal, the "no measured demand" posture, and the
  structure-indexed-type trigger.
- [../../guides/refinement-types-guide.md](../../guides/refinement-types-guide.md)
  -- the supported fragment (line 353), the measure rules (line 372), and the
  purity walk (line 421), all of which this plan works within.
- [../../guides/refinement-solver-internals-guide.md](../../guides/refinement-solver-internals-guide.md)
  -- the staged decision procedure and its caps.
- Liquid Haskell, `{-@ reflect @-}` -- direct prior art for opt-in reflection
  gated on totality. Dafny's `fuel` -- prior art for bounded unfolding as the
  quantifier-free encoding.
