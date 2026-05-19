---
title: Software Transactional Memory (STM) Tutorial
category: Concurrency and Async
description: STM tutorial: concepts, patterns, and worked examples
---

# Software Transactional Memory (STM) Tutorial

Composable, deadlock-free concurrent programming using transactional memory.

## Overview

**Software Transactional Memory (STM)** provides an alternative to locks. Transactions execute atomically with automatic conflict detection and retry, eliminating deadlocks and priority inversion while enabling composability.

Turmeric's STM is modeled on **Haskell's `Control.Concurrent.STM`**, offering a proven API and semantics.

## Core Concepts

### Transactions as Atomic Units

Like database transactions, an STM transaction either completes entirely or rolls back:

```turmeric
;; Transfer funds between accounts atomically
(atomically
  (fn []
    (let [from-balance (read-tvar from-account)
          to-balance   (read-tvar to-account)]
      (when (>= from-balance amount)
        (write-tvar from-account (- from-balance amount))
        (write-tvar to-account (+ to-balance amount))))))
```

### Transactional Variables: TVar

**`TVar<T>`** is a mutable reference that can only be accessed within a transaction:

```turmeric
;; Create a transactional variable
(def counter (new-tvar 0))

;; Read within a transaction
(atomically
  (fn []
    (let [value (read-tvar counter)]
      (write-tvar counter (+ value 1)))))
```

### Properties

- **Atomicity:** A transaction completes entirely or rolls back.
- **Isolation:** Concurrent transactions appear to execute serially.
- **Composability:** Transactions can call other transactions freely without deadlock risk.
- **Automatic retry:** On conflict, transactions restart from the beginning.

## Advantages Over Locks

| Aspect | Locks | STM |
|--------|-------|-----|
| **Deadlock** | Possible; require careful ordering | Impossible; automatic retry |
| **Priority inversion** | Possible | Impossible |
| **Composability** | Difficult; lock ordering required | Easy; transactions compose freely |
| **Error handling** | Lock held during exception; manual cleanup | Automatic cleanup on transaction abort |
| **Debugging** | Hard; deadlock traces are complex | Easier; transactional semantics |

## Quick Start

### Simple Counter

```turmeric
(def counter (new-tvar 0))

;; Increment atomically
(atomically
  (fn []
    (let [v (read-tvar counter)]
      (write-tvar counter (+ v 1)))))

(println (atomically (fn [] (read-tvar counter))))  ; => 1
```

### Bank Transfer (No Deadlock!)

```turmeric
;; Accounts as transactional variables
(def account-a (new-tvar 100))
(def account-b (new-tvar 50))

;; Transfer with automatic conflict resolution
(defn transfer [from to amount]
  (atomically
    (fn []
      (let [from-balance (read-tvar from)]
        (when (>= from-balance amount)
          (write-tvar from (- from-balance amount))
          (write-tvar to (+ (read-tvar to) amount)))))))

(transfer account-a account-b 30)

;; Concurrent transfers never deadlock!
(thread (fn [] (transfer account-a account-b 10)))
(thread (fn [] (transfer account-b account-a 5)))
```

## Core API

### Creating and Reading/Writing

```turmeric
;; Create a transactional variable
(def tvar (new-tvar 42))

;; Read (only inside atomically)
(atomically
  (fn []
    (read-tvar tvar)))  ; => 42

;; Write (only inside atomically)
(atomically
  (fn []
    (write-tvar tvar 100)))

;; Value is now 100
(atomically
  (fn []
    (read-tvar tvar)))  ; => 100
```

### Running a Transaction

```turmeric
;; Atomically: run transaction, retry on conflict
(atomically tx)  ; => result

;; Returns the result of the transaction
(def result
  (atomically
    (fn []
      (let [x (read-tvar counter)]
        (write-tvar counter (+ x 1))
        (+ x 1)))))
```

### Retry on Conflict

```turmeric
;; Wait until balance > 10
(atomically
  (fn []
    (let [balance (read-tvar account)]
      (when (<= balance 10)
        (retry)))))  ; Block and re-run when any watched TVar changes
```

When `retry` is called:
1. The transaction aborts (without side effects).
2. Turmeric records which `TVar`s were read.
3. The transaction sleeps until one of those `TVar`s changes.
4. Execution resumes from the beginning.

### Choice: Try One Branch Then Another

```turmeric
;; Try to withdraw from account-a, else account-b
(atomically
  (fn []
    (or-else
      (fn [] (withdraw account-a 50))
      (fn [] (withdraw account-b 50)))))
```

If the first branch calls `retry`, the second branch is tried. Both branches see the same transactional state at the moment of choice.

## Transactional Synchronization Primitives

### TMVar: Single-Slot Mailbox

```turmeric
;; Create an empty mailbox
(def mvar (new-tmvar))

;; Try to read (blocks if empty, retries)
(atomically
  (fn []
    (read-tmvar mvar)))  ; Will retry until a value is available

;; Write (blocks if full, retries)
(atomically
  (fn []
    (write-tmvar mvar 42)))
```

### TChan: FIFO Channel

```turmeric
;; Create a channel
(def chan (new-tchan))

;; Write to channel
(atomically
  (fn []
    (write-tchan chan "hello")))

;; Read from channel (blocks if empty, retries)
(atomically
  (fn []
    (read-tchan chan)))  ; => "hello"
```

## Common Patterns

### Producer-Consumer

```turmeric
;; Shared queue
(def queue (new-tmvar '()))

(thread
  (fn []
    ;; Producer: generate items
    (for-each (range 10)
      (fn [i]
        (atomically
          (fn []
            (def q (read-tmvar queue))
            (write-tmvar queue (conj q i))))
        (sleep 100)))))

(thread
  (fn []
    ;; Consumer: process items
    (loop
      (atomically
        (fn []
          (def q (read-tmvar queue))
          (when (empty? q)
            (retry))
          (def item (car q))
          (write-tmvar queue (cdr q))
          (println item))))))
```

### Reader-Writer Lock

```turmeric
;; Using TMVar as a simple semaphore
(def write-lock (new-tmvar true))

(defn with-write-lock [f]
  (atomically
    (fn []
      (read-tmvar write-lock)  ; Acquire
      (let [result (f)]
        (write-tmvar write-lock true)  ; Release
        result))))

(with-write-lock
  (fn []
    ;; Write-protected section
    ))
```

### Barrier

```turmeric
;; Synchronize N threads
(defn barrier-new [n]
  (new-tmvar (list :count n :waiting 0)))

(defn barrier-wait [barrier]
  (atomically
    (fn []
      (def state (read-tmvar barrier))
      (def waiting (+ (get state :waiting) 1))
      (if (= waiting (get state :count))
        ;; All threads arrived
        (write-tmvar barrier (assoc state :waiting 0))
        ;; Wait for others
        (do
          (write-tmvar barrier (assoc state :waiting waiting))
          (retry))))))
```

## Limitations (v1)

- **No nested transactions:** Calling `atomically` inside `atomically` raises an error. (Haskell allows this; Turmeric v2+ may support it.)
- **No interactive transactions:** Transactions must be pure (no I/O); side effects inside `atomically` may occur multiple times on retry.
- **Performance:** Lock-based implementation (v1); wait-free or lock-free variants planned for v2+.

## Example: Concurrent Merge Sort

```turmeric
(defn merge-sort-stm [vec]
  (if (<= (len vec) 1)
    vec
    (let [mid (/ (len vec) 2)
          left-result (new-tmvar #f)
          right-result (new-tmvar #f)]
      
      ;; Sort left half in parallel
      (thread
        (fn []
          (atomically
            (fn []
              (write-tmvar left-result
                          (merge-sort-stm (slice vec 0 mid)))))))
      
      ;; Sort right half in parallel
      (thread
        (fn []
          (atomically
            (fn []
              (write-tmvar right-result
                          (merge-sort-stm (slice vec mid (len vec))))))))
      
      ;; Merge results
      (atomically
        (fn []
          (let [left (read-tmvar left-result)
                right (read-tmvar right-result)]
            (when (or (nil? left) (nil? right))
              (retry))
            (merge left right)))))))
```

## Performance Tips

1. **Keep transactions short:** Minimize the time window to reduce conflict probability.
2. **Read-only transactions:** Batch reads into an `atomically` block.
3. **Predicate isolation:** Use `retry` and watches efficiently; don't poll.
4. **Watch out for `orElse`:** Can cause cascading retries; use judiciously.

## See Also

- [Threading Guide](threading-guide.md) — Locks, mutexes, atomic types
- [Async/Await Guide](async-await-guide.md) — Lightweight concurrency with fibers
- [Effects System Guide](effects-system-guide.md) — Exception handling in transactions
- [turmeric-plan.md](../turmeric-plan.md) §19 — Thread primitives (prerequisite)
