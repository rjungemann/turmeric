# Dogfooding `refined` on a real program

**Status: EXECUTED 2026-07-26 on tur-ecs. ARCHIVED 2026-08-01** -- both things
it fed have landed (Z3 retirement in 0.32.5, graduation for v0.33.0), so it is
finished work, not shelved work. It sat in `docs/upcoming/hold/` for a while
after it had already been executed, which is what the move corrects.

The program this was waiting for came to exist: `spices/ecs` (~5400 lines, 22
modules, 66/66-green suite) carrying the RE1 refined surface. Results --
1.004x cost on unannotated code, zero `TUR-E0371`, Z3 oracle agreement on every
real VC, tier coverage with honest skips -- are in
[`refined-dogfood-ecs-report.md`](refined-dogfood-ecs-report.md),
which follows this plan's report format. The tier checklist below is kept as
written; per-item outcomes (covered / absence-result / skipped-no-natural-
site, including the stdlib-alias mismatch finding) live in the report.

**Why it was held rather than run earlier:** everything measured up to that
point came from fixtures, and fixtures are written by the same person who wrote
the checker. They cover what was thought of. A real program is the only source
of the shapes that were not -- so the plan waited for one to exist rather than
contorting a fixture into standing in for it.

**What it fed (both now closed):**

- [refined-graduation-plan.md](refined-graduation-plan.md)
  precondition 2 -- compile-time cost on something that is not a fixture
  designed to be hard. Satisfied; graduation executed 2026-08-01.
- The Z3 retirement decision. The oracle's whole value is cross-checking VCs
  the corpus cannot supply, and only a real program generates those. Evidence
  banked 2026-07-26; scaffold deleted 2026-07-30 in 0.32.5.

---

## The two questions this answers

Everything else is secondary.

1. **Does `TUR-E0371` fire on correct code?** Static discharge becomes
   unconditional at graduation, so a false positive stops being an opt-in
   nuisance and becomes everyone's build failure. One instance should halt the
   flip.
2. **What does it cost?** Fixtures say ~1.7x on a crossing-heavy file and
   nothing at all on a file with no refinements. A real program has a real
   ratio, and that is the number that belongs in the graduation decision.

A distant third: **is the annotation burden tolerable?** Not a blocker -- the
feature is opt-in per annotation -- but if writing refinements is unpleasant
enough that nobody does, graduating is ceremony.

---

## What the program should contain

This is the part worth planning, because coverage here is what makes the
exercise worth running. Roughly in order of how much I want to see it.

### Tier 1 -- the recently-landed semantics, which have never met real code

These are the reason precondition 3 says "let it sit". Each landed within days
of this being written.

- [ ] **Calls guarded by an `if`, a `let`, or a `match`.** Crossings recover
      path conditions by walking the caller's body, so
      `(if (> n 0) (safe-div 10 n) 0)` should discharge. Include cases where
      the guard is several branches up, and at least one where the guarded
      value is rebound by a `let` on the way.
- [ ] **A caller with parameters that passes a literal to a refined callee.**
      This is what goal-based closedness changed: `(defn f [n : int] ...
      (safe-div 10 0))` is now an error where it used to be silent. If any
      correct code trips this, that is finding #1 above.
- [ ] **A typeclass with a refined method parameter, and instances that vary.**
      One instance restating the class demand, one omitting the annotation
      (inherits it), one explicitly weakening it with a bare `: int`. Call all
      three, and call one through a **generic function** so the dispatch is
      dynamic. This exercises inheritance, the class-signature crossing, and
      `TUR-W0377` in one shape.

### Tier 2 -- the machinery that has fixtures but no real use

- [ ] **`match` on an ADT with refined arms.** Arm selection contributes
      hypotheses: a literal pattern's equality, a guard verbatim, a
      constructor's tag, a field selector. Constructor axioms mean
      `(.a (Box p q))` reduces to `p`, so a program that builds and destructures
      records is the natural exercise.
- [ ] **`:pre` and `:post`.** Fixtures use them; nothing real does.
- [ ] **The stdlib aliases** -- `Nat`, `Pos`, `Byte`, `Percent` from
      `stdlib/refine.tur`. If a real program reaches for a refinement and none
      of these fit, that is a signal about what the stdlib layer is missing.
- [ ] **Floats with non-zero fractional parts.** Per the repo's float rule:
      `7.1`, `3.25`, never `7.0`. Real-sorted refinements go down the QF_LRA
      path, and an integer-valued float literal cannot show a truncation bug.
- [ ] **Recursion, including one mutually-recursive pair.** Result-refinement
      propagation is order-dependent for a function's own return obligation --
      tagged `[incomplete]` in the guide -- and mutual recursion is exactly
      where a hypothesis goes missing. Worth seeing whether that is noticeable
      or theoretical.

### Tier 3 -- the limits, to confirm they are the right ones

Not to make them pass, but to check the guide describes what actually happens.

- [ ] **A `while` loop that accumulates into a refined value.** This is the
      `[deferred]` limit and the trigger condition in
      [loop-invariants-plan.md](loop-invariants-plan.md) is literally "a real
      program wants it". If the program naturally writes one, that plan comes
      off hold.
- [ ] **A function value passed to a function-typed parameter**, where the
      function has refined parameters. The `[prototype]` limit. Confirm the
      runtime check still catches a violation, and that nothing about the
      experience is more confusing than the guide says.
- [ ] **Nonlinear arithmetic** in a predicate (`(* x y)` with both variables).
      Uninterpreted by design; confirm it degrades to a runtime check quietly
      rather than producing something alarming.

### What the program should NOT be

- Not a benchmark suite or a synthetic exercise. The point is code someone
  would write anyway, where refinements are added because they express
  something true rather than to cover a checklist. **If covering a tier item
  requires contorting the program, skip that item and say so** -- a forced
  shape tells us less than its absence does.
- Not exclusively arithmetic. Index bounds and non-zero divisors are the
  obvious uses and are already well covered by fixtures.
- Not tiny. The cost measurement needs enough refinement density to be a
  ratio rather than noise; a few hundred lines with a dozen or so refined
  signatures is a reasonable floor.

---

## How to run it

```sh
# 1. Baseline and gated build, for the cost ratio.
time tur build src/main.tur
time tur build --enable=refined src/main.tur

# 2. The strict pass -- shows everything the solver could not discharge,
#    which is the annotation-burden picture.
tur check --enable=refined --strict-refine src/main.tur

# 3. The oracle cross-check. This is the retirement evidence, and it only
#    exists while the scaffold does -- run it BEFORE Z3 is deleted.
cmake -S . -B build-oracle -DCMAKE_BUILD_TYPE=Debug -DTUR_REFINE_Z3_ORACLE=ON
cmake --build build-oracle -j
./build-oracle/tur build --enable=refined src/main.tur
```

Step 3 is the one that cannot be repeated later. An in-house stage claiming
`RT_VALID` where Z3 says `RT_INVALID` reports `TUR-I0379` and downgrades to
Unknown, so a soundness bug on real VCs surfaces as a diagnostic rather than as
a wrong answer -- but only in an oracle build.

## What the report should record

Short, and mostly numbers:

- compile time with and without the gate, and the ratio;
- counts of `TUR-E0371`, `TUR-W0372`, `TUR-W0377`, and any `TUR-I0379`;
- for every `TUR-E0371`: whether the code was actually wrong. **This is the
  finding.** A single "no, that code was correct" is worth more than the rest
  of the report combined;
- which tier items the program covered and which it could not, with the reason
  -- an item skipped because the program had no natural place for it is itself
  a result about what real code does;
- anything the guide describes inaccurately, which is the cheapest kind of
  finding and the easiest to lose.

## Trigger

A Turmeric program that someone is writing for its own sake, large enough to
have a compile time worth measuring. Adapting an existing spice is fine and
probably better than writing something new -- an existing program has shapes
nobody chose for this exercise, which is exactly the property that makes it
useful.
