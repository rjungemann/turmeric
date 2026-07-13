---
title: async / await on heap continuations (F3)
category: Planning
status: COMPLETE + ARCHIVED 2026-07-13 -- all of F3.0-F3.5 landed behind --enable=cps-async; all three admissibility/robustness gaps resolved (gap 1 works-as-intended, gaps 2+3 fixed). The one remaining residual -- a recursive await evicts to the direct path rather than running stackless on the heap representation -- is carried forward to the successor plan docs/upcoming/v1/compiled-stackless-recursive-await-plan.md (F4). cps-async stays gated until F4 lands + graduation.
description: F3 of the first-class-continuations work. An alternate, heap-continuation representation for async / await that decouples "how many suspended computations are live" from "how many C stacks are alive." await lowers as a shift against an implicit async prompt; the suspended computation becomes a DK heap continuation the reactor resumes with dk_invoke on completion. Not a redesign of async and not a ucontext removal -- the fiber scheduler stays the default; this is a second representation behind --enable=cps-async. Split out of compiled-first-class-continuations-followups-plan.md (archived); its effect-handler sibling is compiled-shallow-handlers-plan.md (F2).
---

# async / await on heap continuations (F3)

> **ARCHIVED (2026-07-13).** F3 is complete -- async/await on the heap
> representation landed behind `--enable=cps-async`, with all three admissibility
> gaps resolved. The single remaining residual (recursive await stays on the
> direct path for stack-depth reasons) is carried into the successor plan
> [compiled-stackless-recursive-await-plan.md](../upcoming/v1/compiled-stackless-recursive-await-plan.md)
> (F4). This document is retained as the F3 record.

## Context

The scoping and substrate rationale live in the archived parent,
[compiled-first-class-continuations-plan.md](compiled-first-class-continuations-plan.md),
and the combined F2/F3 follow-up it was split from,
[compiled-first-class-continuations-followups-plan.md](compiled-first-class-continuations-followups-plan.md).
Relevant landed groundwork:

- **F1 -- landed, always-on.** `handle` / `perform` / `resume` and
  `shift` / `shift0` / `reset` lower onto the DK heap-continuation substrate
  (`src/runtime/cps_prompt.{h,c}`) via the CPS-IR-to-C backend
  (`src/compiler/emit_cps_ir.c`, `src/passes/cps_ir.{h,c}`), now always-on. The
  substrate is multi-shot (`dk_invoke` copies per resume) and supports capture to
  an implicit root prompt.
- **F2 -- DONE + graduated** (sibling plan, now archived:
  [compiled-shallow-handlers-plan.md](compiled-shallow-handlers-plan.md)).
  F3 does not depend on F2; the two are independently expirable, and F2's
  `cps-effects` experiment has already graduated. F3 gets its own `cps-async`
  gate.

The fiber runtime (`src/async/`) stays the default `async` scheduler. F3 adds an
*alternate* representation selected by a flag; it does not delete `ucontext`.

### F3.0 -- substrate proof -- LANDED 2026-07-12

Before touching codegen, the await-as-shift representation was validated on the
DK substrate (mirroring F2's substrate-first landing). `tests/cps_prompt_unit.c`
gained `cps-async-await-shift-suspend-resume`: an async body `1 + await(fut)`
under an async prompt at the body entry (its outer continuation modeled as the
scheduler = the prompt's DONE). `await` is a `dk_shift(ASYNC_TAG, body)` that
captures the rest of the body as a heap continuation, parks a **retained copy**
in a mock reactor (necessary because `dk_run_impl` frees the original `sub` once
the shift body returns), and returns a SUSPEND sentinel -- so `dk_run` unwinds to
the scheduler with **no C stack retained across the await**. On completion the
reactor resumes with `dk_invoke(subk, 41)`, replaying `1 + 41 = 42`. Passes
leak-clean under `detect_leaks=1`. No new substrate primitive was needed --
`dk_shift` / `dk_invoke` / `dk_prompt` already suffice; F3 is a lowering +
reactor-handoff change, not a substrate change. This pins the ownership rule
(F3.2): the parked `DK*` is a retained copy owned by the reactor until it
`dk_invoke`s and frees it.

## Goal

Decouple the number of live suspended computations from the number of live C
stacks: a suspended `await` costs one heap continuation chain, not one
`ucontext` fiber stack. `await` lowers under CPS as a `shift` against an implicit
`async` prompt at the fiber body's entry; the suspended computation becomes a
`DK*` (a `dk_kont`) the reactor resumes with `dk_invoke` on completion, on any
thread. Ships behind `--enable=cps-async`, per compilation unit / per-function to
start.

## F3.1 -- Lowering: `await` as `shift`

### Findings from the seam survey (2026-07-12)

Design decisions are pinned by how the current code actually works:

- **Compiled `async` is a synchronous stub.** `tur_async_fiber(fn)`
  (`emit_module.c`) just calls `fn()` and fulfills the future immediately; real
  suspension only happens in `tur_await_future`'s in-fiber branch (register
  `tur_fiber_block_resume` on `future.on_complete`, then `tur_fiber_block_yield`
  = `swapcontext`). So the `swapcontext` F3 replaces is exactly there, and by the
  time any current program reaches `(await f)` the future is already done.
- **The drive seam is `TurFuture.on_complete` `{fn, env}`.** `tur_future_fulfill`
  calls `on_complete.fn(f, value)`. F3 replaces `tur_fiber_block_resume` with a
  `dk_invoke(kont, value)` resumer on that same callback -- so the existing
  scheduler/reactor drives the resume unchanged, and **cross-thread (F3.2) comes
  free**: the resume runs on whatever thread `tur_future_fulfill` runs on.
- **The CPS IR already reifies the await continuation.** `EX_AWAIT` is delegated
  today via `CT_LETRAW` (`cps_ir.c:1521`) -- `let x = <raw tur_await_future(f)>
  in kont(x)` -- so `kont` (the rest of the async body) is already a first-class
  continuation. F3 emits a `dk_shift` capturing it instead of the raw call. No
  CPS *transform* to build; the reification exists.
- **`await` does NOT color a function today.** `cps_expr_contains_shift`
  (`cps.c:24`) lists the control ops that force CPS coloring; `EX_AWAIT`/`EX_ASYNC`
  are absent. So an async function is emitted by the *direct* emitter and its
  `await` lowers to `tur_await_future`, never reaching the CPS path. F3.1 must add
  `EX_AWAIT -> colored` under the flag.

### Implementation (gated on `g_opt_cps_async`; default byte-identical)

1. **Global + experiment.** Add `g_opt_cps_async` (`globals.{c,h}`) and register
   the `cps-async` row in `experiments.c` (F3.3) -- the row lands *with* this
   lowering (STRICT RULE).
2. **Color on await.** In `cps.c`, under the flag, treat `EX_AWAIT` as a
   coloring trigger so an async function is CPS-emitted.
3. **`CT_AWAIT` term.** Add a `CT_AWAIT { CAtom fut; }` node to
   `src/passes/cps_ir.{h,c}`; under the flag, `EX_AWAIT` builds it instead of the
   `CT_LETRAW` delegate. Its continuation is the enclosing `cur_k` (as for
   `CT_SHIFT`, which ignores `kont` and captures `cur_k` at emit). **Open
   question to resolve carefully:** the tail-position build is trivial
   (`cur_k` = the return continuation), but the bind-position continuation
   threading (`let x = await in rest`, e.g. `(+ 1 (await f))`) needs the same
   letcont wiring `CT_SHIFT` gets from an enclosing `reset` -- the first cut may
   lower only tail-position await to a shift and keep non-tail await on the
   existing delegate (safe, incremental), then extend.
4. **Emit.** In `emit_cps_ir.c`, emit `CT_AWAIT` as
   `return dk_run(dk_shift(DK_ROOT_TAG, __tur_await_body, (intptr_t)<fut>, cur_k), 0);`
   -- mirroring `emit_shift`, with the fixed runtime helper as the shift body.
5. **Runtime helper** (`emit_module.c`, and the DK copy in `emit_dk_runtime.c`
   already has `dk_invoke`/`dk_copy`): `__tur_await_body(env, subk)` -- if the
   future is resolved, `return dk_invoke(subk, f->value)` (resume inline); if
   pending, park `dk_copy(subk)` on `f->on_complete` with a `dk_invoke` resumer
   and return a SUSPEND sentinel. **First cut:** the *ready* branch is fully
   correct and is exercised by every existing async fixture (they are all
   already-done at await); the *pending* branch is the F3.2 completion point --
   until the deferred scheduler drive lands, a pending await aborts loudly rather
   than mis-fulfilling (unreachable in the current synchronous-async model).
6. **Coexistence.** `await` outside an async body, in an uncolored function, or
   (first cut) in non-tail position keeps today's fiber lowering.

### Acceptance for the F3.1 slice

Every existing async fixture (`async-await-basic`, `async-with-handler`,
`effects-async`, the two `cps-oracle-async-*`, ...) produces **identical output**
under `--enable=cps-async` (the ready-branch resume replays to the same value),
proving the representation on the real path without a mini-runtime; default
(no flag) codegen is byte-identical. The genuinely-deferred / stack-growth win is
F3.2 + the F3.5 `async-rec` probe.

### F3.1 -- LANDED 2026-07-12 (ready-branch; deferred drive is F3.2)

The codegen wiring above landed, gated on `g_opt_cps_async` (default byte-identical):

- **Global + experiment.** `g_opt_cps_async` (`globals.{c,h}`); `cps-async` row in
  `experiments.c` (introduced 0.28.2, expires 0.30.0, prototype).
- **Color on await.** `cps_directly_uses_control` (`cps.c`) -- the actual coloring
  predicate (not `cps_expr_contains_shift`) -- treats `EX_AWAIT` as a control seed
  under the flag, so an await-bearing function is CPS-emitted.
- **`CT_AWAIT` node** (`cps_ir.{h,c}`), built by `build_await` (mirrors
  `build_perform`), reached from both the tail and bind builders; `await` is
  marked non-delegatable under the flag (`safe_to_delegate`). Both tail and
  non-tail positions work -- the CPS builder reifies the continuation `rest`
  either way, so the earlier bind-position worry was moot.
- **Emit** (`emit_cps_ir.c`): `emit_await` mirrors `emit_perform`, emitting
  `dk_run(dk_shift(DK_ROOT_TAG, __tur_await_body, (intptr_t)fut, <cont>), 0)`;
  CT_AWAIT arms added to every CTerm switch (`-Wswitch` confirmed coverage).
- **Runtime** (`emit_module.c`): `__tur_await_body` -- ready future -> resume
  inline via `dk_invoke(subk, f->value)`; pending -> abort (F3.2 wires the park +
  reactor drive). Emitted in the always-on preamble (139 snapshots regenerated
  for the new helper).

**Verified:** an await-bearing colored function actually emits the shift
(`dk_shift(DK_ROOT_TAG, __tur_await_body, ...)`); every existing async fixture
(`async-await-basic`, `async-with-handler`, `effects-async`, `taskgroup-async`,
`future-combinators`, both `cps-oracle-async-*`, ...) produces **identical
output** under `--enable=cps-async`; new fixture `async-await-cps` (`43`) pins the
shift path on the compiled harness. Suites: run.sh 2112/0, run-turi 1581/0,
cps_prompt_unit 9/9. Default (no flag) codegen byte-identical.

*Remaining: F3.5 (async-rec probe), then graduate cps-async.*

## F3.2 -- Deferred suspend/resume via the `on_complete` seam -- LANDED

Landed the single-threaded deferred park/resume the runtime survey (below)
established as the reachable F3.2 for the compiled path. A **pending** `await`
now parks the captured heap continuation on the awaited future's `on_complete`
seam and suspends; completing that future resumes the parked continuation and
fulfills the async boundary's outer future -- no busy-wait, no abort.

**Implementation (simpler than the 4-step sketch below -- the direct-entry
wrapper already *is* the async boundary, so no `EX_ASYNC` codegen change was
needed):**

- **Park** (`emit_module.c`, `__tur_await_body`): a resolved future still
  resumes inline (F3.1); a **pending** future allocates a `TurAsyncPark`
  `{ DK *subk; TurFuture *outer; }`, stashes a private `dk_copy_range(subk, NULL)`
  copy, points the future's `on_complete` at `__tur_async_resume`, and raises a
  `tur_async_suspended` flag (+ `tur_async_pending_park`) instead of aborting.
  Returns a dummy value -- the boundary reads the flag, not the value.
- **Boundary** (`tur_async_fiber`): after running the body it checks the flag.
  Suspended -> thread the outer future onto the park (leave it pending); else
  fulfill inline exactly as before. **Byte-identical** for any synchronous body
  (every non-cps-async program), so the only behavioural change is the pending
  path. No `SUSPEND` sentinel value -- a flag avoids colliding with a real
  `int64` await result.
- **Resume** (`__tur_async_resume`, fired by `tur_future_fulfill -> on_complete`):
  `dk_invoke(subk, value)` replays the rest of the body; if it re-parks on a
  further pending await, the outer future is threaded onto the new park; else the
  outer future is fulfilled with the result. Frees the park copy (the F3.0
  ownership rule).

**Verified:**
- `tests/fixtures/async-await-cps-pending/` (`42`): a genuinely pending await
  parks (`future-pending?` on the outer future confirms it), then an explicit
  fulfill drives the parked `1 + await` to 42. This is the deferred path, not
  F3.1's already-resolved inline path.
- `cps-async-await-repark-chain` unit probe (`tests/cps_prompt_unit.c`, 10/10):
  two sequential pending awaits exercise the `__tur_async_resume` re-park branch
  (park -> re-park with the outer threaded through -> fulfill -> `10 + 32 = 42`)
  against the reference DK runtime.
- F3.1 `async-await-cps` (`43`) unchanged; 139 snapshots regenerated for the new
  preamble helpers; default (no-flag) codegen byte-identical.

**Known gap (tracked):** the compiled path reaches this seam only for a
**single-await** function body -- the CPS backend admits one delimited-control
op per body, so a two-await function evicts to the direct emitter
(`tur_await_future`). The re-park chaining is therefore proven at the unit level,
not yet by a compiled fixture. See
[docs/archive/cps-async-two-await-not-admitted.md](cps-async-two-await-not-admitted.md).

### Original framing / runtime survey (2026-07-12) -- reshaped F3.2

*Retained for the record: the survey that established the reachable F3.2 scope
implemented above. The "Revised F3.2" 4-step sketch that follows is superseded by
the simpler LANDED implementation (the direct-entry wrapper is the boundary; a
flag replaces the SUSPEND sentinel); it is kept only as the design trail.*

The compiled async runtime is **not** what the plan's original framing assumed:

- **A compiled program runs the EMITTED runtime**, not `src/async/scheduler.c`.
  The emitted scheduler (`TurScheduler`, `emit_module.c:7375-7688`) is
  **single-threaded** and runs `FiberBlock`s (ucontext) via
  `tur_fiber_block_resume` (swapcontext). The work-stealing MT scheduler in
  `src/async/scheduler.c` is compiled into the compiler object set and its
  same-named symbols are **shadowed** by the emitted `static` defs -- a compiled
  program never uses it. Only `src/async/reactor.c` is actually linked (via
  `stdlib/reactor.tur`).
- **`tur_async_fiber` is fully synchronous** (`emit_module.c:7866-7877`): it calls
  `fn()` inline and fulfills the future before returning. `(async fn)` never
  returns a pending future; no deferral exists.
- **`tur_future_fulfill` runs on the main thread**; nothing on the emitted path
  fulfills a future cross-thread. The only pending futures today come from
  hand-written inline-C fiber spawns (e.g. `taskgroup-async`'s `tg-spawn-async`),
  drained by `tur_await_future`'s top-level `while(!done) tur_scheduler_run_one()`
  busy-loop.
- **`(async fn)` is emitted only against the direct-style entry**, never a `__cps`
  entry (`emit_expr.c:5792-5871`).

**Consequence:** the plan's "cross-thread reactor resume" headline is **not
reachable** on the compiled path as it stands -- there is no cross-thread
fulfillment and no deferral to hook. F3.2 for the compiled runtime is really
**single-threaded deferred suspend/resume through the emitted scheduler + the
`on_complete` seam**; genuine cross-thread (and using the `src/async` MT
scheduler at all) is a separate, larger effort (un-shadow / link the MT scheduler
into compiled programs) that F3 does not require to prove the representation.

### Revised F3.2 (single-threaded deferred, emitted scheduler)

1. **Defer the async thunk as a CPS task.** Under the flag, `(async fn)` for a
   CPS-colored `fn` runs it as a heap-continuation computation: `dk_run` the
   `fn__cps` entry with a continuation that fulfills the thunk's future
   (`dk_frame(__tur_fulfill, fut, dk_done())`). If the body completes it fulfills
   `fut`; if it awaits a pending future it suspends (SUSPEND), leaving `fut`
   pending until the parked kont resumes and reaches the fulfill frame.
   (Requires selecting the `__cps` entry in `EX_ASYNC` emit -- new.)
2. **Park on `on_complete`.** `__tur_await_body`'s pending branch parks
   `dk_copy(subk)` on `fut->on_complete` with a `dk_invoke` resumer and returns
   the SUSPEND sentinel (replacing today's abort). The existing
   `tur_future_fulfill -> on_complete.fn` seam drives the resume.
3. **Top-level drive.** A `main`/entry that spawns async work drives
   `tur_scheduler_run_one` until the top-level future resolves; the parked kont
   resumes via `on_complete` when its awaited fiber/future completes.
4. **Ownership.** The reactor/fulfiller owns the parked `DK*` copy until it
   `dk_invoke`s and frees it (the F3.0 rule).

Cross-thread safety (`cps_prompt.c` has no thread-local pools -- favorable) is
retained as a note for when the MT scheduler is actually linked; it is moot for
the single-threaded emitted runtime.

## F3.3 -- Experiment gate: `cps-async`

Register `cps-async` as a live experiment -- F3's lowering is the "real lowering
to gate" the experiments STRICT RULE requires, so the row lands *with* this work:

- Row in `EXPERIMENTS[]` (`src/runtime/experiments.c`) with every descriptor
  field, `opt_global` -> `&g_opt_cps_async` (declared in
  `src/runtime/globals.{c,h}`), `plan_path` this doc, `introduced` the current
  release, `expires_at` two minor releases out, `XF_LIFECYCLE_PROTOTYPE`.
- `experiment_warn_if_used("cps-async")` at the async-lowering entry point
  (`elab_async` under the flag).
- Independent of `cps-effects` (F2) -- separately expirable, never bundled.
- Enable sources are the standard three (user config, `build.tur :experiments`,
  `--enable=cps-async`), parsed in `src/main.c` as for every other experiment.

## F3.4 -- Bugs addressed at the representation level -- SIGNED OFF

The archived reports
[turi-async-fiber-stack-never-reclaimed](history/turi-async-fiber-stack-never-reclaimed.md)
and
[turi-async-await-deep-recursion-garbage](history/turi-async-await-deep-recursion-garbage.md)
were patched on the **interpreter (turi)** fiber path; F3 removes the class at the
representation level (a suspended computation costs one heap chain, not one C
stack). Re-checked both repros under `--enable=cps-async`:

**Findings:**

- **The exact repros are interpreter bugs and remain resolved on the
  interpreter.** `tur --interpret` returns correct values at depth (`a-rec 500 ->
  500`; the resolution doc verified up to 70000). No regression.

- **On the compiled path the exact `a-rec` repro SIGSEGVs -- but identically
  with and without `--enable=cps-async`, at every depth including 1.** Root
  cause is *not* F3 and *not* depth: the repro's async lambda `(fn []
  (a-rec (- n 1)))` **captures `n`**, and `(async <capturing-closure>)` on the
  compiled path calls the closure's env pointer as a bare function pointer (data
  as code). This is a pre-existing shared-async-lowering bug, filed as
  [compiled-async-capturing-closure-segfault](compiled-async-capturing-closure-segfault.md);
  cps-async is not a regression. **(Since FIXED -- gap 3 in Graduation below; the
  `a-rec` repro now runs correctly on the compiled path.)**

- **The fiber-stack-growth class (bug 1) is structurally absent on the compiled
  cps-async path.** The emitted `tur_async_fiber` is a synchronous direct call --
  it never `mmap`s a per-async 512 KB fiber stack (that cost was interpreter-only)
  -- and a genuinely *suspended* await is a heap DK chain (F3.2), not a C stack.
  So the representation-level claim holds by construction; the O(N) mapped-stack
  growth the report measured cannot occur here.

- **Values are correct on the compiled path wherever the async shape is
  expressible:** `async-await-cps` (43), `async-await-cps-pending` (42), and a
  non-capturing deep async recursion (`sum-rec 100/500/2000` -> correct). The
  last runs via the direct ready-future path since that recursive-await shape does
  not color, but it confirms bug 2's "garbage at depth" symptom does not appear on
  the compiled path either.

**Net:** both archived defects stay fixed where they lived (the interpreter); the
compiled cps-async representation does not reintroduce either class. The one crash
surfaced is an orthogonal, pre-existing compiled-async-closure-capture bug (now
filed), not a cps-async regression.

## F3.5 -- Sign-off probes (F4 tail) -- LANDED

- **`tests/probes/stackless-signoff/async-rec.tur`** -- a 1,000,000-deep
  tail-recursive `(async (fn [] 1))` + `await` loop, run under `ulimit -s 256`,
  wired into `tests/stackless-signoff-probes.sh` (built with `--enable=cps-async`)
  beside `effect-rec`. All 7 probes pass (`go(1000000, 0) = 1000000`, rc 0, no
  SIGSEGV under the reduced stack).

  *Path caveat (the substantive F3.5 finding):* a **recursive** await evicts from
  the cps-async heap-continuation lowering to the direct emitter -- the CPS
  backend admits async coloring only for **non-recursive single-await** straight-
  line bodies. Verified across four recursive shapes (tail/non-tail, let-bound and
  arg-position await): every one emits `tur_await_future` (the direct/fiber await),
  never `dk_shift(DK_ROOT_TAG, __tur_await_body, ...)`. So this probe currently
  guards the direct await path staying flat at depth; it is the **forward guard**
  -- exactly as `effect-rec` was added before effects moved onto heap continuations
  -- for the same stacklessness once recursive await colors onto the DK substrate.
  The heap-substrate stacklessness itself is already guarded (same
  `dk_shift`/`dk_run`/`dk_invoke`) by `effect-rec` at 1M depth and by the
  `cps_prompt_unit` probes. The async closure is non-capturing because
  `(async <capturing-closure>)` SIGSEGVs today
  (docs/archive/compiled-async-capturing-closure-segfault.md).

- **Hot-path neutrality -- structural, not just measured.** `--enable=cps-async`
  changes codegen ONLY at `await` sites that color; every other function -- and
  every async program whose awaits do not color -- emits **byte-identical** C with
  and without the flag (verified: a pure `fib` program and `async-await-basic` both
  hash-identical on/off). The default (no-flag) path is unchanged, so there is no
  non-async hot path to regress. run.sh stays 2113/0.

## Graduation

Original criteria -- **all met**: the async-rec probe passes under a tight
`ulimit -s` (F3.5), both archived async bugs are clean under the flag (F3.4), and
hot-path neutrality holds structurally (F3.5).

**But graduation is deliberately HELD.** Making the heap-continuation
representation the default is premature while the compiled cps-async path admits
only a narrow subset of async shapes. The sign-off work examined three
admissibility/robustness items:

1. **Recursive await eviction -- INVESTIGATED, works-as-intended (gap closed).**
   A recursive (or branching) `await` continuation evicts to the direct emitter.
   A prototype that admitted it onto the heap lowering produced correct results
   but regressed deep **ready-future** recursion from O(1) (direct TCO loop) to
   O(N) C stack -- SIGSEGV at ~100k under a 256KB stack -- because a ready-future
   inline resume recurses through `dk_invoke` (not a tail call). Since `(async
   fn)` is synchronous, recursive await is always a ready future on the compiled
   path, so the direct TCO path is strictly better; the heap representation's
   stackless win is for *suspending* awaits (F3.2), not synchronous recursion.
   The eviction is the correct default; forcing the heap path is a regression.
   Full analysis + measurements + the (trampoline) fix direction:
   docs/reported/cps-async-recursive-await-eviction.md. The `term_core_ok`
   CT_AWAIT arm now carries a comment so this is not "re-fixed" into the O(N)
   regression.
2. **Two sequential awaits in one body evict -- FIXED**
   (docs/archive/cps-async-two-await-not-admitted.md). A new `await_cont_reset_ok`
   predicate admits a BOUNDED full CPS await continuation (a branch or a further
   sequential await, no tail call), lifted like a RESET continuation
   (`LH_RESET_CONT`); `collect_caps_rec` gained a `CT_AWAIT` case; and the entry
   wrappers now guard `dk_free(__root)` with `!tur_async_suspended` so a parked
   RESET_CONT continuation does not dangle `__root`. Two-await bodies now color
   onto the heap path, and the deferred re-park chain works **end-to-end on the
   compiled path** (not just the unit level). Regression fixtures:
   `async-await-cps-two` (ready) and `async-await-cps-repark` (two pending awaits).
   Gap-1's recursive eviction is preserved (the predicate rejects all tail calls).
3. **`(async <capturing-closure>)` SIGSEGV -- FIXED**
   (docs/archive/compiled-async-capturing-closure-segfault.md). A capturing lambda
   is a fat closure box, not a bare function pointer; the async spawn now detects a
   boxed closure (`fn_expr->type.as.fn.boxed`) and routes it to a new
   `tur_async_fiber_closure` helper that reads the thunk out of the box and invokes
   it with the box as its env. Shared with the fiber path, so both are fixed.
   Regression fixtures: `async-capturing-closure` (default) and
   `async-capturing-closure-cps`. The archived `a-rec` repro now runs correctly on
   the compiled path.

**All three items are now resolved** -- (1) works-as-intended, (2) and (3) fixed.
What is proven and landed: the await-as-shift lowering (F3.1), single-threaded
deferred suspend/resume via the `on_complete` seam (F3.2, now proven end-to-end on
the compiled path for multi-await bodies), bounded multi-await coloring (gap 2),
capturing-async spawn (gap 3), byte-identical default codegen, and the forward
guards (async-rec probe, cps_prompt_unit re-park probe). run.sh 2117/0.

`cps-async` is therefore **graduation-ready** on the original criteria (async-rec
probe under tight `ulimit -s`, archived bugs clean, hot-path neutrality, and the
admissibility/robustness items above). Graduation itself -- deleting the flag,
making the heap representation the default (or a documented per-project choice if
the fiber path is retained for a class of workloads), and adding `cps-async` to
`GRADUATED[]` for one minor line -- is a separate, deliberate step left for a
release-cut decision; the known residuals below are non-blocking.

**Known residuals (non-blocking, documented):**
- A *recursive* await stays on the direct TCO path by design (gap 1); a stackless
  *suspending* recursive await would need a trampolined DK inline resume
  (docs/reported/cps-async-recursive-await-eviction.md).
- A parked async body leaks its `__root` prompt (suspension is rare); reclaiming
  it would need resume-completion tracking.
- Mixing `await` with `perform`/`reset`/`handle` in one continuation is still out
  of scope (evicts).

## Depends on / reuses

- DK multi-prompt substrate (`src/runtime/cps_prompt.{h,c}`) and the
  CPS-IR-to-C backend (`src/compiler/emit_cps_ir.c`, `src/passes/cps_ir.{h,c}`,
  always-on) -- `await` reuses the `shift`/`reset` lowering.
- Fiber runtime (`src/async/fiber.{h,c}`, `reactor.c`, `scheduler.c`,
  `atomic_queue.c`, `io*.c`) -- stays the default `async` scheduler; F3 adds the
  alternate representation beside it and reuses the reactor's readiness plumbing.
- The experiments machinery (`src/runtime/experiments.c`, `globals.{c,h}`,
  `src/main.c` `--enable=` parsing, `experiment_warn_if_used`).

## Out of scope

- Removing the `ucontext` fiber code -- F3 is an alternate representation behind
  a flag, selected per unit/function; the fiber scheduler stays default.
- Cross-thread continuation semantics beyond what the DK substrate provides:
  F3.2 hands a kont to exactly one thread at a time (ownership transfer), it does
  not add shared concurrent invocation of a single continuation.
- Shallow effect handlers -- that is F2, in the sibling plan
  [compiled-shallow-handlers-plan.md](compiled-shallow-handlers-plan.md).
