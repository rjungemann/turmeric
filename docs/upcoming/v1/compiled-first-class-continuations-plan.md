---
title: First-class continuations for `async` / `await` / `handle` on the compiled backend -- Plan
category: Planning
description: Phase D3 of the retired compiled-c-crossing-tco parent. The compiled backend runs `async` / `await` on ucontext fibers (with hand-written x64 / arm64 context switches) and lowers `handle` / `perform` / `resume` directly, with a stack-allocated `TurContK` per handler invocation -- Phase 19-5 Path B, deferred CPS. That works, but it costs a fresh C stack per active fiber, keeps effect continuations one-shot, and leaves the async surface on a different substrate from the graduated multi-prompt CPS machine (`src/runtime/cps_prompt.c`) that already runs `shift` / `reset` / `call/cc*`. This plan scopes a codegen-level move of `async` / `await` / `handle` onto heap continuations, reusing the DK-prompt substrate rather than forking a new runtime.
---

# First-class continuations for `async` / `await` / `handle` -- Plan

## Why this exists

The [archived compiled-c-crossing-tco parent](../../archive/compiled-c-crossing-tco-plan.md)
resolved D1 (heap handler chain) and its D3-as-scoped-there (stackless
catch-unwind) via the 2026-07-07 graduation, and dropped D2 (atomically) as a
non-problem -- the compiled backend inlines the transaction loop, so no
per-`atomically` C frame exists to eliminate
(`src/compiler/emit_expr.c:5960`). What remained open was the parent plan's
own D3: **first-class continuations for the `async` / `await` / `handle`
surface**. That work is deliberately deferred, not blocked, and this plan
scopes it as a v1 coherence item rather than a near-term ship gate.

Two facts frame the work:

- **`async` / `await`** run on `ucontext` fibers. Each fiber owns its own C
  stack (`src/async/fiber.{h,c}`), context-switched by hand-rolled x64 / arm64
  routines (`src/async/fiber_ctx_{x64,arm64}.S`). Intra-fiber TCO is fine;
  each *active* fiber is one C stack.
- **`handle` / `perform` / `resume`** are lowered directly by
  `src/compiler/emit_effects.c`, with a fresh `TurContK { bool consumed; }`
  allocated **on the C stack** per `perform` invocation and its address
  cast to `int64_t` and passed as `k` (Phase 19-5 Path B, see
  `docs/archive/history/deferred-tasks-phase15-phase19.md:107`). The
  `consumed` flag drives runtime `(cont? k)` freshness and blocks
  double-resume; there is no captured continuation object -- resume is
  direct-style.

Meanwhile, the compiler already ships a full CPS substrate for the
*delimited-control* operators (`shift` / `reset` / `shift0` /
`cloneable-reset` / `cloneable-shift` / `serial-reset` / `serial-shift` /
`call/cc*`) via the DK multi-prompt machine
(`src/passes/cps.c`, `src/passes/cps_ir.c`, `src/runtime/cps_prompt.{h,c}`,
`src/runtime/cps_rt.{h,c}`; retired plan
`docs/archive/history/cps-transform-plan.md`). The async / effect surface is
the last piece of the language that stays on the fiber / direct-style
runtime while everything else moved onto CPS.

## Status (2026-07-08)

A codebase audit against this plan landed the two self-contained,
substrate-independent artifacts and re-scoped the substantive lowering
against what the tree actually provides. What changed and what it means:

**Landed**

- **F1 mechanical coloring -- already implemented, now locked.** The
  may-capture coloring already seeds on the effect operators: both
  `cps_expr_contains_shift` and `cps_directly_uses_control`
  (`src/passes/cps.c`) treat `EX_PERFORM` / `EX_HANDLE` / `EX_RESUME` /
  `EX_DISCONTINUE` as capture seeds, so a `perform`-reachable function is
  colored exactly like a `shift`-reachable one, and the backward
  fixed-point propagates transitively. This is the "mechanical part" the
  F1 section calls for; it needed no change. It is now pinned by
  `tests/fixtures/cps-effect-coloring/` and the `dump-cps-coloring-effects`
  assertion in `tests/run-flags.sh` (`does-perform` seed + `calls-performer`
  transitive + `has-handle` seed COLORED; the two pure fns uncolored).
- **F4 effect-rec probe -- landed.** `tests/probes/stackless-signoff/effect-rec.tur`
  drives a nested effect handler (`Outer`) around a 1,000,000-iteration
  `Tick` perform/resume loop under `ulimit -s 256`, wired into
  `tests/stackless-signoff-probes.sh` (all 6 probes pass, ~0.75s for the
  effect probe). This was the plan's explicitly-named missing probe.

**Correction to a premise.** "Each *active* fiber is one C stack" is true,
but a deep perform/resume loop does **not** grow the C stack in proportion
to depth on today's runtime: each `handle` body runs in its own fiber and
every `resume` re-enters that *same* body fiber iteratively
(`emit_effects.c` dispatch via `tur_fiber_block_resume`). So the compiled
effect path does not SIGSEGV under deep perform/resume -- it is time/memory
bound, not C-stack bound. The measured pressure point is ~1,000,000
*distinct* nested handles (a fresh handle per recursion level), which
*times out* on per-fiber allocation overhead rather than crashing. The F4
probe therefore guards the "bounded C stack" property that already holds,
so it stays green across the F1 move rather than starting red.

**Remaining work is blocked on an unbuilt prerequisite.** The plan lists
"CPS coloring + lowering pipeline (CPS1--CPS3)" and the "DK multi-prompt
substrate" as landed dependencies. Two facts narrow that:

- The DK machine *is* emitted into generated C (`emit_cps_runtime_prelude`,
  `src/compiler/emit_cps.c`) and *does* drive `shift`/`reset` -- but
  Turmeric's `shift` lowering is **abortive**: the emitted body
  `__dk_abort_body` ignores the captured sub-continuation (Turmeric `shift`
  never resumes it). There is no resumable-capture codegen path yet, which
  is exactly what `perform`/`resume` require.
- `emit_cps_reset` only lowers a **syntactically-local** delimited context
  (`emit_first_shift` walks the reset body). The normal effect shape --
  `perform` in a callee, `handle` in a caller -- is not syntactically
  visible to the handle and cannot be captured by that machinery. Capturing
  it means threading continuations across call boundaries, i.e. emitting the
  ANF/CPS IR (`src/passes/cps_ir.c`) as C. That IR is **dump-only today**
  (`--dump-cps`); the CPS-IR-to-C backend (CPS3 codegen) is unbuilt.

So F1/F2/F3's substantive lowering depends on building CPS-IR-to-C emission
for resumable cross-function capture. That is the real next step, and it is
a plan of its own -- not a mechanical extension of the existing abortive DK
path.

**Experiment registration deferred (deliberately).** Per the experiments
STRICT RULE, an `EXPERIMENTS[]` row must point `opt_global` at a
`g_opt_<name>` bool that the feature's elaboration *reads*. Until the DK
effect lowering exists there is nothing for `--enable=cps-effects` /
`--enable=cps-async` to gate, so registering them now would be a hollow
row. They should be added in the same change that lands the lowering, as
the "Experiment mechanics" section describes.

## What this plan is not

- Not a `ucontext` removal. Fibers stay a valid representation for the
  compiled backend; this is about giving the source language a
  heap-continuation option, not deleting a working one.
- Not motivated by a hit stack wall. `tests/probes/stackless-signoff/`
  contains no recursive-`async` probe today, and no user has reported
  hitting a per-fiber-stack ceiling. The driver is *substrate coherence*
  (one continuation representation across delimited + effectful control),
  not a SIGSEGV.
- Not an effect-system redesign. The source-level surface (`defeffect`,
  `handle`, `perform`, `resume`, `discontinue`, `cont?`, `async`, `await`)
  stays as-is. Only the codegen and runtime move.

## Phase F1 -- lift `handle` / `perform` / `resume` onto the DK substrate

**Goal.** Every `perform` captures a real heap continuation up to the
enclosing `handle`, not a stack-allocated `TurContK` sentinel. `resume`
invokes the captured chain; multi-shot resume is a `dk_invoke` per shot.

- Extend the CPS coloring pass (`src/passes/cps.c`, `cps_expr_contains_shift`
  and its callers) so a function reachable from a `perform` site is marked
  may-capture the same way `shift`-reachable functions are today. This is
  the mechanical part -- the pass already threads may-capture through the
  IR; add the effect ops to the traversal.
- Lower `handle` to a DK prompt push (a fresh prompt tag per handler, or
  the effect name reused as tag -- decide based on how many prompts a
  single `handle` needs). The handler body runs under the prompt; the
  handler cases become DK continuations of the prompt.
- Lower `perform` to a shift-shaped capture against the effect's prompt:
  the sub-continuation from the perform site up to the enclosing `handle`
  is reified as a `dk_kont` chain and passed to the matching handler case
  as the `k` argument.
- `resume k v` becomes `dk_invoke(k, v)`; `discontinue k v` becomes a
  DK-side `dk_invoke_with_panic` (or the equivalent -- pick after auditing
  the existing panic-return-signal machinery for the cleanest hook).
- `(cont? k)` reads the DK continuation's consumed flag directly; the
  stack-allocated `TurContK` disappears.

## Phase F2 -- deep vs shallow handlers on the same substrate

- The DK machine's re-install-on-resume vs no-reinstall distinction
  (`shift` vs `shift0`) maps onto deep vs shallow handlers directly. Confirm
  the mapping against `tests/fixtures/effect-deep-handler/` and add a
  shallow-handler probe if one is missing.
- Multi-shot resume falls out for free (DK continuations are already
  multi-shot). Update the elaborator gate that today rejects multi-shot
  effect resume to permit it under the CPS lowering, mirroring the
  `^multishot` annotation the delimited-control surface uses.

## Phase F3 -- `async` / `await` on heap continuations (optional)

- **Not a redesign of async.** Keep the fiber path as the default
  scheduler; the CPS path is an alternate representation, gated per
  compilation unit or per-function attribute at first.
- Lower `await` under CPS as a shift against an implicit `async` prompt at
  the fiber body's entry. The suspended computation becomes a `dk_kont`;
  the reactor resumes it with `dk_invoke` on completion, on any thread.
- This decouples "how many suspended computations are live" from "how many
  C stacks are alive". Directly addresses the `turi-async-fiber-stack-never-reclaimed`
  and `turi-async-await-deep-recursion-garbage` reported bugs at the
  representation level.
- Ship F3 behind an experiment (`--enable=cps-async`); F1 + F2 are
  self-contained wins and should not block on F3.

## Phase F4 -- sign-off probes

- **LANDED (2026-07-08):** `tests/probes/stackless-signoff/effect-rec.tur` --
  nested `handle` (`Outer`) around a `perform`/`resume` loop at 1,000,000
  under `ulimit -s 256`, no SIGSEGV, oracle-matched at small depth; wired
  into `tests/stackless-signoff-probes.sh` (expected `1000000`). This was
  the missing probe -- the `atom-rec` / `fiber-rec` files test catch-unwind
  carriers, not effect nesting. It is green on the current fiber runtime
  (see Status: deep perform/resume is bounded-stack today) and stays the
  regression guard for that property once F1 moves effects onto heap
  continuations.
- If F3 lands: `async-rec.tur` -- recursively-spawned suspended
  computations without proportional C-stack growth.
- Non-effect hot-path neutrality: measured neutral-or-better on the suite,
  same gate the catch-unwind graduation held itself to.

## Experiment mechanics

- F1 + F2: ship behind `--enable=cps-effects`, prototype lifecycle,
  `expires_at` two releases after landing. Graduate on F4 sign-off (effect
  probe + neutrality).
- F3: separate `--enable=cps-async` gate, independently expirable. Do not
  bundle -- F1 + F2 are the coherence win; F3 is the deeper representation
  change.

## Depends on / reuses

- **DK multi-prompt substrate** (`src/runtime/cps_prompt.{h,c}`) -- landed
  via the retired `docs/archive/history/cps-transform-plan.md`, CPS5.
- **CPS coloring + lowering pipeline** (`src/passes/cps.c`, `cps_ir.c`) --
  landed as CPS1--CPS3.
- **Graduated `panic-return-signal` transport** -- `discontinue` rides the
  same return-path-signal shape that `perform`-produced panics would.
- **Fiber runtime** stays in place as the default `async` scheduler until
  F3 measures out.

## Out of scope

- Source-level surface changes (`defeffect`, effect rows, effect
  polymorphism). This is a codegen-representation plan; the surface is
  unchanged.
- Removing the ucontext fiber code. Keep it; F3 is opt-in.
- Cross-thread continuation invocation semantics beyond what the DK
  substrate already provides. Any thread-safety widening is a follow-up.
- Retrofitting the change onto older releases. This is a v-boundary
  codegen change; land it and move forward.
