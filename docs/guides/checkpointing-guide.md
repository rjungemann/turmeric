---
title: Checkpointing and Persistent Workflows
category: Advanced Control Flow
description: Cloneable continuations for persistent workflows and checkpointing
---

# Checkpointing and Persistent Workflows with Serializable Continuations

Save and resume computations across process boundaries using serializable continuations.

## Overview

Turmeric supports **serializable continuations**, enabling suspended computations to be marshalled to bytes, persisted to disk or sent over a network, then resumed in a fresh process. This enables:

- Persistent multi-step workflows (pause and resume days later)
- Distributed task migration (send half-finished computation to another node)
- Checkpointing (save state periodically; restart from checkpoint on crash)
- Web continuations (Racket-style: continuation as URL token)
- Mobile agents (send code + state to remote peer)
- Debugger snapshots (freeze and replay)

The concrete surface is `serial-reset` / `serial-shift` plus
`serial-cont->bytes`, `bytes->serial-cont`, `serial-resume`, and the
`cont-to-file` / `cont-from-file` helpers in `stdlib/serial.tur`. The compact
API reference lives in
[serializable-continuations-guide.md](serializable-continuations-guide.md);
this guide focuses on the checkpointing patterns built on top of it.

## Core Concept

Delimited continuations reify the call stack as a heap-allocated closure chain. Each frame is a struct on the heap. Serialization traverses this chain, emitting a stable encoding, then reconstructs on load.

```turmeric no-check
;; Capture a continuation and write it to disk
(serial-reset
  (+ 1 (serial-shift k
         ;; serialize to bytes, write to disk or send over a network
         (cont-to-file (serial-cont->bytes k) "continuation.dat")
         10)))

;; Later, in another process:
(def k-result (bytes->serial-cont (cont-from-file "continuation.dat")))
(serial-resume (ok-val k-result) 42)  ; => 43
```

```sweet-exp
;; Capture a continuation and write it to disk
serial-reset
  {1 + serial-shift(k
         (cont-to-file (serial-cont->bytes k) "continuation.dat")
         10)}

;; Later, in another process:
def k-result bytes->serial-cont(cont-from-file("continuation.dat"))
serial-resume(ok-val(k-result) 42)  ; => 43
```

## Serialization Design

### Stable Symbol Table

Function pointers are not portable across builds. Each continuation frame stores:

```turmeric no-check
(defstruct continuation-frame
  [fn-symbol : string  ; e.g., "mymodule.myfunction"
   args : (list any)   ; serializable arguments
   captures : (map symbol any)])  ; captured variables
```

```sweet-exp
defstruct continuation-frame
  [fn-symbol : string  ; e.g., "mymodule.myfunction"
   args : (list any)   ; serializable arguments
   captures : (map symbol any)]  ; captured variables
```

On deserialization, the symbol is resolved to the current build's function pointer.

### The Serializable Typeclass

Not all types can be serialized. Opt-in via the `Serializable` trait:

```turmeric no-check
(defclass Serializable [a]
  (serialize [x : a] : bytes)
  (deserialize [b : bytes] : (Result a cstr)))

;; Primitive implementations
(definstance Serializable int64)
(definstance Serializable bool)
(definstance Serializable cstr)

;; A resource type serializes a stable representation instead -- e.g. store
;; the file path and re-open on deserialize (see Resource Types below).
```

```sweet-exp
defclass Serializable [a]
  serialize [x : a] : bytes
  deserialize [b : bytes] : (Result a cstr)

;; Primitive implementations
definstance Serializable int64
definstance Serializable bool
definstance Serializable cstr

;; A resource type serializes a stable representation instead -- e.g. store
;; the file path and re-open on deserialize (see Resource Types below).
```

### Resource Types

File handles, sockets, and other system resources can define custom **marshal/unmarshal hooks**:

```turmeric no-check
(defclass Resource-Serializable [a]
  ;; Serialize to a stable representation
  (marshal [x : a] : resource-token)
  ;; Restore from token in new process
  (unmarshal [token : resource-token] : a))

(definstance Resource-Serializable [FileHandle]
  (marshal [fh] (file-handle-path fh))
  (unmarshal [path] (open-file path)))
```

```sweet-exp
defclass Resource-Serializable [a]
  ;; Serialize to a stable representation
  marshal [x : a] : resource-token
  ;; Restore from token in new process
  unmarshal [token : resource-token] : a

definstance Resource-Serializable [FileHandle]
  marshal([fh] file-handle-path(fh))
  unmarshal([path] open-file(path))
```

### Ownership Model and Serialization

Serialized continuations produce a **deep copy**. Ownership is transferred; originals are invalidated:

```turmeric no-check
(def r (ref 42))
(serial-shift k
  (serial-cont->bytes k))  ; Serialization deep-copies r
                           ; Original r is now inaccessible
(bytes->serial-cont bytes)  ; Deserialize: new r created with value 42
```

```sweet-exp
def r ref(42)
serial-shift(k
  serial-cont->bytes(k))  ; Serialization deep-copies r
                          ; Original r is now inaccessible
bytes->serial-cont(bytes)  ; Deserialize: new r created with value 42
```

This is safe because:
- The original continuation is no longer reachable (it was consumed by `shift`).
- The deserialized continuation has a fresh copy of captured state.
- No aliasing between old and new process.

## Example: Persistent Workflow

A multi-step business process that survives crashes:

```turmeric no-check
(defn process-order [order-id]
  ;; Step 1: Validate order
  (def order (load-order order-id))
  (when (not (valid-order? order))
    (panic "Invalid order"))
  (checkpoint "order-validated" order)

  ;; Step 2: Charge payment (slow network call)
  (def charge-result (charge-payment (order-payment-info order)))
  (checkpoint "payment-charged" charge-result)

  ;; Step 3: Fulfill order
  (def fulfillment (fulfill order charge-result))
  (checkpoint "order-fulfilled" fulfillment)

  fulfillment)

;; Checkpointing macro
(defmacro checkpoint [name value]
  `(serial-shift k
     ;; Save continuation to disk
     (do
       (cont-to-file (serial-cont->bytes k)
                     (str-concat "checkpoint-" ~name ".bin"))
       ;; Resume immediately on first run
       (serial-resume k ~value))))

;; On crash, user can resume from last checkpoint
(defn resume-from-checkpoint [name value]
  (def bytes (cont-from-file (str-concat "checkpoint-" name ".bin")))
  (def k (ok-val (bytes->serial-cont bytes)))
  (serial-resume k value))
```

```sweet-exp
defn process-order [order-id]
  ;; Step 1: Validate order
  def order load-order(order-id)
  when not(valid-order?(order))
    panic("Invalid order")
  checkpoint("order-validated" order)

  ;; Step 2: Charge payment (slow network call)
  def charge-result charge-payment(order-payment-info(order))
  checkpoint("payment-charged" charge-result)

  ;; Step 3: Fulfill order
  def fulfillment fulfill(order charge-result)
  checkpoint("order-fulfilled" fulfillment)

  fulfillment

;; Checkpointing macro
defmacro checkpoint [name value]
  `(serial-shift k
     (do
       (cont-to-file (serial-cont->bytes k)
                     (str-concat "checkpoint-" ~name ".bin"))
       (serial-resume k ~value)))

;; On crash, user can resume from last checkpoint
defn resume-from-checkpoint [name value]
  def bytes cont-from-file(str-concat("checkpoint-" name ".bin"))
  def k ok-val(bytes->serial-cont(bytes))
  serial-resume(k value)
```

## Example: Distributed Task Migration

Send a half-finished computation to another node:

```turmeric no-check
;; Node A: long-running job; after task1 it captures "the rest of the job"
;; and ships it to another node instead of running tasks 2 and 3 locally.
(serial-reset
  (let [task1-result (run-task1)
        handoff      (serial-shift k
                       (do (send-to-node-b (serial-cont->bytes k)) 0))]
    (run-task3 (run-task2 handoff))))

;; Node B: resume with task1's result
(def job (ok-val (bytes->serial-cont (receive-bytes))))
(def result (serial-resume job task1-result))
```

```sweet-exp
;; Node A: long-running job; after task1 it captures "the rest of the job"
;; and ships it to another node instead of running tasks 2 and 3 locally.
serial-reset
  let [task1-result run-task1()
       handoff      serial-shift(k
                      (do (send-to-node-b (serial-cont->bytes k)) 0))]
    run-task3(run-task2(handoff))

;; Node B: resume with task1's result
def job ok-val(bytes->serial-cont(receive-bytes()))
def result serial-resume(job task1-result)
```

## Example: Web Continuations (Racket-style)

Serialize "what to do when form is submitted" as a URL token:

```turmeric no-check
;; Initial page
(defn get-checkout [req]
  (serial-shift k
    ;; Save continuation to the store, return URL token
    (let [token (save-continuation-to-db (serial-cont->bytes k))]
      (render-page
        (form :action (str-concat "/checkout-submit?token=" token))))))

;; Form submission handler
(defn post-checkout-submit [token req]
  ;; Load and resume continuation
  (let [k (ok-val (bytes->serial-cont (load-continuation-from-db token)))]
    (serial-resume k (parse-form-data req))))
```

```sweet-exp
;; Initial page
defn get-checkout [req]
  serial-shift k
    ;; Save continuation to the store, return URL token
    let [token save-continuation-to-db(serial-cont->bytes(k))]
      render-page
        form(:action str-concat("/checkout-submit?token=" token))

;; Form submission handler
defn post-checkout-submit [token req]
  ;; Load and resume continuation
  let [k ok-val(bytes->serial-cont(load-continuation-from-db(token)))]
    serial-resume(k parse-form-data(req))
```

## Example: Checkpointing Long-Running Computation

Periodic snapshots for crash recovery:

```turmeric no-check
(defn analyze-large-dataset [data]
  (defn checkpoint-every-n [n items]
    (let [processed []]
      (for-each-with-index items
        (fn [i item]
          (set! processed (conj processed (process item)))
          (when (= (mod (+ i 1) n) 0)
            ;; Checkpoint every n items, then keep going
            (serial-shift k
              (do
                (cont-to-file (serial-cont->bytes k)
                              (str-concat "checkpoint-" (int->str i) ".bin"))
                (serial-resume k 0))))))))

  (checkpoint-every-n 1000 data))
```

```sweet-exp
defn analyze-large-dataset [data]
  defn checkpoint-every-n [n items]
    let [processed []]
      for-each-with-index items
        fn [i item]
          set!(processed conj(processed process(item)))
          when {mod({i + 1} n) = 0}
            ;; Checkpoint every n items, then keep going
            serial-shift k
              do
                cont-to-file(serial-cont->bytes(k)
                             str-concat("checkpoint-" int->str(i) ".bin"))
                serial-resume(k 0)

  checkpoint-every-n(1000 data)
```

## Reconstruction and Error Handling

### Schema Versioning

Continuation frames carry schema version. Mismatches produce an error:

```turmeric no-check
(def r (bytes->serial-cont bytes))  ; (err "...") if:
                                    ; - Function no longer exists
                                    ; - Argument types changed
                                    ; - Captured types are incompatible
```

```sweet-exp
def r bytes->serial-cont(bytes)  ; (err "...") if:
                                 ; - Function no longer exists
                                 ; - Argument types changed
                                 ; - Captured types are incompatible
```

`bytes->serial-cont` returns an ordinary `Result`, so error handling is a
`Result` check, not an exception handler:

```turmeric no-check
(let [r (bytes->serial-cont (cont-from-file "checkpoint.bin"))]
  (if (err? r)
    (panic (str-concat "Cannot resume: " (err-val r)))
    (serial-resume (ok-val r) 0)))
```

```sweet-exp
let [r bytes->serial-cont(cont-from-file("checkpoint.bin"))]
  if err?(r)
    panic(str-concat("Cannot resume: " err-val(r)))
    serial-resume(ok-val(r) 0)
```

### Partial Reconstruction

If deserialization of a captured value fails, the whole continuation fails --
`bytes->serial-cont` returns `(err msg)` rather than a half-reconstructed
continuation. To tolerate missing state, keep the fragile value out of the
captured frame (reference it by an identifier and re-load it after resume).

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

```turmeric no-check
;; Delimit a serializable region / capture the continuation
(serial-reset body)
(serial-shift k body)             ; k : serial-continuation<T>

;; Serialize / deserialize
(serial-cont->bytes k)            ; -> bytes
(bytes->serial-cont b)            ; -> (Result (serial-continuation<T>) cstr)

;; Resume a continuation with a value
(serial-resume k v)               ; -> T

;; File helpers (stdlib/serial.tur)
(cont-to-file b path)             ; write serialized bytes; 1 on success
(cont-from-file path)             ; read serialized bytes back

;; Checkpoint macro (example, defined above)
(checkpoint name value)
```

```sweet-exp
;; Delimit a serializable region / capture the continuation
serial-reset body
serial-shift k body              ; k : serial-continuation<T>

;; Serialize / deserialize
serial-cont->bytes(k)            ; -> bytes
bytes->serial-cont(b)            ; -> (Result (serial-continuation<T>) cstr)

;; Resume a continuation with a value
serial-resume(k v)               ; -> T

;; File helpers (stdlib/serial.tur)
cont-to-file(b path)             ; write serialized bytes; 1 on success
cont-from-file(path)             ; read serialized bytes back

;; Checkpoint macro (example, defined above)
checkpoint(name value)
```

## See Also

- [Logic Programming Guide](logic-programming-guide.md) -- Cloneable continuations for backtracking
- [Async/Await Guide](async-await-guide.md) -- One-shot continuations for async I/O
- [Effects System Guide](effects-system-guide.md) -- Dynamic effect handling
