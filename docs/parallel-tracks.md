# Parallel Tracks -- Open Plans and Reports

Snapshot: 2026-06-17 (post-PR #411; post-v0.21.0).

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
  (Vec/Map/MutableMap/Set) to pure Turmeric once the TCO gate is lifted.
  `Eq [Vec]` landed as a pure-Turmeric TCO'd loop in PR #400; Map /
  MutableMap / Set conversions still pending.
- [m3-carrier-bridge-deletion-blocked-on-typeclass-abi](reported/m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md)
  (report) -- closes when M4 + TCO conversions land

**Order:** M4 -> TCO lift -> bridge tail elimination -> archive M3 report.

## Track B -- ECS spice (E2d wiring + sized worlds)

All compiler-side blockers cleared. E2d's last straggler -- by-value
struct fields silently stored as the int64 carrier -- was fixed in
PR #407, so ECS authors no longer need `:int` stand-ins for component
shapes. E2d wiring is unblocked end-to-end.

- ~~[defopaque-struct-payload-fails-through-unsafe-helper](archive/defopaque-struct-payload-fails-through-unsafe-helper.md)~~
  -- **closed**; archived.
- ~~[macro-template-type-position-rejects-unquoted-compound](archive/macro-template-type-position-rejects-unquoted-compound.md)~~
  -- **closed**; archived.
- ~~[defstruct-byvalue-struct-field-stored-as-int-carrier](archive/defstruct-byvalue-struct-field-stored-as-int-carrier.md)~~
  -- **closed in PR #407**; archived. Bare by-value struct/ADT fields
  now store inline rather than as the int64 carrier.
- [ecs-spice-plan](upcoming/ecs-spice-plan.md) -- E2d wiring; all
  compiler-side blockers cleared, spice-side work can resume.
- [ecs-sized-world-plan](upcoming/ecs-sized-world-plan.md) -- surface
  settled; lands after E2d wiring.

**Order:** finish E2d wiring -> sized worlds.

## Track C -- Spices uplift (type hygiene)

Pure spice-side work; no compiler dependencies.

- [spices-int-stand-in-audit-2026-06-14](reported/spices-int-stand-in-audit-2026-06-14.md)
  (report) -- S1 callbacks closed; S2 handles in httpd / osc / rtmidi
  closed in their respective v0.2.0 releases (httpd's Request/Response/
  Wbuf are also re-used by tourist v0.2.1 so the framework boundary
  has zero cross-spice casts). Open follow-ups: S3 result/option on
  `param`/`capture`/`req-header`, S4 cons lists in tourist internals,
  secondary handle leaks inside plutovg / raylib / rtaudio.
- ~~[tourist-middleware-takes-req-not-ctx](reported/tourist-middleware-takes-req-not-ctx.md)~~
  -- **closed in `tur-tourist` v0.2.0** (Ctx-based `use!`, `ctx-attr-*`,
  `ctx-add-header!`, `use-after!`); see
  [archive/tourist-middleware-takes-req-not-ctx](archive/tourist-middleware-takes-req-not-ctx.md).
- [spices-type-features-uplift-plan](upcoming/spices-type-features-uplift-plan.md)
  -- phased per-spice work (rows, typeclasses, sized types where they
  "pay rent")

**Order:** S1 callbacks (done) -> tourist ABI redesign (done) ->
S2 handles in httpd / osc / rtmidi (done) -> per-spice uplift phases
and S3 work on the request path.

---

## Unassigned / cross-cutting reports

All previously listed cross-cutting reports have been resolved and
archived since the prior snapshot:

- ~~[result-typedef-duplicated-across-modules](archive/result-typedef-duplicated-across-modules.md)~~
  -- **closed**; separate-compilation repro landed in PR #408 with
  the de-dup fix.
- ~~[sleep-ms-not-auto-loaded](archive/sleep-ms-not-auto-loaded.md)~~
  -- **closed in PR #410**; `stdlib/time.tur` is now importable as
  the `time` module, retiring per-fixture stubs.
- ~~[mutmap-multi-param-producer-typing-blocked](archive/mutmap-multi-param-producer-typing-blocked.md)~~
  -- **closed in PR #411**; MutableMap typed-pointer producer
  monomorphization fixed.

No open cross-cutting reports as of this snapshot.

---

## Concurrency summary

**Parallelizable today** (no inter-track blocks):

- A (M4 in progress; TCO/stdlib helper conversions in parallel),
  B (E2d wiring resumable; all compiler blockers cleared),
  C (per-spice uplift phases + S3 work).

**No remaining inter-track sequencing constraints.**
