---
title: SizedVec size index is phantom; size equality is runtime-only
category: Expressiveness hole / partial implementation
severity: Limits static-rectangularity claims (ECS dense-storage zip, matrix shape checks, sized-iterator joins). v1 plans accept a runtime length check and lift to compile time once SZ6 lands.
description: `docs/guides/sized-types-guide.md` documents `SizedVec<n, T>` and `SizedMatrix<r, c, T>` with kind-level naturals, but in the current implementation the size index is a phantom -- every constructor returns the same underlying type regardless of `n`, and shape equality is enforced at runtime, not by the elaborator. The ECS v1 plan has been revised to use a runtime check; lifting to compile time when SZ6 ships is a transparent surface-API improvement, not a plan-level rewrite.
---

# SizedVec size index is phantom

## Summary

The sized-types guide presents `SizedVec<n, T>` as if the elaborator
tracks `n` and rejects shape mismatches at compile time. In practice,
the size parameter is a phantom: the constructor erases `n` to the
underlying `Vec<T>`, and `(zip xs ys)` of two `SizedVec`s checks length
at runtime. The plan to make the index load-bearing (SZ6-SZ9) is
in-progress but not landed.

## Where this bites

- `docs/upcoming/v1/ecs-spice-plan.md` originally claimed "a dense
  storage of length `n` yields a `Vec<n, T>`; zip of two dense storages
  over a shared `n` is statically rectangular." The plan has been
  revised: that claim now lives in the "Deferred to v2" subsection and
  the v1 track checks length at runtime. E2c is the SZ6-gated milestone.
- Any matrix / linear-algebra / sized-iterator plan that quotes "shape
  errors are compile-time" against today's implementation is
  overclaiming.

## Observed vs. expected

**Observed.** `SizedVec<3, int>` and `SizedVec<4, int>` have the same
underlying representation and pass the same type slot. A `zip` of the
two compiles and panics at runtime on length mismatch.

**Expected (per the sized-types-guide target state).** The elaborator
carries the size term, equates `SizedVec<n, T>` with `SizedVec<m, T>`
only when `n` and `m` unify (literally, or modulo a kind-level
arithmetic theory), and rejects `(zip xs ys)` at compile time if the
sizes do not unify.

## Root cause

Kind-level naturals exist as a syntactic surface but the elaborator
does not propagate them through constructors or unify them in type
equality. The sized-primitives guide hints at this (the size index is
described as a "witness" rather than a participating type parameter).
SZ6 onward is the work to make it load-bearing.

## Proposed directions

1. **Land SZ6 (size index participates in type equality).** Even
   without arithmetic, literal-vs-literal and variable-vs-variable
   unification is enough for the ECS dense-storage zip case.
2. **Defer kind-level arithmetic to a later phase.** Concatenation
   yielding `SizedVec<n+m, T>` is nice but not required for ECS; the
   per-frame inner loop has fixed `n` already.
3. **Until then, update plans that quote static rectangularity** (ECS
   v1, any matrix work) to name "runtime length check, with a path to
   compile-time once SZ6 lands" as the v1 reality.

## Validation of a fix

- `(zip (sized-vec [1 2 3]) (sized-vec [1 2]))` is a compile error,
  not a runtime panic.
- A function `(defn dot [xs : SizedVec n int] [ys : SizedVec n int])`
  unifies the two `n`s and rejects callers that pass differently-sized
  literals.
- Constructor erasure: `(sized-vec-from-vec v)` produces
  `exists n. SizedVec n T` (existential), and zipping two such
  existentials requires an explicit witness equality check.

## Related

- `docs/guides/sized-types-guide.md` (SZ6-SZ9)
- `docs/guides/sized-primitives-guide.md`
- `docs/upcoming/v1/ecs-spice-plan.md` lines 235-240
- Memory: `project_sized_types_phase.md` (SZ0 complete; SZ1 next)
