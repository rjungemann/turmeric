# Parallel Tracks -- Open Plans and Reports

Snapshot: 2026-06-19 (post-PR #433; post turmeric-spices PR #15
sized-scheduler direction-1 land; post M7 layer-4 emit land +
Functor-`fmap` element-type generality). Track A bug list refreshed
2026-06-18: `ne-from?` inference gap closed (PR #434) and
`unwrap-or` cascade status corrected to STEP 1 + producers LANDED.

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
- [option-consumer-retype-byvalue](reported/option-consumer-retype-byvalue.md)
  (report, PARTIAL 2026-06-18) -- `option-eq?`, `option-map`, `some?`,
  and the BoundedIdx half of step 4 (`bidx-of?` / `bidx-unwrap`) all
  retyped to pure-Turmeric by-value `(Option A)`. Remaining:
  - `result-map` -- deferred; its `:int` signature is a deliberate
    carrier-ABI regression test.
  - `unwrap-or` cascade -- broken out into
    [unwrap-or-byvalue-cascade](reported/unwrap-or-byvalue-cascade.md);
    nearly complete, only the M7-gated kleisli caller remains.
  - NonEmpty half of step 4 (`ne-from?`/`ne-unwrap`) -- **closed
    2026-06-18** by PR #434 (typed `(List A)` witness); see
    [docs/archive/ne-from-byvalue-option-nonempty-element-type-uninferable.md](archive/ne-from-byvalue-option-nonempty-element-type-uninferable.md).
  - Step 5 (`kleisli.tur` `comp`/`k-apply-raw` retype) -- broken out
    into [kleisli-byvalue-option-cascade](reported/kleisli-byvalue-option-cascade.md).
- [unwrap-or-byvalue-cascade](reported/unwrap-or-byvalue-cascade.md)
  (plan, STEP 1 + producer migrations LANDED) -- `unwrap-or` retyped
  to by-value `[A] [o : (Option A) dflt : A] : A` and the ~10 stdlib
  producers (`zipper`, `seq/*`, `json`, `safe`, `env`, `serial`, ...)
  migrated under the temporary `unwrap-or-carrier` shim. Only the
  M7-gated kleisli caller remains -- effectively blocked on the
  kleisli cascade below, not on its own work.
- [kleisli-byvalue-option-cascade](reported/kleisli-byvalue-option-cascade.md)
  (plan, OPEN, NOT YET STARTED) -- retype `k-apply-raw` / `k-apply` /
  `Category [Kleisli]` to thread `(Option B)` by value; retires the
  `(:: r (Option int))` ascription that landed in PR #426 as a
  temporary patch. Gated on M7 HKT (PR #435 landed M7 elaborator
  behind `TUR_M7_HKT`). One self-contained PR once unblocked.
- **M7 by-value HKT dispatch (Phase 3/4.2): Functor `fmap` AND Monad `bind`
  shapes now work end-to-end** under `TUR_M7_HKT` (flag default-OFF; shipped
  path byte-identical). fmap across element types `{int, cstr, float, struct}`;
  bind exits 21, verified for B = cstr and `(none)` short-circuit. Two M7
  reports resolved/archived 2026-06-19:
  [m7-hkt-fn-returning-applied-type-kind-mismatch](archive/m7-hkt-fn-returning-applied-type-kind-mismatch.md)
  (kind-threading) and
  [m7-hkt-bind-body-byvalue-emit](archive/m7-hkt-bind-body-byvalue-emit.md)
  (gate + class-var-head binding). Probes:
  `docs/upcoming/v2/m7-hkt-probe{,-bind}.tur`. Remaining toward flag-on-by-default
  (Phase 4.2): continuations passed as bare `:fn` poly/fat-closure carriers,
  Applicative `ap` / MonadError shapes, then the stdlib instance/class rewrites.

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

All three tracks are independently progressable.

- **Track A** is the critical path; M5/M6/M7 unlocks Track B direction 2
  (cross-world scheduling) and reduces Track C audit pressure.
- **Track B** direction 1 is unblocked and has spice-side traction
  (PR #15); direction 2 stays gated on Track A gap-H.
- **Track C** is fully independent. The prior soft coupling to Track A
  bridge follow-ups is gone (PR #415/#416 -> spices PR #12).
