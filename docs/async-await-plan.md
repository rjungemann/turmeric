# Turmeric — Async/Await Plan

> **Status:** Draft Plan
> **Last Updated:** 2026-05-10
> **Owner:** Runtime team
> **Phase:** v2 (post-Phase 19)

**Cross-references:**
- [turmeric-plan.md](turmeric-plan.md) — Main compiler roadmap
- [thread-safety-and-primitives-plan.md](thread-safety-and-primitives-plan.md) — Thread safety and primitives (Phase 19)
- [effects-plan.md](effects-plan.md) — Algebraic effects design
- Phase 18 — Delimited continuations (`shift`/`reset`)
- Phase 17 — Exceptions

---

## Executive Summary

This document outlines the design and implementation plan for adding **`async`/`await`** syntax to Turmeric. The feature enables ergonomic asynchronous programming for I/O-bound and concurrent tasks while maintaining integration with Turmeric's existing effect system and ownership model.

**Key decisions:**

| Decision | Rationale |
|---|---|
| **Fiber-based model** — `async` creates a fiber (user-space thread) | Leverages Phase 18 delimited continuations; avoids C stack issues |
| **Single-threaded scheduler** (v1) — fibers run on a single OS thread | Simpler implementation; no data races between fibers |
| **Multi-threaded scheduler** (v2) — fibers can run on thread pool | Higher throughput for CPU-bound tasks |
| **`await` desugars to `shift`** | Uses existing CPS infrastructure from Phase 18 |
| **`async` desugars to `reset` + fiber creation** | Minimal new runtime machinery |
| **`Future<T>`** as the return type of `async` blocks | Composable, can be awaited |
| **No implicit thread spawning** — explicit scheduler control | Predictable performance, no hidden overhead |
| **Integration with `defer`** — cleanup on fiber completion | Consistent with existing scope unwind model |

**Sequencing:** This plan is **Phase 21**, scheduled after:
- Phase 18 — Delimited continuations (CPS substrate)
- Phase 19 — Thread primitives (`Arc<T>`, `Mutex<T>`, `Thread`, etc.)
- Phase 20 — Thread pool and higher-level abstractions

The CPS substrate from Phase 18 is the **foundation** for async/await. Without it, implementing async/await would require a separate CPS pass or state machine generation.

---

## 1. Motivation and Goals

### 1.1 Problems Solved by Async/Await

1. **Callback hell** — Nested callbacks for async operations are hard to read and maintain
2. **Manual state machines** — Currently, async I/O requires explicit state tracking
3. **Error propagation** — Errors in async code require manual handling at each level
4. **Composability** — Combining async operations is cumbersome without dedicated syntax

### 1.2 Goals

- Provide **ergonomic syntax** for asynchronous programming
- Maintain **memory safety** with existing ownership model
- Integrate **cleanly with effects** (future v3)
- Have **minimal runtime overhead**
- Support **both I/O-bound and CPU-bound** async patterns
- Enable **structured concurrency** (scoped fiber management)

### 1.3 Non-Goals (v1)

- **No** implicit parallelism — `async` does not automatically spawn threads
- **No** work-stealing scheduler (deferred to v2)
- **No** async I/O primitives in stdlib (users provide via FFI)
- **No** `select!` macro for multi-await (deferred to v2)

---

## 2. Design Overview

### 2.1 Core Concept: Fibers + Delimited Continuations

A **fiber** is a user-space thread that:
- Has its own call stack (implemented via delimited continuations)
- Can **yield** (suspend) and **resume** execution
- Runs on an OS thread managed by a scheduler
- Does **not** have its own OS stack (avoids thread creation overhead)

**Key insight:** Phase 18's `shift`/`reset` already provides the mechanism for capturing and resuming continuations. A fiber is simply a `reset` block that can be suspended with `shift`.

```clojure
;; Conceptually, a fiber is:
(defn fiber [thunk]
  (reset (thunk)))

;; Suspending a fiber:
(defn suspend []
  (shift k
    ;; Store continuation k for later resumption
    (fiber-suspend (current-fiber) k)))

;; Resuming a fiber:
(defn resume [fiber value]
  (let [k (fiber-continuation fiber)]
    (reset (resume k value))))
```

### 2.2 Async/Await as Syntactic Sugar

```clojure
;; Desugaring of async/await:

;; Original:
(async
  (let [a (await (fetch-url "http://example.com"))]
    (let [b (await (fetch-url (str a "/next")))]
      (println b))))

;; Desugars to:
(reset
  (fetch-url "http://example.com"
    (fn [a]
      (reset
        (fetch-url (str a "/next")
          (fn [b]
            (println b)))))))

;; With explicit fiber creation:
(let [fiber (Fiber::new
              (fn []
                (let [a (await (fetch-url "http://example.com"))]
                  (let [b (await (fetch-url (str a "/next")))]
                    (println b)))))]
  (Fiber::start fiber))
```

### 2.3 The `Future<T>` Type

`async` blocks return a `Future<T>` — a handle to a computation that may not have completed yet:

```clojure
(def future (async (compute-answer)))

;; Later, await the result:
(def answer (await future))
```

**Properties:**
- `Future<T>` is **lazy** — computation starts when the future is polled or explicitly started
- `Future<T>` is **`Send`** if `T` is `Send` — can be passed across threads
- `Future<T>` can be **awaited multiple times** (caching the result)
- `Future<T>` can be in states: `Pending`, `Fulfilled(T)`, `Rejected(exn)`

---

## 3. Syntax and Semantics

### 3.1 `async` Blocks

```clojure
;; Basic async block
(def future (async (expensive-computation)))

;; Async block with multiple expressions
(def future (async
  (let [x (compute-x)]
    (let [y (compute-y x)]
      (+ x y))))

;; Async block returns a Future
(type? (async 42))  ; => Future<int>
```

**Semantics:**
- `async` captures the body in a **fiber**
- The body is **not executed immediately** — it runs when the future is polled or started
- The fiber runs on the **current scheduler** (default: current OS thread's event loop)
- Returns a `Future<T>` where `T` is the return type of the body

### 3.2 `await` Expressions

```clojure
;; Await a Future
(def result (await some-future))

;; Await can be used in async blocks
(async
  (let [a (await future-a)]
    (let [b (await future-b)]
      (+ a b))))

;; Await outside async block blocks the current fiber/thread
(def result (await (async 42)))  ; Blocks until future completes
```

**Semantics:**
- `await` **suspends** the current fiber until the future completes
- If called outside an async context (no fiber), it **blocks the OS thread**
- Returns the fulfilled value of the future
- Propagates exceptions: if the future is rejected, `await` throws the exception

### 3.3 `Future<T>` API

```clojure
;; Create a future from a value (already fulfilled)
(def future (Future::of 42))

;; Create a future from an error (already rejected)
(def future (Future::error (exn "Failed")))

;; Check if future is done
(Future::done? future)  ; => bool

;; Get result (blocks if not done)
(Future::get future)  ; => Result<T, exn>

;; Get result with timeout
(Future::get future 1000)  ; => Result<T, exn> or :timeout

;; Await the future (use in async blocks)
(await future)  ; => T (throws if rejected)

;; Map over a future
(def mapped (Future::map future (fn [x] (* x 2))))

;; Chain futures (flat-map)
(def chained (Future::then future (fn [x] (async (* x 2)))))

;; Combine futures
(def combined (Future::join future-a future-b))
;; Returns Future<tuple<Ta, Tb>>

;; Race futures
(def winner (Future::race future-a future-b))
;; Returns Future<T> of whichever completes first

;; All futures must complete
(def all (Future::all [future-a future-b future-c]))
;; Returns Future<vec<T>>

;; Any future completes
(def any (Future::any [future-a future-b future-c]))
;; Returns Future<T> of first to complete
```

### 3.4 Scheduler API

```clojure
;; Get the current scheduler
(def sched (Scheduler::current))

;; Create a new scheduler
(def sched (Scheduler::new))

;; Run a scheduler's event loop
(Scheduler::run sched)

;; Spawn a fiber on a scheduler
(Scheduler::spawn sched (async (compute)))

;; Schedule a callback after a delay
(Scheduler::timeout sched 1000 (fn [] (println "1 second passed")))

;; Yield to the scheduler (allow other fibers to run)
(Scheduler::yield)

;; Park the current fiber (suspend until woken)
(Scheduler::park)

;; Unpark a fiber
(Scheduler::unpark fiber)
```

### 3.5 Structured Concurrency

```clojure
;; Task groups for structured concurrency
(async
  (TaskGroup::with [group]
    (let [t1 (TaskGroup::spawn group (async (task-1)))]
      (let [t2 (TaskGroup::spawn group (async (task-2)))]
        (await t1)
        (await t2)))
    ;; All tasks in group complete when scope exits
    ))

;; Task group cancels all children on error
(async
  (TaskGroup::with [group]
    (TaskGroup::spawn group (async (throw (exn "error"))))
    (TaskGroup::spawn group (async (println "never runs"))))
  ;; Both tasks are cancelled
  )

;; Timeout for a task group
(async
  (TaskGroup::with-timeout [group] 1000
    (TaskGroup::spawn group (async (slow-operation))))
  ;; If slow-operation takes > 1 second, group is cancelled
  )
```

---

## 4. Integration with Delimited Continuations (Phase 18)

### 4.1 `await` as `shift`

The `await` expression is **syntactic sugar for `shift`** with fiber suspension:

```clojure
;; Conceptual lowering of await:

;; Source:
(await future)

;; Lowered to:
(shift k
  (let [fiber (current-fiber)]
    (Future::register-callback future
      (fn [result]
        (Scheduler::enqueue fiber k result)))
    (Scheduler::yield fiber)))
```

**How it works:**
1. `shift` captures the current continuation `k`
2. Register a callback with the future
3. When the future completes, the callback enqueues the fiber with its continuation
4. `Scheduler::yield` suspends the fiber, allowing other fibers to run
5. When the future's callback fires, the scheduler resumes the fiber with `k` and the result

### 4.2 `async` as `reset` + Fiber

```clojure
;; Conceptual lowering of async:

;; Source:
(async body)

;; Lowered to:
(let [fiber (Fiber::new
              (fn []
                (reset body)))]
  (Future::new fiber))
```

**How it works:**
1. Create a new fiber with the body wrapped in `reset`
2. Create a `Future<T>` that tracks the fiber's completion
3. The fiber is **not started automatically** — it starts on first poll or explicit start

### 4.3 Fiber Implementation

A fiber is a **continuation frame** that can be suspended and resumed:

```clojure
;; Fiber state machine:
(def Fiber
  {:id          int
   :status      :created | :running | :suspended | :completed | :errored
   :continuation cont<T>    ; Current continuation (from shift)
   :stack       vec        ; Stack of partial continuations
   :scheduler   Scheduler  ; Owning scheduler
   :result      Result<T, exn>  ; Final result (when completed)
   :error       exn        ; Exception (when errored)
   })

;; Creating a fiber:
(defn Fiber::new [thunk]
  (let [fiber {:id (next-fiber-id!)
               :status :created
               :continuation nil
               :stack []
               :scheduler (Scheduler::current)
               :result nil
               :error nil}]
    (Fiber::init fiber thunk)
    fiber))

;; Initializing a fiber's continuation:
(defn Fiber::init [fiber thunk]
  (set! fiber.continuation
    (call/cc
      (fn [k]
        ;; Wrap thunk in reset to establish continuation boundary
        (reset
          (try
            (let [result (thunk)]
              (Fiber::complete fiber result)
              (resume k result))
            (catch exn e
              (Fiber::error fiber e)
              (resume k (throw e)))))))))

;; Suspending a fiber:
(defn Fiber::suspend [fiber k]
  (set! fiber.status :suspended)
  (set! fiber.continuation k)
  (Scheduler::yield fiber))

;; Resuming a fiber:
(defn Fiber::resume [fiber value]
  (set! fiber.status :running)
  (let [k fiber.continuation]
    (set! fiber.continuation nil)
    (resume k value)))

;; Completing a fiber:
(defn Fiber::complete [fiber result]
  (set! fiber.status :completed)
  (set! fiber.result result)
  ;; Notify any waiters
  (doseq [callback fiber.waiters]
    (callback {:ok result})))

;; Erroring a fiber:
(defn Fiber::error [fiber exn]
  (set! fiber.status :errored)
  (set! fiber.error exn)
  (doseq [callback fiber.waiters]
    (callback {:err exn})))
```

**Note:** This is a **simplified** model. The actual implementation uses the CPS substrate from Phase 18 more directly.

### 4.4 Continuation Capture and Resumption

Phase 18's delimited continuations are **one-shot** — a continuation can be resumed at most once. This is **perfect** for async/await:

- `await` captures the continuation **once**
- When the awaited future completes, the continuation is resumed **once**
- If the future is already complete, the continuation is resumed immediately

**One-shot guarantee:** No continuation is resumed twice, preventing double-execution bugs.

---

## 5. Ownership and Memory Safety

### 5.1 `Future<T>` and `Send`/`Sync`

From [thread-safety-and-primitives-plan.md](thread-safety-and-primitives-plan.md):

| Type | Send | Sync | Rationale |
|------|------|------|-----------|
| `Future<T>` | ✅ if `T: Send` | ✅ if `T: Sync` | Future is a handle; value is immutable once set |
| `Fiber` | ❌ | ❌ | Fibers have captured stack frames |
| `Scheduler` | ✅ | ✅ | Schedulers are thread-safe |

**`Future<T>` is `Send`:** A future can be moved to another thread, and the other thread can await it. The future's value is **immutable** once set.

**`Future<T>` is `Sync`:** Multiple threads can await the same future simultaneously. All awaiters get the same result.

### 5.2 `ref<T>` in Async Context

`ref<T>` is **move-only** and **not `Send`**. In async code:

```clojure
;; OK: ref is moved into the async block
(async
  (let [r (ref 42)]
    (await (async (println (deref r))))
    (deref r)))

;; ERROR: ref is captured by multiple awaits (would require cloning)
(async
  (let [r (ref 42)]
    (await (async (println (deref r))))
    (await (async (println (deref r))))))  ; ERROR: use after move

;; OK: Use Arc<Mutex<T>> for shared state
(async
  (let [shared (Arc::new (Mutex::new 0))]
    (await (async
      (Mutex::lock shared)
      (Mutex::set! shared 42)
      (Mutex::unlock shared)))
    (Mutex::get shared)))
```

### 5.3 Borrow Checking Integration

The borrow checker ensures that:
1. Captured values in async blocks are **`Send`** (can be moved to the fiber's context)
2. Values are not **used after move**
3. References do not **escape their lifetime**

```clojure
;; OK: value is Copy (Send)
(async (let [x 42] (await some-future) x))

;; OK: value is moved
(async (let [r (ref 42)] (deref r)))

;; ERROR: value is not Send
(async (let [r (ref 42)] (await some-future) (deref r)))
;; ERROR: r is captured across await, but ref<T> is not Send

;; OK: wrap in Arc
(async (let [r (Arc::new (Mutex::new 42))] (await some-future) (Mutex::get r)))
```

---

## 6. Scheduler Design

### 6.1 Single-Threaded Scheduler (v1)

The v1 scheduler is **single-threaded** — all fibers run on a single OS thread:

```clojure
(def SingleThreadedScheduler
  {:fibers       queue       ; Run queue (fibers ready to run)
   :waiting      map         ; Map: wake-time -> [fibers]
   :io-waiting   map         ; Map: io-handle -> [fibers]
   :current      fiber       ; Currently running fiber
   :executor     thrd_t      ; OS thread running the scheduler
   })

;; Run loop:
(defn Scheduler::run [sched]
  (loop []
    (if-let [fiber (queue-pop sched.fibers)]
      (do
        (set! sched.current fiber)
        (Fiber::resume fiber)
        (set! sched.current nil)
        (recur))
      (Thread::park))))  ; No fibers to run — wait

;; Yield to scheduler:
(defn Scheduler::yield [fiber]
  (let [sched fiber.scheduler]
    (queue-push sched.fibers fiber)
    ;; Switch to next fiber
    ))

;; Wake a fiber:
(defn Scheduler::wake [fiber]
  (queue-push fiber.scheduler.fibers fiber))
```

**Advantages:**
- Simple implementation
- No data races (all fibers on one thread)
- No need for locks between fibers
- Easy to reason about

**Disadvantages:**
- No parallelism — CPU-bound tasks don't benefit
- I/O-bound tasks can proceed, but CPU is underutilized

### 6.2 Multi-Threaded Scheduler (v2)

The v2 scheduler supports **multiple OS threads** running fibers:

```clojure
(def MultiThreadedScheduler
  {:fibers       atomic-queue  ; Lock-free run queue
   :waiting      priority-queue ; Time-ordered wait queue
   :io-waiting   map           ; IO waiters
   :threads      vec           ; Pool of OS threads
   :num-threads  int           ; Current thread count
   :max-threads  int           ; Maximum thread count
   })

;; Work-stealing: each thread has its own deque
(def ThreadLocalState
  {:run-queue    deque    ; Local run queue
   :steal-queue  deque    ; Queue for stealing (LIFO)
   :stealing     bool     ; Another thread is stealing
   })

;; Scheduling a fiber:
(defn Scheduler::spawn [sched fiber]
  (let [tls (thread-local-state)]
    (deque-push-back tls.run-queue fiber)
    (maybe-wake-thread sched)))

;; Work stealing:
(defn Scheduler::steal [sched thief]
  (let [victim (rand-thread sched)]
    (if-let [fiber (deque-steal victim.steal-queue)]
      fiber
      nil)))

;; Run loop for each thread:
(defn Scheduler::thread-run [sched]
  (loop []
    (if-let [fiber (or (deque-pop-front (thread-local-state).run-queue)
                       (Scheduler::steal sched))]
      (do
        (Fiber::resume fiber)
        (recur))
      (Thread::park))))
```

**Advantages:**
- CPU-bound tasks can run in parallel
- Better throughput for mixed workloads
- Scales with available cores

**Disadvantages:**
- More complex implementation
- Requires synchronization between threads
- Fibers can migrate between OS threads (must be careful with TLS)

### 6.3 Scheduler and Thread Pool Integration

The scheduler can use a **thread pool** from Phase 20:

```clojure
(defn Scheduler::new [& {:max-threads 4}]
  (let [pool (ThreadPool::new max-threads)]
    {:pool pool
     :fibers (atomic-queue)
     :waiting (priority-queue)
     :io-waiting {}}))

;; Submit a fiber to the scheduler:
(defn Scheduler::spawn [sched fiber]
  (atomic-queue-push sched.fibers fiber)
  (ThreadPool::submit sched.pool
    (fn [] (Scheduler::run-one sched))))
```

---

## 7. I/O Integration

### 7.1 Async I/O Model

Turmeric does **not** provide built-in async I/O primitives. Instead, users provide them via FFI or libraries:

```clojure
;; Example: async file read using libuv via FFI
(extern-c uv_fs_read [^ptr<uv_fs> req ^ptr<uv_file> file ^ptr<u8> buf ^usize size ^usize offset ^ptr<uv_fs_cb> cb] : int)

(defn async-read-file [path]
  (async
    (let [buf (malloc 4096)
          req (malloc (sizeof uv_fs_t))
          result (ref nil)]
      (uv_fs_read req (open-file path) buf 4096 0
        (extern-c-callback [req status]
          (if (= status 0)
            (Future::fulfill result buf)
            (Future::reject result (exn "Read failed")))))
      (defer (free buf) (free req))
      (await result))))
```

### 7.2 Scheduler I/O Integration

The scheduler needs to integrate with I/O event loops:

```clojure
;; Register I/O interest:
(defn Scheduler::io-wait [sched fd interest callback]
  ;; interest: :read, :write, :readwrite
  (let [fiber (current-fiber)]
    (add-to-io-waiting sched fd interest fiber)
    (set-fd-callback fd interest
      (fn []
        (remove-from-io-waiting sched fd interest)
        (callback)
        (Scheduler::wake fiber)))))
```

### 7.3 Timer Integration

```clojure
;; Sleep in async context:
(defn async-sleep [ms]
  (async
    (let [result (ref nil)]
      (Scheduler::timeout (current-scheduler) ms
        (fn [] (Future::fulfill result nil)))
      (await result))))

;; Timeout for any future:
(defn Future::timeout [future ms]
  (async
    (let [sleep (async-sleep ms)]
      (select
        (future [v] v)
        (sleep [] (throw (exn "Timeout")))))))
```

---

## 8. Error Handling

### 8.1 Exception Propagation

Exceptions in async code propagate through futures:

```clojure
(async
  (throw (exn "error")))  ; Future is rejected

;; Awaiting a rejected future throws:
(await (async (throw (exn "error"))))  ; throws exn "error"
```

### 8.2 Exception Handling in Async

```clojure
(async
  (try
    (await risky-future)
    (catch exn e
      (println "Caught:" e)
      :recovered)))

;; Or using Result type:
(async
  (case (await risky-future)
    (:ok v) v
    (:err e) :recovered))
```

### 8.3 Cancellation

Futures can be **cancelled** if they haven't started or are still running:

```clojure
(def future (async (long-running-task)))

;; Cancel the future
(Future::cancel future)

;; Awaiting a cancelled future throws:
(await future)  ; throws exn "Future was cancelled"
```

**Cancellation semantics:**
- If the fiber hasn't started, it never runs
- If the fiber is running, it's **cooperatively cancelled** — the fiber must check for cancellation
- Cancellation is **not preemptive** — fibers must yield to be cancelled

```clojure
(async
  (loop [i 0]
    (if (Fiber::cancelled?)
      (throw (exn "Cancelled"))
      (do
        (perform-step i)
        (await (async-sleep 100))
        (recur (inc i))))))
```

---

## 9. Implementation Plan

### 9.1 Phase 21: Async/Await Core (v1)

**Goal:** Add `async`/`await` syntax and single-threaded scheduler.

**Dependencies:**
- Phase 18 — Delimited continuations (`shift`/`reset`)
- Phase 17 — Exceptions
- Phase 5 — `ref<T>` ownership model

**Deliverables:**

| ID | Task | Priority | Effort | Dependencies |
|----|------|----------|--------|--------------|
| AW-001 | Add `async`/`await` syntax to reader | P0 | 1 day | None |
| AW-002 | Add `Future<T>` type and API | P0 | 2 days | Phase 2 (type system) |
| AW-003 | Implement single-threaded scheduler | P0 | 3 days | Phase 18 |
| AW-004 | Lower `await` to `shift` + scheduler integration | P0 | 3 days | AW-003, Phase 18 |
| AW-005 | Lower `async` to fiber creation | P0 | 2 days | AW-004 |
| AW-006 | Implement `Future::map`, `Future::then`, `Future::join` | P0 | 2 days | AW-002 |
| AW-007 | Implement `Future::all`, `Future::race`, `Future::any` | P1 | 1 day | AW-006 |
| AW-008 | Implement `Future::timeout` | P1 | 1 day | AW-007 |
| AW-009 | Implement `Scheduler::yield`, `Scheduler::run` | P0 | 2 days | AW-003 |
| AW-010 | Implement `Scheduler::timeout` | P1 | 1 day | AW-009 |
| AW-011 | `Send`/`Sync` trait implementations for `Future<T>` | P0 | 1 day | AW-002, Phase 19 |
| AW-012 | Borrow checker integration for async closures | P0 | 2 days | AW-001, Phase 5 |
| AW-013 | Fixtures: `async-basic.tur`, `await-basic.tur`, `future-basic.tur` | P0 | 2 days | All above |

**Total effort:** ~23-25 days

### 9.2 Phase 22: Structured Concurrency and Task Groups (v1.1)

**Goal:** Add structured concurrency primitives.

**Dependencies:**
- Phase 21 — Async/await core

**Deliverables:**

| ID | Task | Priority | Effort | Dependencies |
|----|------|----------|--------|--------------|
| SC-001 | Implement `TaskGroup` type | P0 | 2 days | Phase 21 |
| SC-002 | Implement `TaskGroup::with` macro | P0 | 1 day | SC-001 |
| SC-003 | Implement `TaskGroup::spawn` | P0 | 2 days | SC-001 |
| SC-004 | Implement `TaskGroup::cancel` | P0 | 1 day | SC-003 |
| SC-005 | Implement `TaskGroup::with-timeout` | P1 | 2 days | SC-004 |
| SC-006 | Cancellation propagation (child to parent) | P0 | 2 days | SC-004 |
| SC-007 | Fixtures: `taskgroup-basic.tur`, `taskgroup-cancel.tur` | P0 | 2 days | All above |

**Total effort:** ~12 days

### 9.3 Phase 23: Multi-Threaded Scheduler and Work Stealing (v2)

**Goal:** Add multi-threaded scheduler with work-stealing.

**Dependencies:**
- Phase 21 — Async/await core
- Phase 19 — Thread primitives
- Phase 20 — Thread pool

**Deliverables:**

| ID | Task | Priority | Effort | Dependencies |
|----|------|----------|--------|--------------|
| MT-001 | Implement work-stealing scheduler | P0 | 4 days | Phase 21, Phase 20 |
| MT-002 | Implement per-thread run queues | P0 | 2 days | MT-001 |
| MT-003 | Implement fiber migration between threads | P0 | 3 days | MT-002 |
| MT-004 | Implement lock-free queue for cross-thread communication | P1 | 3 days | MT-003 |
| MT-005 | Thread pool integration | P0 | 2 days | MT-001, Phase 20 |
| MT-006 | I/O integration with multi-threaded scheduler | P1 | 2 days | MT-001 |
| MT-007 | `Fiber::thread-id` for debugging | P2 | 1 day | MT-003 |
| MT-008 | Fixtures: `scheduler-multithread.tur`, `workstealing.tur` | P0 | 2 days | All above |

**Total effort:** ~19 days

### 9.4 Phase 24: Async I/O and Timer Integration (v2.1)

**Goal:** Add built-in async I/O and timer support in stdlib.

**Dependencies:**
- Phase 21 — Async/await core
- Phase 23 — Multi-threaded scheduler

**Deliverables:**

| ID | Task | Priority | Effort | Dependencies |
|----|------|----------|--------|--------------|
| IO-001 | `async-sleep` primitive | P0 | 1 day | Phase 21 |
| IO-002 | Timer wheel for efficient timeouts | P1 | 2 days | IO-001 |
| IO-003 | `AsyncFile` type for async file I/O | P0 | 3 days | Phase 23 |
| IO-004 | `AsyncSocket` type for async network I/O | P0 | 5 days | Phase 23 |
| IO-005 | `AsyncPipe` for async stdin/stdout | P1 | 2 days | Phase 23 |
| IO-006 | `select!` macro for multi-await | P1 | 3 days | Phase 21 |
| IO-007 | Fixtures for async I/O | P0 | 3 days | All above |

**Total effort:** ~19 days

### 9.5 Phase 25: Effects + Async/Await Integration (v3)

**Goal:** Full integration with the algebraic effects system from [effects-plan.md](effects-plan.md).

**Dependencies:**
- Phase 21 — Async/await core
- [effects-plan.md](effects-plan.md) — Algebraic effects

**Deliverables:**

| ID | Task | Priority | Effort | Dependencies |
|----|------|----------|--------|--------------|
| EF-001 | Effect handlers in async context | P0 | 3 days | Phase 21, effects |
| EF-002 | `Async` effect for async operations | P0 | 2 days | EF-001 |
| EF-003 | `Await` effect for awaiting futures | P0 | 2 days | EF-002 |
| EF-004 | Effect handlers can spawn fibers | P1 | 3 days | EF-001 |
| EF-005 | `async` blocks as effect handlers | P1 | 2 days | EF-004 |
| EF-006 | Fixtures: `effects-async.tur` | P0 | 2 days | All above |

**Total effort:** ~14 days

---

## 10. Lowering to C

### 10.1 Fiber Representation in C

```c
// A fiber in C
typedef struct tur_fiber {
    tur_cont *cont;           // Current continuation (from shift)
    enum tur_fiber_status {
        TUR_FIBER_CREATED,
        TUR_FIBER_RUNNING,
        TUR_FIBER_SUSPENDED,
        TUR_FIBER_COMPLETED,
        TUR_FIBER_ERRORED,
        TUR_FIBER_CANCELLED
    } status;
    tur_value result;         // Result value
    tur_exn *error;           // Exception if errored
    struct tur_scheduler *sched;  // Owning scheduler
    tur_fiber *next;          // Next in run queue
    bool cancelled;           // Cancellation flag
} tur_fiber;

// Future representation
typedef struct tur_future {
    enum tur_future_status {
        TUR_FUTURE_PENDING,
        TUR_FUTURE_FULFILLED,
        TUR_FUTURE_REJECTED,
        TUR_FUTURE_CANCELLED
    } status;
    tur_value value;          // Result value
    tur_exn *error;           // Exception if rejected
    tur_fiber *fiber;         // Associated fiber (if any)
    tur_callback *on_complete; // Completion callbacks
} tur_future;
```

### 10.2 Scheduler Representation in C

```c
// Single-threaded scheduler
typedef struct tur_scheduler {
    tur_fiber *run_queue;     // Queue of runnable fibers
    tur_fiber **waiting;      // Array of waiting fibers (for timeouts)
    size_t waiting_len;
    size_t waiting_cap;
    tur_fiber **io_waiting;   // Array of fibers waiting on I/O
    size_t io_waiting_len;
    thrd_t thread;            // OS thread running the scheduler
    bool running;             // Is the scheduler running?
    mtx_t lock;               // Lock for thread safety (v2)
} tur_scheduler;
```

### 10.3 Lowering Example: Async Block

```clojure
;; Source:
(async (let [a (await future-a)]
         (+ a 1)))

;; Lowered IR (conceptual):
(block
  (let [future (Future::new)]
    (let [fiber (Fiber::new
                  (fn []
                    (reset
                      (let [a (shift k
                                 (Future::register-callback future-a
                                   (fn [result]
                                     (Scheduler::enqueue (current-fiber) k result)))
                                 (Scheduler::yield (current-fiber)))]
                        (+ a 1)))))]
      (Future::associate future fiber)
      future)))

;; Generated C (simplified):
void async_block_thunk(tur_fiber *fiber) {
    tur_value a;
    
    // await future-a
    tur_future *fut = future_a;
    if (fut->status == TUR_FUTURE_FULFILLED) {
        a = fut->value;
    } else if (fut->status == TUR_FUTURE_REJECTED) {
        tur_fiber_set_error(fiber, fut->error);
        return;
    } else {
        // Suspend the fiber
        tur_future_register_callback(fut, async_block_cont, fiber);
        fiber->status = TUR_FIBER_SUSPENDED;
        tur_scheduler_yield(fiber->sched, fiber);
        return;
    }
    
    // (+ a 1)
    tur_value result = tur_add(fiber->env, a, TUR_INT(1));
    tur_fiber_complete(fiber, result);
}

// Continuation for when future-a completes
void async_block_cont(tur_future *fut, tur_fiber *fiber) {
    if (fut->status == TUR_FUTURE_FULFILLED) {
        // Resume with the value
        fiber->cont(fiber, fut->value);
    } else {
        // Resume with error
        fiber->cont(fiber, TUR_ERROR(fut->error));
    }
}
```

### 10.4 CPS Integration

Since Phase 18 already has CPS infrastructure, `async`/`await` can leverage it directly:

```clojure
;; With CPS:
(async
  (let [a (await future-a)]
    (let [b (await future-b)]
      (+ a b))))

;; Lowered using CPS:
(defn async-body [k]
  (Future::register-callback future-a
    (fn [a]
      (Future::register-callback future-b
        (fn [b]
          (k (+ a b)))))))
```

The CPS pass from Phase 18 handles the continuation transformation automatically.

---

## 11. Testing Strategy

### 11.1 Unit Tests

Each component has dedicated fixtures:

- `async-basic.tur` — Basic async blocks
- `await-basic.tur` — Basic await expressions
- `future-basic.tur` — Future API
- `future-combinators.tur` — `map`, `then`, `join`, `all`, `race`
- `scheduler-basic.tur` — Single-threaded scheduler
- `scheduler-multithread.tur` — Multi-threaded scheduler
- `taskgroup-basic.tur` — Task group basics
- `taskgroup-cancel.tur` — Task group cancellation
- `async-error.tur` — Error handling in async
- `async-cancel.tur` — Cancellation

### 11.2 Integration Tests

- `async-fizzbuzz.tur` — Async FizzBuzz
- `async-http.tur` — Simulated HTTP client with async
- `async-echo-server.tur` — Echo server using async I/O
- `parallel-map.tur` — Parallel map using async
- `async-pipeline.tur` — Async data pipeline

### 11.3 Stress Tests

- `async-stress.tur` — 1000 concurrent async tasks
- `await-stress.tur` — Deeply nested awaits
- `scheduler-stress.tur` — Scheduler with 10000 fibers
- `taskgroup-stress.tur` — Nested task groups

### 11.4 Sanitizer Testing

All tests run with:
- **ThreadSanitizer (TSan)** — Detects data races in multi-threaded scheduler
- **AddressSanitizer (ASan)** — Detects memory errors
- **UndefinedBehaviorSanitizer (UBSan)** — Detects UB

```sh
make test TSAN=1   # Run tests with ThreadSanitizer
```

---

## 12. Performance Considerations

### 12.1 Overhead

| Operation | Overhead | Notes |
|-----------|----------|-------|
| `async` block creation | ~100ns | Fiber allocation + continuation setup |
| `await` (future complete) | ~20ns | Direct continuation resume |
| `await` (future pending) | ~100ns | Suspend fiber + enqueue callback |
| Fiber switch | ~50ns | Continuation capture + resume |
| Scheduler yield | ~20ns | Queue operation |
| Scheduler run | ~10ns/fiber | Per-fiber overhead |

### 12.2 Optimizations

1. **Fiber pooling** — Reuse fiber allocations
2. **Inline small async blocks** — Avoid fiber allocation for trivial async
3. **Batch callbacks** — Process multiple future callbacks at once
4. **Lock-free queues** — For multi-threaded scheduler
5. **CPS optimization** — Leverage existing CPS optimizations from Phase 18

### 12.3 Benchmarks

- **Async throughput** — Tasks per second for I/O-bound workloads
- **CPU utilization** — For CPU-bound async workloads
- **Memory usage** — Fibers per MB, overhead per fiber
- **Context switch** — Time to switch between fibers

---

## 13. Portability

### 13.1 Platform Support

| Platform | Async/Await | Notes |
|----------|-------------|-------|
| Linux | ✅ | Full support |
| macOS | ✅ | Full support |
| Windows | ✅ | Full support (via MinGW or MSVC) |
| FreeBSD | ✅ | Full support |
| OpenBSD | ⚠️ | Less tested |
| WebAssembly | ❌ | No thread support (future) |

### 13.2 Fallback for Non-C11 Platforms

For platforms without C11 threads:
- **Single-threaded scheduler works** without threads
- **No multi-threaded scheduler** — only single-threaded mode
- **No async I/O** — only manual polling

---

## 14. Comparison with Other Languages

| Feature | Turmeric | Rust (async-std) | JavaScript | Python (asyncio) | C# |
|---------|----------|------------------|------------|------------------|----|
| Model | Fibers + continuations | Fibers (tasks) | Event loop | Event loop | Tasks |
| Syntax | `async`/`await` | `async`/`await` | `async`/`await` | `async`/`await` | `async`/`await` |
| Scheduler | Single/multi-threaded | Multi-threaded | Single | Single | Thread pool |
| Work stealing | v2 | ✅ | ❌ | ❌ | ✅ |
| Structured concurrency | v1.1 | ✅ (tokio) | ❌ | ✅ (trio) | ❌ |
| Cancellation | Cooperative | Cooperative | ❌ | Cooperative | Cooperative |
| Stack | Heap-allocated | Heap-allocated | JS stack | Heap-allocated | Heap-allocated |
| Preemption | ❌ | ❌ | ❌ | ✅ (trio) | ❌ |

---

## 15. Risks and Mitigations

| Risk | Likelihood | Impact | Mitigation |
|------|------------|--------|------------|
| Stack overflow in deep async | Low | Medium | Limit fiber stack depth |
| Memory leaks from fibers | Medium | Medium | Fiber pooling, proper cleanup |
| Data races in multi-threaded scheduler | Medium | High | Careful design, TSan testing |
| Performance overhead | Medium | Medium | Benchmarking, optimization passes |
| Complexity of CPS integration | Medium | High | Build incrementally on Phase 18 |
| Interaction with effects | Medium | High | Defer to v3, design carefully |
| Debugging difficulty | High | Medium | Good error messages, fiber tracing |

---

## 16. Appendix: Example Programs

### 16.1 Sequential Async

```clojure
(defn main []
  (let [result (async
                (let [a (await (async-fetch "http://a.com"))]
                  (let [b (await (async-fetch (str a "/b")))]
                    b)))]
    (println (await result))))
```

### 16.2 Parallel Async

```clojure
(defn main []
  (let [a-future (async-fetch "http://a.com")
        b-future (async-fetch "http://b.com")]
    (let [result (async
                  (let [a (await a-future)
                        b (await b-future)]
                    [a b]))]
      (println (await result)))))
```

### 16.3 Task Group with Cancellation

```clojure
(defn main []
  (TaskGroup::with [group]
    (let [t1 (TaskGroup::spawn group (async (fetch-url "http://a.com")))
          t2 (TaskGroup::spawn group (async (fetch-url "http://b.com")))]
      (TaskGroup::with-timeout [group] 1000
        (await t1)
        (await t2)))))
```

### 16.4 Async HTTP Server

```clojure
(defn handle-request [req]
  (async
    (let [path (req-path req)]
      (case path
        "/" (await (async-file-read "index.html"))
        "/api" (await (async-db-query req))
        (await (async-file-read "404.html"))))))

(defn main []
  (let [server (AsyncServer::new 8080 handle-request)]
    (AsyncServer::listen server)
    (Scheduler::run (Scheduler::new))))
```

### 16.5 Producer-Consumer with Channels

```clojure
(defn producer [ch n]
  (async
    (dotimes [i n]
      (Chan::send ch i)
      (await (async-sleep 100)))
    (Chan::close ch))

(defn consumer [ch]
  (async
    (loop []
      (case (Chan::recv ch)
        (:ok v) (do (println "Got:" v) (recur))
        (:err _) (println "Done"))))))

(defn main []
  (let [ch (Chan::new)]
    (let [p (producer ch 10)
          c (consumer ch)]
      (await p)
      (await c))))
```

### 16.6 Async Pipeline

```clojure
(defn pipeline [input output transform]
  (async
    (loop []
      (case (Chan::recv input)
        (:ok v) (do
                  (Chan::send output (transform v))
                  (recur))
        (:err _) (Chan::close output)))))

(defn main []
  (let [in-ch (Chan::new)
        out-ch (Chan::new)]
    (Chan::send in-ch 1)
    (Chan::send in-ch 2)
    (Chan::send in-ch 3)
    (Chan::close in-ch)
    
    (let [p (pipeline in-ch out-ch (fn [x] (* x 2)))]
      (async
        (await p)
        (loop []
          (case (Chan::recv out-ch)
            (:ok v) (do (println "Output:" v) (recur))
            (:err _) (println "Done")))))))))
```
