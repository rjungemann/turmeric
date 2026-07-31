# Representation consolidation -- the meta-plan

**Status:** proposed. This is not a plan for one seam; it is the plan for how
every representation-consolidation increment gets chosen, de-risked, landed,
and verified, so the work neither stalls (as attempts here have) nor trades
away the low-level performance the current representations exist to buy.
Child plans (starting with
[fn-value-fat-normalization-plan.md](fn-value-fat-normalization-plan.md))
follow this template; this doc owns the principles, the sequencing, and the
process gates.

The representation inventory this plan operates on is
[docs/guides/value-representations-guide.md](../guides/value-representations-guide.md):
~4 data representations (int64 carrier, by-value aggregate, heap pointer,
concrete scalar) and ~6 fn-value forms, times every boundary kind, with each
bridge implemented point-by-point.

## What "consolidation" means here -- and what it must not mean

**Consolidate the *conventions*, not (necessarily) the *representations*.**
The bug family is not "too many representations"; it is too many *unwritten
pairwise agreements* about which representation crosses which boundary,
each decided locally at an emit site. N representations behind one decision
function and one bridge library is fine -- that is how fast compilers work.
N representations times M boundaries, each pair hand-negotiated, is the
current defect generator.

So the target state is:

1. **One decision function per axis.** For any (type, boundary-position)
   pair, a single named routine answers "which representation crosses here".
   Emit sites consult it; none of them re-derive it.
2. **One bridge library.** Every representation conversion (carrier<->
   concrete, thin<->fat, spill<->unspill) is a named chokepoint routine.
   An emit site that needs a conversion calls the chokepoint; a Debug-build
   ICE fires when a value reaches a boundary un-routed.
3. **Fewer representations only where a form is redundant.** A
   representation is deleted only when the decision function proves no
   (type, position) pair still needs it -- deletion is an outcome of
   consolidation, never the method.

This split matters because the two failed instincts here are both
representation-first: "widen the fast path's domain" (reverted --
`poly-hof-constrained-arg-baked-carrier`: demoting named-tyvar fn params
onto the carrier baked the int64 ABI over by-value struct args) and
"reject what the zoo can't represent" (abandoned -- the poly-result report's
compile-time diagnostic fired on six correct fixtures, because
representability is decided downstream across five emit surfaces and cannot
be reconstructed at the elaboration call site). Both failures are the same
lesson: **until the decision lives in one place, neither widening nor
gating it can be done correctly.**

## Performance: what must survive, and the principle that protects it

The representations exist for speed, and the consolidation must be
articulate about which properties are load-bearing:

- **Carrier fast paths** -- scalar and heap-pointer payloads cross erased
  boundaries with zero conversion cost (the bits are the value). This is
  the backbone of typeclass dispatch and generic specialization.
- **Thin calls for captureless fns** -- a bare function crossing as a code
  pointer costs one direct call; no env alloc, no double indirection.
- **By-value aggregates** -- small structs/ADTs live in registers and on
  the stack; forcing them onto the heap to make them "uniform" would tax
  every `Option`/`Result` in the language.
- **Monomorphized clones** -- the M7 by-value HKT path exists precisely
  because the carrier dict model was scored and rejected
  (`docs/archive/history/hkt-dispatch-options-tradeoff.md` scored the three
  dispatch models on blast radius, expressiveness, and "decisively --
  whether they retire or reintroduce the int64 carrier ABI").
- **Raw C function pointers at `extern-c` boundaries** -- a correctness
  constraint that reads like a perf one: the closure-unification plan's
  risk list is explicit that consolidation "must not box a fn destined for
  a raw C function-pointer parameter"; the raw-callback set is excluded
  from any fn-value normalization by name.

Two archive findings sharpen how cheap consolidation can be when it
respects width:

- **"Reinterpret, not box" (the b4 key decision):** a single-carrier
  wrapper whose by-value form fits one word crosses the fat-closure
  boundary "by reinterpret with no heap box and no deref." **Width, not
  nominal kind, is the right discriminator** -- a same-width unification
  is free, and only genuinely wide values need the spill/box bridge.
- **The by-value <-> carrier box bridge is already mandatory where it
  exists** (`emit_expr.c` ~432: "the box/unbox bridge is mandatory at the
  seam"), with `:heap` as the documented escape ("it already IS a
  pointer-sized carrier"). Consolidation does not add this cost; it makes
  the existing cost total instead of site-by-site.

The protecting principle: **fast in the small, uniform in the large.**

- *Interior* of a compilation-visible region -- a function body, a direct
  call to a known callee, a monomorphized clone -- the compiler may use the
  fastest representation it can prove: thin calls, unboxed by-value forms,
  raw scalars. Nothing in this plan touches interior code.
- *Escape points* -- a value stored into a container or struct field,
  returned through a fn-typed or erased boundary, ascribed, captured, or
  passed to a parameter whose representation the caller cannot see -- get
  the normalized form for their axis, with the conversion (shim, spill,
  re-wrap) paid once at the edge.

The precedent is the struct-field-fat fix: bare fns are shimmed to fat
`{thunk, env}` handles *at the store*, and field-calls dispatch fat -- while
direct calls to the same functions stay thin everywhere else. Nobody has
measured a regression from it. Each child plan still carries an explicit
benchmark gate (below) so this stays an empirical claim, not a vibe.

## Why past efforts stalled -- and why the successes succeeded

The archive holds both outcomes; the meta-plan is built from the deltas.

**Succeeded: the closure representation unification (2026-06).**
`docs/archive/history/closure-representation-unification-plan.md` faced
nearly this exact problem (captureless -> bare pointer, capturing -> heap
fat box, five stdlib crash sites) and its decisive move was **escalating
scope to the root fix**: rather than "papering over the `:ptr<void>`
overload with an implicit-`^fat` heuristic (Option A)," it introduced the
first-class closure type (Option B) -- "this removes the bare/fat
representation split at its root ... without needing a per-call-site
boxing heuristic that has to enumerate every fat sink." Option A stayed
documented as the fallback if B proved too large. The rule: when the
choice is between a heuristic that must enumerate sites and a
representation change that makes the enumeration unnecessary, take the
representation change -- and keep the cheap option on file as the
pre-registered retreat.

**Succeeded: the CPS backend unification -- the repo's best retirement
template** (`docs/archive/cps-backend-unification-plan.md` and siblings).
Two lowering strategies consolidated to one, old path deleted
(`emit_cps.{c,h}` no longer exist). What made it land:

- **Two milestones, explicitly not conflated**: "becoming the default" and
  "deleting the old path" were separate gates with separate criteria.
- **A forced-on probe over the whole corpus** as the graduation signal
  (2142 fixtures, every failure classified) -- and, crucially, **the probe
  was wrong and the doc says so**: flipping the default surfaced 24
  failures the probe missed, because the eviction gate admitted shapes it
  should have evicted. The fix was *narrowing* what the new path claims
  ("Restricting CPS-emitted signatures to scalars keeps the ABI
  single-valued"), not patching the 24 sites.
- **A hard `expires_at` contract** as forcing function -- the release-cut
  skills refuse to bump past it until the row graduates or shelves.
- **Byte-identity** as the faithfulness proof for the final deletion
  (flag-off output byte-identical across the corpus).

Its stalled sub-efforts carry the single most recurring stall verdict in
the archive -- **"load-bearing, not redundant."** Three independent docs
reach it (`abortive-shift-retirement-blocked.md`,
`cps-backend-n6-fallback-followups-blocked.md`,
`closure-result-monomorphization-plan.md`): a path that looked like a
redundant duplicate turned out to be strictly more expressive or more
general for its case, and the deletion was blocked by numbered probes.
The corollary rule: **every representation slated for deletion gets a
probe whose only job is to falsify "this is redundant" -- before any code
moves.**

There is also a second-order stall: **"functionally free."** The
closure-result monomorphization plan shipped its groundings but abandoned
its consolidation objective ("delete the bridge / 0 crossings": crossing
count went 102 -&gt; 102, Phase 3 "NOT PURSUED, by decision") because "the
bridge stays -- load-bearing and functionally free." A redundant-looking
path survives if keeping it costs nothing; deletion must state its benefit
beyond tidiness (here: each unconsolidated cell is a standing bug
generator -- the missing-cells table is the benefit ledger).

**Succeeded: the carrier<->concrete crossing campaign (2026-06).** PRs
#437-#481 were a reactive whack-a-mole -- one defect ("a parametric
payload's concrete element type collapsing to the int64 carrier"),
surfacing at a different emit site per spice. What ended it
(`docs/archive/carrier-concrete-abi-crossing-audit-plan.md` +
`docs/archive/history/carrier-crossing-recovery-routing-plan.md`):

1. an **audit** enumerated every crossing site up front, converting an
   open-ended bug stream into a closeable list;
2. a **composition stress matrix** found the next bugs proactively ("found
   by us, not by a downstream spice");
3. a **routing plan** made the recovery routines mandatory chokepoints
   (R1), migrated the ad-hoc sites (R2), added a Debug ICE for "forgot to
   route" (R3), and shipped a static registry + CI ratchet
   (`tools/check_crossing_routing.py`, wired into `tests/run.sh`) so the
   consolidation cannot silently un-happen (R4).

**Succeeded: the M7 by-value HKT migration.** A measurement-driven design
pass scored the options *before* committing; the migration ran dual-path
under `TUR_M7_HKT` with the new path as default; the legacy carrier path
was retired only after the suite flip -- and CLAUDE.md now says "there is
no longer a second suite." Dualism was a phase with an exit (~2 weeks),
not a steady state, and the exit was engineered in advance: a **written
rot license** ("may degrade as classes migrate ... that is expected, not a
regression to chase") is what made the second suite non-load-bearing, a
downstream-dependency sweep (including `../turmeric-spices/` and open
reports citing the flag as a workaround) cleared the deletion, and a
pre-registered abort path said exactly what to do if the old path turned
out load-bearing after all. Two more of its lessons bind here: the flag
was deliberately NOT promoted to `EXPERIMENTS[]` ("a toggle whose 'off'
branch is explicitly permitted to rot ... is dead code waiting to be
removed"), and its aftershock is a warning --
`b4-fat-closure-byvalue-adt-abi-plan.md` records that "M7 graduating did
not deliver the ABI change described here": **consolidating a dispatch
path is not consolidating a representation**; each axis needs its own
campaign.

**Stalled (instructively): the first fn-element substitution fix
(2026-07-30).** `docs/archive/fn-element-tyvars-not-substituted-in-spec-types.md`
is the sharpest recent record. The first attempt applied a global
substitution change without a grounding guard: 1916 fixture failures,
reverted. The landed fix measured each step in isolation (step 1 alone:
3 snapshot diffs, runtime unchanged; step 2 alone: 1916; steps 1+2+3:
green) and its decisive piece was a guard -- **only adopt a substitution
that RESOLVED**; when the context cannot ground a tyvar, keep the stable
erased form. The general form of that guard appears in every successful
change in this area: *normalization must be provably grounded at the point
it is applied, and the un-normalized form must remain legal where proof is
unavailable.*

**Left undone by design: the fn-value axis.** The routing plan's inventory
explicitly excluded "the fn-value inner-clone derivation" as by-design.
The 2026-07 bug harvest -- `poly-result-hof-capturing-closure-sigbus`,
`fn-typed-value-return-ascribe-miscompiles`,
`fn-payload-in-container-undeclared-temp`, plus the type-fuzzer findings --
clusters exactly on the axis the last campaign carved out. That is not a
coincidence; it is the map telling us where the next campaign goes.

Distilled stall anatomy:

| Anti-pattern | Instance | Rule that replaces it |
| --- | --- | --- |
| Global change, unmeasured blast radius | fn-element attempt 1 (1916 failures) | Measure each step in isolation before combining; suite count is the gauge |
| Consolidate by widening a fast path | carrier widening over named-tyvar fn params | Fast paths keep their exact legality predicate; consolidation moves the *decision*, not the domain |
| Enforce before centralizing | poly-result compile-time diagnostic (6 false positives) | The ICE/gate comes *after* the chokepoint exists (routing plan R3 after R1/R2) |
| Permanent dual-path | (avoided by M7; risk for any flag) | A dual-path ships with its retirement criterion, per the experiments discipline |
| Fix sites one fish at a time | PRs #437-#481, #475-#504 | Audit first; route through chokepoints; ratchet with a registry check |
| Delete a "redundant" path unprobed | abortive-shift retirement, N6 fallback ("load-bearing, not redundant") | A redundancy-falsification probe per deletion candidate, before code moves |
| No forcing function | (the shelved consolidations) | Every dual-path carries an `expires_at`-style contract or a written rot license |
| Assume the flip probe is complete | CPS graduation (probe missed 24 failures) | The default flip is its own measurement; when it disagrees with the probe, narrow the new path's claim rather than patch the misses |

Two further findings from the archive shape expectations rather than rules:

- **Coincidences hold this area up.** Two 2026-07 docs say so verbatim:
  `result_kind` staying `TY_INT` was "a correct handle width by accident,
  which is the coincidence this area rests on," and the mangling fix
  "removed the coincidence that was hiding" a latent mismatch.
  Consolidation will therefore *surface* latent bugs; pre-register that as
  an expected outcome of each increment, not a regression against it.
- **Representation splits hide behind one-sided test coverage.** The
  arrow-thin crash was "masked only because the test suite exercises
  captureless arrows exclusively." For every representation an increment
  touches, first find (or write) the fixture that exercises the *other*
  side of the split.

## The probe discipline

"Probe" here follows the house usage (`tests/shallow-handler-probes.sh`,
`stackless-signoff-probes.sh`, `tests/probes/cps-abi-c0/`, the refine
plans' RE probes): a deliberately non-suite measurement that answers ONE
de-risking question about a migration, under conditions the fixture
harness cannot express -- distinct from a fixture, which permanently pins
a behavior. A probe that produces a result worth pinning gets promoted
into a fixture (the shallow-handler 105-vs-10 result is the precedent).
The house's sharpest statement of why probes come first is in the
refinement plan: a report there was "WRONG TWICE before it was right ...
both earlier readings were consistent with the probes run at the time --
which is the argument for widening the probe set BEFORE writing down a
root cause, not after."

This campaign uses five kinds:

1. **Blast-radius probes.** Before an increment lands, its mechanical core
   is applied alone and the suite delta measured (the fn-element fix's
   per-step table is the template). An increment whose isolated delta is
   not understood does not proceed to composition.
2. **Boundary-behavior probes.** The hand-minimized ok/broken matrices
   (e.g. the 11-row table in
   `fn-typed-value-return-ascribe-miscompiles.md`) become fixtures *before*
   the fix -- ok rows included, so the working boundary cannot regress
   while the broken rows flip.
3. **Continuous composition probes.** `tests/type-fuzz-src.py` is the
   standing generalization of the audit plan's composition stress matrix:
   correct-by-construction programs over random wrapper x boundary
   compositions, with `--known-probes` pinning each open report and
   `known_bug_slug` keeping default runs green. Each landed increment
   retires its known rows, returns those shapes to the default pool, and
   runs fresh-seed sessions as acceptance.
4. **ABI-ratification probes.** For a new normalized convention, prove the
   calling convention in hand-written C against the real runtime *before*
   the emitter that will produce it exists -- `tests/probes/cps-abi-c0/`
   is the exemplar ("each transcribes a colored function into the ABI by
   hand, node-for-node ... and compiles against the real DK runtime").
   Increment 1's fat-normalized boundaries should get the same treatment:
   a hand-written C file per boundary shape, ASan/UBSan clean, kept for
   reproducibility.
5. **Performance probes.** Each child plan names the benchmark(s) that
   actually exercise its seam (`benchmarks/`, `tur run bench` -- e.g.
   `bench-poly-specialize.tur` for dispatch seams, closure-heavy benches
   for the fn axis), records before/after on the same box, and states its
   acceptable delta *in the plan before landing*. The house neutrality
   template is `catch-unwind-graduation-plan.md` Part B: flag-off codegen
   byte-identical for untouched fixtures, plus suite wall-clock and a
   representative bench with the change on vs off -- and the CPS
   readiness doc states the governance: "if a regression shows, it gates
   the flip, not the correctness." A regression outside the stated
   envelope is a stop-and-redesign signal, not a note in the PR.

## Observability: make the decision auditable

`--emit-abi-trace` already classifies every resolved call site by ABI path
(concrete-clone / dictionary / polymorphic-wrapper / carrier,
`src/compiler/emit_module.c` ~4546). Extend it with a **representation
trace**: one line per boundary crossing -- value's type, boundary kind,
chosen representation, bridge applied. Two payoffs:

- a probe can *diff traces* across a change, so "this increment only
  re-routes fn-typed stores" becomes a checkable claim rather than a hope;
- disagreement becomes visible before it becomes a segfault: a trace line
  where producer and consumer name different representations is a bug
  report with the location attached.

The trace lands early (increment 0) because every later increment uses it
for its blast-radius probe.

## Sequencing

Ordered so each increment shrinks the surface the next one must reason
about, and cowpaths are paved before the field is closed:

- **Increment 0 -- observability + inventory freshness.** The
  representation trace above; the guide's missing-cells table reconciled
  against `docs/reported/` (it is the campaign's live scoreboard, per the
  Guide-upkeep tasks each report carries).
  *Status 2026-07-30: landed (both slices).* `repr-trace` lines ship under
  `--emit-abi-trace`:
  - one per fn-typed parameter (carrier / fat / cfnptr / thin-fn + which
    gate forced it, from the `carrier_ok` decision in `elab_fns.c`);
  - one per emit-side fat bridge (bare-to-fat with shim kind,
    poly-to-fat);
  - one per carrier<->concrete crossing lowered by the
    `emit_carrier_bridge` chokepoint, with direction and the lowering form
    it picked (heap-reinterpret / inline-reinterpret / aggregate) --
    container-element crossings (Vec push/get) route through here;
  - one per aggregate heap-box/unbox at the `emit_agg_box`/`emit_agg_unbox`
    chokepoints (poly-carrier and wide-byval crossings).

  All pinned by `tests/run-repr-trace.sh` (ctest `tur_repr_trace`, 8
  classifications). Guide table reconciled (+2 rows:
  `fn-payload-in-container-undeclared-temp`, enumerations-drift Finding
  2); all 10 `--known-probes` fire on current main; suite green (2436/0)
  with the trace in. Known coverage gap, intentionally deferred: ad-hoc
  spill sites NOT routed through the named chokepoints (e.g. the
  call-site by-value->carrier-`:int`-sink spill) do not trace -- those
  sites becoming chokepoint calls is increments 2-4's job, and the trace
  will grow with them.
- **Increment 1 -- the fn-value axis.**
  [fn-value-fat-normalization-plan.md](fn-value-fat-normalization-plan.md),
  staged as written (params -> return/let/ascribe -> unify flag'd sinks).
  Highest bug density, clearest precedent (struct-field-fat), and the axis
  the last campaign deliberately deferred.
- **Increment 2 -- method-result bridging.** The carrier->concrete re-wrap
  applied uniformly to typeclass method results at every consumer position
  (typed defn boundary, generic call argument, ascription) -- closes
  `result-monad-bind-typed-boundary-miscompiles` and
  `class-method-result-into-generic-invalid-c`. The let-bind workaround
  proves the bridge exists; this increment makes consulting it total,
  routing-plan style.
  *Status 2026-07-31: first cell landed.* The generic-call-argument cell is
  CLOSED (and its report archived): the defect was inverted from the
  prediction -- not a missing bridge but a wrongly-consulted one
  (`fn_body_tail_is_carrier_producer` classified every `__inst_*` callee as
  a carrier producer by NAME; the M7 by-value instance amendment fixes it;
  suite 2444/0 in isolation, pinned by
  `tests/fixtures/class-method-result-into-generic/`). The `bind` cell is
  diagnosed one level deeper (see the report's 2026-07-31 investigation
  update): a continuation return-ABI mismatch -- the elab-side
  `boxes_aggregate` gate pairs the wrapper ABI by receiver abstractness
  while the emit-side dispatch selects the entry point; partially-applied
  instance heads (`(Result _ B)`) get the carrier base with a
  by-value-returning wrapper. Fixing it means deciding the pairing where
  the entry point is selected -- remaining work for this increment.
- **Increment 3 -- container element protocol.** One rule for what a
  container element slot holds per element class (scalar bits / heap ptr /
  spilled by-value / fat fn handle), shared by Vec, parametric ADT
  payloads, and struct fields -- closes
  `vec-byvalue-struct-element-invalid-c` and
  `fn-payload-in-container-undeclared-temp`, or (acceptably) turns the
  unrepresentable cases into real diagnostics.
- **Increment 4 -- the decision function.** Only after 1-3: collapse the
  per-site representation choices into the single `repr-of(type, position)`
  routine plus chokepoint bridges, with the R3-style Debug ICE and an
  R4-style registry + CI ratchet extended to cover the new axes. This is
  the true consolidation; it goes last because by then the sites agree in
  *behavior* and the collapse is mechanical rather than semantic.
- **Increment 5 (conditional) -- representation retirement.** If the
  decision function shows a form with no remaining (type, position) pairs
  -- the by-value fat struct in-flight form is the likely candidate --
  delete it CPS-style, with the two milestones kept explicitly separate:
  becoming the default (forced-on corpus probe, every failure classified)
  and deleting the old path (byte-identity proof, dependency sweep,
  pre-registered abort path). The redundancy-falsification probe from the
  checklist runs before either milestone starts.

## Per-increment landing checklist (the template child plans follow)

1. Assumption probes written and green (or their failures understood).
2. Blast radius measured per mechanical step, in isolation, before
   composition; numbers recorded in the plan.
3. Boundary matrix fixtures added (ok rows and broken rows) before the fix.
4. Grounding guard stated: where proof is unavailable, the old form stays
   legal (no un-grounded normalization).
5. Performance probe named, baseline recorded, acceptable delta stated.
6. Snapshot regen in the same PR; one regen window per the fixture-churn
   rule.
7. Fuzzer: known rows retired, `--known-probes` flipped to FIXED, two
   fresh-seed sessions green.
8. Representation trace diff reviewed: only the intended boundaries moved.
9. Guide updated (inventory + missing-cells table) in the same PR --
   enforced socially by each report's Guide-upkeep section.
10. Any dual-path flag carries its retirement criterion in writing: a hard
    `expires_at`-style contract (CPS) or a written rot license (M7) --
    never an open-ended coexistence.
11. For any representation slated for deletion: the redundancy-
    falsification probe ran and failed to find a load-bearing use, AND a
    fixture exists exercising the *other* side of the split it leaves
    behind.
12. Deletion states its benefit beyond tidiness (the "functionally free"
    test) -- usually the missing-cells rows it permanently closes.

## Stall-recovery rule

A revert is data, not failure. The fn-element report set the standard:
when an attempt is reverted, the measurement table and the disproven
hypothesis are written into the report *before* moving on, so the next
attempt starts from what was proven rather than re-deriving it. Any
increment of this campaign that gets reverted does the same, in its child
plan, with the revert commit referenced.

## Doc follow-up

- Each landed increment updates the representations guide (its Maintenance
  section already requires this) and this plan's sequencing status.
- When increment 4 lands, the guide's "missing cells" framing inverts: the
  table becomes the *coverage* table of the decision function, and the
  fuzzer's `known_bug_slug` table should be empty.
