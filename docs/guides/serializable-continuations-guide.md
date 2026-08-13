---
title: Serializable Continuations Guide
category: Advanced Control Flow
description: Serializable continuations for persistent workflows and cross-process computation
---

# Serializable Continuations Guide

Save and resume computations across process boundaries using serializable continuations.

## Overview

Turmeric's **serializable continuations** enable suspended computations to be marshalled to bytes, persisted to disk or sent over a network, then resumed in a fresh process. This builds on Phase 18's delimited continuations (`shift`/`reset`) which reify the call stack as a heap-allocated closure chain.

Use cases include:
- **Persistent workflows** -- Pause and resume multi-step business processes
- **Distributed task migration** -- Send half-finished computations to other nodes
- **Checkpointing** -- Save state periodically; restart from last checkpoint on crash
- **Web continuations** -- Racket-style: serialize "what to do next" as a URL token
- **Mobile agents** -- Send code + state to a remote peer for execution
- **Debugger snapshots** -- Freeze and replay running program state

## Core Concepts

### What is a Serializable Continuation?

A **continuation** represents "the rest of the computation" -- a suspended state that can be resumed later. A **serializable continuation** can be converted to bytes and restored, even in a different process.

```turmeric
;; Capture a serializable continuation
(serial-reset
  (let [x 42]
    (serial-shift k
      ;; k is a serializable continuation expecting an int64
      (save-to-disk k "my-continuation.bin")
      0)))

;; Later, in another process:
(def k (load-from-disk "my-continuation.bin"))
(serial-resume k 100)  ; Resumes computation with x=42, returns 142
```
```sweet-exp
;; Capture a serializable continuation
serial-reset
  let [x 42]
    serial-shift k
      ;; k is a serializable continuation expecting an int64
      save-to-disk(k "my-continuation.bin")
      0

;; Later, in another process:
def k load-from-disk("my-continuation.bin")
serial-resume(k 100)  ; Resumes computation with x=42, returns 142
```

### The `Serializable` Typeclass

Not all values can be serialized. Types must opt-in via the `Serializable` typeclass:

```turmeric
(defclass Serializable [a]
  (serialize   [x : a] : bytes)
  (deserialize [b : bytes] : (Result a cstr)))
```
```sweet-exp
defclass Serializable [a]
  serialize   [x : a] : bytes
  deserialize [b : bytes] : (Result a cstr)
```

Primitive types have automatic instances:

```turmeric
(definstance Serializable int64)
(definstance Serializable float64)
(definstance Serializable bool)
(definstance Serializable cstr)
(definstance Serializable bytes)
```
```sweet-exp
definstance Serializable int64
definstance Serializable float64
definstance Serializable bool
definstance Serializable cstr
definstance Serializable bytes
```

Container types derive their instances from element types:

```turmeric
(definstance (Serializable (Vec a)) [Serializable a])
(definstance (Serializable (Pair a b)) [Serializable a, Serializable b])
(definstance (Serializable (Option a)) [Serializable a])
(definstance (Serializable (Result a b)) [Serializable a, Serializable b])
```
```sweet-exp
definstance (Serializable (Vec a)) [Serializable a]
definstance (Serializable (Pair a b)) [Serializable a, Serializable b]
definstance (Serializable (Option a)) [Serializable a]
definstance (Serializable (Result a b)) [Serializable a, Serializable b]
```

Types that **do not** implement `Serializable` (file handles, raw pointers, `Mutex<T>`) cannot be captured in a serializable continuation. The elaborator enforces this at the `serial-reset` boundary.

### Resource Types

System resources like file handles can implement custom marshal/unmarshal logic:

```turmeric
(defclass ResourceSerializable [a]
  (marshal   [x : a] : value)
  (unmarshal [v : value] : a))

;; Example: serialize a file handle by its path, reopen on resume
(definstance ResourceSerializable FileHandle
  (marshal [fh] (file-handle-path fh))
  (unmarshal [path] (open-file path ReadOnly)))
```
```sweet-exp
defclass ResourceSerializable [a]
  marshal   [x : a] : value
  unmarshal [v : value] : a

;; Example: serialize a file handle by its path, reopen on resume
definstance ResourceSerializable FileHandle
  marshal [fh] file-handle-path(fh)
  unmarshal [path] open-file(path ReadOnly)
```

## Surface API

### Core Functions

```turmeric no-check
;; Delimit a serializable region
(serial-reset body)

;; Capture the continuation
(serial-shift [k] body)  ; k : serial-continuation<T>

;; Serialize a continuation to bytes
(serial-cont->bytes k) : bytes

;; Deserialize bytes to a continuation
(bytes->serial-cont b) : (Result (serial-continuation<T>) cstr)

;; Resume a continuation with a value
(serial-resume k v) : T
```
```sweet-exp
;; Delimit a serializable region
serial-reset body

;; Capture the continuation
serial-shift [k] body  ; k : serial-continuation<T>

;; Serialize a continuation to bytes
serial-cont->bytes(k) : bytes

;; Deserialize bytes to a continuation
bytes->serial-cont(b) : (Result (serial-continuation<T>) cstr)

;; Resume a continuation with a value
serial-resume(k v) : T
```

### The `serial-continuation<T>` Type

```turmeric
(defstruct serial-continuation<T>
  [resume    : (-> T serial-continuation<T>)
   to-bytes  : (-> bytes)
   schema-id : cstr])  ; Stable hash of frame chain shape
```
```sweet-exp
defstruct serial-continuation<T>
  [resume    : (-> T serial-continuation<T>)
   to-bytes  : (-> bytes)
   schema-id : cstr]  ; Stable hash of frame chain shape
```

> **`schema-id` should be an owned `String`.** The comment calls it a *stable
> hash of the frame chain shape* -- a **computed** value, stored in the struct and
> serialized alongside the continuation. A `cstr` field here borrows a pointer
> that dangles once the buffer that produced the hash is freed; only a hash that
> is always a static literal could stay `cstr`. Since it is computed, use
> `schema-id : String` so the continuation owns its own copy. See
> [strings-guide.md](strings-guide.md).

## Examples

### Persistent Workflow

A multi-step business process that survives crashes:

```turmeric
(defn process-order [order-id : int64] : unit
  (serial-reset
    ;; Step 1: Validate
    (def order (load-order order-id))
    (unless (valid-order? order)
      (throw (validation-error "Invalid order")))
    
    ;; Checkpoint after validation
    (serial-shift [k]
      (save-checkpoint "order-validated" k order)
      (continue k order))
    
    ;; Step 2: Charge payment
    (def charge-result (charge-payment order.payment-info))
    
    (serial-shift [k]
      (save-checkpoint "payment-charged" k charge-result)
      (continue k charge-result))
    
    ;; Step 3: Fulfill
    (fulfill order charge-result)))

;; Resume from last checkpoint
(defn resume-order [order-id : int64] : unit
  (let? [checkpoint (load-latest-checkpoint order-id)
         k (bytes->serial-cont checkpoint.bytes)]
    (serial-resume k checkpoint.value)))
```
```sweet-exp
defn process-order [order-id : int64] : unit
  serial-reset
    ;; Step 1: Validate
    def order load-order(order-id)
    unless valid-order?(order)
      throw(validation-error("Invalid order"))

    ;; Checkpoint after validation
    serial-shift [k]
      save-checkpoint("order-validated" k order)
      continue(k order)

    ;; Step 2: Charge payment
    def charge-result charge-payment(order.payment-info)

    serial-shift [k]
      save-checkpoint("payment-charged" k charge-result)
      continue(k charge-result)

    ;; Step 3: Fulfill
    fulfill(order charge-result)

;; Resume from last checkpoint
defn resume-order [order-id : int64] : unit
  let? [checkpoint load-latest-checkpoint(order-id)
        k bytes->serial-cont(checkpoint.bytes)]
    serial-resume(k checkpoint.value)
```

### Distributed Task Migration

Send a half-finished computation to another node:

```turmeric
;; Node A: Start computation
(def result
  (serial-reset
    (def task1 (run-task1))
    (def task2 (run-task2 task1))
    (serial-shift [k]
      ;; Serialize and send to Node B
      (def bytes (serial-cont->bytes k))
      (send-to-node-b bytes task2)
      (continue k (recv-from-node-b)))))

;; Node B: Resume computation
(defn handle-migration [bytes : bytes, input : any] : bytes
  (let? [k (bytes->serial-cont bytes)]
    (def result (serial-resume k input))
    (serial-cont->bytes (serial-shift [k'] (continue k' result)))))
```
```sweet-exp
;; Node A: Start computation
def result
  serial-reset
    def task1 run-task1()
    def task2 run-task2(task1)
    serial-shift [k]
      ;; Serialize and send to Node B
      def bytes serial-cont->bytes(k)
      send-to-node-b(bytes task2)
      continue(k recv-from-node-b())

;; Node B: Resume computation
defn handle-migration [bytes : bytes, input : any] : bytes
  let? [k bytes->serial-cont(bytes)]
    def result serial-resume(k input)
    serial-cont->bytes $ serial-shift [k'] (continue k' result)
```

### Web Continuations (Racket-style)

Serialize "what to do when form is submitted" as a URL token:

```turmeric
;; Generate a form page with continuation token
(defn get-checkout [req : HttpRequest] : HttpResponse
  (serial-reset
    (def cart (get-cart req.session))
    (serial-shift [k]
      ;; Save continuation, return URL with token
      (def token (save-continuation k))
      (render-page
        (form :action (str "/checkout-submit?token=" token))
          (label "Credit Card") (input :type "text" :name "cc")
          (submit)))))

;; Handle form submission
(defn post-checkout-submit [req : HttpRequest] : HttpResponse
  (let? [token (parse-token req.query.token)
         k (load-continuation token)]
    (def cc-number (parse-form-data req))
    (serial-resume k cc-number))
  (render-page (h1 "Thank you for your order!")))
```
```sweet-exp
;; Generate a form page with continuation token
defn get-checkout [req : HttpRequest] : HttpResponse
  serial-reset
    def cart get-cart(req.session)
    serial-shift [k]
      ;; Save continuation, return URL with token
      def token save-continuation(k)
      render-page
        (form :action str("/checkout-submit?token=" token))
        label("Credit Card")
        input(:type "text" :name "cc")
        submit()

;; Handle form submission
defn post-checkout-submit [req : HttpRequest] : HttpResponse
  let? [token parse-token(req.query.token)
        k load-continuation(token)]
    def cc-number parse-form-data(req)
    serial-resume(k cc-number)
  render-page(h1("Thank you for your order!"))
```

### Checkpointing Long-Running Computation

Periodic snapshots for crash recovery:

```turmeric
(defn analyze-dataset [data : (Vec Record)] : Report
  (defn checkpoint-every [n : int64, items : (Vec Record)] : Report
    (let [processed (Vec.new)]
      (for-each-with-index items
        (fn [i item]
          (Vec.push processed (process item))
          (when (= (mod (+ i 1) n) 0)
            (serial-shift [k]
              (save-checkpoint (str "checkpoint-" i) k)
              (continue k)))))
      (compute-report processed)))
  
  (checkpoint-every 1000 data))

;; On restart: find latest checkpoint and resume
(defn recover-analysis [] : Report
  (def latest (find-latest-checkpoint))
  (let? [k (bytes->serial-cont latest.bytes)]
    (serial-resume k)))
```
```sweet-exp
defn analyze-dataset [data : (Vec Record)] : Report
  defn checkpoint-every [n : int64, items : (Vec Record)] : Report
    let [processed Vec.new()]
      for-each-with-index items
        fn [i item]
          Vec.push(processed process(item))
          when {mod({i + 1} n) = 0}
            serial-shift [k]
              save-checkpoint(str("checkpoint-" i) k)
              continue(k)
      compute-report(processed)

  checkpoint-every(1000 data)

;; On restart: find latest checkpoint and resume
defn recover-analysis [] : Report
  def latest find-latest-checkpoint()
  let? [k bytes->serial-cont(latest.bytes)]
    serial-resume(k)
```

## Error Handling

### Schema Versioning

Continuation frames carry a schema version. If the code changes between serialization and deserialization, an error is returned:

```turmeric
(try-with
  (fn []
    (def k (bytes->serial-cont bytes))
    (serial-resume k value))
  (fn [e k]
    (match e
      (SchemaMismatch old new) ->
        (error "Cannot resume: checkpoint uses schema " old 
               "but current code uses schema " new)
      _ -> (raise e))))
```
```sweet-exp
try-with
  fn []
    def k bytes->serial-cont(bytes)
    serial-resume(k value)
  fn [e k]
    match e
      (SchemaMismatch old new)
      ->
      error("Cannot resume: checkpoint uses schema " old
            "but current code uses schema " new)
      _
      ->
      raise(e)
```

### Handling Unserializable Types

If you try to capture an unserializable value, the elaborator produces an error:

```text
error: binding `handle : file-handle` captured inside `serial-reset`
       but `file-handle` does not implement `Serializable`
  --> src/main.tur:42:5
   |
42 |   (serial-shift [k] (save-cont! k) 0)
   |                 ^ `handle` captured here
   = help: use resource marshalling or move outside serial-reset boundary
```

To fix: either implement `Serializable`/`ResourceSerializable` for the type, or restructure your code to avoid capturing it.

### Circular References

Serialization performs a **deep copy** of all captured state. Circular reference structures are detected and produce an error:

```turmeric
(def circular : (Option (Box circular)))
(set-box! circular (Some (Box.new circular)))

;; This will fail with a circular reference error
(serial-reset
  (serial-shift [k] (save-cont k) 0))
```
```sweet-exp
def circular : (Option (Box circular))
set-box!(circular Some(Box.new(circular)))

;; This will fail with a circular reference error
serial-reset
  serial-shift [k]
    save-cont(k)
    0
```

## Interaction with Ownership

### Deep Copy Semantics

Serialization always performs a **deep clone**. Reference-counted values are fully copied into the wire encoding, not shared. On resume, fresh heap allocations are created.

```turmeric
(def r (ref 42))
(serial-reset
  (serial-shift [k]
    ;; Serialization deep-copies r
    (def bytes (serial-cont->bytes k))
    (continue k)))

;; After deserialization, the resumed continuation has a NEW ref
;; with the same value (42), but it's a different cell in memory
```
```sweet-exp
def r ref(42)
serial-reset
  serial-shift [k]
    ;; Serialization deep-copies r
    def bytes serial-cont->bytes(k)
    continue(k)

;; After deserialization, the resumed continuation has a NEW ref
;; with the same value (42), but it's a different cell in memory
```

### `ref<T>` and `rc<T>` Behavior

- `ref<T>` may be captured only if `T: Serializable`
- On serialization, the **current value** is snapshotted
- On resume, a **fresh** `ref` cell is created with that value
- Sharing between multiple captures of the same `ref` is **not** preserved -- each resumed continuation gets its own independent copy

## Standard Library Support

### `stdlib/serial.tur`

```turmeric
;; File I/O helpers
(defn cont-to-file [k : serial-continuation<T>, path : cstr] : (Result unit cstr)
  (write-file path (serial-cont->bytes k)))

(defn cont-from-file [path : cstr] : (Result (serial-continuation<T>) cstr)
  (bytes->serial-cont (read-file path)))
```
```sweet-exp
;; File I/O helpers
defn cont-to-file [k : serial-continuation<T>, path : cstr] : (Result unit cstr)
  write-file(path serial-cont->bytes(k))

defn cont-from-file [path : cstr] : (Result (serial-continuation<T>) cstr)
  bytes->serial-cont(read-file(path))
```

### `stdlib/workflow.tur`

**Stable API surface (Phase 21).** Downstream code -- including the
application-image-dumps plan (`docs/archive/history/application-image-dumps-plan.md`,
AI2) -- builds on exactly these four entry points; treat their signatures as
the SemVer contract:

| Function | Signature | Role |
|---|---|---|
| `save-cont!` | `[k : ptr<void>] : ptr<void>` | marshal a captured continuation `k` to a `bytes` buffer (caller-owned) |
| `resume-cont!` | `[b : ptr<void> v : int] : int` | rebuild the chain from `b` and resume it on `v` |
| `workflow-suspend` | `[k : ptr<void>] : ptr<void>` | alias of `save-cont!` |
| `workflow-resume` | `[b : ptr<void> v : int] : int` | alias of `resume-cont!` |

`k` is the opaque handle the `serial-shift` receiver is passed (a DK chain
carried as `ptr<void>`). These are thin shims over the
`tur_serial_cont_serialize`/`_deserialize`/`_resume` runtime, which is emitted
whenever a program contains serial syntax. **Capture scope:** the continuation
must be a *supported delimited context* (a single-scalar-hole chain of `let`
prelude + `+ - * /` + 2-arg calls + one `if`); contexts outside that grammar do
not capture (see
`docs/archive/history/serial-shift-unsupported-context-miscompile.md`).

A higher-level API for persistent workflows:

```turmeric
;; Define a workflow step that can be suspended and resumed
(defworkflow-step process-approval [order-id : int64] : bool
  (def approved?
    (serial-shift [k]
      (db-save-continuation order-id k)
      false))
  (when approved?
    (fulfill-order! order-id))
  approved?)

;; Resume a workflow from the database
(defn resume-approval [order-id : int64, approved? : bool] : unit
  (let? [k (db-load-continuation order-id)]
    (serial-resume k approved?)))
```
```sweet-exp
;; Define a workflow step that can be suspended and resumed
defworkflow-step process-approval [order-id : int64] : bool
  def approved?
    serial-shift [k]
      db-save-continuation(order-id k)
      false
  when approved?
    fulfill-order!(order-id)
  approved?

;; Resume a workflow from the database
defn resume-approval [order-id : int64, approved? : bool] : unit
  let? [k db-load-continuation(order-id)]
    serial-resume(k approved?)
```

## Best Practices

### Minimize Captured State

Only capture what you need. Use identifiers instead of entire objects:

```turmeric
;; Good: capture only the ID
(serial-reset
  (def order-id 12345)
  (serial-shift [k]
    (save-cont k)
    (process-order order-id)))

;; Less good: capture the entire order object
(serial-reset
  (def order (load-order 12345))  ; Large object
  (serial-shift [k]
    (save-cont k)
    (continue k)))
```
```sweet-exp
;; Good: capture only the ID
serial-reset
  def order-id 12345
  serial-shift [k]
    save-cont(k)
    process-order(order-id)

;; Less good: capture the entire order object
serial-reset
  def order load-order(12345)  ; Large object
  serial-shift [k]
    save-cont(k)
    continue(k)
```

### Limit Continuation Depth

Deep call stacks increase serialization time and size. Design workflows with shallow stacks where possible.

### Use Schema Versioning for Long-Term Storage

If storing continuations long-term, consider implementing custom schema evolution logic:

```turmeric
;; Wrap continuation with version info
(defn save-versioned [k : serial-continuation<T>, version : int64] : unit
  (def bytes (serial-cont->bytes k))
  (save-to-disk (struct [version version, data bytes])))

;; On load, verify version compatibility
(defn load-versioned [path : cstr] : (Result (serial-continuation<T>) cstr)
  (def stored (load-from-disk path))
  (when (= stored.version CURRENT_VERSION)
    (bytes->serial-cont stored.data)))
```
```sweet-exp
;; Wrap continuation with version info
defn save-versioned [k : serial-continuation<T>, version : int64] : unit
  def bytes serial-cont->bytes(k)
  save-to-disk $ struct [version version, data bytes]

;; On load, verify version compatibility
defn load-versioned [path : cstr] : (Result (serial-continuation<T>) cstr)
  def stored load-from-disk(path)
  when {stored.version = CURRENT_VERSION}
    bytes->serial-cont(stored.data)
```

### Security Considerations

Deserializing from an untrusted source is analogous to Java deserialization vulnerabilities. Consider:

- Validating continuation bytes before deserialization
- Using a capability token or allowlist of permitted symbol keys
- Running deserialized continuations in a sandboxed environment

## Comparison to Alternatives

| Approach | Pros | Cons |
|----------|------|------|
| **Serializable Continuations** | Automatic, composable, captures exact state | Requires Serializable typeclass |
| **Hand-Written State Machines** | Full control, no overhead | Tedious, error-prone, manual updates |
| **Green-Thread Snapshots** | Captures full thread state | Not portable, captures OS resources, complex |
| **Persistent Processes** | Live state, hot reloading | Requires VM support, not suitable for C target |
| **Application Image Dumps** | Warm-start (skip init) like Lisp/pdumper; build-stamp safe | Cross-binary pinned; only serializable continuations cross. See [image-dumps-guide.md](image-dumps-guide.md) |

The `save-cont!` / `resume-cont!` surface above is the SemVer-pinned API that
`stdlib/image.tur` (application image dumps) builds on (plan AI0.4).

## Residual Liveness Imprecision (1.0 limitation)

The same conservative capture check that applies to cloneable continuations
(`TUR-E0014`) applies here: bindings in scope at a `serial-shift` site inside
the same function are checked against `Serializable`, even if they are not
actually referenced in the continuation body.  Bindings from enclosing outer
functions are excluded (CF7.3), but same-function bindings that happen to be
in lexical scope may be flagged even if they are dead at the shift point.

Full precision requires the post-1.0 CPS liveness pass (tracked in
[control-flow-completeness-plan.md](https://github.com/rjungemann/turmeric/blob/main/docs/archive/history/control-flow-completeness-plan.md) CF7.5).

**Workaround:** consume or drop non-Serializable values before the shift point,
or restructure so only Serializable bindings remain in scope.

## See Also

- [Checkpointing Guide](checkpointing-guide.md) -- More examples of persistent workflows
- [Async/Await Guide](async-await-guide.md) -- One-shot continuations for async I/O
- [Logic Programming Guide](logic-programming-guide.md) -- Cloneable continuations for backtracking
- [Effects System Guide](effects-system-guide.md) -- Algebraic effects and custom control flow
