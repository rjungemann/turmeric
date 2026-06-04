---
title: Stdlib Linearity / Affinity for Resource Handles
category: Planning
description: Promote stdlib resource-handle newtypes (Mutex, Chan, Promise, Future, TaskGroup, Reactor, TmpFile, ChildHandle, Bytes) from plain `defopaque` to `:linear` / `:affine`. Catch double-free, use-after-free, missing-wait, and double-fulfill at compile time.
---

# Stdlib Linearity / Affinity for Resource Handles -- Plan

> **Type:** stdlib API hardening -- linearity discipline on resource handles
> **Prerequisite:** the corresponding handle is already a `defopaque` newtype
> (see [[stdlib-opaque-handle-types-plan]]); this plan layers on top.

> **Status (2026-06-04, update 3):** L2/L3 inventory completed. `Reactor`
> (`:linear`; all accessors `^borrow`, `reactor-free` consumes -- the thin
> extern wrappers coerce with `(:: r :ptr<void>)` at the C boundary),
> `ChildHandle` (new `:linear` newtype distinct from `Pid`; `process/spawn`
> returns it, `process/wait` consumes it, so a forgotten or doubled reap is a
> compile error), and `Bytes` (`:linear`; `bytes-alloc`/`bytes-concat` produce,
> `bytes-len`/`bytes-data` `^borrow`, `bytes-free` consumes -- the `Serializable`
> typeclass surface stays raw `ptr<void>` and is untouched) are now promoted,
> each with positive + negative fixtures. With L1 (update 2) this clears the
> entire inventory table below. `Pid` remains a non-linear opaque (it is a
> freely-copied identifier from `process/pid`/`-ppid`, not a freed resource);
> the `LocalFiberGroup` handle in `tur/reactor` stays a plain `ptr<void>` (out
> of scope -- the plan names only `Reactor`). `Future` remains the one
> deliberate non-promotion; see update-2 note and
> [[../reported/stdlib-future-linearity-aliasing]].
>
> **Status (2026-06-04, update 2):** L1/L2 inventory largely promoted. On top
> of the foundational slice below, `Chan` + `AsyncChan` (`:linear`; send/recv/
> try/count accessors `^borrow`, `*-free` consumes), `TaskGroup` (`:linear`; all
> group accessors `^borrow`, `task-group-free` consumes -- `TaskHandle` stays a
> plain opaque since a group hands out many un-freed handles), and `Promise`
> (`:linear`; `promise-fulfill`/`-fail`/`-free` consume by value, so
> double-fulfill is now a compile-time `TUR-E0101`) are all promoted, each with
> positive + negative (`-dropped` / `-double-free` / `-double-fulfill`)
> fixtures. While promoting `TaskGroup` a real `^borrow` false-positive was
> found and fixed: a `^borrow` parameter forwarded to another `^borrow`
> parameter (or simply unused) in a non-inline-C body spuriously reported
> `TUR-E0100`; see [[../reported/borrow-param-forwarding-drop]] (RESOLVED).
> `Future` is intentionally **not** promoted: a `Promise` and its `Future` are
> aliases of one shared `FutureCell`, which single-ownership forbids -- see
> [[../reported/stdlib-future-linearity-aliasing]] for the design tension and
> fix directions. Still unpromoted (prerequisite not met -- not yet a
> `defopaque`): `Reactor` and `Bytes` (bare `ptr<void>`), and `ChildHandle`
> (`tur/process` uses `Pid`, a freely-copied value rather than a freed
> resource). Promote those only after they gain opaque-handle status.
>
> **Status (2026-06-04):** foundational slice + borrow form landed. The compiler
> now supports `defopaque Name :base :linear` / `:affine` (previously the
> attribute was silently dropped), inline-C accessor bodies no longer mis-report
> `TUR-E0100` on their own linear params (consumption is enforced at the
> Turmeric call site), and the **`^borrow` parameter attribute** (LB1) lets a
> non-consuming accessor read a linear/affine handle without spending its single
> consumption -- this unblocks multi-use handles. Promoted so far: `TmpFile`
> (`:linear`; `fs/tmpfile-path` / `fs/tmpfile-fd` are `^borrow`) and `Mutex`
> (`:linear`; `mutex-lock` / `mutex-unlock` / `mutex-try-lock` / `condvar-wait`
> are `^borrow`, `mutex-free` consumes), each with positive + negative
> (double-free / drop / use-after-free) fixtures. The borrow form is described
> in [[../reported/stdlib-linear-handle-borrows]] (now RESOLVED). Remaining
> inventory (`Chan`, `Promise`, `Future`, `TaskGroup`, `Reactor`,
> `ChildHandle`, `Bytes`) can now be promoted the same way: mark consuming ops
> by value and non-consuming accessors `^borrow`.

## Motivation

`stdlib/io.tur:288` already uses `defopaque FileHandle :linear`, which the
type checker tracks for use-exactly-once / use-at-most-once discipline.
The same discipline catches whole bug classes at compile time:

- Double-free of a `Chan`, `Mutex`, `Promise`, or `TaskGroup`.
- Use-after-free across reactor callbacks.
- Forgetting `task-group-wait` before dropping a group.
- Calling `promise-fulfill` twice on the same `Promise`.

Today, the concurrency-core handles are opaque (Tier 1 of the
opaque-handle plan) but not linear, so these bugs still slip through.

## Inventory

| Module           | Handle              | Discipline | Notes |
|------------------|---------------------|------------|-------|
| `tur/mutex`      | `Mutex`             | linear     | new/free pair |
| `tur/chan`       | `Chan`, `AsyncChan` | linear     | |
| `tur/future`     | `Promise`           | linear     | consumed by `promise-fulfill` |
| `tur/future`     | `Future`            | affine     | optional cancel / drop |
| `tur/taskgroup`  | `TaskGroup`         | linear     | `wait` consumes |
| `tur/reactor`    | `Reactor`           | linear     | |
| `tur/fs`         | `TmpFile`           | linear     | implicit unlink-on-close |
| `tur/process`    | `ChildHandle`       | linear     | `wait` consumes |
| `tur/serial`     | `Bytes`             | linear     | `bytes-alloc` / `bytes-free` |

## Design

Promote the opaque declarations to `:linear` (or `:affine` for cancellable
resources):

```turmeric
(defopaque Promise   ptr<void> :linear)   ;; consumed by promise-fulfill
(defopaque Future    ptr<void> :affine)   ;; may be dropped uncollected
(defopaque TaskGroup ptr<void> :linear)   ;; wait consumes
```

Constructor / consumer signatures:

```turmeric
(defn promise-new []                          : Promise)
(defn promise-fulfill [p : Promise v : int]   : nil)   ;; consumes p
(defn task-group-wait [g : TaskGroup]         : int)   ;; consumes g
```

The opaque-handle plan's "C-side declarations are unaffected" guarantee
still holds: the linearity attribute is enforced in the type checker, not
the C ABI.

## Phasing

1. **L1** -- Promote `Mutex`, `Chan`, `AsyncChan`, `Promise`, `Future` to
   linear/affine. Land in lockstep with their opaque-handle PRs (or as the
   immediately following PR per module).
2. **L2** -- `TaskGroup`, `Reactor`, `ChildHandle`, `TmpFile`. Touches
   more call sites, including reactor callbacks.
3. **L3** -- Re-audit `tur/io` consumers; convert `bytes-alloc` /
   `bytes-free` in `tur/serial`.

## Risks

- Linearity errors will surface in user code that today legally aliases
  handles. Provide an `unsafe-dup` escape hatch on each linear newtype for
  the rare cases where aliasing is intended.
- Closure captures of linear values need the existing "closure consumes"
  rule; surface this in the `tur/effects` guide.

## Acceptance

- Each handle module exposes the new linear/affine type.
- `bash tests/run.sh` passes with zero `FAIL` lines (ASan/LSan on).
- A representative fixture (e.g. a "double-fulfill" test) now fails to
  compile with a clear linearity diagnostic.
- `tur run docs` regenerated; new linearity attributes appear in the API
  reference.

## Cross-references

- Layered on [[stdlib-opaque-handle-types-plan]]; ordering matters.
- Pairs with [[stdlib-session-typed-channels-plan]] (which uses linearity
  to enforce protocol step ordering).
- Split out from the original umbrella `stdlib-advanced-typing-plan` so
  each feature ships independently.
