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
  -- umbrella. M2/M3-downscoped/M4 substantively landed. Next: **M5**
  (constrained-polymorphic dict typing) -> M6/M7 HKT design+implementation.
  Empirical M5 surface is pinned by `m5-scope-audit-2026-06-18`: most M5
  behaviors already landed via M4c Path A; one genuine HOF gap remains.
- [m5-scope-audit-2026-06-18](upcoming/m5-scope-audit-2026-06-18.md)
  -- pins what's left for M5 against M4c-landed behavior. One genuine
  M5-class gap (constrained-poly HOF arg baked through carrier);
  everything else in the original M5 scope already works.
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
  - `unwrap-or` -- cascade-coupled (~10 stdlib modules produce
    carrier-int Options); needs its own PR.
  - NonEmpty half of step 4 (`ne-from?`/`ne-unwrap`) -- blocked on
    inference, see next bullet.
  - Step 5 (`kleisli.tur` `comp`/`k-apply-raw` retype) -- blocked on
    the broader carrier-Option-producer cascade.
- [ne-from-byvalue-option-nonempty-element-type-uninferable](reported/ne-from-byvalue-option-nonempty-element-type-uninferable.md)
  (plan, OPEN 2026-06-19) -- the NonEmpty half of step 4 stays on the
  carrier until the inference gap is closed by giving `ne-from?` a
  typed list parameter (`(defopaque List [A] :int)` + `list-of`
  smart constructor), so `A` is recovered from the argument rather
  than ascribed at the call site. Caller-ascription `(:: o (Option int))`
  workaround is explicitly out -- it would propagate carrier-`:int`
  into every NonEmpty consumer and is the kind of "tighten the types
  later" patch CLAUDE.md forbids.
- [tco-map-set-eq-pure-turmeric-followup](upcoming/tco-map-set-eq-pure-turmeric-followup.md)
  -- low-priority type-hygiene residual; not on the audit critical path
  (Map/Set are `:heap`, no longer cross the bridge). **Note:** the
  primary deliverable (TCO'd pure-Turmeric `Eq [Map]`/`Eq [Set]`) shipped
  in PR #424; verify what residual content remains and archive if empty.

**Recently resolved** (archived since last snapshot):

- `option-map-byvalue-result-into-carrier-consumer-let-inside-arg`
  -- emit-side spill-bridge generalized for `let`/`do`/`if` wrappers
  whose tail produces a by-value aggregate into a carrier-int slot
  (PR #425).
- `option-map-literal-none-unannotated-fn-no-A-inference` -- closed by
  PR #421's emit-side guard suite; regression fixture
  `tests/fixtures/option-map-literal-none-unannotated-lambda/`.
- `zero-arg-construct-ground-byvalue-return` and
  `parametric-option-return-clone-struct-app-leak` -- both retired
  (cleared the path for the BoundedIdx half of step 4).
- Earlier-snapshot resolutions still load-bearing for context:
  `option-none-as-null-byvalue-param-segfault` (PR #414),
  `result-bridge-tail-call-from-pure-tur-to-inline-c` (PR #415/#416),
  `tco-in-abi-specs-for-stdlib-iteration`,
  `m3-carrier-bridge-deletion`.

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

- [ecs-spice-plan](upcoming/ecs-spice-plan.md) -- needs a refresh: the
  "E2d wiring pending" / "direction-1 unblocked but unimplemented"
  framing is now stale. Remaining real work is direction 2 (cross-world
  scheduling) once Track A retires the carrier bridge.
- [ecs-sized-world-plan](upcoming/ecs-sized-world-plan.md) -- surface
  settled; largely subsumed by E2c slices 1-12 + PR #15. Refresh or
  archive after the ecs-spice-plan refresh decides scope.

**Order:** refresh both plan docs against landed state -> gap-H if
cross-world scheduling is needed for Track B's next slice.

## Track C -- Spices uplift (type hygiene)

Pure spice-side work; no compiler dependencies.

**Open work:**

- [spices-type-features-uplift-plan](upcoming/spices-type-features-uplift-plan.md)
  -- phased per-spice uplift (rows, typeclasses, sized types where they
  pay rent). Independent across spices.
- [spices-int-stand-in-audit-2026-06-14](reported/spices-int-stand-in-audit-2026-06-14.md)
  (report) -- 19 spices still carry `:int` stand-ins for handles /
  callbacks / options; phased retype across the offending spices. Blocks
  any session-middleware-style composition work.

**Recently landed in turmeric-spices** (since last snapshot):

- PR #12 (tourist v0.2.2): `param`/`capture` retyped to `ctx : Ctx`.
- PR #13 (tourist v0.2.5): `defopaque Pattern` for
  `router-compile`/`router-match`/`router-free`.
- PR #16: remaining tourist `ctx : int` -> `ctx : Ctx` retype.
- PR #18 (ansi v0.2.0): `Color` typeclass collapse.
- Audit row `spices-int-stand-in-audit-2026-06-14` previously closed
  S1/S2/S3 surfaces + four secondary-handle rows (tourist internals
  landed in turmeric-spices 9a590cb / v0.2.6).

### Quick decision: spice helpers returning `(Result T E)` / `(Option T)`

Just write it; declare the return type. Do **not** gate new S3-style
work on the M3 bucket-C residual. Primitive payloads, by-value-struct
payloads, and constrained-generic helpers all monomorphize today
(`tur-regex`, `tur-tourist`, `tur-httpd` v0.2.0 all ship typed Result
returns through the public surface). The narrow permanent case is
`Serializable [Option]`-shaped instances whose `none` arm relies on
`none` being the NULL int64 -- keep those inline-C with a `;;` NOTE.
See [archive/m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md
"Update 2026-06-17"](archive/m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md)
for the segfault repro before rewriting.

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
