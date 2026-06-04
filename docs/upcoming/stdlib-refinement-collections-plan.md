---
title: Stdlib Refinement / GADT Bridges for Collections
category: Planning
description: Replace partial functions in list/vec/slice with total versions guarded by `NonEmpty<A>` and `BoundedIdx<n>` refinement newtypes. Rewrite `range.tur`'s bound-kind sentinel ints with a `Bound A` GADT. Smart constructors (`ne-from?`, etc.) keep the existing partial APIs callable while making the total ones available.
---

# Stdlib Refinement / GADT Bridges for Collections -- Plan

> **Type:** stdlib API hardening -- refinement types and GADT bridges
> **Prerequisites:** sized vectors / GADTs are already in tree
> (`stdlib/sized.tur`, `stdlib/sized-buf.tur`, `stdlib/gadt-vec.tur`).

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
