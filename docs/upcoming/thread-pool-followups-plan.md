---
title: Thread-Pool Followups Plan
category: Planning
description: Wrap the raw `ptr<void>` pool handle in a `defopaque` with linear discipline, add a typed `Future<T>` so submitters can await a result, ship a `with-pool` scope macro, and add try-submit + introspection. Tightens the surface CLAUDE.md's "No lazy :int / ptr<void> stand-ins" rule already flags.
---

# `thread-pool` Followups -- Plan

> **Status:** Not started (as of 2026-06-23). `thread-pool/src/thread-pool/pool.tur` still exposes `pool-new`/`pool-submit`/`pool-stop` over `ptr<void>`; no `Pool`/`Pool<T>` opaque, no `Future<R>`, no `with-pool` macro, no `pool-try-submit`. None of the new modules (`future.tur`, `parallel.tur`, `scope.tur`) exist yet.

## Context

`thread-pool` v0.1.0 shipped with v0.24.0. The user-facing guide is
[`docs/guides/thread-pool-guide.md`](../../guides/thread-pool-guide.md).
The spice exports three defns:

```turmeric
(defn pool-new [size : int callback : ptr<void>] : ptr<void>)
(defn pool-submit [pool : ptr<void> item : ptr<void>] : void)
(defn pool-stop [pool : ptr<void>] : void)
```

Everything is `ptr<void>`: the pool handle, the work item, the
callback. `ptr<void>` is a stand-in for a real type in three places --
exactly the pattern CLAUDE.md's **"No Lazy :int Stand-Ins"** rule
flags. The shipped surface is correct under-the-hood (ring buffer with
configurable capacity, worker-side back-pressure, clean shutdown), but
it asks every consumer to manage type discipline by convention, and it
asks them to bake their own result-reporting (shared statics or a
side-channel queue) onto every submission.

This plan tightens the surface to what the type system can already
express today, without changing the runtime characteristics that the
existing tests pin down.

## Goals

1. **TY-V0 -- typed `Pool` handle.** `defopaque Pool :int` with
   linear discipline, so a pool is consumed exactly once by
   `pool-stop` (or by exiting a `with-pool` scope).
2. **WORK-V0 -- typed work items.** Parametric `(Pool<T>)` whose
   `pool-submit` takes an owned `T` rather than `ptr<void>`. The
   pool's callback signature becomes `(fn [T] : void)`. Callers stop
   doing the `(intptr_t)` cast dance for every queue entry.
3. **FUT-V0 -- `Future<R>` for fan-out / await.** A second submit
   form, `pool-submit-future`, returns a `Future<R>` the caller can
   await. Fan-out / collect becomes one stdlib pattern instead of
   "thread a Mutex+Cond/result-slot through your work item."
4. **SCOPE-V0 -- `with-pool` scope macro.** `(with-pool [p (pool-new
   ...)] body)` guarantees `pool-stop` runs on any exit path; matches
   the existing `with-resource` patterns elsewhere in stdlib.
5. **TRY-V0 -- non-blocking submit and introspection.** A
   `pool-try-submit` that returns `result<unit FullError>` (back-
   pressure observable) plus `pool-pending`, `pool-workers`,
   `pool-queue-cap` accessors.

## Non-goals

- A work-stealing scheduler. The fixed-pool, FIFO queue, blocking-on-
  full design is intentional and matches the consumers we have. A
  stealing scheduler is a different plan; it would not subsume this
  one.
- Priority queues / per-task deadlines.
- Pinning workers to CPUs / NUMA placement.
- An async/await runtime built on top. The `Future<R>` here is a
  thin one-shot result channel, not a continuation-passing scheduler.
  An async runtime can be built on `pool` + `Future<R>` later; not
  part of this plan.
- Cross-pool work transfer (a task that bounces from one pool's queue
  to another's).
- Cancellation propagation. Once a task is in the queue or running, it
  runs to completion. Cooperative cancellation is the application's
  job (check a flag inside the work).

## Design

### TY-V0 -- typed `Pool` handle

```turmeric
(defopaque Pool :int)   ;; was: returned as ptr<void> from pool-new

(defn pool-new [size : int callback : (fn [ptr<void>] : void)] : ^linear Pool ...)
(defn pool-stop [^linear pool : Pool] : void ...)
```

The handle is `^linear`: the type system enforces that a `Pool` is
consumed exactly once. `pool-stop` and `with-pool` are the only two
sinks. Forgetting `pool-stop` is a compile error; calling it twice is
a compile error.

The constructor still takes a raw fn-pointer callback (see WORK-V0
for the typed form). Keep `pool-new` available for the
"my work is already `ptr<void>`-ABI" use cases (e.g. interop with a
C library that hands you a `void*` queue entry).

### WORK-V0 -- typed work items via `Pool<T>`

A second, parametric constructor:

```turmeric
(defn pool-new<T> [size : int callback : (fn [T] : void)] : ^linear Pool<T> ...)

(defn pool-submit<T> [^borrow pool : Pool<T> item : T] : void ...)
(defn pool-stop<T>   [^linear pool : Pool<T>] : void ...)
```

`Pool<T>` is `defopaque Pool<T> :int` parameterized on the work type.
The codegen erases `T` to `int64_t` at the ABI (everything routes
through a `void*` slot internally), but the type system tracks the
parameter end-to-end so an `int`-typed pool refuses a `cstr` item at
compile time.

**The `T` -> `void*` boundary is the only ABI hop.** Inside the
worker, a wrapper trampoline reinterprets the carrier back to `T` and
invokes the user's `(fn [T] : void)`. The trampoline is generic per-T
via monomorphization, no per-pool dispatch table.

**Ownership.** `pool-submit<T>` consumes the item by value (the
worker callback receives ownership). For `^linear T` this is the
only semantically correct choice -- copy is forbidden by the
substructural system. Document explicitly: "the worker callback owns
its item; the submitter loses access on submit."

### FUT-V0 -- `Future<R>` for await + fan-out

Result-returning variant:

```turmeric
(defopaque Future<R> :int)

(defn pool-submit-future<T R>
    [^borrow pool : Pool<T>
     item        : T
     work        : (fn [T] : R)] : ^linear Future<R> ...)

(defn future-await<R>  [^linear fut : Future<R>] : R ...)
(defn future-try<R>    [^borrow fut : Future<R>] : option<R> ...)  ;; non-blocking
(defn future-cancel<R> [^linear fut : Future<R>] : void ...)       ;; releases storage; does NOT stop in-flight work
```

The `pool-submit-future` callback is a separate function from the
pool's default callback; the trampoline boxes the `(item, slot)` pair
and routes through the pool's queue with a per-item one-shot result
slot.

**Storage.** A `Future<R>` owns a heap cell with a mutex + cond + an
optional `R`. `future-await` blocks; `future-try` peeks. The slot is
freed when the future is consumed (`future-await` or `future-cancel`
both consume it; `future-try` borrows).

**Fan-out idiom in stdlib:**

```turmeric
;; submit N items, collect N results.
(defn pool-map<T R> [^borrow pool : Pool<T>
                     items : list<T>
                     work  : (fn [T] : R)] : list<R> ...)
```

`pool-map` is a thin loop over `pool-submit-future` + `future-await`;
ship it because it's the obvious convenience and there's exactly one
right way to write it.

**Memory ordering.** The slot's mutex is the synchronization edge; no
memory-model claims beyond "the work's writes happen-before the
awaiter's read" because that's all the user can correctly rely on
under pthreads.

### SCOPE-V0 -- `with-pool` scope macro

```turmeric
(with-pool [p (pool-new<int> 8 worker)]
  (pool-submit p 1)
  (pool-submit p 2)
  ...)
;; pool-stop p runs here automatically (also on panic / early return)
```

Lowers to a `try` / `finally`-shaped form that consumes the linear
`Pool` on exit. Same shape as the established `with-mutex` /
`with-resource` macros in stdlib; this is mechanical given the
linear-handle discipline TY-V0 introduces.

### TRY-V0 -- non-blocking submit + introspection

```turmeric
(defdata FullError (PoolFull [pending : int cap : int]))

(defn pool-try-submit<T> [^borrow pool : Pool<T> item : T]
  : result<unit FullError> ...)

(defn pool-pending<T>   [^borrow pool : Pool<T>] : int)
(defn pool-workers<T>   [^borrow pool : Pool<T>] : int)
(defn pool-queue-cap<T> [^borrow pool : Pool<T>] : int)
```

`pool-try-submit` is the same code path as `pool-submit` minus the
condvar wait. Useful for backpressure-aware producers (drop-on-full,
overflow-to-disk, shed-load metrics).

The three accessors read fields the ring buffer already tracks. No
new bookkeeping.

## Work items

| # | Item | File(s) |
|---|------|---------|
| TY-V0.1 | Wrap the pool handle in `defopaque Pool :int`; mark handle linear; flip `pool-new` return + `pool-stop` param. | `thread-pool/src/thread-pool/pool.tur` |
| TY-V0.2 | Tests: forgotten `pool-stop` is a compile error; double-stop is a compile error; the existing runtime fixtures still pass through the wrapper. | `thread-pool/tests/` |
| TY-V0.3 | Guide update: handle is opaque + linear now; show `with-pool` (after SCOPE-V0). | `docs/guides/thread-pool-guide.md` |
| WORK-V0.1 | Introduce `Pool<T>` opaque + parametric `pool-new<T>` + typed `pool-submit<T>`. | `thread-pool/src/thread-pool/pool.tur` |
| WORK-V0.2 | Implement the per-T trampoline that bridges the `T` user callback to the underlying `void*` worker. | same |
| WORK-V0.3 | Tests: a `Pool<int>` rejects a `cstr` submit at compile time; round-trip of an opaque struct preserves identity; substructural item type sees consume-once. | `thread-pool/tests/typed-pool.tur` (new) |
| WORK-V0.4 | Guide update: typed-pool subsection becomes the primary form; raw-`ptr<void>` form is documented as the C-interop hatch. | `docs/guides/thread-pool-guide.md` |
| FUT-V0.1 | `Future<R>` opaque + heap cell layout (mutex/cond/slot/state). | `thread-pool/src/thread-pool/future.tur` (new) |
| FUT-V0.2 | `pool-submit-future`, `future-await`, `future-try`, `future-cancel`. | same |
| FUT-V0.3 | `pool-map` convenience in `thread-pool/parallel.tur`. | new module |
| FUT-V0.4 | Tests: await blocks then receives the result; try returns none before completion; cancel releases storage; pool-map round-trips N items. | `thread-pool/tests/future.tur` (new) |
| FUT-V0.5 | Guide update: new "Returning results" section; pool-map example. | `docs/guides/thread-pool-guide.md` |
| SCOPE-V0.1 | `with-pool` macro in `thread-pool/scope.tur`. | new module |
| SCOPE-V0.2 | Tests: scope-exit stops the pool; panic-exit stops the pool; nested with-pool works. | `thread-pool/tests/with-pool.tur` |
| SCOPE-V0.3 | Guide update: `with-pool` becomes the recommended construct. | `docs/guides/thread-pool-guide.md` |
| TRY-V0.1 | `pool-try-submit` returning `result<unit FullError>`. | `thread-pool/src/thread-pool/pool.tur` |
| TRY-V0.2 | Accessors `pool-pending`, `pool-workers`, `pool-queue-cap`. | same |
| TRY-V0.3 | Tests: full pool returns `FullError` with pending=cap; introspection accessors return the configured size. | `thread-pool/tests/try-submit.tur` (new) |
| TRY-V0.4 | Guide update: new "Backpressure" subsection. | `docs/guides/thread-pool-guide.md` |

**Ordering.**

- TY-V0 -> SCOPE-V0 (`with-pool` needs the linear discipline).
- TY-V0 -> WORK-V0 -> FUT-V0 (FUT-V0 builds on the typed surface).
- TRY-V0 is independent; can land any time after TY-V0.

## Testing

The existing harness (a couple of tests under
`thread-pool/tests/`) drives the pool through a fixed work load and
checks shutdown. New tests reuse the same scaffold.

For FUT-V0 specifically, write at least one test that submits N
futures, awaits them all, and asserts the union of returned values
equals the union of submitted seeds. This is the canonical
"is fan-out actually correct" check.

## Risk

- **Linear-pool migration is a hard break for existing consumers.**
  Every spice that holds a `Pool` in a long-lived global needs to
  switch to the linear handle (or to a wrapper that owns the linear
  handle internally and exposes a borrow-only API). Inventory the
  in-tree consumers (`ecs-raylib`, anything else) before flipping.
- **Future heap-cell lifetime is the trickiest design knob.** A user
  who drops a `Future` without awaiting could leak the slot; the
  linear discipline (`future-cancel` is the way to drop without
  awaiting) prevents that statically, but the worker still needs a
  way to detect a canceled slot so it doesn't write into freed
  memory. Layout the slot with a refcount that the worker decrements
  after writing, the awaiter decrements after reading, and the
  canceler decrements after taking ownership -- whoever lands at 0
  frees.
- **Trampoline codegen size.** Monomorphizing per-T multiplies the
  trampoline. Acceptable -- the trampoline is ~20 lines C-side --
  but worth a fixture that exercises 5-10 distinct `Pool<T>` types
  in one TU to verify the codegen growth is sane.

## Out of scope

- Work-stealing.
- Per-task priority / deadlines.
- Worker pinning to CPUs.
- A full async runtime.
- Cancellation propagation into in-flight work.
- Cross-pool task transfer.
