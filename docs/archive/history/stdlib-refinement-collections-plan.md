---
title: Stdlib Refinement / GADT Bridges for Collections
category: Planning
description: Replace partial functions in list/vec/slice with total versions guarded by `NonEmpty<A>` and `BoundedIdx<n>` refinement newtypes. Rewrite `range.tur`'s bound-kind sentinel ints with a `Bound A` GADT. Smart constructors (`ne-from?`, etc.) keep the existing partial APIs callable while making the total ones available.
---

# Stdlib Refinement / GADT Bridges for Collections -- Plan

> **Type:** stdlib API hardening -- refinement types and GADT bridges
> **Prerequisites:** sized vectors / GADTs are already in tree
> (`stdlib/sized.tur`, `stdlib/sized-buf.tur`, `stdlib/gadt-vec.tur`).

## Status (2026-06-04): implemented

All three phases shipped. New files:

- `stdlib/refined.tur` -- **R1** `NonEmpty` (`ne-of`, `ne-singleton`,
  `ne-head`, `ne-tail`, `ne->list`, `ne-len`, `ne-from?`, `ne-unwrap`) and
  **R2** `BoundedIdx` (`bidx-of?`, `bidx->int`, `bidx-unwrap`,
  `vec-get-checked`, `slice-get-checked`).
- `stdlib/range-bound.tur` -- **R3** the `Bound A` GADT
  (`Inclusive`/`Exclusive`/`Unbounded`) with constructors, accessors, the
  `bound-range` builder, and the bidirectional compatibility shims
  `bound->range-bound` / `range-bound->bound`.

Fixtures: `tests/fixtures/refined-nonempty`,
`tests/fixtures/refined-bounded-idx`, `tests/fixtures/range-bound-gadt`.

### Deviations from the design block (and why)

The design sketches used aspirational syntax that the compiler does not yet
support; each was adapted to a working, idiomatic form and the underlying gap
filed as a report:

1. **`(defopaque NonEmpty A int)` / `(defopaque BoundedIdx n int)`** --
   `defopaque` takes no type-parameter vector, so both are monomorphic
   carrier-level opaques over `:int` (the element type `A` / length `n` live
   at the value level, enforced by the smart constructors). The total-accessor
   guarantee is unchanged. See
   [docs/reported/parameterized-defopaque.md](../reported/parameterized-defopaque.md).
2. **R2 checked indexing targets `Vec`/`Slice`, not `SizedVec`.** `SizedVec`
   is a GADT and its values are move-tracked, so computing the length consumes
   the vector and it can no longer be indexed -- exactly the "sized-vec
   ergonomics" blocker the Risks section anticipated. `Vec`/`Slice` are
   copyable int handles and directly address the stated motivation
   (`vec-get`/`slice-get` accept unchecked indices). See
   [docs/reported/defgadt-copy-and-shared-bounds.md](../reported/defgadt-copy-and-shared-bounds.md).
3. **R3 is an opt-in layer, not an in-place rewrite of range.tur's
   internals.** Two blockers: `defgadt` requires `-Xgadt` (putting the GADT in
   range.tur would force the flag on every range consumer, including the `for`
   macros), and GADT values are move-tracked while range bounds are freely
   shared/re-read (`defgadt` has no `:copy`). The Bound GADT therefore lives in
   `stdlib/range-bound.tur` (`-Xgadt`) and bridges to range.tur's untouched
   pointer representation via the compatibility shims, so no existing caller
   changes. Folding the GADT into range.tur's internals is unblocked once
   `defgadt :copy` lands (see the report above).

The internal-helper migration in `option.tur` / `list.tur` was intentionally
**not** done: `refined.tur` depends on `list.tur`/`option.tur` (auto-loaded),
so making those files call back into `refined.tur` would be a load cycle.

A compiler crash found while probing `:copy` support is filed separately:
[defgadt-malformed-pattern-segfault.md](../archive/history/defgadt-malformed-pattern-segfault.md).

## Motivation

`sized.tur` / `sized-buf.tur` already model `SizedVec n` / `SizedBuf n`,
and `gadt-vec.tur` demonstrates GADT typing for length-indexed vectors.
Two partial-function clusters remain unprotected:

1. **`list-head`, `vec-first`, `slice-first`** are partial on empty
   input.
2. **`vec-get`, `slice-get`** accept unchecked indices.

`range.tur` separately encodes inclusive / exclusive / unbounded as
sentinel ints (`range-bound-new [inclusive : int ...]`,
`range-abort-not-connected`), a classic stringly-typed sum that the
GADT machinery can replace.

## Design

### `NonEmpty<A>` phantom-tagged Cons

```turmeric
(defopaque NonEmpty A int)
(defn ne-head  [xs : NonEmpty<A>]              : A)              ;; total
(defn ne-of    [x  : A xs : List<A>]           : NonEmpty<A>)
(defn ne-from? [xs : List<A>]                  : Option<NonEmpty<A>>)
```

### `BoundedIdx<n>` refinement on int

```turmeric
(defopaque BoundedIdx n int)
(defn vec-get-checked [v : SizedVec<n A> i : BoundedIdx<n>] : A)
```

### `Bound A` GADT for range endpoints

```turmeric
(defadt Bound A
  Inclusive [A]
  Exclusive [A]
  Unbounded [])
```

`range.tur`'s internal bound representation moves to the GADT; a
wrapped `int`-returning compatibility shim keeps the existing API
working through one minor release.

## Scope

- Add `NonEmpty`, `BoundedIdx` to `stdlib/sized.tur` (or a new
  `stdlib/refined.tur`).
- Rewrite `tur/range`'s internal bound representation to use the GADT.
- Migrate `option.tur` / `list.tur` internal helpers to use the total
  accessors where possible.

## Phasing

1. **R1** -- `NonEmpty` + total accessors. Migrate `option.tur`,
   `list.tur` internal helpers. Add fixtures for `ne-head`,
   `ne-from?`.
2. **R2** -- `BoundedIdx` + sized-vec checked indexing. Opt-in -- the
   existing `vec-get` stays partial; `vec-get-checked` is the new
   total form.
3. **R3** -- `range.tur` bound-kind GADT rewrite. Compatibility shim
   keeps existing callers working.

## Out of scope

- **Decidable refinement solving**; the user supplies the proof
  obligation via `ne-from?`-style smart constructors.
- **Removing the existing partial accessors**; this is purely additive.

## Risks

- **User-visible breakage** is opt-in: total accessors are new names;
  the partial ones stay. R3 has the largest churn surface because
  `range.tur` is imported by `for` macros; the compatibility shim
  must be exhaustive.
- **Sized-vec ergonomics.** `BoundedIdx` adoption depends on
  `SizedVec`'s existing ergonomics, which are spike-validated but not
  widely used in stdlib. Track adoption blockers in this plan as they
  surface.

## Acceptance

- `NonEmpty<A>` / `BoundedIdx<n>` / `Bound A` types are exported.
- Fixtures exercise the total accessors and the GADT.
- `range.tur` rewrite ships with a passing compatibility-shim
  regression fixture.
- `bash tests/run.sh` passes with zero `FAIL` lines.
- `tur run docs` regenerated.

## Cross-references

- Independent of opaque-handle / linearity / session / effects work.
- Split out from the original umbrella `stdlib-advanced-typing-plan`.
