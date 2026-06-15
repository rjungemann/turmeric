# ECS sized-world API plan

Design plan that gates ECS E2c (sized-rectangular dense iteration).
See [`docs/reported/ecs-e2c-sized-dense-needs-bounded-world.md`](../reported/ecs-e2c-sized-dense-needs-bounded-world.md)
for why a transparent spice-side update is not sufficient and what
compiler prereqs already shipped (P1-P4 resolved 2026-06-12).

Status: DRAFT. Q1-Q4 settled; surface specced (step 4); ready for spice-side implementation.

## Goal

Introduce a bounded-capacity world type `(World n)` so the
`for-each` macro can statically unify the capacity index across
multiple component storages, replacing the runtime `__fe-min-cap`
check with a compile-time TUR-E0260 on mismatch. Dense, sparse, and
tag storages are all parameterised on the same `n`.

The win this plan is buying:

```turmeric
(defworld GameWorld (Static 1024) [Pos Vel])
;; pos : (Dense (Static 1024) Pos), vel : (Dense (Static 1024) Vel)

(for-each [p (world.pos w) v (world.vel w)]
  ...)   ;; statically rectangular -- no runtime min-cap probe
```

A `(for-each [... w1.pos ... w2.vel ...])` mixing two differently-sized
worlds fails at compile time with TUR-E0260, not at runtime.

## Q1 -- Cap lifecycle: SETTLED (fixed at construction)

**Decision.** `n` is committed when the world is constructed and
cannot grow. Resizing requires constructing a new world and
copying through the existential pack/open pair already shipped in
[`stdlib/sized-handle-existential.tur`](../../stdlib/sized-handle-existential.tur).

**Why.**

- Growing `n` would make it runtime-flavored: every accessor and
  every for-each call would need to re-prove the cap is still the
  one it was elaborated against. That defeats the cross-parameter
  unification win SZ6-SZ8 set up.
- The mutating `realloc`-grow story is incompatible with cross-param
  unification on phantom indices (the entire reason this plan exists
  -- see the originating report).
- The pack/open existential lift (P4) already gives callers a
  type-safe path to "open at runtime size, prove it stays fixed for
  the body". A growable world is not needed for the load-bearing
  cases.

**Implications to wire through downstream questions.**

- Storage layout commits at world creation. Dense buffers are
  `calloc`'d to `n` slots once; no `realloc` in the sized path.
- `spawn` cannot increase `n`. If the entity count would exceed
  `n`, spawn must signal failure (-> Q3).
- "Grow the world" is a user-level operation: open the existential,
  allocate a new `(World n')` with `n' >= n`, copy components, close.
  The plan ships a `world-resize` helper that wraps this once Q2 is
  settled.

**Non-goals.**

- Dynamic resize hidden behind the `(World n)` type. Explicitly
  out of scope; users who need this keep the unsized world.
- Heterogeneous per-storage caps. All storages in a world share `n`.

## Q2 -- Sparse and tag storage: SETTLED (uniform `n` across shapes)

**Decision.** `n` is the world's capacity -- the maximum number of
simultaneously-live entity ids -- and the same `n` indexes every
storage shape in that world:

- `Dense<n, T>`   -- `n` slots, slot-indexed. Storage cost = O(n).
- `Sparse<n, T>` -- hashmap with key domain `[0, n)`. Storage cost
  is data-proportional; `n` bounds the id range, not the memory.
- `Tag<n>`        -- n-bit bitset. Storage cost = O(n) bits.

The world's capacity and each storage's size index are the *same
value*: a `(World (Static 1024))` contains storages typed against
`(Static 1024)`, full stop. This is what makes for-each's
cross-parameter unification fire across mixed-representation
iterations (`Dense<n,T>` zipped with `Sparse<n,U>` zipped with
`Tag<n>`).

**Documentation requirement.** The `Sparse<n,T>` type-noise
("`n` doesn't bound my memory") is real and will surprise users.
The spice docs must call this out explicitly: `n` is an
**id-space bound**, not a storage-cost bound. Splitting into a
separate `(Sparse dom T)` parameter was considered and rejected --
it would break the load-bearing property that one `n` unifies
across every storage shape in for-each.

**Implications.**

- `defworld` declares `n` once; every component storage inherits
  it. No per-storage capacity in the sized path.
- Tag-vs-Dense rectangularity falls out of the uniform `n`; the
  for-each loop body sees the tag's present-bit alongside the
  dense slot at the same index. Confirm with a fixture once the
  surface is specced.
- Slot-reuse semantics (despawn frees an id for reuse) are
  invariant-preserving: the id domain stays `[0, n)`, only the
  live subset changes. This makes Q3's spawn invariant
  tractable (-> Q3).

## Q3 -- Fallible `spawn`: SETTLED

**Decision.** `spawn : (World n) -> result<EntityId, WorldFull>`.

- Matches turmeric's "no `:int` status codes" rule and the existing
  result idiom across the stdlib.
- Ship a `spawn! : (World n) -> EntityId` panicking variant for
  benchmark/demo paths so the ergonomic hit is opt-in, not forced
  on every caller.

**Despawn / slot reuse.** Despawn frees an id for reuse; the id
domain stays `[0, n)` and only the live subset shrinks. Spawn
succeeds whenever `live-count < n`. The invariant is "live-count
never exceeds `n`", which is preserved by both spawn (-> result)
and despawn (-> live-count decreases).

**Out of scope for E2c.** A `(with-capacity! world k body)` macro
that lets users prove `k` spawns fit before entering the body
would carry a `Fin n` remaining-capacity witness. That path is
refinement-types-gated and overlaps E2b; revisit when refinements
land.

## Q4 -- Accessor cap-gating: SETTLED (runtime bounds checks stay)

**Decision.** Hand-written `dense-get`/`dense-set!` keep their
existing runtime bounds checks in E2c. The for-each macro generates
unchecked accessors because the macro walks `0..n` by construction
and the index is provably in-bounds without a runtime probe.

**The asymmetry, named honestly.**

- `for-each` over sized storages: statically rectangular,
  bounds-check elided in codegen.
- `(dense-get s i)` with `i : int`: runtime bounds-check stays.

This reflects what the type system actually proves today. The
static-index path (`dense-get : (Dense n A) -> (Fin n) -> A`)
needs `Fin n`, which is refinement-types-gated and lines up with
E2b/E2d.

**Why not block on `Fin n`.** The for-each rectangularity win is
the load-bearing payoff of this plan and does not require accessor
type changes. Conflating "static for-each rectangularity" with
"static index bounds" would defer a shippable win behind a much
larger refinement-types story. Ship E2c with the asymmetry;
revisit accessors when refinements land.

**Out of scope variants considered and rejected:**

- A `dense-get-checked!` panic-on-OOB variant uniform with for-each
  codegen. Slight ergonomic win, no static guarantee; not worth a
  third accessor name.
- Eagerly threading `Fin n` placeholder types through accessors
  in anticipation of refinements. Premature; the surface would
  churn when refinements actually land.

## Critical path

1. ~~Settle Q2 (storage-shape parameterisation).~~ Done.
2. ~~Settle Q3 (spawn signature).~~ Done.
3. ~~Settle Q4 (accessor checking story).~~ Done.
4. ~~Spec the `(defworld <name> <n> [<components>...])` surface and
   the macros' lowering.~~ Done (see "Surface" section).
5. Spice-side wiring (the E2c work item proper).

Spice-side prereqs already in the bag (per the reported doc):
P1, P2, P3, P4 all green. The two follow-up gaps
([`sz8-projection-size-recovery-gap`](../archive/sz8-projection-size-recovery-gap.md),
[`make-struct-phantom-typeparam-lowering`](../reported/make-struct-phantom-typeparam-lowering.md))
live in the spice-side phase and do not block this design plan.

## Surface: `defworld` with explicit capacity

The unsized form continues to work unchanged. The sized form adds
a leading capacity slot before the component vector:

```turmeric
;; Unsized (today's surface, untouched):
(defworld GameWorld [Pos Vel])

;; Sized, monomorphic capacity baked in at declaration:
(defworld GameWorld (Static 1024) [Pos Vel])

;; Sized, polymorphic over n -- callers pick the capacity:
(defworld [n] GameWorld n [Pos Vel])
```

The polymorphic form is the load-bearing one for libraries that
ship reusable world shapes; the monomorphic form is the
ergonomic-default for application code with a fixed budget.

### Lowering -- monomorphic capacity

```turmeric
(defworld GameWorld (Static 1024) [Pos Vel])
```

lowers to:

```turmeric
(defstruct GameWorld
  [pos  : (Dense (Static 1024) Pos)
   vel  : (Dense (Static 1024) Vel)
   gens : (Dense (Static 1024) Gen)
   live : int])   ;; current live-count, runtime
```

`live` is the only mutable scalar; the storages' capacity is
encoded in the type and never changes.

### Lowering -- polymorphic capacity

```turmeric
(defworld [n] GameWorld n [Pos Vel])
```

lowers to a parameterised defstruct:

```turmeric
(defstruct GameWorld [n]
  [pos  : (Dense n Pos)
   vel  : (Dense n Vel)
   gens : (Dense n Gen)
   live : int])
```

Callers materialise a concrete world by ascribing `n`:

```turmeric
(let [w (make-game-world (Static 256))]
  ...)   ;; w : (GameWorld (Static 256))
```

`make-game-world` is the generated constructor; it `calloc`s each
storage to `n` slots and zero-initialises `live`. Because all
storages share `n` structurally, the SZ8 cross-parameter unifier
proves rectangularity across them without a runtime probe (the
load-bearing win this whole plan is buying).

### `spawn` / `despawn`

```turmeric
(defn spawn  [n] [w : (GameWorld n)] : (result int WorldFull) ...)
(defn spawn! [n] [w : (GameWorld n)] : int ...)
(defn despawn [n] [w : (GameWorld n) e : int] : nil ...)
```

`spawn` consults `w.live` against the type-level `n`; on success
it picks a free slot (preferring reused ids from a free-list),
bumps the generation, and returns `(ok eid)`. On full, returns
`(err world-full)`. The type-level `n` only participates in
rectangularity proofs; the `live < n` check is runtime, which is
fine -- the load-bearing static guarantee is across-storage
unification, not per-spawn capacity.

`despawn` decrements `live` and pushes the slot onto the
free-list. Generation bump on next reuse.

### `for-each` -- the payoff

```turmeric
(for-each [p (.pos w) v (.vel w)]
  (update-position! p v))
```

Macro-expands to a loop indexed `0..n` where the bound comes from
the world's type, not a `__fe-min-cap` runtime min. The
cross-parameter unifier rejects a mixed-`n` invocation at compile
time with TUR-E0260:

```turmeric
(for-each [p (.pos w-256) v (.vel w-512)]
  ...)   ;; TUR-E0260: (Static 256) /= (Static 512)
```

Inside the loop body, `dense-get`/`dense-set!` codegen elides
bounds checks because the macro knows `i ∈ [0, n)` structurally.
Hand-written accessor calls (-> Q4) keep their runtime check.

### `defcomponent-accessors` -- unchanged surface, sized lowering

```turmeric
(defcomponent-accessors GameWorld Pos)
```

continues to emit `get-pos` / `set-pos!` / `has-pos?` over the
world's `Pos` storage. The only change is the underlying calls
now go through the sized `(Dense n Pos)` accessor surface
instead of the int-typed one.

### Resize (out-of-band, via existential)

There is no in-place world resize. The `world-resize` helper
opens the world as an existential, allocates a fresh `(World n')`
with the new capacity, copies components slot-by-slot, and
closes:

```turmeric
(defn world-resize [n n'] [w : (GameWorld n) n' : (Static-size n')]
                          : (GameWorld n') ...)
```

Built on `pack-sized` / `open-sized` from
[`stdlib/sized-handle-existential.tur`](../../stdlib/sized-handle-existential.tur).

## Open implementation questions (post-design)

These are *implementation* gaps to surface during the spice-side
phase, not design-blockers:

- **`Sparse<n,T>` lowering.** The current sparse storage is a
  hashmap with int keys; threading `n` requires either a
  defopaque wrapper (`(Sparse n T)` over the existing int handle)
  or genuine type-level use of `n` in the storage. Wrapper is
  almost certainly enough.
- **`Tag<n>` lowering.** Existing tag bitset already knows its
  size at construction; lifting to `(Tag n)` is a defopaque-and-
  ascribe pass.
- **Free-list representation.** Slot reuse needs a free-list
  (sized vector of int, or a packed run of free ids). Either
  works; sized-vector matches the rest of the world's storages.
- **`make-world` factoring.** With a polymorphic world the
  generated constructor is parameterised over `n`; defstruct
  factories already support this, but confirm a fixture before
  the spice-side phase commits.

## Related

- [`docs/reported/ecs-e2c-sized-dense-needs-bounded-world.md`](../reported/ecs-e2c-sized-dense-needs-bounded-world.md)
- [`docs/upcoming/ecs-spice-plan.md`](ecs-spice-plan.md) (E2c entry)
- [`stdlib/sized-handle-existential.tur`](../../stdlib/sized-handle-existential.tur)
  (`pack-sized`/`open-sized` -- the existential lift this plan relies on for resize)
