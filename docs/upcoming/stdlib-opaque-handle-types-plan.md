# Stdlib Opaque Handle Types Plan

> **Status:** Draft Plan
> **Last Updated:** 2026-06-02
> **Type:** stdlib API hardening -- type safety via `defopaque` newtypes

---

## Overview

Large portions of the stdlib expose resource handles as bare `:int` or
`:ptr<void>` values. A thread pool handle, a future handle, a timer id, a
condvar, and a mutex are all the same type at the signature level, so the
compiler will happily accept `(condvar-wait mutex condvar)` -- arguments
swapped -- or `(thread-pool-submit future ...)` -- pool/future confused -- as
well-typed code. The error surfaces at runtime (SEGV, deadlock, or silent
misbehavior) rather than at compile time.

Two language features have landed since most of this stdlib was written that
make wrapping these handles cheap and high-value:

1. **`defopaque T :int`** newtypes -- zero-cost wrappers that the type checker
   treats as nominally distinct.
2. **Identity-based type checking for opaque/struct/ADT arguments,** including
   in variadic `& rest :T` parameters. Per CLAUDE.md's quick-decision guide,
   handles are now explicitly endorsed as `defopaque` material; the old
   workaround of declaring rest as `:int` and casting inside the body is no
   longer needed.

This plan inventories the worst offenders and lays out a phased rollout that
hardens the concurrency core first, then mid-level sync primitives, then
fills in remaining gaps.

---

## Motivation

The `tur/threadpool` API ([API reference][threadpool-api]) is the canonical
example. Every constructor returns `:ptr<void>` and every operation accepts
`:ptr<void>`:

```turmeric
work-queue-new-bounded [cap :int] :ptr<void>
thread-pool-new        [n :int]   :ptr<void>
thread-pool-submit     [tp :ptr<void> ...] :ptr<void>   ;; pool in, future out
future-get             [f :ptr<void>] :int
```

Nothing prevents `(future-get pool)` or `(thread-pool-submit future ...)`.
The same pattern repeats in `tur/future` (Promise vs Future), `tur/chan`
(sync `Chan` vs `AsyncChan`), `tur/timer` (TimerId is a bare `:int`), and the
mutex/condvar/rwlock trio.

Wrapping these handles in `defopaque` newtypes:

- Converts a class of latent runtime bugs into compile errors.
- Carries zero runtime cost -- the wrappers compile to the same int/pointer.
- Self-documents the API: a signature accepting `Mutex` is unambiguous.
- Composes with `& rest :T` checking, so e.g. `(future-race-n & futures :Future)`
  rejects a stray pool handle at the call site.

[threadpool-api]: https://turmeric-lang.com/docs/html/api/tur-threadpool

---

## Inventory

### Tier 1 -- Concurrency core (highest impact)

| Module          | Newtypes to introduce                                | Notes |
|-----------------|------------------------------------------------------|-------|
| `tur/threadpool`| `WorkQueueHandle`, `ThreadPoolHandle`, `FutureHandle`| Pool and submitted future are both `:ptr<void>` today |
| `tur/future`    | `Promise`, `Future`                                  | Write end vs read end currently indistinguishable |
| `tur/chan`      | `Chan`, `AsyncChan`                                  | Sync and async channels share a type |

Representative signatures:

- `stdlib/threadpool.tur:86` `work-queue-new-bounded -> :ptr<void>`
- `stdlib/threadpool.tur:339` `thread-pool-new -> :ptr<void>`
- `stdlib/threadpool.tur:414` `thread-pool-submit [tp :ptr<void> ...] -> :ptr<void>`
- `stdlib/future.tur:137` `promise-fulfill [p :ptr<void> v :int]`
- `stdlib/future.tur:222` `future-get [f :ptr<void>]`
- `stdlib/chan.tur:43,78` `chan-new`, `chan-send`

### Tier 2 -- Mid-level sync & event IDs

| Module        | Newtypes                                  | Notes |
|---------------|-------------------------------------------|-------|
| `tur/timer`   | `TimerId`                                 | Currently any `:int` can be passed to `timer-cancel` |
| `tur/reactor` | `EventSourceId`                           | `add-fd` / `add-timer` / `add-signal` all return bare `:int` |
| `tur/taskgroup` | `TaskGroup`, `TaskHandle`               | Group handle vs spawned task handle |
| `tur/mutex`   | `Mutex`                                   | |
| `tur/condvar` | `CondVar`                                 | `condvar-wait [c m]` -- args swappable today |
| `tur/rwlock`  | `RwLock`                                  | |

Representative signatures:

- `stdlib/timer.tur:32` `timer-set -> :int`
- `stdlib/timer.tur:50` `timer-cancel [id :int]`
- `stdlib/reactor.tur:51-69` add/modify/remove all use bare `:int` IDs
- `stdlib/condvar.tur:43` `condvar-wait [c :ptr<void> m :ptr<void>]`

### Tier 3 -- Completeness

| Module        | Newtypes        |
|---------------|-----------------|
| `tur/atomic`  | `AtomicCell`    |

Additional candidates worth a second pass once tiers 1-2 land: `tur/thread`
(`ThreadHandle`), `tur/fiber` (`FiberHandle`), `tur/stm`, `tur/ref`, and the
I/O modules (`tur/io`, `tur/fs`, `tur/net`, `tur/async_socket`,
`tur/async_file`, `tur/async_pipe`, `tur/process`, `tur/serial`) where file
descriptors are passed as bare `:int`.

---

## Design

### Newtype shape

```turmeric
(defopaque ThreadPoolHandle :ptr<void>)
(defopaque FutureHandle     :ptr<void>)
(defopaque TimerId          :int)
```

Each module owns the newtypes it returns. Cross-module APIs that *consume* a
handle import the relevant newtype.

### Constructor / destructor pattern

Public constructors return the newtype directly:

```turmeric
(defn thread-pool-new [n : int] : ThreadPoolHandle
  ;; existing body returns :ptr<void>; wrap on return
  ...)
```

Public operations take the newtype:

```turmeric
(defn thread-pool-submit [tp : ThreadPoolHandle ...] : FutureHandle
  ...)
```

Inline-C blocks that unwrap to the raw representation stay unchanged below
the `defopaque` boundary -- the codegen lowers `:ThreadPoolHandle` to its
underlying `:ptr<void>` in the C ABI, so existing C-level code keeps
working without edits.

### Interaction with `& rest`

```turmeric
(defn future-race-n [& futures : FutureHandle] : int ...)
```

The variadic checker now compares opaque types by identity, so passing a
`ThreadPoolHandle` here is a compile error rather than a runtime miscast.

### Backward compatibility

`defopaque` is nominal but ABI-compatible with its base type, so:

- C-side declarations of `tur` symbols are unaffected.
- Fixture binaries and snapshots regenerate cleanly (per CLAUDE.md's fixture
  rule, snapshots must be updated alongside this work).
- Downstream `.tur` code that previously passed bare `:int`/`:ptr<void>` will
  need to update its own signatures or use an explicit `(<newtype> raw)`
  cast. This is the intended source-level break.

---

## Rollout

### Phase 1 -- Tier 1 (threadpool, future, chan)

1. Land `defopaque` declarations + signature updates in each module.
2. Update intra-stdlib callers (`tur/taskgroup`, `tur/scheduler`, etc.).
3. Regenerate all `tests/fixtures/*/expected.c` snapshots per the
   fixture-snapshot rule.
4. Run `bash tests/run.sh` to confirm zero `FAIL` lines.
5. One PR per module (threadpool, future, chan) to keep review tractable.

### Phase 2 -- Tier 2 (timer, reactor, taskgroup, mutex/condvar/rwlock)

Same procedure. Mutex/condvar/rwlock can ship as a single PR since they
co-evolve (`condvar-wait` needs both newtypes simultaneously).

### Phase 3 -- Tier 3 + sweep

- `tur/atomic`.
- Re-audit `tur/thread`, `tur/fiber`, `tur/stm`, `tur/ref`.
- I/O module sweep (file descriptors, sockets).

### Out of scope

- Pure-data modules where `:int` is a real integer (`math`, `nat`, `bits`,
  `hash`, `range`, `float-range`, `sized*`).
- Reworking module internals -- this is a signature/typing change, not a
  refactor of the underlying C-level implementation.

---

## Risks

- **Snapshot churn.** Every signature change touches `expected.c` for any
  fixture that exercises the API. Mitigated by following the standard
  snapshot-regeneration recipe in CLAUDE.md.
- **Downstream `.tur` code.** Anything outside the stdlib that passes a bare
  pointer needs updating. This is the intended hardening but should be
  called out in the changelog for each phase.
- **Web / WASM glue.** `web/turmeric.js` exports the C ABI, which is
  unaffected by `defopaque` at the source level. No glue changes expected.

---

## Acceptance criteria

- All Tier 1 + Tier 2 modules expose handle-typed signatures.
- `tests/run.sh` passes with zero `FAIL` lines, including ASan/LSan.
- API docs (`tur run docs`) regenerated and show the new newtypes.
- A representative "wrong handle" test fixture under `tests/fixtures/`
  demonstrates the error now manifests at compile time.
