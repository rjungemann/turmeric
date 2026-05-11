# Turmeric — Thread Safety and Thread Primitives Plan

> **Status:** Draft Plan
> **Last Updated:** 2026-05-10
> **Owner:** Runtime team
> **Phase:** v2 (post-Phase 18)

**Cross-references:**
- [effects-plan.md](effects-plan.md) — Algebraic effects design (v3 stretch goal)
- [turmeric-plan.md](turmeric-plan.md) — Main compiler roadmap
- Phase 18 — Delimited continuations infrastructure
- Phase 5 — `ref<T>` ownership model
- Phase 4 — `defer` + scope unwind

---

## Executive Summary

This document outlines the design and implementation plan for adding **thread safety** and **thread primitives** to Turmeric. The goal is to enable concurrent programming while maintaining memory safety and integrating cleanly with Turmeric's existing ownership model (`ref<T>`, `rc<T>`, borrow checking) and effect system foundation.

**Key decisions:**

| Decision | Rationale |
|---|---|
| **C11 `<threads.h>`** as the portable substrate | C99 has no threads; C11 is widely supported (GCC 4.9+, Clang 3.3+, MSVC 2012+). |
| **No built-in scheduler** — expose primitives only | Users bring their own thread pools (e.g., via FFI to libdispatch, libuv, or custom C). |
| **`Send`/`Sync` marker traits** for thread-safety in types | Static checking prevents data races at compile time where possible. |
| **Thread-local `ref<T>`** — ownership is per-thread by default | Prevents accidental sharing; explicit `Arc<T>` for shared ownership. |
| **`Arc<T>`** for atomic reference counting | Shared ownership across threads with thread-safe RC. |
| **`Mutex<T>`**, `RwLock<T>`** — scoped locking with `defer` integration | Locks release automatically on scope exit. |
| **`Atomic<T>`** — wrap C11 `_Atomic` for basic types | Direct mapping to C11 atomics for `int`, `bool`, pointer types. |
| **`Thread`/`JoinHandle`** — 1:1 OS threads | Simple model; users can build thread pools on top. |
| **No `async`/`await`** in v1 — use threads + channels | `async`/`await` deferred to v2, built on top of threads + effects. |

**Sequencing:** This plan is **Phase 19**, scheduled after Phase 18 (delimited continuations). The CPS substrate from Phase 18 is a prerequisite for v2 effects, but v1 threads do not require CPS — threads are independent of the effect system.

---

## 1. Thread Model

### 1.1 OS Threads (1:1)

Turmeric uses **1:1 OS threads** via C11 `<threads.h>`. Each Turmeric thread maps to one OS thread.

```clojure
;; Spawn a new thread
(def thread-result
  (thread
    (fn []
      (println "Hello from thread!")
      42)))

;; Block until completion and get result
(println (thread-join thread-result))  ; prints 42
```

**Pros:**
- Simple mental model
- Full access to OS thread APIs via FFI
- No runtime scheduler overhead
- Predictable performance

**Cons:**
- Thread creation overhead (~10-100μs)
- Limited scalability (100s-1000s of threads max)
- Users must manage thread pools for high concurrency

**Mitigation:** Provide a **thread pool** in stdlib (Phase 20) built on top of these primitives.

### 1.2 Thread-Local Storage

Each thread has its own stack and thread-local storage. Turmeric exposes:

```clojure
;; Thread-local variable
(thread-local my-tls 42)

;; Get current value
(thread-local-get my-tls)  ; => 42

;; Set for current thread
(thread-local-set! my-tls 100)
```

**Lowering:** Maps directly to C11 `thread_local` storage or `tss_t` key-based TLS.

### 1.3 Thread Identity

```clojure
;; Get current thread ID
(def my-id (thread-id))

;; Compare thread IDs
(thread-id=? (thread-id) other-id)  ; => bool
```

**Lowering:** `thrd_t` via `thrd_current()` and `thrd_equal()`.

---

## 2. Ownership and Thread Safety Model

### 2.1 Core Principle: `Send` and `Sync`

Turmeric adopts a **Rust-inspired** model for thread safety:

- **`Send`** — A type `T` is `Send` if it can be safely transferred to another thread (ownership moves).
- **`Sync`** — A type `T` is `Sync` if it can be safely shared across threads (immutable or properly synchronized).

**Default rules:**

| Type | Send | Sync | Rationale |
|------|------|------|-----------|
| `int`, `bool`, `float`, etc. | ✅ | ✅ | Plain old data, copy-by-value |
| `ptr<T>` | ❌ | ❌ | Raw pointers are unsafe |
| `ref<T>` | ❌ | ❌ | Move-only, not thread-safe by default |
| `rc<T>` | ❌ | ❌ | Non-atomic reference counting |
| `Arc<T>` | ✅ | ✅ | Atomic reference counting (new) |
| `Mutex<T>` | ✅ | ✅ | Synchronized access |
| `Atomic<T>` | ✅ | ✅ | Atomic operations |
| `cont<T>` | ❌ | ❌ | Continuations are not `Send` (capture stack) |
| `fn` closures | ⚠️ | ⚠️ | Depends on captured environment |

### 2.2 Thread-Safe Reference Counting: `Arc<T>`

`ref<T>` is **not thread-safe** — it uses non-atomic refcounting and assumes single-threaded ownership. For shared ownership across threads, we introduce `Arc<T>` (Atomic Reference Count):

```clojure
;; Create an Arc
(def shared-rc (Arc::new 42))

;; Clone the Arc (atomic refcount increment)
(def shared-rc2 (Arc::clone shared-rc))

;; Get the value (immutable borrow)
(def value (Arc::get shared-rc))  ; => 42

;; Drop the Arc (atomic refcount decrement, may free)
(Arc::drop shared-rc)
```

**Properties:**
- `Arc<T>` is `Send + Sync` if `T` is `Send + Sync`.
- `Arc::get` returns an immutable reference to `T`.
- `Arc<T>` where `T` is mutable requires interior mutability (e.g., `Mutex<T>`).

**Lowering:**
```c
// C11 atomic refcounting
typedef struct {
    _Atomic(int) refcount;
    T value;
} Arc_T;

void Arc_drop(Arc_T *arc) {
    if (atomic_fetch_sub(&arc->refcount, 1) == 1) {
        // Last reference — drop the value
        drop_T(&arc->value);
        free(arc);
    }
}
```

### 2.3 `ref<T>` and Threads

`ref<T>` is **not `Send`** and **not `Sync`** by default. Attempting to send a `ref<T>` across threads is a **compile-time error**:

```clojure
;; ERROR: ref<int> is not Send
(thread (fn [] (ref 42)))
```

**Workarounds:**

1. **Move into thread (explicit transfer):**
   ```clojure
   ;; OK: ref is moved into the thread, not shared
   (let [r (ref 42)]
     (thread (fn []
       ;; r is consumed (moved) into this closure
       (println (deref r)))))
   ```

2. **Use `Arc<Mutex<T>>` for shared mutable state:**
   ```clojure
   (def shared (Arc::new (Mutex::new 0)))
   
   (thread (fn []
     (Mutex::lock shared)
     (Mutex::set! shared (+ (Mutex::get shared) 1))
     (Mutex::unlock shared)))
   ```

3. **Use `Arc<Atomic<T>>` for lock-free shared state (where applicable):**
   ```clojure
   (def counter (Arc::new (Atomic::new 0)))
   
   (thread (fn []
     (Atomic::fetch-add counter 1)))
   ```

### 2.4 Closures and `Send`

Closures are `Send` **if and only if** all captured values are `Send`:

```clojure
;; OK: captures only int (Send)
(def f (fn [x] (fn [] (+ x 1))))
(thread (f 42))  ; OK

;; ERROR: captures ref<int> (not Send)
(def g (fn [r] (fn [] (deref r))))
(thread (g (ref 42)))  ; ERROR: closure is not Send

;; OK: captures Arc<Mutex<int>> (Send)
(def h (fn [shared] (fn [] (Mutex::get shared))))
(thread (h (Arc::new (Mutex::new 0))))  ; OK
```

**Borrow checker integration:** The borrow checker tracks which captured values are `Send` and rejects closures that capture non-`Send` types when used across threads.

### 2.5 Type System Extensions

Add two new **marker traits** (similar to typeclasses):

```clojure
;; Marker trait definitions (built-in)
(trait Send T)
(trait Sync T)

;; Auto-implement for primitive types
(impl Send int)
(impl Send bool)
(impl Send float)
;; ... all primitive types

(impl Sync int)
(impl Sync bool)
(impl Sync float)
;; ... all primitive types

;; Arc<T> is Send/Sync if T is Send/Sync
(impl [T: Send] Send (Arc T))
(impl [T: Sync] Sync (Arc T))

;; Mutex<T> is Send/Sync if T is Send
(impl [T: Send] Send (Mutex T))
(impl [T: Send] Sync (Mutex T))

;; Atomic<T> is Send/Sync if T is Send + Sync
(impl [T: Send + Sync] Send (Atomic T))
(impl [T: Send + Sync] Sync (Atomic T))
```

**Note:** `Sync` implies `Send` (if you can share `T` across threads, you can send it). The elaborator enforces this automatically.

---

## 3. Synchronization Primitives

### 3.1 `Mutex<T>` — Mutual Exclusion

```clojure
;; Create a mutex protecting a value
(def lock (Mutex::new 0))

;; Lock and get value
(Mutex::lock lock)
(def val (Mutex::get lock))
(Mutex::unlock lock)

;; More ergonomic: scoped lock with defer
(Mutex::with-lock lock [v]
  (Mutex::set! lock (+ v 1)))
;; Equivalent to:
;; (Mutex::lock lock)
;; (defer (Mutex::unlock lock))
;; (let [v (Mutex::get lock)] ...)
```

**Lowering:** Maps to C11 `mtx_t`.

**Poisoning:** If a thread panics while holding a lock, the lock is **poisoned**. Subsequent lock attempts fail:

```clojure
(Mutex::lock lock)  ; OK
(panic! "oops")     ; Lock is poisoned

;; In another thread:
(Mutex::lock lock)  ; Returns Err(Poisoned)
```

**Rationale:** Prevents deadlocks from unhandled panics and makes poisoning explicit.

### 3.2 `RwLock<T>` — Reader-Writer Lock

```clojure
(def rw (RwLock::new #{}))

;; Read lock (multiple readers allowed)
(RwLock::read lock)
(def data (RwLock::get lock))
(RwLock::read-unlock lock)

;; Write lock (exclusive)
(RwLock::write lock)
(RwLock::set! lock new-data)
(RwLock::write-unlock lock)

;; Scoped variants
(RwLock::with-read lock [data] ...)
(RwLock::with-write lock [] ...)
```

**Lowering:** Maps to a custom reader-writer lock implementation (C11 doesn't have one built-in). Use a pthread-based implementation on POSIX, SRW lock on Windows.

### 3.3 `Atomic<T>` — Atomic Operations

```clojure
;; Supported types: int, uint, bool, isize, usize, and pointer types
(def atomic-flag (Atomic::new false))

;; Load (Acquire ordering)
(def flag (Atomic::load atomic-flag :acquire))

;; Store (Release ordering)
(Atomic::store atomic-flag true :release)

;; Exchange (AcqRel ordering)
(def old (Atomic::exchange atomic-flag false :acqrel))

;; Compare-and-exchange (AcqRel ordering)
(def result (Atomic::compare-exchange atomic-flag old new :acqrel :acqrel))
;; Returns: {:ok new-value} or {:err current-value}

;; Fetch-and-add (for numeric types)
(def prev (Atomic::fetch-add counter 1 :acqrel))

;; Memory ordering options:
;; :relaxed    - No ordering constraints
;; :acquire    - Acquire ordering (loads after this see writes before)
;; :release    - Release ordering (writes before this visible after)
;; :acqrel     - Both acquire and release
;; :seqcst     - Sequentially consistent (strongest, default if omitted)
```

**Lowering:** Maps directly to C11 `_Atomic` and `atomic_*` functions.

**Supported types:**
- `bool` → `_Atomic(bool)`
- `int`, `uint`, `isize`, `usize` → `_Atomic(int)`, `_Atomic(unsigned)`, etc.
- `ptr<T>` → `_Atomic(T*)`

**Not supported:** Complex types (structs, arrays) — use `Mutex<T>` instead.

### 3.4 `Condvar` — Condition Variables

```clojure
(def lock (Mutex::new false))
(def cond (Condvar::new))

;; Waiter thread
(Condvar::with-wait cond lock []
  (println "Got notification!"))

;; Notifier thread
(Condvar::notify-one cond)   ; Wake one waiter
(Condvar::notify-all cond)   ; Wake all waiters
```

**Lowering:** Maps to C11 `cnd_t`.

**Usage pattern:**
```clojure
(def ready? (Mutex::new false))
(def cond (Condvar::new))

;; Worker thread
(thread
  (fn []
    (Mutex::with-lock ready? [r?]
      (while (not r?)
        (Condvar::wait cond ready?))
      (println "Ready!"))))

;; Main thread
(Mutex::with-lock ready? []
  (Mutex::set! ready? true)
  (Condvar::notify-one cond))
```

### 3.5 `Once` — One-Time Initialization

```clojure
(def init-once (Once::new))
(def init-value (ref nil))

(defn get-initialized []
  (Once::call init-once
    (fn []
      (set! init-value (expensive-computation))
      (deref init-value))))
```

**Lowering:** Maps to C11 `once_flag` and `call_once`.

### 3.6 `Barrier` — Synchronization Barrier

```clojure
;; 4 threads wait for each other
(def barrier (Barrier::new 4))

(thread (fn []
  (println "Thread 1 ready")
  (Barrier::wait barrier)
  (println "All threads ready!")))

(thread (fn []
  (println "Thread 2 ready")
  (Barrier::wait barrier)
  (println "All threads ready!")))
```

**Lowering:** Custom implementation using `Mutex` + `Condvar` + counter.

---

## 4. Channel-Based Communication

Channels provide a **message-passing** alternative to shared memory concurrency.

### 4.1 `Chan<T>` — Synchronous Channel

```clojure
;; Create a channel
(def ch (Chan::new))

;; Spawn sender
(thread
  (fn []
    (Chan::send ch 42)))

;; Receive in main thread
(def value (Chan::recv ch))  ; Blocks until value available
(println value)  ; prints 42
```

**Semantics:** Synchronous — `send` blocks until a receiver is ready, and `recv` blocks until a sender is ready.

### 4.2 `AsyncChan<T>` — Asynchronous Channel

```clojure
;; Create with buffer size
(def ch (AsyncChan::new 10))

;; Non-blocking send (if buffer not full)
(AsyncChan::try-send ch 42)  ; Returns :ok or :full

;; Non-blocking receive
(AsyncChan::try-recv ch)  ; Returns {:ok value} or :empty

;; Blocking variants
(AsyncChan::send ch 42)   ; Blocks if buffer full
(AsyncChan::recv ch)     ; Blocks if buffer empty
```

**Lowering:** Custom implementation using `Mutex` + `Condvar` + queue.

### 4.3 `Select` — Multi-Channel Operations

```clojure
(def ch1 (Chan::new))
(def ch2 (Chan::new))

;; Wait on multiple channels
(select
  (ch1 [v] (println "Got from ch1:" v))
  (ch2 [v] (println "Got from ch2:" v))
  (default (println "Timeout!")))
```

**Lowering:** Custom implementation. Each `select` creates a temporary state machine that registers with all involved channels.

---

## 5. Thread Primitive APIs

### 5.1 `Thread` and `JoinHandle`

```clojure
;; Spawn a thread
(def handle (thread
  (fn []
    ;; thread body
    42)))

;; Check if thread has finished
(Thread::done? handle)  ; => bool

;; Join and get result
(def result (Thread::join handle))  ; => Result<int, exn>

;; Detach a thread (no join possible)
(Thread::detach handle)

;; Get thread ID
(def tid (Thread::id handle))
```

**Lowering:**
- `thread` → `thrd_create()` with a trampoline
- `JoinHandle` → wraps `thrd_t` + result storage + state flag
- `Thread::join` → `thrd_join()` + result extraction
- `Thread::detach` → `thrd_detach()`

### 5.2 Thread Attributes

```clojure
;; Spawn with custom stack size
(def handle (thread
  {:stack-size 1048576}  ; 1MB stack
  (fn [] ...)))

;; Spawn with detached state (no join possible)
(def handle (thread
  {:detached true}
  (fn [] ...)))
```

**Supported attributes:**
- `:stack-size` — Thread stack size in bytes (default: 2MB on most platforms)
- `:detached` — If true, thread cannot be joined
- `:name` — Thread name (for debugging, platform-dependent)

---

## 6. Memory Model

### 6.1 C11 Memory Model

Turmeric inherits the **C11 memory model** via `<threads.h>` and `<stdatomic.h>`. Key concepts:

- **Sequentially Consistent (SC):** Strongest ordering; all threads see the same order of SC operations.
- **Acquire-Release:** Weaker than SC but sufficient for most synchronization.
- **Relaxed:** No ordering constraints; only atomicity guaranteed.

### 6.2 Happens-Before Relationship

Turmeric guarantees:

1. **Lock Acquire → Lock Release:** All writes before a lock release are visible after the lock is acquired by another thread.
2. **Atomic Store (Release) → Atomic Load (Acquire):** The store happens-before the load.
3. **Thread Spawn → Thread Join:** The parent's operations before spawn happen-before the child's operations, and the child's operations happen-before the parent's operations after join.
4. **Atomic Exchange:** The exchange operation has both acquire and release semantics.

### 6.3 Data Race Freedom

Turmeric's type system and borrow checker aim to prevent **data races** at compile time:

- A **data race** occurs when two threads simultaneously access the same memory location, and at least one access is a write.
- Turmeric prevents data races by:
  1. Requiring `Sync` for shared access
  2. Requiring `Send` for ownership transfer
  3. Enforcing that `ref<T>` is not `Sync` (not shared)
  4. Requiring explicit synchronization for mutable shared state

**Undefined Behavior:** Data races are **undefined behavior** in C11 and Turmeric. The compiler may assume no data races and optimize accordingly.

---

## 7. Integration with Existing Features

### 7.1 Integration with `defer`

Locks integrate with `defer` for automatic release:

```clojure
(Mutex::lock lock)
(defer (Mutex::unlock lock))
;; ... critical section ...
```

**Guarantee:** The lock is released when the scope exits, even if an exception is thrown.

### 7.2 Integration with `ref<T>`

`ref<T>` remains **single-threaded only**. For shared mutable state, use `Arc<Mutex<T>>` or `Arc<Atomic<T>>`.

**Future:** Consider a `thread-local-ref<T>` for thread-local mutable state with `ref<T>` semantics.

### 7.3 Integration with RC and Borrow Checking

- `rc<T>` — **Not thread-safe**. Use `Arc<T>` for shared ownership.
- Borrow checker — Extend to track `Send`/`Sync` traits and reject unsafe cross-thread operations.

### 7.4 Integration with Exceptions (Phase 17)

Thread creation and joining can fail:

```clojure
(def result (Thread::join handle))
;; result is Result<T, exn>
(case result
  (:ok v) (println "Success:" v)
  (:err e) (println "Thread panicked:" e))
```

**Propagation:** If a thread panics, the exception is stored in the `JoinHandle` and returned when joined.

### 7.5 Integration with Delimited Continuations (Phase 18)

**Important:** Continuations (`cont<T>`) are **not `Send`** and **not `Sync`**. Capturing a continuation across a thread boundary is **undefined behavior**.

```clojure
;; UB: Capturing a continuation in one thread and resuming in another
(reset
  (shift k
    (thread (fn [] (resume k 42)))))  ; UNDEFINED BEHAVIOR
```

**Rationale:** Continuations capture the C stack, which is thread-local. Resuming a continuation on a different thread would corrupt the stack.

**Future work (v2):** Consider **fibers** (user-space threads) that share a C stack and can safely use continuations. This would require a custom scheduler.

---

## 8. Implementation Plan

### 8.1 Phase 19: Thread Primitives (v1)

**Goal:** Add basic thread safety and thread primitives to Turmeric.

**Dependencies:**
- Phase 5 — `ref<T>` ownership model
- Phase 4 — `defer` + scope unwind
- Phase 17 — Exceptions (for panic propagation)
- Phase 18 — Delimited continuations (for future fibers, not required for v1)

**Deliverables:**

| ID | Task | Priority | Effort | Dependencies |
|----|------|----------|--------|--------------|
| TH-001 | Add `Send` and `Sync` marker traits to type system | P0 | 2 days | Phase 2 (type system) |
| TH-002 | Implement `Arc<T>` — atomic reference counting | P0 | 2 days | Phase 5 (RC) |
| TH-003 | Implement `Atomic<T>` for primitive types | P0 | 1 day | None |
| TH-004 | Implement `Mutex<T>` with poison detection | P0 | 2 days | Phase 4 (defer) |
| TH-005 | Implement `RwLock<T>` | P0 | 2 days | TH-004 |
| TH-006 | Implement `Thread`/`JoinHandle` | P0 | 2 days | Phase 17 (exceptions) |
| TH-007 | Implement `Condvar` | P0 | 1 day | TH-004 |
| TH-008 | Implement `Once` | P1 | 0.5 day | TH-004 |
| TH-009 | Implement `Barrier` | P1 | 0.5 day | TH-004, TH-007 |
| TH-010 | Implement `Chan<T>` (synchronous) | P1 | 2 days | TH-004, TH-007 |
| TH-011 | Implement `AsyncChan<T>` | P1 | 2 days | TH-010 |
| TH-012 | Implement `Select` for channels | P1 | 2 days | TH-010 |
| TH-013 | Thread-local storage (`thread-local`, `thread-local-get`, `thread-local-set!`) | P1 | 1 day | None |
| TH-014 | Thread ID (`thread-id`, `thread-id?`) | P1 | 0.5 day | None |
| TH-015 | Borrow checker integration for `Send`/`Sync` | P0 | 3 days | TH-001, Phase 5 |
| TH-016 | `thread-local-ref<T>` for thread-local mutable state | P2 | 1 day | Phase 5 |
| TH-017 | `thread-pool` stdlib module (basic thread pool) | P2 | 3 days | TH-006 |
| TH-018 | Fixtures: `mutex-basic.tur`, `arc-basic.tur`, `thread-basic.tur`, etc. | P0 | 2 days | All above |

**Total effort:** ~26-28 days

### 8.2 Phase 20: Thread Pool and Higher-Level Abstractions (v1.1)

**Goal:** Add a thread pool and higher-level concurrency abstractions.

**Deliverables:**

| ID | Task | Priority | Effort | Dependencies |
|----|------|----------|--------|--------------|
| TP-001 | `ThreadPool` with fixed size | P0 | 3 days | Phase 19 |
| TP-002 | `ThreadPool` with dynamic scaling | P1 | 2 days | TP-001 |
| TP-003 | `Future<T>` — async computation | P0 | 2 days | TP-001 |
| TP-004 | `Promise<T>` — async value | P0 | 2 days | TP-003 |
| TP-005 | `WorkQueue<T>` — task queue | P1 | 1 day | TP-001 |
| TP-006 | `Semaphore` — counting semaphore | P1 | 1 day | Phase 19 |
| TP-007 | `ThreadPool` integration with `Future`/`Promise` | P0 | 2 days | TP-001, TP-003 |
| TP-008 | Fixtures for thread pool and futures | P0 | 2 days | All above |

**Total effort:** ~15 days

### 8.3 Phase 21: Fibers and Effects Integration (v2)

**Goal:** Add **fibers** (user-space threads) that integrate with delimited continuations and effects.

**Prerequisites:**
- Phase 18 — Delimited continuations
- Phase 19 — Thread primitives
- [effects-plan.md](effects-plan.md) — Algebraic effects (v3 stretch goal)

**Deliverables:**

| ID | Task | Priority | Effort | Dependencies |
|----|------|----------|--------|--------------|
| FB-001 | Fiber type and basic API | P0 | 3 days | Phase 18 |
| FB-002 | Fiber scheduler (cooperative multitasking) | P0 | 3 days | FB-001 |
| FB-003 | Fiber-local storage | P1 | 1 day | FB-001 |
| FB-004 | Fiber + `reset`/`shift` integration | P0 | 2 days | FB-001, Phase 18 |
| FB-005 | `async`/`await` syntax sugar (desugars to fibers + continuations) | P1 | 3 days | FB-001 |
| FB-006 | Thread pool with fiber support | P1 | 2 days | FB-001, TP-001 |
| FB-007 | Fixtures for fibers and async/await | P0 | 2 days | All above |

**Total effort:** ~16 days

**Note:** This phase is **deferred** until after effects are implemented. Fibers require the CPS substrate from Phase 18, which is already in place, but the full integration with effects is a v2 feature.

---

## 9. C API and FFI

### 9.1 C11 Header Inclusion

Turmeric-generated C code includes `<threads.h>` and `<stdatomic.h>` when thread primitives are used.

```c
// Generated C code includes:
#include <threads.h>
#include <stdatomic.h>
```

### 9.2 FFI for Custom Threading

Users can access OS-specific threading APIs via FFI:

```clojure
(extern-c pthread_create [^ptr<void> thread ^ptr<thread-attr> attr ^ptr<void> start ^ptr<void> arg] : int)
(extern-c pthread_join [^ptr<void> thread ^ptr<ptr<void>> ret] : int)
```

**Caution:** Mixing Turmeric's `Thread` with raw pthreads may lead to undefined behavior. Stick to one threading model.

### 9.3 Compilation Flags

Thread support requires linking with `-pthread` on POSIX systems:

```sh
./build/tur build my-program.tur -l pthread
```

Turmeric automatically adds `-pthread` to the linker flags when thread primitives are used.

---

## 10. Testing Strategy

### 10.1 Unit Tests

Each primitive has a dedicated fixture file:

- `mutex-basic.tur` — Basic mutex operations
- `mutex-poison.tur` — Poison detection
- `rwlock-basic.tur` — Reader-writer lock
- `atomic-basic.tur` — Atomic operations
- `arc-basic.tur` — Atomic reference counting
- `thread-basic.tur` — Thread spawn and join
- `thread-arc.tur` — `Arc<T>` with threads
- `channel-basic.tur` — Synchronous channels
- `async-channel.tur` — Asynchronous channels
- `select-basic.tur` — Multi-channel select
- `barrier.tur` — Barrier synchronization
- `once.tur` — One-time initialization

### 10.2 Integration Tests

- `threaded-fizzbuzz.tur` — Multi-threaded FizzBuzz
- `producer-consumer.tur` — Producer-consumer with channels
- `dining-philosophers.tur` — Classic concurrency problem
- `raytracer.tur` — Parallel raytracer using thread pool

### 10.3 Stress Tests

- `thread-stress.tur` — Spawn 1000 threads, join all
- `mutex-stress.tur` — 10 threads contention on a mutex
- `atomic-stress.tur` — Atomic increment from 100 threads
- `arc-stress.tur` — Arc clone/drop from multiple threads

### 10.4 Sanitizer Testing

All tests run with:
- **ThreadSanitizer (TSan)** — Detects data races
- **AddressSanitizer (ASan)** — Detects memory errors
- **UndefinedBehaviorSanitizer (UBSan)** — Detects UB

```sh
make test TSAN=1   # Run tests with ThreadSanitizer
```

---

## 11. Performance Considerations

### 11.1 Overhead

| Primitive | Overhead | Notes |
|-----------|----------|-------|
| `Arc<T>` | ~10-20ns per clone/drop | Atomic increment/decrement |
| `Mutex<T>` | ~20-50ns lock/unlock (uncontended) | C11 `mtx_t` |
| `Atomic<T>` | ~5-10ns per operation | C11 atomics |
| `Thread` | ~10-100μs spawn | OS thread creation |
| `Chan<T>` | ~50-100ns send/recv (uncontended) | Mutex + Condvar |

### 11.2 Optimizations

1. **`Arc<T>` optimization:** For types that are `Copy` (bit-copyable), `Arc::clone` can use a fast path that checks the refcount before incrementing.
2. **`Mutex<T>` optimization:** Uncontended lock fast path using `mtx_try_lock`.
3. **`Atomic<T>` optimization:** Use `memory_order_relaxed` where possible.
4. **Thread pool:** Amortize thread creation overhead across many tasks.

---

## 12. Portability

### 12.1 Platform Support

| Platform | C11 Threads | Notes |
|----------|-------------|-------|
| Linux (glibc ≥ 2.16) | ✅ | Full support |
| macOS (Xcode ≥ 5) | ✅ | Full support |
| Windows (MSVC ≥ 2012, MinGW) | ✅ | Full support |
| FreeBSD | ✅ | Full support |
| OpenBSD | ⚠️ | C11 threads available but less tested |
| WebAssembly | ❌ | No thread support (future: WASM threads) |

### 12.2 Fallback for Non-C11 Platforms

For platforms without C11 threads (e.g., older compilers), Turmeric provides a **fallback mode**:

- **POSIX:** Use `pthread` directly
- **Windows:** Use Win32 threads directly
- **No threads:** Compile-time error if thread primitives are used

**Detection:** `configure` script checks for C11 thread support and sets appropriate flags.

```sh
./configure --disable-threads  # Disable thread support entirely
```

---

## 13. Documentation

### 13.1 User Guide

- **Thread Safety in Turmeric** — Overview of the ownership and thread safety model
- **Thread Primitives** — API reference for all thread-related types and functions
- **Concurrency Patterns** — Common patterns and best practices
- **Data Race Prevention** — How Turmeric prevents data races at compile time

### 13.2 API Reference

Generated from docstrings in the stdlib. Each primitive has:
- Type signature
- Description
- Examples
- Panics (if any)
- Safety (thread safety guarantees)

---

## 14. Future Work

### 14.1 v2 Features

| Feature | Description | Priority |
|---------|-------------|----------|
| Fibers | User-space threads with `reset`/`shift` support | High |
| `async`/`await` | Syntactic sugar for async programming | High |
| Work stealing | Thread pool with work-stealing scheduler | Medium |
| `Atomic` for more types | Support for structs, arrays | Medium |
| `Weak` for `Arc` | `Arc::downgrade` and `Weak::upgrade` | Medium |
| `Park`/`Unpark` | Thread parking (for async I/O) | Low |

### 14.2 v3 Features (Effects Integration)

| Feature | Description | Priority |
|---------|-------------|----------|
| Effect handlers + threads | Handle effects in multi-threaded context | High |
| `async`/`await` with effects | Full async/await with effect support | High |
| Fibers + effects | Fibers with algebraic effects | High |
| STM (Software Transactional Memory) | Transactional memory for shared state | Low |

---

## 15. Risks and Mitigations

| Risk | Likelihood | Impact | Mitigation |
|------|------------|--------|------------|
| Data races in user code | High | High | Static checking (`Send`/`Sync`), TSan testing |
| Deadlocks | Medium | High | Documentation, linter warnings for nested locks |
| Performance overhead | Medium | Medium | Benchmarking, optimization passes |
| Platform-specific bugs | Low | Medium | CI testing on all supported platforms |
| Interaction with effects | Medium | High | Defer fibers/effects integration to v2 |
| Memory leaks with `Arc` | Medium | Medium | Borrow checker integration, `Weak` pointers |

---

## 16. Appendix: C11 Threads API Reference

### 16.1 Thread Management

| C11 Function | Turmeric Equivalent | Description |
|--------------|---------------------|-------------|
| `thrd_create` | `Thread::spawn` | Create a new thread |
| `thrd_join` | `Thread::join` | Wait for thread to finish |
| `thrd_detach` | `Thread::detach` | Detach a thread |
| `thrd_current` | `Thread::id` | Get current thread ID |
| `thrd_equal` | `thread-id?` | Compare thread IDs |
| `thrd_sleep` | `Thread::sleep` | Sleep for a duration |
| `thrd_yield` | `Thread::yield` | Yield to scheduler |

### 16.2 Mutexes

| C11 Function | Turmeric Equivalent | Description |
|--------------|---------------------|-------------|
| `mtx_init` | `Mutex::new` | Initialize a mutex |
| `mtx_destroy` | `Mutex::drop` | Destroy a mutex |
| `mtx_lock` | `Mutex::lock` | Lock a mutex |
| `mtx_trylock` | `Mutex::try-lock` | Try to lock a mutex |
| `mtx_unlock` | `Mutex::unlock` | Unlock a mutex |
| `mtx_timedlock` | `Mutex::timed-lock` | Lock with timeout |

### 16.3 Condition Variables

| C11 Function | Turmeric Equivalent | Description |
|--------------|---------------------|-------------|
| `cnd_init` | `Condvar::new` | Initialize a condition variable |
| `cnd_destroy` | `Condvar::drop` | Destroy a condition variable |
| `cnd_signal` | `Condvar::notify-one` | Wake one waiter |
| `cnd_broadcast` | `Condvar::notify-all` | Wake all waiters |
| `cnd_wait` | `Condvar::wait` | Wait for notification |
| `cnd_timedwait` | `Condvar::timed-wait` | Wait with timeout |

### 16.4 Atomic Operations

| C11 Function | Turmeric Equivalent | Description |
|--------------|---------------------|-------------|
| `atomic_init` | `Atomic::new` | Initialize an atomic |
| `atomic_store` | `Atomic::store` | Store a value atomically |
| `atomic_load` | `Atomic::load` | Load a value atomically |
| `atomic_exchange` | `Atomic::exchange` | Exchange values atomically |
| `atomic_compare_exchange_strong` | `Atomic::compare-exchange` | CAS operation |
| `atomic_fetch_add` | `Atomic::fetch-add` | Fetch and add |
| `atomic_fetch_sub` | `Atomic::fetch-sub` | Fetch and subtract |
| `atomic_flag_test_and_set` | `Atomic::test-and-set` | Test and set for bool |
| `atomic_flag_clear` | `Atomic::clear` | Clear a flag |

### 16.5 Memory Ordering

| C11 Enum | Turmeric Keyword | Description |
|----------|------------------|-------------|
| `memory_order_relaxed` | `:relaxed` | No ordering constraints |
| `memory_order_acquire` | `:acquire` | Acquire ordering |
| `memory_order_release` | `:release` | Release ordering |
| `memory_order_acq_rel` | `:acqrel` | Acquire + release |
| `memory_order_seq_cst` | `:seqcst` | Sequentially consistent |

---

## 17. Appendix: Example Programs

### 17.1 Parallel Map

```clojure
(defn pmap [f xs]
  (let [results (ref (vec-repeat (length xs) nil))
        mutex (Mutex::new 0)
        index (Atomic::new 0)]
    (doseq [x xs]
      (thread
        (fn []
          (let [i (Atomic::fetch-add index 1 :acqrel)]
            (Mutex::lock mutex)
            (vec-set! results i (f x))
            (Mutex::unlock mutex)))))
    (Thread::join-all (get-threads))
    results))

(defn main []
  (println (pmap (fn [x] (* x x)) [1 2 3 4 5])))
```

### 17.2 Producer-Consumer with Channels

```clojure
(defn producer [ch n]
  (dotimes [i n]
    (Chan::send ch i))
  (Chan::close ch))

(defn consumer [ch]
  (loop []
    (case (Chan::recv ch)
      (:ok v) (do (println "Got:" v) (recur))
      (:err _) (println "Channel closed")))))

(defn main []
  (let [ch (Chan::new)]
    (thread (producer ch 10))
    (consumer ch)))
```

### 17.3 Thread-Safe Counter with `Arc<Atomic<int>>`

```clojure
(defn main []
  (let [counter (Arc::new (Atomic::new 0))]
    (dotimes [_ 10]
      (thread
        (fn []
          (dotimes [_ 1000]
            (Atomic::fetch-add (Arc::get counter) 1 :acqrel)))))
    (Thread::sleep 1000)  ; Wait for threads to finish
    (println "Counter:" (Atomic::load (Arc::get counter) :acquire))))
```

### 17.4 Dining Philosophers

```clojure
(defn philosopher [id left-fork right-fork]
  (dotimes [_ 5]  ; Eat 5 times
    (Mutex::lock left-fork)
    (Mutex::lock right-fork)
    (println id "is eating")
    (Thread::sleep 100)
    (Mutex::unlock right-fork)
    (Mutex::unlock left-fork)
    (println id "is thinking")
    (Thread::sleep 100)))

(defn main []
  (let [forks (vec-map (range 5) (fn [_] (Mutex::new ())))]
    (dotimes [i 5]
      (thread (philosophers i (get forks i) (get forks (mod (+ i 1) 5)))))))
```
