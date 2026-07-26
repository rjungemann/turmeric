# Stateful Refinements -- frozen regions and `#reads`

> **Status: in-flight.** The `frozen` region form ships today in the `tur-ecs`
> spice (`ecs/freeze`). The `#reads` annotation and the congruence grant it
> enables are **specified here and not yet implemented** -- this guide is their
> spec as much as their manual. They are gated behind `--enable=refined` and
> tracked in
> [`docs/upcoming/v1/refine-stateful-measures-plan.md`](../upcoming/v1/refine-stateful-measures-plan.md).
> Read the [Refinement Types guide](refinement-types-guide.md) first; this one
> assumes it.

## The gap this fills

A refinement predicate is only useful if the solver can reason about the terms
in it, and the load-bearing move is **congruence**: treating two occurrences of
`(alive? w e)` -- one in a guard, one in a crossing -- as the *same value*, so a
guard discharges the read it protects.

The Refinement Types guide states the rule that blocks this for mutable things:
[*"a measure must be provably pure"*](refinement-types-guide.md). A predicate
about mutable state is not pure -- `alive?` reads a generation counter, `open?`
reads a socket's state, `in-bounds?` reads a buffer's current length -- so each
occurrence gets a *distinct* opaque symbol, and:

```turmeric
(if (alive? w e)
  (get-Pos! w e)      ;; the crossing does NOT discharge -- `0 proven, 1 unknown`
  (handle-dead))
```

does not prove. And that is *correct* by default: with a `tick` that counts up,
`(- (tick) (tick))` is `-1`, not `0`, and the same reasoning applies to a world
despawned between the guard and the read. Congruence on an impure measure is a
miscompile, and the refinement design refuses it.

Stateful refinements recover the guard-discharges-the-read pattern for mutable
state **without** loosening that rule, using two pieces:

1. a **region** in which the state provably cannot change, and
2. a **declaration** of which state a measure reads,

so that *inside the region* the measure is a function of frozen state and its
pure arguments -- and therefore congruent *there*, and only there.

## Piece 1 -- the `frozen` region (ships today)

`(frozen w body...)` runs `body` while the owned value `w` is **borrowed** for
the region's extent. It lowers to an ordinary borrow held across a `let`:

```turmeric
(frozen w
  (read-only-pass w))     ;; ==  (let [_ (& w)] (read-only-pass w))
```

The point is what the borrow forbids. Declare the mutator that could change the
state to require **exclusive** access:

```turmeric
(defn despawn! [^unique ^mut w : World e : Entity] : nil ...)
```

Inside `(frozen w ...)` a borrow of `w` is live, so passing `w` as `^unique ^mut`
is rejected -- `TUR-E0200: cannot pass 'w' as ^unique ^mut -- active borrow
exists`. A despawn of `w` is a **compile error** in the region, not a runtime
check.

This is sound by construction, and needs no new machinery:

- The authority to mutate `w` is `w`'s own exclusive-mutable access. It cannot be
  forged from a shared borrow, so there is no capability to mint, hoard, or alias
  -- the failure mode a capability token would have.
- `frozen` *borrows*, it does not consume: `w` is usable again after the region,
  so a real `despawn!` outside the region is fine.
- Read-only accessors take `[^borrow w]` and coexist with the region borrow, so
  reads stay callable inside; only the exclusive mutator is locked out.

Requirements: `w` is an **owned** `^mut` local (the frame that mutates owns the
value); a `^borrow` parameter does not register the borrow the region needs. The
region body is a plain `let` body -- any result type, and elaborated *inline*
(not a closure), which is what lets the refinement encoder see it.

> `frozen` is exported by `ecs/freeze` in `tur-ecs`. It is world-agnostic -- the
> same shape freezes a file handle, a buffer, a lock, or a transaction, given a
> mutator declared `^unique ^mut`.

## Piece 2 -- `#reads w` (specified, in-flight)

The region proves the state is frozen, but the encoder still does not know that
`alive?` *depends on* that state -- `alive?`'s body is inline C, opaque to the
purity walk, so it is simply "impure" and gets a fresh symbol per occurrence,
region or no region (verified: still `0 proven, 1 unknown` inside `frozen`).

`#reads w` supplies the missing fact. It annotates a measure with the borrowed
argument whose mutable state it reads:

```turmeric
(defn alive? [^borrow w : World e : Entity] #reads w : bool
  ;; body reads gens[idx] out of w's control block through inline C,
  ;; which is why the purity walk cannot see the dependence on w.
  ...)
```

`#reads w` names a `^borrow` parameter. With it, the encoder's congruence rule
gains one arm:

- a call `(alive? w e)` whose callee declares `#reads w` is given a **stable**
  (congruent) symbol **when a live borrow of `w` is in scope** -- i.e. inside
  `(frozen w ...)`;
- and a **fresh** symbol everywhere else, exactly as an impure measure gets
  today.

Region exit, a `set!` of `w`, and the `do`-split rule all invalidate the
hypothesis -- the same three invalidation sites, sharing one predicate. So:

```turmeric
(frozen w
  (if (alive? w e)
    (get-Pos! w e)      ;; discharges: alive?(w,e) is congruent in the region
    (handle-dead)))
```

proves, and the same code *outside* a `frozen w` does not.

A measure written in **pure Turmeric** -- reading `w` through ordinary field
accesses, no inline C -- needs no `#reads`: it is already congruent by the
existing rule ("a field read is as pure as its receiver"). `#reads` is only for
measures whose dependence on the state is *invisible* to the purity walk, which
in practice means an inline-C or otherwise-opaque body.

## Why a *trusted* annotation is sound here

`#reads w` is a **promise, not a checked fact** -- the compiler cannot look into
an inline-C body to confirm the measure reads *only* `w`'s state and nothing
else. The Refinement Types guide is emphatic that a trusted purity attribute is
[forbidden](refinement-types-guide.md), for a precise reason: *"the cost of a
wrong purity claim is an elided check."*

That reason does not apply here, because the check that matters is **not
elided**. The guide's own limit spells it out:

> **[by design] A callee's entry check is never elided.** The call-site layer
> reports, it does not remove the callee's guard.

A guarded stateful read has *two* checks:

| check | who | elided by a proof? |
|---|---|---|
| the **crossing** at `(get-Pos! w e)` -- verifies `(alive? w e)` at the call site | the caller | **yes** |
| `get-Pos!`'s **own entry check** -- the `gens[idx]` compare before it reads | the callee | **no, ever** |

A congruence proof from `#reads` + `frozen` elides only the *crossing* check.
So a **wrong** `#reads w` -- a measure that secretly reads other mutable state
-- costs a **missed compile-time lint**: an unguarded read that should have
warned `TUR-W0372` compiles clean. It can **never** cause a use-after-free: the
callee's entry check still runs and aborts on a dead handle at runtime.

That is exactly the asymmetry the refinement design is built on -- *the cost of a
wrong claim is a **kept** check, not an elided one* -- reached by declining to
elide the safety check rather than by refusing the declaration. `#reads` buys a
**stronger compile-time signal** (the guard is now provable), never a weaker
runtime guarantee.

The corollary is a rule of thumb: **do not use `#reads` to elide the safety
check itself.** Its whole soundness argument is that the kept entry check is the
backstop. A design that elided the entry check on the strength of a `#reads`
proof would be unsound, and is out of scope by construction.

## Where this generalizes

`#reads` is narrower than it looks in one direction and broader in another, and
both are worth knowing.

**Broader: it is not an ECS feature.** Aliveness is one instance of "a predicate
about a mutable resource, congruent in a scope where that resource is frozen."
The same `frozen` + `#reads` pair covers an open file (`(open? conn)`), a
resizable buffer (`(in-bounds? buf i)` -- the bounds-elimination case
[`loop-invariants-plan`](../upcoming/hold/loop-invariants-plan.md) wants), a held
lock, a session in a state, a row inside a transaction. `ecs/freeze` lives in
the ECS spice only because the ECS is the first program that demanded it.

The concept has a well-established name outside Turmeric: a **`reads` clause**,
as in Dafny's `reads`/`modifies` frames or separation logic's read/write
footprints -- the standard way heap-aware verification states what a function may
touch. `#reads w` is a coarse (per-argument, not per-heap-location) `reads`
clause.

**Narrower: the *trusted* form is refinement-only.** The same read-frame
information would enable common-subexpression elimination, safe parallelization
(two operations with disjoint read/write frames may run concurrently -- which is
what `tur-ecs`'s `WriteCap`/`ReadCap` scheduler already does by hand, per
component), and reactive/incremental recomputation. But every one of those
*elides or reorders real work* on the strength of the claim, so they would need
`#reads` to be **checked**, not trusted -- and checking an inline-C body is the
wall this trusted form steps around. The trusted variant is sound precisely
because it changes nothing at runtime.

### The intended trajectory

`#reads` is introduced deliberately as the **minimal, trusted, refinement-only**
slice, shaped so a stronger version can grow from it without a rename or a
semantics break:

1. **trusted now** -- a promise, sound because the safety check is kept
   (this guide);
2. **checkable later** -- verified by the purity walk once a measure's state is
   Turmeric-visible (struct fields rather than an inline-C handle), at which
   point the annotation is a checked fact and may guard reordering/CSE;
3. **effect-row eventually** -- `#reads`/`#writes` as a real read/write effect
   row (the one `#fx{}` never was -- it "tracks algebraic effects and infers
   nothing from `set!`, a mutable global, or inline C"), subsuming the ECS's
   hand-rolled cap-based conflict detection and feeding the loop-invariant
   bounds work.

Treat today's `#reads` as step 1 of that path, not as a finished `reads`-clause
feature.

## Quick reference

| you have | you want | use |
|---|---|---|
| a pure-Turmeric predicate over struct fields | congruence | nothing -- already congruent |
| an impure (inline-C) predicate over a mutable value | congruence *in a scope where it can't change* | `frozen` + `#reads` |
| to stop a mutation for a scope | a compile error on the mutator | declare the mutator `^unique ^mut`; wrap the scope in `frozen` |
| to elide the *safety* check on a stateful read | -- | not supported, by design; the entry check is the backstop |

## See also

- [Refinement Types guide](refinement-types-guide.md) -- the base feature;
  "a measure must be provably pure" and "a callee's entry check is never elided"
  are the two rules this guide builds on.
- [Substructural Types guide](substructural-types-guide.md) -- `^borrow`,
  `^unique ^mut`, and the `TUR-E0200` exclusive-access rule the `frozen` region
  relies on.
- [Uniqueness Types guide](uniqueness-types-guide.md) -- `^unique` semantics.
- [`refine-stateful-measures-plan.md`](../upcoming/v1/refine-stateful-measures-plan.md)
  -- the design record, including why the capability-token approach was retired
  in favour of `frozen` and why `#reads` is trusted.
- [ECS guide](ecs-guide.md) -- the first consumer; `tur-ecs`'s `ecs/freeze`
  ships the `frozen` region.
