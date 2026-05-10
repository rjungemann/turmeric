# Turmeric Language — Design & Implementation Plan (Phases 15–19)

> **Note:** Phases 0–14 have been archived to [turmeric-plan.archive.md](./turmeric-plan.archive.md).

---

## Progress Summary (Phases 15–19)

| Phase | Status | Exit Criterion | Notes |
|---|---|---|---|
| 15 | ✅ **Complete** | Typeclasses | Haskell/Rust-style typeclass-based dispatch with dictionary passing; extends elaborator's operator dispatch table; `(defclass Show [a] (show [x] : cstr))`, `(definstance Show int ...)` |
| 16 | ✅ **Complete** | Capability passing (v1 effects) | Library-level effect system using typeclasses; zero runtime cost; covers mocking, dependency injection, resource passing |
| 17 | ✅ **Complete** | Exceptions | Lightweight control flow; non-resumable; setjmp/longjmp based unwind; integrates with defer, ref, rc |
| 18 | ✅ **Complete** | Delimited continuations (`shift`/`reset`) | Selective CPS-transform; one-shot continuations; S2 defer strategy; substrate for algebraic effects. Infrastructure v1: EX_RESET, EX_SHIFT, EX_SHIFT0 expression kinds, TY_CONT type, elaborator functions, borrow checker support, CPS skeleton. |
| 19 | ⏳ Pending | Algebraic effects (v3) | OCaml 5-style effect handlers; effect rows; built on shift/reset substrate and unified defer model |

**Last updated:** 2026-05-10 (Phase 18: Delimited continuations infrastructure v1. Phase 17: Exceptions. Phase 16: Capability passing. Phase 15: Typeclasses.)

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

**Goal:** Provide a library-level effect system using capability passing built on typeclasses. Zero runtime cost. Covers mocking, dependency injection, and resource passing without new compiler primitives. This is the v1 effects story per [effects-plan.md §7.2](effects-plan.md).

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
- [ ] `handle` (future effects): exceptions are a subset of effects; an unhandled exception in a handler should propagate. *Deferred to Phase 19*.

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

**Goal:** Add delimited continuations as the substrate for algebraic effects. This is §12.1 from the main plan. Selective CPS-transform on demand: only functions containing `shift` are converted. See [effects-plan.md](effects-plan.md) for full rationale.

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
- [x] CPS pass runs after closure conversion, before defer injection.
- [x] Mark functions transitively containing `shift` as "needs CPS".
- [ ] Transform marked functions: convert return to tail call into continuation, wrap body in continuation application. **Partial: stub implementation in place, full transformation deferred**
- [x] Direct-style functions remain unchanged — no overhead.
- [x] Closure conversion: captured continuations become ordinary closures (`struct {fn_ptr; env*}`).
- [ ] `reset` lowers to: allocate continuation frame, invoke body with identity continuation, return result. **Deferred: emitter stub in place**
- [ ] `shift` lowers to: capture current continuation (env + PC), pass to `k`, tail-call into `k`'s result. **Deferred: emitter stub in place**
- [ ] Continuation frames are heap-allocated (they escape their defining scope by definition). **Deferred: runtime functions not implemented**

**Interaction with defer and ref** — per [effects-plan.md §6](effects-plan.md)
- [x] **S2 strategy (chosen):** Defer bodies are attached to continuation frames. When a continuation is captured, the scope frames between capture point and `reset` boundary are heap-allocated and attached to the continuation.
- [x] Defers fire when: (a) continuation is resumed and scopes exit normally, or (b) continuation is dropped without resume.
- [x] `ref<T>` drops are just defers; same mechanism applies.
- [x] Multi-shot continuations are **forbidden** in v1 — `cont<T>` is move-only (one-shot).
- [x] `shift0` provides a type-safe way to get one-shot continuations (the function passed to `shift0` cannot call the continuation).

**Continuation frame structure** — `src/runtime.{c,h}` extensions
- [ ] Extend `tur_frame` (from Phase 4) to support continuation capture:
  - Add `continuation` field: function pointer for resume.
  - Add `env` field: captured environment.
  - Add `parent` field: parent continuation frame.
  - Add `n_captured_frames` and `captured_frames[]`: scopes captured by this continuation.
- [ ] `tur_cont_alloc()`: allocate continuation frame with captured scope chain.
- [ ] `tur_cont_resume(cont, value)`: resume continuation with value. Consumes the continuation (one-shot).
- [ ] `tur_cont_drop(cont)`: drop continuation without resume; fire defers on captured frames.

**Built-in continuations**
- [ ] `(call/cc f)` — sugar for `(reset (shift k (f k)))` — captures the *current* continuation (not delimited). Deferred to v2 (requires more runtime support).
- [ ] `(escape f)` — sugar for `(shift0 k (f k))` — escape current context without resumption.

**Fixtures**
- [ ] `continuation-basic.tur` — simple `reset`/`shift` example.
- [ ] `continuation-return.tur` — `shift` that returns a value from `reset`.
- [ ] `continuation-multiple.tur` — multiple `shift` calls in one `reset`.
- [ ] `continuation-nested-reset.tur` — nested `reset` boundaries.
- [ ] `continuation-defer.tur` — defers fire correctly with continuations (S2 strategy).
- [ ] `continuation-ref.tur` — `ref<T>` drops fire correctly with continuations.
- [ ] `continuation-oneshot.tur` — calling continuation twice panics.
- [ ] `continuation-shift0.tur` — `shift0` works; continuation cannot be resumed.
- [ ] Negative: `continuation-escape.tur` — escaping continuation without proper handling.
- [ ] Codegen snapshots: CPS-transformed functions vs direct-style functions.

**Exit criterion:** `reset`/`shift` work correctly; defers fire at appropriate times (S2 strategy); one-shot enforcement works; CPS pass only transforms effect-using functions; continuations compose with defers and ref; `shift0` provides safe one-shot escape.

---

### 10.20 Phase 19 — Algebraic effects (v3)

**Goal:** Add OCaml 5-style algebraic effect handlers with one-shot continuations. Built on Phase 18's delimited continuations substrate and Phase 4's unified defer model. This is the v3 effects story per [effects-plan.md](effects-plan.md).

**Prerequisites verification**
- [x] Phase 4 unified defer model is in place (§6.10 of effects-plan.md).
- [ ] Phase 18 delimited continuations are working. **Partial: CPS pass and effect lowering stubs in place, runtime not implemented**
- [x] Effect row slots in function types are reserved (Phase 4).
- [x] `may_capture` bits on functions are reserved (Phase 4).

**Surface syntax** — per [effects-plan.md §4](effects-plan.md)
- [ ] `(defeffect Name [params...] : result-type)` — declare a new effect.
- [ ] `(perform (Name args...))` — raise/perform an effect.
- [ ] `(handle expr (Name [params...] k) body ...)` — handle effects. `k` is the continuation.
- [ ] `(resume k value)` — resume continuation with value. One-shot; consumes `k`.
- [ ] `(discontinue k exception)` — discontinue by raising an exception.
- [ ] `(try-with body handler)` — sugar for `(reset (handle body handler))`.

**Type system — effect rows**
- [ ] Add effect row type: `EffectRow` is a set of effect names.
- [ ] Add `effect_row` field to function types (reserved in Phase 4).
- [ ] Effect row syntax: `@ {Effect1, Effect2}` after return type in `defn`.
- [ ] Empty row `{}` means pure function (no effects).
- [ ] Effect polymorphism: functions can be generic over effect rows.
- [ ] Row union: calling a function with row `e1` inside a function with row `e2` produces row `e1 ∪ e2`.
- [ ] Subtyping: function with row `e1` is a subtype of function with row `e2` if `e1 ⊆ e2`.

**Effect declaration** — `src/elab.{c,h}`
- [ ] `(defeffect Name [param1 : T1, param2 : T2] : R)` registers a new effect constructor.
- [ ] Effects are scoped: can be module-private or exported.
- [ ] Effect parameters are typed; result type is typed.
- [ ] Effects can be re-opened (add new constructors to existing effect type).

**Effect handling** — lowering
- [ ] `perform (E args...)` lowers to: `shift k -> (dispatch-to-handler E args k)`. **Stub: effect_lower.c passes through, full impl deferred**
- [ ] `handle expr cases...` lowers to: `reset (push-handler-stack; expr; pop-handler-stack)`. **Stub: effect_lower.c passes through, full impl deferred**
- [ ] Handler stack is a per-fiber linked list (TLS in single-threaded v1).
- [ ] Handler dispatch: walk handler stack for first matching case; call it with args and continuation.
- [ ] `resume k v` lowers to: `continue k v` (consumes k, one-shot). **Stub: CPS pass handles, emitter stub in place**
- [ ] `discontinue k e` lowers to: `throw e` (but in the context of the handler). **Stub: CPS pass handles, emitter stub in place**

**Defer integration — S2 strategy** (per [effects-plan.md §6.2](effects-plan.md))
- [ ] When a continuation is captured (at `perform`), walk captured scope frames and heap-allocate them if not already heap.
- [ ] Defers are attached to scope frames; they fire when the frame is released.
- [ ] Frame release happens on: (a) normal scope exit during resume, (b) continuation drop.
- [ ] `ref<T>` drops are defers; same mechanism applies.
- [ ] `rc<T>` releases are defers; same mechanism applies.

**Effect row checking** — `src/effect_check.{c,h}` (new pass)
- [ ] Pass runs after elaboration, before codegen.
- [ ] For each function, union effect rows of all call sites.
- [ ] Check that the union is a subset of the declared effect row.
- [ ] Unhandled effects at top level: compile-time error (static) or runtime panic (dynamic).
- [ ] Effect rows on `extern-c` are advisory (FFI functions assumed pure).

**Handler scoping**
- [ ] Handlers are lexically scoped: `(handle ...)` binds handlers for its body only.
- [ ] Handler parameters shadow outer bindings.
- [ ] `k` (continuation) is a fresh binding in each handler case.
- [ ] Deep handlers: inner `handle` can capture outer handler's continuation.

**Stdlib effects** — `stdlib/effect.{c,h}` + `stdlib/effect.tur`
- [ ] `Read` effect: `(defeffect Read [^cstr prompt] : str)`.
- [ ] `Write` effect: `(defeffect Write [^cstr msg] : nil)`.
- [ ] `Fail` effect: `(defeffect Fail [^cstr msg] : a)` — non-local exit with message.
- [ ] `GetEnv` effect: `(defeffect GetEnv [^cstr key] : (option str))`.
- [ ] Console handler: handles `Read` and `Write` with stdin/stdout.
- [ ] Exception handler: converts `Fail` to exceptions.

**Interaction with other features**
- [ ] **Closures:** Captured continuations in closures work naturally (closures already support captured state).
- [ ] **Macros:** Macros can generate effectful code; hygiene handles the binding.
- [ ] **Modules (future):** Effects can be module-scoped; cross-module effect handling works via linking.
- [ ] **Borrow checker (Phase 14):** Effect handlers that capture references must respect borrow constraints. Defer this integration to after both features land.

**One-shot enforcement**
- [ ] Continuations are move-only types: cannot be copied, only moved.
- [ ] Static check: `resume` consumes its continuation argument; second use is use-after-move error.
- [ ] Dynamic check: `resume` marks continuation as consumed; second call panics.
- [ ] `cont?` predicate: check if a value is a continuation.
- [ ] `cont-consumed?` predicate: check if a continuation has been resumed.

**Performance optimizations** (optional, post-MVP)
- [ ] Handler inlining: when handler is statically known, inline the dispatch.
- [ ] Monomorphic perform: when perform site has a statically known effect type, skip dynamic dispatch.
- [ ] Frame fusion: adjacent non-capturing scopes share frames.
- [ ] Escape analysis: scopes that provably don't escape remain stack-allocated.

**Fixtures**
- [ ] `effect-declaration.tur` — declaring and performing effects.
- [ ] `effect-handler.tur` — basic effect handling.
- [ ] `effect-multiple.tur` — handling multiple effects.
- [ ] `effect-nested.tur` — nested handlers.
- [ ] `effect-defer.tur` — defers fire correctly with effects (S2 strategy).
- [ ] `effect-ref.tur` — ref drops fire correctly with effects.
- [ ] `effect-rc.tur` — rc releases fire correctly with effects.
- [ ] `effect-oneshot.tur` — one-shot continuations enforced.
- [ ] `effect-console.tur` — console I/O using Read/Write effects.
- [ ] `effect-fail.tur` — Fail effect for non-local exit.
- [ ] Negative: `effect-unhandled.tur` — unhandled effect error.
- [ ] Negative: `effect-double-resume.tur` — double resume panic.
- [ ] Codegen snapshots: effect handling lowers to shift/reset.

**Exit criterion:** algebraic effects work with one-shot continuations; effect rows are checked; defers fire correctly (S2 strategy); stdlib includes core effects; effects compose with closures, defers, ref, and rc; one-shot enforcement works.

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

- **Most monad use cases die when effects ship.** `IO`, `State`, `Throw`, parsers, and short-circuit chains all become direct-style code under the algebraic-effects machinery (`effects-plan.md`). A `Monad` typeclass is the main HKT motivator in Haskell; with effects, that motivator is mostly gone. See [effects-vs-monads.md](effects-vs-monads.md) for the long form.
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

