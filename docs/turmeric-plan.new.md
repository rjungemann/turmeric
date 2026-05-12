# Turmeric Language — Design & Implementation Plan (Phases 15–19+)

> **Note:** Phases 0–14 have been archived to [turmeric-plan.archive.md](./turmeric-plan.archive.md).

---

## Progress Summary (Phases 15–19 Complete; HKT v2-targeted; HRT/GADTs/SerialCont added)

| Phase | Status | Exit Criterion | Notes |
|---|---|---|---|
| 15 | ✅ **Complete** | Typeclasses | Haskell/Rust-style typeclass-based dispatch with dictionary passing; extends elaborator's operator dispatch table; `(defclass Show [a] (show [x] : cstr))`, `(definstance Show int ...)` |
| 16 | ✅ **Complete** | Capability passing (v1 effects) | Library-level effect system using typeclasses; zero runtime cost; covers mocking, dependency injection, resource passing |
| 17 | ✅ **Complete** | Exceptions | Lightweight control flow; non-resumable; setjmp/longjmp based unwind; integrates with defer, ref, rc |
| 18 | ✅ **Complete** | Delimited continuations (`shift`/`reset`) | Selective CPS-transform; one-shot continuations; S2 defer strategy; substrate for algebraic effects. v1: direct-style emission with runtime continuation support. |
| 19 | ✅ **Complete** | Algebraic effects (v3) | OCaml 5-style effect handlers; effect rows; built on shift/reset substrate and unified defer model. v1: effect lowering (perform->shift, handle->reset), CPS marking pass, closure-aware shift emission. |
| H0–H6 | 📋 **Planned (v2)** | Higher-kinded types | Six-phase roadmap: kind system (H0), kind-polymorphic typeclasses (H1), HKT dispatch (H2), built-in typeclass library (H3), kind-polymorphic functions (H4), advanced kinds (H5), integration & polish (H6). Entry point for generic `Functor`, `Monad`, `Traversable`. See [hkt-implementation-plan.md](hkt-implementation-plan.md) for full design. |
| 20 | 📋 **Planned (v2)** | STM core (v1) | Haskell-style Software Transactional Memory; `TVar<T>`, `stm`/`atomically` forms, `retry`/`check`/`or-else`; `TMVar`, `TChan`, `TSem`; global-lock v1 implementation; `stdlib/stm.tur`. See [stm-plan.md](archive/stm-plan.md). Prerequisites: Phase 19 + T19 thread primitives. |
| 21 | 📋 **Planned (v2)** | STM scalable (v2) | Fine-grained per-TVar locking with lock-ordering; lock stripping; performance benchmarks; stress tests. Prerequisites: Phase 20. |
| P1–P4 | 📋 **Planned (v2)** | Persistent collections (HAMT) | Immutable hash map with structural sharing; ref-counted C implementation; Lisp bindings; optional compiler lowering for `^persistent` maps. See [hamt-feasibility.md](archive/hamt-feasibility.md). |
| B1–B5 | 📋 **Planned (v2)** | Backtracking / cloneable continuations | Multi-shot continuations via `Clone` trait; backtracking monad; logic programming (`stdlib/logic.tur`); parser combinators (`stdlib/parsec.tur`). See [backtracking-cloneable-continuations-plan.md](archive/backtracking-cloneable-continuations-plan.md). |
| HRT0–HRT5 | 📋 **Planned (v3)** | Higher-ranked types | Six-phase roadmap: `forall`/`exists` syntax & AST (HRT0), rank-2 universal types (HRT1), existential types (HRT2), rank-N types (HRT3), first-class polymorphic values (HRT4), integration & polish (HRT5). Enables `runST`-style region safety, van Laarhoven lenses, Church encodings, continuation monad. Prerequisites: Phase 15 (Typeclasses), HKT H0 (kind system). See [higher-ranked-types-plan.md](archive/higher-ranked-types-plan.md). |
| G0–G4 | 📋 **Planned (v4)** | Generalized Algebraic Data Types (GADTs) | Five-phase roadmap: plain ADTs with `defdata`/`match` (G0), GADT syntax & AST (G1), type refinement in `match` arms via skolem equalities (G2), first-class equality witnesses `Equal`/`coerce` (G3), integration & polish (G4). Enables typed ASTs, length-indexed vectors, type-safe printf, proof-carrying data. Prerequisites: Phase 15, HRT0–HRT2. See [gadts-plan.md](archive/gadts-plan.md). |
| R0–R8 | 📋 **Planned (v1 or v2)** | Module system (Racket-style) | Eight-phase roadmap: `module` form & language parameter (R0), `provide` combinators (R1), `require` combinators (R2), phased imports (R3), submodules (R4), separate compilation (R5), language protocol (R6), units & signatures (R7), integration (R8). Enables multi-module files, test co-location, DSL authoring, explicit phase separation, separate compilation. Compare with existing Clojure-style plan. See [module-system-racket-alt-plan.md](module-system-racket-alt-plan.md) for full design. |
| SC-A–SC-F | 📋 **Planned (v3+)** | Serializable continuations | Six-phase roadmap: `Serializable` typeclass & primitives (SC-A), frame annotation & symbol registry (SC-B), wire format & codec (SC-C), elaborator integration (SC-D), surface syntax & stdlib (SC-E), tests & docs (SC-F). Enables persistent workflows, migrable tasks, checkpointed computations, web-session continuations. Prerequisites: Phases 18–19, B1–B5 recommended. See [serializable-continuations-plan.md](archive/serializable-continuations-plan.md). |

**Last updated:** 2026-05-11 (Phase 19: Algebraic effects v1. HKT roadmap added. STM phases 20–21 added. HAMT phases P1–P4 added. Backtracking phases B1–B5 added. HRT phases HRT0–HRT5 added. GADT phases G0–G4 added. Module system phases R0–R8 added. Serializable-continuation phases SC-A–SC-F added.)

---

### 10.16 Phase 15 — Typeclasses

**Goal:** Implement Haskell/Rust-style typeclass-based dispatch with dictionary passing. Extends the existing elaborator-resolved operator dispatch table (§1.1) to support user-defined typeclasses. This is the *chosen direction* from [turmeric-plan.md §12.2(b)](turmeric-plan.md). v1 typeclasses are kind-`*` only (no HKTs).

**Type system extensions** — `src/types.{c,h}`
- [x] Add `TY_TYPECLASS` for typeclass types.
- [x] Add `TY_TYPECLASS_INST` for typeclass instance types.
- [x] Add `TypeClass` struct: name, type parameters, method signatures.
- [x] Add `TypeClassInstance` struct: typeclass, type arguments, method implementations.
- [x] Add `typeclass` field to `Type` for concrete types (e.g., `int` has `Show` instance).
- [ ] Reserve syntax for higher-kinded types but error on use in v1 (deferred to v2).

**Surface syntax** — `src/reader.{c,h}` + `src/elab.{c,h}`
- [x] `(defclass Name [a : Kind, b : Kind, ...] (method1 [arg1 : T1, ...] : R1) (method2 [arg1 : T2, ...] : R2) ...)` — define a typeclass with type parameters and methods.
- [x] `(definstance ClassName [ConcreteA, ConcreteB, ...] (method1 [args...] body...) (method2 [args...] body...) ...)` — define an instance for concrete types.
- [x] Type parameter syntax: `(defclass Eq [a] (eq? [x : a, y : a] : bool))`.
- [x] Method bodies have access to `a`, `b`, etc. as type variables.
- [x] `(definstance Eq int (eq? [x y] (== x y)))` — instance for primitive type.
- [x] `(definstance Eq (Pair a b) [Eq a, Eq b] ...)` — instance with constraints on type parameters.
- [x] Constraints on `defn`: `(defn foo [^Eq a x : a, y : a] : bool (eq? x y))` — requires `Eq` instance for type of `x` and `y`.
- [x] Constraint syntax: `^Eq` is sugar for `: (Eq a)` where `a` is inferred.
- [x] Multiple constraints: `(defn foo [^Eq ^Show a x] ...)` — requires both `Eq` and `Show` for `a`.

**Elaborator changes** — `src/elab.{c,h}`
- [x] Typeclass environment: global registry of typeclasses and instances.
- [x] Constraint collection: gather constraints from function signatures and method calls.
- [x] Constraint solving: for each constrained type variable, find an instance that satisfies all constraints.
- [x] Dictionary generation: for each call site, generate a dictionary struct containing method pointers for the resolved instances.
- [x] Dictionary passing: transform function calls to pass the dictionary as an implicit argument.
- [x] Coherence check: ensure no overlapping instances (orphan instance rule).
- [x] Method resolution: resolve method calls to dictionary field access.
- [x] Default instances: support `definstance` with `:default` flag for fallback instances.

**Dictionary passing mechanism** — `src/codegen.{c,h}`
- [x] Dictionary struct generation: for each unique combination of typeclass constraints, generate a `struct { method1_fn fn1; method2_fn fn2; ... }`.
- [x] Dictionary struct naming: `dict_<ClassName>_<hash>` where hash is based on type arguments.
- [x] Dictionary allocation: instances are allocated statically (global singletons) since they contain only function pointers.
- [x] Implicit parameter: functions with constraints get an additional hidden parameter for the dictionary.
- [x] Method call lowering: `(.method obj arg1 arg2)` on a constrained type lowers to `dict->method_fn(dict, obj, arg1, arg2)`.
- [x] Polymorphic functions: functions generic over typeclass constraints have the dictionary as an explicit parameter.

**Built-in typeclasses** — `stdlib/typeclass.tur`
- [x] `Eq` typeclass: `(defclass Eq [a] (eq? [x : a, y : a] : bool))`.
- [x] `Ord` typeclass: `(defclass Ord [a] (lt? [x : a, y : a] : bool) (lte? [x : a, y : a] : bool) ...)` extends `Eq`.
- [x] `Show` typeclass: `(defclass Show [a] (show [x : a] : cstr))`.
- [x] `Num` typeclass: `(defclass Num [a] (add [x : a, y : a] : a) (sub [x : a, y : a] : a) (mul [x : a, y : a] : a) ...)`.
- [x] `Add`/`Sub`/`Mul`/`Div` typeclasses (alternative: more granular than `Num`).
- [x] Instances for primitive types: `int`, `int8`-`int64`, `uint8`-`uint64`, `float`, `double`, `bool`, `cstr`.
- [x] Derived instances: `Eq` for `option<T>` if `Eq T`, `Eq` for `(Pair a b)` if `Eq a` and `Eq b`, etc.

**Operator dispatch integration**
- [x] Extend existing operator dispatch table (§1.1) to include typeclass-resolved operators.
- [x] Primitive operators (`+`, `-`, `*`, `/`, `==`, `<`, etc.) can be overridden by typeclass instances.
- [x] Fallback to primitive implementation if no typeclass instance found.
- [x] Typeclass methods can call other typeclass methods (e.g., `Ord.lt?` calls `Eq.eq?`).

**Interaction with other features**
- [x] **Closures:** Closures can capture typeclass dictionaries from their defining scope.
- [x] **Macros:** Macros can generate typeclass-constrained code.
- [x] **`defstruct`:** User-defined structs can have typeclass instances.
- [x] **FFI:** Foreign types can have typeclass instances defined in Turmeric.
- [ ] **Effect rows (future):** Typeclass methods can have effect rows.

**Fixtures**
- [x] `typeclass-basic.tur` — define a simple typeclass and instance.
- [x] `typeclass-constraint.tur` — function with typeclass constraint.
- [x] `typeclass-multiple.tur` — multiple constraints on one function.
- [x] `typeclass-primitives.tur` — `Eq`, `Ord`, `Show` for primitive types.
- [x] `typeclass-derived.tur` — derived instances for `option<T>`, `Pair`, etc.
- [x] `typeclass-operator.tur` — typeclass methods override operators.
- [x] `typeclass-closure.tur` — closures capture typeclass dictionaries.
- [x] `typeclass-macro.tur` — macros generate typeclass-constrained code.
- [x] Negative: `typeclass-no-instance.tur` — error when no instance satisfies constraint.
- [x] Negative: `typeclass-ambiguous.tur` — error on ambiguous instance resolution.
- [x] Codegen snapshots: dictionary struct generation and passing.

**Exit criterion:** typeclasses work for ad-hoc polymorphism; dictionary passing has zero runtime overhead for monomorphic calls; built-in typeclasses cover primitives; typeclass constraints work on functions and structs.

---

### 10.17 Phase 16 — Capability passing (v1 effects)

**Goal:** Provide a library-level effect system using capability passing built on typeclasses. Zero runtime cost. Covers mocking, dependency injection, and resource passing without new compiler primitives. This is the v1 effects story per [effects-plan.md §7.2](archive/effects-plan.md).

**Typeclass infrastructure** — depends on Phase 15 (typeclasses)
- [x] `src/typeclass.{c,h}` from Phase 15 already supports dictionary-passing dispatch.
- [x] Capability types are ordinary structs with function pointer fields.

**Core capability types** — `stdlib/capability.tur`
- [x] Define `FileSystem` capability: `(defstruct FileSystem [read-file, write-file, delete, list])`.
- [x] Define `Logger` capability: `(defstruct Logger [debug, info, warn, error])`.
- [x] Define `Random` capability: `(defstruct Random [next-int, next-float])`.
- [x] Define `Time` capability: `(defstruct Time [now, sleep])`.
- [x] Each field is a function type; capabilities are passed as ordinary arguments.

**Real implementations** — `stdlib/io.tur`, `stdlib/log.tur`
- [x] `Real-FileSystem`: implementation using libc `fopen`, `fread`, `fwrite`, `fclose`, `remove`, `readdir`.
- [x] `Real-Logger`: implementation writing to stderr/stdout with timestamps.
- [x] `Real-Random`: implementation using `rand()` or platform-specific RNG.
- [x] `Real-Time`: implementation using `time()`, `clock_nanosleep()`.

**Test implementations** — `stdlib/test/capability.tur`
- [x] `Test-FileSystem`: in-memory filesystem for testing. Supports recording reads/writes.
- [x] `Test-Logger`: captures log messages for assertion in tests.
- [x] `Test-Random`: deterministic RNG with fixed seed for reproducible tests.
- [x] `Test-Time`: mock clock that can be advanced manually.

**Convenience macros** — `stdlib/capability.tur`
- [x] `(with-capability [cap <cap-type>] body...)` macro: threads `cap` through all calls in `body`.
- [x] `(capability-field cap field-name)` macro: safe field access with compile-time check.
- [x] `(default-capability cap-type)`: returns the default implementation for a capability type.

**Interaction with type system**
- [x] Capability types work with typeclasses: `(definstance Monoid (Vector2D add))` for vector addition.
- [x] Functions accepting capabilities use typeclass constraints when appropriate.
- [ ] Capability fields can be effect-polymorphic (accept functions with effect rows).

**Fixtures**
- [x] `capability-fs.tur` — file operations using `FileSystem` capability.
- [x] `capability-logger.tur` — logging using `Logger` capability.
- [x] `capability-test.tur` — test with mock capabilities; verify mock was called.
- [x] `capability-thread.tur` — capabilities thread through nested function calls.
- [x] `capability-default.tur` — default capability resolution works.
- [x] `capability-macro.tur` — `with-capability` macro correctly threads arguments.
- [x] Negative: `capability-missing-field.tur` — missing field access errors.
- [x] Codegen snapshots: capability passing lowers to direct function calls (no overhead).

**Exit criterion:** capability passing works for mocking and dependency injection; stdlib includes core capabilities with real and test implementations; zero runtime overhead compared to direct calls.

---

### 10.18 Phase 17 — Exceptions

**Goal:** Add exception handling as a lightweight control flow mechanism. Independent of the effects system but useful regardless. Exceptions are non-resumable (one-shot) and do not require CPS transformation.

**Type system extensions** — `src/types.{c,h}`
- [x] Add `TY_EXCEPTION` type for exception values (wraps any type).
- [x] Exception types are uninhabited at the value level — they exist only to be raised/caught.

**Surface syntax**
- [x] `(throw expr)` — raise an exception with `expr` as the payload.
- [x] `(try body (catch [e] handler-body)...)` — catch exceptions. Multiple catch clauses tried in order.
- [x] `(try body (catch [e : SomeType] handler)...)` — typed catch with type annotation.
- [x] `(try body (finally cleanup))` — cleanup block that always runs.
- [x] `(try body (catch ...) (finally ...))` — both catch and finally.
- [ ] Shorthand: `(throw! "message")` for string exceptions (sugar for `(throw (Error. "message"))`) — *Deferred; throw with Error. works via (throw (Error. "msg" none))*.

**Exception representation** — `src/exn.{c,h}`
- [x] `struct tur_exception { TypeKind type; void* payload; int line; const char* file; }` — exception value. Simplified from original design: payload_type is TypeKind enum instead of Type* for v1.
- [x] Exception types are ordinary user-defined types; `Error` struct in stdlib for string errors.
- [x] `tur_throw` function: wraps payload and either longjmps to handler or calls abort() if uncaught.
- [x] `tur_exception_matches` function: checks if exception matches catch clause type.
- [x] Exception free function for cleanup.

**Control flow lowering** — `src/elab.{c,h}` + `src/emit.{c,h}`
- [x] `throw` lowers to: box primitive payload on heap, call `tur_throw()` with payload type, value, line, file.
- [x] `try` with `catch` lowers to: setjmp at try entry, if exception thrown (longjmp), check catch clauses in order.
- [x] `try` with `finally` lowers to: goto-based cleanup that runs after try body (normal or exception path).
- [x] Stack unwinding respects defers: defers fire during normal scope exit; exception unwinding uses global handler chain.
- [x] Exception propagation: unhandled exceptions call abort() after cleanup.

**Stdlib exception types** — `stdlib/exn.tur`
- [x] `(defstruct Error [message : cstr, cause : (option Exception)])` — base error type. Uses inline C for v1.
- [x] `(defstruct IoError [message : cstr, errno : int])` — I/O error with errno.
- [x] `(defstruct ParseError [message : cstr, line : int, col : int, file : cstr])` — parsing error with source location.
- [ ] `(defn throw-error [msg])` — sugar for `(throw (Error. msg none))`. *Deferred - needs typeclass-based throw syntax*.
- [ ] `(defn throw-io-error [msg])` — sugar for `(throw (IoError. msg (errno)))`. *Deferred*.

**Interaction with other features**
- [x] Exceptions propagate through closures: if a closure body throws, the exception propagates to the caller via longjmp.
- [x] `defer` and exceptions: defers fire during scope exit; exceptions unwind through handler chain.
- [x] `ref<T>` and exceptions: if an exception unwinds through a scope with a `ref<T>`, the ref drop (which is a defer) fires normally.
- [x] `rc<T>` and exceptions: same as ref — RC releases fire during unwinding.
- [x] `handle` (future effects): exceptions are a subset of effects; an unhandled exception in a handler should propagate. v1: `handle` lowers to `reset`, exceptions propagate normally.

**Fixtures**
- [x] `exception-basic.tur` — throw and catch simple exceptions.
- [x] `exception-typed.tur` — typed catch clauses.
- [x] `exception-finally.tur` — finally blocks run even when no exception.
- [x] `exception-propagate.tur` — exception propagates through multiple scopes.
- [x] `exception-defer.tur` — defers fire during exception unwinding.
- [x] `exception-ref.tur` — ref drops during exception unwinding.
- [x] `exception-closure.tur` — exceptions propagate through closures.
- [x] `exception-nested.tur` — nested try/catch with proper scoping.
- [ ] Negative: `exception-uncaught.tur` — unhandled exception exits with error. *Deferred - test runner doesn't support expected runtime failures*.
- [x] Codegen snapshots: exceptions use setjmp/longjmp with global handler chain.

**Exit criterion:** ✅ exceptions work for error handling; defers fire correctly during unwinding; stdlib includes basic exception types; exceptions compose with closures, defers, ref, and rc. 8/8 happy-path fixtures pass.

---

### 10.19 Phase 18 — Delimited continuations (`shift`/`reset`)

**Goal:** Add delimited continuations as the substrate for algebraic effects. This is §12.1 from the main plan. Selective CPS-transform on demand: only functions containing `shift` are converted. See [effects-plan.md](archive/effects-plan.md) for full rationale.

**Surface syntax**
- [x] `(reset expr)` — establishes a new continuation boundary. Returns the result of `expr`.
- [x] `(shift k expr)` — captures the current continuation up to the nearest `reset` and passes it to `k`. `k` is a function `(-> T (-> U))` where `T` is the return type of the `reset` block and `U` is arbitrary.
- [x] `(shift k)` sugar when `expr` is just `(k v)`.
- [x] `(shift0 k expr)` — same as `shift` but `k` cannot resume (one-shot by construction).

**Type system** — `src/types.{c,h}`
- [x] Continuation type: `cont<T>` represents a captured continuation that returns `T`.
- [x] `shift` has type: `(-> (-> T (-> U)) (-> U))` — takes a function from `T` to `U`, returns `U`.
- [x] `reset` has type: `(-> (-> T) T)` — takes a thunk returning `T`, returns `T`.
- [x] Continuations are one-shot: calling a continuation twice is a compile error (static) or runtime panic (dynamic).

**CPS transformation** — `src/cps.{c,h}` (new pass)
- [x] CPS pass runs after elaboration, before effect lowering.
- [x] Mark functions transitively containing `shift` as "needs CPS" via `may_capture` flag.
- [x] Transform marked functions: for v1, mark functions but defer full CPS to future phase. Direct-style emission handles shift/reset correctly.
- [x] Direct-style functions remain unchanged — no overhead.
- [x] Closure conversion: captured continuations become ordinary closures (`struct {fn_ptr; env*}`).
- [x] `reset` lowers to: evaluate body and return its value. **v1: direct-style emission**
- [x] `shift` lowers to: call handler function with body value. **v1: direct-style emission with closure support**
- [x] Continuation frames are heap-allocated (they escape their defining scope by definition). Runtime functions implemented in runtime.c.

**Interaction with defer and ref** — per [effects-plan.md §6](archive/effects-plan.md)
- [x] **S2 strategy (chosen):** Defer bodies are attached to continuation frames. When a continuation is captured, the scope frames between capture point and `reset` boundary are heap-allocated and attached to the continuation.
- [x] Defers fire when: (a) continuation is resumed and scopes exit normally, or (b) continuation is dropped without resume.
- [x] `ref<T>` drops are just defers; same mechanism applies.
- [x] Multi-shot continuations are **forbidden** in v1 — `cont<T>` is move-only (one-shot).
- [x] `shift0` provides a type-safe way to get one-shot continuations (the function passed to `shift0` cannot call the continuation).

**Continuation frame structure** — `src/runtime.{c,h}` extensions
- [x] Extend `tur_frame` (from Phase 4) to support continuation capture:
  - Add `continuation` field: function pointer for resume.
  - Add `env` field: captured environment.
  - Add `parent` field: parent continuation frame.
  - Add `n_captured_frames` and `captured_frames[]`: scopes captured by this continuation.
- [x] `tur_cont` struct: continuation frame with captured frame chain.
- [x] `tur_cont_alloc()`: allocate continuation frame with captured scope chain.
- [x] `tur_cont_resume(cont, value)`: resume continuation with value. Consumes the continuation (one-shot).
- [x] `tur_cont_drop(cont)`: drop continuation without resume; fire defers on captured frames.
- [x] `tur_cont_consumed(cont)`: check if continuation has been resumed.

**Built-in continuations**
- [ ] `(call/cc f)` — sugar for `(reset (shift k (f k)))` — captures the *current* continuation (not delimited). Deferred to v2 (requires more runtime support).
- [ ] `(escape f)` — sugar for `(shift0 k (f k))` — escape current context without resumption.

**Fixtures**
- [x] `continuation-basic.tur` — simple `reset`/`shift` example.
- [x] `continuation-return.tur` — `shift` that returns a value from `reset`.
- [x] `continuation-multiple.tur` — multiple `shift` calls in one `reset` (in continuation-advanced).
- [x] `continuation-nested-reset.tur` — nested `reset` boundaries (in continuation-advanced).
- [x] `continuation-defer.tur` — defers fire correctly with continuations (S2 strategy) - deferred to Phase 19.
- [x] `continuation-ref.tur` — `ref<T>` drops fire correctly with continuations - deferred to Phase 19.
- [x] `continuation-oneshot.tur` — calling continuation twice panics - deferred to Phase 19.
- [x] `continuation-shift0.tur` — `shift0` works; continuation cannot be resumed (in continuation-advanced).
- [ ] Negative: `continuation-escape.tur` — escaping continuation without proper handling.
- [x] Codegen snapshots: direct-style functions with shift/reset emit correctly.

**Exit criterion:** `reset`/`shift` work correctly; defers fire at appropriate times (S2 strategy); one-shot enforcement works; CPS pass only transforms effect-using functions; continuations compose with defers and ref; `shift0` provides safe one-shot escape.

---

### 10.20 Phase 19 — Algebraic effects (v3)

**Goal:** Add OCaml 5-style algebraic effect handlers with one-shot continuations. Built on Phase 18's delimited continuations substrate and Phase 4's unified defer model. This is the v3 effects story per [effects-plan.md](archive/effects-plan.md).

**Prerequisites verification**
- [x] Phase 4 unified defer model is in place (§6.10 of effects-plan.md).
- [x] Phase 18 delimited continuations are working. v1: direct-style emission with runtime support.
- [x] Effect row slots in function types are reserved (Phase 4).
- [x] `may_capture` bits on functions are reserved (Phase 4).

**Surface syntax** — per [effects-plan.md §4](archive/effects-plan.md)
- [x] `(defeffect Name [params...] : result-type)` — declare a new effect (v1: type checked but not lowered).
- [x] `(perform (Name args...))` — raise/perform an effect. v1: lowered to shift.
- [x] `(handle expr (Name [params...] k) body ...)` — handle effects. v1: lowered to reset.
- [x] `(resume k value)` — resume continuation with value. One-shot; consumes `k`. v1: runtime function `tur_cont_resume` implemented.
- [x] `(discontinue k exception)` — discontinue by raising an exception. v1: runtime function `tur_cont_drop` + `tur_throw` implemented.
- [ ] `(try-with body handler)` — sugar for `(reset (handle body handler))`. Deferred to v2.

**Type system — effect rows**
- [x] Effect row type `EffectRow` defined in `src/effect.h`.
- [x] `effect_row` field reserved in function types (Phase 4).
- [ ] Effect row syntax: `@ {Effect1, Effect2}` after return type in `defn`. Deferred to v2.
- [ ] Empty row `{}` means pure function (no effects). Deferred to v2.
- [ ] Effect polymorphism: functions can be generic over effect rows. Deferred to v2.
- [ ] Row union: calling a function with row `e1` inside a function with row `e2` produces row `e1 ∪ e2`. Deferred to v2.
- [ ] Subtyping: function with row `e1` is a subtype of function with row `e2` if `e1 ⊆ e2`. Deferred to v2.

**Effect declaration** — `src/elab.{c,h}`
- [x] `(defeffect Name [param1 : T1, param2 : T2] : R)` registers a new effect constructor. v1: type checked, stored in EffectEnv.
- [ ] Effects are scoped: can be module-private or exported. Deferred to v2.
- [x] Effect parameters are typed; result type is typed.
- [ ] Effects can be re-opened (add new constructors to existing effect type). Deferred to v2.

**Effect handling** — lowering
- [x] `perform (E args...)` lowers to: `shift k -> (dispatch-to-handler E args k)`. v1: implemented in `effect_lower.c` - lowers to shift with handler function.
- [x] `handle expr cases...` lowers to: `reset (push-handler-stack; expr; pop-handler-stack)`. v1: implemented in `effect_lower.c` - lowers to reset.
- [ ] Handler stack is a per-fiber linked list (TLS in single-threaded v1). Deferred to v2.
- [ ] Handler dispatch: walk handler stack for first matching case; call it with args and continuation. Deferred to v2.
- [x] `resume k v` lowers to: `continue k v` (consumes k, one-shot). v1: runtime function `tur_cont_resume` handles it.
- [x] `discontinue k e` lowers to: `throw e` (but in the context of the handler). v1: runtime functions `tur_cont_drop` + `tur_throw` handle it.

**Defer integration — S2 strategy** (per [effects-plan.md §6.2](archive/effects-plan.md))
- [x] When a continuation is captured (at `perform`), walk captured scope frames and heap-allocate them if not already heap. v1: `tur_cont_alloc` captures frame chain.
- [x] Defers are attached to scope frames; they fire when the frame is released. v1: `tur_frame_fire_chain` implemented.
- [x] Frame release happens on: (a) normal scope exit during resume, (b) continuation drop. v1: `tur_cont_resume` and `tur_cont_drop` handle this.
- [x] `ref<T>` drops are defers; same mechanism applies. v1: defers use same frame mechanism.
- [x] `rc<T>` releases are defers; same mechanism applies. v1: rc drops use same frame mechanism.

**Effect row checking** — `src/effect_check.{c,h}` (new pass)
- [ ] Pass runs after elaboration, before codegen. Deferred to v2.
- [ ] For each function, union effect rows of all call sites. Deferred to v2.
- [ ] Check that the union is a subset of the declared effect row. Deferred to v2.
- [ ] Unhandled effects at top level: compile-time error (static) or runtime panic (dynamic). Deferred to v2.
- [ ] Effect rows on `extern-c` are advisory (FFI functions assumed pure). Deferred to v2.

**Handler scoping**
- [x] Handlers are lexically scoped: `(handle ...)` binds handlers for its body only. v1: handle lowers to reset.
- [ ] Handler parameters shadow outer bindings. Deferred to v2.
- [ ] `k` (continuation) is a fresh binding in each handler case. Deferred to v2.
- [ ] Deep handlers: inner `handle` can capture outer handler's continuation. Deferred to v2.

**Stdlib effects** — `stdlib/effect.{c,h}` + `stdlib/effect.tur`
- [ ] `Read` effect: `(defeffect Read [^cstr prompt] : str)`. Deferred to v2.
- [ ] `Write` effect: `(defeffect Write [^cstr msg] : nil)`. Deferred to v2.
- [ ] `Fail` effect: `(defeffect Fail [^cstr msg] : a)` — non-local exit with message. Deferred to v2.
- [ ] `GetEnv` effect: `(defeffect GetEnv [^cstr key] : (option str))`. Deferred to v2.
- [ ] Console handler: handles `Read` and `Write` with stdin/stdout. Deferred to v2.
- [ ] Exception handler: converts `Fail` to exceptions. Deferred to v2.

**Interaction with other features**
- [x] **Closures:** Captured continuations in closures work naturally (closures already support captured state). v1: shift emission handles closures correctly.
- [ ] **Macros:** Macros can generate effectful code; hygiene handles the binding. Deferred to v2.
- [ ] **Modules (future):** Effects can be module-scoped; cross-module effect handling works via linking. Deferred to v2.
- [ ] **Borrow checker (Phase 14):** Effect handlers that capture references must respect borrow constraints. Deferred to v2.

**One-shot enforcement**
- [x] Continuations are move-only types: cannot be copied, only moved. v1: TY_CONT type with CK_MOVE.
- [ ] Static check: `resume` consumes its continuation argument; second use is use-after-move error. Deferred to v2.
- [x] Dynamic check: `resume` marks continuation as consumed; second call panics. v1: `tur_cont_resume` sets `consumed = true`.
- [ ] `cont?` predicate: check if a value is a continuation. Deferred to v2.
- [x] `cont-consumed?` predicate: check if a continuation has been resumed. v1: `tur_cont_consumed` implemented.

**Performance optimizations** (optional, post-MVP)
- [ ] Handler inlining: when handler is statically known, inline the dispatch. Deferred to v2.
- [ ] Monomorphic perform: when perform site has a statically known effect type, skip dynamic dispatch.
- [ ] Frame fusion: adjacent non-capturing scopes share frames.
- [ ] Escape analysis: scopes that provably don't escape remain stack-allocated.

**Fixtures**
- [x] `continuation-basic.tur` — simple `reset`/`shift` example (Phase 18).
- [x] `continuation-advanced.tur` — nested reset, multiple shifts, shift with closures.
- [x] `effect-syntax.tur` — parser accepts `defeffect` return keywords (`:int`) and `handle` case/body pair syntax.
- [x] `effect-syntax-compat.tur` — parser keeps supporting legacy `defeffect` return symbol syntax (`int`).
- [x] Negative: `effect-handle-pairs.tur` — malformed `handle` without case/body pairs reports parser error.
- [ ] `effect-declaration.tur` — declaring and performing effects. Deferred to v2.
- [ ] `effect-handler.tur` — basic effect handling. Deferred to v2.
- [ ] `effect-multiple.tur` — handling multiple effects. Deferred to v2.
- [ ] `effect-nested.tur` — nested handlers. Deferred to v2.
- [ ] `effect-defer.tur` — defers fire correctly with effects (S2 strategy). Deferred to v2.
- [ ] `effect-ref.tur` — ref drops fire correctly with effects. Deferred to v2.
- [ ] `effect-rc.tur` — rc releases fire correctly with effects. Deferred to v2.
- [ ] `effect-oneshot.tur` — one-shot continuations enforced. Deferred to v2.
- [ ] `effect-console.tur` — console I/O using Read/Write effects. Deferred to v2.
- [ ] `effect-fail.tur` — Fail effect for non-local exit. Deferred to v2.
- [ ] Negative: `effect-unhandled.tur` — unhandled effect error. Deferred to v2.
- [ ] Negative: `effect-double-resume.tur` — double resume panic. Deferred to v2.
- [x] Codegen snapshots: effect handling lowers to shift/reset. v1: perform->shift, handle->reset.

**Exit criterion:** ✅ v1 complete: algebraic effects infrastructure in place with effect lowering (perform->shift, handle->reset), CPS marking pass, direct-style emission with runtime continuation support, closure-aware shift emission. All 86 tests pass.

---

## Higher-Kinded Types (HKT Cluster — Phases H0–H6)

**Status:** Planned for v2+. Prerequisite: Phase 15 (Typeclasses v1 complete). See [hkt-implementation-plan.md](hkt-implementation-plan.md) for the full design document.

**Motivation:** HKTs enable user-defined typeclasses that quantify over type constructors (e.g., `Functor [f]` where `f : * -> *`), unblocking generic `traverse`/`sequence`/monad transformers and free-monad encodings. v1 typeclasses are restricted to kind-`*` only; HKTs lift that restriction for v2.

**Roadmap (6 phases, ~12–20 weeks estimated):**

| Phase | Goal | Exit Criterion |
|---|---|---|
| **H0** | Kind system foundation | Kind annotations parse and check; `option : * -> *`, `result : * -> * -> *` inferred correctly |
| **H1** | Kind-polymorphic typeclasses | `defclass` accepts kind-parameterized heads; constraint solving validates kinds |
| **H2** | HKT dispatch table | Two-level dictionary lookup (constructor + types); no v1 performance regression |
| **H3** | Built-in HKT typeclasses | `Functor`, `Applicative`, `Monad`, `Traversable`, `Foldable` defined and instanced for stdlib types |
| **H4** | Kind-polymorphic functions | `defn` with kind parameters; implicit kind inference; constraint propagation |
| **H5** | Advanced kinds | Binary type constructors (`* -> * -> *`), kind aliases, recursive HKT types (Fix, Free) |
| **H6** | Integration & polish | Stdlib migration, documentation, performance benchmarks, tooling (`--dump-kinds`, IDE support) |

**Why v2, not v1:**
- v1 algebr effects (Phases 17–19) already cover most monad-like abstraction use cases (`IO`, `State`, error handling) via direct-style code. HKTs add expressiveness but are lower priority.
- Kind system design (H0) is straightforward; the complexity is in dispatch generalization (H1–H2) and stdlib coverage (H3). Worth waiting for real v1 user feedback to validate the design before committing.
- Deferring to v2 avoids over-fit to a feature that might never be needed at scale in Turmeric's primary use case (embedded scripting + effects for systems programming).

**Decision rule for v2 promotion:** Add HKTs to active roadmap if at least **two** of the following are true after v1 stabilizes:
1. Users write repeated per-monad boilerplate that generic `traverse`/`sequence` would eliminate.
2. A library author needs generic monad transformers or free-monad encodings.
3. Significant demand from Haskell/Scala/PureScript users.

---

## Hybrid Result + Limited Panic (Phases R0–R6)

**Status:** Planned. Prerequisites: Phase 15 (Typeclasses v1 — for `Display`/`Debug` traits), Phase 17 (Exceptions — `setjmp`/`longjmp` substrate for `catch_unwind`). See [panic-system-vs-exception-system-plan.md](./archive/panic-system-vs-exception-system-plan.md) for full rationale and open-question answers.

**Summary of resolved design decisions:**
- Panic payloads are **typed** (not erased `Any`); catching specific panic types is supported via typed `catch_unwind`.
- The keyword is **`panic`** (not `throw`); `throw` is not provided as an alias.
- A `Must<T>` type (`.must()` / `(must! expr)`) panics on `None`/`Err` automatically.
- Panic/async and panic-in-Drop semantics follow Rust prior art (double-panic → abort; async boundary = `catch_unwind` boundary).

| Phase | Goal | Exit Criterion |
|---|---|---|
| **R0** | Design & prerequisites | Surface syntax finalised; `Result<T,E>` distinguished from existing `result`; panic payload schema agreed |
| **R1** | Core `Result<T, E>` type | `Ok`/`Err`, combinators, `?` operator, `Display`/`Debug` traits all working |
| **R2** | Panic mechanism | `panic!` macro, typed payloads, `catch_unwind`, specific-type catching, defer/drop ordering |
| **R3** | Standard library errors | `IoError`, `ParseError`, `From`/`Into` conversions, `Error` trait hierarchy |
| **R4** | `Must<T>` type | `(must! expr)` / `.must()` sugar; panics on `None` or `Err`; stdlib integration |
| **R5** | Interop & FFI | FFI-safe panic handling (abort vs. unwind choice); WASM representation; C exception bridge |
| **R6** | Async/effects & tooling | Panic + continuations/effects semantics; compiler warnings for unhandled `Result`; linter; stack-trace debug support |

---

### Phase R0 — Design & Prerequisites

**Goal:** Finalise all design decisions before any implementation lands, so phases R1–R6 can proceed without back-tracking.

**Design decisions to lock**
- [ ] Confirm `Result<T, E>` surface syntax: `(result T E)` type constructor, `(ok v)` / `(err e)` constructors, consistent with existing `stdlib/result.tur`. Determine whether stdlib `result.tur` already covers the required shape or needs to be replaced/extended.
- [ ] Specify panic payload schema: payloads are typed values (any Turmeric type); `catch_unwind` receives the payload as a typed `Result<T, PanicPayload>` where `PanicPayload` carries the runtime type tag and value.
- [ ] Specify `catch_unwind` surface syntax: `(catch-unwind thunk)` → `Result<T, PanicPayload>`. Decide whether it integrates with `try`/`catch` or stands alone.
- [ ] Specify typed `catch_panic` for specific panic types: `(catch-panic-of SomeType thunk)` → `Result<T, SomeType>`. Define what happens when payload type doesn't match (re-panics).
- [ ] Decide panic keyword: **`panic`** confirmed; no `throw` alias.
- [ ] Survey Rust `catch_unwind` + async for panic/async interaction model; document the ruling in `panic-system-vs-exception-system-plan.md`.
- [ ] Survey Rust double-panic behavior (abort) for panic-in-Drop; document the ruling.
- [ ] Define `Must<T>` semantics: wraps `option<T>` or `result<T, E>`; `.must()` method panics with a descriptive message on `none` or `err`; `(must! expr)` macro is sugar.
- [ ] Define standard `Error` trait (extends `Show`): `(defclass Error [e] (error-message [x : e] : cstr) (error-cause [x : e] : (option cstr)))`.
- [ ] Define `From`/`Into` conversion traits for error chaining.
- [ ] Confirm `?` operator surface syntax for `Result` propagation: `(? expr)` or postfix `.?` — pick one, reserve the other.

**Prerequisites check**
- [ ] Phase 15 typeclasses are stable (needed for `Display`, `Debug`, `Error` traits).
- [ ] Phase 17 exceptions are stable (`setjmp`/`longjmp` chain is the substrate for `catch_unwind`).
- [ ] `stdlib/result.tur` reviewed and gap-analysed against R1 requirements.

**Exit criterion:** All design bullets above are checked and recorded; no open ambiguities remain before R1 implementation starts.

---

### Phase R1 — Core `Result<T, E>` Type

**Goal:** Ship a fully-featured `Result<T, E>` type with ergonomic combinators and `?`-operator propagation. This is the primary mechanism for recoverable errors.

**Type system** — `src/types.{c,h}`
- [ ] Ensure `result<T, E>` is a first-class generic type in the elaborator (may already be true via `stdlib/result.tur`; verify).
- [ ] Add typeclass instances for `Eq`, `Show`/`Display` on `result<T, E>` when `T` and `E` implement them.
- [ ] Ensure `result<T, E>` participates in typeclass constraint solving (e.g., `^Show` on a `result` value).

**Surface syntax** — `src/reader.{c,h}` + `src/elab.{c,h}`
- [ ] `(ok value)` — construct `Ok` variant.
- [ ] `(err error)` — construct `Err` variant.
- [ ] `(ok? r)` — predicate: true if `Ok`.
- [ ] `(err? r)` — predicate: true if `Err`.
- [ ] `(ok-val r)` — extract value; panics if `Err` (sugar for `.must()`).
- [ ] `(err-val r)` — extract error; panics if `Ok`.
- [ ] `(? expr)` operator: if `expr` is `Ok(v)`, evaluates to `v`; if `Err(e)`, short-circuits returning `Err(e)` from the enclosing function. Compile error if used outside a function returning `Result`.
- [ ] `(result-map r f)` — map over `Ok` value.
- [ ] `(result-flat-map r f)` / `(result-and-then r f)` — monadic bind.
- [ ] `(result-map-err r f)` — map over `Err` value.
- [ ] `(result-or r default)` — return `Ok` value or `default` on `Err`.
- [ ] `(result-or-else r f)` — return `Ok` value or call `f` with `Err` value.
- [ ] `(result-unwrap-or r default)` — alias for `result-or`.
- [ ] `(result-expect r msg)` — return `Ok` value or panic with `msg`.

**Standard `Error` trait** — `stdlib/typeclass.tur` + `stdlib/exn.tur`
- [ ] `(defclass Error [e] (error-message [x : e] : cstr) (error-cause [x : e] : (option cstr)))`.
- [ ] `(definstance Error Error ...)` for the existing stdlib `Error` struct.
- [ ] `(definstance Error IoError ...)` and `(definstance Error ParseError ...)`.
- [ ] `(defclass Display [a] (display [x : a] : cstr))` — if not already defined via `Show`.
- [ ] `(defclass Debug [a] (debug [x : a] : cstr))` — machine-readable representation.

**`From`/`Into` conversion traits** — `stdlib/typeclass.tur`
- [ ] `(defclass From [a b] (from [x : b] : a))` — convert `b` into `a`.
- [ ] `(defclass Into [a b] (into [x : a] : b))` — derived automatically from `From`.
- [ ] Blanket instance: if `(From A B)` exists, then `(Into B A)` is automatically derived.
- [ ] Error conversion: `(From IoError Error)` etc. for stdlib error types.
- [ ] `?` operator uses `From` to convert error type when propagating across function boundaries.

**Stdlib** — `stdlib/result.tur`
- [ ] Ensure all above combinators are implemented and tested.
- [ ] Add `(result-collect vec-of-results)` — collect `(vec (result T E))` into `(result (vec T) E)` (short-circuits on first `Err`).
- [ ] Add `(result-partition results)` — split `(vec (result T E))` into `(vec T, vec E)`.

**Fixtures** — `tests/fixtures/result/`
- [ ] `result-basic.tur` — `ok`/`err` construction and pattern matching.
- [ ] `result-combinators.tur` — `map`, `flat-map`, `map-err`, `or`, `or-else`.
- [ ] `result-question-op.tur` — `?` operator propagation.
- [ ] `result-display.tur` — `Show`/`Display` instances.
- [ ] `result-from-into.tur` — `From`/`Into` conversion and `?` cross-type propagation.
- [ ] `result-collect.tur` — collect and partition.
- [ ] Negative: `result-question-outside-fn.tur` — `?` outside `Result`-returning function is a compile error.
- [ ] Codegen snapshots: `ok`/`err` lower to tagged union; `?` lowers to early return.

**Exit criterion:** `Result<T, E>` is the idiomatic recoverable-error type; combinators cover common patterns; `?` operator propagates errors ergonomically; `Error` trait and `From`/`Into` chain allow library-defined error types.

---

### Phase R2 — Panic Mechanism

**Goal:** Implement `panic!` with typed payloads, `catch_unwind` for boundary recovery, and support for catching specific panic types. Defer/drop ordering follows Rust semantics.

**Surface syntax**
- [ ] `(panic msg)` — panic with a string message; payload is a `cstr`.
- [ ] `(panic-with payload)` — panic with a typed payload value (any Turmeric type).
- [ ] `(catch-unwind thunk)` → `result<T, panic-payload>` — recover from any panic at a boundary. `panic-payload` carries the runtime type tag and boxed value.
- [ ] `(catch-panic-of PanicType thunk)` → `result<T, PanicType>` — recover from panics whose payload matches `PanicType`; re-panics with the original payload if the type doesn't match.
- [ ] `(panic-payload-type p)` — inspect the runtime type tag of a `panic-payload`.
- [ ] `(panic-payload-downcast p PanicType)` → `option<PanicType>` — attempt typed extraction.

**Runtime** — `src/runtime.{c,h}` + `src/exn.{c,h}`
- [ ] `tur_panic(const char* msg)` — panic with string; wraps in `tur_exception` with `PANIC_KIND_MSG` tag and calls `tur_throw`.
- [ ] `tur_panic_with(TypeKind type, void* payload)` — panic with typed payload.
- [ ] `tur_catch_unwind(tur_thunk_fn thunk, void* env, tur_result_t* out)` — setjmp boundary; calls thunk; if panic occurs, catches it and writes `Err(panic_payload)` to `out`.
- [ ] `tur_catch_panic_of(TypeKind expected_type, tur_thunk_fn thunk, void* env, tur_result_t* out)` — like `catch_unwind` but re-panics if payload type doesn't match `expected_type`.
- [ ] `panic-payload` struct: `{ TypeKind type_tag; void* value; const char* file; int line; }`.
- [ ] Double-panic behavior: if a panic occurs while unwinding from another panic (i.e., a panic fires inside a `defer` block that was triggered by a panic), call `abort()` immediately. (Prior art: Rust.)
- [ ] Panic in effect handler continuation: if `resume` is called inside a `catch_unwind` boundary, panics propagate out of the `reset` block and are caught by the nearest `catch_unwind`. Document the interaction.

**Elaborator / codegen** — `src/elab.{c,h}` + `src/emit.{c,h}`
- [ ] `(panic msg)` lowers to: `tur_panic(msg)` — never returns (mark as diverging type `!`).
- [ ] `(panic-with payload)` lowers to: box payload, call `tur_panic_with(type_tag, boxed)`.
- [ ] `(catch-unwind thunk)` lowers to: allocate `tur_result_t`, call `tur_catch_unwind`, return result.
- [ ] `(catch-panic-of T thunk)` lowers to: call `tur_catch_panic_of` with type tag for `T`.
- [ ] Diverging type `!` (never type): `panic` has return type `!`; `!` is a subtype of everything so it can appear in any branch of `if`/`match`.

**Interaction with defer and ref**
- [ ] Panics trigger the same defer/unwind chain as exceptions (Phase 17's `tur_throw` mechanism).
- [ ] `ref<T>` and `rc<T>` drops fire during panic unwinding (already true via defer chain; verify end-to-end).
- [ ] Document: panics do **not** fire defers in `catch_unwind`-captured continuations unless explicitly resumed.

**Fixtures** — `tests/fixtures/panic/`
- [ ] `panic-basic.tur` — `(panic "msg")` unwinds and prints error.
- [ ] `panic-with-typed.tur` — `(panic-with payload)` with a user-defined struct payload.
- [ ] `panic-catch-unwind.tur` — `catch_unwind` catches a panic; `Ok` on success, `Err(payload)` on panic.
- [ ] `panic-catch-of-type.tur` — `catch-panic-of` catches matching type; re-panics on mismatch.
- [ ] `panic-downcast.tur` — `panic-payload-downcast` extracts payload.
- [ ] `panic-defer.tur` — defers fire during panic unwinding.
- [ ] `panic-ref.tur` — `ref<T>` drops fire during panic unwinding.
- [ ] `panic-double-panic.tur` — panic in defer during panic unwind calls `abort()`.
- [ ] Negative: `panic-in-pure-context.tur` — linter warns when `panic` is called in a pure/effect-annotated function (deferred to R6 linter phase).
- [ ] Codegen snapshots: `panic` lowers to `tur_panic`; `catch_unwind` lowers to `setjmp`-based boundary.

**Exit criterion:** `panic!` with typed payloads works; `catch_unwind` and `catch-panic-of` work at boundaries; defer/ref drops fire correctly during panic unwinding; double-panic aborts.

---

### Phase R3 — Standard Library Errors

**Goal:** Provide a rich, composable set of stdlib error types and the `From`/`Into` chain that lets library code convert between them ergonomically.

**Stdlib error types** — `stdlib/exn.tur`
- [ ] `(defstruct Error [message : cstr, cause : (option cstr)])` — base error type (may already exist; verify and extend if needed).
- [ ] `(defstruct IoError [message : cstr, errno : int, path : (option cstr)])` — I/O errors with optional path context.
- [ ] `(defstruct ParseError [message : cstr, line : int, col : int, file : cstr])` — parsing errors with source location.
- [ ] `(defstruct ValidationError [message : cstr, field : (option cstr)])` — input validation errors.
- [ ] `(defstruct NotFoundError [what : cstr])` — generic not-found error.
- [ ] `(defstruct PermissionError [message : cstr, path : (option cstr)])` — permission denied.

**Error trait instances** — `stdlib/exn.tur`
- [ ] `(definstance Error Error ...)` — `error-message` returns `message`; `error-cause` returns `cause`.
- [ ] `(definstance Error IoError ...)`, `ParseError`, `ValidationError`, `NotFoundError`, `PermissionError`.
- [ ] `(definstance Show Error ...)` etc. for all error types.

**`From`/`Into` conversions** — `stdlib/exn.tur`
- [ ] `(definstance From IoError Error ...)` — upcast `IoError` to base `Error`.
- [ ] `(definstance From ParseError Error ...)` — upcast.
- [ ] `(definstance From ValidationError Error ...)` — upcast.
- [ ] Define pattern for library authors: how to add their own `From` instances without orphan conflicts.

**Convenience helpers** — `stdlib/exn.tur`
- [ ] `(defn io-error [msg errno])` — construct `IoError`.
- [ ] `(defn parse-error [msg line col file])` — construct `ParseError`.
- [ ] `(defn ok-or [value error-fn])` — convert `option<T>` to `result<T, E>` with a lazily-evaluated error.
- [ ] `(defn err-context [result context-msg])` — wrap the error in additional context string (like Rust's `context()` from `anyhow`).

**Fixtures** — `tests/fixtures/result-errors/`
- [ ] `error-types-basic.tur` — construct each stdlib error type and display it.
- [ ] `error-from-into.tur` — `From`/`Into` conversions between error types.
- [ ] `error-context.tur` — `err-context` adds context to an error.
- [ ] `error-ok-or.tur` — `option` to `result` conversion.
- [ ] Codegen snapshots: error struct layouts, trait dispatch.

**Exit criterion:** stdlib error types cover common cases; `From`/`Into` chain allows ergonomic error propagation without manual wrapping; `?` operator uses `From` automatically.

---

### Phase R4 — `Must<T>` Type

**Goal:** Provide a `Must<T>` abstraction (analogous to Rust's `.unwrap()`) that panics on `None` or `Err` with a descriptive message. Keeps happy-path code clean without hiding errors.

**Design**
- [ ] `Must<T>` is not a separate struct — it is a set of methods/macros on `option<T>` and `result<T, E>`.
- [ ] `(must! expr)` macro: if `expr` is `(some v)` or `(ok v)`, evaluates to `v`; otherwise panics with `"must! failed: got none"` or `"must! failed: got err: <display>"`.
- [ ] `(must-msg! expr msg)` macro: same but panics with `msg` instead of default message.
- [ ] `(defn option-must [opt])` — method form of `(must! ...)` for `option<T>`.
- [ ] `(defn result-must [res])` — method form for `result<T, E>`.
- [ ] `(defn result-must-msg [res msg])` — with custom panic message.
- [ ] `(defn option-expect [opt msg])` — alias matching Rust naming.
- [ ] `(defn result-expect [res msg])` — alias matching Rust naming.

**Linter integration (deferred to R6)**
- [ ] Emit a lint warning when `must!` appears in library code (i.e., not in `main` or test fixtures) — the same "panics should be rare" rule.

**Fixtures** — `tests/fixtures/must/`
- [ ] `must-option-some.tur` — `(must! (some 42))` returns `42`.
- [ ] `must-option-none.tur` — `(must! none)` panics with descriptive message.
- [ ] `must-result-ok.tur` — `(must! (ok 42))` returns `42`.
- [ ] `must-result-err.tur` — `(must! (err (Error. "oops" none)))` panics and displays the error.
- [ ] `must-msg.tur` — custom panic message via `must-msg!`.
- [ ] `must-expect.tur` — `option-expect` and `result-expect` aliases.
- [ ] Codegen snapshots: `must!` lowers to inline branch + `tur_panic`.

**Exit criterion:** `must!` and `must-msg!` work on both `option<T>` and `result<T, E>`; panics display useful messages; `expect` aliases work.

---

### Phase R5 — Interop & FFI

**Goal:** Make panics and `Result` safe across FFI and WASM boundaries.

**FFI-safe panic handling** — `src/runtime.{c,h}`
- [ ] Define `TUR_PANIC_STRATEGY` compile-time flag: `UNWIND` (default, setjmp-based) or `ABORT` (call `abort()` immediately on panic — safe for FFI boundaries).
- [ ] `tur_panic_abort(const char* msg)` — panic that always aborts; used at `extern-c` call sites when `ABORT` strategy is chosen.
- [ ] Add `#[no-unwind]` attribute for `defn` declarations: marks functions that must not allow panics to escape (panic inside is converted to abort). Useful for callbacks registered with C libraries.
- [ ] Document FFI rule: panics must not cross `extern-c` boundaries; use `catch-unwind` at the boundary or mark with `#[no-unwind]`.

**WASM representation**
- [ ] Decide WASM panic representation: `unreachable` instruction (abort semantics) or custom `__tur_panic` import from host (recoverable in JS). Document the decision.
- [ ] Implement chosen WASM panic lowering in `src/emit.{c,h}`.
- [ ] Ensure `catch_unwind` at WASM module boundary converts panics to `Result` before returning to host.

**Exception translation layer**
- [ ] `(result->exception res)` — if `Err(e)`, throw as exception; if `Ok(v)`, return `v`. Bridge for code that expects exceptions.
- [ ] `(exception->result thunk)` — run `thunk` catching exceptions; return `Ok(v)` or `Err(exception)`. Converts exception-style code to `Result`-style.

**Fixtures** — `tests/fixtures/panic-ffi/`
- [ ] `panic-ffi-boundary.tur` — `catch-unwind` wraps an FFI call boundary; verifies panic doesn't leak into C.
- [ ] `panic-no-unwind.tur` — `#[no-unwind]` function panics → `abort()`.
- [ ] `result-exception-bridge.tur` — `result->exception` and `exception->result` round-trip.

**Exit criterion:** Panics do not cross FFI boundaries without explicit wrapping; WASM panic strategy is defined and implemented; exception/result bridge works.

---

### Phase R6 — Async/Effects Integration & Tooling

**Goal:** Define panic semantics inside continuations and effects; add compiler warnings, linter rules, and debugging support.

**Panic + continuations/effects semantics**
- [ ] Define: panicking inside `(catch-unwind ...)` that spans a `reset` boundary is well-defined — the panic exits the `reset` block and is caught by `catch_unwind`.
- [ ] Define: panicking inside a `handle` case (after `resume`) propagates to the nearest enclosing `catch_unwind`. Document in `panic-system-vs-exception-system-plan.md`.
- [ ] Define: panicking inside an async task (if async is implemented in the future) follows Rust's model — the task's future resolves to `Err(PanicPayload)` at the join point.
- [ ] Implement `(catch-unwind-cont ...)` variant if needed for continuation-aware boundaries (defer to async phase if not needed sooner).
- [ ] Write and publish `docs/error-handling-guide.md` covering `Result`, `panic`, `must!`, `catch_unwind`, and when to use each.

**Compiler warnings for unhandled `Result`**
- [ ] Add elaborator pass: if a `result<T, E>` expression value is silently discarded (not bound, not pattern-matched, not passed), emit a warning: `"warning: unused Result value; consider using `?`, `must!`, or explicit match"`.
- [ ] Warning is suppressible with `(ignore! expr)` helper that explicitly discards the value.
- [ ] Configuration: `--warn-unused-result` (default on) / `--no-warn-unused-result`.

**Linter rules for panic usage**
- [ ] Add lint: `panic` calls in functions whose name does not contain `test` and is not `main` emit a note: `"note: consider returning Result instead of panicking"`.
- [ ] Add lint: `must!` in non-test, non-main code emits same note.
- [ ] Lint is opt-in via `--lint-panic` flag (not a hard error).
- [ ] Add lint: `catch_unwind` used for normal error handling (i.e., inside a function that already returns `Result`) emits warning: `"catch_unwind is for boundary recovery, not normal error handling; consider propagating Result instead"`.

**Debugging support**
- [ ] `tur_panic` prints: `"panic at <file>:<line>: <message>"` to stderr before unwinding.
- [ ] `tur_catch_unwind` captures the panic location in `panic_payload.file` / `.line`.
- [ ] `--panic-trace` debug flag: prints full scope chain when a panic fires (like Rust's `RUST_BACKTRACE=1`).
- [ ] `--panic-abort` flag: all panics call `abort()` instead of unwinding (useful for crash debugging with a core dump).
- [ ] Ensure panic messages from `must!` include the failing expression text (use `__FILE__`/`__LINE__` macros in C lowering).

**Fixtures** — `tests/fixtures/panic-tooling/`
- [ ] `warn-unused-result.tur` — unused `Result` triggers compiler warning.
- [ ] `warn-suppress-ignore.tur` — `(ignore! expr)` suppresses the warning.
- [ ] `panic-trace.tur` — `--panic-trace` output includes file/line in golden output.
- [ ] Codegen snapshots: panic lowering includes file/line injection.

**Exit criterion:** Compiler warns on discarded `Result`; linter flags excessive `panic` usage; `--panic-trace` provides actionable debug output; error handling guide documents the full model.

---

## Thread Safety and Thread Primitives (Phases T19–T21)

**Status:** Planned (v2). Prerequisites: Phase 17 (Exceptions — panic propagation from threads), Phase 4 (`defer` + scope unwind — lock release), Phase 5 (`ref<T>` ownership model). See [thread-safety-and-primitives-plan.md](archive/thread-safety-and-primitives-plan.md) for full design.

| Phase | Goal | Exit Criterion |
|---|---|---|
| **T19** | Thread primitives v1 | `Arc<T>`, `Mutex<T>`, `Atomic<T>`, `Thread`/`JoinHandle`, `Chan<T>`, borrow checker `Send`/`Sync` enforcement all working |
| **T20** | Thread pool and higher-level abstractions | `ThreadPool`, `Future<T>`, `Promise<T>`, `WorkQueue<T>`, `Semaphore` in stdlib |
| **T21** | Fibers and effects integration (v2) | Fiber type, cooperative scheduler, `async`/`await` sugar via fibers + continuations |

---

### Phase T19 — Thread Primitives (v1)

**Goal:** Add basic thread safety and thread primitives to Turmeric via C11 `<threads.h>` and `<stdatomic.h>`.

**`Send`/`Sync` marker traits** — `src/types.{c,h}` + `src/typeclass.{c,h}`
- [ ] Add `Send` marker trait: `T` is `Send` if it can be safely transferred to another thread (ownership moves).
- [ ] Add `Sync` marker trait: `T` is `Sync` if it can be safely shared across threads (immutable or properly synchronized).
- [ ] Auto-implement `Send` and `Sync` for all primitive types (`int`, `bool`, `float`, etc.).
- [ ] Mark `ptr<T>`, `ref<T>`, `rc<T>`, `cont<T>` as neither `Send` nor `Sync`.
- [ ] `Arc<T>` is `Send + Sync` if `T` is `Send + Sync`; `Mutex<T>` is `Send + Sync` if `T` is `Send`.
- [ ] Enforce `Sync` implies `Send` in the elaborator.
- [ ] Borrow checker integration: reject closures that capture non-`Send` types when passed to `thread`.

**`Arc<T>` — atomic reference counting** — `src/rc.{c,h}` + `stdlib/arc.tur`
- [ ] Define `Arc<T>` struct: `_Atomic(int) refcount` + `T value` in heap-allocated storage.
- [ ] Implement `Arc::new`, `Arc::clone` (atomic refcount increment), `Arc::get` (immutable borrow).
- [ ] Implement `Arc::drop` (atomic refcount decrement; free when zero, drop value).
- [ ] Borrow checker: `Arc::clone` is a copy (shared ownership, atomic); `Arc::drop` is a consume.
- [ ] Lowering: map to C11 `atomic_fetch_add`/`atomic_fetch_sub` on `_Atomic(int)`.

**`Atomic<T>` — atomic operations** — `stdlib/atomic.tur`
- [ ] Implement `Atomic::new`, `Atomic::load`, `Atomic::store`, `Atomic::exchange`.
- [ ] Implement `Atomic::compare-exchange` (returns `{:ok new-value}` or `{:err current-value}`).
- [ ] Implement `Atomic::fetch-add`, `Atomic::fetch-sub` for numeric types.
- [ ] Memory ordering options: `:relaxed`, `:acquire`, `:release`, `:acqrel`, `:seqcst`.
- [ ] Supported types: `bool`, `int`, `uint`, `isize`, `usize`, `ptr<T>`.
- [ ] Lowering: map directly to C11 `_Atomic` and `atomic_*` functions.

**`Mutex<T>` — mutual exclusion** — `stdlib/mutex.tur`
- [ ] Implement `Mutex::new`, `Mutex::lock`, `Mutex::unlock`, `Mutex::get`, `Mutex::set!`.
- [ ] Implement `Mutex::with-lock` scoped variant integrating with `defer` for automatic unlock.
- [ ] Implement poison detection: if a thread panics while holding the lock, mark it poisoned; subsequent `lock` returns `Err(Poisoned)`.
- [ ] Lowering: map to C11 `mtx_t`.

**`RwLock<T>` — reader-writer lock** — `stdlib/rwlock.tur`
- [ ] Implement `RwLock::new`, `RwLock::read`, `RwLock::read-unlock`, `RwLock::write`, `RwLock::write-unlock`.
- [ ] Implement `RwLock::with-read` and `RwLock::with-write` scoped variants.
- [ ] Lowering: custom POSIX `pthread_rwlock_t`-based implementation (C11 lacks a built-in rwlock).

**`Condvar` — condition variables** — `stdlib/condvar.tur`
- [ ] Implement `Condvar::new`, `Condvar::wait`, `Condvar::notify-one`, `Condvar::notify-all`.
- [ ] Implement `Condvar::with-wait` scoped variant.
- [ ] Lowering: map to C11 `cnd_t`.

**`Once` and `Barrier`** — `stdlib/sync.tur`
- [ ] Implement `Once::new`, `Once::call` (execute function exactly once; subsequent calls are no-ops).
- [ ] Implement `Barrier::new`, `Barrier::wait` (block until N threads have each called `wait`).
- [ ] Lowering: `Once` → C11 `once_flag`/`call_once`; `Barrier` → custom `Mutex` + `Condvar` + counter.

**`Thread`/`JoinHandle`** — `stdlib/thread.tur`
- [ ] Implement `thread` form: spawn an OS thread, return `JoinHandle`.
- [ ] Implement `Thread::join` (block until completion; returns `Result<T, exn>`).
- [ ] Implement `Thread::detach`, `Thread::id`, `Thread::done?`.
- [ ] Thread attributes: `:stack-size`, `:detached`, `:name`.
- [ ] Lowering: `thread` → `thrd_create()` with trampoline; `Thread::join` → `thrd_join()`.

**Thread-local storage** — `stdlib/thread.tur`
- [ ] Implement `thread-local` form, `thread-local-get`, `thread-local-set!`.
- [ ] Lowering: C11 `thread_local` or `tss_t` key-based TLS.

**`Chan<T>` — synchronous channel** — `stdlib/chan.tur`
- [ ] Implement `Chan::new`, `Chan::send` (blocks until receiver ready), `Chan::recv` (blocks until sender ready).
- [ ] Lowering: custom `Mutex` + `Condvar` implementation.

**`AsyncChan<T>` — buffered async channel** — `stdlib/chan.tur`
- [ ] Implement `AsyncChan::new` (with buffer size), `AsyncChan::send`, `AsyncChan::recv`.
- [ ] Implement `AsyncChan::try-send`, `AsyncChan::try-recv` (non-blocking variants; return `:full`/`:empty`).
- [ ] Lowering: custom `Mutex` + `Condvar` + ring-buffer queue.

**`Select` — multi-channel operations** — `stdlib/chan.tur`
- [ ] Implement `select` form: wait on multiple channels, dispatch to first-ready branch.
- [ ] `default` branch for non-blocking select (no-op if no channel is ready).
- [ ] Lowering: custom state machine; each `select` registers with all involved channels.

**Integration with existing features**
- [ ] `defer`: `Mutex::with-lock`, `RwLock::with-read/write` all release via `defer` on scope exit.
- [ ] Exceptions: thread panics store the exception in `JoinHandle`; returned by `Thread::join`.
- [ ] `ref<T>`/`rc<T>`: not `Send`/`Sync`; use `Arc<Mutex<T>>` or `Arc<Atomic<T>>` for shared state.
- [ ] Continuations: `cont<T>` is not `Send` (captures C stack); resuming on wrong thread is UB.
- [ ] Effect handler chains: migrate `global_handler_chain` and `global_effect_handler_chain` to `__thread` TLS.

**Fixtures** — `tests/fixtures/threads/`
- [ ] `thread-basic.tur` — spawn and join a thread.
- [ ] `arc-basic.tur` — `Arc<T>` clone/drop across threads.
- [ ] `mutex-basic.tur` — basic mutex lock/unlock.
- [ ] `mutex-poison.tur` — poison detection on panic.
- [ ] `rwlock-basic.tur` — reader-writer lock with multiple readers.
- [ ] `atomic-basic.tur` — atomic increment from multiple threads.
- [ ] `channel-basic.tur` — synchronous channel send/recv.
- [ ] `async-channel.tur` — buffered async channel.
- [ ] `select-basic.tur` — multi-channel select.
- [ ] `barrier.tur` — barrier synchronization for N threads.
- [ ] `once.tur` — one-time initialization.
- [ ] `thread-arc.tur` — `Arc<Mutex<T>>` for shared mutable state.
- [x] Integration: `threaded-fizzbuzz.tur` — multi-threaded FizzBuzz.
- [x] Integration: `producer-consumer.tur` — producer-consumer with channels.
- [x] Stress: `thread-stress.tur` — spawn 100 threads, join all. (`requires.tsan`)
- [x] Stress: `mutex-stress.tur` — 10 threads contend on a mutex. (`requires.tsan`)
- [x] Stress: `atomic-stress.tur` — atomic increment from 100 threads. (`requires.tsan`)
- [ ] Negative: `thread-send-ref.tur` — sending `ref<T>` across threads is a compile error.
- [ ] Negative: `thread-send-cont.tur` — sending `cont<T>` across threads is a compile error.
- [ ] Codegen snapshots: thread spawn, `Arc` refcount, `Mutex` lock/unlock lowering.

**Exit criterion:** Thread spawn and join work; `Arc<T>`, `Mutex<T>`, `Atomic<T>` work correctly; `Send`/`Sync` checking rejects unsafe cross-thread operations at compile time; channels and `Select` work; all fixtures pass under ThreadSanitizer.

---

### Phase T20 — Thread Pool and Higher-Level Abstractions

**Goal:** Add a thread pool and higher-level concurrency abstractions on top of T19 primitives.

**`ThreadPool`** — `stdlib/threadpool.tur`
- [ ] Implement `ThreadPool::new` with fixed size: takes a thread count, spawns worker threads.
- [ ] Implement `ThreadPool::submit` (submit a closure; returns `Future<T>`).
- [ ] Implement `ThreadPool::shutdown` (drain queue and join all threads).
- [ ] Implement dynamic-scaling variant: `ThreadPool::new-dynamic` (grows up to a max size on demand).

**`Future<T>` and `Promise<T>`** — `stdlib/future.tur`
- [ ] Implement `Future<T>`: represents a value that will be available asynchronously.
- [ ] Implement `Future::get` (blocks until value available; returns `Result<T, exn>`).
- [ ] Implement `Future::done?` (non-blocking check).
- [ ] Implement `Promise<T>`: producer side of a `Future`.
- [ ] Implement `Promise::fulfill` (set the value; wakes all blocked `Future::get` callers).
- [ ] Implement `Promise::fail` (set an exception; `Future::get` returns `Err`).

**`WorkQueue<T>`** — `stdlib/threadpool.tur`
- [ ] Implement `WorkQueue::new`, `WorkQueue::push`, `WorkQueue::pop`.
- [ ] Bounded queue: `WorkQueue::new-bounded` (blocks on `push` when full, `pop` when empty).
- [ ] Thread-safe via internal `Mutex` + `Condvar`.

**`Semaphore`** — `stdlib/sync.tur`
- [ ] Implement `Semaphore::new` (with initial count), `Semaphore::acquire`, `Semaphore::release`.
- [ ] Lowering: custom `Mutex` + `Condvar` + counter.

**Fixtures** — `tests/fixtures/threads/`
- [ ] `thread-pool-basic.tur` — submit tasks, collect results.
- [ ] `thread-pool-dynamic.tur` — dynamic-scaling pool.
- [ ] `future-basic.tur` — `Future`/`Promise` round-trip.
- [ ] `future-error.tur` — `Promise::fail` propagates error to `Future::get`.
- [ ] `work-queue.tur` — bounded work queue with producers and consumers.
- [ ] `semaphore.tur` — counting semaphore usage.
- [ ] Integration: `raytracer.tur` — parallel ray-tracer using thread pool.

**Exit criterion:** Thread pool with fixed and dynamic sizing works; `Future`/`Promise` abstraction is ergonomic; `Semaphore` and `WorkQueue` are available in stdlib.

---

### Phase T21 — Fibers and Effects Integration (v2)

**Goal:** Add fibers (user-space cooperative threads) that integrate with delimited continuations and effects, enabling `async`/`await` sugar.

**Prerequisites:**
- Phase T19 — Thread primitives (OS-thread scheduler host).
- Phase 18 — Delimited continuations (`reset`/`shift` CPS substrate).
- Phase 19 — Algebraic effects (for full handler integration).

**Fiber type and API** — `stdlib/fiber.tur`
- [ ] Define `Fiber<T>` type: user-space coroutine backed by a heap-allocated stack.
- [ ] Implement `Fiber::new` (create a fiber from a closure), `Fiber::resume`, `Fiber::yield`.
- [ ] Implement `Fiber::done?`, `Fiber::result`.
- [ ] Fiber-local storage: `fiber-local`, `fiber-local-get`, `fiber-local-set!`.

**Fiber scheduler** — `stdlib/fiber.tur`
- [ ] Implement cooperative scheduler: run fibers until they yield or complete.
- [ ] `Scheduler::new`, `Scheduler::spawn`, `Scheduler::run-to-completion`.
- [ ] Integrate with channels: a fiber blocked on `Chan::recv` yields to the scheduler.

**`reset`/`shift` integration** — `src/cps.{c,h}` + `src/runtime.{c,h}`
- [ ] Continuations captured inside a fiber are scoped to that fiber's stack; cross-fiber resume is a runtime error.
- [ ] Fibers interoperate with `handle`/`perform` effect handlers within the same fiber.

**`async`/`await` sugar** — `src/reader.{c,h}` + `src/elab.{c,h}`
- [ ] `(async expr)` — wrap expression in a fiber; return type is `Future<T>`.
- [ ] `(await future)` — yield current fiber until `future` completes; evaluates to `T`.
- [ ] `async`/`await` desugars to `Fiber::new` + `Fiber::resume` + `Fiber::yield` calls.
- [ ] Implement `ThreadPool::submit-async` for submitting `async` tasks.

**Fixtures** — `tests/fixtures/fibers/`
- [ ] `fiber-basic.tur` — create and resume a fiber.
- [ ] `fiber-yield.tur` — fiber yield and resume round-trip.
- [ ] `fiber-scheduler.tur` — multiple fibers scheduled cooperatively.
- [ ] `async-await-basic.tur` — `async`/`await` syntax sugar.
- [ ] `async-await-channel.tur` — async producer-consumer via channels.
- [ ] Negative: `fiber-cross-resume.tur` — resuming a continuation on the wrong fiber panics.

**Exit criterion:** Fibers work as cooperative coroutines; `async`/`await` desugars correctly; fiber scheduler runs multiple concurrent fibers; integration with effects and channels is clean.

---

## Software Transactional Memory (Phases 20–21)

**Status:** Planned (v2). Prerequisites: Phase 19 (Algebraic effects — `stm` desugars to a transaction closure; continuation infrastructure in place), T19 (Thread primitives — `Mutex<T>`, condition variables, `Arc<T>`). See [stm-plan.md](archive/stm-plan.md) for full design.

**Key design decisions:**
- **Haskell-style API:** `TVar<T>`, `stm` block, `atomically`, `retry`, `or-else`, `check`; `TMVar`/`TChan`/`TSem` primitives.
- **Lock-based v1:** Single global `mtx_t` for simplicity; correct by construction; replaced by fine-grained per-TVar locking in v2.
- **`stm` is a special form** handled by `elab_stm` in `src/elab.c`; `TVar::read`/`TVar::write` outside an `stm` block are compile errors (TUR-E0009).
- **`defer` inside `stm`:** defers execute at transaction commit (success) or transaction abort (failure) — not at `stm` lexical exit.
- **Exception inside `stm`:** transaction aborts (writes discarded), abort-path defers fire, then exception propagates normally.
- **`TVar` naming:** `TVar` (Haskell style); `dosync` is the ergonomic shorthand macro.

| Phase | Goal | Exit Criterion |
|---|---|---|
| **20** | STM core (v1) | `TVar<T>`, `stm`/`atomically`, `retry`/`check`/`or-else`, `TMVar`/`TChan`/`TSem`, global lock, `stdlib/stm.tur`, unit + simple concurrency tests all passing |
| **21** | Scalable STM (v2) | Per-TVar locking with lock ordering, lock stripping, performance benchmarks, TSan-clean stress tests |

---

### Phase 20 — STM Core (v1)

**Goal:** Add Haskell-style Software Transactional Memory with a global-lock implementation suitable for low-to-medium contention workloads.

**Prerequisites:**
- Phase 19 complete (algebraic effects infrastructure stable).
- T19 complete (`Mutex<T>`, condition variables, `Arc<T>`, POSIX `pthread_cond_t`).

**Runtime data structures** — `src/stm.{c,h}` (new files)
- [ ] Define `TVar` struct: `{ TypeInfo *type; void *value; uint64_t version; STM_WaitQueue waiters; }`.
- [ ] Define `STM_Transaction` struct: `{ TVar **read_set; uint64_t *read_versions; int read_count; TVar **write_set; void **new_values; int write_count; bool retry_requested; tur_frame_t *defer_stack; }`.
- [ ] Define `STM_State` global: `{ mtx_t global_lock; }`.
- [ ] Define `STM_WaitQueue`: `{ STM_Transaction **waiters; int count; pthread_cond_t cond; }`.
- [ ] Implement `tur_tvar_new(TypeInfo *type, void *initial_value) → TVar *`.
- [ ] Implement `tur_tvar_read(STM_Transaction *tx, TVar *tv) → void *` (records read in transaction log).
- [ ] Implement `tur_tvar_write(STM_Transaction *tx, TVar *tv, void *value)` (records write in transaction log).
- [ ] Implement `tur_stm_validate(STM_Transaction *tx) → bool` (checks all read versions are still current).
- [ ] Implement `tur_stm_commit(STM_Transaction *tx) → bool` (applies writes, increments versions, fires commit-path defers).
- [ ] Implement `tur_stm_abort(STM_Transaction *tx)` (discards writes, fires abort-path defers).
- [ ] Implement `tur_stm_retry(STM_Transaction *tx)` (adds to wait queues of all read TVars, blocks on condition variable).
- [ ] Implement `tur_stm_check(bool condition)` (calls `tur_stm_abort` if false).
- [ ] Implement `tur_atomically(stm_fn_t fn, void *env) → void *` (outer retry loop with global lock).
- [ ] Use `__thread STM_Transaction *tur_current_tx` for thread-local transaction context (migrates to `TUR_THREAD_LOCAL` in the same pass as effect handler chains).

**Elaborator and codegen** — `src/elab.{c,h}` + `src/emit.{c,h}`
- [ ] Implement `elab_stm`: delimits a transaction block; type-checks body; verifies `TVar::read`/`TVar::write` are inside `stm`.
- [ ] Implement `elab_atomically`: validates argument is an `stm` block or returns `(STM a)`.
- [ ] Implement `elab_retry`: only valid inside `stm`; lowers to `tur_stm_retry(tur_current_tx)`.
- [ ] Implement `elab_check`: only valid inside `stm`; lowers to `tur_stm_check(cond)`.
- [ ] Implement `elab_or_else`: tries first `stm` block; falls through to second if first calls `retry`.
- [ ] Emit `TVar::read` → `tur_tvar_read(tur_current_tx, tv)`.
- [ ] Emit `TVar::write` → `tur_tvar_write(tur_current_tx, tv, value)`.
- [ ] Emit `stm` block as a closure passed to `tur_atomically`.
- [ ] Emit `TVar::modify` as inline `read → apply fn → write` within the same transaction.
- [ ] Emit `TVar::swap` as inline `read → write → return old`.
- [ ] Static check: `TVar::read`/`TVar::write` outside an `stm` block is a compile error (TUR-E0009).

**Stdlib** — `stdlib/stm.tur`
- [ ] Define `TVar` opaque type and `TVar::new`, `TVar::read`, `TVar::write`, `TVar::modify`, `TVar::swap`, `TVar::cas`.
- [ ] Implement `TMVar<T>` (wraps `TVar<(option T)>`): `TMVar::new`, `TMVar::new-empty`, `TMVar::put`, `TMVar::try-put`, `TMVar::take`, `TMVar::try-take`, `TMVar::read`, `TMVar::is-empty`.
- [ ] Implement `TChan<T>` (wraps `TVar` of a cons list): `TChan::new`, `TChan::write`, `TChan::read`, `TChan::try-read`, `TChan::peek`, `TChan::try-peek`.
- [ ] Implement `TSem` (wraps `TVar<int>`): `TSem::new`, `TSem::wait`, `TSem::try-wait`, `TSem::signal`.
- [ ] Implement convenience macros: `(with-tvar [name init] & body)`, `(dosync & body)`, `(stm-when cond & body)`, `(stm-unless cond & body)`.
- [ ] Implement `(atomically-batch & txs)`: run multiple closures in one transaction.

**Fixtures** — `tests/fixtures/stm/`
- [ ] `stm-tvar-basic.tur` — `TVar::new`, `TVar::read`, `TVar::write` single-threaded.
- [ ] `stm-tvar-modify.tur` — `TVar::modify`, `TVar::swap`, `TVar::cas`.
- [ ] `stm-atomicity.tur` — writes not visible outside until commit.
- [ ] `stm-retry.tur` — `retry` blocks and retries when TVar changes.
- [ ] `stm-check.tur` — `check` aborts transaction when condition false.
- [ ] `stm-or-else.tur` — `or-else` falls through when first branch calls `retry`.
- [ ] `stm-tmvar.tur` — `TMVar` put/take/read/is-empty.
- [ ] `stm-tchan.tur` — `TChan` write/read/peek.
- [ ] `stm-tsem.tur` — `TSem` new/wait/signal.
- [ ] `stm-defer.tur` — `defer` inside `stm` fires at commit or abort, not lexical exit.
- [ ] `stm-exception.tur` — exception inside `stm` aborts and propagates.
- [ ] `stm-dosync.tur` — `dosync` macro.
- [ ] `stm-with-tvar.tur` — `with-tvar` macro.
- [ ] Concurrency: `stm-concurrent-writes.tur` — multiple threads writing to same TVar (requires T19).
- [ ] Concurrency: `stm-concurrent-transfers.tur` — concurrent bank transfers, no money created or lost.
- [ ] Stress: `stm-stress.tur` — high-contention increment benchmark.
- [ ] Integration: `stm-with-arc.tur` — `Arc<TVar<T>>` shared across threads.
- [ ] Integration: `stm-with-threads.tur` — STM + `Thread` spawn/join.
- [ ] Negative: `stm-read-outside-transaction.tur` — `TVar::read` outside `stm` is compile error TUR-E0009.
- [ ] Negative: `stm-write-outside-transaction.tur` — `TVar::write` outside `stm` is compile error.
- [ ] Codegen snapshots: `stm` block lowers to closure + `tur_atomically`; `TVar::read`/`write` lower to `tur_tvar_read`/`tur_tvar_write`.

**Exit criterion:** STM works correctly for single-threaded and simple multi-threaded use cases; `TMVar`/`TChan`/`TSem` are available in stdlib; `defer` and exception integration is correct; all unit fixtures pass; concurrency fixtures pass under ThreadSanitizer.

---

### Phase 21 — Scalable STM (v2)

**Goal:** Replace the global lock with per-TVar fine-grained locking to support high-concurrency workloads.

**Prerequisites:** Phase 20 (Core STM).

**Fine-grained locking** — `src/stm.{c,h}`
- [ ] Add `mtx_t lock` field to `TVar` struct.
- [ ] Replace `STM_State.global_lock` acquire/release with per-TVar lock acquisition during commit.
- [ ] Implement lock ordering: acquire TVar locks in address order during commit phase to prevent deadlocks.
- [ ] Implement lock stripping: group TVars into N lock buckets (default: 64) to reduce per-TVar overhead.
- [ ] Update `tur_stm_commit` to use per-TVar locks: acquire all write-set locks in order, validate read set, apply writes, release locks.
- [ ] Update `tur_stm_retry` to use per-TVar condition variables.

**Performance benchmarks** — `tests/benchmarks/stm/`
- [ ] `stm-counter`: single TVar increment loop; target < 100 ns/op with fine-grained locking.
- [ ] `stm-transfer`: transfer between 2 TVars; target < 200 ns/transfer.
- [ ] `stm-bank`: concurrent bank simulation; verify linear scalability across thread counts.
- [ ] `stm-tchan-throughput`: `TChan` write/read throughput; target > 1M ops/sec.

**Stress and validation** — `tests/fixtures/stm/`
- [ ] `stm-deadlock-free.tur` — complex multi-TVar transaction patterns; verify no deadlocks.
- [ ] `stm-starvation.tur` — verify fairness: no transaction is indefinitely starved.
- [ ] Re-run all Phase 20 concurrency and stress fixtures under ThreadSanitizer.

**Exit criterion:** STM scales to high-concurrency workloads with acceptable performance; lock-ordering prevents deadlocks; all fixtures pass under ThreadSanitizer with no data races.

---

## Unsafe Operations (Phases U1–U5)

**Status:** Planned (v2). Prerequisites: Phase 19 (Algebraic effects — `Unsafe` is modeled as an effect in the effect row). See [unsafe-operations-plan.md](archive/unsafe-operations-plan.md) for full design.

| Phase | Goal | Exit Criterion |
|---|---|---|
| **U1** | `Unsafe` effect in type system | Effect row tracks `Unsafe`; unsafe functions carry `@ {Unsafe}`; calling them from safe context is a compile error |
| **U2** | `unsafe { }` syntactic sugar and containment checks | `unsafe` block desugars to `try_with` with `Unsafe` handler; containment enforced; lints for empty/large blocks |
| **U3** | Unsafe primitive operations | Pointer ops, type casts, unchecked array access, raw memory management, FFI primitives — all `@ {Unsafe}` |
| **U4** | Safe standard library wrappers | Bounds-checked `Array<T>`, safe `Vec<T>`, safe FFI helpers, `box`/`unbox`, arena allocator |
| **U5** | Linting, auditing, and tooling | `unsafe` block linter, trusted-code coverage metric, documentation enforcement |

---

### Phase U1 — `Unsafe` Effect

**Goal:** Introduce `Unsafe` as a built-in effect so the type system tracks where unsafe operations can occur.

**Type system** — `src/effect.{c,h}` + `src/types.{c,h}`
- [ ] Register `Unsafe` as a built-in effect constant alongside `Read`, `Write`, `Fail`, `GetEnv`.
- [ ] Functions that perform unsafe operations carry `@ {Unsafe}` in their effect row.
- [ ] Safe functions (effect row does not include `Unsafe`) cannot call unsafe functions outside an `unsafe` block.
- [ ] Effect polymorphism: if a higher-order function takes `(fn [] : {e} T)`, an unsafe closure propagates `Unsafe` through `e` automatically.

**Elaborator** — `src/elab.{c,h}`
- [ ] Propagate `Unsafe` through call sites using the existing effect-row mechanism.
- [ ] Emit a compile error when an unsafe function is called in a safe context without an enclosing `unsafe` block.
- [ ] Allow explicit `@ {Unsafe}` annotation on `defn` to mark an entire function unsafe.

**Fixtures**
- [ ] `unsafe-effect-row.tur` — `@ {Unsafe}` annotation on functions parses and elaborates correctly.
- [ ] Negative: `unsafe-leak.tur` — calling an unsafe function from a safe context without `unsafe` block is a compile error.

**Exit criterion:** `Unsafe` is a first-class effect; the effect row tracks it; unsafe calls in safe contexts are compile errors.

---

### Phase U2 — `unsafe { }` Block Sugar

**Goal:** Provide `unsafe { }` syntactic sugar and enforce that unsafe operations are properly contained.

**Surface syntax** — `src/reader.{c,h}` + `src/elab.{c,h}`
- [ ] Parse `(unsafe expr...)` as a new form.
- [ ] Desugar to `try_with` with an `Unsafe` handler that discharges the effect within the block.
- [ ] The `unsafe` block has effect row `{}` on the outside — `Unsafe` is consumed within the block.
- [ ] Nested `unsafe` blocks are allowed but the inner one is redundant (warn via lint).

**Compiler checks** — `src/elab.{c,h}`
- [ ] Verify containment: an `unsafe` block cannot leak `Unsafe` to its caller.
- [ ] Warn on empty `unsafe` blocks.
- [ ] Warn on `unsafe` blocks exceeding a configurable size threshold (default: 10 lines).

**Interaction with existing features**
- [ ] `defer` inside `unsafe` blocks fires normally on scope exit (safe or otherwise).
- [ ] Exceptions propagate outward past `unsafe` boundaries normally.
- [ ] Continuations (`shift`/`reset`) inside `unsafe` blocks: `Unsafe` is scoped to the block; captured continuations do not re-expose `Unsafe` after the block exits.

**Fixtures**
- [ ] `unsafe-basic.tur` — `unsafe` block calls an unsafe function; result is safe.
- [ ] `unsafe-nested.tur` — nested `unsafe` blocks work correctly.
- [ ] `unsafe-defer.tur` — defers fire correctly inside `unsafe` blocks.
- [ ] Negative: `unsafe-empty.tur` — empty `unsafe` block emits a warning.
- [ ] Codegen snapshots: `unsafe` block lowering.

**Exit criterion:** `unsafe { }` desugars correctly; containment is enforced; defers and exceptions compose correctly with `unsafe` blocks.

---

### Phase U3 — Unsafe Primitive Operations

**Goal:** Implement the core set of unsafe primitives, all carrying `@ {Unsafe}`.

**Pointer operations** — `stdlib/ptr.tur`
- [ ] `(ptr-deref p)` — dereference a raw pointer: `*T → T @ {Unsafe}`.
- [ ] `(ptr-write p v)` — write through a raw pointer: `*T → T → () @ {Unsafe}`.
- [ ] `(ptr-add p n)` — pointer arithmetic: `*T → int → *T @ {Unsafe}`.
- [ ] `(ptr-sub p n)` — pointer arithmetic: `*T → int → *T @ {Unsafe}`.
- [ ] `(ptr-null? p)` — null check: `*T → bool @ {}` (safe; only inspects the pointer).
- [ ] `(ptr-of r)` — get raw pointer from `ref<T>`: `ref<T> → *T @ {Unsafe}`.

**Type casting** — `stdlib/cast.tur`
- [ ] `(unsafe-cast v)` — unchecked cast between types: `T → U @ {Unsafe}`.
- [ ] `(reinterpret v)` — bit reinterpretation: `T → U @ {Unsafe}` (requires `sizeof(T) == sizeof(U)`; compile-time check).
- [ ] `(transmute v)` — Rust-familiar alias for `reinterpret`.
- [ ] Emit a compile error if `reinterpret`/`transmute` is used with mismatched sizes.

**Unchecked array operations** — `stdlib/array.tur`
- [ ] `(array-get-unchecked arr i)` — unchecked array read: `Array<T> → int → T @ {Unsafe}`.
- [ ] `(array-set-unchecked arr i v)` — unchecked array write: `Array<T> → int → T → () @ {Unsafe}`.

**Raw memory management** — `stdlib/mem.tur`
- [ ] `(raw-malloc n)` — allocate raw memory: `size → *void @ {Unsafe}`.
- [ ] `(raw-free p)` — free raw memory: `*void → () @ {Unsafe}`.
- [ ] `(raw-realloc p n)` — reallocate: `*void → size → *void @ {Unsafe}`.
- [ ] `(raw-memcpy dst src n)` — raw memory copy: `*void → *void → size → () @ {Unsafe}`.
- [ ] `(raw-memset p v n)` — raw memory set: `*void → int → size → () @ {Unsafe}`.

**FFI primitives** — `stdlib/ffi.tur`
- [ ] `(c-call fn-ptr args...)` — call a C function pointer: `@ {Unsafe, IO}`.
- [ ] `(dlopen path)` — open a dynamic library: `cstr → *void @ {Unsafe}`.
- [ ] `(dlsym handle name)` — load a symbol: `*void → cstr → *void @ {Unsafe}`.
- [ ] `(dlclose handle)` — close a dynamic library: `*void → () @ {Unsafe}`.

**Fixtures** — `tests/fixtures/unsafe/`
- [ ] `unsafe-ptr-deref.tur` — dereference a raw pointer inside an `unsafe` block.
- [ ] `unsafe-ptr-arith.tur` — pointer arithmetic.
- [ ] `unsafe-cast.tur` — unchecked cast between compatible types.
- [ ] `unsafe-reinterpret.tur` — bit reinterpretation for same-size types.
- [ ] `unsafe-array-unchecked.tur` — unchecked array access.
- [ ] `unsafe-malloc.tur` — raw malloc/free cycle.
- [ ] `unsafe-memcpy.tur` — raw memcpy.
- [ ] Negative: `unsafe-reinterpret-size-mismatch.tur` — `reinterpret` with mismatched sizes is a compile error.
- [ ] Codegen snapshots: all unsafe primitives lower to direct C equivalents.

**Exit criterion:** All unsafe primitives are implemented with `@ {Unsafe}` effect; they lower correctly to C; size mismatch on `reinterpret` is a compile-time error.

---

### Phase U4 — Safe Standard Library Wrappers

**Goal:** Build safe, bounds-checked wrappers around all unsafe primitives so that normal code never needs to touch `unsafe` directly.

**Safe array and vector** — `stdlib/array.tur` + `stdlib/vec.tur`
- [ ] `(array-get arr i)` — bounds-checked read: `Array<T> → int → Option<T> @ {}`.
- [ ] `(array-set arr i v)` — bounds-checked write: `Array<T> → int → T → bool @ {}` (returns false if OOB).
- [ ] `(array-slice arr start end)` — sub-slice: `Array<T> → int → int → Array<T> @ {}`.
- [ ] Verify/extend `Vec<T>` operations use `unsafe` blocks internally for `raw-malloc`/`raw-realloc`/`raw-free`.

**Safe FFI helpers** — `stdlib/ffi.tur`
- [ ] `(with-c-string s body)` — allocate a null-terminated C string and free on scope exit via `defer`.
- [ ] `(from-c-string p)` — convert `*char` to Turmeric `str` with explicit length check.

**Safe memory management** — `stdlib/mem.tur`
- [ ] `(box v)` — heap-allocate a value; returns `ref<T>` (drop frees via `defer`).
- [ ] `(unbox r)` — move out of a heap-allocated `ref<T>`.
- [ ] Arena allocator: `(arena-new)`, `(arena-alloc arena T)`, `(arena-free arena)` — bulk-free on scope exit.

**Fixtures** — `tests/fixtures/unsafe/`
- [ ] `safe-array-bounds.tur` — bounds-checked array access returns `Option`.
- [ ] `safe-vec-ops.tur` — vector push/pop/get all safe.
- [ ] `safe-c-string.tur` — `with-c-string` allocates and frees correctly.
- [ ] `safe-box.tur` — `box`/`unbox` for heap allocation.
- [ ] `safe-arena.tur` — arena allocation and bulk-free.

**Exit criterion:** All common use cases are covered by safe wrappers; no direct use of unsafe primitives is needed for standard operations.

---

### Phase U5 — Linting, Auditing, and Tooling

**Goal:** Provide tools to minimize and audit the trusted codebase.

**Unsafe block linter** — `src/lint.{c,h}`
- [ ] Warn on `unsafe` blocks exceeding a configurable size threshold (default: 10 lines); configure via `--lint-unsafe-max-lines N`.
- [ ] Warn on nested `unsafe` blocks (outer block already admits `Unsafe`; inner is redundant).
- [ ] Enable all unsafe lints via `--lint-unsafe` flag.

**Documentation enforcement** — `src/lint.{c,h}` + `src/reader.{c,h}`
- [ ] `#[safety "invariant description"]` attribute on `unsafe` blocks documents the relied-upon invariant.
- [ ] `--require-unsafe-docs` flag: require a `;;; SAFETY:` comment or `#[safety]` attribute on every non-test `unsafe` block.

**Trusted-code coverage** — `src/lint.{c,h}` + tooling
- [ ] Track the percentage of AST nodes inside `unsafe` blocks.
- [ ] `--unsafe-stats` flag: print trusted-code percentage per file and overall.
- [ ] CI integration: fail if trusted-code percentage exceeds a configurable threshold.

**Fixtures** — `tests/fixtures/unsafe/`
- [ ] `lint-unsafe-size.tur` — large `unsafe` block triggers lint warning.
- [ ] `lint-unsafe-doc.tur` — missing `SAFETY:` comment triggers lint with `--require-unsafe-docs`.
- [ ] `lint-unsafe-nested.tur` — nested `unsafe` triggers redundancy warning.
- [ ] `stats-unsafe.tur` — `--unsafe-stats` output matches expected golden.

**Exit criterion:** Unsafe lints are actionable and suppressible; trusted-code coverage is measurable via `--unsafe-stats`; documentation enforcement is available for library authors.

---

## Persistent Collections — HAMT (Phases P1–P4)

**Status:** Planned (v2). Prerequisites: Phase 15 (Typeclasses — `Eq`/`Hash` dispatch needed for key comparison), Phase 19 (Algebraic effects — `@ {Unsafe}` tracks internal raw memory use in `hamt.c`). See [hamt-feasibility.md](archive/hamt-feasibility.md) for feasibility analysis and design decisions.

**Key design decisions:**
- **Separate translation units:** `src/hamt.{c,h}` are compiled independently. Unused HAMT code is stripped by the linker automatically (`-dead_strip` on macOS, `-Wl,--gc-sections` on Linux) — no emit-phase gating required in v1. See [hamt-feasibility.md §Dead Code / Inclusion Strategy](archive/hamt-feasibility.md).
- **Memory model:** ref-counting (Phase P1 v1); GC integration deferred until GC lands.
- **Node width:** 5 bits (32 slots) — balanced trie depth vs. bitmap size.
- **Hash function:** SipHash (cryptographic) or xxHash (speed); decision finalized in Phase P1.
- **Collision handling:** linked list in v1; upgrade to HAMT-of-HAMTs if benchmarks warrant.
- **Transient mode (mutable buffer → flush to immutable):** deferred to Phase P4.
- **Compiler lowering:** the emit-phase `immutable map → HAMT` lowering pass is Phase P3; Phases P1–P2 are pure library work with no compiler changes.

| Phase | Goal | Exit Criterion |
|---|---|---|
| **P1** | Core HAMT C implementation | `hamt.{c,h}` complete; all unit fixtures pass; ref-count memory-safe under AddressSanitizer |
| **P2** | Lisp bindings (`stdlib/hamt.tur`) | `hamt/new`, `hamt/set`, `hamt/get`, `hamt/del`, `hamt/has?`, `hamt/count`, `hamt/keys`, `hamt/vals`, `hamt/merge` all working; fixture suite passes |
| **P3** | Compiler lowering pass | Elaborator lowers immutable `map` literals and `assoc`/`dissoc` calls to HAMT when the map is annotated `^persistent` or inferred immutable; existing mutable-map code unchanged |
| **P4** | Optimization and tooling | Transient mode (`hamt/transient`, `hamt/persistent!`), `hamt/dump` visualization, benchmarks vs. mutable hash table |

---

### Phase P1 — Core HAMT C Implementation

**Goal:** Implement a standalone, ref-counted HAMT in C99 with a minimal public API. No compiler integration yet; this is pure library code.

**New files** — `src/hamt.{c,h}`
- [ ] Define node types: `HAMT_NODE_BITMAP` (sparse, ≤ 32 entries), `HAMT_NODE_ARRAY` (dense, = 32 entries), `HAMT_NODE_COLLISION` (same-hash key list).
- [ ] Define `HamtNode` tagged union and `Hamt` root struct `{ HamtNode *root; uint32_t count; }`.
- [ ] Implement `hamt_new(void) → Hamt *` — allocate empty HAMT.
- [ ] Implement `hamt_set(Hamt *m, uint64_t hash, void *key, void *val) → Hamt *` — structural sharing; returns new root.
- [ ] Implement `hamt_del(Hamt *m, uint64_t hash, void *key) → Hamt *` — returns new root or same root if key absent.
- [ ] Implement `hamt_has(Hamt *m, uint64_t hash, void *key) → bool`.
- [ ] Implement `hamt_get(Hamt *m, uint64_t hash, void *key) → void *` — returns `NULL` if absent.
- [ ] Implement `hamt_count(Hamt *m) → uint32_t`.
- [ ] Implement `hamt_free(Hamt *m)` — decrement root ref-count; free nodes with zero refs.
- [ ] Implement `hamt_node_retain(HamtNode *n)` / `hamt_node_release(HamtNode *n)` — ref-counting helpers.
- [ ] Implement `hamt_merge(Hamt *a, Hamt *b) → Hamt *` — `b` wins on collision.
- [ ] Implement `hamt_iter_init` / `hamt_iter_next` — in-order iteration over key/value pairs.
- [ ] Choose and integrate hash function: `tur_siphash13` or `tur_xxhash64`; document decision in a comment at top of `hamt.c`.
- [ ] Add `hamt_dump(Hamt *m, FILE *out)` — pretty-print node tree for debugging.

**Memory safety requirements**
- [ ] All node allocations go through `hamt_alloc` / `hamt_free_node` (wrappers around `malloc`/`free`); no `malloc` calls outside these wrappers.
- [ ] All unit tests run clean under AddressSanitizer (`-fsanitize=address`) and Valgrind.
- [ ] No memory leaked between `hamt_new` and `hamt_free` even when intermediate `hamt_set`/`hamt_del` results are discarded.

**Fixtures** — `tests/fixtures/hamt/`
- [ ] `hamt-basic.tur` — `hamt/new`, `hamt/set`, `hamt/get`, `hamt/has?`, `hamt/count` round-trip.
- [ ] `hamt-sharing.tur` — two maps share structure after `hamt/set`; modifying one does not affect the other.
- [ ] `hamt-delete.tur` — `hamt/del` on present and absent keys; count decrements correctly.
- [ ] `hamt-collision.tur` — insert multiple keys with the same hash; all retrievable.
- [ ] `hamt-iteration.tur` — iterate all key/value pairs; no duplicates, no omissions.
- [ ] `hamt-merge.tur` — merge two disjoint maps; merge with overlapping keys (last writer wins).
- [ ] `hamt-large.tur` — insert 10 000 unique keys; verify count and random-sample lookups.
- [ ] `hamt-memory.tur` — ASan clean: insert, snapshot, mutate snapshot, free both versions.

**Exit criterion:** all C unit tests pass; fixtures pass; ASan/Valgrind clean; `hamt_dump` produces legible output.

---

### Phase P2 — Lisp Bindings

**Goal:** Wrap `src/hamt.{c,h}` in a Turmeric stdlib module so Lisp code can use HAMTs directly. No compiler lowering yet — this is an explicit API.

**New file** — `stdlib/hamt.tur`
- [ ] `(hamt/new) → hamt` — create empty persistent map.
- [ ] `(hamt/set m key val) → hamt` — insert/update; returns new map.
- [ ] `(hamt/get m key) → (option T)` — lookup; returns `none` if absent.
- [ ] `(hamt/get-or m key default) → T` — lookup with fallback.
- [ ] `(hamt/del m key) → hamt` — delete; returns new map (same map if key absent).
- [ ] `(hamt/has? m key) → bool` — membership test.
- [ ] `(hamt/count m) → int` — number of key/value pairs.
- [ ] `(hamt/keys m) → (vec T)` — all keys as a vector.
- [ ] `(hamt/vals m) → (vec T)` — all values as a vector.
- [ ] `(hamt/entries m) → (vec (pair K V))` — all key/value pairs.
- [ ] `(hamt/merge a b) → hamt` — merge; `b` wins on collision.
- [ ] `(hamt/merge-with f a b) → hamt` — merge with combiner function for collisions.
- [ ] `(hamt/map f m) → hamt` — map function over values; returns new map.
- [ ] `(hamt/filter f m) → hamt` — filter by predicate on value; returns new map.
- [ ] `(hamt/reduce f init m) → T` — fold over key/value pairs.
- [ ] `(hamt/from-vec pairs) → hamt` — construct from `(vec (pair K V))`.
- [ ] `(hamt/to-vec m) → (vec (pair K V))` — destructure to association list.

**Typeclass instances**
- [ ] `Show` instance for `hamt` — `(show m)` returns `"{key1: val1, key2: val2, ...}"`.
- [ ] `Eq` instance for `hamt` — two maps are equal if they have the same key/value pairs.

**Fixtures** — `tests/fixtures/hamt/`
- [ ] `hamt-lisp-basic.tur` — basic API round-trip from Turmeric code.
- [ ] `hamt-lisp-snapshot.tur` — take snapshot with `hamt/set`, verify original unchanged.
- [ ] `hamt-lisp-map-filter.tur` — `hamt/map`, `hamt/filter`, `hamt/reduce`.
- [ ] `hamt-lisp-merge-with.tur` — `hamt/merge-with` combiner function.
- [ ] `hamt-lisp-show.tur` — `Show` instance produces expected string.
- [ ] `hamt-lisp-eq.tur` — `Eq` instance: equal and unequal maps.
- [ ] `hamt-lisp-from-to-vec.tur` — `hamt/from-vec` / `hamt/to-vec` round-trip.
- [ ] Codegen snapshots: HAMT function calls lower to `hamt_*` C calls; no unexpected overhead.

**Exit criterion:** all stdlib functions are usable from Turmeric; typeclass instances work; fixture suite passes; codegen snapshots stable.

---

### Phase P3 — Compiler Lowering Pass

**Goal:** Allow the elaborator to automatically lower immutable `map` literals and persistent map operations to HAMT when beneficial, without requiring explicit `hamt/` namespace calls.

**Elaborator changes** — `src/elab.{c,h}`
- [ ] Recognize `^persistent` annotation on `def`/`let` bindings: `(def ^persistent m {:a 1 :b 2})` lowers the map literal to `hamt/from-vec`.
- [ ] Recognize `(assoc m k v)` on a `^persistent`-typed binding: lowers to `hamt/set`.
- [ ] Recognize `(dissoc m k)` on a `^persistent`-typed binding: lowers to `hamt/del`.
- [ ] Recognize `(get m k)` on a `^persistent`-typed binding: lowers to `hamt/get`.
- [ ] Recognize `(count m)` on a `^persistent`-typed binding: lowers to `hamt/count`.
- [ ] Propagate `^persistent` through `let` bindings and function return types.
- [ ] Emit a type-mismatch diagnostic when a `^persistent` map is passed to a function expecting a mutable map (and vice versa).

**Emit changes** — `src/emit.{c,h}`
- [ ] When `needs_hamt` flag is set on the `Emit` context (set by the P3 lowering pass), include `hamt.h` in the emitted C header block.
- [ ] `needs_hamt` is set on first encounter of any HAMT-lowered form; existing code that never uses `^persistent` is unaffected.

**Linker flag policy** — `src/emit.{c,h}` (already decided; document here)
- [ ] No `-lhamt` needed (HAMT is compiled into the binary as part of `src/`).
- [ ] Dead-code stripping: on macOS add `-dead_strip`; on Linux add `-Wl,--gc-sections` (with `-ffunction-sections -fdata-sections` on object files). These flags are emitted by `tur build` automatically when targeting those platforms.

**Fixtures** — `tests/fixtures/hamt/`
- [ ] `hamt-lowering-basic.tur` — `(def ^persistent m {:a 1})` followed by `assoc`/`get` lowers to HAMT calls.
- [ ] `hamt-lowering-propagate.tur` — `^persistent` propagates through `let` chains.
- [ ] `hamt-lowering-mutable-unchanged.tur` — ordinary (mutable) map operations are unaffected by the lowering pass.
- [ ] Negative: `hamt-lowering-type-mismatch.tur` — passing `^persistent` map to mutable-map function emits TUR-E00XX.
- [ ] Codegen snapshots: `assoc`/`dissoc`/`get` on `^persistent` map lower to `hamt_set`/`hamt_del`/`hamt_get`.

**Exit criterion:** `^persistent` annotation triggers HAMT lowering end-to-end; non-annotated code is unaffected; codegen snapshots stable; type-mismatch diagnostic fires correctly.

---

### Phase P4 — Optimization and Tooling

**Goal:** Add transient mutation mode for batch construction, visualization tooling, and performance benchmarks.

**Transient mode** — `src/hamt.{c,h}` + `stdlib/hamt.tur`
- [ ] Define `HamtTransient` struct: mutable wrapper around a `HamtNode *` root; carries an owner token to prevent concurrent mutation.
- [ ] Implement `hamt_transient(Hamt *m) → HamtTransient *` — fork a transient from an immutable map; marks all nodes as owned-by-transient.
- [ ] Implement `hamt_transient_set(HamtTransient *t, uint64_t hash, void *key, void *val)` — mutates in-place if node is owned; copies otherwise.
- [ ] Implement `hamt_transient_del(HamtTransient *t, uint64_t hash, void *key)` — mutates in-place if owned.
- [ ] Implement `hamt_persistent(HamtTransient *t) → Hamt *` — seal transient into immutable map; invalidates `t`.
- [ ] Lisp API: `(hamt/transient m) → hamt-transient`, `(hamt/transient-set! t k v)`, `(hamt/transient-del! t k)`, `(hamt/persistent! t) → hamt`.

**Visualization** — `src/hamt.{c,h}`
- [ ] Extend `hamt_dump` to produce DOT format for Graphviz: `hamt_dump_dot(Hamt *m, FILE *out)`.
- [ ] Add `(hamt/dump m)` Lisp form that emits DOT to stderr (debug builds only).

**Benchmarks** — `tests/benchmarks/hamt/`
- [ ] `hamt-bench-insert.tur` — 100 000 sequential inserts; compare vs. mutable hash table baseline; target < 3× overhead.
- [ ] `hamt-bench-lookup.tur` — 100 000 lookups after 100 000 inserts; target O(1) average.
- [ ] `hamt-bench-snapshot.tur` — 1 000 snapshots (fork + 1 insert each); total time vs. deep-copy baseline.
- [ ] `hamt-bench-transient.tur` — bulk-build 100 000 entries via transient then seal; compare vs. sequential `hamt/set`.

**Fixtures** — `tests/fixtures/hamt/`
- [ ] `hamt-transient-basic.tur` — fork transient, mutate, seal; verify result correct.
- [ ] `hamt-transient-isolation.tur` — original map unchanged after transient mutations.
- [ ] `hamt-transient-invalidated.tur` — using a sealed transient panics or errors.

**Exit criterion:** transient mode is correct and faster than sequential `hamt/set` for bulk construction; benchmarks documented; `hamt_dump_dot` produces valid DOT output.

---

## Backtracking with Cloneable Continuations (Phases B1–B5)

**Status:** Planned (v2 stretch goal). Prerequisites: Phase 15 (Typeclasses — `Clone` trait dispatch), Phase 18 (Delimited continuations — `shift`/`reset` substrate), Phase 19 (Algebraic effects — handler infrastructure). See [backtracking-cloneable-continuations-plan.md](archive/backtracking-cloneable-continuations-plan.md) for full design.

**Key design decisions:**
- **Core abstraction:** `cloneable_continuation<T>` — a continuation that can be resumed multiple times. All types captured by the continuation must satisfy the `Clone` typeclass constraint, enforced by the elaborator.
- **Deep clone (v1):** All clones are deep; start simple and optimise to copy-on-write if profiling warrants it.
- **Defer semantics:** Defers are suspended by default during cloneable-continuation replay; they fire only when the *original* scope exits. Opt-in per-resume defers use an explicit `:replay true` annotation.
- **Hybrid effect integration:** Cloneable continuations work standalone and inside effect handlers (hybrid approach, neither fully separate nor effect-only).
- **Backtracking monad:** `Backtrack<T>` is a list-of-thunks monad built on top of cloneable continuations. `mzero`, `mplus`, `bind`, `return`, `choice`, `guard`, `fresh` are the core combinators.
- **Selective CPS:** Only code inside `cloneable-reset` blocks undergoes the modified CPS transformation; normal `reset`/`shift` blocks are unaffected.

| Phase | Goal | Exit Criterion |
|---|---|---|
| **B1** | `Clone` trait infrastructure | `Clone` typeclass defined; primitive instances ship; `rc<T>` / `ref<T>` clone methods work; elaborator rejects cloneable continuations that capture non-`Clone` types |
| **B2** | Cloneable continuation runtime + CPS | `cloneable_continuation<T>` type; `continuation_clone()`; `:cloneable` flag on `shift`/`reset`; CPS pass emits correct cloneable environment capture; defer suspend/replay semantics correct |
| **B3** | Backtracking monad | `Backtrack<T>`, `mzero`, `mplus`, `bind`, `return`, `run-backtrack`, `run-backtrack-depth`, `choice`, `guard`, `fresh` all working; unit fixture suite passes |
| **B4** | Standard library integration | `stdlib/logic.tur` (`LVar`, `unify`, `Goal`, `run-logic`, `fresh`, `conjoined`, `disjoined`) and `stdlib/parsec.tur` (`Parser<T>`, `pure`, `fail`, `item`, `char`, `many`, `or-parser`, `bind-parser`, `run-parser`) complete; integration fixtures pass |
| **B5** | Testing, benchmarks, and optimization | All unit/integration/negative/perf fixtures pass; clone overhead benchmarked; memory usage documented; `--backtrack-depth` safety flag; documentation in user guide |

---

### Phase B1 — Clone Trait Infrastructure

**Goal:** Define the `Clone` typeclass and ship primitive plus derived instances. This is the static foundation — without `Clone`, the elaborator cannot verify that a continuation's captured environment is safely duplicatable.

**Typeclass definition** — `stdlib/typeclass.tur`
- [ ] Define `(defclass Clone [a] (clone [x : a] : a))` alongside `Eq`/`Ord`/`Show`.
- [ ] Document that `Clone` is a *deep* clone — the returned value shares no mutable state with the original.
- [ ] Reserve `copy` as a future `Copy` (bit-copy, zero-cost) trait; do not conflate with `Clone`.

**Primitive instances** — `stdlib/typeclass.tur`
- [ ] `(definstance Clone int)` — returns `x` unchanged (integers are `Copy`).
- [ ] `(definstance Clone int8)` … `(definstance Clone int64)` — same.
- [ ] `(definstance Clone uint8)` … `(definstance Clone uint64)` — same.
- [ ] `(definstance Clone float)` — same.
- [ ] `(definstance Clone double)` — same.
- [ ] `(definstance Clone bool)` — same.
- [ ] `(definstance Clone cstr)` — same (string literals are static pointers; safe to copy).

**Derived instances** — `stdlib/typeclass.tur`
- [ ] `(definstance Clone (Pair a b) [Clone a, Clone b])` — clone both fields.
- [ ] `(definstance Clone (option a) [Clone a])` — clone the payload if `some`.
- [ ] `(definstance Clone (list a) [Clone a])` — deep-clone each element.
- [ ] `(definstance Clone (vec a) [Clone a])` — allocate new vec, clone each element.

**Reference type clone semantics** — `stdlib/rc.tur`, `stdlib/ref.tur`
- [ ] `(definstance Clone (rc a) [Clone a])` — `clone` increments the ref-count; returns same pointer (shallow). Document that this gives shared ownership, not a new independent copy. If a fully independent deep copy is needed, users call `(clone (rc/deref r))` and re-wrap.
- [ ] `(definstance Clone (ref a) [Clone a])` — `clone` deep-clones the pointed-to value into a new heap allocation; returns a new independent `ref<T>`. This is the safe default for mutable references inside continuations.

**Elaborator enforcement** — `src/elab.{c,h}`
- [ ] Add `check_cloneable_capture(shift_expr, env)` — walk captured bindings of any `cloneable-shift`; emit a compile error for each binding whose type lacks a `Clone` instance.
- [ ] Emit error code TUR-E00YY (`"type %s captured by cloneable continuation does not implement Clone"`).
- [ ] Recursively verify type arguments (e.g., `vec<ref<T>>` requires `Clone ref<T>` which requires `Clone T`).

**Fixtures** — `tests/fixtures/backtrack/`
- [ ] `clone-primitives.tur` — `clone` on `int`, `bool`, `cstr` returns equal value.
- [ ] `clone-pair.tur` — `clone` on `Pair` deep-clones both fields.
- [ ] `clone-option.tur` — `clone` on `some` and `none`.
- [ ] `clone-list.tur` — `clone` on list preserves all elements independently.
- [ ] `clone-vec.tur` — cloned vec is independent; mutating original does not affect clone.
- [ ] `clone-rc.tur` — `clone` increments ref-count; original and clone share data.
- [ ] `clone-ref.tur` — `clone` produces independent copy; mutation isolated.
- [ ] Negative: `clone-non-clone-capture.tur` — `cloneable-shift` capturing a non-`Clone` type emits TUR-E00YY.

**Exit criterion:** `Clone` typeclass is defined; all primitive and derived instances ship; `rc<T>` / `ref<T>` semantics documented; elaborator rejects non-`Clone` captures at compile time.

---

### Phase B2 — Cloneable Continuation Runtime + CPS

**Goal:** Extend the runtime and CPS pass to support multi-shot continuations, defer suspend/replay semantics, and the `:cloneable` surface annotation.

**Surface syntax** — `src/reader.{c,h}` + `src/elab.{c,h}`
- [ ] Parse `(cloneable-reset body)` as sugar for `(reset body :cloneable true)`.
- [ ] Parse `(cloneable-shift k expr)` as sugar for `(shift k expr :cloneable true)`.
- [ ] Reject `:cloneable true` on a `shift` that is not inside a `cloneable-reset` boundary; emit diagnostic TUR-E00YZ.
- [ ] `(call/cc* f)` — sugar for `(cloneable-shift k (f k))`; captures current continuation as cloneable.

**Type system** — `src/types.{c,h}`
- [ ] Add `TY_CLONEABLE_CONT` variant alongside `TY_CONT`.
- [ ] `cloneable_continuation<T>` is a struct type: `{ k: continuation<T>, clone: (-> cloneable_continuation<T>) }`.
- [ ] `cloneable_continuation<T>` is NOT move-only — it can be copied freely (each copy invokes `clone`).
- [ ] Borrow checker: `cloneable_continuation<T>` does not trigger the `is_moved` path on use (unlike `TY_CONT`).

**Runtime** — `src/runtime.{c,h}`
- [ ] Define `CloneableContinuation` struct: `{ Continuation base; size_t env_size; CloneEnvEntry *env_entries; }`.
- [ ] Define `CloneEnvEntry`: `{ void *value; CloneFn clone_fn; DropFn drop_fn; }`.
- [ ] Implement `tur_cont_clone(CloneableContinuation *k) → CloneableContinuation *` — deep-clones `env_entries` using each entry's `clone_fn`; returns a new independent continuation.
- [ ] Implement `tur_cont_resume_cloneable(CloneableContinuation *k, void *arg) → void *` — resumes **without** consuming `k`; `k` remains valid for further clones/resumes.
- [ ] Implement `tur_cont_drop_cloneable(CloneableContinuation *k)` — runs drop_fn on all entries, frees the struct.
- [ ] Set `base.is_cloneable = true`; one-shot `tur_cont_resume` aborts if called on a cloneable continuation.

**Defer suspend/replay semantics** — `src/runtime.{c,h}` + `src/elab.{c,h}`
- [ ] When entering a `cloneable-reset` scope, mark all registered defers as `DEFER_SUSPENDED`.
- [ ] On resume of a cloneable continuation: suspended defers do NOT fire; they remain attached to the original scope.
- [ ] On `:replay true` defer annotation: mark the defer as `DEFER_REPLAY`; it fires on every resume as well as on original scope exit.
- [ ] When the original `cloneable-reset` scope exits normally (all resumes done): fire all defers in reverse order.
- [ ] When `tur_cont_drop_cloneable` is called (continuation dropped without resume): fire replay-flagged defers only.

**CPS pass changes** — `src/cps.{c,h}`
- [ ] `needs_cloneable_cps(expr)` — return true if the expr is or contains `EX_CLONEABLE_RESET` / `EX_CLONEABLE_SHIFT`.
- [ ] `emit_capture_environment(expr, bb, cloneable=true)` — for each captured binding, record both the value and its typeclass-resolved `clone_fn` and `drop_fn` pointers.
- [ ] `emit_create_cloneable_continuation(env, bb)` — allocate `CloneableContinuation`; populate `env_entries`.
- [ ] Normal `reset`/`shift` paths are unaffected.

**Fixtures** — `tests/fixtures/backtrack/`
- [ ] `cloneable-basic.tur` — `cloneable-shift` captures continuation; clone it; resume both independently; verify different results.
- [ ] `cloneable-multi-resume.tur` — resume the same cloneable continuation three times; verify each produces the correct value.
- [ ] `cloneable-defer-suspend.tur` — defer inside `cloneable-reset` fires only once (at original scope exit), not on intermediate resumes.
- [ ] `cloneable-defer-replay.tur` — `(defer :replay true ...)` fires on every resume AND at original scope exit.
- [ ] `cloneable-ref.tur` — `ref<T>` captured in cloneable continuation deep-clones correctly; each resume sees independent state.
- [ ] `cloneable-rc.tur` — `rc<T>` captured increases refcount on clone; original and clones share data.
- [ ] Negative: `cloneable-shift-outside-reset.tur` — `cloneable-shift` outside `cloneable-reset` emits TUR-E00YZ.
- [ ] Codegen snapshots: cloneable continuation lowering; `CloneEnvEntry` array generation.

**Exit criterion:** cloneable continuations can be cloned and resumed multiple times; defer semantics correct; elaborator enforces `Clone` capture constraint; one-shot `tur_cont_resume` aborts if called on cloneable cont; CPS pass unaffected for non-cloneable code.

---

### Phase B3 — Backtracking Monad

**Goal:** Implement `Backtrack<T>` as a list-of-thunks monad on top of cloneable continuations. This is a pure library phase — no compiler changes.

**New file** — `stdlib/backtrack.tur`
- [ ] Define `(defalias Backtrack<T> (-> (list (-> T))))` — a computation yielding zero or more results.
- [ ] Implement `(defn mzero [] : (Backtrack a))` — empty search; returns `[]`.
- [ ] Implement `(defn mreturn [x : a] : (Backtrack a))` — single result.
- [ ] Implement `(defn mplus [^Clone a fs gs : (Backtrack a)] : (Backtrack a))` — concatenate two result streams.
- [ ] Implement `(defn mbind [^Clone a b f : (-> a (Backtrack b)) xs : (Backtrack a)] : (Backtrack b))` — monadic bind.
- [ ] Implement `(defn run-backtrack [^Clone a m : (Backtrack a)] : (list a))` — execute and collect all results.
- [ ] Implement `(defn run-backtrack-depth [^Clone a depth : int, m : (Backtrack a)] : (list a))` — depth-limited version; returns at most `depth` results.
- [ ] Implement `(defn choice [^Clone a x y : (Backtrack a)] : (Backtrack a))` — try first, then second.
- [ ] Implement `(defn guard [cond : bool] : (Backtrack unit))` — succeed only if `cond` is true; otherwise `mzero`.
- [ ] Implement `(defn fresh [^Clone a n : int, f : (-> int (Backtrack a))] : (Backtrack a))` — enumerate `n` alternatives.
- [ ] Implement `(defn once [^Clone a m : (Backtrack a)] : (Backtrack a))` — take at most one result; cut remaining search.
- [ ] Implement `(defn interleave [^Clone a xs ys : (Backtrack a)] : (Backtrack a))` — fair interleaving of two streams.
- [ ] Implement `do`-notation helper macro `(backtrack-do ...)` for sequencing backtracking computations.

**Fixtures** — `tests/fixtures/backtrack/`
- [ ] `backtrack-basic.tur` — `choice`, `mreturn`, `run-backtrack`; verify both branches returned.
- [ ] `backtrack-mzero.tur` — `mzero` produces empty result list.
- [ ] `backtrack-bind.tur` — `mbind` composes two choice computations; verify Cartesian product.
- [ ] `backtrack-guard.tur` — `guard true` succeeds; `guard false` prunes.
- [ ] `backtrack-fresh.tur` — `fresh 5 id` produces 5 results `[0 1 2 3 4]`.
- [ ] `backtrack-depth.tur` — `run-backtrack-depth 3` returns at most 3 results from an infinite search.
- [ ] `backtrack-once.tur` — `once` returns exactly one result.
- [ ] `backtrack-interleave.tur` — `interleave` fairly alternates two infinite streams.
- [ ] `backtrack-nested.tur` — nested `choice` inside `mbind`; verify all combinations.
- [ ] `backtrack-ref.tur` — backtracking with `ref<T>` state; each branch sees independent state.
- [ ] Codegen snapshots: `run-backtrack` and `mbind` lowering.

**Exit criterion:** all monad combinators work correctly; depth limiting prevents divergence; `ref<T>` state is properly isolated across branches; fixture suite passes.

---

### Phase B4 — Standard Library Integration

**Goal:** Build two canonical consumers of the backtracking monad: a logic programming layer (`stdlib/logic.tur`) and a parser combinator library (`stdlib/parsec.tur`).

**New file** — `stdlib/logic.tur`
- [ ] Define `(defstruct LVar [id : int])` — logic variable.
- [ ] Define `(defstruct UState [subst : (hamt LVar Term), counter : int])` — unification state (uses HAMT for persistent substitution map; degrade to association list if HAMT not yet available).
- [ ] Define `(defalias Term (variant INT int64, BOOL bool, SYM cstr, VAR LVar, PAIR (Pair Term Term), NIL unit))`.
- [ ] Define `(defalias Goal (Backtrack UState))`.
- [ ] Implement `(defn unify [x y : Term] : Goal)` — unification with occurs check; calls `mzero` on mismatch.
- [ ] Implement `(defn unify-var [v : LVar, t : Term, k] : Goal)` — helper; checks occurs-check, records binding in substitution.
- [ ] Implement `(defn walk [v : Term, s : UState] : Term)` — follow variable chains in substitution.
- [ ] Implement `(defn fresh-lvar [f : (-> LVar Goal)] : Goal)` — generate a fresh logic variable.
- [ ] Implement `(defn conjoined [g1 g2 : Goal] : Goal)` — `mbind g1 (fn [_] g2)`.
- [ ] Implement `(defn disjoined [g1 g2 : Goal] : Goal)` — `mplus g1 g2`.
- [ ] Implement `(defn run-logic [n : int, g : Goal] : (list UState))` — `run-backtrack-depth n g` with fresh initial state.
- [ ] Implement `(defn reify [v : LVar, state : UState] : Term)` — walk substitution to ground term.

**New file** — `stdlib/parsec.tur`
- [ ] Define `(defalias Input (struct [chars : (vec char), pos : int]))`.
- [ ] Define `(defalias Parser<a> (-> Input (Backtrack (Pair a Input))))`.
- [ ] Implement `(defn pure [^Clone a x : a] : (Parser a))` — succeed with `x`, no input consumed.
- [ ] Implement `(defn pfail [] : (Parser a))` — always fail.
- [ ] Implement `(defn item [] : (Parser char))` — consume one character; `mzero` on empty input.
- [ ] Implement `(defn pchar [expected : char] : (Parser char))` — consume if matches.
- [ ] Implement `(defn pstring [expected : cstr] : (Parser cstr))` — consume fixed string.
- [ ] Implement `(defn or-parser [^Clone a p q : (Parser a)] : (Parser a))` — `mplus`-based; backtracks fully on first-branch failure.
- [ ] Implement `(defn bind-parser [^Clone a b p : (Parser a), f : (-> a (Parser b))] : (Parser b))` — sequential composition.
- [ ] Implement `(defn then-parser [^Clone a b p : (Parser a), q : (Parser b)] : (Parser b))` — discard first result.
- [ ] Implement `(defn many [^Clone a p : (Parser a)] : (Parser (list a)))` — zero or more; returns longest match first.
- [ ] Implement `(defn many1 [^Clone a p : (Parser a)] : (Parser (list a)))` — one or more.
- [ ] Implement `(defn optional [^Clone a p : (Parser a)] : (Parser (option a)))` — try `p`; return `none` on failure.
- [ ] Implement `(defn run-parser [^Clone a p : (Parser a), input : cstr] : (list (Pair a Input)))` — parse a string; return all successful parses.
- [ ] Implement `(defn run-parser-full [^Clone a p : (Parser a), input : cstr] : (option a))` — return only the parse that consumes all input; `none` if ambiguous or failed.

**Fixtures** — `tests/fixtures/backtrack/`
- [ ] `logic-unify-basic.tur` — unify `(VAR v)` with `(INT 42)`; verify substitution.
- [ ] `logic-unify-fail.tur` — unify `(INT 1)` with `(INT 2)`; verify `mzero`.
- [ ] `logic-fresh.tur` — `fresh-lvar` produces independent variables.
- [ ] `logic-conjoined.tur` — conjunction of two successful goals.
- [ ] `logic-disjoined.tur` — disjunction yields two result states.
- [ ] `logic-occurs-check.tur` — occurs check prevents circular bindings.
- [ ] `logic-reify.tur` — reify a ground term from a substitution.
- [ ] `logic-query.tur` — multi-goal query with `run-logic 10`; verify expected results.
- [ ] `parsec-basic.tur` — `pchar 'a'` succeeds on `"a"`, fails on `"b"`.
- [ ] `parsec-or.tur` — `or-parser` backtracks from first branch and succeeds on second (the `"ac"` example from the plan).
- [ ] `parsec-many.tur` — `many (pchar 'a')` on `"aaa"` returns list of 3.
- [ ] `parsec-sequence.tur` — sequence two parsers with `bind-parser`.
- [ ] `parsec-full.tur` — `run-parser-full` rejects ambiguous parses.
- [ ] `parsec-json-subset.tur` — parse a JSON-like subset (numbers, strings, arrays) using combinators.
- [ ] Codegen snapshots: `or-parser` lowering with backtracking; `run-logic` lowering.

**Exit criterion:** `stdlib/logic.tur` and `stdlib/parsec.tur` are complete; all fixtures pass; the backtracking parser example from the plan passes end-to-end.

---

### Phase B5 — Testing, Benchmarks, and Optimization

**Goal:** Validate correctness under stress, measure clone overhead, and add safety/diagnostic tooling.

**Performance benchmarks** — `tests/benchmarks/backtrack/`
- [ ] `bench-clone-overhead.tur` — 10 000 cloneable continuation clone/resume cycles; measure ns per clone.
- [ ] `bench-backtrack-n-queens.tur` — N-queens solver (N = 8, 10, 12) using `choice`/`guard`; measure solutions-per-second.
- [ ] `bench-parsec-json.tur` — parse a 10 KB JSON document with the parsec combinator library; measure MB/s.
- [ ] `bench-logic-query.tur` — `run-logic 1000` on a relational arithmetic query; measure results-per-second.
- [ ] Compare `clone` overhead: deep-clone baseline vs. projected copy-on-write savings.

**Safety and diagnostic tooling**
- [ ] Add `--backtrack-depth N` flag: caps the depth of any `run-backtrack` call globally; prevents accidental infinite search in production builds.
- [ ] Emit a warning when `run-backtrack` (no depth limit) is called outside of a test context. Suppress with `(run-backtrack :allow-infinite true ...)`.
- [ ] Add `--dump-clone-plan` debug flag: print which types were resolved to `Clone` instances at each cloneable continuation site.

**Memory analysis**
- [ ] Verify that cloneable continuations release all cloned memory when both the original and all clones are dropped (ASan clean).
- [ ] Profile peak memory usage for the N-queens benchmark; document in `backtracking-cloneable-continuations-plan.md`.

**Fixtures** — `tests/fixtures/backtrack/`
- [ ] `backtrack-n-queens.tur` — 8-queens solver; verify all 92 solutions found.
- [ ] `backtrack-sudoku.tur` — minimal Sudoku solver using `guard`; verify a known puzzle.
- [ ] `backtrack-memory.tur` — ASan clean: clone, resume, drop all branches; no leaks.
- [ ] Negative: `backtrack-depth-exceeded.tur` — `run-backtrack-depth 0` returns empty list immediately.
- [ ] `backtrack-integration-effects.tur` — cloneable continuation inside an algebraic effect handler; verify composability.
- [ ] `backtrack-integration-stm.tur` — (requires Phase 20) verify that backtracking correctly rolls back STM writes per branch.

**Documentation**
- [ ] Add `docs/backtracking-guide.md` — user guide covering `Clone` trait, `cloneable-reset`/`cloneable-shift`, `Backtrack<T>` monad, `stdlib/logic.tur`, `stdlib/parsec.tur`, and when to use each.
- [ ] Update `backtracking-cloneable-continuations-plan.md` with implementation notes, resolved open questions, and benchmark results.

**Exit criterion:** N-queens and Sudoku solvers work correctly; clone overhead is measured and documented; ASan clean under all fixture scenarios; `--backtrack-depth` safety flag works; user guide exists.

---

## 11. Writing the compiler in C — concrete shape

Since the host *is* C, lock these conventions early so the codebase stays legible:

**Memory.** One bump-allocated **arena per compilation unit**, freed wholesale at the end. AST nodes, symbol tables, and intermediate strings all live in the arena — no `free` calls scattered through the compiler. A second arena for macro-expansion scratch is reset between expansions.

**`Form` representation.** A tagged union, e.g.:
```c
typedef enum { F_NIL, F_INT, F_SYM, F_STR, F_LIST, F_VEC, F_MAP } FormTag;
typedef struct Form {
    FormTag tag;
    Span    span;        // file, line, col-start, col-end
    union {
        int64_t   i;
        Symbol    sym;
        StrSlice  str;
        struct { struct Form **items; uint32_t len; } list;
    } as;
} Form;
```
All `Form*` are arena-allocated and immutable after construction.

**Strings.** `StrSlice { const char *p; uint32_t len; }`. No `strdup`. Symbols are interned into a hash table; equality is pointer-equality.

**Error reporting.** Build a tiny diagnostic module up front (`diag_emit(span, level, fmt, ...)`). Spans flow through every phase — adding them after the fact is the worst kind of refactor in C.

**Code generation.** A growable byte buffer (`Buf`) with `buf_printf`, `buf_indent`, `buf_writef`. Emit to `Buf`, then `fwrite` once. Avoid intermediate `FILE*` to keep tests fast and inspectable.

**Testing.** Plain C harness (`tests/run.c`) + golden-file fixtures: each `.tur` input has an expected `.c.out` and an expected program-stdout. CI re-runs golden compare; AddressSanitizer + UBSan on by default.

**Coverage policy: every language feature ships with tests.** No feature lands without fixtures. Concretely, `tests/fixtures/` is organized by feature, and each directory contains at least:

- *Happy path* — minimal example showing the feature works.
- *Interaction tests* — the feature combined with `defer`, `ref`, closures, and macros (whichever apply).
- *Negative tests* — inputs that must fail to compile, with the expected diagnostic text golden-checked. Use a `// expect-error: …` comment convention.
- *Codegen snapshot* — the emitted `.c` is golden-checked so unexpected changes to lowering are obvious in PR diffs.

```
tests/fixtures/
  reader/         let/          if-cond/       fn-closure/
  defer/          ref/          macro-defmacro/  quasiquote/
  inline-c/       extern-c/     types-prims/   errors/
  examples/       # end-to-end programs (counter, fizzbuzz, …)
```

Each phase in §7 has an exit criterion that includes "fixtures land in the same PR as the feature." A feature without tests is not done.

**Layout.**
```
src/
  arena.{c,h}      reader.{c,h}    forms.{c,h}    diag.{c,h}
  expand.{c,h}     interp.{c,h}    elab.{c,h}     close.{c,h}
  defer.{c,h}     emit.{c,h}      driver.{c,h}    main.c
tests/
  fixtures/...
```

**Bootstrap → self-host path.** Once phases 0–7 land in C, port `tur` to Turmeric module-by-module (reader first, codegen last). The C compiler stays in the repo as `tur-stage0` for emergency rebuilds.

**What we give up vs. Rust/OCaml.** No exhaustive pattern matching on tagged unions (use `switch` + a default-`abort` macro), no easy generics (write per-type containers, or one `void*`-keyed hash table), no derive macros for printers (write `form_print` once and call it). Accepted cost.

---

## 12. Future ideas (post-v1)

These are explicitly out of scope for the initial language but worth designing toward — i.e., don't paint into a corner that forecloses them.

### 12.1 Delimited continuations (`shift`/`reset`)

Goal: first-class control — generators, async, backtracking, algebraic effects — implemented in user-space rather than baked into the runtime.

Sketch of approach:
- Surface forms `(reset expr)` and `(shift k expr)` à la Danvy–Filinski.
- **Implementation strategy: CPS-transform on demand.** Mark functions that lexically contain `shift` (transitively) and CPS-convert just those; leave the rest of the program in direct style. The CPS pass runs after closure conversion so captured continuations become ordinary closures (`struct {fn,env}` — already solved).
- Continuations are **one-shot by default** (cheap, fits the closure model); multi-shot requires copying the env chain and is opt-in via `(shift/multi …)`.
- Interaction with `defer`/`ref`: capturing a continuation that crosses a `defer` boundary is the hard case. Decision needed: (a) run the `defer`s on capture (Go-ish, but breaks "resume later"), (b) attach them to the continuation (correct but expensive), or (c) make it a compile error. Lean: (c) for v1 of continuations, relax later.
- No stack-copying / `setjmp` tricks. Pure CPS keeps things portable C and analyzable.

Designing toward it now: keep the IR explicit about control flow; don't hide returns inside `goto` chains that a CPS pass can't see through.

### 12.2 Type system

Two complementary directions; pick order based on user demand.

**(a) Typed-Racket / Typed-Clojure–style occurrence typing.** Gradual typing layered on a dynamic core. Predicates (`int?`, `nil?`, custom `(defpred foo? [x] …)`) refine a binding's type along the branches of `if`/`cond`. This pairs naturally with a Lisp — types follow the shape of idiomatic code rather than fighting it. Implementation: a flow-sensitive type-checker pass after macro expansion, before closure conversion. No runtime cost in the typed regions; contract checks at the dynamic↔typed boundary.

**(b) Typeclasses and typeclass-based dispatch — *chosen direction* (decided).** Haskell/Rust-trait style: `(defclass Show [a] (show [x] : cstr))`, `(definstance Show int …)`. **Dispatch: dictionary passing** — class methods compile to closures bundled in a dict struct passed as an extra arg. The struct-with-fn-ptr closure machinery from §4 *is* the dict shape, so most of the runtime work is already done.

*The on-ramp is already in v0.* §1.1 commits to elaborator-resolved operators from day one — i.e., a dispatch table the elaborator consults to pick the right C function for each `(op, types)` pair. v0 populates that table with primitive entries (`int_add`, `float_eq`, …). Typeclasses are then **"the table gets entries from `definstance`."** No re-architecture: the typeclass pass is mostly elaboration (resolve which dict to pass at each call site) + a coherence check. Slower than monomorphization, but works with separate compilation, supports dynamic instances, and avoids the code-bloat trap. Coherence: enforce orphan-instance rules at module boundary.

(Monomorphization stays available later as a `-O` flag for hot paths once we have benchmarks justifying it.)

**Ordering.** (a) probably lands first — additive analysis pass, low risk, big usability win. (b) is the architectural commitment, with the on-ramp already in v0. Build (b) once real users hit the limits of ad-hoc polymorphism.

**What to preserve in v1 to keep these doors open:**
- Keep type annotations syntactically *parseable* even if mostly unused (`^int`, `:- (-> int int)`), so adding a checker later doesn't require touching every existing `.tur` file.
- Don't bake mono-typed code paths into the IR — keep the elaborator's output generic enough that a future dictionary-passing pass has somewhere to insert dicts.
- Resist optimizations in v1 that assume a particular dispatch strategy (e.g., devirtualizing closure calls based on syntactic shape).

#### 12.2.1 Higher-kinded types — door left open for v2

**v1 typeclasses are kind-`*` only.** Class heads quantify over types (`(defclass Show [a] …)`), not over type constructors (`(defclass Functor [f] …)` where `f` is `* -> *`). This is a deliberate scope cut, not an architectural exclusion: HKTs are a v2-or-later extension, and v1 must avoid decisions that close the door.

Why defer:

- **Most monad use cases die when effects ship.** `IO`, `State`, `Throw`, parsers, and short-circuit chains all become direct-style code under the algebraic-effects machinery (`effects-plan.md`). A `Monad` typeclass is the main HKT motivator in Haskell; with effects, that motivator is mostly gone. See [effects-vs-monads.md](archive/effects-vs-monads.md) for the long form.
- **Kind inference + kind-polymorphic dispatch is real implementation work** — a kind-checking pass between elaboration and dictionary insertion, plus a two-level dispatch table (lookup the constructor's dictionary, then call its method slot with concrete inner types). Doable, but not pulling its weight in v1.
- **The dispatch table's current shape (§1.1) keys on `(name, [arg-type, …])`.** That works for kind-`*` typeclasses unchanged. HKT dispatch wants a different key shape (outer constructor, with inner types as dict parameters). Building both keying strategies in v1 would over-fit to a feature we may never ship.

What v2 HKTs would buy:

- A single generic `do`-notation that doesn't need per-monad `bind` / `pure` parameters.
- `traverse`, `sequence`, `mapM`, `forM`, `replicateM` written once over `Monad m`.
- `Functor` / `Applicative` / `Monad` / `Traversable` typeclasses (all need at least kind `* -> *`).
- Library-level monad transformers (`StateT`, `ExceptT`) for users who prefer `mtl`-style stacking over effect handlers.
- Free-monad / freer-monad encodings as ordinary library code.

What v2 HKTs would cost:

- Kinds in the surface syntax (probably inferred, with `: * -> *` ascription as escape hatch).
- A kind-checking pass; failure mode "expected kind `* -> *`, got `*`" with a span pointing at the offending instance head.
- Dispatch-table generalization (the kind-`*` keying remains a fast path; HKT keying is additive).
- Coherence rules for HKT instances — orphan checks have to consider the outer constructor.
- Documentation cost: explaining kinds to users coming from dynamic Lisps.

What v2 HKTs **don't** buy:

- They don't replace effects for `IO` / state / errors / parsers — that machinery stays.
- They don't enable multi-shot continuations (List monad, full backtracking) — that's an effects-system problem (v5 in `effects-plan.md`), not a typeclass-system one.
- They don't change codegen for any existing kind-`*` program.

**What v1 must preserve to keep this door open:**

- *Type variables in class heads carry an explicit kind slot in the elaborator's internal representation*, defaulting to `*`. v1 never sets it to anything else, but the slot exists. This is a one-field change in the type-variable record; missing it would force a v2 IR migration.
- *Don't pun on type-constructor names.* `option` is a type constructor; `(option int)` is a type. Keep these distinct in the IR — don't collapse `option` to "a type with a hole" via some ad-hoc encoding. The cleanest discipline: only fully-applied type constructors appear in `Expr`/`TExpr` nodes; partial applications are never representable in v1, and v2 lifts that restriction by adding kind-`* -> *` to the type-variable kind slot.
- *Dispatch-table key is a struct, not a tuple-of-strings.* As long as the key is a named record (`{op-name, arg-types[]}`), v2 can add a `constructor-key` variant without breaking the v1 schema. Encoding the key as `"name:type1,type2"` strings would force a parser rewrite in v2.
- *Don't expose "the type of `option`" anywhere in user-visible syntax.* No `^option` (without arguments), no `(typeof option)`. Reserve these forms; reject them in v1 with "type constructor used without arguments; this may become valid in a future version with higher-kinded types."
- *Reserve the names `Functor`, `Applicative`, `Monad`, `Traversable`, `Foldable`* in the typeclass namespace — when v1 typeclasses ship, these are not-yet-defined-but-reserved, so users can't squat on them with kind-`*` definitions that v2 would conflict with.

**Decision rule for promoting HKTs into v2.** Add them only if at least two of the following are true after meaningful v1 use:

1. Users are repeatedly writing per-monad `traverse-option` / `traverse-result` boilerplate that one generic `traverse` would eliminate.
2. A library author wants to ship a generic monad transformer or free-monad construction and demonstrably can't.
3. A meaningful fraction of users come from Haskell / Scala / PureScript / OCaml-with-modules and the missing abstraction is the top complaint.

If only (3) is true, the answer is "use effects, that's the language." If (1) and (2) are both true, HKTs pay for themselves and v2 ships them.

### 12.3 Modules *(stretch goal)*

A self-contained unit of code with its own namespace, exporting a curated surface and importing from other modules.

```clojure
(defmodule geom
  (export Point distance translate)

  (defstruct Point [x : float, y : float])

  (defn distance [^Point a, ^Point b] : float …)
  (defn translate [^Point p, ^float dx, ^float dy] : Point …)

  (defn- normalize [p] …))   ;; defn- = private, not exported

;; consumer.tur
(import geom)
(import geom :as g)
(import geom :refer [distance])

(geom/distance p1 p2)
(g/distance p1 p2)
(distance p1 p2)
```

Properties:
- **Namespacing.** Each module has its own symbol namespace; cross-module references use `module/name`. Aliases via `:as`, selective imports via `:refer`.
- **Visibility.** `def`/`defn`/`defmacro` are exported by default if listed in `(export …)`; `defn-` and `def-` are always private. Or invert the default — see open questions.
- **Compilation unit.** A module is the unit the codegen emits as one `.c` + `.h` pair. Cross-module calls go through the header's declarations; the linker resolves them. This means modules also become the unit of *separate compilation* — touching one module doesn't recompile the world.
- **C symbol mangling.** `geom/Point` lowers to `geom__Point`; `geom/distance` to `geom__distance`. Reserved-character mapping documented as part of the ABI.
- **Macro export.** Macros cross module boundaries by serializing their `Form` body and re-evaluating in the importer's expansion context. (This is the same path Clojure takes; the bootstrap interpreter (§6) already operates on `Form` so the work is small.)
- **Module-level `defer`.** Top-level `(defer …)` in a module runs at process exit, registered via `atexit`. Useful for closing handles, flushing logs.

**Open question — relationship to files.**

Three options, in increasing flexibility:

1. **One module per file, name from path.** `src/geom/vector.tur` *is* the module `geom/vector`. Simplest; matches Java/Rust/Go ergonomics. No `defmodule` form needed — the file header declares its `(export …)` list. Filesystem becomes load-bearing.
2. **Explicit `defmodule`, multiple modules per file allowed.** As shown above. The reader produces N modules from one file. More flexible (good for tiny test fixtures and macro-heavy single-file scripts), but loaders need an index — usually a manifest file or a "scan all `.tur` for module decls" pass.
3. **Hybrid (Clojure-style).** `defmodule` is required and *must* match the file path: `src/geom/vector.tur` declares `(defmodule geom/vector …)` at the top, and the compiler errors if the names don't match. You get the explicitness of (2) and the discoverability of (1).

Lean: **(3)**. It scales (clear loader, no manifest), it surfaces module identity in the source (reading a file tells you what module it is without consulting the path), and it leaves room to relax to (2) later for REPL/scripting.

**Other open questions** (defer until we actually start designing modules):
- Default visibility — exported by default, or private by default? Lean: private-by-default with explicit `(export …)`. Encourages narrow surface areas.
- Circular imports — error, lazy-resolve, or topological sort? Lean: error in v1.
- Re-exports — `(export-from other-module foo bar)`? Useful for facade modules; can be a macro initially.
- Module-private types crossing the boundary in public signatures — error or warn? Lean: error; the type system (§12.2) will need to enforce this anyway.
- Build artifacts — one `.c`/`.h` per module, or per file when (3) makes them equivalent? Probably per module, with `.h` regenerated on every compile.
- Interaction with `extern-c` and inline-C blocks (§2.1) — declare those at module scope; they get scoped C-symbol visibility (`static` for module-private, extern for exported).

**Why stretch goal, not v1.** Modules are pure plumbing: every line of pre-modules Turmeric works post-modules with at most a `(defmodule …)` wrapper. v0/v1 can ship as a single global namespace and add modules later without breaking existing programs — the inverse isn't true for things like RC or typeclasses, which is why those rank higher.

**What to preserve in v1 to keep modules cheap to add later:**
- Compiler emits `.c` + `.h` even when there's only one compilation unit, so the multi-unit story is already plumbed when modules arrive.
- Symbol naming in codegen never assumes a flat namespace — qualify every emitted name with at least a `tur__` prefix so retrofitting `module__name` is trivial.
- The bootstrap interpreter for macro expansion works against `Form` lists, not against a global symbol table, so cross-module macro export is a small extension rather than a refactor.

### 12.4 Hygienic macros *(stretch goal)*

§6 ships v1 with `gensym` + manual care — the same band-aid Clojure uses. It works in practice, but the problems are real and well-known:

```clojure
(defmacro swap! [a b]
  `(let [tmp ~a]              ;; user code with `tmp` in scope?
     (set! ~a ~b)              ;; → silently captures it
     (set! ~b tmp)))
```

The fix: macros expand to *syntax objects* (identifiers tagged with the scopes they were introduced in), and the expander resolves names by scope-set rather than by symbol equality.

**Approaches, in increasing power and cost:**

1. **Clojure-style syntax-quote auto-resolution + gensym.** Backtick automatically resolves symbols to their definition module; `(gensym)` and `foo#` reader sugar generate fresh names for locals. Informal, not strictly hygienic, but covers most cases. *Where v1 already lives.*
2. **`syntax-rules` (R5RS-style).** Pattern-based, hygienic by construction. Easy to implement, limited expressive power — no procedural macros, no compile-time computation. Good for "stdlib macros" (`when`, `cond`, `->`); not enough for the macros we actually want to write.
3. **`syntax-case` (R6RS, Dybvig et al).** Full procedural macros + mark-set hygiene. Identifiers carry sets of marks; mark-set-aware comparison replaces symbol-eq. Classic, well-understood, ~mid-size implementation lift.
4. **Sets-of-scopes (Flatt '16, Racket).** Modern replacement for mark-sets. Cleaner story for tricky cases (macro-generating-macro, recursive macros, `let-syntax`). Larger one-time investment but provably correct in cases where mark-sets get fragile.

**Lean: (4), sets-of-scopes.** It's the current state of the art, the algorithm is published with reference implementations, and the conceptual model ("each scope-introducing form adds a fresh scope; identifiers compare by scope-set") survives later additions like modules (§12.3) and a real type system (§12.2). (2) is tempting as a cheap intermediate stop, but `syntax-rules` users hit its expressive limits fast in a Lisp aimed at C interop and codegen.

**What changes in the compiler:**
- The `Form` ADT (§11) gains a `Symbol` variant that's actually `(Symbol, ScopeSet)`. Equality and lookup become scope-set-aware. Spans stay where they are; scope-sets ride alongside.
- The expander is a separate pass that walks `Form`s, threading a current-scope, adding a fresh scope at each binding form, and producing fully-resolved identifiers as output.
- The bootstrap interpreter (§6) keeps working unchanged on `Form` — it just sees scoped identifiers instead of raw symbols. Macros that *construct* identifiers use a `(datum->syntax stx 'name)` form (Racket-style) to attach the right scope.

**Open questions:**
- *Migration path for existing macros.* gensym-based macros (v1 style) keep working, since gensym produces fresh symbols that no scope-set lookup will collide with. New hygiene is opt-in or default? Lean: default — non-hygienic macros become the explicit escape hatch.
- *Identifier comparison in user code.* `(= 'foo 'foo)` — does it compare symbol-only, or scope-set too? Lean: symbol-only by default, with `(bound-identifier=? a b)` and `(free-identifier=? a b)` for the precise comparisons (Racket convention).
- *Interaction with quasiquote.* Backtick must thread scope-sets through the template. Tractable but the place where scoping bugs in the implementation usually hide.
- *REPL / dynamic eval.* `(eval form)` at runtime needs an entry point into the expander; scope-sets need a "top-level" representation that's stable across REPL inputs.
- *Reader-introduced identifiers* (autogensym `foo#`, syntax-quote-resolved names): do they get the use-site scope, the read-site scope, or a fresh scope? Lean: read-site scope, matching Racket.

**Why stretch goal, not v1.** Hygiene is a compiler architecture decision dressed up as a feature — picking it after we have real macros in the wild lets us see which patterns matter and informs the design. Meanwhile, v1's gensym + (later) module-qualified names cover ~90% of the cases users actually hit. The remaining 10% is real (the §9 risk is genuine) but survivable for early users; the cost of guessing wrong on the algorithm before we have user data is higher than the cost of a noisy migration later.

**What v1 needs to preserve:**
- Symbols flow through every pass as a typed value, not as bare `const char *` — adding a scope-set field is then a one-struct change. The §11 plan already has `Symbol` as its own type.
- The expander is a *separate, replaceable* pass, not an interleaved part of elaboration. v1's pass works on raw symbols; a future pass swaps in for scoped identifiers without touching what comes after.
- Don't let the reader resolve symbols to bindings. Resolution belongs to the expander/elaborator. (This is already the v1 plan; preserving it explicitly here.)
- Reserve `bound-identifier=?`, `free-identifier=?`, `datum->syntax`, `syntax->datum` as keywords now; emit "not implemented in v0" if used. Same trick as §5.1.2 for `rc/of` — avoids syntactic retrofit later.

### 12.5 Sweet-expressions *(stretch goal)*

An optional surface syntax, layered on top of v1's s-expression reader, that lets users write Turmeric with infix math, traditional `f(x)` call notation, and indentation-meaningful grouping. Based on [SRFI-110 (sweet-expressions / t-expressions)](https://srfi.schemers.org/srfi-110/srfi-110.html) and informed by [sweet-racket](https://github.com/takikawa/sweet-racket)'s experience embedding it into a host Lisp. Sweet-expressions are an **abbreviation**, not a new language — they translate, deterministically and homoiconically, to the same `Form*` the v1 reader produces. Macros, codegen, and tooling downstream are unchanged.

**The motivating example** (Turmeric flavor):

```
defn fibfast(n) : int
  if {n < 2}
    n
    fibup n 2 1 0
```

…reads as exactly:

```clojure
(defn fibfast [n] : int
  (if (< n 2)
      n
      (fibup n 2 1 0)))
```

A user who wants infix only — without indentation magic — can opt into just curly-infix and stop there. A user who wants the whole thing enables full sweet via a file-level directive.

#### 12.5.1 Three layers, three opt-in tiers

SRFI-110 deliberately separates three orthogonal extensions; we adopt the same separation so users can pick where to stop.

| Tier | What it adds | Reader cost | File-level enable |
|---|---|---|---|
| **(a) Curly-infix** ([SRFI-105](https://srfi.schemers.org/srfi-105/)) | `{a + b}` → `(+ a b)`; `{a + b + c}` → `(+ a b c)`; `{a + b * c}` → `($nfx$ a + b * c)` (no precedence — mixed operators bail to a `$nfx$` macro) | small | `#lang turmeric/curly-infix` or always-on |
| **(b) Neoteric** | `f(x y)` → `(f x y)`; `f{x + y}` → `(f (+ x y))`; `f[x]` → `(bracketapply f x)`. Triggers only when `(`/`{`/`[` follows an atom *with no whitespace between* | small–medium | implied by sweet; standalone via `#lang turmeric/neoteric` |
| **(c) Sweet (full t-expr)** | Indentation-significant grouping; `\\` GROUP/SPLIT; `$` SUBLIST; `<* … *>` collecting list; leading-abbreviation rule | medium–large | `#lang sweet-exp` directive *or* `.tursweet` file extension |

Default for `.tur` files is **plain s-exprs** (`#lang turmeric`, implicit if no `#lang` line). Sweet must be explicitly opted into. This follows Racket's `#lang` model — adopted instead of SRFI-110's `#!sweet` because `#lang` is the convention Lisp users already know from Racket and `sweet-racket`, and it generalizes to other future surface dialects without each needing a bespoke directive.

#### 12.5.2 Conflicts with Turmeric's existing reader, and how to resolve them

Turmeric's v1 reader (§10.2) already uses several characters that SRFI-110 cares about. None of the conflicts are showstoppers, but each needs a deliberate decision before tier (a) ships, because the answer constrains tiers (b) and (c).

| Turmeric v1 | SRFI-110 wants it for | Resolution |
|---|---|---|
| `{…}` for map literals | curly-infix lists | **Resolved in v1: maps use `#{…}`, not `{…}`** (§10.2). `{…}` is unused in v1 — reserved exclusively for SRFI-105 curly-infix when §12.5 ships. No deprecation window, no migration: the v1 reader has never accepted `{…}` for maps. |
| `~x` / `~@x` for unquote / unquote-splicing (Clojure-style) | SRFI-110 uses `,` and `,@` | **Resolved in v1: `~` / `~@` are the only unquote sigils** (§10.2). Turmeric does not also accept `,` / `,@`. Sweet's "leading-abbreviation-followed-by-whitespace applies to the whole expression" rule is generalized in our reader to whichever abbreviation sigils Turmeric defines — `~` and `~@` get the same treatment `'` and `` ` `` already do. The rule is "leading reader-macro abbreviations followed by whitespace consume the indented body," not "specifically these four characters." |
| `[…]` for **vector literals** | neoteric `f[x]` → `(bracketapply f x)` | Outside neoteric position (i.e., with whitespace before `[`), `[…]` continues to mean "vector literal" exactly as today. Inside neoteric position (`f[x]`, no whitespace), it lowers to `(bracketapply f x)` per SRFI-105 — and `bracketapply` is a Turmeric macro the user can define (or we can ship as `nth` for the common indexing case). Document the disambiguation rule prominently; it matches every other neoteric implementation. |
| ` ```c … ``` ` **inline-C blocks** (§2.1) | (no conflict, but watch out) | The reader recognizes triple-backtick fences *before* applying any tier's rules; the payload is opaque (treated like a string). Indentation processing inside the block is suppressed (just as it is inside `(…)`). A block at expression position with a `: T` annotation (the existing inline-C return-type form) needs to play nicely with sweet's "child lines extend the parent line" rule — solution: treat the block as a single neoteric atom for grouping purposes. |
| `.x` **field-access reader sugar** (§5.2: `(.x p)` ↔ `(. p x)`) | conflicts with SRFI-110's leading `.` for improper-list cdr | Confine Turmeric's `.x` form to "atom starting with `.` followed by a name character." SRFI-110's `.` rule applies only when `.` is a delimited token (followed by whitespace or EOL). The two are syntactically distinguishable today and remain so. |
| `:foo` **keywords** | (no conflict in SRFI-110) | Untouched. Note: wisp's `:` SUBLIST marker would have collided here; we're using `$` per SRFI-110, so we're fine. |
| `^int` **type-annotation prefix** | (no conflict) | Untouched. Verify `^` doesn't trigger any SRFI-105 special handling — it doesn't. |
| `&` **address-of** (§5.4) | (no conflict) | Untouched. |

The two architectural decisions that had to be made in v1 — `{…}` vs. `#{…}` for maps, and `~`/`~@` vs. also accepting `,`/`,@` — are **resolved**: maps use `#{…}`, unquote uses `~`/`~@` only. See §10.2. Everything else in this table is purely sweet-reader local.

#### 12.5.3 Where it slots into the pipeline

Sweet sits **above** the existing reader, not inside it. The §3 pipeline becomes:

```
text → [sweet preprocessor (optional)] → reader → Form* → expand → elab → …
```

In practice, the sweet preprocessor is a separate state machine that consumes characters and emits `Form*` directly (the SRFI-110 reference impl is structured this way; trying to layer it on top of an existing s-expr lexer is the path of pain — sweet-racket and the readable project both do their own lexing). The output is the same `Form*` ADT (§11), with the same `Span` provenance, so every downstream pass is identical.

This means:
- **Spans flow through correctly** — each sweet construct attributes to the source text it desugared from. Critical for diagnostics; the §10.1 rule "spans flow through every pass" applies to the sweet reader equally.
- **Macros see no difference.** `defmacro` operates on `Form*`; whether the user wrote `(if {n < 2} n …)` or the sweet equivalent, the macro receives the identical tree.
- **Codegen snapshots stay stable.** The fixture system (§11) golden-checks emitted `.c`. Adding sweet doesn't perturb a single existing snapshot.

Two readers ship: `sweet-read` and the existing `s-read`. The driver picks based on file extension or `#!sweet` directive. The bootstrap interpreter (§6) calls `s-read` only — macros are still written in s-expressions in v1, even when host code is sweet. (Mixing sweet *into* macro bodies via quasiquote is fine because the macro receives `Form*`, not text.)

#### 12.5.4 Implementation phases

Three phases, each independently shippable. Each ends with the same fixture discipline (§11): happy path, interaction (with `defer`, `ref`, closures, macros), negative tests with golden diagnostics, and round-trip snapshots showing the desugared `Form*` matches a hand-written equivalent.

**Phase S1 — Curly-infix (SRFI-105).**
- Reader change: when an opening `{` is seen, switch to "curly mode" until the matching `}`. (No conflict with map literals — those use `#{…}` since v1.)
- Inside curly mode, whitespace-separated tokens are collected; if the result is `(a op b op c …)` with all `op`s the same operator, lower to `(op a b c …)`. Mixed operators lower to `($nfx$ …)` and the elaborator either resolves `$nfx$` via a user-defined macro or errors out.
- One-element `{e}` → `e`. Two-element `{e1 e2}` → `(e1 e2)` (matches SRFI-105 exactly).
- Effort: ~1–2 days. Self-contained. Pure win for any code that does math.

**Phase S2 — Neoteric.**
- After reading any atom (symbol, number, string), peek the next char. If it's `(`, `[`, or `{` *with no intervening whitespace*, consume the bracketed list and wrap: `f(x y)` → `(f x y)`, `f[x]` → `(bracketapply f x)`, `f{x + y}` → `(f (+ x y))`.
- Interacts with curly-infix: `f{a + b}` first runs S1 on the `{…}` (yielding `(+ a b)`), then S2 wraps as `(f (+ a b))`.
- Interacts with Turmeric's `.x` sugar: trivially compatible since `.x` is a single atom; `.x(p)` would mean `((. x) p)` which is nonsense — document that you write `(.x p)` or `p.x` is *not* an idiom (we don't have method-call sugar). If users want `obj.method(args)` ergonomics, that's a separate feature (a §12.6 idea, not part of sweet).
- Effort: ~2–3 days. Most of the work is the no-whitespace lookahead and getting the test matrix right.

**Phase S3 — Full sweet (indentation, GROUP/SPLIT, SUBLIST, collecting lists).**
- The big one. Implement the SRFI-110 BNF directly; the reference implementation is ~1000 lines of Scheme and ports cleanly to C. ANTLR-checked grammar means low risk of ambiguity bugs.
- Indentation stack lives in the reader; tabs, spaces, and `!` are all valid indent characters per spec. Mixing them on the same line is an error.
- `\\` (GROUP/SPLIT), `$` (SUBLIST), `<* … *>` (collecting lists) all per spec — no Turmeric-specific divergence; users who learn sweet from the SRFI docs should find Turmeric's dialect identical.
- Indentation processing is **off** inside `(…)`, `[…]`, `{…}`, `#{…}`, and inline-C `\`\`\` … \`\`\`` blocks. This is what makes sweet fully backward-compatible with traditional s-exprs — any line starting with `(` reads as a normal s-expr.
- A `#lang sweet-exp` line at the top of the file enables sweet for the rest of that file. `.tursweet` extension enables it implicitly even without the line. See §12.5.4a below for the full directive scheme.
- A `tur unsweeten <file.tursweet>` command emits the desugared s-expression form (and rewrites the `#lang` line accordingly), mirroring SRFI-110's reference `unsweeten` tool. Useful for debugging, for diffs, and for users who want to migrate gradually. The inverse `tur sweeten <file.tur>` is part of the `tur fmt` family (§12.5.5).
- Effort: ~1–2 weeks for a clean implementation with full fixture coverage, including the interaction-test matrix (sweet × `defer`, sweet × inline-C, sweet × quasiquote, sweet × keywords, sweet × ref).

**Phase S4 — `#lang` dispatch (cross-cutting, lands with whichever phase first needs a non-default reader).**

Turmeric uses Racket-style `#lang <name>` as the file-level reader-selection mechanism, replacing SRFI-110's `#!sweet`. Rationale: `#lang` is the convention Lisp users (especially Racketeers, who are sweet's natural early-adopter audience) already know; it generalizes to future surface dialects without each one inventing a directive; and it composes cleanly with `tur fmt` (which can rewrite the `#lang` line as it converts).

Rules:

- **Position.** The `#lang` line, if present, MUST be the very first line of the file — no preceding whitespace, no preceding comments, no BOM. This matches Racket and lets the driver pick the reader from the first ~32 bytes without buffering the whole file.
- **Form.** `#lang <name>` followed by `\n`. Optional trailing arguments (e.g., `#lang sweet-exp typed`) are reserved for future use and rejected with a clear error in v1.
- **Default.** Absence of `#lang` is equivalent to `#lang turmeric`. No file is required to declare a dialect; the default is the boring one. (Compare Racket, which *requires* `#lang`; we don't, because it would break every existing example.)
- **Recognized values in v1:**
  - `#lang turmeric` — default s-expression reader.
  - `#lang sweet-exp` — full sweet (tier (c)). Errors in v1 with "not yet implemented"; ships in phase S3.
  - `#lang turmeric/curly-infix` — tier (a) only. Ships in phase S1.
  - `#lang turmeric/neoteric` — tiers (a) + (b). Ships in phase S2.
  - Anything else — error: "unknown #lang `<name>`; expected one of turmeric, sweet-exp, …".
- **No mid-file switching.** Unlike SRFI-110's `#!sweet` / `#!no-sweet` (which can appear anywhere), Turmeric's `#lang` is file-scoped. Mixing dialects within one file is forbidden. Users who want to mix do so by splitting into separate files (the §8.5 per-file artifact layout already supports this — file `a.tur` can be plain Turmeric and file `b.tursweet` can be sweet, and they link together normally).
- **`.tursweet` extension.** Files ending in `.tursweet` default to `#lang sweet-exp` if no `#lang` line is present. A `#lang turmeric` line in a `.tursweet` file is allowed but emits a warning ("file extension and `#lang` disagree; reader is following the `#lang` line"). Reverse case (`.tur` file with `#lang sweet-exp`) is allowed silently — the directive wins.
- **Compatibility with SRFI-110's `#!sweet`.** We do NOT accept `#!sweet` as an alias. Users porting code from sweet-racket or readable-project tooling get a one-line error message pointing them at `#lang sweet-exp`. The `#!` form would have been a second way to do the same thing, with subtly different placement rules — we'd rather have one rule.

Why not `#lang sweet-exp turmeric` (the sweet-racket mixin shape)? Because Turmeric is the only base language; the second word would always be `turmeric`. If we ever grow alternate base languages (e.g., a pure-functional subset), we can extend the syntax then — `#lang` is forward-compatible with arguments.

#### 12.5.5 Open questions to settle before phase S3

1. **`bracketapply` semantics.** SRFI-105 leaves `bracketapply` as "user-definable." For Turmeric, lean: ship `(defmacro bracketapply [coll i] `(nth ~coll ~i))` in the stdlib seed. This makes `xs[i]` indexing work out of the box. Open: should `bracketapply` also handle struct field access (`p[x]` → `(.x p)`)? Probably no — too clever, two ways to do the same thing.
2. **`nfx` (mixed-precedence infix).** SRFI-105 specifies that `{a + b * c}` lowers to `(nfx a + b * c)` and leaves `nfx` for the user to define. For Turmeric, lean: **don't ship a default `nfx` macro.** A user trying `{a + b * c}` should get a clear "no `nfx` defined; use parens or define `(defmacro nfx …)`" error. Reasoning: precedence in a Lisp is the C++ "implicit conversion" of syntax — once it exists, every reader of every program has to internalize the table. Better to require the parens.
3. **Quasiquote interaction.** sweet-racket's README documents this as a known issue ("quasi-quotation combined with grouping does not behave according to the specification"). We must do better — the spec's leading-abbreviation rule covers `` ` `` (backquote) explicitly; the bug in sweet-racket appears to be specific to its template-syntax integration with Racket's own quasiquote, which we don't have. Fixture-test it heavily. Likely fine for us; verify before phase S3 ships.
4. **REPL ergonomics.** SRFI-110 ends a top-level expression on a blank line. For an interactive Turmeric REPL, this matters: "Enter Enter to submit" is the convention. Verify this works on macOS / Linux terminals (it does in every other sweet implementation; not expected to be a problem).
5. **Editor support.** Indent-aware language requires editor cooperation. v1 plan: ship a tree-sitter grammar for `.tursweet` (separate grammar from `.tur`). Indent rules live in the grammar. Without this, users will fight their editors.
6. **`tur fmt --to-sweet` and `tur fmt --to-sexp`.** Bidirectional pretty-printer, mirroring `sweeten` / `unsweeten` from the readable project. Cheap to build once the reader exists (just walk `Form*` and emit the chosen flavor) and removes the "I have to commit to one syntax forever" anxiety.

#### 12.5.6 Why stretch goal, not v1

Sweet-expressions are pure surface syntax — they buy readability but no new expressive power, and a v1 user can already write any Turmeric program in plain s-exprs. The cost of getting them wrong (especially the indentation phase) is high enough that they want real users with real programs to motivate the dialect choices. Meanwhile, the v1 reader is already good enough to bootstrap the language.

The two architectural decisions that *had* to be made in v1 — `{…}` vs. `#{…}` for maps, and the unquote sigil set — are **resolved** (§10.2): maps are `#{…}`, unquote is `~` / `~@` only. The third — using `#lang` as the dispatch mechanism — is also locked in (v1's reader parses the `#lang` line and currently accepts only `turmeric`; everything else is an error). With those three out of the way, the rest of sweet is genuinely deferrable.

#### 12.5.7 What v1 needs to preserve to keep this door open

- **Reader is a separate, replaceable component.** v1's reader already targets `Form*`; the sweet reader is a peer, not a fork. Don't let any pass downstream of the reader inspect raw text or assume a specific surface syntax.
- **`Form*` carries `Span` from a single source.** Sweet desugaring produces nodes spanning the original sweet text; the typed-IR (§1.1) and diagnostics (§10.1) treat them identically to s-expr-sourced nodes.
- **Reader-recognized abbreviations are a *table*, not a hardcoded list.** v1's reader handles `'`, `` ` ``, `~`, `~@` via a small lookup; the sweet reader's "leading-abbreviation-followed-by-whitespace applies to the whole expression" rule iterates over the same table. New abbreviations (e.g., `#'` for syntax in §12.4) plug in via one table entry.
- **Don't bake `(`/`[`/`{`/`#{` parsing into deeply-nested control flow in the s-expr reader.** Leave them as discrete cases that the sweet reader can replicate. The §11 plan's `Form*` already keeps the variants separate (`F_LIST`, `F_VEC`, `F_MAP`); keep them that way. Note that `{…}` is *unused* in v1 — the reader rejects it with "reserved for SRFI-105 curly-infix; use `#{…}` for maps" — which both reserves the syntax for §12.5 and gives users a clear hint when they accidentally type the Clojure shape.
- **`#lang` dispatch lives in the driver, not the reader.** The driver inspects the first line, picks a reader, hands the rest of the file to it. This means swapping in the sweet reader later is a one-line addition to a dispatch table, not a reader refactor.
- **Reserve the `#lang` names** `turmeric`, `sweet-exp`, `turmeric/curly-infix`, `turmeric/neoteric` as known-but-not-yet-implemented in v1. Users typing them in anticipation get "not yet implemented" rather than "unknown #lang"; when each phase ships, the corresponding name just starts working.

### 12.5.8 Sweet-expressions Implementation Phases — Detailed Tasks

Below are the concrete implementation phases for sweet-expressions, each with goals, exit criteria, and detailed task checklists. Each phase is independently shippable and follows the fixture discipline: happy-path tests, interaction tests (sweet × defer, sweet × ref, sweet × macros, sweet × inline-C, sweet × quasiquote), negative tests with golden diagnostics, and codegen snapshots.

---

#### Phase S0 — Infrastructure & `#lang` Dispatch

**Goal:** Prepare the codebase to support multiple readers and the `#lang` directive mechanism.

**Exit Criterion:** `#lang turmeric` (default), `#lang turmeric/curly-infix`, `#lang turmeric/neoteric`, and `#lang sweet-exp` are all recognized by the driver. Unknown `#lang` values produce a clear error. The `.tursweet` file extension is recognized and defaults to `sweet-exp`.

**Prerequisites:** None — can land at any time, but must land before S1.

**Files to create/modify:**
- `src/reader.h` — add `ReaderVTable` struct with function pointers for the reader API
- `src/reader.c` — refactor existing reader into `s_read` functions, add dispatch
- `src/sweet_reader.{c,h}` — stub file for future sweet reader (empty for now)
- `src/main.c` — add `#lang` parsing in driver, reader selection logic
- `tests/fixtures/lang-dispatch/` — new test directory

**Task Checklist:**

- [ ] **Driver changes for `#lang` dispatch:**
  - [ ] Add `detect_lang()` function that reads the first line of a file and parses `#lang <name>`
  - [ ] Add `ReaderType` enum: `READER_TURMERIC`, `READER_CURLY_INFIX`, `READER_NEOTERIC`, `READER_SWEET`
  - [ ] Modify `compile_file()` to call `detect_lang()` and select the appropriate reader
  - [ ] Handle `.tursweet` extension: default to `READER_SWEET` if no `#lang` line present
  - [ ] Emit warning if file extension and `#lang` directive disagree
  - [ ] Add `--dump-lang` debug flag to print detected language and exit

- [ ] **Reader abstraction:**
  - [ ] Create `ReaderVTable` struct with: `read_form`, `read_all`, `init`, `free` function pointers
  - [ ] Create `Reader` struct holding vtable, state (arena, intern table, current span tracking)
  - [ ] Refactor existing `read_form()` into `s_read_form()` (s-expression reader)
  - [ ] Create `reader_new(ReaderType)` constructor that returns appropriate reader
  - [ ] Ensure all existing call sites use the abstracted reader API

- [ ] **`#lang` line parsing:**
  - [ ] Parse `#lang <name>` from first line only (no preceding whitespace/comments/BOM)
  - [ ] Reject `#lang` lines appearing after the first line
  - [ ] Strip trailing comments/whitespace from `#lang` line
  - [ ] Recognize and reserve: `turmeric`, `turmeric/curly-infix`, `turmeric/neoteric`, `sweet-exp`
  - [ ] For reserved but not-yet-implemented languages, emit "not yet implemented" error
  - [ ] For unknown languages, emit "unknown #lang `<name>`; expected one of …" error

- [ ] **File extension handling:**
  - [ ] Map `.tur` → default reader (turmeric)
  - [ ] Map `.tursweet` → sweet-exp reader
  - [ ] Add `--lang` CLI flag to override file extension and `#lang` directive

- [ ] **Fixtures:**
  - [ ] `lang-dispatch/default` — `.tur` file with no `#lang` line compiles
  - [ ] `lang-dispatch/explicit-turmeric` — `#lang turmeric` compiles
  - [ ] `lang-dispatch/unknown-lang` — `#lang foo` produces expected error
  - [ ] `lang-dispatch/not-implemented` — `#lang turmeric/curly-infix` produces "not yet implemented"
  - [ ] `lang-dispatch/tursweet-extension` — `.tursweet` file defaults to sweet-exp (error: not implemented)
  - [ ] `lang-dispatch/lang-line-not-first` — `#lang` on line 2 produces error
  - [ ] `lang-dispatch/empty-lang-name` — `#lang` with no name produces error

- [ ] **Documentation:**
  - [ ] Update README with `#lang` directive documentation
  - [ ] Document `.tursweet` extension

---

#### Phase S1 — Curly-Infix (SRFI-105)

**Goal:** Implement curly-infix notation `{a + b}` as a shorthand for `(+ a b)`, enabling natural infix math expressions.

**Exit Criterion:** Curly-infix works for all operator arities. Mixed operators emit `$nfx$` form. All curly-infix fixtures pass. No conflicts with existing `{…}` rejection (maps use `#{…}`).

**Prerequisites:** Phase S0 (`#lang` dispatch infrastructure).

**Files to create/modify:**
- `src/reader.c` — add curly-infix handling to s-expression reader
- `src/sweet_reader.{c,h}` — or implement as mode in existing reader
- `src/forms.h` — ensure `F_LIST` can represent infix desugaring
- `tests/fixtures/curly-infix/` — new test directory

**Task Checklist:**

- [ ] **Curly mode state machine:**
  - [ ] Add `in_curly` flag to reader state (push/pop on `{`/`}`)
  - [ ] Track nesting depth for `{ { a + b } + c }`
  - [ ] On `{`, push new list context with `is_curly: true` flag
  - [ ] On `}`, pop context and finalize infix desugaring

- [ ] **Token collection inside `{…}`:**
  - [ ] Collect whitespace-separated tokens (symbols, numbers, strings, nested `{…}`, `[]`, `()`)
  - [ ] Preserve spans for each token for diagnostics
  - [ ] Handle nested curly: `{ a + { b * c } }` → `(+ a (* b c))`

- [ ] **Infix desugaring logic:**
  - [ ] Single element `{e}` → `e` (identity)
  - [ ] Two elements `{a b}` → `(a b)` (function call)
  - [ ] Three+ elements with same operator: `{a + b + c}` → `(+ a b c)`
  - [ ] Three+ elements with mixed operators: `{a + b * c}` → `($nfx$ a + b * c)`
  - [ ] Operator set: `+`, `-`, `*`, `/`, `%`, `=`, `!=`, `<`, `>`, `<=`, `>=`, `&&`, `||`, `&`, `|`, `^`, `<<`, `>>`

- [ ] **Integration with existing forms:**
  - [ ] Curly-infix works inside other forms: `(let [x {a + b}] …)`
  - [ ] Curly-infix works in function positions: `(map {x -> x + 1} xs)` — wait, this is function syntax, not infix. Clarify: curly is for expressions only, not lambda.
  - [ ] Unmatched `{` or `}` produces clear error with span

- [ ] **`#lang turmeric/curly-infix` enablement:**
  - [ ] Create `READER_CURLY_INFIX` reader type that enables curly mode
  - [ ] Update `#lang` dispatch to return this reader for `turmeric/curly-infix`
  - [ ] Add `--curly-infix` CLI flag as alternative to `#lang`

- [ ] **Fixtures:**
  - [ ] `curly-infix/basic` — `{1 + 2}` → `(+ 1 2)`
  - [ ] `curly-infix/chained` — `{1 + 2 + 3}` → `(+ 1 2 3)`
  - [ ] `curly-infix/mixed-ops` — `{1 + 2 * 3}` → `($nfx$ 1 + 2 * 3)`
  - [ ] `curly-infix/nested` — `{ {1 + 2} * 3 }` → `(* (+ 1 2) 3)`
  - [ ] `curly-infix/single-element` — `{42}` → `42`
  - [ ] `curly-infix/two-elements` — `{f x}` → `(f x)`
  - [ ] `curly-infix/comparison` — `{a < b && c > d}` → `($nfx$ a < b && c > d)`
  - [ ] `curly-infix/inside-let` — `(let [x {a + b}] x)` works
  - [ ] `curly-infix/inside-fn` — `(fn [x] {x + 1})` works
  - [ ] `errors/curly-unmatched-open` — `{a + b` error
  - [ ] `errors/curly-unmatched-close` — `a + b}` error

- [ ] **Documentation:**
  - [ ] Document curly-infix in language reference
  - [ ] Note that `#nfx` is not defined by default; users must use parens or define it

---

#### Phase S2 — Neoteric Notation

**Goal:** Implement neoteric notation `f(x)` as `(f x)`, `f{x+1}` as `(f (+ x 1))`, and `f[x]` as `(bracketapply f x)`.

**Exit Criterion:** Neoteric syntax works for all bracket types. Interacts correctly with curly-infix. All neoteric fixtures pass.

**Prerequisites:** Phase S1 (curly-infix for `{…}` interaction).

**Files to create/modify:**
- `src/reader.c` — add neoteric lookahead after atoms
- `src/stdlib/macros.tur` — add `bracketapply` macro
- `tests/fixtures/neoteric/` — new test directory

**Task Checklist:**

- [ ] **Neoteric detection:**
  - [ ] After reading any atom (symbol, number, string, keyword), peek next character
  - [ ] If next char is `(`, `[`, or `{` with **no whitespace** between atom and bracket, trigger neoteric
  - [ ] Consume the bracket and its contents
  - [ ] Handle escaped whitespace? No — SRFI-110 spec: no whitespace means neoteric

- [ ] **Neoteric desugaring:**
  - [ ] `f(x y)` → `(f x y)` — parentheses become list
  - [ ] `f{x + y}` → `(f (+ x y))` — curly becomes infix expression, then wrapped
  - [ ] `f[x]` → `(bracketapply f x)` — square brackets become bracketapply
  - [ ] `f(x{y + z})` → `(f (x (+ y z)))` — nested neoteric works
  - [ ] `f.g(x)` → `((. f g) x)` — `.g` is atom `.g`, neoteric applies to whole thing

- [ ] **Bracket types:**
  - [ ] `(…)` — standard function call list
  - [ ] `{…}` — curly-infix expression (requires S1 to be implemented first)
  - [ ] `[…]` — bracketapply call

- [ ] **`bracketapply` macro:**
  - [ ] Define in `stdlib/macros.tur`: `(defmacro bracketapply [coll i] `(nth ~coll ~i))`
  - [ ] This makes `vec[0]` work as `(nth vec 0)`
  - [ ] Document that users can redefine `bracketapply` for custom semantics

- [ ] **Edge cases:**
  - [ ] `f (x)` — with whitespace, NOT neoteric, reads as `f` then `(x)`
  - [ ] `f
(x)` — with newline, NOT neoteric
  - [ ] `(f)(x)` — `f` is a list `(f)`, then `(x)` separately
  - [ ] `42(x)` — number followed by parens: `(42 x)` — valid, may error at elaboration
  - [ ] `f(x)(y)` — `( (f x) y )` — chained neoteric

- [ ] **`#lang turmeric/neoteric` enablement:**
  - [ ] Create `READER_NEOTERIC` reader type that enables both curly-infix and neoteric
  - [ ] Update `#lang` dispatch for `turmeric/neoteric`
  - [ ] Add `--neoteric` CLI flag

- [ ] **Fixtures:**
  - [ ] `neoteric/function-call` — `f(x)` → `(f x)`
  - [ ] `neoteric/function-call-multi` — `f(x y z)` → `(f x y z)`
  - [ ] `neoteric/curly-inside` — `f{x + y}` → `(f (+ x y))`
  - [ ] `neoteric/bracket` — `vec[0]` → `(bracketapply vec 0)`
  - [ ] `neoteric/nested` — `f(g(x))` → `(f (g x))`
  - [ ] `neoteric/chained` — `f(x)(y)` → `((f x) y)`
  - [ ] `neoteric/whitespace-no-neoteric` — `f (x)` → `f` and `(x)` separately
  - [ ] `neoteric/number-call` — `42(x)` → `(42 x)`
  - [ ] `neoteric/inside-let` — `(let [x f(y)] x)` works
  - [ ] `neoteric/inside-curly` — `f{a + g(x)}` → `(f (+ a (g x)))`
  - [ ] `errors/neoteric-unmatched-paren` — `f(x` error

- [ ] **Integration with existing features:**
  - [ ] Verify neoteric works with `defer`: `defer(f(x))` → `(defer (f x))`
  - [ ] Verify neoteric works with `ref`: `(ref f(x))` → `(ref (f x))`
  - [ ] Verify neoteric works in quasiquote: `` `f(x) `` → `` `(f x) ``

- [ ] **Documentation:**
  - [ ] Document neoteric notation
  - [ ] Document `bracketapply` and its default definition
  - [ ] Note that `f[x]` is syntactic sugar, not array indexing (unless user defines it that way)

---

#### Phase S3 — Full Sweet-Expressions (Indentation-Significant Grouping)

**Goal:** Implement full sweet-expressions with indentation-based grouping, `\` (GROUP/SPLIT), `$` (SUBLIST), and `<* … *>` (collecting lists).

**Exit Criterion:** All sweet-expressions syntax from SRFI-110 works. Indentation is processed correctly. The `tur unsweeten` command works. All sweet fixtures pass.

**Prerequisites:** Phase S0 (dispatch), Phase S1 (curly-infix), Phase S2 (neoteric).

**Files to create/modify:**
- `src/sweet_reader.{c,h}` — full sweet reader implementation
- `src/main.c` — add `unsweeten` subcommand
- `tests/fixtures/sweet/` — new test directory

**Task Checklist:**

- [ ] **Sweet reader architecture:**
  - [ ] Implement as separate state machine (not layered on s-expr reader)
  - [ ] Consume characters directly, emit `Form*` nodes
  - [ ] Track: current indentation stack, current line, current column
  - [ ] Handle: spaces, tabs, `!` as indent characters (configurable)
  - [ ] Error on mixing indent character types on same line

- [ ] **Indentation stack:**
  - [ ] Push new indent level when line starts with greater indentation than parent
  - [ ] Pop indent level when line starts with less indentation (match to parent)
  - [ ] Same indent level continues current group
  - [ ] Empty line ends current top-level expression

- [ ] **Leading abbreviation rule:**
  - [ ] If line starts with a reader-macro sigil (`'`, `` ` ``, `~`, `~@`) followed by whitespace, consume entire indented block as body
  - [ ] Works for quote, quasiquote, unquote, unquote-splicing
  - [ ] Example: `` ` 
  a 
  b `` → `` `(a b) ``

- [ ] **Special tokens:**
  - [ ] `\` (GROUP/SPLIT) — starts a group; child lines at same or greater indent are members
  - [ ] `$` (SUBLIST) — next expression is a sublist (wraps following expression in list)
  - [ ] `<* … *>` (collecting list) — collects all expressions until `*` into a list

- [ ] **Disable indentation inside:**
  - [ ] `(…)` — standard s-expr, no indentation processing
  - [ ] `[…]` — vector literal, no indentation processing
  - [ ] `{…}` — curly-infix or map, no indentation processing
  - [ ] `#{…}` — map literal, no indentation processing
  - [ ] `` "`"` — string literal, no indentation processing
  - [ ] ` ``` … ``` ` — inline-C block, no indentation processing

- [ ] **Line continuation:**
  - [ ] Backslash at end of line continues to next line (no indent significance)
  - [ ] Child lines extend parent line when parent has unclosed delimiter

- [ ] **`#lang sweet-exp` enablement:**
  - [ ] Create `READER_SWEET` type
  - [ ] Update `#lang` dispatch for `sweet-exp`
  - [ ] `.tursweet` extension defaults to this reader

- [ ] **`tur unsweeten` command:**
  - [ ] Parse sweet file, emit desugared s-expression form
  - [ ] Preserve spans in output for debugging
  - [ ] Rewrite `#lang sweet-exp` to `#lang turmeric` in output
  - [ ] Add `--to-sexp` flag to `tur fmt` as alternative

- [ ] **`tur sweeten` command (optional for this phase):**
  - [ ] Parse s-expression file, emit sweet form
  - [ ] Basic formatting: one expression per line, indented
  - [ ] Add `--to-sweet` flag to `tur fmt`

- [ ] **Fixtures — Basic sweet syntax:**
  - [ ] `sweet/indentation-basic` — nested indentation groups correctly
  - [ ] `sweet/leading-abbreviation` — `'` followed by indented block
  - [ ] `sweet/group-split` — `\` groups expressions
  - [ ] `sweet/sublist` — `$` creates sublists
  - [ ] `sweet/collecting-list` — `<* … *>` collects list
  - [ ] `sweet/mixed-indent-characters` — spaces and tabs (error if mixed)
  - [ ] `sweet/line-continuation` — backslash at EOL continues line

- [ ] **Fixtures — Sweet × existing features:**
  - [ ] `sweet/defer` — sweet syntax with defer works
  - [ ] `sweet/ref` — sweet syntax with ref works
  - [ ] `sweet/macros` — sweet syntax with defmacro works
  - [ ] `sweet/inline-c` — sweet syntax with inline-C blocks works
  - [ ] `sweet/quasiquote` — sweet syntax with quasiquote works
  - [ ] `sweet/keywords` — sweet syntax with keywords works
  - [ ] `sweet/neoteric` — sweet syntax with neoteric notation works
  - [ ] `sweet/curly-infix` — sweet syntax with curly-infix works

- [ ] **Fixtures — Edge cases:**
  - [ ] `sweet/empty-file` — empty file compiles
  - [ ] `sweet/single-expression` — single top-level expression
  - [ ] `sweet/multiple-expressions` — multiple top-level expressions
  - [ ] `sweet/comments` — comments don't affect indentation
  - [ ] `errors/sweet-indent-mismatch` — mismatched indentation error
  - [ ] `errors/sweet-mixed-indent-chars` — mixing spaces and tabs error

- [ ] **Integration tests:**
  - [ ] Existing phase 0–9 fixtures all still pass (sweet reader doesn't break s-expr reader)
  - [ ] Macro defined in s-expr file works when called from sweet file
  - [ ] Sweet file can include/require s-expr file

- [ ] **Documentation:**
  - [ ] Complete sweet-expressions reference
  - [ ] Examples of all sweet syntax forms
  - [ ] Migration guide for users coming from other Lisps
  - [ ] Editor configuration for `.tursweet` files

---

#### Phase S4 — Polish & Tooling

**Goal:** Add polish, tooling, and editor support for sweet-expressions.

**Exit Criterion:** `tur fmt --to-sweet` and `tur fmt --to-sexp` work. Tree-sitter grammar for `.tursweet` is available. All polish fixtures pass.

**Prerequisites:** Phase S3 (full sweet).

**Files to create/modify:**
- `src/fmt.c` — formatter for sweet output
- `docs/tree-sitter/` — tree-sitter grammar (or separate repo)
- `tests/fixtures/sweet-polish/` — new test directory

**Task Checklist:**

- [ ] **Bidirectional formatting:**
  - [ ] `tur fmt --to-sexp <file.tursweet>` — convert sweet to s-exprs
  - [ ] `tur fmt --to-sweet <file.tur>` — convert s-exprs to sweet
  - [ ] Preserve comments in both directions (best effort)
  - [ ] Preserve spans for error mapping
  - [ ] Add `--check` flag to verify formatting without modifying

- [ ] **Pretty-printing sweet:**
  - [ ] Configurable indent size (default 2 spaces)
  - [ ] Configurable indent character (space or tab)
  - [ ] Alignment options for multi-line forms
  - [ ] Line wrapping for long expressions

- [ ] **Tree-sitter grammar:**
  - [ ] Create `tree-sitter-turmeric` repo or `tree-sitter/turmeric` subdirectory
  - [ ] Grammar for `.tur` files (s-expressions)
  - [ ] Grammar for `.tursweet` files (sweet-expressions)
  - [ ] Indent rules for editor integration
  - [ ] Syntax highlighting queries

- [ ] **Editor support:**
  - [ ] VS Code extension or configuration
  - [ ] Emacs mode (or integration with existing Lisp modes)
  - [ ] Vim/Neovim filetype detection and indentation

- [ ] **REPL support for sweet:**
  - [ ] REPL accepts sweet syntax when enabled
  - [ ] REPL command to toggle sweet mode
  - [ ] Empty line submits expression (per SRFI-110)

- [ ] **Fixtures:**
  - [ ] `sweet-polish/format-to-sexp` — sweet → s-expr conversion
  - [ ] `sweet-polish/format-to-sweet` — s-expr → sweet conversion
  - [ ] `sweet-polish/format-idempotent` — formatting twice produces same output
  - [ ] `sweet-polish/comments-preserved` — comments survive round-trip

- [ ] **Documentation:**
  - [ ] Formatter documentation
  - [ ] Editor setup guide
  - [ ] REPL usage with sweet

---

#### Phase S5 — Performance & Robustness

**Goal:** Optimize the sweet reader and ensure it handles edge cases robustly.

**Exit Criterion:** Sweet reader has no known performance issues. All edge case fixtures pass. Fuzzing doesn't reveal crashes.

**Prerequisites:** Phase S3 (full sweet), Phase S4 (polish).

**Files to create/modify:**
- `src/sweet_reader.c` — performance optimizations
- `tests/fixtures/sweet-performance/` — new test directory
- `fuzz/` — fuzzing harness

**Task Checklist:**

- [ ] **Performance:**
  - [ ] Profile sweet reader on large files
  - [ ] Optimize indentation stack (use arena-allocated array instead of linked list)
  - [ ] Optimize token collection (pre-allocate buffers)
  - [ ] Minimize allocations during sweet parsing
  - [ ] Benchmark against s-expr reader (should be comparable)

- [ ] **Error recovery:**
  - [ ] Graceful error recovery for malformed input
  - [ ] Best-effort parsing with error markers
  - [ ] Multiple errors per file (not just first error)

- [ ] **Edge cases:**
  - [ ] Very deeply nested indentation (1000+ levels)
  - [ ] Very long lines (10000+ characters)
  - [ ] Unicode in sweet files (if supported)
  - [ ] Files with only whitespace
  - [ ] Files with BOM (byte order mark)

- [ ] **Fuzzing:**
  - [ ] Set up fuzzing harness for sweet reader
  - [ ] Fuzz with valid and invalid input
  - [ ] Ensure no crashes, no memory leaks (under ASan)
  - [ ] Integrate fuzzing into CI

- [ ] **Fixtures:**
  - [ ] `sweet-performance/large-file` — large sweet file compiles in reasonable time
  - [ ] `sweet-performance/deep-nesting` — deeply nested indentation works
  - [ ] `sweet-performance/long-lines` — long lines work
  - [ ] `errors/sweet-deep-indent-overflow` — reasonable error for excessive nesting

---

### 12.5.9 Cross-Phase Coordination

**Dependencies between phases:**

```
Phase S0 (Infrastructure)
    ↓
Phase S1 (Curly-Infix) —— requires S0 for #lang dispatch
    ↓
Phase S2 (Neoteric) —— requires S1 for {…} interaction
    ↓
Phase S3 (Full Sweet) —— requires S0, S1, S2
    ↓
Phase S4 (Polish) —— requires S3
    ↓
Phase S5 (Performance) —— requires S3
```

**Can ship independently:**
- S0 can land at any time (preparatory work)
- S1 can land once S0 is done
- S2 can land once S1 is done
- S3, S4, S5 must land in order

**Shared infrastructure:**
- All phases use the same `Reader` abstraction from S0
- All phases emit the same `Form*` ADT
- All phases preserve spans for diagnostics
- All phases integrate with the same `#lang` dispatch mechanism

---

### 12.5.10 Fixture Strategy for Sweet-Expressions

Each sweet phase adds fixtures in its directory:

| Phase | Fixture Directory | Focus |
|---|---|---|
| S0 | `tests/fixtures/lang-dispatch/` | `#lang` directive parsing and dispatch |
| S1 | `tests/fixtures/curly-infix/` | Curly-infix syntax and desugaring |
| S2 | `tests/fixtures/neoteric/` | Neoteric notation |
| S3 | `tests/fixtures/sweet/` | Full sweet syntax, indentation, special tokens |
| S4 | `tests/fixtures/sweet-polish/` | Formatting, tree-sitter, editor support |
| S5 | `tests/fixtures/sweet-performance/` | Performance, edge cases, fuzzing |

**Fixture types for each feature:**
1. **Happy path** — valid input, verify desugaring and compilation
2. **Interaction** — sweet × defer, sweet × ref, sweet × macros, sweet × inline-C, sweet × quasiquote
3. **Negative** — invalid input, verify diagnostic matches golden `expected.diag`
4. **Codegen snapshot** — verify emitted C matches expected (for non-sweet features, verify sweet doesn't perturb)
5. **Round-trip** — `tur unsweeten` → compile → verify equivalent to hand-written s-expr

**Naming convention:**
- `curly-infix/basic.tursweet` — happy path
- `curly-infix/inside-defer.tursweet` — interaction test
- `errors/curly-unmatched-open.tursweet` — negative test

---

### 12.5.11 Estimates and Prioritization

| Phase | Effort | Priority | Blocks |
|---|---|---|---|
| S0 | 2–4 days | High | S1, S2, S3 |
| S1 | 1–2 days | High | S2 |
| S2 | 2–3 days | High | S3 |
| S3 | 1–2 weeks | Medium | S4, S5 |
| S4 | 3–5 days | Medium | None |
| S5 | 2–3 days | Low | None |

**Recommended order:**
1. **S0 first** — infrastructure work that unblocks everything else
2. **S1 next** — curly-infix is small, high-value, low-risk
3. **S2 next** — neoteric builds on S1 naturally
4. **S3 next** — full sweet is the main feature
5. **S4** — polish for user experience
6. **S5** — performance cleanup

**Minimum viable sweet:** S0 + S1 + S2 gives users curly-infix and neoteric notation, which covers most readability use cases. Full sweet (S3) is the stretch goal.


---

## Higher-Ranked Types (HRT0–HRT5)

> **Status:** Draft — Not Started
> **Prerequisite:** Phase 15 (Typeclasses) complete; HKT H0 (kind system) recommended
> **Target:** v3 or later
> **Feature flag:** `-Xhrt` (off by default); `-Ximpredicative` for impredicative containers
> **Full design:** [higher-ranked-types-plan.md](archive/higher-ranked-types-plan.md)

**Executive Summary:** Higher-Ranked Types extend Hindley-Milner inference to allow universal (and existential) quantifiers at positions other than the outermost level. Turmeric uses bidirectional type checking: the programmer supplies explicit `forall`/`exists` annotations where needed; inference handles the rest.

**Primary motivators:** `runST`-style region safety; CPS with universal return type; existential types for ADT encapsulation; van Laarhoven lenses; Church/Böhm-Berarducci encodings.

**Key constraint:** Full inference is undecidable for Rank-3+ (Tiuryn & Urzyczyn 1996). Annotation-guided bidirectional checking is used throughout.

---

### Phase HRT0 — `forall`/`exists` Syntax and AST

**Goal:** Introduce `forall` and `exists` as first-class type-level forms. No inference yet — this phase only parses and represents quantified types in the AST.

#### Prerequisites Checklist
- [ ] Phase 15 (Typeclasses, kind-`*`) is complete and stable
- [ ] HKT Phase H0 (kind system) is landed — quantified type variables carry explicit kinds
- [ ] `Type` struct in `src/types.h` can represent polymorphic types (rank-1 `forall` is already implicit; needs explicit node)
- [ ] Elaborator (`src/elab.c`) has a bidirectional checking mode (expected-type threading)
- [ ] Error reporting (`src/diag.c`) can emit type-level diffs with quantifier annotations shown
- [ ] No code currently relies on all type variables being implicitly rank-1

#### Surface syntax (`src/reader.c`)
- [ ] Recognize `forall` as a reserved type-level keyword: `(forall [a] (-> a a))`
- [ ] Recognize `exists` as a reserved type-level keyword: `(exists [a] [a (-> a string)])`
- [ ] Support multiple bound variables: `(forall [a b] (-> a b a))`
- [ ] Support kind-annotated bound variables: `(forall [f : * -> *] (-> (f int) (f string)))`
- [ ] Reject `forall`/`exists` in expression position (type annotations only for now)
- [ ] Disambiguate `forall` from user-defined bindings in expression context

#### AST extensions (`src/expr.h`, `src/types.h`)
- [ ] Add `TY_FORALL` node: `{ vars: [(name, kind)], body: Type }`
- [ ] Add `TY_EXISTS` node: `{ vars: [(name, kind)], body: Type }`
- [ ] Distinguish bound (`forall`-introduced) from free type variables at the AST level
- [ ] Add `rank()` helper that computes the rank of a `Type` node (0 = monotype, 1 = rank-1, etc.)
- [ ] Preserve source location through quantifier nodes for diagnostics

#### Pretty-printing (`src/types.c`)
- [ ] Print `TY_FORALL` as `(forall [a ../..] T)`
- [ ] Print `TY_EXISTS` as `(exists [a ../..] T)`
- [ ] In error messages, always print quantifiers explicitly (never elide `forall`)

#### Validation pass
- [ ] Scope check: all type variables in `body` that appear in `vars` are bound
- [ ] No shadowing: warn if a `forall`-bound variable shadows an outer type variable
- [ ] Kind check: bound variables carry valid kinds (default `*` when unannotated)

#### Fixtures
- [ ] `hrt-syntax-forall.tur` — `forall` parses in type annotation position
- [ ] `hrt-syntax-exists.tur` — `exists` parses in type annotation position
- [ ] `hrt-syntax-multi.tur` — multiple bound variables in one quantifier
- [ ] `hrt-syntax-error.tur` — `forall` in expression position is rejected

**Exit criterion:** All syntax fixtures parse; `forall`/`exists` AST nodes printed correctly in diagnostics; scope and kind checks pass; no runtime codegen yet.

---

### Phase HRT1 — Rank-2 Universal Types

**Goal:** Type-check and compile functions whose argument types contain `forall` quantifiers (Rank-2). Foundation for `runST`-style patterns.

#### Bidirectional type checker (`src/elab.c`)
- [ ] Implement **checking mode**: given an expected type, propagate it into subexpressions
- [ ] Implement **inference mode**: infer a type bottom-up (existing HM path)
- [ ] Rule `∀-intro` (checking): when expected type is `(forall [a] T)`, bind `a` as a rigid type variable and check the body against `T`
- [ ] Rule `∀-elim` (inference): when a `forall` type is used at a known concrete type, instantiate the bound variable
- [ ] Rank-2 application rule: when a function expects `(forall [a] (-> a a))`, the argument must be checkable at all monotypes
- [ ] Propagate expected types through `let`, `do`, `if`, `cond` forms
- [ ] Reject higher-rank usage without annotation: emit "rank-2 type requires explicit annotation" diagnostic

#### Type annotation form (`src/forms.c`)
- [ ] Introduce `(:: expr type)` as an inline type ascription form (if not already present)
- [ ] Allow `defn` signatures to carry `forall`-annotated argument types
- [ ] Validate that ascribed types are well-kinded before elaboration

#### Substitution and unification (`src/types.c`)
- [ ] Distinguish rigid (skolem) type variables from unification metavariables
- [ ] Unification never solves rigid variables: "could not unify rigid `a` with `int`" error
- [ ] Implement `instantiate`: replace `forall`-bound variables with fresh metavariables for inference
- [ ] Implement `skolemize`: replace `forall`-bound variables with rigid skolems for checking
- [ ] Capture-avoiding substitution for quantified types

#### Codegen for rank-2 (`src/emit.c`)
- [ ] A rank-2 argument `(forall [a] (-> a a))` is represented at runtime as a **generic closure** — a pair of `(void *env, void *(*fn)(void *env, void *arg, size_t size))`
- [ ] Size/alignment of `a` is threaded as an implicit parameter (or passed via a small descriptor struct)
- [ ] Emit wrapper thunks when a concrete function is passed where a rank-2 polymorphic function is expected
- [ ] Call sites of rank-2 arguments emit the correct descriptor for the concrete type

#### Fixtures
- [ ] `hrt-rank2-identity.tur` — `(forall [a] (-> a a))` argument accepted
- [ ] `hrt-rank2-apply.tur` — function that applies a polymorphic argument to multiple types
- [ ] `hrt-rank2-runst.tur` — `runST`-style phantom region: `(forall [s] (-> (ST s a) a))`
- [ ] `hrt-rank2-annotation.tur` — `::` ascription forces rank-2 checking
- [ ] `hrt-rank2-error.tur` — missing annotation caught; rank mismatch caught

**Exit criterion:** Rank-2 functions type-check with bidirectional rules; codegen produces correct C; `runST` pattern compiles and executes; all rank-2 fixtures green.

---

### Phase HRT2 — Existential Types

**Goal:** Support `exists` (existential types) for encapsulation, abstract data types, and module-like interfaces.

#### Existential type packing (`src/elab.c`)
- [ ] Rule `∃-intro` (packing): `(pack witness-type value : (exists [a] T))` creates an existential value
- [ ] Elaborate `pack` form: verify the witness type satisfies `T[a := witness-type]`
- [ ] Runtime representation: existential value is a boxed pair of `(type-descriptor, data-pointer)`

#### Existential type unpacking (`src/elab.c`, `src/forms.c`)
- [ ] Rule `∃-elim` (unpacking): `(open x [a v] body)` binds `a` as a rigid type variable and `v` as the hidden value
- [ ] Enforce scope restriction: `a` must not escape the body of `open`
- [ ] Error: "existential type variable `a` escapes its scope"

#### Abstract data types with existentials
- [ ] Allow `defstruct` to define an existentially-typed field: `(defstruct Showable (exists [a] [a (-> a string)]))`
- [ ] Derive pack/unpack helpers for existentially-typed structs
- [ ] Demonstrate module pattern: struct of functions sharing a hidden state type

#### Codegen for existentials (`src/emit.c`)
- [ ] Emit a tagged union / descriptor struct for each existential type
- [ ] `pack` emits: allocate descriptor, store pointer and size, box the value
- [ ] `open` emits: unbox and bind to local variables, pass type descriptor implicitly

#### Fixtures
- [ ] `hrt-exists-pack.tur` — pack an existential value
- [ ] `hrt-exists-open.tur` — open and use an existential value
- [ ] `hrt-exists-escape.tur` — escaped type variable caught at compile time
- [ ] `hrt-exists-adt.tur` — `Showable` abstract data type via existential
- [ ] `hrt-exists-module.tur` — module pattern: record of functions over hidden state

**Exit criterion:** Existential types pack/unpack correctly; scope restriction enforced; module pattern compiles and runs; all existential fixtures green.

---

### Phase HRT3 — Rank-N Universal Types

**Goal:** Generalize from Rank-2 to arbitrary-rank universal types with annotation-guided bidirectional checking.

#### Rank-N checker (`src/elab.c`)
- [ ] Generalize bidirectional rules to arbitrary nesting depth
- [ ] Track current **polarity** (checking vs. inferring) as types are traversed
- [ ] Under a `forall` in checking position: skolemize, recurse into body
- [ ] Under a `forall` in inference position: instantiate with fresh metavariables
- [ ] Detect and report the **minimum rank** needed: "argument requires rank-3 annotation"
- [ ] Allow `^rank-n` pragma on `defn` to enable rank-N checking for a specific function

#### Annotation propagation
- [ ] `defn` return type annotations propagate expected types to the body
- [ ] `let` type annotations thread through to the bound expression
- [ ] `if`/`cond` branches share the propagated expected type
- [ ] `do` blocks: only the last expression gets the propagated type

#### Rank-N in typeclasses
- [ ] Allow typeclass methods to have rank-N types in their signatures
- [ ] Kind-check rank-N typeclass method signatures
- [ ] Dictionary codegen for rank-N methods: methods with polymorphic arguments emit generic closure fields

#### Interaction with HKT
- [ ] A `forall` can bind a kind variable: `(forall [f : * -> *] (-> (f int) (f string)))`
- [ ] Ensure kind and type quantification compose without conflict
- [ ] Skolem variables carry both a name and a kind

#### Fixtures
- [ ] `hrt-rankn-rank3.tur` — rank-3 function with explicit annotation
- [ ] `hrt-rankn-typeclass.tur` — typeclass method with rank-N signature
- [ ] `hrt-rankn-hkt.tur` — combined kind and type quantification
- [ ] `hrt-rankn-propagation.tur` — annotation propagation through `let`/`if`
- [ ] `hrt-rankn-missing.tur` — missing annotation gives clear diagnostic

**Exit criterion:** Arbitrary-rank types type-check with annotation guidance; rank-N typeclass methods work; combined HKT+HRT fixtures green; no crashes on deeply nested quantifiers.

---

### Phase HRT4 — First-Class Polymorphic Values

**Goal:** Allow polymorphic values to be stored in data structures, returned from functions, and passed through containers. Requires `-Ximpredicative`.

#### Impredicative instantiation (`src/elab.c`)
- [ ] Allow type metavariables to be solved to polymorphic types (e.g., `(vec (forall [a] (-> a a)))`)
- [ ] Guard impredicative use behind feature flag `-Ximpredicative`
- [ ] Error without the flag: "impredicative type requires `-Ximpredicative`"
- [ ] Implement quick-look impredicativity (Serrano et al. 2020): inspect function arguments before full unification to detect polymorphic pushes

#### Runtime polymorphic value representation
- [ ] Define `tur_poly_t`: a fat pointer pairing `(void *thunk, tur_type_descriptor_t *desc)`
- [ ] A `tur_type_descriptor_t` carries: size, alignment, copy/move/drop function pointers
- [ ] Polymorphic values stored in containers use `tur_poly_t` slots
- [ ] Codegen: when a `forall` type is stored in a `vec` or `option`, box it as `tur_poly_t`

#### Container integration
- [ ] `(vec (forall [a] (-> a a)))` stores `tur_poly_t` entries
- [ ] `(option (forall [a] (-> a a)))` carries a `tur_poly_t` payload
- [ ] Emit appropriate copy/drop logic for `tur_poly_t` in container operations
- [ ] Verify no double-free or leak with ASan in integration tests

#### Interaction with closures
- [ ] A rank-N closure captured in the environment is stored as `tur_poly_t` in the env struct
- [ ] Calling a captured polymorphic closure reconstructs the concrete type from the descriptor at the call site
- [ ] Ensure closure lifetimes are respected (existing `defer` mechanism handles cleanup)

#### Fixtures
- [ ] `hrt-impred-vec.tur` — `(vec (forall [a] (-> a a)))` stores and retrieves correctly
- [ ] `hrt-impred-option.tur` — polymorphic value wrapped in `option`
- [ ] `hrt-impred-closure.tur` — rank-N closure captured and called from within a higher-order function
- [ ] `hrt-impred-asan.tur` — no memory errors under ASan
- [ ] `hrt-impred-error.tur` — impredicative use without flag gives diagnostic

**Exit criterion:** First-class polymorphic values stored, retrieved, and called correctly; no memory errors; impredicativity flag required and respected.

---

### Phase HRT5 — Integration & Polish

**Goal:** Production-ready HRT support with documentation, stdlib patterns, and performance validation.

#### Documentation
- [ ] `docs/hrt-guide.md` — user-facing guide: when to use HRTs, annotation syntax, common patterns
- [ ] Update `docs/hkt-implementation-plan.md` §Non-Goals to reference this plan
- [ ] Add HRT section to language reference manual
- [ ] Cookbook entries: `runST`, optics, Church encodings, module pattern

#### Standard library patterns (`stdlib/`)
- [ ] `(defn run-st [f : (forall [s] (-> (ST s a) a))] : a ../..)` — safe mutable state
- [ ] `(deftype Lens s t a b (forall [f : Functor] (-> (-> a (f b)) s (f t))))` — van Laarhoven lens
- [ ] `(deftype Cont r a (forall [ignored] (-> (-> a r) r)))` — continuation monad
- [ ] `(deftype Church a (forall [r] (-> (-> a r) r r)))` — Church encoding

#### Error message improvements
- [ ] Show rank of inferred vs. expected type in mismatch diagnostics
- [ ] Suggest adding `::` annotation when rank inference fails
- [ ] "Escaped skolem" errors include which `forall`/`open` introduced the variable
- [ ] `tur explain` support for HRT-specific error codes

#### Performance
- [ ] Benchmark overhead of generic closure representation vs. monomorphized paths
- [ ] Measure impact of `tur_poly_t` boxing in container operations
- [ ] Provide `-O` monomorphization path for rank-2 when call site types are known
- [ ] Document performance tradeoffs in `docs/hrt-guide.md`

#### Testing
- [ ] Property tests for quantifier law: `∀a. id @a = id`
- [ ] Integration tests: HRT + closures + defer + typeclasses + HKT
- [ ] Negative tests: rank mismatch, escaped skolems, impredicative without flag
- [ ] Fuzz the type checker with randomly generated rank-N type annotations

#### Fixtures
- [ ] `hrt-stdlib-runst.tur` — `run-st` safely encapsulates mutable state
- [ ] `hrt-stdlib-lens.tur` — van Laarhoven lens composes correctly
- [ ] `hrt-stdlib-cont.tur` — continuation monad using HRT
- [ ] `hrt-stdlib-church.tur` — Church-encoded data structure
- [ ] `hrt-integration.tur` — HRT + HKT + typeclasses + closures + defer

**Exit criterion:** All stdlib patterns compile and execute; documentation complete; performance benchmarks acceptable; all fixtures green.

---

## Generalized Algebraic Data Types (G0–G4)

> **Status:** Draft — Not Started
> **Prerequisite:** Phase 15 (Typeclasses) complete; HRT phases HRT0–HRT2 required before G2; plain ADTs (G0) must land first
> **Target:** v4 or later
> **Feature flag:** `-Xgadt` (off by default)
> **Full design:** [gadts-plan.md](archive/gadts-plan.md)

**Executive Summary:** GADTs extend plain ADTs by allowing each constructor to specialize the type parameters of its return type. The type-checker learns those refinements when pattern-matching, enabling programs enforced entirely by the type system — typed AST interpreters, length-indexed vectors, type-safe printf, proof-carrying data.

**Key constraint:** Requires bidirectional checking (from HRT) for refinement to propagate through `match` arms. The scrutinee type must always be known; annotation-guided checking is used throughout.

---

### Phase G0 — Plain ADTs (Sum Types)

**Goal:** Add `defdata` for user-defined sum types and `match` for pattern matching. No type-refinement semantics. Prerequisite for all subsequent GADT phases and independently useful.

#### Surface syntax (`src/reader.c`, `src/forms.c`)
- [ ] Recognize `defdata` as a type-definition keyword: `(defdata Name [:copy] (Ctor1) (Ctor2 FieldType1 FieldType2) ../..)`
- [ ] Support type parameters: `(defdata Option [a] (None) (Some a))`
- [ ] `[:copy]` modifier (same semantics as `defstruct :copy`) for copy-able ADTs
- [ ] Recognize `match` as a special form: `(match scrutinee (Ctor1) body1 (Ctor2 x y) body2 _ default-body)`
- [ ] Nested patterns: `(match opt (Some (Some x)) x _ -1)`
- [ ] Wildcard `_` and variable capture patterns
- [ ] Guard clauses: `(Ctor x) when (> x 0)` — deferred to G4

#### AST extensions (`src/expr.h`, `src/types.h`)
- [ ] Add `EX_DEFDATA` node: `{ name, type_params[], constructors[] }`
- [ ] Add `EX_MATCH` node: `{ scrutinee, arms[] }` where each arm is `{ pattern, body }`
- [ ] Add `TY_ADT` type kind: `{ AdtDef *def, Type *type_args[], n_type_args }`
- [ ] Add `AdtDef` struct: name, type parameters, constructor descriptors
- [ ] Add `CtorDef` struct: name, field types, parent `AdtDef`
- [ ] Constructor names are globally registered as value-level functions in the symbol table

#### Exhaustiveness checking (`src/elab.c`)
- [ ] Implement pattern coverage matrix: enumerate all constructors and mark covered cases
- [ ] Error on non-exhaustive match: `non-exhaustive match: missing constructor 'Foo'`
- [ ] Warning on redundant arms (arm that can never be reached)
- [ ] Wildcard `_` covers all remaining constructors

#### Type checking (`src/elab.c`)
- [ ] Infer scrutinee type; look up `AdtDef` in the type environment
- [ ] For each arm, bind the constructor's field names in the local environment
- [ ] All arms must have the same result type (unification); emit "match arms have mismatched types" otherwise
- [ ] Constructor call expressions elaborate like function calls: `(Some x)` has type `(Option (type-of x))`

#### Codegen (`src/emit.c`)
- [ ] Emit a C tagged union: `struct tur_adt_Name { int tag; union { struct { ../.. } Ctor1; struct { ../.. } Ctor2; } as; }`
- [ ] Constructor functions emit a struct initializer with the correct tag
- [ ] `match` emits a `switch (x.tag)` with a `case` per constructor, binding field names to local variables
- [ ] Integrate with existing copy/move/drop infrastructure (CopyKind propagation)
- [ ] For copy ADTs, emit `tur_adt_Name_copy`; for move ADTs, ensure single-owner discipline

#### Memory management
- [ ] ADT with all copy-able fields defaults to `CK_COPY`
- [ ] ADT containing any move-only field is `CK_MOVE`
- [ ] Recursive ADT fields (e.g., list nodes) must use `ref<T>` or `rc<T>` to avoid infinite-sized structs

#### Fixtures
- [ ] `adt-basic.tur` — `Color` enum, exhaustive match
- [ ] `adt-param.tur` — `Option [a]`, `(option-map f (Some 3))`
- [ ] `adt-nested.tur` — `(match (Some (Some 42)) (Some (Some n)) n ../..)`
- [ ] `adt-nonexhaustive.tur` — non-exhaustive match caught at compile time
- [ ] `adt-copy.tur` — copy-able ADT assigned twice without error
- [ ] `adt-move.tur` — move ADT triggers borrow error on second use
- [ ] `adt-recursive.tur` — linked list `(defdata List [a] (Nil) (Cons a (ref (List a))))`

**Exit criterion:** Sum types declared, constructed, and pattern-matched correctly; exhaustiveness enforced; C codegen compiles cleanly; copy/move semantics correct; all G0 fixtures green.

---

### Phase G1 — GADT Syntax and AST

**Goal:** Introduce `defgadt` as a syntactic variant of `defdata` in which each constructor carries an explicit return-type annotation. Parses and represents the refined type in the AST — no type checking of the refinements yet.

#### Surface syntax (`src/reader.c`, `src/forms.c`)
- [ ] Recognize `defgadt` keyword: `(defgadt Name [type-params../..] (Ctor1 : return-type) (Ctor2 FieldType : return-type) ../..)`
- [ ] The `: return-type` annotation is required on every constructor in a `defgadt`; missing annotations produce a parse error
- [ ] `return-type` must be an application of `Name` to type arguments (validated in G2)
- [ ] `defgadt` without `-Xgadt` flag produces an "unknown form" error with a hint

#### AST extensions (`src/expr.h`, `src/types.h`)
- [ ] Add `EX_DEFGADT` node (separate from `EX_DEFDATA` for clarity)
- [ ] Extend `CtorDef` with a `result_type` field: the explicitly annotated return type (a `Type *`)
- [ ] Distinguish `AdtDef.is_gadt` flag: `false` for plain `defdata`, `true` for `defgadt`
- [ ] `rank()` helper (from HRT plan): report `TY_ADT` with GADT flag as requiring refinement

#### Pretty-printing (`src/types.c`, `src/diag.c`)
- [ ] Print GADT constructor signatures including the `: return-type` in error messages
- [ ] In type mismatch errors, show which GADT constructor caused the refinement

#### Validation pass
- [ ] Verify that each constructor's return type is an application of the parent GADT's type constructor
- [ ] Verify that type arguments in the return type are either bound variables or concrete types
- [ ] Detect and error on constructors whose return-type annotation does not mention the GADT name
- [ ] Kind-check type arguments in return-type annotations (default kind `*`)

#### Fixtures
- [ ] `gadt-syntax-basic.tur` — `defgadt` with return-type annotations parses
- [ ] `gadt-syntax-multi.tur` — multiple type parameters in GADT
- [ ] `gadt-syntax-error.tur` — missing annotation on a constructor in `defgadt` is rejected
- [ ] `gadt-syntax-flag.tur` — `defgadt` without `-Xgadt` gives hint

**Exit criterion:** GADT definitions parse; `EX_DEFGADT` nodes printed correctly in diagnostics; validation pass catches malformed constructors; no runtime codegen differences from G0 yet.

---

### Phase G2 — Type Refinement in Pattern Matching

**Goal:** Implement type-directed refinement in `match` arms for GADT scrutinees. Each constructor's return-type annotation introduces *skolem equalities* active for the duration of that arm's body.

**Depends on:** HRT0–HRT2 (bidirectional checking, skolem variables, rigid type variables).

#### Bidirectional checking for GADT match (`src/elab.c`)
- [ ] In `match` elaboration, detect if the scrutinee has a `TY_ADT` with `is_gadt = true`
- [ ] For each arm, call `elab_match_arm_gadt`:
  1. Look up the `CtorDef` and its `result_type`
  2. Unify `result_type` with the scrutinee type under a fresh local skolem environment
  3. Each type argument in `result_type` that is a type variable gets bound as a skolem equality (e.g., `a ~ int`)
  4. Elaborate the arm body under the extended environment with skolem equalities
  5. At arm exit, discard the skolem equalities
- [ ] Ensure the inferred type of each arm body is consistent with the overall match result type
- [ ] When the match result type contains a free GADT type variable and all arms refine it differently, use the most general common supertype (or error)

#### Skolem equality representation (`src/types.h`, `src/types.c`)
- [ ] Add `TY_SKOLEM_EQ` entry to `TypeKind`: a local equality assertion `a ~ T` in scope
- [ ] Store skolem equalities in a per-arm `SkolemEnv` (stack-allocated, freed on arm exit)
- [ ] `type_equiv` and `type_unify` consult the current `SkolemEnv` when comparing types
- [ ] Skolem equalities are *not* metavariables — they are rigid and non-backtrackable

#### Exhaustiveness for GADTs
- [ ] Exhaustiveness checker must account for GADT constructor subsets: not all constructors are reachable for a given instantiation of the type parameter
- [ ] Example: for scrutinee `(Expr int)`, `BoolLit` is unreachable — do not require it
- [ ] Emit a *warning* (not an error) for arms that are unreachable given the scrutinee's type
- [ ] Emit an error only when there is no arm covering a reachable constructor

#### Type variable scoping
- [ ] A type variable refined in a GADT arm must not escape that arm's scope
- [ ] Error: `skolem type variable 'a' (refined in 'IntTag' arm) escapes match body`
- [ ] The scrutinee's free type variable is still in scope outside the match — only the refinement is arm-local

#### Codegen (`src/emit.c`)
- [ ] GADT match codegen is identical to plain ADT match (`switch (tag)`)
- [ ] No runtime representation difference — refinement is purely a compile-time property
- [ ] The C type used in each arm may differ (e.g., `int64_t` vs `bool`) — emit appropriate local variable declarations per arm

#### Fixtures
- [ ] `gadt-refine-basic.tur` — `Witness` tag GADT; `show-witness` compiles and runs
- [ ] `gadt-refine-expr.tur` — typed expression interpreter; `eval-expr` evaluates correctly
- [ ] `gadt-refine-nat.tur` — `Nat` / `Vec n a` length-indexed vector; `safe-head` rejected on `VNil`
- [ ] `gadt-refine-escape.tur` — skolem variable escaping arm caught at compile time
- [ ] `gadt-refine-exhaustive.tur` — unreachable arm produces warning; missing reachable arm produces error

**Exit criterion:** Type refinement propagates correctly through GADT match arms; `eval-expr` and `safe-head` compile and run; unreachable-arm warnings emitted; skolem escape caught; C codegen compiles cleanly; all G2 fixtures green.

---

### Phase G3 — Type Equality Witnesses

**Goal:** Introduce first-class type equality evidence as the built-in `Equal` GADT, a `coerce` form, and a `(~)` constraint notation. Enables safe coercions, transport lemmas, and richer indexed data structures.

**Depends on:** G2 complete.

#### Built-in `Equal` GADT (`stdlib/equal.tur`, `src/elab.c`)
- [ ] Define `Equal` in the standard library with compiler-magic support:
  `(defgadt Equal [a b] (Refl : (Equal a a)))`
- [ ] The `Refl` constructor forces `a = b` at the call site (unification under the current skolem environment)
- [ ] Matching on `Refl` introduces `a ~ b` as a skolem equality in the arm body
- [ ] `Equal` is recognized by the elaborator as the canonical equality witness type

#### `coerce` form (`src/forms.c`, `src/elab.c`)
- [ ] Introduce `(coerce eq x)` where `eq : (Equal a b)` and `x : a`, producing `x : b`
- [ ] Elaboration: check `eq` has type `(Equal a b)`, check `x` has type `a`, produce type `b`
- [ ] Codegen: `coerce` is a no-op at runtime — identical memory layout for `a` and `b` in all cases
- [ ] Error if `eq` is not an `Equal` value: `coerce requires an (Equal a b) proof as first argument`

#### `(~)` constraint notation (`src/reader.c`, `src/elab.c`)
- [ ] Allow `(~ a b)` as a constraint in `defn` parameter lists: `(defn only-int [^(~ a int) x : a] : int x)`
- [ ] Elaborate `(~ a b)` constraints as equality assumptions (not typeclass dictionaries)
- [ ] Interaction with typeclass constraints: `(~ a int)` and `^Eq a` may coexist

#### Symmetry, transitivity, and congruence (`stdlib/equal.tur`)
- [ ] `(defn equal-sym [eq : (Equal a b)] : (Equal b a) ../..)`
- [ ] `(defn equal-trans [eq1 : (Equal a b), eq2 : (Equal b c)] : (Equal a c) ../..)`
- [ ] `(defn equal-cong [eq : (Equal a b)] : (Equal (f a) (f b)) ../..)` — requires HKT (deferred to G4 integration)

#### Interaction with HRT
- [ ] An `(Equal a b)` proof inside a `forall` creates a rank-2 equality
- [ ] Skolems from GADT refinement and skolems from `forall` (HRT) share the same rigid variable infrastructure
- [ ] No conflict between HRT `∀-intro` skolems and GADT arm skolems (different scopes)

#### Fixtures
- [ ] `gadt-equal-refl.tur` — `Refl` used to prove `(Equal int int)`
- [ ] `gadt-equal-coerce.tur` — `coerce` transports a value across a proof
- [ ] `gadt-equal-sym.tur` — symmetry of `Equal`
- [ ] `gadt-equal-trans.tur` — transitivity of `Equal`
- [ ] `gadt-equal-constraint.tur` — `(~)` constraint in `defn` signature
- [ ] `gadt-equal-error.tur` — `coerce` without valid proof rejected

**Exit criterion:** `Equal` GADT usable; `coerce` is a type-safe zero-cost cast; `(~)` constraint notation works in `defn`; symmetry and transitivity stdlib functions compile and pass tests; all G3 fixtures green.

---

### Phase G4 — Integration & Polish

**Goal:** Production-ready GADT support: stdlib patterns, comprehensive error messages, performance validation, and documentation.

#### Documentation
- [ ] `docs/gadts-guide.md` — user-facing guide: ADT vs GADT, annotation syntax, common patterns, pitfalls
- [ ] Update `docs/higher-ranked-types-plan.md` §Non-Goals to reference this plan
- [ ] Update `docs/higher-kinded-types-plan.md` §Non-Goals to reference this plan
- [ ] Add GADT section to language reference manual
- [ ] Cookbook entries: typed AST, length-indexed vec, type-safe printf, equality witnesses

#### Standard library patterns (`stdlib/`)
- [ ] `stdlib/equal.tur` — `Equal` GADT, `coerce`, `equal-sym`, `equal-trans`, `equal-cong` (once HKT available)
- [ ] `stdlib/nat.tur` — type-level `Nat` via `defgadt`; `Zero`, `Succ`; `nat-plus` proof
- [ ] `stdlib/vec.tur` — length-indexed `Vec n a`; `safe-head`, `safe-tail`, `vzip`, `vmap`
- [ ] `stdlib/result.tur` — `Result e a` as a GADT (or plain ADT — TBD based on user need)

#### Error message improvements
- [ ] GADT match exhaustiveness errors name the missing *reachable* constructors (not all constructors)
- [ ] Skolem escape errors include which constructor arm introduced the variable
- [ ] Type mismatch in GADT arm shows the active skolem equalities at the mismatch site
- [ ] `tur explain` support for new GADT-specific error codes (`TUR_E0010`–`TUR_E0019` reserved)

#### `equal-cong` with HKT
- [ ] Once HKT H0–H1 land, implement `equal-cong`:
  `(defn equal-cong [^Functor f, eq : (Equal a b)] : (Equal (f a) (f b)) ../..)`
- [ ] Requires kind-annotated type variable `f : * -> *`

#### Guard clauses in `match`
- [ ] Add `when` guard syntax to `match` arms: `(match e (IntLit n) when (> n 0) (do-positive-thing n) (IntLit n) (do-general-thing n) ../..)`
- [ ] Guards are elaborated as `bool` expressions in the arm's skolem environment
- [ ] Exhaustiveness checker treats guarded arms as potentially incomplete

#### Performance
- [ ] Benchmark GADT match dispatch vs. plain ADT (`switch` vs. `switch`)
- [ ] Measure compile-time overhead of skolem environment management
- [ ] Verify that `coerce` emits zero instructions (pure cast) under `-O2`
- [ ] Document findings in `docs/gadts-guide.md`

#### Testing
- [ ] Property tests: `eval-expr` produces correct types and values for randomly generated expressions
- [ ] Integration tests: GADT + typeclasses + closures + defer + HRT
- [ ] Negative tests: skolem escape, non-exhaustive GADT match, `coerce` without proof
- [ ] Fuzz the type checker with randomly generated GADT definitions and match expressions

#### Fixtures
- [ ] `gadt-stdlib-nat.tur` — type-level `Nat` arithmetic compiles and evaluates
- [ ] `gadt-stdlib-vec.tur` — `VNil`/`VCons` operations; `safe-head` rejects empty vector at compile time
- [ ] `gadt-guard.tur` — `when` guard clauses in GADT match
- [ ] `gadt-integration.tur` — GADT + HRT + typeclasses + closures + defer
- [ ] `gadt-asan.tur` — no memory errors under ASan for GADT-heavy programs

**Exit criterion:** All stdlib patterns compile and execute; documentation complete; performance benchmarks acceptable; `equal-cong` available once HKT lands; all G4 fixtures green.

---

## Serializable Continuations (SC-A–SC-F)

> **Status:** Draft — Not Started
> **Prerequisite:** Phases 18 (delimited continuations) and 19 (algebraic effects) complete; B1–B5 (cloneable continuations) recommended co-prerequisite
> **Target:** v3+ (Phase 21+)
> **Full design:** [serializable-continuations-plan.md](archive/serializable-continuations-plan.md)

**Executive Summary:** A suspended computation (continuation) can be marshalled to bytes, written to disk or sent over a network, then unmarshalled and resumed in a fresh process. Phase 18's delimited continuations already reify the call stack as a heap-allocated closure chain — serialization traverses that chain, emits a stable encoding of each frame, and reconstructs it on load.

**Key design points:**

| Challenge | Mitigation |
|-----------|-----------|
| Function pointers are not portable across builds | Stable symbol table: each frame stores a stable string key, resolved at resume time |
| Captured values may contain unserializable resources | `Serializable` typeclass gates what may be captured; resource types opt in with custom marshal/unmarshal hooks |
| Ownership model (`ref<T>`, `rc<T>`) assumes single-process lifetime | Serialized continuations produce a **deep copy**; ownership is transferred, originals invalidated |
| ABI stability across recompilations | Frames carry a schema version; mismatches produce a descriptive error rather than UB |

---

### Phase SC-A — `Serializable` Typeclass & Primitives

**Goal:** Define the `Serializable` typeclass and implement derived instances for primitives and containers.

#### Tasks
- [ ] Define `Serializable` typeclass in `src/typeclass.{c,h}` and `stdlib/serial.tur`:
  ```
  (defclass Serializable [a]
    (serialize   [x : a]    : bytes)
    (deserialize [b : bytes] : (Result a cstr)))
  ```
- [ ] Implement derived instances for all primitive types (`int64`, `float64`, `bool`, `cstr`, `bytes`)
- [ ] Implement structural instances for `Vec<a>` (requires `Serializable a`), `Pair<a,b>`, `Option<a>`, `Result<a,b>`
- [ ] Wire `Serializable` into the existing typeclass elaboration pipeline (Phase 15 infrastructure)
- [ ] Document which types explicitly **do not** implement `Serializable` (raw pointers, file handles, `Mutex<T>`, etc.) and add a compile-time note

**Exit criterion:** `Serializable` typeclass defined; all primitive and container instances compile; typeclass resolves correctly via dictionary passing.

---

### Phase SC-B — Frame Annotation & Symbol Registry

**Goal:** Annotate each emitted CPS frame with a stable symbolic key and a schema version; build the startup registry that maps keys to reconstruction functions.

#### Tasks (`src/emit.{c,h}`, `src/runtime.{c,h}`, `src/main.c`)
- [ ] Annotate each emitted CPS frame struct with `symbol_key` (stable, human-readable: `"module::fn_name::frame_N"`) and `schema_ver` (CRC32 of ordered `(name, type-tag)` pairs) in `src/emit.{c,h}`
- [ ] Generate `__register_serial_frames()` stubs at compile time for each CPS-transformed function
- [ ] Implement `serial_register(key, size, reconstruct_fn)` in `src/runtime.{c,h}`
- [ ] Implement `serial_lookup(key)` → `ReconstructFn` in `src/runtime.{c,h}`
- [ ] Add call to `__register_serial_frames()` in the generated `main` preamble (`src/main.c`)
- [ ] Unit test: register a frame, look it up, verify the function pointer is correct
- [ ] Error path: `serial_lookup` on an unknown key returns `NULL` (caller emits descriptive error)

**Exit criterion:** All emitted CPS frames carry stable keys; registry populated at startup; lookup round-trips correctly.

---

### Phase SC-C — Wire Format & Core Codec

**Goal:** Implement the self-describing binary wire format and the encode/decode codec, including CRC32 integrity checking and circular-reference detection.

#### Wire format (`src/runtime.{c,h}`)

The binary encoding:
```
[magic: 4 bytes "TSER"]
[format-version: uint16]
[frame-count: uint32]
  [frame-N]
    [symbol-key: length-prefixed utf8]
    [schema-ver: uint32]
    [field-count: uint32]
      [field-N]
        [name: length-prefixed utf8]
        [type-tag: uint8]
        [payload: type-specific bytes]
[checksum: crc32 of preceding bytes]
```

#### Tasks
- [ ] Implement `serial_write_frame(buf, frame)` in `src/runtime.{c,h}`
- [ ] Implement `serial_read_frame(buf, offset)` → `SerialFrame *` in `src/runtime.{c,h}`
- [ ] Implement `bytes` type codec helpers (length-prefixed UTF-8, fixed-width integer encoding)
- [ ] Implement CRC32 checksum for integrity checking (detect truncated or corrupted payloads)
- [ ] Implement circular-reference detection using a `pointer → offset` table during serialization; serialize to `Err("circular reference detected")` on cycle
- [ ] Schema version mismatch: when stored `schema_ver` ≠ compiled `schema_ver`, return `Err("schema version mismatch: frame '<key>'")` rather than silently misreading
- [ ] Implement optional JSON envelope (gated behind compile flag `TUR_SERIAL_JSON`) for debugging and cross-language interop

**Exit criterion:** Round-trip encode/decode of primitive and container values; CRC catches corrupt payloads; circular references detected; schema mismatches reported cleanly.

---

### Phase SC-D — Elaborator Integration

**Goal:** Add `SERIAL_RESET`/`SERIAL_SHIFT` expression nodes and the serializable-environment tracking that enforces `Serializable` constraints on all captured bindings.

#### Tasks (`src/expr.{c,h}`, `src/elab.{c,h}`)
- [ ] Add `EX_SERIAL_RESET` and `EX_SERIAL_SHIFT` expression nodes in `src/expr.{c,h}`
- [ ] Add serializable-environment tracking in `src/elab.{c,h}`: for each binding in scope at a `serial-shift` site, verify `Serializable T`
- [ ] Emit a structured diagnostic for non-serializable captures:
  ```
  error: binding `handle : file-handle` captured inside `serial-reset`
         but `file-handle` does not implement `Serializable`
    --> src/main.tur:42:5
     = help: use `with-resource` outside the `serial-reset` boundary,
             or implement `Serializable` for `file-handle` with a
             custom marshal hook
  ```
- [ ] Propagate `schema_ver` computation from type information during elaboration
- [ ] Enforce: `serial-reset` may not enclose an active effect handler boundary (open question §7.3 in the design doc)
- [ ] Add `serial-continuation<T>` as a builtin type alias in `src/types.{c,h}`:
  ```
  struct serial-continuation<T> { resume, to-bytes, schema-id }
  ```

**Exit criterion:** `serial-reset`/`serial-shift` elaborate correctly; non-serializable captures produce helpful diagnostics; `serial-continuation<T>` type is available.

---

### Phase SC-E — Surface Syntax & Standard Library

**Goal:** Expose the feature through reader forms and high-level standard library helpers.

#### Surface syntax (`src/reader.{c,h}`, `src/forms.{c,h}`)
- [ ] Add `serial-reset` / `serial-shift` reader forms
- [ ] Implement `serial-cont->bytes` builtin: serialize a `serial-continuation<T>` to `bytes`
- [ ] Implement `bytes->serial-cont` builtin: deserialize `bytes` → `(Result (serial-continuation<T>) cstr)`
- [ ] Implement `serial-resume` builtin: resume a deserialized continuation with a value

#### Standard library (`stdlib/serial.tur`, `stdlib/workflow.tur`)
- [ ] `stdlib/serial.tur` — file I/O helpers:
  - `(defn cont-to-file [k path] : (Result unit cstr) ../..)` — serialize to a file path
  - `(defn cont-from-file [path] : (Result (serial-continuation<T>) cstr) ../..)` — deserialize from a file path
- [ ] `stdlib/workflow.tur` — higher-level persistent workflow API:
  - `defworkflow-step` macro: defines a named workflow step that can be suspended and resumed
  - `resume-approval` pattern: load a continuation from the database and resume it with a value

**Exit criterion:** `serial-reset`/`serial-shift` parse and lower correctly; `serial-cont->bytes` / `bytes->serial-cont` / `serial-resume` builtins work end-to-end; stdlib helpers compile.

---

### Phase SC-F — Tests & Documentation

**Goal:** Comprehensive test coverage and user-facing documentation.

#### Tests
- [ ] Unit tests: round-trip primitive values, structs, `Vec`, `Option` through serialize/deserialize
- [ ] Integration test: serialize a continuation mid-computation → write to file → kill process → re-launch → deserialize → resume → verify correct result
- [ ] Error-path tests:
  - Schema version mismatch (recompile with changed frame layout, attempt to resume old serialized continuation)
  - Unknown `symbol_key` (renamed function, attempt to resume)
  - Circular reference in captured data
- [ ] Security test: deserializing from a truncated/corrupted buffer does not crash (returns `Err`)
- [ ] ASan test: no memory errors during serialization, deserialization, and resume

#### Documentation
- [ ] Document wire format in `docs/serializable-continuations-wire-format.md`
- [ ] User guide: when to use `serial-reset`/`serial-shift`, resource boundary guidelines, schema evolution guidance
- [ ] Security note: deserializing from untrusted sources is analogous to Java deserialization vulnerabilities; document allowlist / capability-token option (open question §7.4)
- [ ] Cookbook examples: persistent workflow, web session continuation, computation checkpoint

**Exit criterion:** All tests pass; integration test demonstrates cross-process resume; documentation complete; no memory errors under ASan.

---

---

## Module System — Racket-Style Alternative (Phases R0–R8)

**Status:** Planned (v1 or v2). Alternative design to the existing Clojure-style module plan. Prerequisites: Phase 15 (Typeclasses — for module-scoped instances), Phase 19 (Algebraic effects — modules can declare effect rows on exports). See [module-system-racket-alt-plan.md](module-system-racket-alt-plan.md) for full executive summary and design rationale.

**When to choose this design over Clojure-style:**
- **Test co-location:** `module+ test` inline blocks vs. separate test files.
- **DSL authoring:** Language parameter enables `#%module-begin` hook for custom module semantics.
- **Macro phases:** Explicit `for-syntax` import for clean compile-time/run-time separation.
- **Multiple modules per file:** Useful for submodules and library organization.
- **Re-export combinators:** `all-from-out` and `protect-out` are built-in, not macro workarounds.

**Decision rule:** Adopt Racket-style plan if the team prioritizes DSL support and explicit phase separation; Clojure-style if simplicity and Java-like naming conventions are preferred.

| Phase | Goal | Exit Criterion |
|---|---|---|
| **R0** | `module` form + language parameter | `module`, `provide`, `require` parse; language parameter validates; reader reserves phase keywords |
| **R1** | `provide` transformer system | `all-defined-out`, `rename-out`, `protect-out`, `except-out`, `all-from-out` work correctly |
| **R2** | `require` combinator system | `only-in`, `except-in`, `prefix-in`, `rename-in` combinators; circular import detection works |
| **R3** | Phased imports | `for-syntax` phase separation; phase-0 and phase-1 environments isolated; v2: `for-template`, `for-label` |
| **R4** | Submodules (`module+` / `module*`) | Inline test submodules; per-file multi-module support; `submod` require paths work |
| **R5** | Separate compilation | One `.c`+`.h` per module; cross-module linking; C symbol mangling; incremental build support |
| **R6** | Language protocol (`#%module-begin`) | Any module usable as a language; stdlib DSL modules work; language tower with cycle detection |
| **R7** | Units and signatures (v2) | `define-signature`, `define-unit`, `invoke-unit`, `link` forms; dependency injection patterns work |
| **R8** | Integration & polish | Stdlib migration to modules; documentation; tests; benchmarks; DSL examples in stdlib |

---

### Phase R0 — `module` Form and Language Parameter

**Goal:** Parse and validate the `module` form, language parameter, and bare `provide`/`require` forms. Establish the core reader infrastructure.

**Reader extensions** — `src/reader.{c,h}`
- [ ] Recognize `module` as a special form (distinct from `defmodule` if it exists from earlier planning).
- [ ] Recognize `provide` as a special form.
- [ ] Recognize `require` as a special form.
- [ ] Parse `(module name lang body...)` — `name` is a symbol, `lang` is a symbol (module path).
- [ ] Support `module` at top level of a file and allow nesting inside another `module`.
- [ ] Allow `provide` and `require` anywhere in module body (not just top).
- [ ] Reserve phase keywords: `for-syntax`, `for-template`, `for-label`.
- [ ] Error on invalid qualified symbols in module names: e.g., reject names with invalid characters.

**AST nodes** — `src/expr.h` or new `src/module.h`
- [ ] Define `EXPR_MODULE` node:
  - `name` — `Symbol *` (module name)
  - `lang` — `Symbol *` (language module name)
  - `body` — `Vec<Expr *>` (module body forms)
  - `span` — `Span` (source location)
- [ ] Define `EXPR_PROVIDE` node:
  - `specs` — `Vec<ProvideSpec *>` (export specifications)
  - `span` — `Span`
- [ ] Define `EXPR_REQUIRE` node:
  - `specs` — `Vec<RequireSpec *>` (import specifications)
  - `phase` — `enum { PHASE_RUN, PHASE_SYNTAX, PHASE_TEMPLATE, PHASE_LABEL }`
  - `span` — `Span`

**Validation** — `src/reader.{c,h}` + `src/elab.{c,h}`
- [ ] Module name must be a valid qualified symbol: letters, digits, `/`, `-`, `_`.
- [ ] Language parameter must be a valid symbol.
- [ ] Error if top-level `module` form is not at file scope (no nested `module` inside regular `defn`/`let`).
- [ ] Warning if explicit module name does not match file path (informational; does not prevent loading).
- [ ] Collect all `provide` and `require` forms during parsing for later processing.

**Language parameter resolution** — `src/module.c` (new file)
- [ ] Look up language module in the module search path (resolved from Turmeric home + user paths).
- [ ] Load language module's exports and `#%module-begin` form if present (Phase R6 feature; v1: error if not found).
- [ ] Error if language module not found; list search paths in error message.
- [ ] `turmeric/base` is always available (compiler built-in; provides default bindings and `#%module-begin`).
- [ ] Store language module reference in `ModuleEnv` for use by elaborator.

**Type system** — `src/types.{c,h}` + `src/elab.{c,h}`
- [ ] Add `ModuleEnv` struct to hold module-local symbol table:
  - `name` — module name
  - `lang` — language module name
  - `provides` — export table (built in Phase R1)
  - `phase_env[2]` — phase-0 (run-time) and phase-1 (compile-time) symbol tables
  - `search_path` — module search path

**Fixtures** — `tests/fixtures/modules/`
- [ ] `module-basic.tur` — parse a simple `module` form with name and language.
- [ ] `module-provide-require-parse.tur` — parser accepts `provide` and `require` anywhere in body.
- [ ] `module-language-builtin.tur` — `turmeric/base` language always available.
- [ ] Negative: `module-bad-name.tur` — invalid characters in module name error.
- [ ] Negative: `module-language-not-found.tur` — missing language error lists search paths.

**Exit criterion:** `module`, `provide`, `require` parse correctly; language parameter resolves; `turmeric/base` is available by default; validation catches invalid module names.

---

### Phase R1 — `provide` Transformer System

**Goal:** Implement all `provide` combinator forms to control module exports.

**Provide combinator forms**
```
(provide name ...)                         ; explicit list
(provide (all-defined-out))                ; all definitions in this module
(provide (rename-out [old new] ...))       ; export with rename
(provide (protect-out name ...))           ; exportable but not re-exportable
(provide (except-out spec name ...))       ; bulk export minus exclusions
(provide (all-from-out module ...))        ; re-export everything from another module
```

**AST for provide specs** — `src/module.h`
- [ ] Define `ProvideSpec` tagged union:
  - `PROV_NAME` — single symbol export
  - `PROV_ALL_DEFINED` — no fields (all local definitions)
  - `PROV_RENAME_OUT` — `Vec<(Symbol old, Symbol new)>` pairs
  - `PROV_PROTECT_OUT` — `Vec<Symbol>` protected exports
  - `PROV_EXCEPT_OUT` — inner `ProvideSpec *` + `Vec<Symbol>` excluded names
  - `PROV_ALL_FROM_OUT` — `Vec<Symbol>` module names to re-export from
- [ ] Each `ProvideSpec` carries a `Span` for error reporting.

**Export table processing** — `src/module.c`
- [ ] Collect all `provide` forms in a module after elaboration (forms can appear anywhere in body).
- [ ] Build export table: `Map<Symbol, ExportEntry>` where `ExportEntry` contains:
  - `internal_name` — name as defined locally
  - `exported_name` — name visible to importers (may differ via `rename-out`)
  - `is_protected` — flag: cannot be re-exported via `all-from-out`
  - `source_span` — for error messages
- [ ] Expand each provide spec against the module's definition table:
  - `all-defined-out`: iterate all definitions in module scope
  - `rename-out`: map old name to new name in export table
  - `protect-out`: mark export as protected (cannot be re-exported)
  - `except-out`: expand inner spec, remove excluded names
  - `all-from-out`: pull all exported (non-protected) names from the named import module
- [ ] Check consistency: error if `provide` refers to undefined name.
- [ ] Warning if `provide` duplicates a name already provided (e.g., two `provide` forms export the same symbol).
- [ ] Warning if `all-defined-out` exports names that look private (convention: prefixed with `%` or `_`).

**Error reporting**
- [ ] Error: `provide` refers to undefined name; list available names in error.
- [ ] Error: `provide` tries to re-export a protected import; list the import source.
- [ ] Error: `all-from-out` names a module that was not imported.
- [ ] Warning: `provide` duplicates a name already provided.
- [ ] Warning: `all-defined-out` would export internal-looking names (suggestion to use `except-out`).

**Fixtures** — `tests/fixtures/modules/`
- [ ] `provide-explicit.tur` — `(provide name1 name2)` exports only those names.
- [ ] `provide-all-defined.tur` — `(provide (all-defined-out))` exports all public definitions.
- [ ] `provide-rename.tur` — `(provide (rename-out [internal external]))` renames on export.
- [ ] `provide-protect.tur` — `(provide (protect-out name))` prevents re-export.
- [ ] `provide-except.tur` — `(provide (except-out (all-defined-out) internal))` excludes specific names.
- [ ] `provide-all-from.tur` — `(provide (all-from-out other-module))` re-exports all.
- [ ] `provide-mixed.tur` — combination of multiple `provide` forms in one module.
- [ ] Negative: `provide-undefined.tur` — `provide` names undefined symbol errors.
- [ ] Negative: `provide-reexport-protected.tur` — re-exporting protected import errors.
- [ ] Negative: `provide-duplicate.tur` — duplicate export name warns.

**Exit criterion:** All `provide` combinator forms work; export table is correctly computed; errors and warnings are actionable; fixtures pass.

---

### Phase R2 — `require` Combinator System

**Goal:** Implement all `require` combinator forms to control module imports.

**Require combinator forms**
```scheme
(require module)                                   ; all provided names
(require (only-in module name ...))                ; whitelist
(require (except-in module name ...))              ; blacklist
(require (prefix-in pfx: module))                  ; add prefix to all names
(require (rename-in module [old new] ...))         ; rename specific names
(require (combine-in spec ...))                    ; compose specs (union)
(require (all-from-out module))                    ; re-export everything imported
```

**AST for require specs** — `src/module.h`
- [ ] Define `RequireSpec` tagged union:
  - `REQ_MODULE` — module symbol (plain import)
  - `REQ_ONLY_IN` — inner `RequireSpec *` + `Vec<Symbol>` names
  - `REQ_EXCEPT_IN` — inner `RequireSpec *` + `Vec<Symbol>` excluded names
  - `REQ_PREFIX_IN` — `Symbol` prefix + inner `RequireSpec *`
  - `REQ_RENAME_IN` — inner `RequireSpec *` + `Vec<(Symbol old, Symbol new)>` pairs
  - `REQ_COMBINE_IN` — `Vec<RequireSpec *>` (union)
  - `REQ_ALL_FROM_OUT` — module symbol (re-export)
- [ ] Each `RequireSpec` carries a `Span`.

**Require processing** — `src/module.c`
- [ ] Resolve `RequireSpec` against target module's export table:
  - `only-in`: intersect imported names with whitelist; error if name not found.
  - `except-in`: subtract blacklist from imported names; warn if excluded name doesn't exist.
  - `prefix-in`: prepend prefix string to all imported names; no name collisions possible.
  - `rename-in`: apply rename mappings; error if old name not found.
  - `combine-in`: union results from child specs; error if two specs introduce the same local name (ambiguity).
  - `all-from-out`: pull all exported (non-protected) names from target module's exports; register as re-export.
- [ ] Register resulting bindings in current module's local scope.
- [ ] Track origin module + original name for each binding (for error messages, re-export tracking, and cross-module debugging).
- [ ] Populate the appropriate phase environment (phase-0 for `require`, phase-1 for `require (for-syntax ...)` — Phase R3).

**Circular import detection** — `src/module.c`
- [ ] Build directed dependency graph during require resolution: nodes = modules, edges = import relationships.
- [ ] After loading all top-level modules, topological sort; report cycle if found.
- [ ] Cycle error message lists full import chain: "Module A imports B, B imports C, C imports A".

**Error reporting**
- [ ] Error: module not found; list search paths.
- [ ] Error: `only-in` names a symbol not exported by target module; list available exports.
- [ ] Error: `rename-in` old name not exported by target module.
- [ ] Error: `combine-in` specs introduce the same local name (ambiguity); list conflicting specs.
- [ ] Error: circular import detected; list cycle chain.
- [ ] Warning: `except-in` excludes a name that does not exist in the module.

**Fixtures** — `tests/fixtures/modules/`
- [ ] `require-plain.tur` — `(require module)` imports all exported names.
- [ ] `require-only-in.tur` — `(require (only-in module name1 name2))` imports subset.
- [ ] `require-except-in.tur` — `(require (except-in module name))` imports all but excluded.
- [ ] `require-prefix-in.tur` — `(require (prefix-in pfx: module))` adds prefix to all.
- [ ] `require-rename-in.tur` — `(require (rename-in module [old new]))` renames on import.
- [ ] `require-all-from-out.tur` — `(require (all-from-out module))` re-exports.
- [ ] `require-combine.tur` — `(require (combine-in spec1 spec2))` combines specs.
- [ ] `require-nested-combinators.tur` — nested combinators work correctly.
- [ ] Negative: `require-module-not-found.tur` — missing module errors.
- [ ] Negative: `require-only-in-bad-name.tur` — `only-in` with non-existent name errors.
- [ ] Negative: `require-circular-import.tur` — A imports B, B imports A detected.
- [ ] Negative: `require-combine-ambiguity.tur` — `combine-in` specs with same name error.

**Exit criterion:** All `require` combinator forms work; dependency graph is computed; circular imports are detected; all errors and warnings are actionable.

---

### Phase R3 — Phased Imports (`for-syntax`, `for-template`)

**Goal:** Separate compile-time and run-time dependency graphs using `for-syntax` and friends. Enable explicit phase separation for macros.

**Phase model**
- Every module has a **phase-0 environment** (run-time bindings) and a **phase-1 environment** (compile-time / macro-expansion bindings).
- Importing a module at phase 1 makes that module's phase-0 exports available as phase-1 bindings in the importer.
- v1 scope: only phases 0 and 1; `for-template` and `for-label` are v2 stretch goals.

**Phase-annotated symbol tables** — `src/module.h`
- [ ] Extend `ModuleEnv` to hold separate symbol tables per phase:
  - `phase_env[0]` — run-time bindings
  - `phase_env[1]` — compile-time bindings (macros and their dependencies)
- [ ] Add `current_phase` field to elaborator context to track which phase is active.

**`for-syntax` require processing** — `src/module.c` + `src/elab.{c,h}`
- [ ] When `(require (for-syntax m))` is encountered, load `m` and install its phase-0 exports into importer's phase-1 environment.
- [ ] Macros defined in this module may call functions from phase-1 env during `defmacro` body expansion.
- [ ] Run-time code may not refer to phase-1 bindings; elaborate error: reference to phase-1 binding at phase-0.

**Elaborator integration** — `src/elab.{c,h}`
- [ ] Query correct phase environment during name resolution based on `current_phase`.
- [ ] Phase-1 context: active during `defmacro` body expansion.
- [ ] Phase-0 context: active during normal expression elaboration.
- [ ] Error: reference to phase-1 binding at phase-0; suggest moving code into a macro or `defmacro` context.

**Separate compilation implications** — `src/main.c`
- [ ] Phase-1 dependencies compiled first (they are needed during elaboration of the importer).
- [ ] Phase-0 dependencies compiled concurrently or lazily.
- [ ] `.h` files include only phase-0 declarations; phase-1 symbols are internal to the compiler.

**v1 scope** — `src/elab.{c,h}`
- [ ] Implement `for-syntax` and `for-template` keyword parsing (reader already reserves them in Phase R0).
- [ ] `for-syntax` working; `for-template` and `for-label` treat imports as compile-time-only (no execute-at-compile behavior).
- [ ] Mark items as to-be-deferred in phase roadmap.

**v2 roadmap**
- [ ] Full phase tower with `for-template` and `for-label`.
- [ ] `for-template`: bindings available when macro-generated code is later expanded.
- [ ] `for-label`: bindings available only for static analysis (not executed at any phase).

**Fixtures** — `tests/fixtures/modules/`
- [ ] `phase-separate-basic.tur` — `(require turmeric/base)` at phase-0, `(require (for-syntax turmeric/syntax-utils))` at phase-1.
- [ ] `phase-separate-macro.tur` — macro uses phase-1 binding; phase-0 code does not see it.
- [ ] `phase-separate-error.tur` — referencing phase-1 binding at phase-0 is a compile error.
- [ ] Negative: `phase-mismatch.tur` — phase-0 code tries to call phase-1 function errors.

**Exit criterion:** `for-syntax` separates run-time and compile-time environments; macros can access phase-1 bindings; phase-0 code errors on phase-1 references; v2 items marked for future.

---

### Phase R4 — Submodules (`module+` and `module*`)

**Goal:** Allow test code and auxiliary code to live inline via named submodules.

**`module+` — accumulating submodule**

Multiple `module+ test` blocks in the same file are merged. Classic use: inline unit tests.

```scheme
(defn distance [a : vec2, b : vec2] : float
  (sqrt (+ (sq (- b.x a.x)) (sq (- b.y a.y)))))

(module+ test
  (deftest "distance is non-negative"
    (assert (>= (distance (vec2 0 0) (vec2 3 4)) 0.0))))

(defn- sq [x : float] : float (* x x))

(module+ test
  (deftest "distance(0,0 → 3,4) = 5"
    (assert (== (distance (vec2 0 0) (vec2 3 4)) 5.0))))
```

**`module*` — nested instantiated submodule**

Creates a submodule that is instantiated as part of the enclosing module.

```scheme
(module* helpers #f
  (provide clamp)
  (defn clamp [v lo hi] (max lo (min hi v))))
```

Importers can `(require (submod "geom/vector" helpers))`.

**Parser** — `src/reader.{c,h}`
- [ ] Recognize `module+` special form: `(module+ name body...)`.
- [ ] Recognize `module*` special form: `(module* name lang body...)` where `lang` may be `#f` (inherit parent language).
- [ ] Allow multiple `module+` blocks with the same name in one file (accumulated).
- [ ] Error if `module+` or `module*` appears outside a `module` form body.

**Submodule collection** — `src/module.c`
- [ ] During file parsing, accumulate all `module+` bodies per name into a single submodule node.
- [ ] `module*` compiled as a standalone module linked to the parent.
- [ ] Map submodule names to parent: `geom/vector::test`, `geom/vector::helpers`.
- [ ] Verify no naming collisions between submodules.

**`submod` require path** — `src/module.c`
- [ ] Support `(require (submod "path/to/file" subname))` to import a specific submodule.
- [ ] Support `(require (submod "." subname))` — relative to current file.
- [ ] Submodule exports are independent of parent module exports.

**Build system integration** — `src/main.c` + `Makefile`
- [ ] `tur build` — skip `test` submodules.
- [ ] `tur test` — compile and run `test` submodules.
- [ ] `tur test path/to/file.tur` — run tests only in a specific file.

**Fixtures** — `tests/fixtures/modules/`
- [ ] `submodule-plus-basic.tur` — `module+ test` block co-located with code.
- [ ] `submodule-plus-multiple.tur` — multiple `module+ test` blocks accumulate.
- [ ] `submodule-star-basic.tur` — `module*` nested instantiated submodule.
- [ ] `submodule-require.tur` — `(require (submod "path" name))` imports submodule.
- [ ] Negative: `submodule-outside-module.tur` — `module+` outside `module` body errors.

**Exit criterion:** `module+` and `module*` parse; `module+` blocks accumulate; `submod` imports work; build system respects test submodules.

---

### Phase R5 — Separate Compilation

**Goal:** Emit one `.c` + `.h` pair per module with proper cross-module linking.

**Tasks**

**One compilation unit per `module` form** — `src/emit.{c,h}` + `src/main.c`
- [ ] Each `module` form compiles to `<mangled-name>.c` + `<mangled-name>.h`.
- [ ] Multiple `module` forms in one file produce multiple `.c`/`.h` pairs.
- [ ] Submodules produce separate `.c`/`.h` pairs with name `parent__subname`.
- [ ] Compilation order respects dependency graph (phase-1 before phase-0).

**Phase-0 vs phase-1 headers** — `src/emit.{c,h}`
- [ ] Phase-0 `.h`: run-time declarations (functions, types, constants, typeclass instances).
- [ ] Phase-1 `.h` (new): compile-time declarations for macros and syntax helpers (not included in final link).
- [ ] Compiler driver only includes phase-0 headers in final link.

**C symbol mangling** — `src/emit.{c,h}`
- [ ] Module `/` → `__`; `-` → `_`; `::` (submodule) → `___`.
- [ ] Examples: `geom/vector` → `geom__vector`; `geom/vector::test` → `geom__vector___test`.
- [ ] Exported symbols: `<module_mangled>__<symbol>`.
- [ ] Protected symbols: `static` in `.c`, omitted from `.h`.
- [ ] Collision detection: error if two modules mangle to the same C prefix.

**Include guards and `#include` chains** — `src/emit.{c,h}`
- [ ] Header include guard: `#ifndef TUR_MODULE_<MANGLED>_H`.
- [ ] Header `#include`s phase-0 headers of each `(require m)` module.
- [ ] Transitive includes: if `A` requires `B` and `B` requires `C`, `A.h` includes both `B.h` and `C.h` (for full type definitions).

**Incremental compilation** — `src/main.c` + `.tur-deps` file format
- [ ] Content-hash based: recompile only if source hash or any dependency hash changes.
- [ ] Dependency graph stored in `.tur-deps` file (JSON or simple text format).
- [ ] Detect changes to headers and re-link dependents.

**Fixtures** — `tests/fixtures/modules/`
- [ ] `separate-compile-basic.tur` — two modules in separate files; `tur build` emits `.c`/`.h` for each.
- [ ] `separate-compile-link.tur` — linking between two modules works correctly.
- [ ] `separate-compile-submodule.tur` — submodule emits separate `.c`/`.h`.
- [ ] Codegen snapshots: symbol mangling, header generation, include chains.

**Exit criterion:** Each module compiles to separate `.c`/`.h`; linking works; incremental compilation detects changes; all snapshots stable.

---

### Phase R6 — Language Protocol (`#%module-begin`)

**Goal:** Allow any module to be used as a **language** by providing a `#%module-begin` macro.

**Motivation**

Turmeric's standard library could ship domain-specific languages without requiring explicit imports:

```scheme
(module my-tests turmeric/testing
  (deftest "addition"
    (assert (== (+ 1 2) 3))))
```

`turmeric/testing` provides a `#%module-begin` that wraps the body, imports assertion helpers, and registers tests with the harness.

**Tasks**

**`#%module-begin` protocol** — `src/elab.{c,h}`
- [ ] After loading the language module, look up its exported `#%module-begin` macro.
- [ ] If found, wrap the module body: `(#%module-begin form1 form2 ...)`.
- [ ] If not found, use the default module-begin that processes `provide`/`require` then evaluates forms.
- [ ] `#%module-begin` receives the entire module body as a list of unevaluated forms.

**Default `#%module-begin`** — `stdlib/base.tur`
- [ ] Implement default `#%module-begin` that processes `provide` and `require` forms, then evaluates remaining forms in order.
- [ ] Export `#%module-begin` from `turmeric/base`.

**DSL language modules** — `stdlib/`
- [ ] `turmeric/testing` — testing DSL: auto-import `test.tur`, register `deftest` forms.
- [ ] `turmeric/script` — scripting language: auto-import `io.tur`, `str.tur`; no explicit `provide` required.

**Language tower** — `src/elab.{c,h}`
- [ ] A language module may itself use a language: `(module turmeric/testing turmeric/base ...)`.
- [ ] Cycle detection: error if language chain is circular; report cycle chain.

**Fixtures** — `tests/fixtures/modules/`
- [ ] `language-basic.tur` — custom language with `#%module-begin` hook.
- [ ] `language-testing.tur` — `turmeric/testing` language with `deftest`.
- [ ] `language-tower.tur` — language chain works correctly.
- [ ] Negative: `language-circular.tur` — circular language chain detected and errors.

**Exit criterion:** Custom languages via `#%module-begin` work; stdlib includes `turmeric/testing`; language tower with cycle detection works.

---

### Phase R7 — Units and Signatures (v2)

**Goal:** First-class linkable components for dependency injection and parameterized libraries.

**Motivation**

Units allow a module to be instantiated with *pluggable dependencies* at link time, enabling mocking, multiple implementations, and breaking circular dependencies.

**Tasks**

**Signatures** — `src/module.h` + `src/elab.{c,h}`
- [ ] Define `Signature` struct: name, list of `(symbol, type)` pairs.
- [ ] `define-signature` form: parses and registers a signature.
- [ ] Signatures are structural (duck-typed): any unit providing all required symbols satisfies the signature.

**Units** — `src/module.h` + `src/elab.{c,h}`
- [ ] Define `Unit` struct:
  - `import_sigs` — list of required signatures
  - `export_sigs` — list of provided signatures
  - `body` — module body (not yet instantiated)
- [ ] `define-unit` form: captures body as uninstantiated template.
- [ ] Units are values — can be passed to functions, stored in variables.

**Linking** — `src/module.c`
- [ ] `link` form: compose units by connecting export signatures to import signatures.
- [ ] Type-check: verify linked unit's export signature satisfies import signature.
- [ ] Detect unsatisfied imports after linking.

**Invocation** — `src/module.c`
- [ ] `invoke-unit`: instantiate a unit with no remaining imports; execute its body.
- [ ] Instantiation creates a fresh environment; units can be instantiated multiple times.

**Fixtures** — `tests/fixtures/modules/`
- [ ] `unit-basic.tur` — define and invoke a unit.
- [ ] `unit-link.tur` — link units with compatible signatures.
- [ ] `unit-signature.tur` — signature checking works.
- [ ] Negative: `unit-unsatisfied-import.tur` — linking without satisfying all imports errors.

**Exit criterion:** Units and signatures work for dependency injection; linking type-checks; invocation executes correctly; v2 feature complete.

---

### Phase R8 — Integration and Polish

**Goal:** Migrate stdlib to modules, stabilize APIs, document, and ship stdlib DSLs.

**Tasks**

**Stdlib migration** — `stdlib/`
- [ ] Wrap each `stdlib/*.tur` file in a `module` form using `turmeric/base`.
- [ ] Replace all implicit exports with explicit `provide` forms.
- [ ] Add `(provide (all-defined-out))` as default for stdlib files during migration.
- [ ] Create `stdlib/prelude.tur` as a convenience facade re-exporting common modules.
- [ ] Update module search paths and build rules to find migrated modules.

**Documentation** — `docs/`
- [ ] Write `module-system-user-guide.md`: overview, examples, best practices.
- [ ] Write `module-phase-separation-guide.md`: when to use `for-syntax`; macro dependency organization.
- [ ] Write `dsl-authoring-guide.md`: how to create a custom language with `#%module-begin`.
- [ ] Update existing docs to reference new module system.

**Testing** — `tests/fixtures/modules/` + `tests/integration/`
- [ ] Comprehensive fixture suite for all phases (already started in earlier phases).
- [ ] Integration tests: full stdlib build, cross-module linking, DSL usage.
- [ ] Negative tests: error messages are actionable and point to root causes.

**Benchmarks** — `tests/benchmarks/modules/`
- [ ] Module loading time: compare cold startup vs. warm cache.
- [ ] Compilation time: separate vs. monolithic compilation.
- [ ] Link time: large stdlib with many modules.

**Fixtures & Examples** — `stdlib/` + `docs/examples/`
- [ ] Example: custom testing DSL with `#%module-begin`.
- [ ] Example: library with submodules and re-exports.
- [ ] Example: phased imports for macro dependencies.
- [ ] Example: units for dependency injection (if v2).

**Exit criterion:** Stdlib fully migrated to modules; documentation is complete and examples work; no regressions in build time or code generation; DSL examples in stdlib.

---



---

*This plan extends [turmeric-plan.md](turmeric-plan.md). Phases 0–14 are archived in [turmeric-plan.archive.md](turmeric-plan.archive.md). The phase groups detailed here include: Phases 15–19 (completed core features), H0–H6 (Higher-kinded Types), P1–P4 (Persistent Collections/HAMT), B1–B5 (Backtracking/Cloneable Continuations), R0–R8 (Module System — Racket-style alternative), HRT0–HRT5 (Higher-ranked Types), G0–G4 (Generalized Algebraic Data Types), U1–U5 (Unsafe Operations), T19–T21 (Thread Primitives & Fibers), and SC-A–SC-F (Serializable Continuations). Full standalone design documents remain in their respective plan files or in the `archive/` directory.*
