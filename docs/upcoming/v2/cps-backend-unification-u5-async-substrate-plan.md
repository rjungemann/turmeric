---
title: "U5 async substrate: port await suspension onto the DK continuation machine"
status: proposed
parent: cps-backend-unification-plan.md
description: The U5 first slice (placement, not eviction) landed -- async/await delegate via CT_LETRAW so a colored function that awaits stays on the CT-IR path. The parent plan's deeper U5 goal ("port the scheduler wiring on top of the now-unified cloneable/serial base") turns out, on investigation, to be a large stackful->stackless coroutine migration that is DECOUPLED from the emit_cps.c retirement the rest of the plan drives. This note records the actual async architecture, corrects the inventory's inaccurate "rides cloneable/serial" claim, scopes the real port, and flags the open decision of whether it should happen at all.
---

# U5 async substrate -- port await onto the DK machine (or not)

## What actually landed (placement)

`async` / `await` do **not** color a function (they are absent from
`cps_expr_contains_shift`) and lower to self-contained runtime calls
(`tur_async_fiber` / `tur_await_future`) that do not thread the caller's DK
continuation. Eviction only bit the *mix* case (a function colored by an
effect/shift that also awaits). The first slice makes `EX_ASYNC` / `EX_AWAIT`
delegatable via `CT_LETRAW` so the colored parts stay on the CT-IR DK machine
while the async region delegates to the proven fiber runtime. Oracle:
`cps-oracle-async-placement`. That is done and correct.

## The actual async architecture (correcting the inventory)

The U0 inventory (section 5) says async/await "*rides the cloneable/serial
machinery*." **This is not accurate.** The async runtime, emitted in
`emit_module.c` (not `emit_cps.c`), is a **stackful cooperative fiber** system,
entirely separate from the DK multi-prompt machine that reset/shift, cloneable,
and serial use:

- **`FiberBlock`** (`emit_module.c:~6795`) embeds two `ucontext_t` (`ctx`,
  `caller_ctx`) -- stackful coroutines via `makecontext` / `swapcontext`.
  `tur_current_fiber` is thread-local.
- **`TurFuture`** (`:~7567`) carries a `status` / `value` / `error`, a backing
  `FiberBlock *fiber`, and an `on_complete` callback.
- **`tur_async_fiber(fn)`** (`:~7613`) creates a future; the current v1 runs `fn()`
  synchronously and fulfills the future immediately (the scheduler exists but the
  common path is eager).
- **`tur_await_future(f)`** (`:~7627`): if the future is done, return its value;
  else, inside a fiber, register `on_complete = tur_fiber_block_resume(self)` and
  `tur_fiber_block_yield(0)` (swapcontext back to the caller); outside a fiber,
  `while (!done) tur_scheduler_run_one()`.
- **`TurScheduler`** (`:~7121`) is a `FiberBlock**` run-queue; the whole
  concurrency surface -- channels, `select`, timers, the async HTTP servers --
  rides these fibers, and `tur_effect_perform` even consults
  `tur_current_fiber->effect_handler_chain` for fiber-local handlers.

So the codebase has **two independent continuation mechanisms**: the **DK machine**
(stackless, heap-reified continuations: reset/shift, cloneable, serial, effects)
and the **fiber runtime** (stackful, `ucontext`: async/await and all of
concurrency). Async is the one that does *not* touch the DK machine.

## What "port onto the cloneable/serial base" would mean

Making `await` suspend on the DK machine instead of a `ucontext` fiber is a
**stackful -> stackless coroutine migration**:

1. **Coloring.** A stackless suspend can only capture a *reified* continuation, so
   any function that can suspend at an `await` must be CPS-transformed -- i.e.
   `await` (and transitively its callers up to the async boundary) must **color**
   the function. Today `await` colors nothing; this is the load-bearing change and
   the one with the widest blast radius (every `await` site, and every function on
   a call path between an `async` spawn and its `await`).
2. **await lowering.** On a pending future, `dk_shift` the current continuation,
   store it in `future->on_complete` as a resumable DK continuation (a
   `cloneable_cont` / DK chain), and `dk_run` it when the future fulfills --
   replacing `tur_fiber_block_yield` / `tur_fiber_block_resume`.
3. **Scheduler.** The run-queue holds resumable DK continuations instead of
   `FiberBlock*`; `tur_scheduler_run_one` invokes a continuation instead of
   `swapcontext`.
4. **The rest of concurrency.** Channels, `select`, timers, and the async servers
   all block by yielding a fiber. Each blocking primitive would need the same
   DK-continuation treatment, or a bridge, or it stays on fibers and the two
   worlds coexist (defeating the unification).

## The key decoupling: this is NOT on the emit_cps.c-retirement path

The parent plan exists to collapse **two CPS lowerings** (`emit_cps.c` and the
CT-IR backend) into one so `emit_cps.c` can be deleted (U6/U7). The async fiber
runtime lives in **`emit_module.c`** and is untouched by that deletion:
`emit_cps_program_uses_*` gates nothing async, and no async code routes through
`emit_cps.c`. Therefore:

- **Retiring `emit_cps.c` (U7) does not require porting async.** U5's placement
  slice (async stays CPS-emittable, delegated) is all U7 needs from async.
- The async-onto-DK migration is a *separate* "one continuation machine" goal
  that the parent plan lumped into U5 on the (mistaken) premise that async already
  rides cloneable/serial. It should be tracked as its own initiative, gated on its
  own justification, not as a blocker for the unification's finish line.

## Is it even desirable? (open decision)

Stackful and stackless coroutines are a genuine engineering trade, not an obvious
win either way:

| | stackful fibers (today) | stackless DK (proposed) |
|---|---|---|
| surface | `await` anywhere, no coloring, transparent across call frames | every awaiting fn must be colored / CPS-transformed |
| runtime cost | per-fiber `ucontext` stack, `swapcontext` | heap continuation nodes, no separate stacks |
| unification | second mechanism to maintain | one machine for all suspension |
| interop with effects | fiber-local handler chain | DK handler prompts (already unified) |
| migration risk | -- | high: touches all of concurrency |

The strongest argument *for* the port is conceptual economy (one suspension
machine, effects/async/delimited-control all on DK) and shedding the `ucontext`
portability tax. The strongest argument *against* is that stackful fibers give
`await`-anywhere without coloring, which is a real ergonomic property the surface
language currently enjoys, and the migration risk spans the entire concurrency
runtime.

**Recommendation:** treat the async-onto-DK migration as **out of scope for the
cps-backend-unification finish line.** Land U6/U7 (retire `emit_cps.c`) on the
strength of the placement slice, and pursue the async substrate migration -- if at
all -- as an independent project with its own design review, because its value
(one continuation machine) and its cost (recoloring + concurrency-wide rewrite)
are both independent of `emit_cps.c`.

## If pursued anyway -- a safe staging

Each step is independently shippable behind the delegation:

1. **await of a resolved future, natively.** When the awaited future is already
   done (the common eager-`tur_async_fiber` path), `await` is just
   `tur_future_get` -- no suspension, no coloring. Emit that inline in CT-IR
   (a `CT_LETPRIM`-like read) instead of delegating; verify `direct == cps` on
   `cps-oracle-async-basic` (which hits exactly this path).
2. **await-with-suspension in an already-colored fn.** For a function already
   colored (by an effect/shift), lower a pending `await` to a `dk_shift` that
   stores the continuation in `on_complete`, keeping the fiber scheduler as the
   driver but resuming a DK continuation instead of a `swapcontext`. Bridge, don't
   replace, the scheduler.
3. **Coloring async fns.** Add `EX_AWAIT` to `cps_expr_contains_shift` behind an
   experiment flag; measure the blast radius (how many fns recolor) before
   committing.
4. **Migrate the blocking primitives** (channel recv, `select`, timer) one at a
   time to DK-continuation waiters, retiring `FiberBlock` last.

Steps 1-2 are bounded and low-risk and could land opportunistically; steps 3-4
are the genuine migration and should not start without the decision above.

## Non-goals

- Not a prerequisite for U6/U7 -- see the decoupling section.
- Not a change to the async *surface* (`async` / `await` stay as they are).
- Not touching the U5 placement slice, which is the correct interim regardless of
  whether the substrate is ever ported.
