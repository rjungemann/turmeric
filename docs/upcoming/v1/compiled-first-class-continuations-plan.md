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

- **Add** `tests/probes/stackless-signoff/effect-rec.tur`: nested `handle`
  around a recursive `perform`/`resume` loop at 1,000,000 under
  `ulimit -s 256`, no SIGSEGV, oracle-matched at small depth. This is the
  missing probe -- the current `atom-rec` / `fiber-rec` files test
  catch-unwind carriers, not effect nesting.
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
