# `::` coercing cast (and inline-C) can reconstruct an alias that bypasses a `frozen` region

**Severity:** low (bounds the `frozen`-region guarantee to well-behaved code; it
is a deliberate escape hatch, not silent unsoundness -- the same trust boundary
inline-C and the whole `#reads` feature already carry). Worth recording because
it is the limit of the RE1/aliased-mutation soundness story.

## Summary

The `frozen` region's soundness argument (`refine-stateful-measures-plan.md`,
"aliased-mutation") is: while `(& w)` is live, no *mutating* handle to the frozen
world can be acquired, because a mutator takes `^unique ^mut w` and uniqueness
forbids a second mutable handle. That holds against ordinary code. It does NOT
hold against the `::` coercing cast: a world handle rides the int64 carrier, and
`::` freely converts both directions across a module boundary, so a caller can
mint an ALIAS and mutate through it inside the region.

## Reproduce

With `spices/ecs/src/ecs/refined-world.tur` (RE1's `defopaque RGWorld :int`
facade, whose only exported mutators are `^unique ^mut`):

```turmeric
(frozen w
  (let [w2 (:: (:: w :int) RGWorld)]   ;; unwrap to the backing int, re-wrap as a fresh handle
    (rgworld-despawn! w2 e)))          ;; w2 is OWNED, not the borrowed w -> no TUR-E0200
```

Both casts compile (exit 0): `(:: w :int)` unwraps the opaque, `(:: int RGWorld)`
reconstructs it. `(rgworld-despawn! w e)` on the *actual* borrowed `w` is
correctly `TUR-E0200`; the aliased `w2` is not, so the despawn goes through and
the region's "no despawn here" invariant is broken. A `defstruct` field
(`(.ctrl w)` + `(RGWorld ctrl)`) is the same hole through a different door.

## Why it happens / is it a bug

`::` is a **coercing** cast (turmeric's carrier-interop escape hatch, e.g.
`(:: (make-write-cap 0) (WriteCap Pos))`), not a checked ascription -- so
int<->opaque both ways is intended. `defopaque` therefore does not encapsulate a
handle against reconstruction, and there is no mechanism today to make a
carrier-typed handle non-reconstructable (a module-private constructor / a
`::`-opaque newtype).

## Consequence for RE1

RE1's shipped accessor module (`ecs/refined-world`) is a **congruence +
compile-time guard for ordinary code**, sound for callers that reach the world
only through its API -- documented as a trust boundary in the module docstring.
It is not a capability that survives a hand-written `::`/inline-C bypass. If a
hard, adversarial guarantee is ever wanted, it needs a language feature:
module-private construction, or an opaque newtype the `::` cast refuses to
fabricate.

## Fix directions (language, if pursued)

- A `defopaque` variant whose constructor is private to the defining module and
  which `::` will not fabricate from the representation type.
- Or a lint/error when `::` targets such a sealed type from outside its module.
