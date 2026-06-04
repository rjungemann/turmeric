---
title: ECS Cross-World Systems Plan
category: Planning
description: Extend `tur-ecs` `defsystem` with per-world `:reads-from` / `:writes-to` annotations so one system can move data between distinct worlds (render extract, client/server reconciliation, save snapshots) while keeping the scheduler's static non-conflict story intact.
---

# `tur-ecs` Cross-World Systems -- Plan

## Status and scope

Follow-up to [docs/upcoming/ecs-spice-plan.md](../ecs-spice-plan.md). That
plan resolved D4 as **single-world v1** and deferred cross-world
systems. This plan picks D4 back up for the post-v1 release. It does
*not* re-litigate D1-D3 -- they are taken as given.

Prerequisite: the v1 spice (`tur-ecs` E0-E4) must be shipped and
exercised by at least one real game (the raylib demo) before this
plan starts. We want concrete pain points from real use, not
speculative ergonomics.

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
`[]`). The existing single-world shorthand:

```turmeric
defsystem physics [w :GameWorld dt :float] :void
  :reads  [Pos Vel]
  :writes [Pos]
  ...
```

remains valid as sugar for `:reads-from w [Pos Vel] :writes-to w [Pos]`.
No existing v1 system needs to change.

### Capability typing

Read and write capabilities are already substructural in v1 (per the
ECS plan's D2 + the substructural types guide). The extension is that
each capability is now keyed by `(World, Component)` instead of just
`Component`:

- `(Read sim Pos)` and `(Read ren Pos)` are distinct capabilities even
  though `Pos` is the same type. Conflict checks compare the full key.
- The elaborator exposes `get-Pos`/`for-each` on `sim` only if `Pos` is
  in `sim`'s read set; exposes `set-Pos!`/`spawn`/`despawn` on `ren`
  only if `Pos` is in `ren`'s write set. Same rule as v1, parameterised
  by world.

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

**X1 -- per-world capability keying (3-4 days).** Extend the
substructural capability tracker so capabilities are keyed by `(world
binding, component)`. The single-world shorthand keeps working
because it desugars to one-world capability keys.

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
Add a side-by-side comparison to Bevy's Extract phase in the
existing `ecs-vs-haskell-ecs.md` guide.

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

- Parent plan: [docs/upcoming/ecs-spice-plan.md](../ecs-spice-plan.md)
  (decision D4)
- Bevy SubApp / Extract phase:
  <https://bevyengine.org/learn/migration-guides/0.10-0.11/#sub-app-labels>
- Unity DOTS Worlds:
  <https://docs.unity3d.com/Packages/com.unity.entities@1.0/manual/concepts-worlds.html>
- `docs/guides/substructural-types-guide.md`
