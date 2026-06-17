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
  -- umbrella; M2 landed. M3 bridge down-scope **complete for the non-HKT
  collection-Eq cascade** (2026-06-17): audit floor is 34 crossings with
  **zero monomorphic deref-copies** -- all 34 are by-design boundaries
  (`:heap` casts / blessed inline-C / type-erased)
- [tco-in-abi-specs-for-stdlib-iteration](upcoming/tco-in-abi-specs-for-stdlib-iteration.md)
  -- **Vec TCO'd by-value loop landed (#400)**, dropping the bridge audit
  60 -> 34. Map/Set/MutableMap producer slices held for follow-up (they no
  longer cross the bridge -- `:heap` already)
- [m4-typeclass-per-method-abi-plan](upcoming/m4-typeclass-per-method-abi-plan.md)
  -- per-method dict-slot typing. NOTE: re-audit shows M4 dict slots clear
  **0** of the current 34 crossings (bucket A' is a fat-closure cast, not a
  dict-slot consumption); the remaining 22 `Vec` casts clear only via
  fat-closure-element monomorphization (M5/M7-adjacent). M4 remains worth
  doing for ABI cleanliness but is not the lever for this cascade's residual
- [m3-carrier-bridge-deletion-blocked-on-typeclass-abi](reported/m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md)
  (report) -- down-scope complete; the bridge is kept (not deleted) for the
  three by-design boundaries. Full audit-zero needs fat-closure-element mono.

**Order:** TCO lift (Vec done) -> bridge tail elimination (**done: monomorphic
deref-copies are zero; bridge down-scoped to by-design boundaries**) ->
remaining residual is the fat-closure-element / HKT frontier (M5/M7), not M4.

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

## Track E -- Interpreter parity (post-v1) -- **RESOLVED 2026-06-17**

No dependencies on any other track. **Every phase has landed or been
intentionally carved**; the plan is archived at
[turi-parity-post-v1-plan](archive/turi-parity-post-v1-plan.md).

- `EX_*` parity: **115/116 handled, 1 carved, 0 gaps** (was 36 unhandled).
- Native parity: **0 uncarved native gaps** (2 conditional-preload carve-outs).
- Both CI ratchets (`check_turi_parity.py`, `check_turi_native_parity.py`)
  pass and are wired into `tests/run.sh`.
- The parity matrix shipped (TI9,
  [turi-parity-guide](guides/turi-parity-guide.md)).
- The allowlist -> denylist flip (TI8.b/W5) landed: `tests/run-turi.sh`
  carries no allowlist and runs every fixture under `tur --interpret`
  minus the documented carve-outs.
- TI10 Tiers A+B landed (scalar-keyed maps, turi-closure key comparators,
  reentrant comparators); the non-int-value carrier-ascription follow-up
  is resolved and archived.
- Turi harness green at **1212 passed, 0 failed, 416 skipped** (all 416
  are permanent user-inline-C carve-outs).

Residual surface is exactly the documented, intentional carve-outs:
`EX_CPS_CONT_APP` (never emitted by the elaborator), user inline-C
(`docs/turi-carve-out.txt`), and WASM async (`src/turi/fiber.c`).

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

- A (start M4) and C (start S1 callbacks) -- two streams can run
  concurrently. (E and F both resolved 2026-06-17.)

**Sequenced inside tracks:**

- B is gated on its two bug fixes before E2d wiring can resume.
