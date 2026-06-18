# Parallel Tracks -- Open Plans and Reports

Snapshot: 2026-06-19 (post-PR #433; post turmeric-spices PR #15
sized-scheduler direction-1 land).

Index of every non-`v1/`, non-archived plan in `docs/upcoming/` and every
open report in `docs/reported/`, bucketed into tracks that can largely
move on their own clock. Within a track, items are listed in landing
order; across tracks, work is parallelizable except where flagged.

When an item lands, **archive it** (`docs/archive/` for the resolved
report, `docs/archive/history/` for the paper trail) and update this
file -- never leave a resolved item parked here.

---

## Track A -- Compiler ABI (end-to-end monomorphization)

North-star track. Critical path; every other track benefits when it
advances.

**Open work:**

- [end-to-end-monomorphization-plan](upcoming/end-to-end-monomorphization-plan.md)
  -- **rewritten 2026-06-19** as remaining-work-only with actionable
  per-phase checklists. M1-M5 landed (M5 closed by PRs #427/#428,
  verified). Five remaining phases: (1) Track A bucket-C residuals
  (independent of the rest), (2) HKT design pass with measured
  per-(f,A) cost, (3) HKT implementation, (4) carrier-helper rewrites
  (M9 prerequisite per the archived blocker doc), (5) bridge deletion
  + re-audit. Predecessor framing archived at
  `docs/archive/end-to-end-monomorphization-plan.md`; M5 scope audit
  archived at `docs/archive/m5-scope-audit-2026-06-18.md`.
- [m4-typeclass-per-method-abi-plan](upcoming/m4-typeclass-per-method-abi-plan.md)
  -- M4a deliverables landed. Bridge audit floor: **41 crossings / 11
  fixtures**; only bucket C (8 crossings) is tractable and is tracked
  under `option-consumer-retype-byvalue`. Buckets A'/B are by-design
  carrier-bridge regression coverage.
- [option-consumer-retype-byvalue](reported/option-consumer-retype-byvalue.md)
  (report, PARTIAL 2026-06-19) -- `option-eq?`, `option-map`, `some?`,
  and the BoundedIdx half of step 4 (`bidx-of?` / `bidx-unwrap`) all
  retyped to pure-Turmeric by-value `(Option A)`. Remaining:
  - `result-map` -- deferred; its `:int` signature is a deliberate
    carrier-ABI regression test.
  - `unwrap-or` cascade -- broken out into
    [unwrap-or-byvalue-cascade](reported/unwrap-or-byvalue-cascade.md).
  - NonEmpty half of step 4 (`ne-from?`/`ne-unwrap`) -- blocked on
    inference, see [ne-from-byvalue-option-nonempty-element-type-uninferable](reported/ne-from-byvalue-option-nonempty-element-type-uninferable.md).
  - Step 5 (`kleisli.tur` `comp`/`k-apply-raw` retype) -- broken out
    into [kleisli-byvalue-option-cascade](reported/kleisli-byvalue-option-cascade.md).
- [ne-from-byvalue-option-nonempty-element-type-uninferable](reported/ne-from-byvalue-option-nonempty-element-type-uninferable.md)
  (plan, OPEN 2026-06-19) -- the NonEmpty half of step 4 stays on the
  carrier until the inference gap is closed by giving `ne-from?` a
  typed list parameter (`(defopaque List [A] :int)` + `list-of`
  smart constructor), so `A` is recovered from the argument rather
  than ascribed at the call site. Caller-ascription `(:: o (Option int))`
  workaround is explicitly out -- it would propagate carrier-`:int`
  into every NonEmpty consumer and is the kind of "tighten the types
  later" patch CLAUDE.md forbids.
- [unwrap-or-byvalue-cascade](reported/unwrap-or-byvalue-cascade.md)
  (plan, OPEN 2026-06-19) -- retype `unwrap-or` to by-value
  `[A] [o : (Option A) dflt : A] : A` and migrate the ~10 stdlib
  producers (`zipper`, `seq/*`, `json`, `safe`, `env`, `serial`, ...)
  one PR per row, gated on a temporary `unwrap-or-carrier` shim so the
  suite stays green at each step. No caller-ascription bridges.
- [kleisli-byvalue-option-cascade](reported/kleisli-byvalue-option-cascade.md)
  (plan, OPEN 2026-06-19) -- retype `k-apply-raw` / `k-apply` /
  `Category [Kleisli]` to thread `(Option B)` by value; retires the
  `(:: r (Option int))` ascription that landed in PR #426 as a
  temporary patch. Independent of `unwrap-or`; one self-contained PR.

## Track B -- ECS spice (sized worlds + scheduler)

**Direction-1 unblocker landed** -- turmeric-spices PR #15
(`sized-defsystem-scheduled`) takes a heap-pointer world and single-world
sized scheduling, closing the prior Track B blocker. Slice 8 ("wire sized
worlds through the parallel scheduler") can now proceed.

**Compiler-side prereqs all cleared:** PR #407 (inline struct fields),
PR #412 (typeclass-dispatch identity), PR #420 (existential pack/open via
heap-boxing). Direction 2 (cross-world / heterogeneous scheduling) stays
blocked on **gap-H world-type polymorphism**, which depends on Track A's
monomorphization phases retiring the carrier bridge.

**Spice-side already landed** (turmeric-spices, verified against git log):

- E2d wiring (PRs #6-#8): associated-type storage projection (P1-P5),
  variadic `defworld` collapse (P5b), `StorageOps` typeclass (P6).
- E2c sized worlds (slices 1-12): `SizedDense n A` /
  `SizedSparse` / `SizedTag` shapes, `sized-defworld`, `sized-spawn`,
  generational `Entity` handles, `sized-for-each` payoff macro, fallible
  `sized-spawn -> (Result int WorldFull)`.
- PR #15: sized-scheduler direction 1.
- PR #17: `sized-defworld` `world-resize` existential wrapper.

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

**Archived since last snapshot:**

- `ecs-sized-world-plan` -- Q1-Q4 settled, surface specced, and every
  spice-side slice (1-12) plus PR #15 (direction-1 scheduler) and
  PR #17 (world-resize wrapper) have shipped. Moved to
  [docs/archive/ecs-sized-world-plan.md](archive/ecs-sized-world-plan.md).

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

All three tracks are independently progressable.

- **Track A** is the critical path; M5/M6/M7 unlocks Track B direction 2
  (cross-world scheduling) and reduces Track C audit pressure.
- **Track B** direction 1 is unblocked and has spice-side traction
  (PR #15); direction 2 stays gated on Track A gap-H.
- **Track C** is fully independent. The prior soft coupling to Track A
  bridge follow-ups is gone (PR #415/#416 -> spices PR #12).
