# Turmeric — Haskell-Style STM Plan

> **Status:** Draft Plan
> **Last Updated:** 2026-05-10
> **Owner:** Runtime team
> **Phase:** v2 (post-Phase 19)

**Cross-references:**
- [thread-safety-and-primitives-plan.md](thread-safety-and-primitives-plan.md) — Thread primitives, `Arc<T>`, `Mutex<T>`, `Atomic<T>`
- [turmeric-plan.md](turmeric-plan.md) — Main compiler roadmap
- [effects-plan.md](effects-plan.md) — Algebraic effects design
- Phase 19 — Delimited continuations infrastructure
- Phase 18 — Algebraic effects (v3 stretch goal)

---

## Executive Summary

This document outlines the design and implementation plan for adding **Haskell-style Software Transactional Memory (STM)** to Turmeric. STM provides a composable, declarative approach to concurrent programming that avoids deadlocks, priority inversion, and many common concurrency bugs.

**Key decisions:**

| Decision | Rationale |
|---|---|
| **Haskell-style API** | Proven design, composable transactions, `TVar` abstraction |
| **Lock-based implementation** (v1) | Practical, portable, builds on existing `Mutex<T>` |
| **Transactional retry** via `retry` | Automatic blocking and wakeup on `TVar` changes |
| **`STM` monad** for transaction composition | Type-safe, composable transaction blocks |
| **`TVar` for transactional variables** | Atomic, composable mutable references |
| **`TMVar`/`TChan` for transactional synchronization** | Common concurrency patterns |
| **No nested transactions** in v1 | Simplifies implementation, matches Haskell semantics |
| **Orphan detection** for dead transaction cleanup | Prevents memory leaks from abandoned transactions |

**Sequencing:** STM depends on Phase 19 (algebraic effects) for the monadic interface. The transaction runtime builds on Phase 1 (thread primitives: `Mutex<T>`, `Atomic<T>`, `Arc<T>`, condition variables). Target: **Phase 20**.

---

## 1. Background: What is STM?

Software Transactional Memory is a concurrency paradigm that treats shared memory operations as **atomic transactions**, similar to database transactions:

- **Atomicity:** A transaction either completes entirely or not at all
- **Isolation:** Concurrent transactions appear to execute serially
- **Composability:** Transactions can be freely composed (unlike lock-based approaches)

### 1.1 Why STM?

| Problem | STM Solution |
|---------|--------------|
| Deadlocks | No locks to order; conflicts resolved automatically |
| Priority inversion | No priority inheritance needed; transactions are atomic |
| Lock granularity | Fine-grained without performance penalty |
| Error handling | Transactions can abort cleanly |
| Composability | Transactions can call other transactions freely |

### 1.2 Haskell's STM Model

Haskell's STM (as in `Control.Concurrent.STM`) provides:

```haskell
-- Transactional variables
data TVar a

-- STM monad for atomic transactions  
newTVar :: a -> STM (TVar a)
readTVar :: TVar a -> STM a
writeTVar :: TVar a -> a -> STM ()

-- Run a transaction atomically
atomically :: STM a -> IO a

-- Retry: block until a TVar changes
retry :: STM a

-- Or: try one branch, then another
orElse :: STM a -> STM a -> STM a

-- Synchronization primitives
newTMVar :: a -> STM (TMVar a)
newTChan :: STM (TChan a)
```

**Key insight:** `STM` is a **monad** — transactions can be composed using `>>=` (bind), and `atomically` executes the entire transaction atomically.

---

## 2. Design Overview

### 2.1 Core Abstractions

#### 2.1.1 `TVar<T>` — Transactional Variable

A `TVar` is a mutable reference that can only be read/written within an `STM` transaction. All operations on `TVar`s within a transaction are atomic.

```clojure
;; Create a transactional variable
(def counter (TVar::new 0))

;; In a transaction:
(stm
  (let [old (TVar::read counter)
        new (+ old 1)]
    (TVar::write counter new)
    new))
```

**Properties:**
- `TVar<T>` is `Sync` (can be shared across threads)
- `TVar<T>` is **not** `Send` (ownership is global, not movable)
- Reads/writes are only valid within `stm` blocks

#### 2.1.2 `STM` Monad — Transaction Block

The `stm` macro/form delimits a transaction. All `TVar` operations within it are recorded and executed atomically.

```clojure
;; Transfer funds between two accounts
(defn transfer [from to amount]
  (stm
    (let [balance-from (TVar::read from)]
      (when (<= amount balance-from)
        (TVar::write from (- balance-from amount))
        (TVar::write to (+ (TVar::read to) amount))
        true))))
```

**Semantics:**
- If any `TVar` read during the transaction has been modified by another committed transaction, this transaction **retry**
- If all `TVar` reads are still valid, all writes are applied atomically
- If an exception occurs, the transaction aborts (no writes applied)

#### 2.1.3 `atomically` — Execute Transaction

`atomically` runs an `STM` computation and blocks until it commits successfully.

```clojure
;; atomically : (STM a) -> a
(def result (atomically
               (stm
                 (TVar::read counter))))
```

### 2.2 Synchronization Primitives

#### 2.2.1 `TMVar<T>` — Transactional MVars

A `TMVar` is an empty-or-full transactional variable (like Haskell's `MVar`).

```clojure
(def mailbox (atomically (TMVar::new "Hello")))

;; Blocking read
(def msg (atomically (TMVar::take mailbox)))

;; Blocking write
(atomically (TMVar::put mailbox "World"))

;; Non-blocking variants
(atomically (TMVar::try-take mailbox))  ; => (option T)
(atomically (TMVar::try-put mailbox val))  ; => bool
```

#### 2.2.2 `TChan<T>` — Transactional Channels

A `TChan` is a FIFO queue for transactional communication (like Haskell's `TChan`).

```clojure
(def chan (atomically (TChan::new)))

;; Write to channel
(atomically (TChan::write chan "msg"))

;; Read from channel (blocks if empty)
(def msg (atomically (TChan::read chan)))

;; Peek without consuming
(def peek-msg (atomically (TChan::peek chan)))
```

#### 2.2.3 `TSem` — Transactional Semaphores

```clojure
(def sem (atomically (TSem::new 5)))

;; Acquire (decrement, blocks if 0)
(atomically (TSem::wait sem))

;; Release (increment, wakes one waiter)
(atomically (TSem::signal sem))
```

### 2.3 Control Flow

#### 2.3.1 `retry` — Retry Transaction

Blocks the current transaction and retries when any `TVar` changes.

```clojure
(defn wait-until [tv pred]
  (stm
    (when-not (pred (TVar::read tv))
      (retry))
    (TVar::read tv)))
```

#### 2.3.2 `or-else` — Alternative Transaction

Tries the first transaction; if it retries, tries the second.

```clojure
(defn transfer-with-fallback [from to amount fallback]
  (atomically
    (stm
      (or-else
        (stm
          (let [balance (TVar::read from)]
            (when (>= balance amount)
              (TVar::write from (- balance amount))
              (TVar::write to (+ (TVar::read to) amount))
              true))
        (stm
          (fallback)))))))
```

#### 2.3.3 `check` — Assertion

Aborts the transaction if the condition is false (like `assert` but transactional).

```clojure
(stm
  (let [val (TVar::read tv)]
    (check (> val 0))  ; Aborts if val <= 0
    (TVar::write tv (- val 1))))
```

---

## 3. Type System Integration

### 3.1 Type System Extensions

Add a new **effect row** for STM operations, or integrate with the existing effect system:

```clojure
;; Option A: STM as an effect
(defeffect STM-Read [^TVar a]  : a)
(defeffect STM-Write [^TVar a, a] : ())

;; Option B: STM monad (preferred for v1)
;; stm block has type (STM a)
;; atomically : (STM a) -> a
```

**V1 approach:** Use a dedicated `STM` monad type that's separate from the effect system. This keeps v1 simple and self-contained.

### 3.2 `TVar` Type

```clojure
;; TVar is a struct wrapping:
;; - Current value (boxed)
;; - Version number (for validation)
;; - Wait queue (for blocked transactions)
(defstruct TVar [value : ref, version : int64, waiters : WaitQueue])
```

**Marker traits:**
```clojure
(impl [T: Send] Sync (TVar T))
;; TVar is Sync (shared across threads) but not Send (global)
```

### 3.3 Transaction Type

```clojure
;; STM a represents a transaction that returns a
;; Internally: a closure that records reads/writes
(defalias STM (-> (TransactionCtx) a))
```

---

## 4. Implementation Strategy

### 4.1 V1: Lock-Based STM (Phase 20)

**Approach:** Use a **single global lock** for simplicity (GHC's original STM implementation used this).

```c
// Global STM lock
typedef struct {
    mtx_t lock;
    // All TVars in the system
    // Version numbers for validation
    // Blocked transaction queues
} STM_State;
```

**Transaction execution:**
1. Acquire global STM lock
2. Execute transaction closure, recording all `TVar` reads/writes
3. Validate: check that all read `TVar` versions haven't changed
4. If valid: apply writes, increment `TVar` versions, commit
5. If invalid: discard writes, retry (go to step 2)
6. Release lock

**Pros:**
- Simple implementation
- Correct by construction (serial execution)
- No deadlocks

**Cons:**
- Poor scalability (contention on global lock)
- Not suitable for high-concurrency workloads

### 4.2 V2: Fine-Grained Locking (Phase 21)

**Approach:** Per-`TVar` locks with **two-phase locking**:

1. **Grow phase:** Acquire locks on all `TVar`s to be read/written (in a consistent order to prevent deadlocks)
2. **Shrink phase:** Validate and apply writes, release locks

**Optimization:** Use **lock stripping** — group `TVar`s into lock buckets to reduce contention.

### 4.3 V3: Lock-Free STM (Future)

**Approach:** Use **Multi-Word Compare-and-Swap (MW-CAS)** or **Transactional Locking II (TL2)** algorithm.

**Requirements:**
- Platform support for MW-CAS (x86 has `CMPXCHG16B`)
- Or software fallbacks

**Not a priority** for initial implementation.

### 4.4 Transaction Logging

Each transaction maintains a **log** of its operations:

```c
typedef struct {
    // Set of TVars read
    TVar** read_set;
    int read_count;
    
    // Map of TVar -> new value
    TVar** write_set;
    void** new_values;
    int write_count;
    
    // Retry continuation
    STM_Continuation* on_retry;
    
    // Commit continuation
    STM_Continuation* on_commit;
} STM_Transaction;
```

### 4.5 Retry and Wakeup

When a transaction calls `retry`:

1. Transaction is added to a **wait queue** associated with the `TVar`s it depends on
2. Transaction releases the STM lock and blocks
3. When a `TVar` is written, all transactions in its wait queue are **woken up**
4. Woken transactions re-acquire the lock and retry

**Implementation:** Use C11 condition variables (`cnd_t`) for blocking/wakeup.

```c
// Per-TVar wait queue
typedef struct {
    TVar* tvar;
    STM_Transaction** waiters;
    int count;
    cnd_t cond;
} TVar_WaitQueue;
```

### 4.6 Orphan Detection

A transaction that's blocked on `retry` but whose parent thread has died is an **orphan**. We need to detect and clean up orphans to prevent memory leaks.

**Approach:** Use **weak references** to track transaction ownership:

```c
typedef struct {
    STM_Transaction* transaction;
    WeakRef* owner_ref;  // Weak reference to owning thread/closure
} STM_OraphanTracker;
```

When a thread exits, all its orphaned transactions are aborted and cleaned up.

---

## 5. Lowering to C

### 5.1 Data Structures

```c
// TVar implementation
typedef struct TVar {
    TypeInfo* type;
    void* value;          // Current value (boxed)
    uint64_t version;     // Incremented on each write
    STM_WaitQueue waiters; // Transactions waiting on this TVar
    mtx_t lock;           // Per-TVar lock (v2+)
} TVar;

// STM State (v1: global)
typedef struct {
    mtx_t global_lock;
    TVar** all_tvars;
    int tvar_count;
    STM_Transaction** blocked_transactions;
} STM_State;

// Transaction log
typedef struct {
    TVar** read_set;      // TVars read by this transaction
    uint64_t* read_versions; // Versions at read time
    int read_count;
    
    TVar** write_set;     // TVars to write
    void** new_values;    // New values
    int write_count;
    
    bool valid;           // Validation result
    bool committed;       // Whether transaction committed
    cnd_t* cond;          // Condition variable to wait on
} STM_Transaction;
```

### 5.2 Core Functions

```c
// Create a new TVar
TVar* tur_tvar_new(TypeInfo* type, void* initial_value);

// Read a TVar (within transaction)
void* tur_tvar_read(STM_Transaction* tx, TVar* tvar);

// Write a TVar (within transaction)
void tur_tvar_write(STM_Transaction* tx, TVar* tvar, void* value);

// Validate transaction
bool tur_stm_validate(STM_Transaction* tx);

// Commit transaction
bool tur_stm_commit(STM_Transaction* tx);

// Execute transaction atomically (blocks until success)
void* tur_atomically(STM_Transaction* (*transaction_fn)(void*), void* arg);

// Retry current transaction
void tur_stm_retry(STM_Transaction* tx);

// Check condition (aborts if false)
void tur_stm_check(bool condition);
```

### 5.3 Transaction Execution Loop

```c
void* tur_atomically(STM_Transaction* (*fn)(void*), void* arg) {
    STM_State* state = get_global_stm_state();
    
    while (true) {
        // Create transaction
        STM_Transaction tx = {0};
        
        // Acquire lock
        mtx_lock(&state->global_lock);
        
        // Execute transaction closure
        STM_Transaction* result = fn(arg);
        
        // Validate
        if (tur_stm_validate(&tx)) {
            // Commit
            if (tur_stm_commit(&tx)) {
                mtx_unlock(&state->global_lock);
                return result;
            }
        }
        
        // Invalidate and retry
        tx.valid = false;
        
        // If transaction called retry, block
        if (tx.retry) {
            // Add to wait queue
            tur_stm_block(&tx);
            // Release lock and wait
            mtx_unlock(&state->global_lock);
            cnd_wait(tx.cond, &state->global_lock);
            // Re-acquire lock on wakeup
            mtx_lock(&state->global_lock);
            continue;
        }
        
        // Discard and retry immediately
        mtx_unlock(&state->global_lock);
    }
}
```

---

## 6. Surface Syntax

### 6.1 Core Operations

```clojure
;; Create TVar
(def counter (TVar::new 0))

;; Read TVar (only in stm block)
(stm (TVar::read counter))

;; Write TVar (only in stm block)
(stm (TVar::write counter 42))

;; Modify TVar
(stm (TVar::modify counter (fn [x] (+ x 1))))

;; atomically: run transaction, block until commit
(def val (atomically (stm (TVar::read counter))))

;; Short form: atomically can take stm block directly
(def val (atomically (stm (TVar::read counter))))
```

### 6.2 Control Flow

```clojure
;; Retry: block and retry when TVar changes
(stm
  (when-not (ready? (TVar::read flag))
    (retry))
  (do-something))

;; Check: abort if condition false
(stm
  (check (> (TVar::read balance) 0))
  (TVar::write balance (- (TVar::read balance) 1)))

;; or-else: try first, then second
(atomically
  (stm
    (or-else
      (stm (take from-account amount))
      (stm (use-credit amount)))))
```

### 6.3 Synchronization Primitives

```clojure
;; TMVar: empty or full
(def mvar (atomically (TMVar::new "value")))
(atomically (TMVar::put mvar "new"))
(def val (atomically (TMVar::take mvar)))
(def val (atomically (TMVar::read mvar)))  ; Read without taking

;; TChan: FIFO channel
(def chan (atomically (TChan::new)))
(atomically (TChan::write chan "msg"))
(def msg (atomically (TChan::read chan)))

;; TSem: semaphore
(def sem (atomically (TSem::new 5)))
(atomically (TSem::wait sem))
(atomically (TSem::signal sem))
```

### 6.4 Macros for Ergonomics

```clojure
;; with-tvar: create and bind a TVar
(with-tvar [counter 0]
  (atomically (TVar::write counter 1)))

;; dosync: shorthand for atomically + stm
(dosync
  (TVar::write counter (+ (TVar::read counter) 1)))

;; stm-when: conditional in transaction
(stm-when (TVar::read flag)
  (do-something))
```

---

## 7. Integration with Existing Features

### 7.1 Thread Safety

STM builds on and integrates with existing thread primitives:

| Feature | Integration |
|---------|--------------|
| `Arc<T>` | Use `Arc<TVar<T>>` for shared TVars |
| `Mutex<T>` | Underlying implementation for v2+ |
| `Atomic<T>` | Not directly used (TVars have their own versioning) |
| `Thread` | `atomically` blocks the calling thread |

### 7.2 Effect System

In v1, STM is **independent** of the effect system. In v2, we can integrate:

```clojure
;; STM as an effect
(defeffect STM [] : ())

;; perform (STM) inside a handler could run a transaction
;; This allows STM to be mocked/tested via effect handlers
```

### 7.3 Borrow Checker

The borrow checker must understand that:
- `TVar<T>` is a shared reference (like `Arc<T>`)
- Values read from `TVar<T>` are **immutable snapshots** within a transaction
- Writes to `TVar<T>` don't affect the original value until commit

```clojure
;; This is safe: we get an immutable snapshot
(stm
  (let [val (TVar::read tvar)]  ; val is a copy/snapshot
    (do-something val)        ; val doesn't change during transaction
    (TVar::write tvar new-val)))
```

### 7.4 Defer Integration

`defer` inside `stm` blocks should **not** execute until after transaction commit (if successful) or abort (if failed).

```clojure
(stm
  (let [temp (open-resource)]
    (defer (close-resource temp))
    (TVar::write tvar (process temp))))
;; close-resource called after transaction commits
```

**Implementation:** Transaction holds a defer stack; defers are executed at commit/abort time.

---

## 8. Standard Library Additions

### 8.1 Core Module: `stdlib/stm.tur`

```clojure
;; Types
(defstruct TVar [value] :opaque)
(defstruct TMVar [value] :opaque)
(defstruct TChan [] :opaque)
(defstruct TSem [count] :opaque)

;; TVar operations
(defn TVar::new [value] : (TVar a))
(defn TVar::read [tv] : a)          ; Only in stm
(defn TVar::write [tv value] : ()) ; Only in stm
(defn TVar::modify [tv f] : ())    ; f : (a) -> a
(defn TVar::swap [tv new] : a)     ; Write and return old

;; STM monad
(defn stm [& body] : (STM a))      ; Delimits transaction
(defn atomically [tx] : a)          ; Execute transaction
(defn retry [] : a)                 ; Retry transaction
(defn check [cond] : ())            ; Assert condition
(defn or-else [tx1 tx2] : (STM a))  ; Try tx1, then tx2

;; TMVar operations
(defn TMVar::new [value] : (STM (TMVar a)))
(defn TMVar::new-empty [] : (STM (TMVar a)))
(defn TMVar::put [mvar value] : (STM ()))
(defn TMVar::try-put [mvar value] : (STM bool))
(defn TMVar::take [mvar] : (STM a))
(defn TMVar::try-take [mvar] : (STM (option a)))
(defn TMVar::read [mvar] : (STM a))
(defn TMVar::is-empty [mvar] : (STM bool))

;; TChan operations
(defn TChan::new [] : (STM (TChan a)))
(defn TChan::write [chan value] : (STM ()))
(defn TChan::read [chan] : (STM a))
(defn TChan::try-read [chan] : (STM (option a)))
(defn TChan::peek [chan] : (STM a))
(defn TChan::try-peek [chan] : (STM (option a)))

;; TSem operations
(defn TSem::new [count] : (STM (TSem)))
(defn TSem::wait [sem] : (STM ()))
(defn TSem::try-wait [sem] : (STM bool))
(defn TSem::signal [sem] : (STM ()))
```

### 8.2 Convenience Module: `stdlib/stm-utils.tur`

```clojure
;; With macros
(defmacro with-tvar [[name init] & body]
  `(let [~name (TVar::new ~init)]
     (atomically (stm ~@body))))

(defmacro dosync [& body]
  `(atomically (stm ~@body)))

(defmacro stm-when [cond & body]
  `(stm
     (when ~cond ~@body)))

(defmacro stm-unless [cond & body]
  `(stm
     (unless ~cond ~@body)))

;; Common patterns
(defn TVar::update [tv f & args]
  (TVar::modify tv (fn [x] (apply f x args))))

(defn TVar::cas [tv old new]
  (stm
    (let [current (TVar::read tv)]
      (if (= current old)
        (do (TVar::write tv new) true)
        false))))

;; Batch operations
(defn atomically-batch [& txs]
  (atomically
    (stm
      (doseq [tx txs]
        (tx)))))
```

---

## 9. Testing Strategy

### 9.1 Unit Tests

| Test | Description |
|------|-------------|
| `stm-tvar-basic.tur` | Basic TVar create/read/write |
| `stm-tvar-modify.tur` | TVar modify operations |
| `stm-atomicity.tur` | Transaction atomicity verification |
| `stm-isolation.tur` | Transaction isolation (no intermediate states visible) |
| `stm-retry.tur` | Retry on TVar change |
| `stm-check.tur` | Check condition and abort |
| `stm-or-else.tur` | or-else combinator |
| `stm-tmvar.tur` | TMVar operations |
| `stm-tchan.tur` | TChan operations |
| `stm-tsem.tur` | TSem operations |

### 9.2 Concurrency Tests

| Test | Description |
|------|-------------|
| `stm-concurrent-writes.tur` | Multiple threads writing to same TVar |
| `stm-concurrent-transfers.tur` | Concurrent bank transfers (no money lost/created) |
| `stm-deadlock-free.tur` | Verify no deadlocks with complex transaction patterns |
| `stm-starvation.tur` | Verify fairness (no transaction starved) |
| `stm-stress.tur` | High-contention stress test |

### 9.3 Integration Tests

| Test | Description |
|------|-------------|
| `stm-with-arc.tur` | STM with Arc-wrapped TVars |
| `stm-with-defer.tur` | Defer inside STM transactions |
| `stm-with-threads.tur` | STM with Thread primitives |

---

## 10. Performance Considerations

### 10.1 Benchmarks

| Benchmark | Description | Target |
|-----------|-------------|--------|
| `stm-counter` | Increment a TVar in a loop | < 100ns per operation (v1) |
| `stm-transfer` | Transfer between 2 TVars | < 200ns per transfer (v1) |
| `stm-bank` | Concurrent bank simulation | Linear scalability (v2+) |
| `stm-tchan-throughput` | TChan write/read throughput | > 1M ops/sec |

### 10.2 Optimizations

**V1 optimizations:**
- Inline small transactions
- Cache frequently accessed TVars
- Batch TVar reads/writes

**V2 optimizations:**
- Fine-grained locking (per-TVar)
- Lock ordering heuristics
- Transaction coalescing

**V3 optimizations:**
- Lock-free algorithms
- MW-CAS where available
- Software transactional memory (STMM)

---

## 11. Phasing and Milestones

### 11.1 Phase 20: Core STM (V1)

**Prerequisites:**
- Phase 19 (Delimited continuations / Algebraic effects)
- Phase 1 (Thread primitives: `Mutex<T>`, condition variables)

**Deliverables:**
- [ ] `TVar<T>` implementation
- [ ] `stm` / `atomically` forms
- [ ] `retry`, `check`, `or-else`
- [ ] `TMVar<T>`, `TChan<T>`, `TSem`
- [ ] Global lock implementation
- [ ] Basic stdlib (`stdlib/stm.tur`)
- [ ] Unit tests
- [ ] Simple concurrency tests

**Exit criterion:** STM works correctly for single-threaded and simple multi-threaded use cases. Performance is acceptable for low-contention workloads.

### 11.2 Phase 21: Scalable STM (V2)

**Prerequisites:**
- Phase 20 (Core STM)

**Deliverables:**
- [ ] Fine-grained locking (per-TVar)
- [ ] Lock ordering to prevent deadlocks
- [ ] Lock stripping for reduced contention
- [ ] Performance benchmarks
- [ ] Stress tests

**Exit criterion:** STM scales to high-concurrency workloads with acceptable performance.

### 11.3 Future: Lock-Free STM (V3)

**Deliverables:**
- [ ] MW-CAS based implementation
- [ ] Platform-specific optimizations
- [ ] Fallback paths

**Status:** Research / stretch goal

---

## 12. Related Work

### 12.1 Haskell STM

- **Paper:** "Composable Memory Transactions" (Harris, Marlow, Peyton Jones, Herlihy, 2005)
- **Implementation:** GHC's `stm` package
- **Key insight:** STM as a monad with `retry`

### 12.2 Other STM Implementations

| Language | Implementation | Notes |
|----------|---------------|-------|
| Clojure | `clojure.core/atom` + STM libs | Software-based, not built-in |
| Scala | ScalaSTM | Library-level STM |
| Python | `transaction` package | Software-based |
| Java | Multiverse STM | High-performance Java STM |
| C/C++ | libstm | Software transactional memory |

### 12.3 Alternatives Considered

| Approach | Pros | Cons |
|----------|------|------|
| **Lock-based (chosen)** | Simple, correct, portable | Global lock is a bottleneck |
| **Lock-free MW-CAS** | Best performance | Complex, platform-dependent |
| **Hybrid (lock + lock-free)** | Best of both | Very complex |
| **Transactional Boost** | C++ standard | C++ specific, not applicable |

---

## 13. Open Questions

1. **Naming:** Should we use `TVar` (Haskell) or `TxVar`? `stm` or `transaction`?
2. **Error handling:** What happens when a transaction throws an exception?
3. **Timeouts:** Should `atomically` support timeouts?
4. **Nested transactions:** Should we support nested transactions in the future?
5. **STM + Effects:** How should STM integrate with the effect system in v2?
6. **Debugging:** How do we provide visibility into transaction conflicts?

---

## 14. Appendix: Example Programs

### 14.1 Bank Account Transfer

```clojure
(defstruct Account [balance : TVar[int]])

(defn new-account [initial]
  (Account. (TVar::new initial)))

(defn get-balance [account]
  (atomically (stm (TVar::read (:balance account)))))

(defn deposit [account amount]
  (atomically
    (stm
      (TVar::update (:balance account) + amount))))

(defn withdraw [account amount]
  (atomically
    (stm
      (let [balance (TVar::read (:balance account))]
        (check (>= balance amount))
        (TVar::write (:balance account) (- balance amount))
        true))))

(defn transfer [from to amount]
  (atomically
    (stm
      (let [from-bal (TVar::read (:balance from))
            to-bal (TVar::read (:balance to))]
        (check (>= from-bal amount))
        (TVar::write (:balance from) (- from-bal amount))
        (TVar::write (:balance to) (+ to-bal amount))
        true))))
```

### 14.2 Producer-Consumer with TChan

```clojure
(defn producer [chan n]
  (dotimes [i n]
    (atomically (TChan::write chan i))
    (Thread::sleep 10)))

(defn consumer [chan]
  (loop []
    (let [val (atomically (TChan::read chan))]
      (println "Got:" val)
      (recur))))

(defn main []
  (let [chan (atomically (TChan::new))]
    (Thread::spawn (fn [] (producer chan 10)))
    (Thread::spawn (fn [] (consumer chan)))
    (Thread::join thread1)
    (Thread::join thread2)))
```

### 14.3 Barrier with TSem

```clojure
(defn barrier [n]
  (let [sem1 (atomically (TSem::new 0))
        sem2 (atomically (TSem::new (dec n)))]
    (fn []
      (atomically
        (stm
          (TSem::signal sem1)
          (TSem::wait sem2))))))

;; Usage:
(def barrier (barrier 10))
;; 10 threads call (barrier) - all block until all 10 arrive
```

### 14.4 Work Queue

```clojure
(defstruct WorkQueue [queue : TVar[(list Task)], lock : TVar[bool]])

(defn new-work-queue []
  (WorkQueue. (TVar::new '()) (TVar::new false)))

(defn enqueue [q task]
  (atomically
    (stm
      (TVar::modify (:queue q) (fn [qs] (cons task qs)))
      (when (TVar::read (:lock q))
        (TVar::write (:lock q) false)))))

(defn dequeue [q]
  (atomically
    (stm
      (loop []
        (let [qs (TVar::read (:queue q))]
          (if (seq qs)
            (do
              (TVar::write (:queue q) (rest qs))
              (first qs))
            (do
              (TVar::write (:lock q) true)
              (retry))))))))
```

---

## 15. Glossary

| Term | Definition |
|------|------------|
| **STM** | Software Transactional Memory |
| **TVar** | Transactional Variable |
| **TMVar** | Transactional MVar (empty/full variable) |
| **TChan** | Transactional Channel |
| **TSem** | Transactional Semaphore |
| **retry** | Block and retry transaction when TVar changes |
| **check** | Assert condition, abort transaction if false |
| **atomically** | Execute transaction, block until commit |
| **or-else** | Try first transaction, then second if first retries |

---

## 16. References

1. Harris, T., Marlow, S., Peyton Jones, S., & Herlihy, M. (2005). "Composable Memory Transactions." ACM SIGPLAN.
2. Harris, T., & Fraser, K. (2003). "Language Support for Lightweight Transactions." OOPSLA.
3. Herlihy, M., & Moss, J. E. B. (1993). "Transactional Memory: Architectural Support for Lock-Free Data Structures." ISCA.
4. GHC STM Documentation: https://hackage.haskell.org/package/stm
5. C11 Standard (ISO/IEC 9899:2011) — Threads and Atomics
