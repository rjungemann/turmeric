# Plan: Loop Invariants for Refinement Types (`:invariant`)

> **Status:** Deferred -- placeholder for later elaboration. Not started, and
> deliberately not scheduled; see "Why not now" for the bar it has to clear.
> **Last Updated:** 2026-07-25
> **Type:** Compiler / Refinement types
> **Depends on:** [refinement-types-plan.md](../../archive/refinement-types-plan.md)
> (RT0--RT7 + S0--S4, all landed) and on the `refined` experiment graduating.

## Goal

Let a `while` loop carry a refinement across its iterations, so that a value
built by a loop can satisfy a refined return type.

Today it cannot. A loop accumulator is `Unknown` no matter what the loop does:

```turmeric
(defn count-up [n : int] : #refine{ r : int | (>= r 0) }
  (let [^mut acc 0
        ^mut i   0]
    (while (< i n)
      (set! acc (+ acc 1))
      (set! i   (+ i 1)))
    acc))
```

The runtime check stays and the program is correct; what is lost is the proof.

---

## The decision that is already made

**Checking, not inference.** This is not an open question and should not be
reopened during elaboration -- it is the founding decision of the refinement
design (see "Why checking, not inference" in the parent plan), and it is what
makes the whole feature shippable without a heavyweight solver.

So this plan is about an invariant the USER WRITES:

```turmeric
(while (< i n) :invariant (and (>= acc 0) (>= i 0))
  (set! acc (+ acc 1))
  (set! i   (+ i 1)))
```

Inferring the invariant is out of scope permanently, not deferred. An earlier
note in the parent plan said loop support "needs invariants, which the
prototype excludes"; that conflated the two, and the correction is the reason
this file exists. A written invariant is checking, is the same shape as the
`:pre` / `:post` annotations that already ship, and is a bounded feature rather
than a multi-week analysis.

---

## Sketch

Everything below is a sketch to be firmed up at elaboration time, not a
decision. The pieces the parent plan already provides -- obligation collection,
the normalized VC, the staged solver, path splitting -- are expected to carry
this with no change; only the obligation SOURCES are new.

A `while` with an invariant `p` generates three obligations, which is the
standard Hoare rule and is why this is small:

| # | Obligation | Meaning |
|---|---|---|
| 1 | `env \|- p` | `p` holds on entry, before the first test |
| 2 | `env, p, c \|- p'` | the body re-establishes `p` (`p'` is `p` after the body's assignments) |
| 3 | -- | after the loop, `p AND (not c)` is available to what follows |

Obligation 2 is the only genuinely new machinery. It needs the body's
assignments modelled as a substitution -- `acc` after the body is
`acc + 1` -- which is the same "assert `x = v`, alpha-rename on shadow"
shape the `let` splitting already implements, applied to `set!` instead of a
binding form. That similarity is the main reason to believe the estimate.

Obligation 3 is what makes the feature worth anything: without it the loop
proves its own invariant and then nothing downstream can use it.

---

## Why not now

Three reasons, in order of weight. All three are measured, not assumed.

1. **No demand.** As of 2026-07-25 there are 48 refinement fixtures and
   `stdlib/refine.tur`, and **zero** of them contain a loop that touches a
   refinement. The stdlib refinement layer is entirely scalar aliases --
   `Nat`, `Pos`, `NonZero`, `Neg`, `Byte`, `Percent`, `NonNegFloat`,
   `PosFloat`, `UnitFloat` -- with no bounded-index type at all, which is the
   canonical loop-shaped motivation for refinement types. The `while` gap was
   found by writing a synthetic probe, not by hitting it in real code.

2. **The experiment has a clock.** `refined` carries `expires_at = 0.34.0`
   against a `VERSION` of `0.30.8`. Adding new surface syntax to an experiment
   that must graduate or be shelved is the wrong direction while the priority
   is graduating what already exists. This plan should not start before that
   resolves.

3. **Something else is worth more and costs less.** Path conditions for
   call-site crossings is a larger measured gap (the canonical
   decreasing-argument recursion cannot discharge its own recursive call) and
   needs no new syntax.

## The trigger

Start this when a real program wants it. Concretely, any one of:

- A bounded-index type lands in `stdlib/refine.tur` and gets used to walk a
  vector -- that is the shape that makes loop-carried refinements ordinary
  rather than hypothetical.
- Two or more distinct callers ask for it, or a spice hits it in real code.
- `refined` graduates and the loop gap is the top item on what is left.

Until then this file is the record, and "no measured demand" is the answer.

---

## Open questions for elaboration

1. **Syntax and placement.** `:invariant` after the condition, as sketched?
   That reads consistently with `:pre` / `:post` but `while` has no existing
   keyword-argument position, so the reader work is real. Alternatives: a
   leading `(invariant p)` form inside the body, or an attribute on the
   enclosing `defn`.

2. **Which loops.** `while` only, or also `loop` / `for` / the tail-recursive
   idiom? `for` desugars, so the invariant would have to survive the
   expansion. Starting with `while` alone is the obvious first cut.

3. **Modelling the body's assignments.** Obligation 2 needs `p` after the
   body. A `set!` to a local is a substitution; a `set!` through a field or a
   reference is not, and the honest first cut is to DECLINE the split when the
   body writes anything the substitution cannot model -- the same
   conservative posture as the `do`-block assignment scan already in
   `rt_prove_paths`.

4. **`break` / early exit.** An early exit leaves the loop without `(not c)`
   holding, so obligation 3's post-condition is wrong for that path. Either
   require the invariant alone downstream (weaker but always sound) or decline
   loops containing an early exit.

5. **Diagnostics.** A failed obligation 2 is the interesting one and the
   message has to say WHICH iteration-carried fact broke, not just "unknown".
   RT6's counterexample translation is the machinery; the wording is new.

6. **Interaction with `--strict-refine`.** An unproven invariant under strict
   mode is an error. Is a loop with no `:invariant` at all also an error there,
   or silently Unknown as today? Silently Unknown, most likely -- strict mode
   should not turn every existing loop into a build failure.

## Explicitly not in scope

- **Invariant inference** of any kind, including "guess `>= 0` and see". See
  above; this is permanent, not deferred.
- **Termination.** A refinement says nothing about whether the loop finishes,
  and nothing here should imply it does. A non-terminating loop with a true
  invariant is perfectly well-typed.
- **Ranking functions / decreasing measures.** Same reason.

---

## References

- [refinement-types-plan.md](../../archive/refinement-types-plan.md) -- the parent
  plan; "Why checking, not inference" is the constraint this one inherits.
- [../../guides/refinement-types-guide.md](../../guides/refinement-types-guide.md)
  -- the user-facing write-up; the `while` limitation is documented there.
