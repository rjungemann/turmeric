# Threading and Concurrency Primitives Guide

Safe concurrent programming with OS threads, atomic types, and synchronization primitives.

## Overview

Turmeric provides **1:1 OS threads** via C11 `<threads.h>` plus thread-safe abstractions (`Arc<T>`, `Mutex<T>`, `Atomic<T>`) for safe concurrent programming. Integration with Turmeric's ownership model (`ref<T>`, borrow checking) ensures memory safety.

## Thread Model

### Creating Threads

```turmeric
;; Spawn a new thread
(def result
  (thread
    (fn []
      (println "Hello from thread!")
      42)))

;; Block until thread completes and get result
(println (thread-join result))  ; prints 42
```

### Properties

- **1:1 model:** Each Turmeric thread maps to one OS thread.
- **OS scheduling:** OS scheduler handles preemption and load balancing.
- **Overhead:** ~10-100μs per thread creation; suitable for 100s-1000s of threads.
- **Full FFI access:** Threads are real OS threads; FFI calls work naturally.

### Thread-Local Storage

Each thread has its own stack and thread-local variables:

```turmeric
;; Declare a thread-local
(thread-local my-tls 42)

;; Get current value (only accessible within this thread)
(thread-local-get my-tls)  ; => 42

;; Set current value
(thread-local-set my-tls 100)
```

## Shared Ownership: Arc<T>

**`Arc<T>`** (atomic reference counting) enables shared ownership across threads:

```turmeric
;; Create a shared value
(def shared (arc (make-counter 0)))

;; Clone the arc (increments reference count)
(thread
  (fn []
    (let [my-copy (arc-clone shared)]
      (modify-counter my-copy))))

;; Original and clones all point to same value
(println (counter-value shared))
```

### Properties

- **Atomic:** Reference count increments/decrements are atomic (thread-safe).
- **Move semantics:** `arc-clone` increments the count; `arc` drop decrements atomically.
- **Shared but not mutable:** `Arc<T>` gives shared read-only access. For mutable shared state, wrap in `Mutex<T>`.

### When to Use Arc

- Sharing read-only data across threads.
- Ownership is genuinely shared (multiple threads live simultaneously).
- Passing objects to spawned threads.

## Mutual Exclusion: Mutex<T>

**`Mutex<T>`** (mutual exclusion) protects mutable shared state:

```turmeric
;; Create a mutex
(def counter (mutex 0))

;; Acquire lock, modify, and release (automatically via defer)
(with-lock counter
  (fn [value]
    (let [new-val (+ value 1)]
      new-val)))

;; Check current value (requires lock)
(let [val (with-lock counter (fn [v] v))]
  (println val))
```

### Properties

- **Scoped locking:** Locks release automatically on scope exit (via `defer`).
- **Prevents races:** Only one thread can hold the lock at a time.
- **Composable:** Multiple mutexes can be held (but beware deadlocks with out-of-order release).
- **Blocking:** If another thread holds the lock, `with-lock` blocks until available.

### Read-Write Locks: RwLock<T>

For read-heavy workloads, use `RwLock<T>`:

```turmeric
;; Multiple readers or one writer
(def data (rw-lock (vec 1 2 3)))

;; Read lock (multiple threads can hold simultaneously)
(read-lock data
  (fn [vec] (println vec)))

;; Write lock (exclusive)
(write-lock data
  (fn [vec] (set-vec vec 0 42)))
```

## Atomic Types: Atomic<T>

For simple types, **`Atomic<T>`** provides lock-free atomic operations:

```turmeric
;; Atomic integer
(def counter (atomic 0))

;; Atomic load
(println (atomic-load counter))  ; => 0

;; Atomic store
(atomic-store counter 42)

;; Atomic compare-and-swap
(atomic-cas counter 42 100)  ; success if value was 42, set to 100

;; Atomic add/sub
(atomic-add counter 5)  ; atomically add 5
```

### Types Supported

- `int`, `bool`, `usize`, pointer types
- Basic arithmetic operations on integers
- Compare-and-swap, load, store on all types

### When to Use Atomic

- High-frequency operations where lock contention is too expensive.
- Fine-grained counters, flags, pointers.
- Building higher-level synchronization primitives.

## Synchronization Primitives

### Channels: Send/Receive

(Planned for Phase 20+; currently available via FFI.)

### Condition Variables

Block until a condition is signaled:

```turmeric
(def cond (condition-variable))

;; Thread A: wait for signal
(with-lock mutex
  (fn [_]
    (condition-wait cond mutex)
    (println "woken!")))

;; Thread B: signal the condition
(condition-signal cond)
```

## Thread Pools

(Planned for Phase 20; currently available via FFI to libdispatch, thread pool libraries, etc.)

## Safety Guarantees

### Send and Sync Traits

Marker traits control what types can be safely shared:

- **`Send`** — Type can be moved across thread boundaries. If `T : Send`, `Arc<T>` can be cloned and sent to another thread.
- **`Sync`** — Type can be safely shared via `&T` in multiple threads. If `T : Sync`, multiple threads can hold `&T` simultaneously without a `Mutex`.

```turmeric
;; These are Send (safe to move to threads)
int, bool, string, (Pair a b) [Send a, Send b]

;; These are Sync (safe to share via &)
int, bool, Mutex<T> [T : Sync]

;; NOT Sync (require Mutex for shared access)
Rc<T> (thread-local ref counting)
ref<T> (single-thread ownership)
```

Most library types implement these traits automatically based on their fields.

### Borrow Checking Across Threads

Turmeric's borrow checker enforces:

```turmeric
;; ERROR: cannot move borrowed reference to thread
(let [x 42]
  (thread
    (fn []
      (println x))))  ; x is borrowed; can't move across boundary

;; OK: clone or use Arc
(let [x (arc 42)]
  (thread
    (fn []
      (println (arc-deref x)))))
```

## Common Patterns

### Producer-Consumer (via Channels)

(See stdlib once available; currently use FFI channels.)

### Thread-Safe Counter

```turmeric
(def counter (mutex 0))

(for-each (range 10)
  (fn [i]
    (thread
      (fn []
        (with-lock counter
          (fn [n]
            (+ n 1)))))))

(println (with-lock counter (fn [n] n)))  ; => 10
```

### Barrier

```turmeric
(def barrier (barrier-new 3))

(for-each (range 3)
  (fn [i]
    (thread
      (fn []
        (println (str "Thread " i " starting"))
        (barrier-wait barrier)
        (println (str "Thread " i " done"))))))
```

## See Also

- [Async/Await Guide](async-await-guide.md) — Lightweight fibers for I/O concurrency
- [STM Tutorial](stm-tutorial.md) — Composable transactions (alternative to locks)
- [Effects System Guide](effects-system-guide.md) — Dependency injection and exception handling
- [turmeric-plan.md](../turmeric-plan.md) §19 — Thread primitives architecture
