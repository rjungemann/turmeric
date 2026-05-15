# Threading Guide -- Documentation Tasks

Six concurrency features are implemented in stdlib but either missing from or
misrepresented in `docs/guides/threading-guide.md`. Each task below covers what
to write and how to update the guide when done.

---

## Task 1 -- Channels (`stdlib/chan.tur`)

**Status:** Implemented (Phase T19-C/D). Guide currently says "Planned for
Phase 20+; currently available via FFI."

### What to document

**Synchronous channel (`Chan`)** -- blocking send/recv, no try variants.

| Function | Signature | Notes |
|---|---|---|
| `chan-new` | `[cap :int] :ptr<void>` | Allocates ring-buffer with given capacity |
| `chan-send` | `[ch val :int] :nil` | Blocks when full |
| `chan-recv` | `[ch] :int` | Blocks when empty |
| `chan-free` | `[ch] :nil` | Destroys mutex/condvar, frees buffer |

**Async buffered channel (`AsyncChan`)** -- same ring-buffer struct, adds
non-blocking try variants.

| Function | Signature | Notes |
|---|---|---|
| `async-chan-new` | `[cap :int] :ptr<void>` | |
| `async-chan-send` | `[ch val :int] :nil` | Blocks when full |
| `async-chan-recv` | `[ch] :int` | Blocks when empty |
| `async-chan-try-send` | `[ch val :int] :bool` | Returns false if full |
| `async-chan-try-recv` | `[ch] :int` | Returns `INT64_MIN` if empty |
| `async-chan-count` | `[ch] :int` | Current item count (locked) |
| `async-chan-free` | `[ch] :nil` | |

### Example to include

```turmeric
;; Synchronous producer-consumer
(def ch (chan-new 8))

(thread
  (fn []
    (chan-send ch 1)
    (chan-send ch 2)
    (chan-send ch 3)))

(println (chan-recv ch))  ; => 1
(println (chan-recv ch))  ; => 2
(println (chan-recv ch))  ; => 3
(chan-free ch)

;; Non-blocking try with AsyncChan
(def ach (async-chan-new 4))
(async-chan-send ach 99)
(if (async-chan-try-send ach 100) ...)          ; true
(let [v (async-chan-try-recv ach)] ...)         ; 99 (or INT64_MIN if empty)
(println (async-chan-count ach))                ; current queue depth
(async-chan-free ach)
```

### Guide changes

- Replace the "Channels: Send/Receive" stub under "Synchronization Primitives"
  with a full section covering both `Chan` and `AsyncChan`.
- Replace the "Producer-Consumer (via Channels)" pattern stub with a real
  example using `chan-new` / `chan-send` / `chan-recv`.
- Remove the "(Planned for Phase 20+; currently available via FFI.)" note.

---

## Task 2 -- Thread Pools (`stdlib/threadpool.tur`)

**Status:** Implemented (Phase T20-B and T20-D). Guide currently says "Planned
for Phase 20; currently available via FFI."

### What to document

**WorkQueue** -- thread-safe FIFO; two variants.

| Function | Signature | Notes |
|---|---|---|
| `work-queue-new` | `[] :ptr<void>` | Unbounded, grows dynamically |
| `work-queue-new-bounded` | `[cap :int] :ptr<void>` | Fixed ring-buffer; push blocks when full |
| `work-queue-push` | `[q v :int] :nil` | Blocks on bounded queue when full |
| `work-queue-pop` | `[q] :int` | Blocks until item available; returns `INT64_MIN` after close |
| `work-queue-close` | `[q] :nil` | Wakes all blocked producers and consumers |
| `work-queue-free` | `[q] :nil` | |

**Fixed ThreadPool** -- `n` worker threads, tasks submitted as C function pointers,
results delivered via `Future`.

| Function | Signature | Notes |
|---|---|---|
| `thread-pool-new` | `[n :int] :ptr<void>` | Spawns `n` workers immediately |
| `thread-pool-submit` | `[tp task-fn task-arg] :ptr<void>` | Returns a `FutureCell*` |
| `thread-pool-shutdown` | `[tp] :nil` | Closes queue, joins all workers |
| `thread-pool-free` | `[tp] :nil` | Must be called after shutdown |

**Auto-scaling ThreadPool** -- starts at `min-threads`, grows to `max-threads`
when all workers are busy.

| Function | Signature | Notes |
|---|---|---|
| `thread-pool-new-dynamic` | `[min max :int] :ptr<void>` | |
| `thread-pool-dynamic-submit` | `[tp task-fn task-arg] :ptr<void>` | Spawns new worker if idle count is 0 and below max |
| `thread-pool-dynamic-shutdown` | `[tp] :nil` | |
| `thread-pool-dynamic-free` | `[tp] :nil` | |

### Example to include

```turmeric
;; Fixed pool -- submit work and await results
(def tp (thread-pool-new 4))

(def fut (thread-pool-submit tp my-work-fn my-arg))
(def result (future-get fut))  ; blocks until worker completes

(thread-pool-shutdown tp)
(thread-pool-free tp)

;; Auto-scaling pool
(def dtp (thread-pool-new-dynamic 2 8))
(def fut2 (thread-pool-dynamic-submit dtp my-work-fn nil))
(thread-pool-dynamic-shutdown dtp)
(thread-pool-dynamic-free dtp)
```

### Guide changes

- Replace the "Thread Pools" section (currently a single "Planned" line) with
  a full section covering WorkQueue, fixed pool, and auto-scaling pool.
- Note the relationship between `thread-pool-submit` and `Future` (cross-reference
  the Future/Promise section added in Task 6).
- Remove the "(Planned for Phase 20)" note.

---

## Task 3 -- TaskGroup (`stdlib/taskgroup.tur`)

**Status:** Implemented (Phase T22). Not mentioned in the guide at all.

### What to document

TaskGroup provides structured concurrency: spawn a group of fibers, then wait
for or cancel all of them together. Cancellation is cooperative -- tasks must
periodically check `fiber-cancelled?` / `task-group-should-exit?`.

**Lifecycle**

| Function | Notes |
|---|---|
| `task-group-new` | Allocates empty group |
| `task-group-spawn [group f]` | Increments task count; returns fiber handle |
| `task-group-task-done [group]` | Each spawned task must call this on exit |
| `task-group-join [group handle]` | Wait for one specific fiber |
| `task-group-wait [group]` | Block until all tasks complete |
| `task-group-free [group]` | |

**Macros (preferred API)**

| Macro | Notes |
|---|---|
| `task-group-with [group & body]` | Runs body, then calls `task-group-wait` |
| `task-group-with-timeout [group ms & body]` | Auto-cancels group after `ms` milliseconds |
| `task-group-with-cancellation [group & body]` | Skips body if already cancelled |

**Cancellation**

| Function | Notes |
|---|---|
| `task-group-cancel [group]` | Manual cancel; reason = 0 |
| `task-group-cancel-with-reason [group reason]` | 0=manual 1=panic 2=timeout 3=error |
| `task-group-cancel-panic/timeout/error [group]` | Convenience wrappers |
| `task-group-cancel-reason [group]` | Returns reason code |
| `task-group-cancelled? [group]` | Check group-level cancel flag |
| `fiber-cancelled?` | Check thread-local cancel flag |
| `task-group-should-exit? [group]` | Combines both checks |
| `fiber-should-exit?` | Alias for `fiber-cancelled?` |

**Polling**

| Function | Notes |
|---|---|
| `task-group-done? [group]` | Non-blocking group completion check |
| `task-handle-done? [handle]` | Non-blocking per-fiber check |

**Async integration**

| Function / Macro | Notes |
|---|---|
| `task-group-spawn-async [group f]` | Spawns fiber and returns a `Future` |
| `task-group-async [group thunk]` | Macro wrapping `task-group-spawn-async` |

### Examples to include

```turmeric
;; Basic structured concurrency
(def g (task-group-new))

(task-group-spawn g
  (fn []
    (println "worker A")
    (task-group-task-done g)))

(task-group-spawn g
  (fn []
    (println "worker B")
    (task-group-task-done g)))

(task-group-wait g)
(task-group-free g)

;; With macro (preferred)
(def g (task-group-new))
(task-group-with g
  (task-group-spawn g my-fiber-a)
  (task-group-spawn g my-fiber-b))

;; Cooperative cancellation
(def g (task-group-new))
(task-group-spawn g
  (fn []
    (while (not (task-group-should-exit? g))
      (do-work))
    (task-group-task-done g)))
(task-group-cancel g)
(task-group-wait g)
(task-group-free g)

;; Timeout
(def g (task-group-new))
(task-group-with-timeout g 5000
  (task-group-spawn g long-running-task))
```

### Guide changes

- Add a new top-level "Structured Concurrency: TaskGroup" section after the
  "Thread Pools" section.
- Include all lifecycle, cancellation, polling, and async-integration tables.
- Note that panic propagation is automatic when a fiber has a `task_group` set
  (built into the runtime's `tur_fiber_shim`).

---

## Task 4 -- Multi-Channel Select (`stdlib/select.tur`)

**Status:** Implemented (Phase T19-D). Not mentioned in the guide at all.

### What to document

`select` waits on multiple channel operations simultaneously and executes the
first one that is ready.

**Macro syntax**

```turmeric
(select
  (ch1 :recv)            ; receive from ch1
  (ch2 :send 42)         ; send 42 to ch2
  (:default expr))       ; optional non-blocking fallback
```

Returns `(index value)`:
- `index` is 0-based position of the clause that fired, or `-1` for `:default`
- `value` is the received value for `:recv`, `true` for `:send`, or the
  `expr` result for `:default`

**Implementation note (v1 limitation):** The blocking path (when no clause is
immediately ready and no `:default`) blocks on the first channel only. True
fair multi-channel blocking is planned for a future phase.

### Example to include

```turmeric
(def ch-a (chan-new 4))
(def ch-b (chan-new 4))

;; Non-blocking poll with default
(let [[idx val] (select
                  (ch-a :recv)
                  (ch-b :recv)
                  (:default :nothing))]
  (cond
    (= idx 0) (println (str "received from ch-a: " val))
    (= idx 1) (println (str "received from ch-b: " val))
    :else     (println "nothing ready")))

;; Send-or-default
(select
  (ch-a :send 99)
  (:default (println "ch-a full, dropping")))
```

### Guide changes

- Add a "Multi-Channel Select" subsection inside "Synchronization Primitives",
  after the condition variable subsection.
- Document the v1 blocking limitation clearly.

---

## Task 5 -- Once and Semaphore (`stdlib/sync.tur`)

**Status:** Implemented (Phase T19-C). Not mentioned in the guide at all.

### What to document

**Once** -- guarantees an initializer runs exactly once across all threads.

| Function | Notes |
|---|---|
| `once-flag-new` | Allocates a `pthread_once_t` on the heap |
| `once-call [flag init-fn]` | Calls `init-fn` at most once; safe under concurrent calls |
| `once-flag-free [flag]` | Frees the flag |

**Semaphore** -- counting semaphore (portable; `pthread_sem_t` unnamed form is
unavailable on macOS).

| Function | Notes |
|---|---|
| `sem-new [initial]` | `initial=0` starts locked; `initial=1` is a binary semaphore |
| `sem-acquire [s]` | Decrements; blocks when count is 0 |
| `sem-release [s]` | Increments; wakes one blocked acquirer |
| `sem-free [s]` | |

### Examples to include

```turmeric
;; Once -- safe lazy initialization
(def flag (once-flag-new))
(def resource nil)

(defn init-resource []
  (set! resource (load-expensive-data)))

;; Safe to call from many threads
(once-call flag init-resource)

;; Semaphore -- limit concurrency to N parallel workers
(def sem (sem-new 3))  ; at most 3 concurrent

(thread
  (fn []
    (sem-acquire sem)
    (do-work)
    (sem-release sem)))
```

### Guide changes

- Add a "One-Time Initialization: Once" subsection and a "Semaphore" subsection
  under "Synchronization Primitives".

---

## Task 6 -- Future and Promise (`stdlib/future.tur`)

**Status:** Implemented (Phase T20-C). Not mentioned in the guide at all.

### What to document

Promise is the producer; Future is the consumer. Both share a single
heap-allocated `FutureCell` (mutex + condvar + value/exn + settled flag).

**Core API**

| Function | Notes |
|---|---|
| `promise-new` | Returns a shared `FutureCell*` |
| `promise-fulfill [p v]` | Settles ok; aborts if already settled |
| `promise-fail [p e]` | Settles with error code `e`; aborts if already settled |
| `promise-free [p]` | |
| `future-get [f]` | Blocks; returns a heap `Result*` (use `ok?`/`ok-val`/`err-val`) |
| `future-done? [f]` | Non-blocking check |
| `future-cancel [f]` | Settles with `exn = -2` if not yet settled |
| `future-cancelled? [f]` | Returns true if `exn == -2` |
| `future-free [f]` | |

**Pre-settled futures**

| Function | Notes |
|---|---|
| `future-of [v]` | Already fulfilled with `v` |
| `future-error-of [e]` | Already rejected with `e` |

**Combinators**

| Function | Notes |
|---|---|
| `future-map [f fn]` | Maps `fn` over fulfilled value; propagates rejection |
| `future-then [f fn]` | Flat-map; `fn` must return a new `Future` |

**Multi-combinators**

| Function | Notes |
|---|---|
| `future-race [fa fb]` | First to settle wins |
| `future-all2 [fa fb]` | Both must succeed; result carries `fa`'s value |
| `future-any2 [fa fb]` | First to fulfill wins; both rejecting yields `fb`'s error |
| `future-join [fa fb]` | Both must succeed; result is a `TurTuple2` |
| `tuple-first / tuple-second / tuple-free` | Accessors for `future-join` result |

**Variadic**

| Function | Notes |
|---|---|
| `future-race-n [futures n]` | Array of `FutureCell*` pointers |
| `future-all-n [futures n]` | All must succeed |
| `future-any-n [futures n]` | First to fulfill wins |

**Timeouts**

| Function | Notes |
|---|---|
| `future-timeout [ms]` | Settles with `exn = -1` after `ms` milliseconds |
| `future-with-timeout [f ms]` | Races `f` against a `ms`-ms deadline |

### Examples to include

```turmeric
;; Basic promise/future pair
(def p (promise-new))
(def f p)  ; same cell -- both producer and consumer views

(thread (fn [] (promise-fulfill p 42)))

(def result (future-get f))   ; blocks
(println (ok? result))         ; => true
(println (ok-val result))      ; => 42
(promise-free p)

;; Pre-settled
(def f (future-of 99))
(future-done? f)               ; => true immediately

;; Combinator chain
(def f2 (future-map f (fn [v] (* v 2))))  ; => future of 198

;; Race with timeout
(def task-future (run-async-task))
(def result (future-get (future-with-timeout task-future 5000)))
(if (future-cancelled? result)
    (println "timed out")
    (println (ok-val result)))

;; Join two futures
(def tup-future (future-join fa fb))
(def tup-result (future-get tup-future))
(when (ok? tup-result)
  (let [tup (ok-val tup-result)]
    (println (tuple-first tup))
    (println (tuple-second tup))
    (tuple-free tup)))
```

### Guide changes

- Add a top-level "Futures and Promises" section near the end, before "See Also".
- Cover core API, pre-settled shortcuts, combinators, variadic combinators, and
  timeouts.
- Cross-reference the thread pool section (Task 2) since `thread-pool-submit`
  returns a `Future`.
- Cross-reference the TaskGroup async integration (Task 3).

---

## Task 7 -- Update the Guide (do after Tasks 1--6)

Once all sections above are written, make these structural changes to
`docs/guides/threading-guide.md`:

1. **Remove all "Planned" stubs** -- Channels, Thread Pools, and Producer-Consumer
   pattern sections currently defer to FFI or mark themselves as future work.
   Replace every such note with the real content from Tasks 1 and 2.

2. **Add new top-level sections** in this order after "Atomic Types":
   - Future and Promise (Task 6)
   - Multi-Channel Select (Task 4)
   - TaskGroup / Structured Concurrency (Task 3)
   - Thread Pools (Task 2, replacing stub)
   - WorkQueue (Task 2, as a subsection of Thread Pools)

3. **Expand "Synchronization Primitives"** with:
   - Channels (Task 1, replacing stub)
   - Semaphore (Task 5)
   - Once (Task 5)

4. **Update "Common Patterns"**:
   - Replace "Producer-Consumer (via Channels)" stub with a real example (Task 1).
   - Add a "Structured Concurrency with TaskGroup" pattern (Task 3).
   - Add a "Fan-out with Thread Pool + Futures" pattern (Tasks 2 + 6).

5. **Update "See Also"**:
   - Verify that `async-await-guide.md`, `stm-tutorial.md`, `effects-system-guide.md`
     still exist and point to them correctly.
   - Add a cross-reference to `stm-guide.md` if it covers different material
     than `stm-tutorial.md`.
