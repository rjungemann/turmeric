# Parallel Tracks -- Open Plans and Reports

Snapshot: 2026-06-19 (post-PR #433; post turmeric-spices PR #15
sized-scheduler direction-1 land; post M7 layer-4 emit land +
Functor-`fmap` element-type generality). **Track A is complete:
end-to-end monomorphization landed; the small residual ABI bridge is
intentional and necessary, with no further work to be done.** All Track A
reports are resolved and archived (see Track A below).

Index of every non-`v1/`, non-archived plan in `docs/upcoming/` and every
open report in `docs/reported/`, bucketed into tracks that can largely
move on their own clock. Within a track, items are listed in landing
order; across tracks, work is parallelizable except where flagged.

When an item lands, **archive it** (`docs/archive/` for the resolved
report, `docs/archive/history/` for the paper trail) and update this
file -- never leave a resolved item parked here.

---

## Track A -- Compiler ABI (end-to-end monomorphization) -- COMPLETE

North-star track. **Complete as of 2026-06-19: end-to-end monomorphization
landed.** Values thread by value end-to-end. A small ABI bridge remains, but it
is **intentional and necessary** -- not a defect to retire -- and **there is no
further work to be done on it.** Every other track that was gated on this
critical path is now unblocked.

**Open work:** none. All Track A reports are resolved and archived.

- [end-to-end-monomorphization-plan](upcoming/end-to-end-monomorphization-plan.md)
  -- the driving plan. M1-M5 landed (M5 closed by PRs #427/#428); HKT design
  + implementation landed (M7 elaborator PR #435, layer-4 by-value HKT emit
  PR #436) with Functor `fmap` and Monad `bind` exiting end-to-end. The
  remaining-bridge-deletion question is settled: the small ABI bridge is
  kept by design. Predecessor framing archived at
  `docs/archive/end-to-end-monomorphization-plan.md`; M5 scope audit at
  `docs/archive/m5-scope-audit-2026-06-18.md`.

**Resolved + archived 2026-06-19** (monomorphization complete; residual ABI
bridge intentional and necessary, so the by-value retype/carrier-removal work
these tracked is closed):

- [option-consumer-retype-byvalue](archive/option-consumer-retype-byvalue.md)
  -- RESOLVED. The landed retypes (`option-eq?`, `option-map`, `some?`,
  `result-map`, BoundedIdx + NonEmpty halves) stand; the remaining `unwrap-or`
  and kleisli tails are closed with the bridge accepted.
- [unwrap-or-byvalue-cascade](archive/unwrap-or-byvalue-cascade.md)
  -- RESOLVED. STEP 1 + zipper/`env` producer migrations stand; the remaining
  kleisli `unwrap-or-carrier` caller no longer represents outstanding work.
- [kleisli-byvalue-option-cascade](archive/kleisli-byvalue-option-cascade.md)
  -- RESOLVED. The Kleisli arrow staying on the carrier behind the intentional
  bridge is accepted; the step-5 retype is closed.
- [kleisli-k-apply-raw-B-uninferable](archive/kleisli-k-apply-raw-B-uninferable.md)
  -- RESOLVED. Was the prerequisite-1 inference gate for the step-5 retype;
  closed with it.
- [carrier-option-producers-gated-on-handle-typing](archive/carrier-option-producers-gated-on-handle-typing.md)
  -- RESOLVED. The carrier-`Option` producers still routed through the bridge
  (seq/json/safe/serial) are accepted; no miscompile, no further migration.
- [m7-hkt-traverse-method-level-hkt-tyvar](archive/m7-hkt-traverse-method-level-hkt-tyvar.md)
  -- RESOLVED. The Traversable `traverse` HKT shape is covered by the completed
  monomorphization work.
- [polymorphic-float-carrier-ascription-value-cast](archive/polymorphic-float-carrier-ascription-value-cast.md)
  -- RESOLVED. Float elements thread by value end-to-end, so the `(:: x :A)`
  carrier round-trip truncation/value-cast is no longer reachable on the
  monomorphized path.

Two M7 reports were resolved/archived earlier on 2026-06-19:
[m7-hkt-fn-returning-applied-type-kind-mismatch](archive/m7-hkt-fn-returning-applied-type-kind-mismatch.md)
(kind-threading) and
[m7-hkt-bind-body-byvalue-emit](archive/m7-hkt-bind-body-byvalue-emit.md)
(gate + class-var-head binding). Probes:
`docs/upcoming/v2/m7-hkt-probe{,-bind}.tur`.

## Track B -- ECS spice (sized worlds + scheduler)

**Open work:**

- [ecs-spice-plan](upcoming/ecs-spice-plan.md) -- top status block
  current as of 2026-06-18. Two residual follow-ups, neither
  design-blocking:
  1. Routing `defcomponent-accessors` through `StorageOps` -- waiting
     on struct-element projection (independent of PR #420).
  2. Sized-scheduler direction 2 (cross-world / heterogeneous
     scheduling) -- gated on **gap-H world-type polymorphism**,
     itself behind Track A monomorphization. Direction 1 shipped
     (PR #15); the `world-resize` existential wrapper shipped (PR #17).
  E2b (refinement-typed APIs) stays gated on refinement types.

**Order:** the remaining `StorageOps` routing is implementable when
struct-element projection lands; direction-2 cross-world scheduling
waits on Track A's gap-H.

## Track C -- Spices uplift (type hygiene)

Pure spice-side work; no compiler dependencies.

**Open work:**

- [spices-type-features-uplift-plan](upcoming/spices-type-features-uplift-plan.md)
  -- phased per-spice uplift (rows, typeclasses, sized types where they
  pay rent). Independent across spices. **U2 target 1 (ansi Color
  typeclass collapse) landed 2026-06-19** as turmeric-spices PR #18;
  remaining U2 targets are `plot` (Renderer class), `json`
  (Encode/Decode -- minimal slice already shipped under P2a, full
  typeclass collapse pending), and `http`/`httpd` (Handler class).
  U1/U3/U4/U5/U6 still open.

**Recently landed in turmeric-spices** (since last snapshot):

- PR #12 (tourist v0.2.2): `param`/`capture` retyped to `ctx : Ctx`.
- PR #13 (tourist v0.2.5): `defopaque Pattern` for
  `router-compile`/`router-match`/`router-free`.
- PR #16: remaining tourist `ctx : int` -> `ctx : Ctx` retype.
- PR #18 (ansi v0.2.0): `Color` typeclass collapse -- **U2 target 1**.

**Archived since last snapshot:**

- `spices-int-stand-in-audit-2026-06-14` -- all S1/S2/S3 surfaces and
  the four follow-up rows (plutovg secondary handles, raylib
  models/text/textures/camera/shapes, rtaudio `DeviceInfo` + callback,
  tourist internals via v0.2.6 / commit 9a590cb) are closed. Moved to
  [docs/archive/spices-int-stand-in-audit-2026-06-14.md](archive/spices-int-stand-in-audit-2026-06-14.md).

---

## Unassigned / cross-cutting reports

None open as of this snapshot.

---

## Concurrency summary

- **Track A** is **complete** (end-to-end monomorphization landed; the small
  residual ABI bridge is intentional and necessary). Its completion unblocks
  Track B direction 2 (cross-world scheduling, via gap-H) and removes the
  remaining Track C audit pressure.
- **Track B** direction 1 is unblocked and has spice-side traction
  (PR #15); direction 2's Track A dependency (gap-H world-type polymorphism)
  is now satisfied -- the remaining gating is the gap-H implementation itself.
- **Track C** is fully independent. The prior soft coupling to Track A
  bridge follow-ups is gone (PR #415/#416 -> spices PR #12).
