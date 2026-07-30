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

## Status (2026-07-29): addressed behind `--enable=sealed-opaque`

Fix direction 1 was taken, as `:sealed`:

```turmeric
(defopaque RGWorld :int :sealed)
```

Outside the declaring module, `::` refuses **both** directions with
`TUR-E0302` -- fabricating the opaque from its representation (which is what
mints the alias) and unwrapping it to the representation (without which the
raw carrier escapes to inline-C anyway). Inside the module `::` is unchanged.

Design, semantics, the two-direction rationale, and graduation criteria:
[docs/upcoming/sealed-opaque-plan.md](../upcoming/sealed-opaque-plan.md).
User-facing docs: [opaques-guide.md](../guides/opaques-guide.md#sealing-an-opaque-sealed).

### Verification

Reproduced first, self-contained (no spice checkout needed): a `defopaque` over
`:int` with a `^unique ^mut` mutator, borrowed with `(& w)`. Mutating the
borrow directly is `TUR-E0200`; the `::`-rebuilt alias compiled clean, ran, and
its mutation was observable through the live borrow. With the experiment on,
the alias is `TUR-E0302`.

Fixtures: `sealed-opaque-in-module` (in-module casts still legal; `:sealed`
composes with `:affine`), `sealed-opaque-gate-off` (parses and imposes nothing
without the flag), `errors/sealed-opaque-cross-module-fabricate`,
`errors/sealed-opaque-cross-module-unwrap`. `bash tests/run.sh` 2416 passed /
0 failed; `bash tests/run-turi.sh` 1671 passed / 0 failed.

### Scope of the claim -- unchanged in kind, only in degree

`:sealed` is a compile-time discipline over the `::` surface. inline-C can
still cast an `int64_t` to anything, in any module, so this does **not** turn
the `frozen` region into an adversarial guarantee. It moves the bypass from
"one `::` away, in ordinary code" to "requires deliberate inline-C." The
report's framing -- a trust boundary, not a capability -- still stands; the
boundary is just considerably harder to cross by accident.

The severity stays low for that reason, and this remains open until the
experiment graduates or is shelved.
