# Plan: Extract Thread Pool into tur-thread-pool Spice

> **Status:** Draft
> **Last Updated:** 2026-06-01
> **Type:** Spice Extraction
> **Spice Location:** `turmeric-spices/spices/thread-pool`
> **Extracted from:** `turmeric-spices/spices/httpd` (`httpd/pool`)

---

## Overview

`httpd/pool` implements a solid bounded worker-thread pool
(mutex + two condition variables + ring buffer of `int` items, with
cooperative back-pressure).  Its current implementation is tightly
coupled to `tur-httpd` in two ways:

1. **Item type is `int` (socket fd).** The pool speaks file-descriptor
   integers directly; there is no generic work-item type.
2. **Worker dispatch hardcodes `httpd__server__srv_worker_loop`.**
   Each dequeued item is immediately dispatched to the HTTP
   connection-servicing function via a hardcoded `extern` declaration.

This plan extracts the pool into a general-purpose spice
(`tur-thread-pool`) that accepts an arbitrary `ptr<void>` work item and
an arbitrary `ptr<void>` worker callback, then makes `tur-httpd` depend
on it rather than duplicate the logic.

---

## Motivation

- **Reuse.** Other spices (OSC server, Postgres connection pool, notebook
  kernel, future job-queue use cases) could use a bounded worker pool
  without pulling in `tur-httpd`.
- **Correctness concentration.** Thread-pool edge cases (stop-before-first-
  worker, mid-drain stop, cap overflow, allocation failure) are subtle.
  Maintaining one canonical implementation is safer than multiple copies.
- **Simpler `httpd/pool`.** Once `httpd/pool` delegates to `tur-thread-pool`,
  its entire inline-C body can be replaced by a thin wrapper.

---

## Scope

### In scope for v0.1.0

- New spice `tur-thread-pool` in `turmeric-spices/spices/thread-pool`
- Module `thread-pool/pool` exporting:
  - `pool-new`    -- allocate pool, spawn N worker threads
  - `pool-submit` -- enqueue a `ptr<void>` work item (blocks on back-pressure)
  - `pool-stop`   -- drain, join all workers, free
- Rewrite `httpd/pool` to delegate to `tur-thread-pool`; keep its existing
  public API (`pool-new`, `pool-enqueue`, `pool-stop`) as a thin wrapper
- Unit fixture tests for the new spice

### Out of scope for v0.1.0

- Priority queues or multiple named lanes
- Dynamic pool resize
- Work-stealing or work-stealing deques
- Per-task result channels / futures
- Async I/O integration (io_uring / kqueue)

---

## Design

### `__thread_pool` struct

The extracted C struct replaces the fd-specific `__httpd_pool` with a
generic pointer-item ring buffer:

```c
struct __thread_pool {
  pthread_mutex_t mu;
  pthread_cond_t  not_empty;
  pthread_cond_t  not_full;
  void          **items;   /* ring buffer of ptr<void> work items */
  int             cap;
  int             head;
  int             tail;
  int             count;
  int             stop;
  void           *callback;  /* void (*)(void *item) -- called per item */
  pthread_t      *workers;
  int             nworkers;
};
```

Replacing `int *fds` with `void **items` and adding a `callback` field is
the only structural change from `__httpd_pool`.

### Worker entry point

```turmeric
;;; pool-worker-loop -- (internal) thread entry; dequeues and dispatches items.
(defn pool-worker-loop [arg :ptr<void>] :ptr<void>
  ...)
```

The worker pops `item = items[head]` and calls `((void (*)(void*))p->callback)(item)`.
The callback owns the item's lifetime (allocate before submit, free inside callback).

### Public API

```turmeric
(defmodule thread-pool/pool
  (export pool-new pool-submit pool-stop))

;;; pool-new -- allocate a bounded worker pool and spawn `size` threads.
;;;
;;; Parameters:
;;;   size     -- number of worker threads (clamped to >= 1).
;;;   callback -- :ptr<void> (void (*)(void*)) called on each dequeued item.
;;;
;;; Returns:
;;;   :ptr<void> pool handle, or NULL on allocation/thread-spawn failure.
;;;
;;; Example:
;;;   (def pool (pool-new 8 my-callback))
;;;
;;; Since: TP1
(defn pool-new [size :int callback :ptr<void>] :ptr<void> ...)

;;; pool-submit -- enqueue a work item; blocks when ring buffer is full.
;;;
;;; Parameters:
;;;   pool -- :ptr<void> pool handle (NULL discards the item).
;;;   item -- :ptr<void> work item; ownership transferred to pool.
;;;
;;; Returns: :void
;;;
;;; Example:
;;;   (pool-submit pool (make-work-item data))
;;;
;;; Since: TP1
(defn pool-submit [pool :ptr<void> item :ptr<void>] :void ...)

;;; pool-stop -- signal shutdown, drain, join workers, free.
;;;
;;; Parameters:
;;;   pool -- :ptr<void> pool handle (NULL is a no-op).
;;;
;;; Returns: :void
;;;
;;; Example:
;;;   (pool-stop pool)
;;;
;;; Since: TP1
(defn pool-stop [pool :ptr<void>] :void ...)
```

### Ring-buffer capacity

Preserve the existing formula from `httpd/pool`:
```c
int cap = (n * 4 > 64) ? (n * 4) : 64;
```
This keeps the default 4× headroom without breaking `tur-httpd` behaviour.

### `httpd/pool` after extraction

`httpd/pool` becomes a thin adapter:

```turmeric
(defmodule httpd/pool
  (export pool-new pool-enqueue pool-stop))

;; Allocates a thread-pool/pool whose callback wraps srv-worker-loop.
;; pool-new, pool-enqueue (calls pool-submit), pool-stop delegate directly.
```

The `__httpd_warg { int client_fd; void *handler; }` allocation stays in
`httpd/pool` because it is HTTP-specific (packing fd + handler together).

---

## Spice manifest (`build.tur`)

```turmeric
(spice thread-pool
  (version "0.1.0")
  (description "Bounded POSIX thread pool for Turmeric")
  (exports thread-pool/pool)
  (sources "src/thread-pool/pool.tur"))
```

No external dependencies beyond the system's `pthread`.

---

## File layout

```
turmeric-spices/spices/thread-pool/
  build.tur
  README.md
  src/
    thread-pool/
      pool.tur
turmeric-spices/spices/httpd/
  build.tur          (add :spices [thread-pool :path "../thread-pool"])
  src/
    httpd/
      pool.tur       (rewrite as thin wrapper)
      server.tur     (unchanged)
      ...
```

---

## Migration steps

1. **Create** `turmeric-spices/spices/thread-pool/` with `build.tur`,
   `README.md`, and `src/thread-pool/pool.tur` containing the extracted,
   generalized implementation.
2. **Add** `thread-pool` as a `:path` dep in `httpd/build.tur`.
3. **Rewrite** `httpd/pool.tur` to import `thread-pool/pool` and provide
   thin wrappers for `pool-new`, `pool-enqueue`, and `pool-stop` with the
   same signatures as today.
4. **Verify** `tur-httpd` fixture tests pass unchanged (the public API is
   unmodified from the caller's perspective).
5. **Write** standalone fixture tests for `tur-thread-pool` covering:
   - Basic submit + drain
   - Back-pressure (submit with full queue, then drain)
   - Stop before any item is submitted
   - Stop while queue is non-empty
   - `pool-new` with `size = 0` (clamped to 1)
6. **Tag** `thread-pool-v0.1.0` on the spices repo.

---

## Risks and mitigations

| Risk | Mitigation |
|---|---|
| Struct layout duplication across inline-C blocks | Accepted pattern in this codebase; each inline-C block redeclares the struct verbatim (same as current `httpd/pool`). No new risk introduced. |
| `httpd/pool` caller API changes | Wrapper keeps the existing signatures (`pool-new`, `pool-enqueue`, `pool-stop`); callers in `httpd/server` require no changes. |
| Callback lifetime | Documented: callback must remain valid for the full pool lifetime (same requirement as `handler` in the current design). |
| Leak-check regression | The new spice's fixture tests run with ASan leak detection on; process-lifetime workers use `ASAN_OPTIONS=detect_leaks=0` as per existing policy if needed. |
