# Parallel Tracks -- Open Plans and Reports

Snapshot: 2026-06-18 (post-PR #421; post-v0.21.0; post turmeric-spices
PR #14 / tourist v0.2.5).

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

**Open:**

- [end-to-end-monomorphization-plan](upcoming/end-to-end-monomorphization-plan.md)
  -- umbrella. M2 + M3 (down-scoped) + M4 substantively landed. Natural
  sequel: **M5** (constrained-polymorphic dict typing) -> M6/M7 HKT
  design+implementation pass.
- [m4-typeclass-per-method-abi-plan](upcoming/m4-typeclass-per-method-abi-plan.md)
  -- M4a deliverables landed. Bridge audit floor: **41 crossings / 11
  fixtures** (34 `carrier->concrete`, 6 `concrete->carrier`); only
  bucket C (8 crossings) is tractable, tracked in
  `option-consumer-retype-byvalue`. Buckets A' / B are by-design
  carrier-bridge regression coverage.
- [option-consumer-retype-byvalue](reported/option-consumer-retype-byvalue.md)
  (report, PARTIAL 2026-06-18) -- `option-eq?` AND `option-map` now
  retyped to by-value `(Option A)` with pure-Turmeric bodies. The
  step-2 "0-arg constructor `abi_bindings`" follow-up landed: elab
  attaches the constructor-result-tyvar -> caller-tyvar binding for a
  0-arg `(none)`/`(err)` in non-ground return position, with emit-side
  structural-match guards and a spec-return-ABI consult so
  carrier-context and unresolved-element specs stay correct. Remainder:
  `result-map` (deferred -- a deliberate carrier-ABI regression test
  backs its `:int` signature) and `some?`/`unwrap-or` (cascade into
  refined.tur + kleisli Arrow). A spill-bridge gap surfaced at the
  by-value-producer -> carrier-consumer boundary when the producer is
  wrapped in a `let`/`do`/`if` arg slot -- see
  [option-map-byvalue-result-into-carrier-consumer-let-inside-arg](reported/option-map-byvalue-result-into-carrier-consumer-let-inside-arg.md);
  emit-side direction 1 there closes the regression without waiting on
  the cascade.
- [option-map-byvalue-result-into-carrier-consumer-let-inside-arg](reported/option-map-byvalue-result-into-carrier-consumer-let-inside-arg.md)
  (report, OPEN 2026-06-18) -- regression from the `option-map` retype:
  `(unwrap-or (let [o (some 5)] (option-map o (fn [x] (* x 3)))) 99)`
  hits a hard cc error because the by-value `Option__int` producer in
  the `let` tail is passed to the carrier-`int64_t` `unwrap-or` slot
  without the `&temp` spill bridge. Loud, not silent; the same call
  with the option-map call as the *direct* arg of `unwrap-or` compiles
  and runs. Pre-PR #421 the failing repro worked. Fix direction 1 is
  an emit-side bridge generalization (spill around `let`/`do`/`if`
  wrappers whose tail produces a by-value aggregate into a carrier-int
  slot); direction 2 is the cascade-coupled `unwrap-or` retype tracked
  in `option-consumer-retype-byvalue`.
- [tco-map-set-eq-pure-turmeric-followup](upcoming/tco-map-set-eq-pure-turmeric-followup.md)
  -- low-priority type-hygiene residual; not on the audit critical
  path (Map/Set are `:heap` and no longer cross the bridge).

**Recently resolved** (archived since last snapshot):

- `option-map-literal-none-unannotated-fn-no-A-inference` -- the
  exact minimal repro
  `(println (unwrap-or (option-map (none) (fn [x] (* x 3))) 99))`
  compiles and prints `99` in HEAD. Resolved by the emit-side guard
  suite that landed in PR #421 alongside the option-map retype (the
  call routes to the carrier-context spec whose body uses the
  NULL-safe `(o) ? ((Option *)o)->is_some : 0` deref). The proposed
  elab-side inference improvement is unnecessary for correctness.
  Regression fixture: `tests/fixtures/option-map-literal-none-unannotated-lambda/`.
  (Archived this snapshot.)
- `option-none-as-null-byvalue-param-segfault` -- PR #414 enabled
  carrier->concrete conversion at by-value `(Option/Result)` call sites
  with NULL-safe none. (Archived this snapshot.)
- `result-bridge-tail-call-from-pure-tur-to-inline-c` -- PR #415/#416
  bridged carrier tail calls in all tail positions; unblocked the
  Track C tourist `param`/`capture` retype.
- `tco-in-abi-specs-for-stdlib-iteration`, `m3-carrier-bridge-deletion`
  -- both retired; the post-#400 audit floor (34 crossings, zero
  monomorphic deref-copies) met the down-scoped goal.

## Track B -- ECS spice (E2d wiring + sized worlds)

**Blocked on:**
[`sized-scheduler-system-stage-world-carrier`](reported/sized-scheduler-system-stage-world-carrier.md)
-- the parallel scheduler's `System`/`Stage` type-erase the world
through an `int`/`void*` carrier, so a by-value `(GameWorld n)` struct
has nowhere to ride. Direction 1 (heap-pointer world, single-world
scheduling) is **unblocked** at the compiler level by PR #420's
existential pack/open heap-boxing fix and is implementable today, but
**no spice-side commit has taken it yet** -- this is the next
required step before further sized-world track progress, and it
gates the Slice 8 "wire sized worlds through the parallel scheduler"
follow-up. Direction 2 (cross-world / heterogeneous scheduling) stays
blocked on **gap-H world-type polymorphism**, which itself depends on
the Track A monomorphization phases retiring the carrier bridge.

E2d's compiler-side blockers all cleared (PR #407 stored by-value
struct fields inline; PR #412 closed typeclass-dispatch identity gaps
for plain struct/ADT receivers). PR #420 fixed existential pack/open
of multi-field struct payloads via heap-boxing, unblocking the
sized-world `world-resize` helper.

**Spice-side progress since last snapshot** (verified against
`turmeric-spices` git log, not just the plan doc, which is stale at
2026-06-12):

- **E2d wiring landed** -- PRs #6, #7, #8 shipped P1-P6: associated-type
  storage projection (P1-P5), the variadic `defworld` collapse (P5b),
  and the `StorageOps` typeclass (P6). The plan doc still lists these
  as "spice-side wiring pending" but the wiring is in.
- **E2c sized worlds substantively landed** -- slices 1, 2-4, 4c, 5, 12
  shipped (`SizedDense n A` shape, `SizedSparse`/`SizedTag`,
  `sized-defworld`, `sized-spawn`, generational `Entity` handles,
  `sized-for-each` payoff macro, fallible `sized-spawn ->
  (Result int WorldFull)`).
- `ecs-spice-plan.md`'s "E2c is no longer just wiring" framing is now
  obsolete -- the bounded-capacity world API got built around the
  sized constructors instead. Plan needs a refresh (out of scope here).

**Still open:**

- [ecs-spice-plan](upcoming/ecs-spice-plan.md) -- plan doc is **stale**.
  Reflects 2026-06-12 state; should be re-baselined against E2d
  P1-P6 + E2c slices 1-12 having landed.
- [ecs-sized-world-plan](upcoming/ecs-sized-world-plan.md) -- surface
  settled; partly subsumed by the E2c slices that already shipped.
- [sized-scheduler-system-stage-world-carrier](reported/sized-scheduler-system-stage-world-carrier.md)
  (report -- **the current Track B blocker**, see top of section)
  -- `System`/`Stage` type-erase the world through an int carrier.
  Direction 1 is unblocked at the compiler level but the spice-side
  by-pointer rework has not landed; cross-world (gap-H) scheduling
  remains compiler-blocked.

**Order:** land direction-1 single-world sized scheduling (clears the
current Track B blocker) -> refresh ecs-spice-plan status against
landed E2d/E2c slices -> gap-H if cross-world scheduling is needed.

## Track C -- Spices uplift (type hygiene)

Pure spice-side work; no compiler dependencies.

**Spice-side progress since last snapshot** (verified against
`turmeric-spices` git log):

- **tourist v0.2.2 (PR #12)** -- `param` / `capture` retyped to
  `ctx : Ctx`; closes the residual S2 item the prior snapshot listed
  as "unblocked, awaiting next pass."
- **tourist v0.2.5 (PR #13)** -- `defopaque Pattern` for
  `router-compile` / `router-match` / `router-free`; closes the
  "Pattern opaque still pending" internal S2 item.
- **`Captures` defopaque** -- shipped in tourist v0.2.1 (referenced
  by PR #13 prerequisites).
- **Track B/C cross-over: ecs PRs #4-#14** are listed under Track B
  but also land spice-side type hygiene.

**Still open:**

- [spices-int-stand-in-audit-2026-06-14](reported/spices-int-stand-in-audit-2026-06-14.md)
  -- S1/S2/S3 closed across the audited public surfaces. Genuinely
  remaining:
  - **S4 cons lists** in tourist internals.
  - **Secondary handle leaks** outside the audited public surfaces:
    - raylib: models / text / textures / camera / shapes.
    - rtaudio: `DeviceInfo` handle + the `rtaudio/stream.tur:62`
      S1-class callback hole.
    - plutovg: dash-array / font-cache / gradient-stops / rect.
- [spices-type-features-uplift-plan](upcoming/spices-type-features-uplift-plan.md)
  -- phased per-spice work (rows, typeclasses, sized types where they
  "pay rent"). Independent of the audit residuals above.

The audit report's open section can be trimmed to the two bullets
above; nothing else from the original audit is still pending.

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

A / C are independently progressable. The prior soft coupling between
Track A (bridge follow-up) and Track C (`param`/`capture` retype) is
gone (PR #415/#416 -> spices PR #12).

**Track B is currently blocked** on
[`sized-scheduler-system-stage-world-carrier`](reported/sized-scheduler-system-stage-world-carrier.md).
The compiler-side prerequisite for direction 1 (heap-pointer world,
single-world sized scheduling) landed in PR #420, but the spice-side
`System`/`Stage` rework to take it has not -- until that lands, the
Slice 8 follow-up "wire sized worlds through the parallel scheduler"
cannot proceed, and cross-world (direction 2 / gap-H) work stays
gated on Track A's monomorphization phases.
