---
title: Checkpointing and Persistent Workflows
category: Advanced Control Flow
description: Cloneable continuations for persistent workflows and checkpointing
---

# Checkpointing and Persistent Workflows with Serializable Continuations

Save and resume computations across process boundaries using serializable continuations.

## Overview

Turmeric v2 will support **serializable continuations**, enabling suspended computations to be marshalled to bytes, persisted to disk or sent over a network, then resumed in a fresh process. This enables:

- Persistent multi-step workflows (pause and resume days later)
- Distributed task migration (send half-finished computation to another node)
- Checkpointing (save state periodically; restart from checkpoint on crash)
- Web continuations (Racket-style: continuation as URL token)
- Mobile agents (send code + state to remote peer)
- Debugger snapshots (freeze and replay)

## Core Concept

Phase 18's delimited continuations reify the call stack as a heap-allocated closure chain. Each frame is a struct on the heap. Serialization traverses this chain, emitting a stable encoding, then reconstructs on load.

```turmeric
;; Capture a continuation
(def saved #f)
(def result
  (+ 1 (cloneable-shift [k]
         (set! saved k)
         10)))

;; Serialize to bytes
(def bytes (serialize saved))

;; Write to disk or send over network
(write-file "continuation.dat" bytes)

;; Later, in another process:
(def k (deserialize (read-file "continuation.dat")))
(resume k 42)  ; => 43
```

```sweet-exp
;; Capture a continuation
def saved #f
def result
  {1 + cloneable-shift([k]
    set!(saved k)
    10)}

;; Serialize to bytes
def bytes serialize(saved)

;; Write to disk or send over network
write-file("continuation.dat" bytes)

;; Later, in another process:
def k deserialize(read-file("continuation.dat"))
resume(k 42)  ; => 43
```

## Serialization Design

### Stable Symbol Table

Function pointers are not portable across builds. Each continuation frame stores:

```turmeric
(struct continuation-frame
  [fn-symbol : string  ; e.g., "mymodule.myfunction"
   args : (list any)   ; serializable arguments
   captures : (map symbol any)])  ; captured variables
```

```sweet-exp
struct continuation-frame
  [fn-symbol : string  ; e.g., "mymodule.myfunction"
   args : (list any)   ; serializable arguments
   captures : (map symbol any)]  ; captured variables
```

On deserialization, the symbol is resolved to the current build's function pointer.

### The Serializable Typeclass

Not all types can be serialized. Opt-in via the `Serializable` trait:

```turmeric
(defclass Serializable [a]
  (serialize [x : a] : bytes)
  (deserialize [b : bytes] : a))

;; Primitive implementations
(instance Serializable int64 ...)
(instance Serializable string ...)
(instance Serializable bool ...)

;; Derived implementations
(instance Serializable (Pair a b) [Serializable a, Serializable b] ...)

;; NOT serializable
(instance Serializable FileHandle
  ;; Custom handler: store file path, re-open on deserialize
  (serialize [fh] (file-handle-path fh))
  (deserialize [path] (open-file path)))
```

```sweet-exp
defclass Serializable [a]
  serialize([x : a] : bytes)
  deserialize([b : bytes] : a)

;; Primitive implementations
instance Serializable int64 ...
instance Serializable string ...
instance Serializable bool ...

;; Derived implementations
instance Serializable (Pair a b) [Serializable a, Serializable b] ...

;; NOT serializable
instance Serializable FileHandle
  ;; Custom handler: store file path, re-open on deserialize
  serialize([fh] file-handle-path(fh))
  deserialize([path] open-file(path))
```

### Resource Types

File handles, sockets, and other system resources can define custom **marshal/unmarshal hooks**:

```turmeric
(defclass Resource-Serializable [a]
  ;; Serialize to a stable representation
  (marshal [x : a] : resource-token)
  ;; Restore from token in new process
  (unmarshal [token : resource-token] : a))

(instance Resource-Serializable FileHandle
  (marshal [fh] (file-handle-path fh))
  (unmarshal [path] (open-file path)))
```

```sweet-exp
defclass Resource-Serializable [a]
  ;; Serialize to a stable representation
  marshal([x : a] : resource-token)
  ;; Restore from token in new process
  unmarshal([token : resource-token] : a)

instance Resource-Serializable FileHandle
  marshal([fh] file-handle-path(fh))
  unmarshal([path] open-file(path))
```

### Ownership Model and Serialization

Serialized continuations produce a **deep copy**. Ownership is transferred; originals are invalidated:

```turmeric
(def r (ref 42))
(cloneable-shift [k]
  (serialize k))  ; Serialization deep-copies r
                  ; Original r is now inaccessible
(deserialize bytes)  ; Deserialize: new r created with value 42
```

```sweet-exp
def r ref(42)
cloneable-shift([k]
  serialize(k))  ; Serialization deep-copies r
                 ; Original r is now inaccessible
deserialize(bytes)  ; Deserialize: new r created with value 42
```

This is safe because:
- The original continuation is no longer reachable (it was consumed by `shift`).
- The deserialized continuation has a fresh copy of captured state.
- No aliasing between old and new process.

## Example: Persistent Workflow

A multi-step business process that survives crashes:

```turmeric
(defn process-order [order-id]
  ;; Step 1: Validate order
  (def order (load-order order-id))
  (unless (valid-order? order)
    (throw (validation-error "Invalid order")))
  (checkpoint "order-validated" order)
  
  ;; Step 2: Charge payment (slow network call)
  (def charge-result (charge-payment order.payment-info))
  (checkpoint "payment-charged" charge-result)
  
  ;; Step 3: Fulfill order
  (def fulfillment (fulfill order charge-result))
  (checkpoint "order-fulfilled" fulfillment)
  
  fulfillment)

;; Checkpointing macro
(defmacro checkpoint [name value]
  `(cloneable-shift [k]
     ;; Save continuation to disk
     (def checkpoint-file (str "checkpoint-" ~name ".bin"))
     (write-file checkpoint-file (serialize k))
     ;; Resume immediately on first run
     (continue k ~value)))

;; On crash, user can resume from last checkpoint
(defn resume-from-checkpoint [name]
  (def checkpoint-file (str "checkpoint-" name ".bin"))
  (def k (deserialize (read-file checkpoint-file)))
  (resume k))
```

```sweet-exp
defn process-order [order-id]
  ;; Step 1: Validate order
  def order load-order(order-id)
  unless valid-order?(order)
    throw(validation-error("Invalid order"))
  checkpoint("order-validated" order)

  ;; Step 2: Charge payment (slow network call)
  def charge-result charge-payment(order.payment-info)
  checkpoint("payment-charged" charge-result)

  ;; Step 3: Fulfill order
  def fulfillment fulfill(order charge-result)
  checkpoint("order-fulfilled" fulfillment)

  fulfillment

;; Checkpointing macro
defmacro checkpoint [name value]
  `(cloneable-shift [k]
     ;; Save continuation to disk
     (def checkpoint-file (str "checkpoint-" ~name ".bin"))
     (write-file checkpoint-file (serialize k))
     ;; Resume immediately on first run
     (continue k ~value))

;; On crash, user can resume from last checkpoint
defn resume-from-checkpoint [name]
  def checkpoint-file str("checkpoint-" name ".bin")
  def k deserialize(read-file(checkpoint-file))
  resume(k)
```

## Example: Distributed Task Migration

Send a half-finished computation to another node:

```turmeric
;; Node A: long-running job, half done
(def job
  (cloneable-reset
    (fn []
      (def task1-result (run-task1))
      (def task2-result (run-task2 task1-result))
      (def task3-result (run-task3 task2-result))
      task3-result)))

;; Save state
(def bytes (serialize job))
(send-to-node-b bytes)

;; Node B: resume
(def job (deserialize (receive-bytes))
(def result (resume job))
```

```sweet-exp
;; Node A: long-running job, half done
def job
  cloneable-reset
    fn []
      def task1-result run-task1()
      def task2-result run-task2(task1-result)
      def task3-result run-task3(task2-result)
      task3-result

;; Save state
def bytes serialize(job)
send-to-node-b(bytes)

;; Node B: resume
def job deserialize(receive-bytes())
def result resume(job)
```

## Example: Web Continuations (Racket-style)

Serialize "what to do when form is submitted" as a URL token:

```turmeric
;; Initial page
(defn get-checkout [req]
  (cloneable-shift [k]
    ;; Save continuation to disk, return URL token
    (def token (save-continuation-to-db k))
    (render-page
      (form :action (str "/checkout-submit?token=" token)))))

;; Form submission handler
(defn post-checkout-submit [token req]
  ;; Load and resume continuation
  (def k (load-continuation-from-db token))
  (def response (resume k (parse-form-data req)))
  response)
```

```sweet-exp
;; Initial page
defn get-checkout [req]
  cloneable-shift([k]
    ;; Save continuation to disk, return URL token
    def token save-continuation-to-db(k)
    render-page
      form(:action str("/checkout-submit?token=" token)))

;; Form submission handler
defn post-checkout-submit [token req]
  ;; Load and resume continuation
  def k load-continuation-from-db(token)
  def response resume(k parse-form-data(req))
  response
```

## Example: Checkpointing Long-Running Computation

Periodic snapshots for crash recovery:

```turmeric
(defn analyze-large-dataset [data]
  (defn checkpoint-every-n [n items]
    (let [processed []]
      (for-each-with-index items
        (fn [i item]
          (set! processed (conj processed (process item)))
          (when (= (mod (+ i 1) n) 0)
            ;; Checkpoint every n items
            (cloneable-shift [k]
              (write-file (str "checkpoint-" i ".bin")
                         (serialize k))
              (continue k)))))))
  
  (checkpoint-every-n 1000 data))
```

```sweet-exp
defn analyze-large-dataset [data]
  defn checkpoint-every-n [n items]
    let [processed []]
      for-each-with-index(items
        fn [i item]
          set!(processed conj(processed process(item)))
          when {mod({i + 1} n) = 0}
            ;; Checkpoint every n items
            cloneable-shift([k]
              write-file(str("checkpoint-" i ".bin")
                         serialize(k))
              continue(k)))

  checkpoint-every-n(1000 data)
```

## Reconstruction and Error Handling

### Schema Versioning

Continuation frames carry schema version. Mismatches produce an error:

```turmeric
(def k (deserialize bytes))  ; May fail if:
                             ; - Function no longer exists
                             ; - Argument types changed
                             ; - Captured types are incompatible
```

```sweet-exp
def k deserialize(bytes)  ; May fail if:
                          ; - Function no longer exists
                          ; - Argument types changed
                          ; - Captured types are incompatible
```

Error handling:

```turmeric
(try-with
  (fn []
    (deserialize (read-file "checkpoint.bin")))
  (fn [e k]
    (match e
      (schema-mismatch _ old-version) ->
        (throw (error (str "Cannot resume: checkpoint uses version " old-version
                           " but current code is version " (current-version)))))))
```

```sweet-exp
try-with
  fn []
    deserialize(read-file("checkpoint.bin"))
  fn [e k]
    match e
      (schema-mismatch _ old-version) ->
        throw $ error $ str("Cannot resume: checkpoint uses version " old-version
                            " but current code is version " current-version())
```

### Partial Reconstruction

If deserialization of a captured value fails, the whole continuation fails. To tolerate missing state:

```turmeric
;; Wrap potentially failing values in Option
(def opt-value
  (try
    (deserialize captured-value)
    (catch [e] (None))))
```

```sweet-exp
;; Wrap potentially failing values in Option
def opt-value
  try
    deserialize(captured-value)
    catch([e] None())
```

## Performance Considerations

### Serialization Overhead

- **Small continuations (~1-10 frames):** Microsecond-scale serialization
- **Large continuations (100+ frames):** Millisecond-scale; consider streaming
- **Deep captured state:** Clone overhead proportional to state size

### Strategies

1. **Limit continuation depth** -- Design workflows to have shallow call stacks.
2. **Minimize captured state** -- Use identifiers (e.g., order ID) instead of entire objects.
3. **Lazy serialization** -- For large state, write once, reference by ID on resumption.
4. **Incremental checkpointing** -- Save deltas instead of full continuation.

## API Summary

```turmeric
;; Serialize a continuation
(serialize cont : (cloneable-shift [k] k)) : bytes

;; Deserialize a continuation
(deserialize bytes : bytes) : (cloneable-shift [k] k)

;; Resume a continuation with a value
(resume k : (cloneable-shift [k] k) v : a) : a

;; Checkpoint macro (example)
(checkpoint name value)

;; Resource marshalling
(marshal resource : a) : resource-token
(unmarshal token : resource-token) : a
```

```sweet-exp
;; Serialize a continuation
serialize(cont : (cloneable-shift [k] k)) : bytes

;; Deserialize a continuation
deserialize(bytes : bytes) : (cloneable-shift [k] k)

;; Resume a continuation with a value
resume(k : (cloneable-shift [k] k) v : a) : a

;; Checkpoint macro (example)
checkpoint(name value)

;; Resource marshalling
marshal(resource : a) : resource-token
unmarshal(token : resource-token) : a
```

## See Also

- [Logic Programming Guide](logic-programming-guide.md) -- Cloneable continuations for backtracking
- [Async/Await Guide](async-await-guide.md) -- One-shot continuations for async I/O
- [Effects System Guide](effects-system-guide.md) -- Dynamic effect handling
- [turmeric-plan.md](../turmeric-plan.md) §18 -- Delimited continuations
