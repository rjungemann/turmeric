---
title: First-class continuations -- deep/shallow handlers + async on heap continuations (F2/F3)
category: Planning
status: open (F1/F4 landed; F2 next, F3 optional)
description: F1 (lift handle / perform / resume onto the DK heap-continuation substrate) landed as the CPS-IR-to-C backend's Phase C4 and graduated with cps-backend always-on; F4's effect-rec sign-off probe landed. What remains of the first-class-continuations work is F2 (deep vs shallow handlers on the same substrate, plus multi-shot effect resume), F3 (async / await on heap continuations, optional, behind --enable=cps-async), and the deferred experiment registration those two need.
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

- Map the DK machine's reinstall-on-resume vs no-reinstall distinction (`shift`
  vs `shift0`) onto deep vs shallow handlers. Confirm the mapping against
  `tests/fixtures/effect-deep-handler/`; add a shallow-handler fixture/probe if
  one is missing.
- Multi-shot resume falls out for free (DK continuations are already multi-shot).
  Update the elaborator gate that today rejects multi-shot effect resume to permit
  it under the CPS lowering, mirroring the `^multishot` annotation the
  delimited-control surface already uses.
- **Interaction with the owning/capture cut:** a multi-shot effect resume that
  captures an owning value is the O3 hazard from the owning-pointers follow-ups --
  keep it behind the same guard until the env-capture / deep-clone story lands.

## Phase F3 -- `async` / `await` on heap continuations (optional)

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

- **`cps-effects`** (F1 + F2): register the `EXPERIMENTS[]` row in
  `src/runtime/experiments.c` with all descriptor fields, a `g_opt_cps_effects`
  the effect elaboration reads, `plan_path` pointing at this doc,
  `experiment_warn_if_used("cps-effects")` at the entry point, and an `expires_at`
  two releases out. Graduate on F2 sign-off + hot-path neutrality.
- **`cps-async`** (F3): a separate, independently-expirable
  `--enable=cps-async` gate. Do not bundle with `cps-effects`.

(Note: F1's effect lowering currently rides the graduated `cps-backend` path
rather than a dedicated `cps-effects` gate. Deciding whether F2 needs its own gate
or extends the always-on path is the first F2 sub-task; if effects stay always-on,
`cps-effects` may collapse into a doc-only note rather than a live row.)

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
