---
title: "CPS backend graduation readiness -- flip the default, then retire emit_cps.c"
status: proposed
parent: cps-backend-unification-plan.md
description: A measurement-grounded assessment of what remains for the CT-IR CPS backend to become the DEFAULT (graduate the `cps-backend` experiment), and how that differs from RETIRING emit_cps.c. Headline finding: with `cps-backend` forced on across the entire fixture suite, every program still builds and runs correctly -- all 278 failures are `expected.c` codegen-snapshot churn, ZERO behavior/runtime/build failures. Graduation is gated on snapshot regeneration + a maintainer decision, not on correctness; the `expires_at` 0.29.0 contract is the forcing function.
---

# CPS backend graduation readiness

## Two distinct milestones (do not conflate)

1. **Graduate the experiment ("become the default").** Make `emit_cps_ir_try_fn`
   run for colored functions WITHOUT `--enable=cps-backend` -- delete the gate row
   in `src/runtime/experiments.c`, feature goes always-on. The direct emitter
   (`emit_cps.c`) **stays** as the fallback for anything the CT-IR backend evicts
   (non-scalar values, owning pointers, Tier C crossings, uncolored/`main`/exported
   functions). This is a small, reversible change gated on correctness + snapshots.

2. **Retire `emit_cps.c` (U7 delete).** Remove the direct lowering entirely. This
   needs the CT-IR backend to cover **100%** of colored functions AND the
   delimited-control shapes in uncolored/`main`/exported functions (which today
   take the direct path and cannot take the `f__cps(args, DK*)` + wrapper shape).
   Much larger; gated on the emittable-subset gap plans below.

"Becoming the default" is milestone 1. It does NOT require milestone 2.

## Headline measurement: correctness is essentially proven

Forcing `cps-backend` on for the WHOLE suite (seed `:enable [cps-backend]` into the
run's isolated `XDG_CONFIG_HOME`; the suite normally isolates itself from user
config, so this is a one-off probe, reverted) gives:

- **2142 fixtures, 278 FAIL -- and every single FAIL is `codegen mismatch`.**
- **Zero** stdout/stderr/behavior mismatches, zero build failures, zero crashes,
  zero timeouts.

Every program still **builds and runs correctly** with the CPS backend driving
every colored function. The 278 failures are purely `expected.c` snapshots, which
were baselined against the direct backend and legitimately change when a colored
function emits the CT-IR CPS form instead. Behaviour is identical.

This is the load-bearing graduation signal: the backend is behaviourally correct
across the whole corpus, not just the opt-in `cps-oracle-*` fixtures.

## Coverage (eviction) is safe, and mostly native

On a stdlib-heavy program (reset/shift + call/cc + cloneable, loading
`stdlib/workflow.tur`), ~**38 of 55** colored functions emit natively (~69%); the
rest **evict to the direct emitter** -- which is correct (that is the fallback) and
is exactly why the behavior tests all pass. Eviction causes are the emittable-subset
gaps, each with its own plan already on file:

- `cps-backend-non-scalar-values-plan.md`
- `cps-backend-owning-pointers-plan.md`
- `cps-backend-tier-c-crossing-plan.md`
- `cps-backend-effectful-callbacks-plan.md`
- `cps-backend-generic-monomorph-classification-plan.md`

These raise the native fraction; none is a correctness blocker for graduation
(evicted functions stay correct via the direct fallback). They are how milestone 1
delivers more *value* and how milestone 2 becomes *possible*.

## The forcing function

`cps-backend` is `XF_LIFECYCLE_PROTOTYPE`, introduced 0.27.1, **`expires_at`
0.29.0** (current tree: 0.27.6). `expires_at` is a hard contract -- the release-cut
skills refuse to bump past it until the row is graduated (deleted; feature always-on)
or shelved. So the decision is due within ~2 minor releases: graduate, or shelve.

## Sequenced next steps toward graduation (milestone 1)

1. **Broaden the correctness proof.** The 278-vs-0 result is one build/platform.
   Before flipping: (a) run the same forced-on probe under the Release build and,
   if available, a second OS/arch; (b) keep the `cps-oracle-*` net green (it already
   pins direct == cps per family). Low effort, high confidence.

2. **Decide the snapshot policy** for the 278 churned `expected.c`. Either
   (a) regenerate them against the CPS backend as part of the flip (one coordinated
   commit -- see CLAUDE.md's fixture-regen recipe), or (b) keep the direct backend
   as the snapshot baseline and exclude colored-fn fixtures from the emit-c compare.
   (a) is cleaner and matches the "CPS is the default" story; it is mechanical but
   large, so land it in the same PR as the flip to avoid a half-migrated tree.

3. **Perf / code-size spot check.** The CPS path emits heap-reified continuations;
   confirm no unacceptable code-size or runtime regression on a few representative
   programs (the plan's "emit only what's used" risk). If a regression shows,
   it gates the flip, not the correctness.

4. **Flip the gate.** Delete the `cps-backend` row from `EXPERIMENTS[]` and make
   `emit_cps_ir_try_fn` / `emit_cps_ir_program_has_emittable` unconditional (drop the
   `g_opt_cps_backend` guard). Direct stays as fallback. Reversible via revert.

5. **Then, separately, pursue milestone 2** (retire `emit_cps.c`) by working the
   emittable-subset gap plans until eviction reaches zero for colored functions and
   the delimited-control families are handled in uncolored/`main`/exported code --
   at which point U7's delete has no callers left.

## Recommendation

The correctness bar for graduation is **met** (measured, not assumed). The real
remaining work for "become the default" is **snapshot regeneration + a perf spot
check + the maintainer flip decision** -- not more native coverage. Native-coverage
work (the eviction plans) is valuable but belongs to milestone 2 (retirement) and to
maximizing the flip's payoff, not to unblocking the flip itself. Given the 0.29.0
expiry, the flip should be scheduled deliberately rather than allowed to lapse into
a shelve.
