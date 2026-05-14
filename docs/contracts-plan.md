# Runtime Contracts Plan for Turmeric

**Status:** C0 and C1 complete. C2+ planned.

**Author:** [TBD]

**Last Updated:** 2026-05-14

---

## Summary

This document proposes adding runtime contract checking to Turmeric. Runtime contracts complement Turmeric's strong static type system by catching logic errors, validating API boundaries (especially FFI), and providing executable documentation. The design prioritizes zero-cost in release builds, ergonomic syntax, and integration with the existing type system and exception infrastructure.

---

## Motivation

### Problems Not Solved by Static Types

Turmeric's type system, borrow checker, and reference counting eliminate entire classes of errors at compile time:

| Error Class | Handled By | Static? |
|---|---|---|
| Type mismatches | Type system | ✅ |
| Use-after-free | Borrow checker | ✅ |
| Double-free | RC + borrow checker | ✅ |
| Null pointer dereference | Type system (no null) | ✅ |
| **Value invariants** | *Nothing currently* | ❌ |
| **FFI input validation** | *Nothing currently* | ❌ |
| **Precondition violations** | *Nothing currently* | ❌ |
| **Postcondition violations** | *Nothing currently* | ❌ |

Runtime contracts address the gaps where static analysis is fundamentally limited.

### Use Cases

1. **FFI Boundaries**: Validating inputs from and outputs to C functions
   ```lisp
   (extern-c malloc [size :int] :ptr)
   
   (defn safe-malloc [^Positive size :int] : (option :ptr)
     (require! (> size 0) "malloc size must be positive")
     (let [p (malloc size)]
       (if (= p null) none (some p))))
   ```

2. **API Documentation**: Self-documenting public interfaces
   ```lisp
   (defn vec-get [^InBounds vec : (vec int) i :int] :int
     (require! (and (>= i 0) (< i (vec/len vec))) "index out of bounds")
     (vec/index vec i))
   ```

3. **Logic Invariants**: Complex properties that can't be expressed in types
   ```lisp
   (defn binary-search [vec : (vec int) target :int] : (option int)
     (require! (vec/sorted? vec) "vector must be sorted")
     ;; ...
     (ensure! (or (none? result) (== target (vec/get vec (some result))))
               "result must match target or be none"))
   ```

4. **Module Boundaries**: Enforcing module-level assumptions
   ```lisp
   (defn parse-json [s :cstr] : (result Json ParseError)
     (ensure! (valid? result) "parse-json always returns valid Json or error")
     result)
   ```

5. **Property-Based Testing**: Integration with fuzzing
   ```lisp
   (defn prop-reverse-twice-identity [v : (vec int)]
     (let [result (vec/reverse (vec/reverse v))]
       (ensure! (vec/equal? v result) "reverse twice must be identity")))
   ```

---

## Design

### Guiding Principles

1. **Zero-cost in release**: Contract checks compile to no-ops when disabled
2. **Progressive adoption**: Opt-in, no breaking changes to existing code
3. **Integrate with exceptions**: Use existing `panic`/`throw` infrastructure
4. **Leverage types**: Contract predicates should be type-checked
5. **FFI-first**: Primary focus is validating external boundaries

### Contract Types

| Contract Type | Syntax | When Checked | Purpose |
|---|---|---|---|
| **Assertion** | `(assert! condition)` | Always | Internal sanity check |
| **Assertion with msg** | `(assert! condition msg)` | Always | Internal sanity check with message |
| **Precondition** | `(require! condition)` | Function entry | Input validation |
| **Precondition with msg** | `(require! condition msg)` | Function entry | Input validation with message |
| **Postcondition** | `(ensure! condition)` | Function exit | Output validation |
| **Postcondition with msg** | `(ensure! condition msg)` | Function exit | Output validation with message |
| **Invariant** | `(invariant! obj predicate)` | Before/after operations | Struct/class property |

### Syntax Options

#### Option A: Macro-based (Recommended for v1)

```lisp
;; Basic assertions
(assert! (> x 0))
(assert! (vec/in-bounds? vec i) "index out of bounds")

;; Preconditions - checked at function entry
(defn sqrt [x :float] :float
  (require! (>= x 0.0) "sqrt requires non-negative input")
  ;; ...
  )

;; Postconditions - checked at all exit points
(defn add-positive [a :int b :int] :int
  (ensure! (> (+ a b) 0) "sum must be positive")
  (+ a b))
```

Pros: No compiler changes, can be implemented in stdlib today.
Cons: Slightly verbose, less integrated with type system.

#### Option B: Metadata-based (v2)

```lisp
(defn sqrt [x :float] :float
  {:pre [(>= x 0.0)]
   :post [(>= result 0.0)]}
  (math/sqrt x))

;; Or with named conditions
(defn vec-get [vec : (vec int) i :int] :int
  {:pre [(vec/in-bounds? vec i)] :post-sym result-exists}
  (vec/index vec i))
```

Pros: More declarative, cleaner syntax, better integration.
Cons: Requires compiler support for metadata attachment and checking.

#### Option C: Type-level predicates (Future)

```lisp
;; Predicate types as refinements
(defpred Positive [x :int] (> x 0))
(defpred InBounds [vec : (vec a) i :int] (and (>= i 0) (< i (vec/len vec))))

defn sqrt [^Positive x :float] : ^NonNegative float
  (math/sqrt x))

defn vec-get [vec : (vec int) ^InBounds(vec) i :int] :int
  (vec/index vec i))
```

Pros: Most expressive, enables static checking of predicates.
Cons: Requires significant type system extensions (refinement types).

**Decision**: Start with Option A (macro-based) for v1, migrate to Option B (metadata) for v2 if adoption is high. Option C (refinement types) is a separate, larger feature.

---

## Phases

### Phase C0 — Design & Prerequisites Verification

**Status: COMPLETE**

**Goal:** Finalize design decisions and verify all prerequisites are met.

**Prerequisites Check**
- [x] Phase 17 (Exceptions) is stable -- contracts use `panic` infrastructure
- [x] Phase 15 (Typeclasses) is stable -- for predicate typeclasses like `Show` on error messages
- [x] `stdlib/exn.tur` has `panic` with message support
- [ ] Compiler flag infrastructure exists (`--debug`, `--release`) -- not yet; `contract-enabled?` always returns true in v1

**Design Decisions (Locked)**
- [x] Syntax: two fixed-arity macros per type (`assert!`/`assert-msg!`, etc.) following `must!`/`must-msg!` convention
- [x] Contract failure behavior: always `panic` in v1; configurable failure left for C2
- [x] Message format: any runtime expression (string literals or computed strings)
- [x] Compile-time stripping: `contract-enabled?` returns `true` always in v1; `--no-contracts` flag deferred to C2
- [x] Contracts in `extern-c`: allowed on Turmeric side (wrap the call with require-msg!/ensure-msg!)
- [x] Contract inheritance: not applicable in v1 (no class hierarchy support)

**Exit Criterion:** All design questions answered; no open ambiguities remain. ✓

---

### Phase C1 — Core Contract Macros (v1)

**Status: COMPLETE**

**Goal:** Ship a stdlib module with contract-checking macros that work today with no compiler changes.

**Location:** `stdlib/contract.tur` (and mirrored in `stdlib/macros.tur` for auto-loading)

**Macros**

```lisp
;; Assertion - unconditional check
(defmacro assert! [condition & [msg]]
  (if (compiling-in? :release)
    ;; Release build: compile to nil (no-op)
    (if msg
      `(do))
      `(do))
    ;; Debug build: check and panic
    (if msg
      `(unless ~condition (panic ~msg)))
      `(unless ~condition (panic "Assertion failed")))))

;; Precondition - checked at function entry
(defmacro require! [condition & [msg]]
  ;; Same as assert! but semantically different (documentation)
  `(assert! ~condition ~@(when msg [msg])))

;; Postcondition - checked at all exit points (requires wrapper)
(defmacro ensure! [condition & [msg]]
  ;; This is tricky without compiler support - see implementation below
  `(let [result# ~condition]
     (assert! result# ~@(when msg [msg]))
     result#))
```

**The Postcondition Problem**

Postconditions need to be checked at *all* exit points (normal return, early return, exception). Without compiler support, this is error-prone:

```lisp
;; Manual approach (error-prone)
(defn add-positive [a :int b :int] :int
  (let [result (+ a b)]
    (ensure! (> result 0) "sum must be positive")
    result))
;; This works for normal exit but NOT for exceptions

;; Better approach: wrapper macro
(defmacro defn-with-post [name args ret-type postcondition & body]
  `(defn ~name ~args ~ret-type
     (let [result# (do ~@body)]
       (ensure! ~postcondition)
       result#)))

;; But this doesn't handle early returns or exceptions
```

**Solution for v1:** Use a `try`/`finally` pattern for postconditions:

```lisp
(defmacro defn-with-contract [name args ret-type & {:pre [] :post []} body]
  (let [pre-checks (map (fn [c] `(require! ~c)) pre)
        post-check (when (seq post)
                     `(finally (ensure! ~(first post))))]
    `(defn ~name ~args ~ret-type
       ~@pre-checks
       (try
         ~@body
         ~post-check
         ))))
```

This still doesn't catch all exit paths (exceptions bypass `finally` in some cases). Full postcondition support requires compiler integration (Phase C2).

**Implementation Notes**

Macros call `tur-contract-check [condition :bool msg :cstr] :void` (a C-inline helper) rather than
expanding to `(unless condition ...)`. The `unless` approach drops the return statement when the same
variable appears in both the condition and the function return expression (linear/move semantics).
Passing condition as `:bool` evaluates it first, leaving the original variable intact.

Variadic macros have a ct_eval limitation where `=` is evaluated at compile time, so each contract type
is split into two fixed-arity macros: `assert!`/`assert-msg!`, `require!`/`require-msg!`, etc.

**Stdlib Module** -- `stdlib/contract.tur`
- [x] `(assert! condition)` -- panic if condition is false
- [x] `(assert-msg! condition msg)` -- panic with custom message
- [x] `(require! condition)` -- same as assert, semantic marker for preconditions
- [x] `(require-msg! condition msg)` -- precondition with message
- [x] `(ensure! condition)` -- postcondition check
- [x] `(ensure-msg! condition msg)` -- postcondition with message
- [x] `(invariant! obj predicate)` -- check struct invariant
- [x] `(invariant-msg! obj predicate msg)` -- invariant with message
- [x] `(contract-enabled?)` -- returns true (always enabled in v1)

**Fixtures** -- `tests/fixtures/`
- [x] `contract-assert/` -- basic assertion passing
- [x] `contract-assert-fail/` -- assertion failure panics with correct message
- [x] `contract-require/` -- precondition checks at function entry
- [x] `contract-require-fail/` -- precondition failure panics
- [x] `contract-ensure/` -- postcondition checks
- [x] `contract-ensure-fail/` -- postcondition failure panics
- [x] `contract-invariant/` -- struct invariant validation
- [x] `contract-invariant-fail/` -- invariant failure panics
- [x] `contract-ffi/` -- contracts at FFI boundaries (using llabs)
- [x] `contract-nested/` -- nested function calls with contracts
- [x] `contract-release/` -- `contract-enabled?` returns true

**Exit Criterion:** `stdlib/contract.tur` works; all 11 fixtures pass. ✓

---

### Phase C2 — Compiler-Level Contract Support (v2)

**Goal:** Add first-class contract support with proper postcondition checking at all exit points.

**Surface Syntax**
```lisp
;; Function with contracts
(defn sqrt [x :float] :float
  {:pre [(>= x 0.0)]
   :post [(>= result 0.0)]}
  (math/sqrt x))

;; Multiple pre/post conditions
(defn divide [a :int b :int] :int
  {:pre [(!= b 0) "divisor cannot be zero"]
   :post [(== (* result b) a) "result * divisor equals dividend"]}
  (/ a b))

;; Struct invariants
(defstruct Vector [x :float y :float z :float]
  {:invariant [(> (math/length self) 0) "vector must have positive length"]})
```

**Type System Extensions** — `src/types.{c,h}`
- [ ] Add `Contract` struct: `{preconditions: Expr*, postconditions: Expr*, invariants: Expr*}`
- [ ] Add `contract` field to `FuncType`
- [ ] Add `invariant` field to `StructType`

**Elaborator Changes** — `src/elab.{c,h}`
- [ ] Parse `:pre` and `:post` metadata on `defn`
- [ ] Store contracts in function type information
- [ ] Type-check contract expressions (they have access to function parameters and `result` binding)
- [ ] For postconditions: introduce implicit `result` binding of return type
- [ ] For invariants: attach to struct type, checked on construction and mutation

**Codegen Changes** — `src/emit.{c,h}`
- [ ] Precondition checking: emit checks at function entry, before body
- [ ] Postcondition checking: wrap function body in try/finally-like structure that checks postconditions on all exit paths
- [ ] Use `tur_panic` for contract failures (integrates with exception unwinding)
- [ ] Release mode: omit all contract check code

**Postcondition Implementation Strategy**

To check postconditions at *all* exit points:

```c
// Lowering of (defn f [x] {:post [(> result 0)]} (+ x 1))
int f(int x) {
    int result;
    // Check preconditions here
    
    // Body with postcondition wrapper
    if (setjmp(postcondition_jmpbuf) == 0) {
        result = x + 1;
        // Normal exit: check postcondition
        if (!(result > 0)) {
            tur_panic("Postcondition failed: result > 0");
        }
        return result;
    } else {
        // Exception exit: check postcondition still holds?
        // Actually, postconditions shouldn't be checked on exception exit
        // Only check on normal return paths
        tur_rethrow();
    }
}
```

Actually, this is complex. Better approach: transform the function to always check postconditions before returning:

```c
// Lowering with postcondition check
int f(int x) {
    // Precondition checks
    
    int result = x + 1;
    
    // Postcondition check
    if (!(result > 0)) {
        tur_panic("Postcondition failed: result > 0");
    }
    
    return result;
}
```

But this doesn't handle early returns. Solution: wrap the entire body and require that all returns go through a single point, or use a goto-based approach:

```c
int f(int x) {
    int result;
    
    // Precondition
    
    goto body_start;
  
  check_postcondition:
    if (!(result > 0)) {
        tur_panic("Postcondition failed");
    }
    return result;
    
  body_start:
    result = x + 1;
    goto check_postcondition;
}
```

**Invariant Checking**
- [ ] On struct construction: check invariants after all fields are initialized
- [ ] On struct mutation: re-check invariants (requires tracking which operations mutate)
- [ ] Simpler approach: only check on explicit `invariant!` calls in v2

**Contract Failure Behavior**
- [ ] Default: call `tur_panic` with message
- [ ] Configurable: return `result` type instead (requires wrapping return type)
- [ ] Message includes: file, line, function name, failed condition

**Compiler Flags**
- [ ] `--contracts` / `--no-contracts`: Enable/disable contract checking (default: enabled in debug, disabled in release)
- [ ] `--contracts=panic` / `--contracts=result`: Failure behavior

**Fixtures** — `tests/fixtures/contract-v2/`
- [ ] `contract-pre-checked.tur` — preconditions checked at entry
- [ ] `contract-post-all-exits.tur` — postconditions checked on all normal exit paths
- [ ] `contract-post-early-return.tur` — postconditions work with early returns
- [ ] `contract-post-exception.tur` — postconditions NOT checked on exception exit (only normal exits)
- [ ] `contract-invariant-struct.tur` — struct invariants checked on construction
- [ ] `contract-inheritance.tur` — contracts on inherited/implemented methods
- [ ] `contract-metadata.tur` — contracts specified via metadata map
- [ ] `contract-release-no-op.tur` — contracts compile to nothing in release mode
- [ ] Codegen snapshots: full postcondition checking at all exit points

**Exit Criterion:** Contracts work at all function exit points; invariants work on structs; release mode strips all checks; all fixtures pass.

---

### Phase C3 — Advanced Contracts (v3)

**Goal:** Add powerful contract features for library authors and advanced use cases.

**Dependent Contracts**
```lisp
(defn vec-swap [vec : (vec int) i :int j :int]
  {:pre [(vec/in-bounds? vec i) (vec/in-bounds? vec j)]
   :post [(== (vec/get vec i) old-j-value)
          (== (vec/get vec j) old-i-value)]}
  ;; old-i-value and old-j-value are snapshots of the pre-state
  )
```

**Contract Groups / Named Contracts**
```lisp
(defcontract PositiveInt [x :int] (> x 0))
(defcontract NonEmptyVec [v : (vec a)] (> (vec/len v) 0))

defn process [^PositiveInt x :int ^NonEmptyVec vec : (vec int)]
  ;; ...
  )
```

**Type-Level Contracts (Refinement Types)**
```lisp
;; Future: integrate with type system
(defn sqrt [x : (int :| (> x 0))] : (float :| (>= result 0))
  (math/sqrt x))
```

**Contract Inheritance**
```lisp
(defclass Shape
  {:invariant [(> (.area self) 0)]})

defclass Circle [Shape]
  {:invariant [(> (.radius self) 0)]});
```

**Contract-based Mocking**
```lisp
;; For testing: generate mocks that satisfy contracts
(defmock VecMock :Vec
  {:contracts Vec-contracts})
```

**Performance Optimizations**
- [ ] Static contract evaluation: if contract is a constant expression, check at compile time
- [ ] Contract inlining: inline simple contract checks
- [ ] Contract caching: cache expensive invariant checks

**Fixtures**
- [ ] `contract-dependent.tur` — old/new value comparison in postconditions
- [ ] `contract-named.tur` — named contract groups
- [ ] `contract-inheritance.tur` — contract inheritance in type hierarchies
- [ ] `contract-static.tur` — compile-time contract evaluation

**Exit Criterion:** Advanced contract features work; contracts integrate with type system; performance optimizations in place.

---

## Integration with Other Features

### Exceptions
- Contract failures use `tur_panic` which integrates with exception unwinding
- Postconditions are NOT checked when an exception is thrown (only on normal exits)
- Double-panic guard applies: panic during contract check → `abort()`

### Effects
- Contracts are effect-free by default (pure predicates)
- Effects in contracts: allowed but discouraged; effects are checked in the contract context
- Future: effect rows could track contract-checking effects

### Borrow Checker
- Contract expressions must respect borrow constraints
- Contracts cannot mutate borrowed values
- Contracts on `&mut` references: can mutate, but must not violate invariants

### Typeclasses
- Contracts can use typeclass methods
- Typeclass instances available in contract scope
- Future: typeclass-based contracts (e.g., `^Ord a` implies certain contracts)

### Macros
- Contracts work inside macros
- Macros can generate code with contracts
- `defmacro` can have its own contracts

### FFI
- Contracts are **especially valuable** at FFI boundaries
- `extern-c` functions can have contracts on Turmeric side
- Contracts validate before/after FFI calls

---

## Performance Considerations

### Overhead Analysis

| Check Type | Debug Overhead | Release Overhead |
|---|---|---|
| Simple predicate (`> x 0`) | ~1-2 instructions | 0 (stripped) |
| Complex predicate (`vec/in-bounds?`) | ~5-10 instructions | 0 (stripped) |
| Struct invariant | Varies | 0 (stripped) |
| Postcondition (all exits) | Branching overhead | 0 (stripped) |

### Optimization Strategies

1. **Compile-time stripping**: Remove all contract code in release builds
2. **Dead code elimination**: If contract is always true (proven statically), remove it even in debug
3. **Inlining**: Inline simple contract checks
4. **Caching**: Cache invariant check results when possible
5. **Lazy checking**: Defer expensive checks (but this weakens guarantees)

---

## Configuration

### Compiler Flags

```
--contracts           Enable contract checking (default: on in debug, off in release)
--no-contracts       Disable contract checking
--contracts=panic    Fail with panic (default)
--contracts=result   Fail by returning result type
--contracts=abort    Fail with abort() (no unwinding)
```

### Pragma-Based Control

```lisp
;; File-level
#!contracts off

;; Function-level
(defn f [x] {:no-contracts true} ...)

;; Block-level
(no-contracts
  (expensive-operation))
```

---

## Error Messages

Contract failures produce clear, actionable error messages:

```
panic at src/math.tur:42 in sqrt:
  Precondition failed: (>= x 0.0)
  x = -5
  
Call stack:
  at math.tur:42: sqrt
  at main.tur:10: main
```

Message includes:
- File and line number
- Function name
- Failed condition (as source text)
- Values of variables in condition (when feasible)
- Call stack (when available)

---

## Comparison with Alternatives

| Approach | Pros | Cons |
|---|---|---|
| **Runtime Contracts (this proposal)** | Catches logic errors, zero-cost in release, works at FFI boundaries | Runtime overhead in debug, doesn't catch all issues |
| **Static Assertions** | Zero runtime cost, caught at compile time | Limited expressiveness, can't check runtime values |
| **Property-Based Testing** | Finds edge cases, works with fuzzing | Doesn't prevent bugs in production, separate from main code |
| **Dependent Types** | Strongest guarantees, caught at compile time | Complex, may not be decidable, significant compiler changes |
| **Refinement Types** | More expressive than simple types, compile-time checks | Complex type system changes, may need annotations |

**Recommendation:** Runtime contracts complement static types and property testing. They're the pragmatic choice for catching logic errors that static analysis can't.

---

## Related Work

- **Eiffel**: Pioneered Design by Contract with built-in `require`/`ensure`/`invariant`
- **Racket**: Rich contract system with first-class contracts, contract combinators
- **Rust**: `debug_assert!`, `assert!`, `panic!` macros; no built-in postconditions
- **Clojure**: `:pre`/`:post` metadata on functions
- **Dafny**: Full specification language with pre/post/invariants, verified statically
- **Liquid Haskell**: Refinement types with static checking via SMT solvers

---

## Open Questions

1. Should contracts be part of the type system (affecting subtyping) or purely runtime?
2. Should there be a way to "prove" contracts statically and disable runtime checks?
3. How should contracts interact with delimited continuations?
4. Should contracts be checked in `defer` blocks?
5. How to handle contracts in generic/higher-order functions?

---

## Appendix: Example Contract Library

```lisp
;; stdlib/contract.tur

(defn contract-enabled? [] :bool
  (not (compiling-in? :release)))

(defmacro assert! [condition & [msg]]
  (if (contract-enabled?)
    (if msg
      `(unless ~condition (panic ~msg))
      `(unless ~condition (panic "Assertion failed")))
    `(do)))

(defmacro require! [condition & [msg]]
  `(assert! ~condition ~@(when msg [msg])))

(defmacro ensure! [condition & [msg]]
  (if (contract-enabled?)
    (if msg
      `(let [result# ~condition]
         (unless result# (panic ~msg))
         result#)
      `(let [result# ~condition]
         (unless result# (panic "Postcondition failed"))
         result#))
    `(do ~condition)))

(defmacro invariant! [obj predicate & [msg]]
  `(assert! (~predicate ~obj) ~@(when msg [msg])))

;; Usage
(defn sqrt [x :float] :float
  (require! (>= x 0.0) "sqrt requires non-negative input")
  (let [result (math/sqrt x)]
    (ensure! (>= result 0.0) "sqrt result must be non-negative")
    result))
```

---

## Appendix: Contract Check Implementation Sketch

```c
// In src/runtime.h

// Contract failure handler
void tur_contract_fail(const char *file, int line, const char *func,
                       const char *condition, const char *msg);

// In src/runtime.c
void tur_contract_fail(const char *file, int line, const char *func,
                       const char *condition, const char *msg) {
    char buf[1024];
    snprintf(buf, sizeof(buf), "Contract failed at %s:%d in %s: %s\n%s",
             file, line, func, condition, msg ? msg : "");
    tur_panic(buf);
}

// Macro for codegen
#define TUR_CHECK_CONTRACT(cond, file, line, func, msg) \
    do { \
        if (!(cond)) { \
            tur_contract_fail(file, line, func, #cond, msg); \
        } \
    } while (0)
```
