# Turmeric — Serializable Continuations Plan

> **Status:** Draft Plan
> **Last Updated:** 2026-05-11
> **Phase:** Post-Phase 19 (v2 stretch goal)
> **Cross-references:**
> - [turmeric-plan.md](turmeric-plan.md) §18 (Delimited continuations), §19 (Algebraic effects)
> - [backtracking-cloneable-continuations-plan.md](backtracking-cloneable-continuations-plan.md) — Multi-shot/cloneable continuations
> - [effects-plan.md](effects-plan.md) — Algebraic effects design

---

## Executive Summary

This document outlines the design and implementation of **serializable continuations** for Turmeric, modeled on Racket's `call/cc`-based serialization via `racket/serialize` and the `continuation-mark` infrastructure.

The core idea: a suspended computation (continuation) can be **marshalled to bytes**, written to disk or sent over a network, then **unmarshalled and resumed** in a fresh process. This enables persistent workflows, migrable tasks, and checkpointed long-running computations without bespoke hand-written state machines.

**Key insight:** Phase 18's delimited continuations (`shift`/`reset`) reify the call stack as a heap-allocated closure chain. Each frame is already a struct on the heap. Serialization is a matter of traversing that chain, emitting a stable encoding of each frame, and reconstructing it on load.

**Core challenges:**

| Challenge | Mitigation |
|-----------|------------|
| Function pointers are not portable across builds | Stable symbol table: each continuation frame stores a stable string key, resolved at resume time |
| Captured values may contain unserializable resources (file handles, raw pointers) | `Serializable` typeclass gates what may be captured; resource types opt in with custom marshal/unmarshal hooks |
| Ownership model (`ref<T>`, `rc<T>`) assumes single-process lifetime | Serialized continuations produce a **deep copy**; ownership is transferred, originals invalidated |
| ABI stability across recompilations | Frames carry a schema version; mismatches produce a descriptive error rather than UB |

**Sequencing:** Depends on Phase 18 (delimited continuations) and Phase 19 (algebraic effects). Cloneable continuations ([backtracking-cloneable-continuations-plan.md](backtracking-cloneable-continuations-plan.md)) are a recommended co-prerequisite since both require a `Clone`-like typeclass infrastructure. Target: **Phase 21+**.

---

## 1. Motivation

### 1.1 Use Cases

| Use Case | Description |
|----------|-------------|
| **Persistent workflows** | Pause a multi-step business process, serialize it to a database row, resume it days later |
| **Distributed task migration** | Send a half-finished computation to another node for load balancing |
| **Checkpointing** | Save computation state periodically; restart from last checkpoint on crash |
| **Web session continuations** | Racket's web server style: serialize the "what to do when the user clicks Submit" continuation as a URL token |
| **Mobile agents** | Send code + state to a remote peer to be executed there |
| **Debugger snapshots** | Freeze a running program's continuation for offline inspection or replay |

### 1.2 Racket Precedent

Racket's `racket/serialize` library, combined with `call-with-current-continuation` and `continuation-mark-set`, provides the closest prior art:

```racket
(require racket/serialize)

;; Mark a value as serializable
(serializable-struct point (x y))

;; Capture and serialize a continuation
(define saved #f)
(define result
  (+ 1 (call/cc (lambda (k)
                  (set! saved k)
                  10))))

;; Write the continuation to a file
(with-output-to-file "cont.dat"
  (lambda () (write (serialize saved))))

;; Later, in another process:
(define k2 (deserialize (read)))
(k2 42)  ; resumes: result = 43
```

Racket achieves this by:
1. Heap-allocating all stack frames (similar to Turmeric's S2 strategy).
2. Tagging every value with a serialization descriptor (`prop:serializable`).
3. Storing stable symbolic names for closures (module path + index).
4. Providing a visitor-based marshal/unmarshal protocol.

Turmeric's approach follows the same outline, adapted to a compiled C-emitting language with an ownership model.

---

## 2. Design Overview

### 2.1 The `Serializable` Typeclass

```turmeric
;; A type that can be round-tripped through bytes
(defclass Serializable [a]
  (serialize   [x : a]    : bytes)
  (deserialize [b : bytes] : (Result a cstr)))

;; Primitive instances — automatically derived
(definstance Serializable int64  ...)
(definstance Serializable float64 ...)
(definstance Serializable bool   ...)
(definstance Serializable cstr   ...)
(definstance Serializable bytes  ...)

;; Container instances — structural, require element instance
(definstance (Serializable (Vec a))   [Serializable a] ...)
(definstance (Serializable (Pair a b)) [Serializable a, Serializable b] ...)
(definstance (Serializable (Option a)) [Serializable a] ...)
```

Types that do **not** implement `Serializable` (e.g., raw pointer types, file handles, `Mutex<T>`) cannot be captured in a serializable continuation. The elaborator enforces this at the `serial-reset` boundary.

### 2.2 Surface Syntax

```turmeric
;; serial-reset: like reset, but marks the delimited region as serializable
;; All captured values must implement Serializable
(serial-reset
  (let [x 42]
    (serial-shift k
      ;; k : serial-continuation<int64>
      (save-cont! k)
      0)))

;; Serialize a continuation to bytes
(serial-cont->bytes k)   ; : bytes

;; Deserialize bytes back to a continuation
(bytes->serial-cont b)   ; : (Result (serial-continuation<int64>) cstr)

;; Resume a deserialized continuation
(serial-resume k v)      ; : T
```

### 2.3 `serial-continuation<T>` Type

```turmeric
;; A continuation that knows how to serialize itself
(defalias serial-continuation<T>
  (struct
    [resume    : (-> T (serial-continuation<T>))
     to-bytes  : (-> bytes)
     schema-id : cstr]))   ; stable hash of the frame chain's shape
```

Unlike a plain `continuation<T>`, a `serial-continuation<T>` carries both a live resume function and a serialization thunk. They are always kept in sync: serializing then deserializing a `serial-continuation<T>` produces a value with identical behavior.

---

## 3. Runtime Representation

### 3.1 Frame Encoding

Each CPS frame on the heap is annotated with:

```c
typedef struct SerialFrame {
    const char  *symbol_key;   // stable, human-readable: "module::fn_name::frame_N"
    uint32_t     schema_ver;   // hash of field types, for compatibility checks
    size_t       n_fields;
    SerialField *fields;       // name + type_tag + value for each captured binding
    struct SerialFrame *parent;
} SerialFrame;
```

`symbol_key` is assigned at compile time and emitted into the object file's read-only data section. It survives across separate compilations as long as the source function is not renamed or its CPS frame shape not changed.

### 3.2 Serialization Wire Format

The wire format is a self-describing binary encoding:

```
[magic: 4 bytes "TSER"]
[format-version: uint16]
[frame-count: uint32]
  [frame-0]
    [symbol-key: length-prefixed utf8]
    [schema-ver: uint32]
    [field-count: uint32]
      [field-0]
        [name: length-prefixed utf8]
        [type-tag: uint8]
        [payload: type-specific bytes]
      ...
  [frame-1]
  ...
[checksum: crc32 of preceding bytes]
```

An alternative JSON envelope (for debugging / cross-language interop) is provided via a compile-time flag.

### 3.3 Symbol Resolution at Resume Time

On deserialization, each `symbol_key` is looked up in a **global continuation registry** populated at program startup:

```c
// Generated by the compiler for each CPS-transformed function:
static void __register_serial_frames(void) {
    serial_register("mymodule::process_order::frame_2",
                    sizeof(Frame_process_order_2),
                    reconstruct_process_order_2);
}
```

If a key is not found (e.g., the function was renamed in a newer build), deserialization returns `Err("unknown frame: mymodule::process_order::frame_2")`.

---

## 4. Type System & Safety

### 4.1 Elaborator Enforcement

Inside a `serial-reset` block, the elaborator tracks the **serializable environment**: the set of bindings in scope that may be captured by `serial-shift`. Each binding must have a type that satisfies `Serializable`. If a non-serializable value is referenced inside the delimited region, the elaborator emits a type error:

```
error: binding `handle : file-handle` captured inside `serial-reset`
       but `file-handle` does not implement `Serializable`
  --> src/main.tur:42:5
   |
42 |   (serial-shift k (save-cont! k) 0)
   |                 ^ `handle` captured here
   = help: use `with-resource` outside the `serial-reset` boundary,
           or implement `Serializable` for `file-handle` with a
           custom marshal hook (e.g., re-open by path on resume)
```

### 4.2 Schema Versioning

The `schema-ver` field is a CRC32 of the ordered list of `(name, type-tag)` pairs for a frame. On resume, if the stored schema-ver does not match the compiled schema-ver, an error is returned rather than silently misreading fields. This makes it safe to evolve a program's frame layout across versions as long as old serialized continuations are not expected to be resumed after a breaking change.

### 4.3 Interaction with `rc<T>` and `ref<T>`

Serialization always performs a **deep clone**: reference-counted values are fully copied into the wire encoding, not shared. On resume, fresh heap allocations are created. Circular reference structures are detected and produce a serialization error (rather than looping forever).

`ref<T>` (mutable cells) may be captured only if `T: Serializable`. On serialization the *current value* is snapshotted; on resume a fresh `ref` cell is created with that value. Sharing between multiple captures of the same `ref` across a frame boundary is **not** preserved — each resumed continuation gets its own independent copy.

---

## 5. Standard Library Support

### 5.1 `stdlib/serial.tur`

```turmeric
(defn cont-to-file [k : serial-continuation<T>, path : cstr] : (Result unit cstr)
  (let [b (serial-cont->bytes k)]
    (write-file path b)))

(defn cont-from-file [path : cstr] : (Result (serial-continuation<T>) cstr)
  (let? [b (read-file path)]
    (bytes->serial-cont b)))
```

### 5.2 `stdlib/workflow.tur`

A higher-level API for persistent workflows inspired by Racket's web-server continuation style:

```turmeric
;; Define a workflow step that can be suspended and resumed
(defworkflow-step process-approval [order-id : int64] : bool
  (let [approved? (serial-shift k
                    (db-save-continuation! order-id k)
                    false)]
    (when approved?
      (fulfill-order! order-id))
    approved?))

;; Resume a workflow from the database
(defn resume-approval [order-id : int64, approved? : bool] : unit
  (let? [k (db-load-continuation order-id)]
    (serial-resume k approved?)))
```

---

## 6. Implementation Phases

### Phase A — Serializable Typeclass & Primitives

- [ ] Define `Serializable` typeclass in `src/typeclass.{c,h}` and `stdlib/serial.tur`.
- [ ] Implement derived instances for all primitive types (`int64`, `float64`, `bool`, `cstr`, `bytes`).
- [ ] Implement structural instances for `Vec<a>`, `Pair<a,b>`, `Option<a>`, `Result<a,b>`.
- [ ] Wire into the existing typeclass elaboration pipeline.

### Phase B — Frame Annotation & Symbol Registry

- [ ] Annotate each emitted CPS frame struct with `symbol_key` and `schema_ver` in `src/emit.{c,h}`.
- [ ] Generate `__register_serial_frames()` stubs at compile time.
- [ ] Implement `serial_register()` / `serial_lookup()` in `src/runtime.{c,h}`.
- [ ] Add startup registration call in `src/main.c` generated preamble.

### Phase C — Wire Format & Core Codec

- [ ] Implement `serial_write_frame()` / `serial_read_frame()` in `src/runtime.{c,h}`.
- [ ] Implement `bytes` type codec helpers.
- [ ] Implement CRC32 checksum for integrity checking.
- [ ] Implement circular-reference detection using a pointer → offset table during serialization.

### Phase D — Elaborator Integration

- [ ] Add `SERIAL_RESET` / `SERIAL_SHIFT` expression nodes in `src/expr.{c,h}`.
- [ ] Add serializable-environment tracking in `src/elab.{c,h}`: check all captured bindings satisfy `Serializable`.
- [ ] Emit helpful diagnostics for non-serializable captures.
- [ ] Propagate `schema_ver` computation from type information.

### Phase E — Surface Syntax & Standard Library

- [ ] Add `serial-reset` / `serial-shift` reader forms in `src/reader.{c,h}` and `src/forms.{c,h}`.
- [ ] Implement `serial-cont->bytes` / `bytes->serial-cont` / `serial-resume` builtins.
- [ ] Implement `stdlib/serial.tur` file I/O helpers.
- [ ] Implement `stdlib/workflow.tur` high-level workflow API.

### Phase F — Tests & Documentation

- [ ] Unit tests: round-trip primitive values, structs, `Vec`, `Option`.
- [ ] Integration test: serialize continuation mid-computation, kill process, re-launch, resume.
- [ ] Error-path tests: schema version mismatch, unknown symbol key, circular ref.
- [ ] Document wire format in `docs/serializable-continuations-wire-format.md`.

---

## 7. Open Questions

| # | Question | Notes |
|---|----------|-------|
| 1 | **Stable symbol keys across refactors** | If a function is renamed, old serialized continuations break. Accept this, or support a `#[stable-key "old_name"]` annotation? |
| 2 | **Cross-platform portability** | Endianness: wire format is little-endian. Float encoding: IEEE 754 (no action needed). `int64` sizes: fixed-width types only in the wire format. |
| 3 | **Interaction with algebraic effects** | Can a continuation captured inside a `handle` block cross the handler boundary? Initial answer: no — `serial-reset` may not enclose an active effect handler. Enforce in elaborator. |
| 4 | **Security of deserialized continuations** | Deserializing from an untrusted source is analogous to Java deserialization vulnerabilities. Should `bytes->serial-cont` require a capability token or allowlist of permitted symbol keys? |
| 5 | **Schema evolution helpers** | Provide migration functions: given an old frame encoding and a new schema, transform field-by-field. Similar to Protobuf field-numbering discipline. |
| 6 | **JSON wire format** | Optional debug/interop mode. Gated behind a build flag or a separate `serial-cont->json` builtin? |

---

## 8. Alternatives Considered

### 8.1 Hand-Written State Machines

The traditional alternative: the programmer converts their workflow into an explicit state machine with a `state` enum and a `resume(state, input)` function. This works but is tedious, error-prone, and requires a rewrite whenever the control flow changes. Serializable continuations automate the transformation.

### 8.2 Green-Thread Snapshots (CRIU-style)

Capture the entire OS-level thread state (registers, stack, heap). Too low-level: not portable across architectures, not compatible with garbage-collected heaps, captures unserializable OS resources. Racket's approach (and Turmeric's) of serializing only the **heap-allocated frame chain** avoids all these issues.

### 8.3 Persistent Processes (Erlang/Elixir)

Erlang avoids serializing continuations by making all state live in immutable messages between processes, with hot code reloading handled by the VM. Turmeric's ownership model and C-emission target make this approach impractical.

---

*This plan is a draft. Details in §3 (wire format) and §4.3 (rc/ref semantics) are most likely to change as implementation proceeds.*
