---
title: defgadt has no :copy form; GADT values are move-tracked
category: Reported
description: GADT values defined with defgadt are affine (move-tracked) and defgadt offers no :copy opt-out the way defdata does. This blocks migrating range.tur's internal bound representation onto the planned `Bound A` GADT, because range bounds are shared and read many times; it also makes BoundedIdx-style checked indexing over SizedVec awkward (computing the length consumes the vector).
---

# defgadt has no `:copy` form; GADT values are move-tracked

> **Severity:** expressiveness gap (blocks a planned refactor; no miscompile).
> **Found:** 2026-06-04, executing
> [stdlib-refinement-collections-plan](../upcoming/stdlib-refinement-collections-plan.md)
> phase R3 (and noted in R2).
> **Resolved:** 2026-06-04 -- implemented fix direction 1. `elab_defgadt`
> now parses an optional `:copy` / `:move` keyword after the type-parameter
> vector and threads `is_copy` onto the `AdtDef` exactly as `elab_defdata`
> does. Regression fixture: `tests/fixtures/gadt-copy/`. See
> `src/compiler/elab_structs.c` (`elab_defgadt`).

## Summary

Values of a `defgadt` type are affine: using one twice is a hard
`TUR-E0005` use-after-move. `defdata` accepts a `:copy` keyword to opt a sum
type out of move tracking, but `defgadt` has no equivalent (and rejects
`:copy` -- see the separate crash report
[defgadt-malformed-pattern-segfault.md](history/defgadt-malformed-pattern-segfault.md)).

This is the concrete blocker behind R3's "internal bound representation moves
to the GADT": range bounds are *shared* values (a single bound pointer is read
by `range-bound-inclusive?`, `range-bound-value`, stored in a `Range`, and
re-read by every predicate). A move-tracked `Bound` cannot stand in for them
without threading ownership through the entire module.

## Minimal repro

```turmeric
(defgadt Bound [A]
  (Inclusive int : (Bound int))
  (Exclusive int : (Bound int))
  (Unbounded  : (Bound int)))

(defn bound-value [b : Bound] : int
  (match b (Inclusive v) v (Exclusive v) v (Unbounded) 0))
(defn bound-inclusive? [b : Bound] : bool
  (match b (Inclusive _) true (Exclusive _) false (Unbounded) false))

(defn main [] : int
  (let [b (Inclusive 7)]
    (println (bound-value b))
    (println (if (bound-inclusive? b) 1 0)))   ; <-- second use
  0)
```

```
error [TUR-E0005]: use-after-move: binding 'b' was moved and cannot be used again
  note: moved here   (println (bound-value b))
```

## Observed vs. expected

- **Observed:** the second read of `b` is rejected; there is no way to mark
  `Bound` copyable.
- **Expected:** a `(defgadt Bound [A] :copy ...)` form (mirroring
  `defdata ... :copy`) that makes the GADT a plain value type, freely
  readable, when the constructors carry only copyable payloads.

## Secondary instance (R2)

The same move tracking applies to `SizedVec` (`stdlib/sized.tur`), a GADT:

```turmeric
(let [v (sized-vec-cons 10 (sized-vec-cons 20 (sized-vec-nil)))]
  (println (sized-vec-len v))
  (println (sized-vec-sum v)))   ; TUR-E0005: v moved by sized-vec-len
```

This makes `BoundedIdx`-style checked indexing over `SizedVec` impractical:
the smart constructor needs the length (which consumes the vector), after
which the vector can no longer be indexed. The R2 implementation in
`stdlib/refined.tur` therefore targets the copyable growable `Vec`
(`stdlib/vec.tur`) and `Slice` instead, which is sufficient for the plan's
stated motivation ("`vec-get`, `slice-get` accept unchecked indices").

## Root cause

`elab_defdata` threads an `is_copy` flag (parsed from a leading `:copy`
keyword) into the `AdtDef`, which the move checker consults. `elab_defgadt`
never parses such a keyword and always produces a move-tracked def. See
`src/compiler/elab_structs.c` (`elab_defdata` `:copy` handling around the
`kw_copy` check vs. `elab_defgadt`, which has none).

## Proposed fix directions

1. **Add `:copy` to `defgadt`.** Parse an optional `:copy` keyword after the
   type-parameter vector and set the same `is_copy` path `defdata` uses. Lowest
   risk; directly unblocks both R3 (shared bounds) and R2 (SizedVec indexing).
2. **Infer copyability** when every constructor payload is itself copyable
   (all-primitive, no linear/affine fields). More ergonomic but a larger
   change to the move checker.

## Validation

- The repro above should compile and print `7` then `1`.
- With `:copy`, R3 could fold `Bound` into range.tur's internals (replacing
  the malloc'd `{bool;int64_t}` RangeBound) and drop the bridge shims in
  `stdlib/range-bound.tur`.

## Current workaround (shipped)

`stdlib/range-bound.tur` keeps range.tur's pointer representation intact and
adds the `Bound A` GADT as an opt-in layer (`-Xgadt`) with bidirectional
compatibility shims (`bound->range-bound` / `range-bound->bound`). No range
consumer is forced onto `-Xgadt`, and no existing API changes.
