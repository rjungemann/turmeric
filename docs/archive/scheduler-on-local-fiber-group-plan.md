# Plan: Rewrite `src/async/scheduler.c` on `LocalFiberGroup` (F9 follow-up)

> **Status:** Not started; design decisions resolved (see "Resolved design
> decisions" below). This is the F9 follow-up unblocked by the now-shipped
> `reactor-run-fibers` building block (see
> `docs/archive/history/reactor-run-fibers-plan.md`, F1-F8).
> **Last Updated:** 2026-05-31
> **Type:** Runtime refactor (internal); no public API change intended
> **Related:**
> - `docs/archive/history/reactor-run-fibers-plan.md` -- delivered the `LocalFiberGroup`
>   driver this plan builds on (F1-F8); F9 was anchored there as out-of-scope
> - `src/async/reactor.c` -- houses `LocalFiberGroup` + the fiber pump + park bridge
> - `src/async/scheduler.c`, `src/async/scheduler.h` -- the global work-stealing
>   scheduler being rewritten
> - `src/async/fiber.c`, `src/async/fiber_ctx_*.S` -- shared stackful-coroutine primitive
> - `stdlib/async_socket.tur`, `stdlib/async_file.tur`, `stdlib/async_pipe.tur`
>   -- fiber-blocking I/O whose park path is retargeted here
> - `docs/guides/async-await-guide.md` -- the public `spawn`/`await` surface (must not change)
> - `docs/guides/reactor-guide.md` -- Style 3 documents the local-group layer

---

## Overview

`reactor-run-fibers` (shipped) is a local fiber driver: it pumps a
`LocalFiberGroup`'s ready-queue between `reactor-poll` calls and maps
fiber park/unpark onto reactor sources. The parent plan called out that
once this driver existed, the process-wide cooperative scheduler in
`src/async/scheduler.c` could be re-expressed as **"the process-global
reactor's fiber driver"** -- a `Reactor` + `LocalFiberGroup` pair per
worker thread -- instead of carrying its own bespoke I/O-waiter list and
park/unpark machinery.

This plan does that rewrite. It is deliberately an **internal** refactor:
the user-visible `spawn` / `await` surface, the `async-socket-*` /
`async-file-*` / `async-pipe-*` signatures, and the
`tur_scheduler_mt_*` C entry points all keep their current contracts.
Only the implementation underneath changes.

The payoff is one fiber driver instead of two: a single park/unpark
implementation, a single I/O-readiness path (the reactor's), and a clear
story for "reactor and scheduler on the same thread" that today is
ad-hoc.

---

## Why this is its own plan

The building block (F1-F8) was a self-contained, well-tested stdlib
addition with no risk to the global scheduler. The scheduler rewrite, by
contrast:

- Touches the hottest, most concurrency-sensitive code in the runtime
  (the work-stealing deques, cross-thread submission, the I/O-waiter
  list).
- Has to preserve multi-threaded work-stealing, which a single
  `LocalFiberGroup` (flat, single-thread bag of fibers) does **not**
  provide on its own -- so the mapping is "one reactor + one group per
  worker thread", not "one global group".
- Must keep `spawn`/`await` bit-compatible while swapping the layer
  beneath them, which means a careful, staged migration with the old and
  new paths coexisting behind a flag during bring-up.

Splitting it out keeps the shipped local-group layer stable and lets the
scheduler migration land incrementally.

---

## Current state (what we are replacing)

`src/async/scheduler.c` today has:

- `WorkStealingDeque` per worker thread + an `AtomicQueue` global queue
  for cross-thread submission (`tur_scheduler_mt_spawn`).
- Its **own** `IOBackend` (`s->io_backend`) plus a hand-rolled
  `struct IOWaiter` linked list and `scheduler_mt_io_callback` to unpark
  fibers on fd readiness (`tur_scheduler_mt_io_wait` /
  `_io_modify` / `_io_unregister`).
- `tur_scheduler_mt_park` / `_unpark` that are currently **stubs**
  (`park` is a no-op; `unpark` just re-spawns the fiber), with the real
  resume happening through `tur_fiber_block_resume` in generated code.
- A per-scheduler `TurTimerWheel` (T24) for timers.

The reactor already provides equivalents for the I/O-waiter list
(`reactor-add-fd` + one-shot removal), timers (`reactor-add-timer` /
`-add-interval`, replacing the timer wheel for this path), and a clean
park/unpark bridge (`local-park-fd` / `local-park-chan`). The rewrite
folds the scheduler's bespoke versions into those.

---

## Target architecture

Per worker thread:

```
  thread N:  TurReactor r_N  +  LocalFiberGroup g_N (bound to r_N)
```

- **`spawn`** desugars to `local-spawn` against the current thread's
  group `g_N` (with the global `AtomicQueue` retained only for the
  *cross-thread* submission case -- pushing onto another thread's group,
  see "Cross-thread submission" below).
- **`await` / fiber-blocking I/O** desugars to `local-park-fd` /
  `local-park-chan` against `g_N`. The scheduler's `IOWaiter` list and
  `scheduler_mt_io_callback` are deleted; the reactor owns readiness.
- **Worker loop** becomes `reactor-run-fibers(g_N)` instead of the
  hand-rolled pop/steal/poll loop. Work-stealing is layered on top by
  letting an idle worker pull a fiber from another thread's submission
  queue and `local-spawn` it locally (the stolen fiber re-binds to the
  stealing thread's group, same as today's deque steal re-runs it on the
  thief).
- **Timers** for the scheduler path move onto the reactor
  (`reactor-add-timer`); the standalone `TurTimerWheel` is retired for
  this consumer (kept only if another caller still needs it).

The key invariant preserved: a fiber only ever runs on one thread at a
time, and park/unpark is one-shot. `LocalFiberGroup` already enforces
this per thread; cross-thread handoff stays explicit (channels +
`reactor-wake`, exactly as the reactor/scheduler threading model already
requires).

---

## Resolved design decisions

Resolved 2026-05-31. Each item below was the corresponding open question;
the decision is what the implementation should target.

- **Work-stealing scope -- un-started fibers only.** An idle worker pulls
  a not-yet-started fiber from another thread's submission queue and
  `local-spawn`s it on itself; only the body closure + user-data moves.
  Parked fibers stay on their owning thread and wake on that thread's
  reactor. This avoids cross-thread reactor-source mutation, which would
  be required to migrate a parked fiber's fd/timer/chan registration.
- **Cross-thread submission -- repurpose `AtomicQueue` as a per-group
  inbox.** Each `LocalFiberGroup` gains an `AtomicQueue` inbox (the
  existing primitive in `src/async/atomic_queue.{h,c}`, already a bounded
  MPMC ring and already TSan-audited). A new `local-submit(g, fiber)`
  pushes onto `g.inbox` and calls `reactor-wake(g.reactor)`; the top of
  each `reactor-run-fibers` tick drains the inbox into the local ready
  queue before pumping. Cheapest path that does not introduce a new
  primitive; a bespoke MPSC ring is a possible later optimization if F9-2
  benchmarks show submission is hot.
- **Effect-handler chain -- snapshot at spawn time.** `local-spawn` (and
  `local-submit`) must copy the *spawning* thread's
  `global_effect_handler_chain` into the new `TurFiber.effect_handler_chain`,
  matching what `tur_fiber_block_new` already does for scheduler-spawned
  fibers (`emit_module.c:2423`). This also closes an existing gap:
  `tur_local_spawn` in `reactor.c:747` currently goes through raw
  `tur_fiber_new` and leaves `effect_handler_chain` uninitialized -- the
  rewrite is the right time to fix it. For cross-thread submit, the
  snapshot is taken on the submitter; the fiber carries its origin
  handlers across threads.
- **Re-entrancy with a live reactor -- hard error.** Calling
  `reactor-run-fibers` (or `reactor-run`) on a group that is already
  running rejects with the existing `g->running` check at
  `reactor.c:777`. After the rewrite, "the scheduler is the global
  reactor's fiber driver" is the documented model; nested pumping on the
  same group is a programmer error. A fixture asserts the `-1` return
  path; the parent plan's "park the outer fiber" alternative is
  explicitly not adopted.

---

## Phases

| Step | Task |
|---|---|
| F9-1 | Add a thread-safe cross-thread submission path to `LocalFiberGroup` (drain-on-tick + `reactor-wake`); fixture: submit from another thread, fiber runs on the target |
| F9-2 | Resolve the work-stealing question (steal un-started fibers only); prototype an idle-worker steal that re-`local-spawn`s onto the local group; benchmark against the current deque |
| F9-3 | Re-express one worker thread as `Reactor` + `LocalFiberGroup` behind a `TUR_SCHED_REACTOR=1` flag; route `spawn`/`await` to `local-spawn`/`local-park-*` on that path only |
| F9-4 | Retarget `async-socket-*`, `async-file-*`, `async-pipe-*` park to `local-park-fd` against the current thread's group (no signature change) |
| F9-5 | Move scheduler timers onto `reactor-add-timer`/`-add-interval`; retire the per-scheduler `TurTimerWheel` for this consumer |
| F9-6 | Flip the flag default; delete `struct IOWaiter`, `scheduler_mt_io_callback`, and the bespoke `io_wait`/`io_modify`/`io_unregister` path |
| F9-7 | Define + fixture the "reactor and scheduler on the same thread" semantics (parent-plan open question) |
| F9-8 | Docs: update `async-await-guide.md` internals note + `reactor-guide.md` Style 3 to state the scheduler now *is* the global reactor's fiber driver; archive this plan |

F9-1/F9-2 are the de-risking phases (no scheduler behavior change yet).
F9-3 is the first behavioral fork, gated behind a flag so both paths
coexist. F9-6 is the point of no return (old path deleted) and must only
land after the full ctest suite (including TSan) is green on the new
path.

---

## Non-goals

- **No public API change.** `spawn`, `await`, the `async-*` wrappers, and
  the `tur_scheduler_mt_*` entry points keep their contracts. If any of
  them must change, that is a separate breaking-change plan.
- **No new concurrency model.** Cross-thread handoff stays explicit
  (channels + wake / cross-thread submit). No actor model, no structured
  concurrency -- those remain out of scope, as in the parent plan.
- **No removal of `TurTimerWheel` wholesale.** Only the scheduler's use
  of it is retired; other consumers (if any) keep it until separately
  migrated.

---

## Success criteria

- The entire ctest suite (compiled fixtures + turi + TSan targets) passes
  with the new path as the default and the old path deleted.
- No regression on the async benchmarks used to size F9-2's steal
  prototype.
- `async-echo-server` and the other `async-*` fixtures pass unchanged
  (their `.tur` source is untouched -- only the park implementation moved).
- Leak-clean under the existing ASan/LSan policy (the interpreter
  harnesses keep `detect_leaks=0`; the compiler/codegen path stays
  leak-checked).
