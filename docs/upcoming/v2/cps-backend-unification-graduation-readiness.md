---
title: "CPS backend graduation readiness -- flip the default, then retire emit_cps.c"
status: proposed
parent: cps-backend-unification-plan.md
description: A measurement-grounded assessment of what remains for the CT-IR CPS backend to become the DEFAULT (graduate the `cps-backend` experiment), and how that differs from RETIRING emit_cps.c. Headline finding: with `cps-backend` forced on across the entire fixture suite, every program still builds and runs correctly -- all 278 failures are `expected.c` codegen-snapshot churn, ZERO behavior/runtime/build failures. Graduation is gated on snapshot regeneration + a maintainer decision, not on correctness; the `expires_at` 0.29.0 contract is the forcing function.
---

# CPS backend graduation readiness

## Status: milestone 1 LANDED (2026-07-11)

**The `cps-backend` experiment graduated.** The row is removed from
`EXPERIMENTS[]`, `g_opt_cps_backend` is retired (`globals.{c,h}`), and
`emit_cps_ir_try_fn` / `emit_cps_ir_program_has_emittable` run unconditionally --
the CT-IR CPS backend is the default lowering for every emittable colored
function, with the direct emitter kept as the eviction fallback.
`--enable=cps-backend` is now an accept-and-warn no-op (TUR-W0063). The flip was
verified faithful: default-off output is byte-identical to the prior
`--enable=cps-backend` output (139 `expected.c` snapshots regenerated; the 96
now-redundant `--enable=cps-backend` fixture `flags` files and 48 TUR-W0060
`expected.stderr` files were removed). **Milestone 2** (retire the direct
lowering) is the separate, larger effort sequenced in
[cps-backend-direct-lowering-removal-plan.md](cps-backend-direct-lowering-removal-plan.md).

### Correction to the "zero build failures" headline + the eviction-gate hardening

The pre-graduation forced-on probe reported "278 codegen-mismatch, **zero**
build failures." On the tree at graduation that did **not** hold: forcing the
CPS path on by default surfaced **24** failures the probe missed -- programs
where a colored function was CPS-emitted with a signature/ABI that diverges from
the carrier/dict ABI the rest of the compiler dispatches it through. These were
real, pre-existing CPS-backend bugs (identical under the old `--enable` flag),
not graduation regressions; the probe's "eviction is safe" premise had a hole:
the emittable-subset gate *admitted* shapes it should have evicted.

**20 of the 24 were closed by tightening the eviction gate** (the
`emit_cps_ir.c` admission predicates), so those functions now evict cleanly to
the direct emitter instead of emitting broken C:

- **Signature gate -> scalars only** (`sig_slot_ok` for params/return). A
  colored function whose signature contains a heap-ADT/struct HANDLE (`(Set
  cstr)`) or a by-value ADT aggregate (`tur_adt_H`) is specialized both
  concretely and through the int64 carrier/dict ABI; the CPS wrapper + `__cps`
  re-emit the concrete signature and collide on the base name (`conflicting
  types for 'run'` / `set_hyeq_hyfull`). Restricting CPS-emitted signatures to
  scalars keeps the ABI single-valued.
- **Poly-fat params evict** (`is_poly_fn` in `fn_sig_ok`/`mono_sig_ok`). A
  `tur_poly_fn_t` (rank-2) param is a multi-word aggregate the CPS slot / mono
  path types as `void*` when threaded (`void* = tur_poly_fn_t`).
- **Call-argument gate** (`call_arg_ok`). A by-value ADT arg into a cps->direct
  call would be handed as a bare struct where the callee's carrier ABI expects
  an int64 (`incompatible type for argument`); it now evicts.

Verified: no regressions (every one of the 24 was pre-existing; the gate change
only moved the 20 from "mis-emitted" to "evicted-and-correct").

A **21st** (`cps-mixed-coloring`) was closed by a separate fix: the legacy CPS3
`--cps-path` mechanism (`emit_module.c` forward-decl pass) emits its own
`void <fn>__cps(tur_cps_cont_t*, ...)` prototype, which collided with the
now-always-on cps-backend's `int64_t <fn>__cps(..., DK*)`. The CPS3 forward decl
now skips any function the cps-backend emits (`emit_cps_ir_emits_binding`), so
the superseded `--cps-path` path no longer double-declares the symbol.

The residual failures were **NOT eviction-subset gaps** -- they were CPS
*lowering/emit* bugs the gate could not address. **All three are now fixed; the
full suite is green (2142/2142).**

- `continuation-substrate` -- **RESOLVED.** The CPS backend miscompiled core
  `reset`/`shift`/`shift0` when the shift *receiver* was a capturing closure: the
  synthesized `(recv val)` delegated a raw `EX_CLOSURE` callee, which the direct
  emitter's indirect-call block cast the fat-closure ENV pointer to a bare
  function pointer and jumped into (segfault). A capturing-closure shift receiver
  now evicts to the direct shift lowering (`indirect_callee_ok`, src/passes/cps_ir.c).
  See [docs/archive/history/cps-continuation-substrate-miscompile.md](../../archive/cps-continuation-substrate-miscompile.md).
- `contract-nested` -- **RESOLVED.** A heap-join whose join body is itself a
  cps->cps tail call (`inner__cps(t0, k)`) lifted into a value-transform frame fn
  that has no `k` in scope (`'k' undeclared`). `needs_heap_join` now rejects a
  jbody containing a cps->cps tail call (`jbody_has_cps_tailcall`, emit_cps_ir.c),
  evicting the function to the direct emitter. See
  [docs/archive/history/cps-heap-join-references-enclosing-k.md](../../archive/cps-heap-join-references-enclosing-k.md).
- `hkt-stdlib-parser-instances` -- **RESOLVED.** A `CT_LETCONT` join-param SLOT
  was named by its raw `param.name` (a kebab-case `let` binder `first-results`,
  an invalid C identifier) while the join body referenced the same source binding
  via `name_for_binding` (mangled). The slot is now named via `cvar_cname`,
  matching the body. See
  [docs/archive/history/cps-delegated-binder-raw-kebab-name.md](../../archive/cps-delegated-binder-raw-kebab-name.md).

So the corrected headline is: graduation shipped with 3 pre-existing CPS
lowering/emit bugs surfaced by making the CPS path the default (all orthogonal to
the eviction subset), and **all three are now fixed** -- the full fixture suite
passes with the cps-backend always-on.

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
