# Parallel Tracks -- Open Plans and Reports

Snapshot: 2026-06-15 (post-v0.21.0).

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

- [end-to-end-monomorphization-plan](upcoming/end-to-end-monomorphization-plan.md)
  -- umbrella; M2 landed, M3 deletion blocked on M4
- [m4-typeclass-per-method-abi-plan](upcoming/m4-typeclass-per-method-abi-plan.md)
  -- **immediate next step**; unblocks M3 bridge deletion
- [tco-in-abi-specs-for-stdlib-iteration](upcoming/tco-in-abi-specs-for-stdlib-iteration.md)
  -- runs in parallel with M4; converts inline-C carrier helpers
  (Vec/Map/MutableMap/Set) to pure Turmeric once the TCO gate is lifted
- [m3-carrier-bridge-deletion-blocked-on-typeclass-abi](reported/m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md)
  (report) -- closes when M4 + TCO conversions land

**Order:** M4 -> TCO lift -> bridge tail elimination -> archive M3 report.

## Track B -- ECS spice (E2d wiring + sized worlds)

ECS is mostly shipped (I1-I6, E0-E4, E2d compiler prereqs); two compiler
bugs and a macro hole are all that stand between it and the next two
phases.

- [defopaque-struct-payload-fails-through-unsafe-helper](reported/defopaque-struct-payload-fails-through-unsafe-helper.md)
  (report) -- blocks E2d struct-A flow
- [macro-template-type-position-rejects-unquoted-compound](reported/macro-template-type-position-rejects-unquoted-compound.md)
  (report) -- blocks `defworld` macro
- [ecs-spice-plan](upcoming/ecs-spice-plan.md) -- E2d wiring; resumes
  once both bugs are fixed
- [ecs-sized-world-plan](upcoming/ecs-sized-world-plan.md) -- surface
  settled; lands after E2d wiring

**Order:** fix both reports -> E2d wiring -> sized worlds.

## Track C -- Spices uplift (type hygiene)

Pure spice-side work; no compiler dependencies. Pairs naturally with
Track D.

- [spices-int-stand-in-audit-2026-06-14](reported/spices-int-stand-in-audit-2026-06-14.md)
  (report) -- S1 callbacks first (5 cases, zero type protection)
- [tourist-middleware-takes-req-not-ctx](reported/tourist-middleware-takes-req-not-ctx.md)
  (report) -- redesign Tourist middleware ABI
- [spices-type-features-uplift-plan](upcoming/spices-type-features-uplift-plan.md)
  -- phased per-spice work (rows, typeclasses, sized types where they
  "pay rent")

**Order:** S1 callback fixes -> tourist ABI redesign -> per-spice uplift
phases in any order.

## Track D -- Workspace consolidation (orthogonal)

Mechanical refactor with high friction-removal payoff. Independent of
A/B/C, but **best landed after** Track C's tourist redesign so the merge
doesn't carry a known-bad public API into the monorepo.

- [recombine-turmeric-spices-plan](upcoming/recombine-turmeric-spices-plan.md)
  -- single repo / single workspace

## Track E -- Interpreter parity (post-v1)

No dependencies on any other track. Long-running, can be picked up
whenever.

- [turi-parity-post-v1-plan](upcoming/turi-parity-post-v1-plan.md) --
  36 of 114 `EX_*` kinds unhandled; needs published parity matrix +
  denylist CI gate to replace the allowlist

## Track F -- PR #386 regression hotfix

**Resolved 2026-06-17.** The let-bound `source_binding` alias rule no longer
chains to lifted-lambda `__fn_N` helpers (new `is_lifted_lambda` Binding flag),
restoring closure-dispatch for captureless closure-returning lambdas and the
`with-resource` macro. Both regressed fixtures flipped FAIL -> PASS; the report
is archived at
[docs/archive/pr-386-source-binding-alias-breaks-closure-and-with-resource.md](archive/pr-386-source-binding-alias-breaks-closure-and-with-resource.md).

---

## Concurrency summary

**Parallelizable today** (no inter-track blocks):

- A (start M4), C (start S1 callbacks), E (parity matrix)
  -- three streams can run concurrently. (F resolved 2026-06-17.)

**Sequenced inside tracks:**

- B is gated on its two bug fixes before E2d wiring can resume.
- D should wait on C's tourist redesign.
