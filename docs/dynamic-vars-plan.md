# Dynamic Vars -- Implementation Plan (DV0-DV4)

> **Status:** Draft -- Not Started
>
> **Target:** v3 or later
>
> **Prerequisites:** Phase T19 complete (thread primitives: `spawn`, `join`, `pthread_key_t` available in generated C). STM (Phase 19 atomics) recommended for DV4 interaction testing.
>
> **Related:** [session-types-plan.md](session-types-plan.md),
> [advanced-type-system-feasibility-plan.md](advanced-type-system-feasibility-plan.md)
> (§8 Effect Types, §1 Linear Types)
>
> **Last updated:** 2026-05-19 (open questions resolved; no phases complete)

---

## Overview

This plan covers **dynamic vars** -- typed, thread-local, dynamically-scoped mutable cells inspired by Clojure's `^:dynamic` vars and `binding` form. A dynamic var has a global **root binding** (visible to all threads that have not overridden it) and an optional per-thread **binding stack** pushed and popped by `binding` forms.

The key properties:

- **Dynamic scoping** -- a `binding` override is visible to all code called transitively within the `binding` body, not just lexically enclosed code.
- **Thread isolation** -- each thread has its own binding stack; one thread's `binding` does not affect another's view.
- **Type safety** -- the root type is fixed at `defdynamic` time; all overrides must match.
- **Zero overhead when unused** -- reading a var with no active binding is a single global load; no thread-local storage is consulted.

The implementation compiles to C using POSIX thread-local storage (`pthread_key_t`) for the per-thread binding stack and a regular C global for the root value.

---

## Motivation

Several practical problems in Turmeric programs today require passing implicit context through deep call chains:

- **Logging level / log destination** -- every function that logs must accept a logger parameter, or it must use a fixed global.
- **Request context in web handlers** -- request ID, authenticated user, locale all need to be available deep in the call stack without being explicit parameters everywhere.
- **Test fixture injection** -- replacing I/O or DB connections in tests requires either dependency injection plumbing or global mutation that is not scoped to the test.
- **Locale / timezone** -- formatting functions need a thread-local locale without the caller threading it through.

Turmeric's algebraic effects already solve some of these use cases. Dynamic vars are a lighter-weight complement: they require no handler installation, no `perform` at the call site, and no continuation machinery. They are appropriate for configuration-style context that is rarely overridden and never needs to be caught or resumed.

| Property | Effects (`perform`/`handle`) | Dynamic vars (`binding`/`defdynamic`) |
|---|---|---|
| Mechanism | Continuation capture | Binding stack |
| Call-site annotation | `perform` required | Transparent (reads var directly) |
| Handler install cost | `handle` at each boundary | `binding` only when overriding |
| No-override cost | Effect row overhead | Single global load |
| Multi-shot capable | Yes (`^multishot`) | No |
| Composable interceptors | Yes (layered handlers) | Yes (nested `binding`) |
| Thread isolation | Manual (spawn captures frame) | Automatic (per-thread stack) |

---

## Proposed Syntax

```clojure
;; Declare a dynamic var with a root value
(defdynamic *log-level* :int 0)

;; Override for the dynamic extent of a body expression
(binding [*log-level* 2]
  (do-work))

;; Nested overrides compose correctly
(binding [*log-level* 1]
  (binding [*log-level* 3]
    *log-level*)   ; => 3
  *log-level*)     ; => 1

;; Mutate the current thread's top binding frame (no effect on root)
(binding [*log-level* 1]
  (set! *log-level* 4)
  *log-level*)     ; => 4

;; Multiple vars in one binding form
(binding [*log-level* 2
          *locale*   "en-US"]
  (format-and-log "hello"))

;; Read a dynamic var -- just name it
*log-level*        ; => root value if no binding is active, else top binding
```

### Naming convention

Dynamic vars use **earmuffs** (`*name*`) by convention. The compiler does not enforce this syntactically, but `turc` emits a warning (`TUR_W0600`) if a `defdynamic` name does not match `*...*`. The warning is suppressible with `-Wno-dynvar-earmuffs`.

---

## Motivating Examples

### Example 1: Scoped log level

```clojure
(defdynamic *log-level* :int 1)

(defn log [level msg : str] : unit
  (when (>= level *log-level*)
    (println msg)))

(defn process [] : unit
  (log 1 "processing")
  (log 2 "verbose detail"))

(defn run [] : unit
  (process)                         ; logs "processing" only
  (binding [*log-level* 0]
    (process)))                     ; logs both lines
```

### Example 2: Test fixture injection

```clojure
(defdynamic *db* :ptr 0)

(defn query [sql : str] : str
  (db-exec *db* sql))

(defn run-tests [] : unit
  (binding [*db* (open-test-db)]
    (assert! (= (query "SELECT 1") "1"))
    (assert! (= (query "SELECT 2") "2"))))
```

`query` has no explicit db parameter; tests override `*db*` for their scope without touching production code.

### Example 3: Thread-local locale

```clojure
(defdynamic *locale* :str "en-US")

(defn format-currency [amount : float] : str
  (locale-format *locale* amount))

(defn handle-request [req : Request] : Response
  (binding [*locale* (Request.accept-language req)]
    (make-response (format-currency (compute-total req)))))
```

Each request-handling thread sees its own locale without the locale being threaded through every function call.

### Example 4: Binding conveyed to child threads

```clojure
(defdynamic *request-id* :str "")

(defn log-with-id [msg : str] : unit
  (println (str-concat *request-id* ": " msg)))

(defn handle-request [id : str] : unit
  (binding [*request-id* id]
    ;; Spawn a worker -- it inherits the current binding frame snapshot
    (let [t (spawn-conveying (fn [] (log-with-id "worker started")))]
      (log-with-id "main handler")
      (join t))))
```

`spawn-conveying` captures a snapshot of the current binding frame at spawn time. Subsequent changes in the parent thread do not affect the child, and vice versa.

---

## Alternative Approaches

This section shows how the test-fixture-injection use case (Example 2) looks in Haskell's `IORef` + `ReaderT` pattern and in Turmeric's existing algebraic-effects system, so the trade-offs motivating dynamic vars are concrete.

### Haskell: `IORef` + `ReaderT`

```haskell
-- The environment record holds an IORef to the connection so it can be
-- swapped at test time without changing the type of the computation.
data AppEnv = AppEnv { envDb :: IORef Connection }

type App a = ReaderT AppEnv IO a

-- query reads the connection from the environment on every call.
query :: String -> App String
query sql = do
    env <- ask
    db  <- liftIO $ readIORef (envDb env)
    liftIO $ dbExec db sql

-- Production entry point: wire up the real DB.
runApp :: App a -> IO a
runApp action = do
    db    <- openProdDb
    dbRef <- newIORef db
    runReaderT action (AppEnv dbRef)

-- Tests inject a fake by swapping the IORef before running.
runTests :: IO ()
runTests = do
    testDb <- openTestDb
    dbRef  <- newIORef testDb
    let env = AppEnv { envDb = dbRef }
    result <- runReaderT (query "SELECT 1") env
    assert (result == "1")
```

**Cost:** every function that calls `query` must live in `App` (or a suitably constrained `MonadReader AppEnv m`). Adding a new injected dependency widens `AppEnv`, regenerates the `Has*` instances (if using the `Has` typeclass pattern), and forces all callers deeper into the transformer stack. The boilerplate scales linearly with the number of dependencies.

---

### Turmeric: Algebraic Effects

```clojure
;; Declare an effect row for database operations.
(defeffect DbEffect
  (query [sql : str] : str))

;; query is written as a plain perform -- no explicit threading.
(defn query [sql : str] : str
  (perform (DbEffect.query sql)))

;; All downstream callers can call query without any extra parameters;
;; the effect row propagates through the type system automatically.
(defn run-report [] : str
  (str-concat (query "SELECT name") ": " (query "SELECT count")))

;; Production handler: install the real connection at the boundary.
(defn run-with-db [db thunk] : unit
  (handle (thunk)
    [(DbEffect.query sql k)
     (resume k (db-exec db sql))]))

;; Test handler: intercept the effect and return canned responses.
(defn run-tests [] : unit
  (handle
    (do
      (assert! (= (query "SELECT 1") "1"))
      (assert! (= (run-report)) "alice: 42"))
    [(DbEffect.query sql k)
     (resume k (mock-db-exec sql))]))
```

**Cost:** every function that (transitively) calls `query` acquires `DbEffect` in its effect row. This is visible in types and checked by the compiler, which is good for reasoning but means the effect row grows with each additional injected dependency. Installing a handler (`handle`) at each architectural boundary is explicit and required; forgetting one is a type error, but it also means tests must always wrap calls in a `handle` block.

---

### Summary: when to use which

| Situation | Reach for |
|---|---|
| The dependency is an interceptable operation with multiple implementations (prod vs. mock, local vs. remote) | Algebraic effects -- the `handle` boundary enforces the contract |
| The dependency is configuration that is almost always the root value (log level, locale, feature flags) | Dynamic vars -- `binding` only at the rare override sites |
| You want multi-shot resumption, or the handler needs to observe every invocation | Algebraic effects only |
| Deep call chains where threading the parameter is the only cost | Dynamic vars -- zero call-site annotation |
| Cross-thread conveyance with snapshot isolation | Dynamic vars + `spawn-conveying` |

The two mechanisms are orthogonal: `binding` may appear inside `handle` bodies and `perform` may appear inside `binding` bodies without interaction issues.

---

## Interaction with Existing Features

| Feature | Interaction | Notes |
|---|---|---|
| Linear types (`-Xlinear`) | Dynamic vars **may not** hold linear or unique types | A dynamic var can be read by any number of callers; a linear value must have exactly one consumer. `defdynamic` with a `^linear` or `^unique` type is `TUR_E0603`. |
| Algebraic effects | Orthogonal; `binding` is not an effect | `binding` may appear inside `handle` bodies; `perform` may appear inside `binding` bodies. No interaction issues expected. |
| STM (`atomically`) | `binding` inside `atomically` is permitted; `set!` inside `atomically` is disallowed | On STM retry, the `binding` frame is re-established (the pushed frame is part of the block's stack state). `set!` inside `atomically` is rejected with `TUR_E0605` because mutation of the binding stack could not be rolled back on retry without full undo machinery. |
| Threads (Phase T19) | Per-thread binding stacks are isolated by default | `spawn` does NOT convey bindings. `spawn-conveying` takes a snapshot of the calling thread's current binding frame and installs it as the initial frame on the new thread. |
| Borrow checker / `rc<T>` | Dynamic vars holding `rc<T>` are fine | `rc<T>` is `CK_COPY`; each read of the var clones the reference count. The root value is protected by a mutex during updates. |
| Effect row polymorphism (`-Xeffect-types`) | No interaction | `binding`/`set!` are not effects and do not appear in effect rows. |
| Substructural types (`-Xsubstructural`) | Affine and relevant types are also disallowed in dynamic vars | Same reasoning as linear types; `TUR_E0603` covers all substructural capability kinds weaker than `CK_COPY`. |
| GADTs (`-Xgadt`) | No interaction | Dynamic var type is monomorphic; GADTs may be stored if the GADT type is `CK_COPY`. |
| Session types (`-Xsessions`) | Session channels are `^linear`; `TUR_E0603` blocks them | A session channel cannot be stored in a dynamic var. |
| Modules | `defdynamic` is a module-level form; dynamic vars are exported like `defn` | A module may declare a dynamic var private with `(defdynamic ^private *x* ...)`. |

---

## Architecture

```
src/compiler/types.h        -- TY_DYNVAR (new TypeKind), DynVarType struct
src/compiler/elab_forms.c   -- defdynamic elaboration; binding form; set! for dynamic vars
src/compiler/elab_toplevel.c -- register defdynamic at module scope; export handling
src/compiler/emit_expr.c    -- emit read of dynamic var (pthread_getspecific + stack walk / global fallback)
src/compiler/emit_stmt.c    -- emit binding push/pop (using GCC __attribute__((cleanup)) for safety)
src/compiler/emit_module.c  -- emit pthread_key_t declarations and root-value globals
src/compiler/diag.h         -- TUR_E0600-TUR_E0605 (see error codes below)
stdlib/dynvar.tur           -- common dynamic vars (*locale*, *log-level*, *random-seed*, etc.)
tests/fixtures/dynvar-*/    -- happy-path and negative fixtures
```

---

## Pre-Phase Prerequisites

### P0 -- Error Code Range Allocation

- [ ] Lock the `TUR_E0600`-`TUR_E0605` range for dynamic vars before any DV phase emits error codes. The `TUR_E05xx` block (multishot continuations) ends at `TUR_E0502`; `TUR_E0600` is the next clean block.

  Proposed allocation:

  | Code | Meaning |
  |---|---|
  | `TUR_E0600` | `set!` on a non-dynamic var |
  | `TUR_E0601` | `set!` on a dynamic var with no active `binding` frame on the current thread |
  | `TUR_E0602` | type mismatch: override value does not match `defdynamic` type |
  | `TUR_E0603` | dynamic var declared with a substructural (linear, affine, relevant, or unique) type |
  | `TUR_E0604` | `defdynamic` at non-toplevel position |
  | `TUR_E0605` | `set!` on a dynamic var inside an `atomically` block |

### P1 -- Thread Primitives Audit

- [ ] Confirm Phase T19 provides:
  - `pthread_key_t` creation and destruction in generated C (`pthread_key_create`, `pthread_key_delete`)
  - `pthread_getspecific` / `pthread_setspecific`
  - `spawn` with a zero-arg closure that can capture a value payload (needed to pass the conveyed frame snapshot to the new thread)
- [ ] Confirm that `__attribute__((cleanup(fn)))` is available in the C compiler targeted by `turc` (GCC and Clang both support it; MSVC does not -- flag as a Windows porting concern)

### P2 -- Test Fixture Baseline

- [ ] Create fixture baseline alongside DV0:
  - Happy path: `dynvar-read` (root), `dynvar-binding` (override), `dynvar-nested` (nested overrides), `dynvar-set` (`set!` in binding), `dynvar-multi` (multiple vars)
  - Negative: `dynvar-set-no-binding`, `dynvar-set-non-dynamic`, `dynvar-type-mismatch`, `dynvar-linear-type`, `dynvar-set-in-atomic`
  - All intentionally red until DV1-DV2 implement the operations.

---

## Phase DV0 -- Data Model

**Goal:** Add the `defdynamic` top-level form and `TY_DYNVAR` to the type system. No binding stack or codegen yet.

- [ ] Add `TY_DYNVAR` to `TypeKind` in `src/compiler/types.h`:

  ```c
  TY_DYNVAR,  /* the type of a dynamic var reference (not the stored value) */
  ```

  `TY_DYNVAR` wraps the var's declared value type. Reading a `*name*` var produces a value of the declared type (not `TY_DYNVAR`); the `TY_DYNVAR` node is only held in the environment during elaboration.

- [ ] Add `DynVarEntry` to the elaborator's global symbol table:

  ```c
  typedef struct DynVarEntry {
      const char *name;        /* "*log-level*" */
      Type       *value_type;  /* the declared element type */
      int         index;       /* stable integer ID; used as pthread_key_t index */
  } DynVarEntry;
  ```

- [ ] Elaborate `(defdynamic *name* :type root-expr)`:
  - Resolve `:type` to a `Type *`
  - Reject if type is substructural (`TUR_E0603`)
  - Reject if not at module toplevel (`TUR_E0604`)
  - Elaborate `root-expr` and check it matches `:type` (`TUR_E0602`)
  - Register `DynVarEntry` in the module's dynvar table
- [ ] Add `TUR_E0603` and `TUR_E0604` to `diag.h` / `diag.c`
- [ ] Warn (`TUR_W0600`) if name does not match `*...*` pattern
- [ ] Baseline fixtures from P2 already present; all intentionally red

---

## Phase DV1 -- Elaborator: `binding` and `set!`

**Goal:** Elaborate `binding` and `set!` for dynamic vars; complete the type-checking story.

### `binding` form

`(binding [var1 expr1 var2 expr2 ...] body)` is a special form (not a macro). The elaborator:

- Resolves each `varN` to a `DynVarEntry`; emits `TUR_E0600` if `varN` is not a dynamic var
- Elaborates each `exprN` and checks the type against the var's declared type; emits `TUR_E0602` on mismatch
- Elaborates `body` in a scope where each var's "current binding" is the overriding type (still the same value type -- `binding` does not change the type, only the runtime value)
- Result type is the result type of `body`

`binding` is syntactically a vector of `[var expr ...]` pairs (even count required; odd count is a parse error).

### `set!` for dynamic vars

`(set! *name* expr)` when `*name*` is a `DynVarEntry`:

- Elaborate `expr`; check type against var's declared type (`TUR_E0602`)
- Verify a `binding` frame for `*name*` is in scope on the current thread at elaboration time if statically determinable; otherwise emit a runtime check that fires `TUR_E0601`
- Reject if inside `(atomically ...)` block (`TUR_E0605`)
- Returns `:unit`

The existing `set!` path in `elab_forms.c` dispatches on whether the target is a mutable local, a struct field, or -- after this phase -- a dynamic var. Add the `DynVarEntry` case.

### Error codes added this phase

- `TUR_E0600` -- `set!` on a non-dynamic var
- `TUR_E0601` -- `set!` outside any active `binding` frame
- `TUR_E0602` -- type mismatch in `binding` or `set!`
- `TUR_E0605` -- `set!` inside `atomically`

---

## Phase DV2 -- Codegen

**Goal:** Emit the pthread_key_t binding stack and generate correct C for all dynamic var operations.

### Root value storage

Each `defdynamic` emits a C global for its root value and a `pthread_key_t` for its per-thread binding stack:

```c
/* generated for (defdynamic *log-level* :int 0) */
static int64_t _dynvar_root_log_level = 0;
static pthread_key_t _dynvar_key_log_level;

/* called once at program init (before main) */
__attribute__((constructor))
static void _dynvar_init_log_level(void) {
    pthread_key_create(&_dynvar_key_log_level, free);
}
```

### Binding frame stack

A binding frame is a singly-linked list node allocated on the C stack using a local struct:

```c
typedef struct TurDynFrame {
    struct TurDynFrame *prev;
    void               *value;  /* pointer to value slot below */
} TurDynFrame;
```

`(binding [*log-level* 2] body)` emits:

```c
{
    int64_t _bind_val_log_level = 2;
    TurDynFrame _frame_log_level = {
        .prev  = pthread_getspecific(_dynvar_key_log_level),
        .value = &_bind_val_log_level,
    };
    pthread_setspecific(_dynvar_key_log_level, &_frame_log_level);

    /* cleanup runs when the block exits (normal or via longjmp) */
    void _pop_log_level(TurDynFrame **fp) {
        pthread_setspecific(_dynvar_key_log_level, (*fp)->prev);
    }
    TurDynFrame *_fp_log_level
        __attribute__((cleanup(_pop_log_level))) = &_frame_log_level;

    /* body */
    ...
}
```

> **Note:** `__attribute__((cleanup))` runs the cleanup function when the enclosing block exits by any means, including `longjmp` (used by Turmeric's effect system). This is the key property that makes `binding` safe in the presence of non-local exits. If `__attribute__((cleanup))` is unavailable (e.g. MSVC), an explicit push/pop with `tur_setjmp`-aware cleanup is needed. Document as a porting concern.

### Reading a dynamic var

`*log-level*` emits:

```c
({
    TurDynFrame *_f = pthread_getspecific(_dynvar_key_log_level);
    _f ? *(int64_t *)_f->value : _dynvar_root_log_level;
})
```

A compiler optimization pass can hoist the `pthread_getspecific` call if the var is read multiple times in a hot loop with no intervening `binding` or `set!` forms.

### `set!` on a dynamic var

`(set! *log-level* 4)` inside a `binding` emits:

```c
{
    TurDynFrame *_f = pthread_getspecific(_dynvar_key_log_level);
    if (!_f) { tur_panic("set! on dynamic var with no active binding"); }
    *(int64_t *)_f->value = 4;
}
```

---

## Phase DV3 -- Binding Conveyance

**Goal:** Allow child threads to inherit a snapshot of the parent's binding frame.

### `spawn` vs. `spawn-conveying`

- `spawn closure` -- the child thread starts with no binding frames; all var reads see root values.
- `spawn-conveying closure` -- the child thread starts with a snapshot of the parent's current binding frame for all dynamic vars. Changes in the parent after `spawn-conveying` returns do not affect the child, and vice versa.

### Implementation

`spawn-conveying` captures the current binding values for all registered dynamic vars and passes them to the new thread as an allocation:

```c
/* emitted by spawn-conveying */
TurBindingSnapshot *_snap = tur_binding_snapshot_capture();
tur_spawn_conveying(_snap, _closure);

/* on the new thread, before running the closure: */
tur_binding_snapshot_install(_snap);
tur_binding_snapshot_free(_snap);
```

`tur_binding_snapshot_capture` walks all registered `pthread_key_t` keys and records the current top-of-stack value for each. The snapshot is a flat array of `(key_index, value_copy)` pairs.

`tur_binding_snapshot_install` pushes one binding frame per recorded key onto the new thread's per-key stack.

The snapshot values are **copied** (via the var type's copy semantics), not shared. This preserves isolation.

### Stdlib helper

```clojure
;;; spawn-conveying -- spawn a thread that inherits the current binding frame.
;;;
;;; Parameters:
;;;   f -- zero-arg closure to run on the new thread
;;;
;;; Returns:
;;;   A thread handle (same as spawn).
;;;
;;; Example:
;;;   (binding [*log-level* 2]
;;;     (spawn-conveying (fn [] (log 1 "in child"))))
;;;
;;; Since: DV3
(defn spawn-conveying [f] :thread ...)
```

---

## Phase DV4 -- Integration

**Goal:** Stdlib dynamic vars, effect integration guidance, and complete documentation.

- [ ] Add common dynamic vars to `stdlib/dynvar.tur`:
  - `*log-level* : int` (default `1`)
  - `*locale* : str` (default `"en-US"`)
  - `*random-seed* : int` (default `0` -- `0` means use system entropy)
  - `*current-module* : str` (default `""` -- set by module preamble, useful for structured logging)
- [ ] Document interaction pattern with algebraic effects: dynamic vars for configuration-style context, effects for interceptable operations. Provide a guide section "Effects vs. Dynamic Vars" in `docs/guides/`.
- [ ] `tur explain TUR_E0600`, `TUR_E0601`, `TUR_E0602`, `TUR_E0603`, `TUR_E0604`, `TUR_E0605` entries
- [ ] Integration tests:
  - Scoped log level (Example 1)
  - Test fixture injection (Example 2)
  - Thread-local locale (Example 3)
  - `spawn-conveying` snapshot isolation (Example 4)
  - Negative: `set!` outside `binding`, type mismatch, linear type rejection, `set!` in `atomically`

---

## Complexity Assessment

| Aspect | Complexity | Notes |
|---|---|---|
| Data model (DV0) | Low | `TY_DYNVAR` is a thin wrapper; `DynVarEntry` is a simple struct |
| Elaborator: `binding` (DV1) | Low-Medium | Special form; dispatches on `DynVarEntry`; type check is straightforward |
| Elaborator: `set!` integration (DV1) | Low | Piggybacks existing `set!` dispatch; adds `DynVarEntry` case |
| Codegen: `pthread_key_t` + frame stack (DV2) | Medium | `__attribute__((cleanup))` for correctness with longjmp; one-time setup per var |
| Codegen: root value global (DV2) | Low | Trivial C global + constructor |
| Binding conveyance (DV3) | Medium | Snapshot capture and install; requires iterating all registered keys |
| Stdlib vars (DV4) | Low | Mechanical; modelled on existing stdlib patterns |
| Effect interaction documentation (DV4) | Low | Prose only; no code changes |

---

## Feature Flag

```sh
turc -Xdynamic-vars myfile.tur
```

Enabling `-Xdynamic-vars` makes `defdynamic`, `binding` (the dynamic-var form), and the dynamic-var `set!` path available. It does not affect the existing `binding` form used for local destructuring (if any) -- the two are syntactically distinguishable by whether the bound names are registered `DynVarEntry` symbols.

`-Xdynamic-vars` does not imply any other feature flag.

---

## Implementation Priority

**Low-Medium** -- v3 or later, as a convenience feature after session types (SS0-SS4) are stable.

Dynamic vars are not on the critical path for any core type-system feature. They are valuable for library authors and for ergonomic test injection, but the same use cases can be served (with more boilerplate) by explicit parameters or algebraic effects. The implementation cost is low and the codegen is straightforward.

Recommend implementing DV0-DV2 together as a single sprint once Phase T19 is confirmed stable, then DV3 (conveyance) as a follow-on if thread-based usage patterns demand it.

---

## Open Questions

1. **`alter-root`:** Should there be an explicit form for mutating the root binding (analogous to Clojure's `alter-var-root`)? Mutating the root is a global side effect and should require an explicit acknowledgement. A tentative `alter-root!` form with a prominent warning is one option; alternatively, the root can only be set at `defdynamic` declaration time (no post-declaration root mutation).

   **Decision:** Provide `(alter-root! *name* new-val)` gated behind a `-Xunsafe-alter-root` flag. Without the flag, root mutation is disallowed after module initialization.

2. **`binding` and effect handler boundaries:** If a `binding` form spans an effect handler's resume boundary (i.e., the body of `binding` performs an effect and is resumed later), the binding frame must survive across the continuation. Since the frame lives on the C stack and continuations are implemented via `longjmp`/`setjmp`, the frame's stack slot may have been reclaimed. Does this require heap-allocating the frame in the presence of effect handlers?

   **Decision:** Heap-allocate binding frames when `-Xeffect-types` is active and a `binding` body contains a `perform` site (detectable at elaboration time). Stack allocation remains the default when no `perform` is in scope.

3. **`binding` with a non-`DynVarEntry` name:** If `(binding [x 1] ...)` appears where `x` is a plain local (not a dynamic var), should it be a type error or should it shadow the local lexically? This overlaps with the existing use of `let` for local shadowing. The safest answer is: `binding` requires all names to be `defdynamic` vars; use `let` for local shadowing.

   **Decision:** `binding` with a non-dynamic-var name is `TUR_E0600`. Use `let` for lexical shadowing.

4. **Windows portability:** `__attribute__((cleanup))` is not available on MSVC. If Windows support is added (see `windows-support-plan.md`), the binding stack push/pop must use a portable alternative.

   **Decision:** Wrap every `binding` body in a helper function that takes the frame pointer and a cleanup callback. The helper calls the body, then unconditionally pops the frame on return -- no platform-specific `__try`/`__finally` needed. This is the default codegen path; `__attribute__((cleanup))` is used as an optimisation on GCC/Clang when available (detected via `#ifdef __GNUC__`).

---

## Prior Art

- **Clojure (`^:dynamic` / `binding` / `set!`):** Direct inspiration. Clojure uses per-thread `Var` frames backed by the JVM's thread-local storage; `binding-conveyor-fn` conveys frames to `future` workers.
- **Racket (`make-parameter` / `parameterize`):** Similar semantics; parameters are first-class values (unlike Clojure's module-level vars). Racket's `parameterize` is the macro equivalent of `binding`.
- **Common Lisp (special variables):** Dynamic binding via `defvar` / `let`; the original dynamic-scoping design that influenced both Clojure and Racket.
- **Haskell (`IORef` + `ReaderT`):** No built-in dynamic vars; the Haskell pattern is explicit `ReaderT env IO` threading, or `Data.IORef` in combination with thread-local mocks. More explicit but more boilerplate.

---

## References

- [Clojure Vars and the Global Environment](https://clojure.org/reference/vars)
- [Racket Parameters](https://docs.racket-lang.org/reference/parameters.html)
- POSIX `pthread_key_create(3)` / `pthread_getspecific(3)`
- GCC `__attribute__((cleanup))` documentation
- [advanced-type-system-feasibility-plan.md](advanced-type-system-feasibility-plan.md)
