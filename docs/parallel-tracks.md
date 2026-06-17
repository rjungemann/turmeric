# Parallel Tracks -- Open Plans and Reports

Snapshot: 2026-06-17 (post-PR #415/#416/#417; post-v0.21.0).

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
  -- umbrella; M2 landed, M3 wholesale-deletion goal retired (see
  archived report), M4 substantively landed across #399/#400/#402/
  #405/#407/#411/#412/#414/#415/#416
- [m4-typeclass-per-method-abi-plan](upcoming/m4-typeclass-per-method-abi-plan.md)
  -- M4a audit refreshed 2026-06-17; M4a deliverables all landed;
  current bridge audit floor is **41 crossings / 11 fixtures**
  (34 `carrier->concrete`, 6 `concrete->carrier`), of which only
  bucket C (8 crossings, tracked in `option-consumer-retype-byvalue`)
  is tractable -- buckets A' / B are by-design carrier-bridge
  regression coverage
- ~~[tco-in-abi-specs-for-stdlib-iteration](archive/tco-in-abi-specs-for-stdlib-iteration.md)~~
  -- **archived 2026-06-17**. The three TCO restriction lifts plus the
  `Eq [Vec]` (PR #400) and `Eq [MutableMap]` rewrites shipped; the
  audit-reduction goal is met (post-#400: 34 crossings / 10 fixtures,
  zero monomorphic deref-copies; Map/Set are `:heap` and no longer
  cross the bridge at all). Residual `Eq [Map]` / `Eq [Set]`
  pure-Turmeric rewrites are tracked in
  [tco-map-set-eq-pure-turmeric-followup](upcoming/tco-map-set-eq-pure-turmeric-followup.md)
  as low-priority type-hygiene work, not on the audit critical path.
- ~~[m3-carrier-bridge-deletion-blocked-on-typeclass-abi](archive/m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md)~~
  -- **archived 2026-06-17**. The original "delete `emit_carrier_bridge`
  wholesale" target is **superseded**: post-#400 the audit floor is
  34 crossings / 10 fixtures with **zero monomorphic deref-copies**,
  and all three remaining buckets (A' fat-closure `:heap` casts,
  C blessed inline-C `tur_ok`/`tur_some` construction, E type-erased
  `SChan`) are by-design boundaries the function is **kept** for.
  M4 dict-slot typing does not move any of the 34. The down-scope
  is complete for the non-HKT collection-Eq cascade; the bucket A'
  residual would only clear via fat-closure-element monomorphization
  (M5/M7-adjacent), which is tracked separately if/when prioritized.
- [option-none-as-null-byvalue-param-segfault](reported/option-none-as-null-byvalue-param-segfault.md)
  (report, RESOLVED 2026-06-17) -- a by-value `(Option A)`/`(Result A B)` param
  can now receive a carrier `#{Construct}` result (`some`/`none`/`ok`/`err`),
  with NULL-safe none (carrier `0` -> `(Option__A){0}`, never deref'd). The
  codegen prerequisite for retiring the Option-consumer `:int` carrier stand-ins.
- [option-consumer-retype-byvalue](reported/option-consumer-retype-byvalue.md)
  (report, PARTIAL 2026-06-17) -- `option-eq?` retyped to by-value `(Option A)`;
  `option-map` (construct-in-by-value-return) and `some?`/`unwrap-or` (cascade
  into refined.tur + kleisli Arrow) are the documented remainder.
- ~~[result-bridge-tail-call-from-pure-tur-to-inline-c](archive/result-bridge-tail-call-from-pure-tur-to-inline-c.md)~~
  -- **archived 2026-06-17**. PR #416 fixed the bare tail-call form and
  PR #415 extended the bridge to all tail positions (if-branched,
  do-block, let-body, do-then-tail) by recognising inline-C carrier-ABI
  callees as carrier producers in `fn_body_tail_is_carrier_producer`.
  Regression coverage: `tests/fixtures/result-bridge-tail-call-to-inline-c/`
  and `tests/fixtures/tail-call-inline-c-carrier-bridge/`. The
  duplicate in `docs/reported/` was a stale copy left behind when the
  archive landed; removed in this snapshot. Unblocks Track C's thin
  pure-Turmeric wrappers over inline-C bodies (the tourist
  `param`/`capture` retype no longer needs the inline-C workaround).

**Order:** M4 is substantively done; the audit refresh (this
snapshot) confirms the residual floor is by-design regression
coverage plus the bucket C cascade tracked in
`option-consumer-retype-byvalue`. The natural sequel is **Plan M5**
(constrained-polymorphic dict typing) and then the M6/M7 HKT
design+implementation pass. Parallel sub-thread: Option none-as-NULL
retirement (by-value-param bridge done; option-eq? retyped;
option-map + some?/unwrap-or remain -- bucket C above).

## Track B -- ECS spice (E2d wiring + sized worlds)

All compiler-side blockers cleared. E2d's last straggler -- by-value
struct fields silently stored as the int64 carrier -- was fixed in
PR #407, and the residual typeclass-dispatch identity gaps for plain
struct/ADT receivers closed in PR #412 (assoc-type returns,
multi-param instances, projection discrimination). E2d wiring is
unblocked end-to-end.

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
  has zero cross-spice casts). **S3 return-type rows also closed**
  (`param`/`capture`/`captures-get`/`req-header` all return
  `(Result cstr cstr)` on `main` today -- verified 2026-06-17 against
  `tourist/param.tur:392,430`, `tourist/router.tur:244`,
  `httpd/request.tur:121`; `mw-chain-run` returns `(Option Response)`
  via `use!`'s `(c-fn [Ctx] (Option Response))` shape). Open
  follow-ups: S4 cons lists in tourist internals; residual S2 (a
  `ctx : int` parameter retype on `param`/`capture`, and a missing
  `Captures` defopaque) inside otherwise-fixed tourist -- the
  `param`/`capture` retype was previously soft-blocked by the
  pure-Tur-wrapper-around-inline-C bridge bug
  ([archive/result-bridge-tail-call-from-pure-tur-to-inline-c](archive/result-bridge-tail-call-from-pure-tur-to-inline-c.md),
  Track A) -- now unblocked by PR #415/#416, so the tourist spice can
  drop its inline-C re-implementation of `capture` at the next pass;
  secondary handle leaks inside plutovg / raylib / rtaudio remain.
- ~~[tourist-middleware-takes-req-not-ctx](reported/tourist-middleware-takes-req-not-ctx.md)~~
  -- **closed in `tur-tourist` v0.2.0** (Ctx-based `use!`, `ctx-attr-*`,
  `ctx-add-header!`, `use-after!`); see
  [archive/tourist-middleware-takes-req-not-ctx](archive/tourist-middleware-takes-req-not-ctx.md).
- [spices-type-features-uplift-plan](upcoming/spices-type-features-uplift-plan.md)
  -- phased per-spice work (rows, typeclasses, sized types where they
  "pay rent")

**Order:** S1 callbacks (done) -> tourist ABI redesign (done) ->
S2 handles in httpd / osc / rtmidi (done) -> S3 request-path
returns (done) -> per-spice uplift phases and the residual S4 /
secondary handle work.

### Not a blocker: "M3 struct-carrier handling of generic ok/err"

The M3 report's audit floor mentions a 10-crossing "bucket C"
covering inline-C `tur_ok`/`tur_some` construction at the
typeclass-dispatch boundary (see
[archive/m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md
"Update 2026-06-17"](archive/m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md)).
A natural-but-wrong read is "generic `(ok v)` / `(some v)` from a
typeclass instance doesn't work cleanly, so any new spice surface
that returns `(Result T E)` / `(Option T)` is gated on M3
finishing." That conclusion is **incorrect** and the next engineer
should not treat it as a reason to defer S3-style work. Evidence:

1. **Primitive payloads through return-typeclass dispatch already
   monomorphize by-value.**
   `tests/fixtures/typeclass-return-dispatch-result-wrapped/input.tur`
   covers `(definstance Dec [int] (dec [v] (ok v)))` dispatched via
   `(:: (dec 42) (Result int cstr))` and back through `ok-val`;
   suite green, audit shows 2 -> 0 crossings since #399 (M3 report
   "Update 2026-06-17 (M2-completion ...)" entry).
2. **By-value-struct payloads through polymorphic `ok`/`err` work.**
   `tests/fixtures/polymorphic-ok-err-value-struct-payload/input.tur`
   covers `(ok (make-struct User ...))` and `(err (make-struct Errm
   ...))` end-to-end, including struct-field access on the unwrapped
   value.
3. **Constrained-generic helpers over collections monomorphize.**
   `tests/fixtures/m5-instance-spec-constraint-var/input.tur` covers
   the `(definstance MyEq [Vec] [(Eq A)] ...)` -> sibling
   `(callee [A] [(Eq A)] [x : (Vec A) ...])` composition the M3
   report calls out as previously broken; audit 4 -> 0 since #399.
4. **The pure-Turmeric rewrite pattern is in stdlib.**
   `Serializable [Pair]` in `stdlib/serial.tur` was the worked
   example: an inline-C instance body rewritten to a pure-Turmeric
   body that dispatches a recursive `(.fst x)` / `(.snd x)`
   through declared `(Serializable A)` constraints, so M4c Path A
   mints `serialize_Pair__spec__(Pair__int__int)` and the carrier
   crossing falls away. The fixture
   `tests/fixtures/serial-composite-instances` validates byte-layout
   equivalence with the prior inline-C body.
5. **Already-shipped spice surfaces use exactly this pattern.**
   `tur-regex` v0.2.0 (`regex-compile` -> `(Result Regex cstr)`,
   `regex-match` -> `(Result Match cstr)`), `tur-tourist` v0.2.0
   (`param`/`capture` -> `(Result cstr cstr)`), and `tur-httpd`
   v0.2.0 (`req-header` -> `(Result cstr cstr)`) all ship typed
   Result returns through the spice public surface. None of them
   needed an M3 change to land.

The narrow case that genuinely is permanent at the carrier boundary
is **`Serializable [Option]`-shaped instances whose `none` arm
relies on `none` being the NULL int64**: the dispatch boundary
derefs `*(Option__int *)(intptr_t)(0)` before the body's NULL guard
runs, so a pure-Turmeric rewrite segfaults. This is **out of scope
for any new spice S3 work**: spice surfaces returning `(Option T)` /
`(Result T E)` from a wrapper that calls a C function and constructs
the value at the wrapper boundary are not on this path. If a future
S3 task is tempted to rewrite an `Option`-returning typeclass
instance body in pure Turmeric across the carrier boundary, see the
M3 report's "Resolution 2026-06-15 (`Serializable [Option]`
deliberately LEFT inline-C)" entry for the segfault repro before
declaring it a follow-up.

Quick decision: writing a spice helper that returns
`(Result T E)` / `(Option T)`? Just write it; declare the return
type; do not gate on M3. Writing an inline-C instance body for an
**existing** typeclass method that returns `(Option T)` and whose
`none` arm crosses the dispatch boundary? Use inline-C; flag the
carrier boundary in a `;;` NOTE; do not "clean it up" later without
reading the segfault repro first.

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

**Parallelizable today** (no hard inter-track blocks):

- A (M4 in progress; TCO/stdlib helper conversions in parallel;
  pure-Tur-wrapper-around-inline-C bridge follow-up shipped in
  PR #415/#416),
- B (E2d wiring resumable; all compiler blockers cleared),
- C (per-spice uplift phases + S3 work; tourist `param`/`capture`
  retype is now unblocked by the Track A bridge follow-up landing).

**No hard inter-track sequencing constraints.** The prior soft
coupling between the Track A bridge follow-up and the Track C
`param`/`capture` retype is gone now that the bridge fix landed.
