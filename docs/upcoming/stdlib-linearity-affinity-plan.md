---
title: Stdlib Linearity / Affinity for Resource Handles
category: Planning
description: Promote stdlib resource-handle newtypes (Mutex, Chan, Promise, Future, TaskGroup, Reactor, TmpFile, ChildHandle, Bytes) from plain `defopaque` to `:linear` / `:affine`. Catch double-free, use-after-free, missing-wait, and double-fulfill at compile time.
---

# Stdlib Linearity / Affinity for Resource Handles -- Plan

> **Type:** stdlib API hardening -- linearity discipline on resource handles
> **Prerequisite:** the corresponding handle is already a `defopaque` newtype
> (see [[stdlib-opaque-handle-types-plan]]); this plan layers on top.

> **Status (2026-06-04):** foundational slice landed. The compiler now supports
> `defopaque Name :base :linear` / `:affine` (previously the attribute was
> silently dropped), inline-C accessor bodies no longer mis-report `TUR-E0100`
> on their own linear params (consumption is enforced at the Turmeric call
> site), and `TmpFile` is promoted to `:linear` as the representative handle
> with positive + negative (double-free / drop) fixtures. The remaining
> inventory is **blocked on a borrow form** for non-consuming accessors --
> see [[../reported/stdlib-linear-handle-borrows]]. Multi-use handles
> (`Mutex`, `Chan`, ...) cannot be promoted until borrows land, because every
> use of a linear binding currently consumes it.

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
