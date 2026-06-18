---
title: HKT class dispatch -- options tradeoff (Phase 2.1 measurement + Phase 2.2 recommendation)
category: Planning -- ABI / Codegen rework (M6 design pass)
description: The HKT dispatch design pass for end-to-end-monomorphization-plan.md Phase 2. Measures the current HKT call-graph surface (stdlib + turmeric-spices) and scores the three dispatch models on blast radius, expressiveness, and -- decisively -- whether they retire or reintroduce the int64 carrier ABI.
---

# HKT class dispatch -- options tradeoff

**Snapshot:** 2026-06-18. Measured against `stdlib/` and a fresh
`../turmeric-spices/` checkout (depth-1 clone).

This is the Phase 2.1 deliverable (measurement) + the Phase 2.2 recommendation
of [`end-to-end-monomorphization-plan.md`](../end-to-end-monomorphization-plan.md).

## 2.1 -- Measured HKT surface

### Classes (9)

Functor, Applicative, Monad, Foldable, Traversable, Alternative, Bifunctor,
Comonad, MonadError. Declared in `stdlib/typeclass*.tur`, `stdlib/comonad.tur`,
each with an HKT type param (`[^f]` / `[^m]` / `[^^f]`) and methods that
currently return the **int64 carrier** (`... : int`).

### Instances (30 distinct; all in stdlib, 0 in spices)

| Class | Instances |
|---|---|
| Functor | Option, Result (`_ B`), Either (`E`), Parser, Goal, Backtrack, Schema, Cons, rc |
| Applicative | Option, Parser, Goal, Backtrack, Schema |
| Monad | Option, Result (`_ B`), Parser, Goal, Backtrack |
| Alternative | Option, Parser, Goal, Backtrack, Schema |
| Bifunctor | Result |
| Foldable | rc |
| MonadError | Result (`_ B`) |
| Comonad | identity, pair |

`../turmeric-spices/` declares **zero** HKT instances and makes **zero**
generic-combinator calls in application code (only READMEs mention them).

### Generic combinator call sites

- **stdlib + spices application code: ~0.** Every `(fmap ...)` / `(bind ...)` /
  etc. occurrence is an instance *method body* (`(fmap [container fn] ...)`), a
  `;;;` docstring, a `;;` comment, or a README. The modules *define* the
  instances but do not *consume* the generic combinators.
- **test fixtures: ~21 genuine dispatching calls** (e.g.
  `hkt-stdlib-option-result-instances`, `schema-applicative-user`,
  `hkt-stdlib-parser-instances`, `sum-either-functor-instance`).

### The `A` (element-type) dimension

At the ~21 fixture call sites the element type is overwhelmingly `int`
(19 occurrences), with `Result int int` (6), `Option int` (4), `Parser int`
(1), and a single struct payload (`->User`, 2 occurrences). So the per-`A`
multiplicity is **tiny** -- effectively `{int, a couple of structs/cstr}` per
instance, not an open-ended set.

### Blast-radius estimate (option 1, full per-`(f, A)` clone)

`distinct clones  ~=  (instances actually dispatched)  x  (element types per instance)`.

With ~30 instances, near-zero in-stdlib dispatch, and an element-type set of
~1-3 per dispatched instance, the upper bound on emitted clones is **low tens**,
not hundreds or thousands. The plan's stated worry ("per-instantiation
expansion blast radius is not obvious") resolves empirically to **the blast
radius is small** on the current codebase. A precise count still needs an
elaborator clone-count probe (not yet built); this is a measured *bound*, not
an instrumented exact figure.

> Caveat: the plan quoted "~199 fmap/bind/pure/lift2 grep hits". The current
> tree yields **52** such hits across stdlib+spices, and only ~21 are genuine
> dispatching calls (the rest are method bodies / docs / comments). Either the
> spices tree shrank or the original count included non-call lines. Re-measure
> if the spices surface grows materially.

## 2.1 -- Option comparison

### Option 1 -- full per-`(f, A)` monomorphization (clone)

- **Binary size:** +low-tens of small clones on the current surface (measured
  bound above). Scales with `instances x element-types`; both are small today.
- **Compile time:** negligible at this surface.
- **Expressiveness:** full. Recursion and polymorphic recursion work (each
  needed `(f, A)` is a distinct concrete function, same as ordinary generic
  monomorphization). Nested combinators (`(fmap (fmap f) x)`) just request
  more `(f, A)` tuples.
- **Carrier ABI:** **retires it.** Each clone is concretely typed end-to-end --
  this is the only option that finishes the none-as-NULL / no-`:int` goal the
  umbrella plan exists for.

### Option 2 -- dict-passing with payload erasure at the HKT boundary

- **Binary size:** smallest (one dict per instance, no per-`A` clones).
- **Compile time:** smallest.
- **Expressiveness:** full (dicts handle recursion fine).
- **Carrier ABI:** **reintroduces it.** "Payload erasure at the HKT boundary"
  is exactly the int64 carrier this plan is deleting -- a `(fmap xs f)` that
  erases the element to int64 to call through a dict is the same value-erasure
  that produces the `(Option float)` mis-cast class of bug (see
  `kleisli-k-apply-raw-B-uninferable.md`, `polymorphic-float-carrier-ascription-value-cast.md`).
  Choosing option 2 would directly contradict Phases 4-5 (carrier-helper
  rewrites + bridge deletion). **Disqualifying for this plan's goal.**

### Option 3 -- source-rewriting inline-expansion

- **Binary size:** can blow up on nested combinators (each `(fmap (fmap f) x)`
  inlines both layers at the use site; shared instances are not deduplicated).
- **Compile time:** worse with nesting.
- **Expressiveness:** **regresses.** Polymorphic recursion cannot be
  inline-expanded to a finite program (the classic reason typeclasses are not
  macro-expanded); recursive instance methods (Parser, Goal, Backtrack all
  recurse) would not terminate expansion.
- **Carrier ABI:** retires it, but at the cost of the expressiveness regression
  above.

## 2.2 -- Recommendation: **Option 1** (full per-`(f, A)` clone)

Three reasons, in priority order:

1. **It is the only option that achieves the plan's goal.** Option 2
   reintroduces the int64 carrier at the HKT boundary -- the exact thing
   Phases 4-5 delete -- so it is self-defeating. Option 3 retires the carrier
   but regresses polymorphic/recursive instances (Parser/Goal/Backtrack).
   Option 1 retires the carrier *and* keeps full expressiveness.
2. **The measured blast radius is small** (low tens of clones), well under the
   plan's "default to option 1 unless 2.1's binary estimate is > 2x larger"
   gate. The 2x concern does not materialize on the current surface.
3. **It reuses existing machinery.** Option 1 is the same per-spec
   monomorphization the non-HKT M4 path already uses
   (`find_matched_abi_spec` / `emit_abi_intern_spec`); Phase 3.2 extends it to
   HKT-constrained combinators rather than introducing a new dispatch kind.

### Acceptance criteria pinned for Phase 3

- Zero source-side changes at the ~21 existing call sites (they keep their
  current `(fmap (:: (some 21) (Option int)) dbl)` form; only the emitted ABI
  changes from carrier to by-value clone).
- HKT method results stop returning the int64 carrier (`... : int` in the
  class decls becomes the concrete element type per clone).
- Nested `(fmap (fmap f) x)` reaches all needed `(f, A)` monomorphizations
  without a worklist cycle (Phase 3.2 correctness criterion).
- `bash tests/run.sh` green; the spice suites (`ecs`, `json`) green;
  `TUR_M3_AUDIT` shows no new carrier crossings at HKT boundaries.

### Open item before Phase 3 starts

Build the elaborator clone-count probe to convert the "low tens" *bound* into
an *exact* per-`(f, A)` figure, and re-run it against the spices tree at the
time Phase 3 begins (the surface may have grown). The bound is sufficient to
*choose* option 1 now; the exact figure is needed to size Phase 3 in sessions.

## Cross-references

- [`end-to-end-monomorphization-plan.md`](../end-to-end-monomorphization-plan.md)
  Phases 2-3.
- `docs/reported/kleisli-k-apply-raw-B-uninferable.md` (kind-`*` `Category`
  blocks the Kleisli arrow's element type -- a concrete instance of why HKT
  dispatch is needed; Phase 1.4 is gated on this work).
- `docs/reported/polymorphic-float-carrier-ascription-value-cast.md` (the
  carrier-erasure mis-cast class option 2 would perpetuate).
