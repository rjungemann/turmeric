---
title: First-class continuations -- deep/shallow handlers + async on heap continuations (F2/F3)
category: Planning
status: open (F1/F4 landed; F2 substrate + cps-effects gate landed; F2 fiber-path shallow + F3 remaining)
description: F1 (lift handle / perform / resume onto the DK heap-continuation substrate) landed as the CPS-IR-to-C backend's Phase C4 and graduated with cps-backend always-on; F4's effect-rec sign-off probe landed. F2's substrate slice landed -- shallow handlers on the DK machine (dk_handler_shallow, no reinstall = shift0-twin), a handle-shallow surface behind --enable=cps-effects lowered by the CPS/DK backend, and the cps-effects experiment row. What remains is extending shallow to the deep-only fiber path (the cps-effects graduation gate), F3 (async / await on heap continuations, optional, behind --enable=cps-async), and multi-shot default relaxation (kept guarded by the O3 owning-capture hazard).
---

# First-class continuations -- remaining follow-ups (F2 / F3)

## Context

The scoping, the substrate rationale, and the landed F1/F4 work live in the
archived parent,
[compiled-first-class-continuations-plan.md](../../archive/compiled-first-class-continuations-plan.md).
Summary of where things stand:

- **F1 -- landed.** `handle` / `perform` / `resume` lower onto the DK
  heap-continuation substrate as `CT_HANDLE` / `CT_PERFORM` / `CT_RESUME`
  (`src/compiler/emit_cps_ir.c`); a `perform` captures a real sub-continuation up
  to the enclosing `handle` and `resume` invokes it via the DK machine, replacing
  the stack-allocated `TurContK` sentinel for the CPS-emitted subset. This shipped
  as the CPS-IR-to-C backend's Phase C4 and is now always-on (the `cps-backend`
  flag graduated, commit `d50dd2b89`). Fixture `tests/fixtures/cps-backend-effect/`.
  The parent plan's "blocked on an unbuilt CPS-IR-to-C backend" premise is now
  resolved -- that backend exists.
- **F4 effect-rec probe -- landed.** `tests/probes/stackless-signoff/effect-rec.tur`
  (nested `Outer` handler around a 1,000,000-iteration `Tick` perform/resume loop
  under `ulimit -s 256`), wired into `tests/stackless-signoff-probes.sh`.

The fiber runtime stays the default `async` scheduler; none of the below deletes
it. This remains a *substrate-coherence* item, not a stack-wall fix.

## Phase F2 -- deep vs shallow handlers on the DK substrate

**Goal.** Deep and shallow handlers share the one DK substrate, and multi-shot
effect resume is permitted where the source opts in.

### Landed (substrate slice)

- **Reinstall-on-resume vs no-reinstall mapped onto deep vs shallow.**
  `dk_perform` (`src/runtime/cps_prompt.c`, mirrored in the emitted preamble
  `src/compiler/emit_dk_runtime.c`) now branches on a `shallow` bit on the
  handler node: deep re-installs the handler on the captured sub-continuation
  (a resume re-delimits); shallow appends `dk_done()` instead (the resume runs
  outside the handler) -- the handler-side twin of the `shift`/`shift0` branch
  already in `dk_run_impl`. Constructed via `dk_handler` vs `dk_handler_shallow`.
- **Substrate probe.** `tests/cps_prompt_unit.c` pins the distinction directly
  where it is observable (`dk-deep-handler-reinstall` /
  `dk-shallow-handler-no-reinstall`), alongside the existing shift/shift0
  reinstall tests. The deep mapping is also confirmed against
  `tests/fixtures/effect-deep-handler/`.
- **Surface + gate.** `handle-shallow` (elaborated by `elab_handle_impl` with
  `shallow=true`) is gated behind `--enable=cps-effects` (`g_opt_cps_effects`),
  threaded `HandleExpr.shallow -> CT_HANDLE.handle.shallow`, and lowered by the
  CPS/DK backend (`emit_cps_ir.c` `emit_handle` emits `dk_handler_shallow`).
  Fixtures: `tests/fixtures/effect-shallow-handler/`,
  `tests/fixtures/errors/effect-shallow-no-flag/`.

### Remaining (graduation gate)

- **Fiber-path shallow.** The CPS/DK backend only fires for CPS-eligible handles
  (single tail `perform`), where deep and shallow are behaviourally identical.
  Every shape that makes the difference *observable* (>=2 performs) falls to the
  deep-only fiber path, which currently **rejects** a shallow handle rather than
  miscompiling it (`emit_effects_handle`, `h->shallow` guard;
  `tests/fixtures/errors/effect-shallow-fiber-shape/`). Teaching the fiber
  dispatch loop to pop a shallow handler frame before the resumed fiber
  continues is the `cps-effects` graduation gate. See
  `docs/reported/shallow-handlers-fiber-path-not-supported.md`.

### Multi-shot effect resume

- Multi-shot resume already works where the source opts in via `^multishot`
  (`CK_MULTISHOT`, exempt from the `cont_check_double_use` gate in
  `elab_effects.c`; DK continuations are inherently multi-shot via `dk_invoke`'s
  copy). The un-annotated default stays affine (`CK_UNIQUE`, TUR-E0201 on a
  second resume): relaxing it globally is **not** done here.
- **Interaction with the owning/capture cut:** a multi-shot effect resume that
  captures an owning value is the O3 hazard from the owning-pointers follow-ups --
  the default stays guarded (and the `^multishot` MS2 capture check, TUR-E0500,
  rejects capturing a non-copyable value) until the env-capture / deep-clone
  story lands.

## Phase F3 -- `async` / `await` on heap continuations (optional)

**Status: not started** (optional; must not block F2). The fiber scheduler stays
the default and no `cps-async` lowering exists yet, so no experiment row lands
until it does (experiments STRICT RULE).

**Goal.** An alternate, heap-continuation representation for `async` / `await`
that decouples "how many suspended computations are live" from "how many C stacks
are alive." **Not** a redesign of async and **not** a `ucontext` removal -- the
fiber scheduler stays the default.

- Lower `await` under CPS as a shift against an implicit `async` prompt at the
  fiber body's entry. The suspended computation becomes a `dk_kont`; the reactor
  resumes it with `dk_invoke` on completion, on any thread.
- Gate per compilation unit or per-function attribute at first.
- Addresses the reported `turi-async-fiber-stack-never-reclaimed` and
  `turi-async-await-deep-recursion-garbage` bugs at the representation level.
- Ship **after** F1 + F2 -- those are the self-contained coherence win; F3 is the
  deeper representation change and must not block them.

## Experiment registration (was deferred; F1 now unblocks it)

Per the experiments STRICT RULE, an `EXPERIMENTS[]` row's `opt_global` must point
at a `g_opt_<name>` bool the feature's elaboration reads -- so a row may only land
once there is real lowering to gate. F1's lowering now exists, so:

- **`cps-effects`** (F2): **registered** as a live `EXPERIMENTS[]` row in
  `src/runtime/experiments.c` (all descriptor fields; `g_opt_cps_effects` read by
  `elab_handle_impl`; `plan_path` -> this doc; `experiment_warn_if_used(
  "cps-effects")` at the shallow-handle entry point; `introduced` 0.29.0,
  `expires_at` 0.31.0). The **first F2 sub-task -- whether effects need their own
  gate -- resolved as: deep `handle` stays always-on (it rides the graduated
  `cps-backend` path), and only the *new* `handle-shallow` surface is gated by
  `cps-effects`.** So the row is live (not a doc-only note): the flag gates real
  elaboration. Graduate on the fiber-path shallow extension + hot-path
  neutrality.
- **`cps-async`** (F3): a separate, independently-expirable
  `--enable=cps-async` gate. Do not bundle with `cps-effects`. **Not yet
  registered** -- per the experiments STRICT RULE a row lands only once there is
  real lowering to gate, and F3 lowering is not built.

## Phase F4 tail -- sign-off probes (if F3 lands)

- `async-rec.tur` -- recursively-spawned suspended computations without
  proportional C-stack growth.
- Non-effect hot-path neutrality: measured neutral-or-better on the suite (the
  same gate the catch-unwind graduation held itself to).

## Depends on / reuses

- DK multi-prompt substrate (`src/runtime/cps_prompt.{h,c}`) and the CPS-IR-to-C
  backend (`src/compiler/emit_cps_ir.c`, now always-on).
- The `^multishot` delimited-control annotation and its elaborator gate.
- Fiber runtime (`src/async/fiber.{h,c}`) -- stays the default `async` scheduler
  until F3 measures out.

## Out of scope

- Source-level surface changes (`defeffect`, effect rows, effect polymorphism).
- Removing the `ucontext` fiber code -- F3 is opt-in.
- Cross-thread continuation semantics beyond what the DK substrate already
  provides.
