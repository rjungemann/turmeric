---
title: ECS Cross-World Systems Plan
category: Planning
description: Extend `tur-ecs` `defsystem` with per-world `:reads-from` / `:writes-to` annotations so one system can move data between distinct worlds (render extract, client/server reconciliation, save snapshots) while keeping the scheduler's static non-conflict story intact.
---

# `tur-ecs` Cross-World Systems -- Plan

## Status and scope

Follow-up to [`docs/upcoming/ecs-spice-plan.md`](../ecs-spice-plan.md).
That plan resolved D4 as **single-world v1** and deferred cross-world
systems. This plan picks D4 back up for the post-v1 release. It does
*not* re-litigate D1-D3 -- they are taken as given.

> **Status 2026-06-11.** v1 prereqs E0-E3 shipped, including the
> Phase I cap-gated `defsystem` surface
> ([`docs/archive/history/ecs-defsystem-write-caps-not-enforced.md`](../../archive/history/ecs-defsystem-write-caps-not-enforced.md)).
> E4 (the apecs/aztecs comparison writeup) is partial. The raylib
> demo runs at `../turmeric-spices/spices/ecs-raylib/`. The "wait
> for a real game's pain points before designing" gating condition
> is therefore *partially* satisfied -- the raylib demo exists but
> hasn't been pushed against render-extract patterns yet. Treat
> this plan as ready-to-design once X0 has at least one real
> cross-world use case to anchor the surface to.

## Goal

Let a single `defsystem` body atomically read from one world and
write to another, with the scheduler statically proving non-conflict
across world boundaries:

```turmeric
defsystem extract-renderables
  [sim :SimWorld ren :RenderWorld]
  :reads-from  sim [Pos Sprite]
  :writes-to   ren [RenderPos RenderSprite]
  :reads-from  ren []
  :writes-to   sim []
  for-each [(p :Pos) (s :Sprite)] sim
    fn [e p s]
      ren-spawn-or-update! ren e
        RenderPos    p.x p.y
        RenderSprite s.tex
```

Two world type parameters, two paired read/write sets, one body. The
scheduler treats each (world, component) pair as an independent lock
target.

## Motivating use cases

These came out of v1 user feedback (or, before v1 ships, are the
patterns we expect to see and want to be ready for):

1. **Render extraction.** Game loop wants to overlap "simulate frame
   N+1" with "render frame N." A separate render world holds only
   the components the renderer cares about, populated each frame from
   the sim world by a single extract system.
2. **Client-side prediction.** A client holds a `PredictedWorld` it
   integrates locally and an `AuthoritativeWorld` it overwrites from
   server snapshots. Reconciliation systems read from auth, write to
   predicted, and replay user inputs.
3. **Save snapshots / rewind.** Periodic copy from `LiveWorld` into a
   `SnapshotWorld`; on rewind, copy back. Save/load is the same
   primitive against a serializable world.
4. **Editor vs play mode.** A scene editor wants to mutate an
   `EditorWorld` while a paused `PlayWorld` holds the last simulation
   state.

The shared pattern: **bulk component copy / projection between two
typed worlds, with a transformation in the middle**.

## Design

### Surface syntax

`defsystem` gains repeatable per-world `:reads-from W [...]` and
`:writes-to W [...]` clauses. Each named world parameter must appear in
exactly one read set and exactly one write set (either may be empty
`[]`). The existing single-world shorthand shipped in I3:

```turmeric
(defsystem physics
  [Pos Vel]                 ;; :reads (over the implicit world `w`)
  [Pos]                     ;; :writes
  body)
```

continues to lower to `:reads-from w [Pos Vel] :writes-to w [Pos]`
where `w` is the body-bound int handle the single-world `defsystem`
already exposes. No existing v1 system needs to change. The
multi-world form takes named world bindings instead of the implicit
`w`:

```turmeric
(defsystem extract-renderables
  [sim ren]                              ;; world bindings
  :reads-from  sim [Pos Sprite]
  :writes-to   ren [RenderPos RenderSprite]
  :reads-from  ren []
  :writes-to   sim []
  body)
```

### Capability typing

The v1 cap surface that shipped via Phase I (`ecs/cap`) is keyed only
on the component: `WriteCap<T>` and `ReadCap<T>` (parametric `:linear`
opaques). For cross-world, capabilities lift to `(World, Component)`
keys. The most direct encoding is a second type parameter on the
existing opaques: `WriteCap<W, T>` and `ReadCap<W, T>`, where `W` is
the world type and `T` the component. Conflict checks then compare
the full `(W, T)` pair.

Concretely:

- The cap-mint helpers shipped in I2 (`mint-<World>-<Comp>-write-cap`,
  `mint-<World>-<Comp>-read-cap`) already key on the (World, Comp)
  pair at the binding-name level; the type-level lift to
  `WriteCap<W, T>` makes this an actual type-system distinction
  rather than a naming convention.
- `defsystem` binds `<world>-<Comp>-write-cap : WriteCap<<World>, Comp>`
  in body scope for each entry in `:writes-to <world> [...]`,
  mirroring the I3 binding scheme but namespaced by the world
  binding.
- `defcomponent-accessors` keeps its `set-<Comp>!` shape; the cap
  parameter's type carries the world, so a body holding only a
  `WriteCap<RenderWorld, Pos>` cannot pass it to `set-Pos!` against
  a `SimWorld` handle. The nominal check already in the type system
  catches the mismatch.
- `(Read sim Pos)` and `(Read ren Pos)` are distinct capabilities
  even though `Pos` is the same type; the parametric-linear
  propagation fix that landed alongside Phase I
  ([`docs/archive/history/parametric-linear-opaque-not-enforced.md`](../../archive/history/parametric-linear-opaque-not-enforced.md))
  is the compiler-side piece that makes the two-parameter version
  actually fire its linearity check.

### Static non-conflict

Two systems `S` and `T` may run in parallel iff, **for every world
they both mention**, their (read, write) sets are non-conflicting in
the v1 sense (no write/write, no read/write). Worlds they do not share
are trivially compatible.

This is just the v1 rule lifted point-wise over the set of worlds.
The scheduler implementation already builds a conflict graph per
component; the change is that the graph's nodes are
`(World, Component)` pairs.

### Atomicity

A cross-world system body runs *as one step*. The reader world is
locked for the duration of the body; the writer world is locked for
the duration of the body; nothing else touching either world's
declared sets can run concurrently. We do not offer any "release the
reader lock partway through" primitive -- if the body needs that,
split into two systems.

### What we explicitly do not add

- **Cycles between worlds in one stage.** If `S` writes `ren.Pos` and
  `T` writes `sim.Pos` while reading `ren.Pos`, the stage is
  ill-formed. Insert an explicit barrier (`stage-sequence` instead of
  `stage`) between them. The scheduler rejects cyclic stages at
  elaboration time -- this is a static error, not a deadlock at
  runtime.
- **Entity identity across worlds.** An `Entity` value from `sim` is
  *not* an `Entity` value in `ren`. They are different opaque types
  parameterised by the world. To correlate entities across worlds,
  components store the other-world entity explicitly (e.g.
  `RenderLink { sim-id :SimEntity }`). This is the same discipline
  Bevy enforces via `MainEntity` markers.
- **Component-type renaming.** A component `Pos` registered in both
  worlds is the same type with the same layout. We do not invent a
  `sim.Pos` vs `ren.Pos` distinction. If you want different layouts,
  declare different components (`SimPos`, `RenderPos`).
- **More than two worlds in one system.** Permitted by the grammar but
  flagged by a lint -- in practice, three-way couplings should be
  factored into pairwise systems.

## Comparison to precedents

| | Bevy SubApp | Unity DOTS | tur-ecs cross-world (this plan) |
|---|---|---|---|
| Worlds in one system | No -- subapps run in sequence, talk via Extract phase | No -- systems registered per-World; `EntityCommandBuffer` for cross-world | Yes -- one system, two worlds, declared read/write per world |
| Non-conflict guarantee | Per-world dynamic; subapps serialised | Per-world dynamic; ECB applied at sync points | Static, per (world, component) |
| Entity identity | `MainEntity` component holds the source-world id | Explicit translation tables | `RenderLink { sim-id :SimEntity }` user-level pattern; type system keeps the worlds apart |
| Cycles | Allowed (subapps run in order) | Allowed (sync points serialise) | Rejected at elaboration |

The novelty is the static cross-world non-conflict check. Bevy gets
parallelism by running the SubApp later in the frame; we get
parallelism by proving the systems can't conflict.

## Phasing

**X0 -- design lock (1 day).** Confirm the syntax against two real
v1 users -- the raylib demo and one of: render-extract for a scene
graph, or a client-prediction prototype. Adjust before writing any
elaborator code.

**X1 -- per-world capability keying (3-4 days).** Lift `ecs/cap`'s
`WriteCap<T>` / `ReadCap<T>` to two-parameter `WriteCap<W, T>` /
`ReadCap<W, T>`. Extend `defcomponent-class-instance` to emit
`mint-<World>-<Comp>-{write,read}-cap` returning the world-keyed
form (the per-(World, Comp) naming already shipped in I2 -- this is
the type-level lift). The single-world shorthand keeps working
because it desugars to one-world capability keys; the I1-I4
single-world tests carry through unchanged once `WriteCap<W, T>`
collapses to `WriteCap<DefaultWorld, T>` for the implicit-`w` case.

**X2 -- defsystem grammar (1-2 days).** Parser support for repeated
`:reads-from`/`:writes-to`. Validation that every world parameter
appears in exactly one read and one write clause. Helpful errors when
that fails.

**X3 -- scheduler extension (2-3 days).** Conflict graph nodes become
`(World, Component)` pairs. Cycle detection in cross-world stages.
Sequential fallback (`stage-sequence`) for systems the user wants
ordered.

**X4 -- world-mirror primitive (2 days).** Sugar for "copy these
components verbatim between two worlds with matching entity ids":

```turmeric
mirror-stage sim -> ren
  :copy [Pos Sprite]
  :map  [(Hp -> RenderHp via fn [h] {h.cur / h.max})]
```

Lowers to a `defsystem` per copy/map clause. Covers the
render-extract case in one line without losing the explicit form for
harder transforms.

**X5 -- demo + guide (2 days).** Extend the v1 raylib demo with a
separate render world; ship `docs/guides/ecs-cross-world-guide.md`.

Also update
[`docs/guides/ecs-vs-haskell-ecs.md`](../../guides/ecs-vs-haskell-ecs.md)
to reflect what shipped:

- Flip the "Cross-world systems" row in the bottom-line property
  table -- currently a "Planned" link to this plan with "single-world
  is v1" qualifier -- to "Shipped -- statically non-conflicting
  per-(World, Component) cap keying; see `ecs-cross-world-guide.md`."
- Add a side-by-side comparison to Bevy's Extract phase and Unity
  DOTS Worlds (matching the precedent table in this plan) -- new
  section under "What moved to compile time" or its own top-level
  section, depending on length.
- Update the "Honest scorecard" section to include cross-world
  static non-conflict as a row where tur-ecs is unambiguously ahead
  of apecs and aztecs (which keep cross-world out of scope).
- Update the spice-plan status callout (now describing the
  cross-world plan as ready-to-design) and the spice CHANGELOG with
  the X1-X4 user-facing changes.

## Validation

Fixture tests under `tests/fixtures/ecs-cross-world-*/`:

- two-world spawn / extract / verify,
- conflict cases: write-write on `(ren, RenderPos)` from two
  systems must be a compile-time error,
- non-conflict cases: write `(sim, Pos)` + write `(ren, Pos)` from
  two systems must be allowed (different worlds),
- cycle case: `S` writes `ren.Pos` reading `sim.Pos`, `T` writes
  `sim.Pos` reading `ren.Pos`, in the same stage -- must be a
  compile-time error pointing at both systems,
- mirror-stage syntax produces the same observable result as the
  hand-written extract system on a 10k-entity fixture.

Bench: render-extract over 100k entities. Target: cross-world extract
should be within 1.5x of an equivalent `memcpy`-style bulk copy
written by hand.

## Open questions for X0 design lock

- **Should `:reads-from`/`:writes-to` allow `:all` as a shorthand?**
  `:writes-to ren :all` would mean "every component registered on
  `ren`." Convenient for whole-world mirroring; encourages over-broad
  capability claims. Lean: no. Force explicit lists.
- **Should we offer a `:read-only` world parameter modifier?**
  `defsystem extract [sim :SimWorld :read-only ren :RenderWorld]`
  could replace the empty `:reads-from`/`:writes-to` clauses. Lean:
  no -- a uniform syntax is easier to teach than a special modifier.
- **Cross-world `with`/`without` filters.** A query on `sim` filtered
  by "entity has a `RenderLink` pointing to a `ren` entity that has
  `Dead`" crosses the boundary. v1 query filters are single-world; do
  we extend them, or force the user to join in user code? Lean:
  force user code in this plan, revisit if pattern is common.
- **Mirror-stage and tag components.** A `:copy [Player]` clause for
  a tag is just bit-copying presence. Probably trivial; flag here so
  we don't miss it in X4.

## References

- Parent plan: [`docs/upcoming/ecs-spice-plan.md`](../ecs-spice-plan.md)
  (decision D4)
- Phase I (cap-gated single-world `defsystem`) -- shipped:
  [`docs/archive/history/ecs-defsystem-write-caps-not-enforced.md`](../../archive/history/ecs-defsystem-write-caps-not-enforced.md)
- Parametric `:linear` propagation fix (compiler enabler for the
  proposed `WriteCap<W, T>` shape):
  [`docs/archive/history/parametric-linear-opaque-not-enforced.md`](../../archive/history/parametric-linear-opaque-not-enforced.md)
- Bevy SubApp / Extract phase:
  <https://bevyengine.org/learn/migration-guides/0.10-0.11/#sub-app-labels>
- Unity DOTS Worlds:
  <https://docs.unity3d.com/Packages/com.unity.entities@1.0/manual/concepts-worlds.html>
- `docs/guides/substructural-types-guide.md`
