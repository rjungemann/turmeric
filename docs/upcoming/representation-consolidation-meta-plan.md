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

## Why past efforts stalled -- and why two succeeded

The archive holds both outcomes; the meta-plan is built from the deltas.

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
no longer a second suite." Dualism was a phase with an exit, not a
steady state.

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

## The probe discipline

"Probe" here follows the house usage (`tests/shallow-handler-probes.sh`,
`stackless-signoff-probes.sh`, the refine plans' RE probes): a small,
targeted, runnable check that pins one *assumption* -- distinct from a
fixture, which pins one *behavior*. Probes are how a migration learns
whether its premises hold before it bets the tree on them. This campaign
uses four kinds:

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
4. **Performance probes.** Each child plan names the benchmark(s) that
   actually exercise its seam (`benchmarks/`, `tur run bench` -- e.g.
   `bench-poly-specialize.tur` for dispatch seams, closure-heavy benches
   for the fn axis), records before/after on the same box, and states its
   acceptable delta *in the plan before landing*. A regression outside the
   stated envelope is a stop-and-redesign signal, not a note in the PR.

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
  delete it, M7-style: dual-path flag, default flip, scheduled retirement.

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
10. Any dual-path flag carries its retirement criterion in writing.

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
