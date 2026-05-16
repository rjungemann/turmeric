# Advanced Type System Features — Feasibility Plan for Turmeric

> **Status:** Draft — Not Started  
> **Target:** v3 or later  
> **Prerequisites:** Phase 19 (Algebraic Effects) complete; HKT/HRT/GADT roadmap (v2) recommended  
> **Related:** [higher-ranked-types-plan.md](archive/higher-ranked-types-plan.md), [higher-kinded-types-plan.md](archive/higher-kinded-types-plan.md), [gadts-plan.md](archive/gadts-plan.md)

---

## Executive Summary

This document explores **type system features not yet considered** for Turmeric that could significantly expand its expressiveness for systems programming, formal verification, and advanced functional programming patterns. The current type system (v1) provides:

| Feature | Status | Notes |
|---|---|---|
| Hindley-Milner type inference (Rank-1) | ✅ | Implicit forall at top level |
| Typeclasses (kind-*) | ✅ | Dictionary passing, Phase 15 |
| Borrow checking (& / &mut) | ✅ | Lifetime-agnostic, Phase 12 |
| Reference counting (rc<T>) | ✅ | Shared ownership, Phase 9 |
| Unique ownership (ref<T>) | ✅ | Move semantics, Phase 5 |
| Algebraic effects | ✅ | OCaml 5-style, Phase 19 |

**Already planned for v2:**
- Higher-Kinded Types (HKT) — [hkt-implementation-plan.md](archive/higher-kinded-types-plan.md)
- Higher-Ranked Types (HRT) — [higher-ranked-types-plan.md](archive/higher-ranked-types-plan.md)
- Generalized Algebraic Data Types (GADTs) — [gadts-plan.md](archive/gadts-plan.md)

**This document covers features NOT yet on the roadmap:**

| Feature | Maturity | Complexity | Use Case |
|---|---|---|---|
| [Linear Types](#1-linear-types) | High | Medium-High | Memory safety, resource management |
| [Dependent Types](#2-dependent-types) | Research | Very High | Formal verification, indexed types |
| [Uniqueness Types](#3-uniqueness-types) | Medium | Medium | Alias control, in-place update |
| [Substructural Type Systems](#4-substructural-type-systems) | Medium | Medium | Session types, linear logic |
| [Session Types](#5-session-types) | Medium | High | Protocol verification, concurrency |
| [Refinement Types](#6-refinement-types) | Medium | High | Runtime property verification |
| [Intersection & Union Types](#7-intersection--union-types) | High | Medium | Gradual typing, flexible APIs |
| [Effect Types (Row Polymorphism)](#8-effect-types-row-polymorphism) | High | Medium | Effect tracking, handler typing |
| [Sized Types](#9-sized-types) | Medium | Medium-High | Memory layout, embedded DSLs |
| [Contract Types](#10-contract-types) | Medium | Medium | Runtime assertions, gradual typing |

---

## Decision Framework

Each feature is evaluated against these criteria:

1. **Aligns with Turmeric's goals?** (Lisp expressiveness + systems control + zero-cost abstractions)
2. **Fits the C99 target?** (No GC, manual memory management, predictable performance)
3. **Composes with existing features?** (Borrow checker, RC, typeclasses, effects)
4. **Compilation model complexity?** (Elaboration, codegen, C emission)
5. **User demand?** (Known use cases from Turmeric community)
6. **Prior art?** (Other languages with similar features targeting C)

**Acceptance threshold:** ≥3 of the first 4 criteria must be satisfied, AND complexity must be justifiable by demand.

---

---

## 1. Linear Types

### What

Linear types enforce the **exactly-once** usage discipline: a value of linear type must be used exactly once. This generalizes Rust's ownership model where `ref<T>` is linear (move-only). Linear types prevent:
- Dropping a value without using it (leaks)
- Using a value more than once (double-free, use-after-free)
- Copying a value without explicit duplication

### Syntax (Proposed)

```clojure
;; Linear type annotation
(deftype Linear [a] ^linear a)

;; Or built-in syntax
(defn consume [^linear x : T] : unit ...)

;; Linear function type
(deftype LinearFn [a b] (-> a ^linear b))
```

### Motivating Examples

**Example 1: Resource-safe file handles**

```clojure
;; A file handle that MUST be closed exactly once
(defn open-file [path : cstr] : ^linear (FileHandle))

defn read-file [^linear fh : FileHandle] : (result string IoError)
  ...

defn close-file [^linear fh : FileHandle] : unit
  ...

;; Valid: handle is consumed exactly once
defn read-then-close [path : cstr] : (result string IoError)
  (let [fh (open-file path)]
    (let [result (read-file fh)]  ; fh consumed here
      (close-file fh)               ; ERROR: fh already consumed!
      result))

;; Correct version
defn read-then-close [path : cstr] : (result string IoError)
  (let [fh (open-file path)]
    (let [result (read-file fh)]  ; fh consumed by read-file
      result))                    ; close-file not needed, read-file owns fh
```

**Example 2: Linear closures for one-shot continuations**

```clojure
;; A continuation that can only be invoked once
deftype LinearCont [a] ^linear (-> a unit)

defn with-linear-cont [f : (-> (LinearCont a) b)] : b
  ...
```

### Interaction with Existing Features

| Feature | Interaction | Notes |
|---|---|---|
| `ref<T>` | `ref<T>` is already linear | Aligns perfectly |
| `rc<T>` | `rc<T>` is shared (non-linear) | Different discipline |
| `&T` / `&mut T` | Borrows are non-owning | Linear types don't affect borrows |
| `defer` | Defer runs after linear value consumed | Compatible |
| Typeclasses | Typeclass methods can have linear params | Method signatures must be linear |
| Effects | Linear values can be performed | Effect system unchanged |

### Implementation Strategy

#### Phase LT0: Linear Type Foundations

**Goal:** Add `^linear` annotation to types and track linearity in the type system.

- [ ] Add `TY_LINEAR` flag to `Type` struct (or use existing `CopyKind` with new `CK_LINEAR`)
- [ ] Add `linear` field to `TypeKind` enum or extend `CopyKind`:
  ```c
  typedef enum CopyKind {
      CK_MOVE,      /* Move-only (consumed on use) */
      CK_COPY,      /* Copy (bitwise duplication) */
      CK_UNSIZED,   /* Unsized */
      CK_LINEAR,    /* Linear: must be used exactly once */
  } CopyKind;
  ```
- [ ] Linear types cannot be:
  - Copied (no implicit duplication)
  - Dropped without being consumed
  - Used more than once

#### Phase LT1: Linear Type Checking

**Goal:** Implement linearity tracking in the borrow checker.

- [ ] Extend elaborator to track linear variable consumption
- [ ] Each linear variable has a "consumed" flag in the symbol table
- [ ] Error on:
  - Use of consumed linear variable
  - Scope exit with unconsumed linear variable (leak)
  - Copying a linear variable
- [ ] Linear variables can be:
  - Moved (transfers linearity)
  - Consumed by function calls
  - Pattern-matched (each pattern arm gets the linear value)

#### Phase LT2: Linear Function Types

**Goal:** Support functions that consume their arguments.

- [ ] `(-> ^linear a b)` means `a` is consumed by the function
- [ ] Linear arrow is contravariant in input, covariant in output
- [ ] Function composition with linear types must respect linearity

#### Phase LT3: Linear Type Inference

**Goal:** Infer linearity where possible.

- [ ] `ref<T>` is inferred as linear
- [ ] Functions that take `ref<T>` and consume it are linear in that parameter
- [ ] Linear type inference is local (no global analysis needed)

#### Phase LT4: Integration & Polish

- [ ] Update stdlib: `FileHandle`, `Socket`, `MutexGuard` as linear types
- [ ] Linear type error messages
- [ ] `tur explain` support for linearity errors
- [ ] Performance benchmarks (linearity tracking overhead)

### Complexity Assessment

| Aspect | Complexity | Notes |
|---|---|---|
| Type system changes | Medium | New type flag, consumption tracking |
| Elaborator changes | Medium | Consumption state machine per symbol |
| Codegen changes | Low | No runtime representation change |
| C emission | Low | Identical to move semantics |
| Error messages | Medium | New error class for linearity violations |

### Prior Art

- **Rust:** Ownership model is linear (move semantics)
- **Linear Haskell:** `GHC -XLinearTypes` extension
- **Clean:** Pure functional with uniqueness typing
- **Idris:** Linear types for resource management
- **ATS:** Linear types for memory management

### Recommendation

**✅ ACCEPT — High value, medium complexity, aligns perfectly with existing ownership model.**

Linear types are a natural extension of Turmeric's `ref<T>` semantics. They provide:
1. Compile-time guarantees against resource leaks
2. Better alignment with Rust's ownership model (useful for FFI)
3. Foundation for session types and other substructural features

**Implementation priority:** High (after HKT/HRT, before GADTs)

---

## 2. Dependent Types

### What

Dependent types allow types to depend on **runtime values**. A type `T x` can refine its behavior based on the value `x`. This enables:
- Length-indexed vectors (`Vec n a` where `n` is a value)
- Type-safe matrix operations (matrix dimensions in types)
- Proof-carrying code (types encode pre/post-conditions)

### Syntax (Proposed)

```clojure
;; A vector with a known length at compile time
(deftype Vec [n : nat, a] ...)

;; A function that only accepts non-empty vectors
(defn head [v : (Vec (Succ n) a)] : a ...)

;; A proof that two values are equal
(deftype Equal [a x y] ...)
  (Refl : (Equal a x x))

;; Dependent pattern matching
(defn vec-length [v : (Vec n a)] : n
  (match v
    (VNil) 0
    (VCons _ xs) (Succ (vec-length xs))))
```

### Motivating Examples

**Example 1: Length-indexed vectors (no bounds checks)**

```clojure
(deftype Nat []
  (Zero)
  (Succ Nat))

deftype Vec [n : Nat, a]
  (VNil : (Vec Zero a))
  (VCons : (-> a (Vec n a) (Vec (Succ n) a)))

defn safe-head [v : (Vec (Succ n) a)] : a
  (match v
    (VCons x _) x)

;; Compile-time error: cannot call safe-head on VNil
(safe-head VNil)  ; ERROR: expected Vec (Succ n) a, got Vec Zero a
```

**Example 2: Type-safe printf with format strings**

```clojure
(deftype Fmt [args : (List Type)]
  (FDone : (Fmt []))
  (FInt : (-> (Fmt rest) (Fmt (cons int rest))))
  (FStr : (-> (Fmt rest) (Fmt (cons cstr rest)))))

defn printf [fmt : (Fmt args), a1 : args.0, a2 : args.1, ...] : unit
  ...

;; Type-checker verifies argument count and types
(printf (FInt (FStr FDone)) 42 "hello")  ; OK
(printf (FInt FDone) 42)                  ; OK
(printf (FInt FDone) "hello")            ; ERROR: expected int, got cstr
```

**Example 3: Proof-carrying array indexing**

```clojure
(defn get [v : (Vec n a), i : nat, ^(lt i n) : (LessThan i n)] : a ...)

;; Only valid if we can prove i < length
(defn example [v : (Vec 5 int)] : int
  (get v 3 proof))  ; OK if proof : (LessThan 3 5)
```

### Interaction with Existing Features

| Feature | Interaction | Notes |
|---|---|---|
| Typeclasses | Typeclass instances can have dependent types | Complex |
| Borrow checker | Dependent types don't affect borrowing | Orthogonal |
| Effects | Effects can have dependent types | Needs careful design |
| GADTs | GADTs are a subset of dependent types | GADT plan is prerequisite |
| Codegen | Dependent types require runtime proofs | Zero-cost if proofs are erased |

### Implementation Strategy

#### Phase DT0: Type-Level Naturals and Vectors

**Goal:** Basic dependent types with type-level natural numbers and vectors.

- [ ] Add type-level natural number literals: `0`, `1`, `2`, ...
- [ ] Add `Succ` type constructor for natural numbers
- [ ] Add `Vec n a` dependent type
- [ ] Support pattern matching on dependent types
- [ ] Implement proof erasure: dependent proofs are erased at runtime

#### Phase DT1: Dependent Pattern Matching

**Goal:** Pattern matching that refines dependent types.

- [ ] Extend `match` to support dependent patterns
- [ ] Implement refinement in pattern arms
- [ ] Type-check dependent pattern exhaustiveness

#### Phase DT2: Proof Terms

**Goal:** First-class proof terms for dependent type propositions.

- [ ] Add `Equal` type for equality proofs
- [ ] Add `LessThan` type for inequality proofs
- [ ] Support proof term construction and elimination
- [ ] Implement proof erasure at codegen

#### Phase DT3: Dependent Function Types (Pi Types)

**Goal:** Functions whose return type depends on their input values.

- [ ] Syntax: `(-> (x : a) (Pi x b))` or `(-> x : a, b)`
- [ ] Elaboration: check that return type is well-formed for all inputs
- [ ] Codegen: lambda-lift dependent functions

#### Phase DT4: Dependent Typeclasses

**Goal:** Typeclasses that depend on values.

- [ ] Typeclass parameters can be value-level
- [ ] Instance resolution considers value equality
- [ ] Complex interaction with dictionary passing

#### Phase DT5: Integration & Polish

- [ ] Stdlib: `Vec`, `Matrix`, `Fin n`, proof combinators
- [ ] Error messages for dependent type errors
- [ ] Performance: proof erasure overhead
- [ ] Documentation

### Complexity Assessment

| Aspect | Complexity | Notes |
|---|---|---|
| Type system changes | Very High | Pi types, dependent kinds |
| Elaborator changes | Very High | Dependent unification, proof checking |
| Codegen changes | High | Proof erasure, dependent function emission |
| C emission | Medium | Lambda lifting for dependent functions |
| Error messages | High | Dependent type mismatches are complex |

### Prior Art

- **Idris:** Full dependent types, proof erasure
- **Agda:** Dependent types with coinduction
- **Coq:** Proof assistant with dependent types
- **F*:** Dependent types for verification (compiles to C)
- **Lean 4:** Dependent types with good engineering
- **Haskell (Singletons):** Type-level computation for dependent-like types
- **Rust (const generics):** Limited dependent types via const values

### Recommendation

**⚠️ DEFER — Very high complexity, unclear user demand, limited alignment with C99 target.**

While dependent types are powerful, they:
1. Require significant compiler complexity (dependent unification is hard)
2. Have limited prior art for C-targeting languages
3. May not provide enough benefit for systems programming use cases
4. Conflict with Turmeric's "pragmatic" approach

**Conditions for reconsideration:**
- Strong demand from verification-focused users
- Successful implementation in another C-targeting language
- Clear use cases that cannot be addressed with GADTs + HRT + HKT

**Alternative:** Implement a subset (type-level naturals, length-indexed vectors) as a library using GADTs. This provides 80% of the benefit with 20% of the complexity.

---

## 3. Uniqueness Types

### What

Uniqueness types enforce **at most one** reference to a value exists. Unlike linear types (exactly one), uniqueness allows:
- Zero references (value can be dropped)
- One reference (value is uniquely owned)

Uniqueness types are useful for:
- In-place mutation without aliasing concerns
- Array mutation with unique pointers
- Compile-time alias analysis

### Syntax (Proposed)

```clojure
;; Unique type annotation
(deftype Unique [a] ^unique a)

;; Or built-in syntax
defn mutate-in-place [^unique x : (vec int)] : unit ...)
```

### Motivating Examples

**Example 1: In-place array sorting**

```clojure
;; A unique vector can be mutated in place
(defn sort! [^unique v : (vec int)] : unit
  (in-place-quicksort v))

;; After sorting, the vector is still unique
defn example [] : unit
  (let [v (vec/new 10)]
    (sort! v)
    (println v))
```

**Example 2: Unique mutable borrow**

```clojure
;; A unique mutable reference
(defn modify [^unique ^mut x : int] : unit
  (set! x (+ x 1)))

;; Unique ensures no aliases exist
(defn example [] : unit
  (let [x 42]
    (modify x)  ; OK: x is unique
    (let [y x]
      (modify x))))  ; ERROR: x is aliased by y
```

### Interaction with Existing Features

| Feature | Interaction | Notes |
|---|---|---|
| `ref<T>` | `ref<T>` is unique by default | Aligns perfectly |
| `&mut T` | `&mut T` implies uniqueness | Already enforced by borrow checker |
| `rc<T>` | `rc<T>` is non-unique (shared) | Different discipline |
| `defer` | Defer works with unique values | Compatible |

### Implementation Strategy

#### Phase UT0: Uniqueness Type Foundations

- [ ] Add `^unique` annotation to types
- [ ] Add `CK_UNIQUE` to `CopyKind` enum
- [ ] Unique values cannot be copied (but can be moved)
- [ ] Track aliasing in the elaborator

#### Phase UT1: Uniqueness Checking

- [ ] Error on copying a unique value
- [ ] Error on creating an alias to a unique value
- [ ] Allow dropping unique values without use

#### Phase UT2: Unique Mutable References

- [ ] `^unique ^mut T` for unique mutable references
- [ ] Integration with existing `&mut T` borrow checker

#### Phase UT3: Integration

- [ ] Update stdlib: mutable arrays, buffers as unique types
- [ ] Uniqueness type error messages
- [ ] Performance benchmarks

### Complexity Assessment

| Aspect | Complexity | Notes |
|---|---|---|
| Type system changes | Low | New type flag |
| Elaborator changes | Medium | Alias tracking |
| Codegen changes | Low | No runtime representation |
| C emission | Low | Identical to move semantics |
| Error messages | Low | Similar to borrow errors |

### Prior Art

- **Clean:** Uniqueness typing system
- **Rust:** `Box<T>` provides unique ownership
- **Haskell:** `Data.Unique` library
- **Bluespec:** Uniqueness for hardware design

### Recommendation

**✅ ACCEPT — Low complexity, good alignment with existing model, useful for in-place mutation.**

Uniqueness types are a small extension to the existing ownership model. They provide:
1. Explicit alias control for mutable operations
2. Foundation for more advanced type systems
3. Better error messages for aliasing bugs

**Implementation priority:** Medium (after Linear Types, before Dependent Types)

**Note:** Uniqueness types can be partially simulated with the existing `ref<T>` + borrow checker. However, explicit uniqueness annotations provide better documentation and enable more precise static analysis.

---

## 4. Substructural Type Systems

### What

Substructural type systems relax the standard rule that types can be freely duplicated and discarded. The three dimensions:

| Dimension | Standard | Substructural |
|---|---|---|
| **Weakening** | Can discard values | Cannot discard (relevance) |
| **Contraction** | Can duplicate values | Cannot duplicate (linearity) |
| **Exchange** | Order matters | Order doesn't matter (commutativity) |

Combining these gives:
- **Linear types:** No weakening, no contraction, exchange
- **Affine types:** No contraction, weakening allowed
- **Relevant types:** No weakening, contraction allowed

### Syntax (Proposed)

```clojure
;; Relevant type (must be used, but can be duplicated)
(defn must-use [^relevant x : T] : unit ...)

;; Affine type (can be discarded, but cannot be duplicated)
(defn one-shot [^affine x : T] : unit ...)
```

### Motivating Examples

**Example 1: Relevant types for resource tracking**

```clojure
;; A value that must be used (cannot be discarded)
(defn process [^relevant resource : Resource] : unit
  ...)

;; Error: resource not used
defn bad [] : unit
  (let [r (acquire-resource)]
    (do-nothing))  ; ERROR: relevant value 'r' not used
```

**Example 2: Affine types for one-time use**

```clojure
;; A value that can be discarded but not duplicated
(defn initialize [^affine key : EncryptionKey] : unit
  ...)

;; Error: key duplicated
defn bad [] : unit
  (let [k (generate-key)]
    (initialize k)
    (initialize k)))  ; ERROR: affine value 'k' used twice
```

### Interaction with Existing Features

| Feature | Interaction | Notes |
|---|---|---|
| Linear types | Linear = no weakening + no contraction | Subsumed by substructural framework |
| Borrow checker | Orthogonal | Borrows are non-owning |
| Effects | Substructural effects | Effect rows can be substructural |

### Implementation Strategy

#### Phase ST0: Substructural Type Flags

- [ ] Add `TY_RELEVANT`, `TY_AFFINE`, `TY_LINEAR` flags to `Type`
- [ ] These are mutually exclusive (a type can have at most one)
- [ ] Default: all types are standard (weakening + contraction allowed)

#### Phase ST1: Substructural Type Checking

- [ ] Track usage of relevant/affine/linear values
- [ ] Error on:
  - Not using a relevant value
  - Duplicating an affine or linear value
  - Using a linear value more than once

#### Phase ST2: Substructural Type Inference

- [ ] Infer substructural annotations where possible
- [ ] `ref<T>` is inferred as linear
- [ ] Functions that consume resources are inferred as relevant

#### Phase ST3: Integration

- [ ] Substructural type error messages
- [ ] Stdlib patterns for substructural types

### Complexity Assessment

| Aspect | Complexity | Notes |
|---|---|---|
| Type system changes | Low | Type flags |
| Elaborator changes | Medium | Usage tracking |
| Codegen changes | Low | No runtime representation |
| C emission | Low | Identical to standard types |
| Error messages | Medium | New error classes |

### Prior Art

- **Rust:** Linear types via ownership
- **Linear Haskell:** Full substructural type system
- **BiblioPolis:** Substructural type system for session types
- **Pfenning & Davies:** Substructural logic foundation

### Recommendation

**✅ ACCEPT — Provides a unifying framework for Linear, Uniqueness, and Affine types.**

Substructural type systems:
1. Generalize linear types naturally
2. Provide fine-grained control over value usage
3. Have clean semantic foundations
4. Low implementation complexity

**Implementation priority:** Medium (after Linear Types, as a generalization)

**Note:** Start with Linear Types (most useful), then generalize to the full substructural framework.

---

## 5. Session Types

### What

Session types describe **communication protocols** between concurrent processes. A session type specifies the sequence and types of messages exchanged. Session types enable:
- Compile-time verification of communication protocols
- Deadlock prevention
- Type-safe distributed systems
- Structured concurrency

### Syntax (Proposed)

```clojure
;; A session type for a simple protocol
(deftype GetInt []
  (! int))  ; Send an int

(deftype PutInt []
  (? int))  ; Receive an int

(deftype Echo []
  (-> (recv int) (send int) Echo))

;; A session channel
(deftype Session [P] ...)

;; Endpoint types
deftype Endpoint [P]
  (Send : (-> (Endpoint Q) P))
  (Recv : (-> (-> T (Endpoint Q)) P))
```

### Motivating Examples

**Example 1: Request-response protocol**

```clojure
;; Server protocol: receive request, send response, repeat
(deftype ServerProtocol []
  (recv Request
    (send Response
      ServerProtocol)))

;; Client protocol: send request, receive response, repeat
(deftype ClientProtocol []
  (send Request
    (recv Response
      ClientProtocol)))

;; Server implementation
defn server [^linear chan : (Session ServerProtocol)] : unit
  (loop []
    (let [req (recv chan)]
      (let [resp (handle-request req)]
        (send chan resp)
        (recur))))

;; Client implementation
defn client [^linear chan : (Session ClientProtocol)] : unit
  (let [req (make-request)]
    (send chan req)
    (let [resp (recv chan)]
      (process-response resp)))
```

**Example 2: Type-safe RPC**

```clojure
;; A typed RPC endpoint
(deftype Calculator []
  (recv [Add int int]
    (send int
      Calculator))
  (recv [Sub int int]
    (send int
      Calculator))
  (recv Quit
    (close)))

defn calculator-server [chan : (Session Calculator)] : unit
  (loop []
    (match (recv chan)
      (Add x y) (send chan (+ x y)) (recur)
      (Sub x y) (send chan (- x y)) (recur)
      (Quit)    (close chan)))
```

### Interaction with Existing Features

| Feature | Interaction | Notes |
|---|---|---|
| Linear types | Session channels are linear | Must be used exactly once |
| Algebraic effects | Sessions can use effects | For error handling, etc. |
| STM | Sessions with STM | Atomic message passing |
| Threads | Sessions between threads | Type-safe message passing |

### Implementation Strategy

#### Phase SS0: Session Type Foundations

- [ ] Add session type constructors: `!T` (send), `?T` (receive), `&` (choice), `|` (branch)
- [ ] Add `deftype Session [P]` for session channels
- [ ] Session types are linear (channels cannot be duplicated)

#### Phase SS1: Session Type Checking

- [ ] Duality checking: client and server protocols are duals
- [ ] Progress checking: sessions must eventually terminate or recurse
- [ ] Type-check message passing operations

#### Phase SS2: Session Codegen

- [ ] Emit message passing primitives
- [ ] Integrate with existing thread/channel infrastructure
- [ ] Protocol verification at runtime (optional, for debugging)

#### Phase SS3: Session Combinators

- [ ] Choice (`&`), branch (`|`), recursion (`μ`)
- [ ] Delegation (passing sessions to other processes)
- [ ] Session subtyping

#### Phase SS4: Integration

- [ ] Stdlib: common protocols (echo, RPC, publish-subscribe)
- [ ] Integration with STM for atomic sessions
- [ ] Integration with effects for error handling

### Complexity Assessment

| Aspect | Complexity | Notes |
|---|---|---|
| Type system changes | High | New type constructors, duality |
| Elaborator changes | High | Protocol checking, progress checking |
| Codegen changes | Medium | Message passing primitives |
| C emission | Medium | Channel operations |
| Error messages | High | Protocol mismatch errors |

### Prior Art

- **Haskell (Session Types library):** Type-safe communication
- **Rust (Sessions):** Research implementations
- **Scribble:** Protocol specification language
- **Linear Logic:** Theoretical foundation for session types
- **Covington et al.:** Session Types for C

### Recommendation

**✅ ACCEPT — High value for concurrent/distributed programming, aligns with linear types.**

Session types provide:
1. Compile-time verification of communication protocols
2. Deadlock prevention
3. Type-safe concurrency patterns
4. Natural fit with Turmeric's effect system and STM

**Implementation priority:** Medium-High (after Linear Types, Substructural Types)

**Prerequisites:** Linear Types (for channel linearity), Thread primitives (Phase T19)

**Note:** Start with simple send/receive protocols, then add choice, recursion, and delegation.

---

## 6. Refinement Types

### What

Refinement types extend types with **runtime predicates**. A refinement type `T { p x }` represents values of type `T` that satisfy predicate `p` on value `x`. Refinement types enable:
- Type-safe array indexing with bounds
- Runtime property verification
- Gradual typing with runtime checks

### Syntax (Proposed)

```clojure
;; A refinement type: int that is non-negative
(deftype Nat { x : int | (>= x 0) })

;; A function that takes a non-negative int
(defn sqrt [n : { x : int | (>= x 0) }] : double ...)

;; Refinement in struct fields
(defstruct BoundedBuffer [
  data : (vec int)
  index : { i : int | (and (>= i 0) (< i (vec/len data))) }])
```

### Motivating Examples

**Example 1: Bounds-checked array access**

```clojure
;; A vector with a refinement on the index
(defn get [v : (vec a), i : { x : int | (and (>= x 0) (< x (vec/len v))) }] : a
  (vec/get v i))  ; No runtime bounds check needed!

;; The type-checker verifies the index is in bounds
defn example [v : (vec int)] : int
  (let [i 3]
    (if (and (>= i 0) (< i (vec/len v)))
      (get v i)  ; OK: predicate satisfied
      0))
```

**Example 2: Type-safe units**

```clojure
;; Units of measure as refinement types
(deftype Meters { x : double | (unit x :meters) })
(deftype Seconds { x : double | (unit x :seconds) })
(deftype MetersPerSecond { x : double | (unit x :meters/seconds) })

defn velocity [distance : Meters, time : Seconds] : MetersPerSecond
  (/ distance time)  ; Type-checker verifies unit consistency
```

### Interaction with Existing Features

| Feature | Interaction | Notes |
|---|---|---|
| Borrow checker | Orthogonal | Refinement doesn't affect borrowing |
| Typeclasses | Typeclass instances can have refinements | Complex |
| Effects | Refinements can depend on effects | Needs careful design |
| Codegen | Refinements may require runtime checks | Unless proven at compile time |

### Implementation Strategy

#### Phase RT0: Basic Refinement Types

- [ ] Add refinement type syntax: `{ x : T | p x }`
- [ ] Parse refinement predicates
- [ ] Store refinements in type system

#### Phase RT1: Refinement Type Checking

- [ ] Check that refinement predicates are well-typed
- [ ] Subtyping: `T { p x }` is a subtype of `T`
- [ ] Refinement intersection and union

#### Phase RT2: Refinement Propagation

- [ ] Propagate refinements through operations
- [ ] Example: if `x : { i : int | (>= i 0) }` and `y : { i : int | (>= i 0) }`, then `(+ x y) : { i : int | (>= i 0) }`
- [ ] Use SMT solvers for refinement reasoning (optional, for better inference)

#### Phase RT3: Runtime Refinement Checks

- [ ] Insert runtime checks for refinements that cannot be proven at compile time
- [ ] Runtime check failure: panic or return `result`
- [ ] Optimize away proven refinements

#### Phase RT4: Integration

- [ ] Stdlib: bounds checking, units of measure
- [ ] Integration with typeclasses
- [ ] Refinement type error messages

### Complexity Assessment

| Aspect | Complexity | Notes |
|---|---|---|
| Type system changes | High | Refinement type representation |
| Elaborator changes | Very High | Refinement propagation, SMT integration |
| Codegen changes | High | Runtime check insertion |
| C emission | Medium | Runtime check code |
| Error messages | High | Refinement mismatch explanations |

### Prior Art

- **Liquid Haskell:** Refinement types for Haskell
- **F7:** Refinement types for F#
- **Dafny:** Refinement types with SMT solving
- **Refined:** Refinement types for ML
- **Z3:** SMT solver for refinement reasoning

### Recommendation

**⚠️ DEFER — High complexity, unclear benefit for systems programming.**

Refinement types are powerful but:
1. Require significant compiler complexity (SMT integration)
2. Have limited prior art for C-targeting languages
3. Many use cases can be addressed with dependent types or GADTs
4. Runtime checks add overhead that may not be acceptable for systems code

**Conditions for reconsideration:**
- Strong demand from security/verification-focused users
- Successful integration with a C-targeting language
- Clear use cases that cannot be addressed with other features

**Alternative:** Implement a library-based approach using GADTs and proof-carrying types. This provides some refinement capabilities without compiler changes.

---

## 7. Intersection & Union Types

### What

Intersection types (`A & B`) represent values that satisfy both `A` and `B`. Union types (`A | B`) represent values that satisfy either `A` or `B`. These enable:
- Gradual typing (mixing typed and untyped code)
- Flexible API design
- Type-safe duck typing

### Syntax (Proposed)

```clojure
;; Intersection type
(deftype ReadableAndWritable []
  (Readable & Writable))

;; Union type
(deftype IntOrString []
  (int | cstr))

;; Function with union parameter
(defn print-value [x : (int | cstr | bool)] : unit
  (match x
    (i : int)   (println i)
    (s : cstr)  (println s)
    (b : bool)  (println (if b "true" "false"))))
```

### Motivating Examples

**Example 1: Gradual typing**

```clojure
;; A function that can accept any type (dynamic typing)
(defn debug-print [x : any] : unit
  (println x))

;; Gradually add types
(defn typed-print [x : (int | cstr)] : unit
  (debug-print x))
```

**Example 2: Type-safe duck typing**

```clojure
;; A typeclass for serializable things
(defclass Serializable [a]
  (serialize [x : a] : cstr))

;; A function that accepts anything serializable
(defn save [^Serializable x : a] : unit
  (let [data (serialize x)]
    (file/write data "output.txt")))

;; Intersection with a concrete type
(defn save-int-or-str [x : (int & Serializable)] : unit
  (save x))
```

**Example 3: Algebraic data types as unions**

```clojure
;; Option as a union type
(deftype Option [a]
  (none | a))

;; Result as a union type
(deftype Result [e a]
  (err e | ok a))
```

### Interaction with Existing Features

| Feature | Interaction | Notes |
|---|---|---|
| Typeclasses | Typeclass constraints with unions | Complex |
| Borrow checker | Union/intersection doesn't affect borrowing | Orthogonal |
| GADTs | GADTs can be encoded as unions | Alternative to `defdata` |
| Codegen | Unions may require tagged unions | Similar to ADT codegen |

### Implementation Strategy

#### Phase IT0: Union Type Foundations

- [ ] Add union type syntax: `(A | B | C)`
- [ ] Add `TY_UNION` to `TypeKind` enum
- [ ] Parse union types

#### Phase IT1: Union Type Checking

- [ ] Subtyping: `A` is a subtype of `(A | B)`
- [ ] Function application: `(-> (A | B) C)` accepts `A` or `B`
- [ ] Pattern matching on unions

#### Phase IT2: Intersection Type Foundations

- [ ] Add intersection type syntax: `(A & B & C)`
- [ ] Add `TY_INTERSECTION` to `TypeKind` enum
- [ ] Parse intersection types

#### Phase IT3: Intersection Type Checking

- [ ] Subtyping: `(A & B)` is a subtype of `A` and `B`
- [ ] Function application: `(-> (A & B) C)` requires both `A` and `B`
- [ ] Intersection elimination: from `(A & B)` you can get `A` or `B`

#### Phase IT4: Integration

- [ ] Stdlib: `any` type, gradual typing utilities
- [ ] Integration with typeclasses
- [ ] Union/intersection type error messages

### Complexity Assessment

| Aspect | Complexity | Notes |
|---|---|---|
| Type system changes | Medium | New type kinds |
| Elaborator changes | Medium | Subtyping rules, unification |
| Codegen changes | Medium | Tagged union emission |
| C emission | Medium | Union type representation |
| Error messages | Medium | Union/intersection mismatch |

### Prior Art

- **TypeScript:** Union and intersection types
- **Flow:** Union and intersection types
- **Haskell (GADTs):** Can encode union types
- **Scala:** Union and intersection types
- **Ceylon:** Union and intersection types
- **OCaml:** Polymorphic variants (similar to unions)

### Recommendation

**✅ ACCEPT — Medium complexity, high value for gradual typing and flexible APIs.**

Intersection & Union types provide:
1. Gradual typing capabilities
2. More flexible API design
3. Type-safe duck typing
4. Natural encoding of ADTs

**Implementation priority:** Medium (can be implemented independently)

**Note:** Start with union types (more useful), then add intersection types. Consider implementing ADTs as syntactic sugar for unions.

---

## 8. Effect Types (Row Polymorphism)

### What

Effect types make effect usage explicit in types. The **effect row** `E` in `a -> E b` tracks which effects a computation can perform. Row polymorphism allows abstracting over effect sets.

Turmeric already has algebraic effects (Phase 19), but without explicit effect typing. Effect types would add:
- Type-safe effect composition
- Effect polymorphism (functions generic over their effects)
- Effect subtyping
- Handler typing

### Syntax (Proposed)

```clojure
;; Effect row in function type
(defn read-file [path : cstr] : Io a)

;; Polymorphic effect
(defn generic [x : a] : (forall [e] IoE e a) ...)

;; Effect row variable
(defn with-file [path : cstr, f : (-> FileHandle Io a)] : Io a
  ...)
```

### Motivating Examples

**Example 1: Effect-polymorphic functions**

```clojure
;; A function that works with any effect
(defn map [f : (-> a Io b), xs : (vec a)] : Io (vec b)
  ...)

;; map can use Io effects internally
```

**Example 2: Effect subtyping**

```clojure
;; NoEffect is a subtype of Io
deftype NoEffect []
deftype Io []

;; A pure function
defn pure-func [x : a] : NoEffect b ...)

;; Can be used where Io is expected
defn example [] : Io unit
  (pure-func 42)
```

**Example 3: Handler typing**

```clojure
;; A handler for a specific effect
defn handle-io [e : Io, h : (-> IoError a)] : NoEffect a
  ...
```

### Interaction with Existing Features

| Feature | Interaction | Notes |
|---|---|---|
| Algebraic effects | Effect types extend Phase 19 | Natural fit |
| Typeclasses | Typeclass methods can have effect rows | Complex |
| HRT | Effect rows require rank-N types | HRT is prerequisite |
| Codegen | Effect rows guide CPS transformation | Existing CPS infrastructure |

### Implementation Strategy

#### Phase ET0: Effect Row Syntax

- [ ] Add effect row syntax to function types
- [ ] Add `TY_EFFECT_ROW` to type system
- [ ] Parse effect row annotations

#### Phase ET1: Effect Row Checking

- [ ] Track effect rows through function application
- [ ] Effect row subtyping
- [ ] Effect row unification

#### Phase ET2: Effect Polymorphism

- [ ] Add `forall [e]` for effect polymorphism
- [ ] Effect row variables
- [ ] Effect row constraint solving

#### Phase ET3: Handler Typing

- [ ] Type handlers with explicit effect row parameters
- [ ] Handler subtyping
- [ ] Handler composition

#### Phase ET4: Integration

- [ ] Update Phase 19 effects with effect typing
- [ ] Stdlib: effect hierarchies, common effect sets
- [ ] Effect type error messages

### Complexity Assessment

| Aspect | Complexity | Notes |
|---|---|---|
| Type system changes | High | Effect row representation |
| Elaborator changes | Very High | Effect row tracking, unification |
| Codegen changes | High | CPS transformation guided by effect rows |
| C emission | Medium | Effect row information used in codegen |
| Error messages | High | Effect mismatch explanations |

### Prior Art

- **OCaml 5:** Effect rows in algebraic effects
- **Eff:** Effect types with row polymorphism
- **Koka:** Effect types with row polymorphism
- **Haskell (Effect Handlers):** Research implementations
- **Unison:** Effect types with ability-based typing

### Recommendation

**✅ ACCEPT — Natural extension of Phase 19, provides type safety for effects.**

Effect types provide:
1. Type-safe effect composition
2. Effect polymorphism for reusable abstractions
3. Better error messages for effect usage
4. Foundation for effect-based optimizations

**Implementation priority:** High (after HRT, as it depends on rank-N types)

**Prerequisites:** Phase 19 (Algebraic Effects), HRT Phase HRT1 (Rank-2 types)

**Note:** Effect types should be designed in conjunction with the existing effect system to ensure smooth integration.

---

## 9. Sized Types

### What

Sized types track the **size** of data structures in the type system. This enables:
- Memory layout verification
- Stack allocation of fixed-size structures
- Type-safe embedded DSLs
- Array shape verification

### Syntax (Proposed)

```clojure
;; A sized vector (length known at compile time)
(deftype SizedVec [n : size, a] ...)

;; Size arithmetic
(deftype Size []
  (Static : (-> int Size))
  (Add : (-> Size Size Size))
  (Mul : (-> Size Size Size)))

;; A struct with known size
(defstruct Point [
  x : int
  y : int]
  :size 8)  ; 2 * 4 bytes
```

### Motivating Examples

**Example 1: Stack-allocated arrays**

```clojure
;; A stack-allocated array with known size
deftype StackArray [n : StaticInt, a]

;; Stack allocation when size is known
defn make-array [n : StaticInt 10, a : int] : (StackArray 10 int)
  (stack-alloc 10)
```

**Example 2: Type-safe matrix operations**

```clojure
;; A matrix with known dimensions
deftype Matrix [rows : StaticInt, cols : StaticInt, a]

defn mat-mul [a : (Matrix m n float), b : (Matrix n p float)] : (Matrix m p float)
  ...

;; Compile-time error: dimension mismatch
(mat-mul (Matrix 3 4) (Matrix 5 6))  ; ERROR: expected n = 4, got n = 5
```

**Example 3: Embedded DSL with sized types**

```clojure
;; A DSL for hardware description
deftype BitVec [n : StaticInt]

defn add [a : (BitVec n), b : (BitVec n)] : (BitVec (n + 1))
  ...
```

### Interaction with Existing Features

| Feature | Interaction | Notes |
|---|---|---|
| Memory management | Sized types affect allocation | Stack vs heap decision |
| FFI | Sized types for C structs | Natural fit |
| Codegen | Size information for layout | Optimization opportunities |

### Implementation Strategy

#### Phase SZ0: Size Type Foundations

- [ ] Add `StaticInt` type for compile-time integers
- [ ] Add size arithmetic operations
- [ ] Add sized type constructors

#### Phase SZ1: Sized Type Checking

- [ ] Size arithmetic type checking
- [ ] Sized type subtyping
- [ ] Size inference

#### Phase SZ2: Memory Layout

- [ ] Use size information for memory layout decisions
- [ ] Stack allocation for sized types when possible
- [ ] Heap allocation fallback

#### Phase SZ3: Integration

- [ ] Stdlib: sized vectors, matrices, bit vectors
- [ ] Integration with FFI for C structs
- [ ] Sized type error messages

### Complexity Assessment

| Aspect | Complexity | Notes |
|---|---|---|
| Type system changes | Medium | Size type representation |
| Elaborator changes | Medium | Size arithmetic |
| Codegen changes | High | Memory layout decisions |
| C emission | Medium | Size information in generated C |
| Error messages | Medium | Size mismatch explanations |

### Prior Art

- **Idris:** Sized types for dependent pattern matching
- **Agda:** Sized types with termination checking
- **ATS:** Sized types for memory management
- **F*:** Sized types for verification
- **C (const generics):** Limited sized types

### Recommendation

**✅ ACCEPT — Medium complexity, good fit for systems programming and embedded DSLs.**

Sized types provide:
1. Memory layout control
2. Stack allocation opportunities
3. Type-safe array operations
4. Embedded DSL support

**Implementation priority:** Medium (after Linear Types)

**Note:** Start with simple static sizes, then add size arithmetic and inference.

---

## 10. Contract Types

### What

Contract types add **runtime assertions** to types. A contract type `T { p }` represents values of type `T` that satisfy predicate `p` at runtime. Contracts enable:
- Gradual typing with runtime checks
- Defensive programming
- API boundary validation
- Runtime property verification

### Syntax (Proposed)

```clojure
;; Contract type syntax
(deftype PositiveInt { x : int | (>= x 0) })

;; Function with contract
(defn sqrt [x : { y : double | (>= y 0) }] : double
  :contract (>= result 0)
  ...)

;; Contract on function parameters and return
(defn divide [x : int, y : { z : int | (!= z 0) }] : int
  (/ x y))
```

### Motivating Examples

**Example 1: Runtime bounds checking**

```clojure
;; A vector access with runtime bounds check
(defn vec-get [v : (vec a), i : int] : a
  :contract (and (>= i 0) (< i (vec/len v)))
  (vec/get-unsafe v i))

;; If contract is violated, panic or return error
```

**Example 2: Gradual typing with contracts**

```clojure
;; A function that accepts any type with a contract
(defn ensure-positive [x : any] : { y : int | (>= y 0) }
  :contract (>= x 0)
  x)

;; Use in typed code
defn example [] : { y : int | (>= y 0) }
  (ensure-positive 42)
```

**Example 3: API boundary validation**

```clojure
;; Validate inputs at FFI boundaries
(extern-c some_c_function [x : int] : int
  :contract (>= x 0))
```

### Interaction with Existing Features

| Feature | Interaction | Notes |
|---|---|---|
| Borrow checker | Orthogonal | Contracts are runtime, borrow is compile-time |
| Typeclasses | Contracts can use typeclass methods | For runtime checking |
| Effects | Contracts can use effects | For error handling |
| Codegen | Contracts require runtime check insertion | Unless proven at compile time |

### Implementation Strategy

#### Phase CT0: Contract Type Foundations

- [ ] Add contract syntax to type annotations
- [ ] Parse contract predicates
- [ ] Store contracts in type system

#### Phase CT1: Contract Checking

- [ ] Insert runtime checks for contract predicates
- [ ] Contract failure handling (panic, return error, custom handler)
- [ ] Contract propagation through operations

#### Phase CT2: Contract Inference

- [ ] Infer contracts where possible
- [ ] Contract simplification
- [ ] Contract composition

#### Phase CT3: Contract Optimization

- [ ] Remove contracts that can be proven at compile time
- [ ] Inline simple contracts
- [ ] Contract caching

#### Phase CT4: Integration

- [ ] Stdlib: common contracts (bounds, null checks, etc.)
- [ ] Integration with FFI
- [ ] Contract type error messages

### Complexity Assessment

| Aspect | Complexity | Notes |
|---|---|---|
| Type system changes | Medium | Contract representation |
| Elaborator changes | Medium | Contract propagation |
| Codegen changes | Medium | Runtime check insertion |
| C emission | Medium | Runtime check code |
| Error messages | Medium | Contract violation explanations |

### Prior Art

- **Racket:** Contract system
- **Eiffel:** Design by Contract
- **Java (Annotations):** `@NotNull`, `@Positive`, etc.
- **Haskell (Liquid):** Refinement types with runtime checks
- **Python (pydantic):** Runtime type checking with contracts
- **TypeScript (Runtime):** Runtime type guards

### Recommendation

**✅ ACCEPT — Medium complexity, high value for gradual typing and defensive programming.**

Contract types provide:
1. Runtime verification of invariants
2. Gradual typing capabilities
3. Defensive programming support
4. API boundary validation

**Implementation priority:** Medium (can be implemented independently)

**Note:** Contracts can be implemented as a library using existing features (runtime checks, exceptions). However, built-in contract support provides better integration and error messages.

---

---

## Feature Comparison Matrix

| Feature | Complexity | Value | C99 Fit | Composability | Demand | Priority |
|---|---|---|---|---|---|---|
| [Linear Types](#1-linear-types) | Medium | High | ✅ Excellent | ✅ Excellent | High | **High** |
| [Substructural Types](#4-substructural-type-systems) | Medium | High | ✅ Excellent | ✅ Excellent | Medium | **High** |
| [Uniqueness Types](#3-uniqueness-types) | Low | Medium | ✅ Excellent | ✅ Excellent | Medium | **Medium** |
| [Session Types](#5-session-types) | High | High | ✅ Good | ✅ Good | Medium | **Medium-High** |
| [Intersection & Union](#7-intersection--union-types) | Medium | High | ✅ Good | ✅ Good | Medium | **Medium** |
| [Effect Types](#8-effect-types-row-polymorphism) | High | High | ✅ Good | ✅ Excellent | Medium | **High** |
| [Sized Types](#9-sized-types) | Medium | Medium | ✅ Excellent | ✅ Good | Low | **Medium** |
| [Contract Types](#10-contract-types) | Medium | Medium | ✅ Good | ✅ Good | Medium | **Medium** |
| [Dependent Types](#2-dependent-types) | Very High | High | ❌ Poor | ✅ Good | Low | **Deferred** |
| [Refinement Types](#6-refinement-types) | Very High | Medium | ⚠️ Fair | ✅ Good | Low | **Deferred** |

---

## Recommended Implementation Roadmap

### Phase 1: Linear & Substructural Types (v3)

**Duration:** 8-12 weeks
**Priority:** High

| Feature | Phases | Duration | Dependencies |
|---|---|---|---|
| Linear Types | LT0-LT4 | 6-8 weeks | Phase 19 |
| Uniqueness Types | UT0-UT3 | 2-4 weeks | Linear Types |
| Substructural Framework | ST0-ST3 | 2-4 weeks | Linear + Uniqueness |

**Rationale:** Linear types are the most impactful feature that aligns with Turmeric's existing ownership model. They provide compile-time memory safety guarantees without significant runtime overhead. Substructural types generalize this to a clean framework.

### Phase 2: Effect Types & Session Types (v3-v4)

**Duration:** 12-16 weeks
**Priority:** High

| Feature | Phases | Duration | Dependencies |
|---|---|---|---|
| Higher-Ranked Types | HRT0-HRT5 | 11.5-18 weeks | Phase 15 |
| Effect Types | ET0-ET4 | 8-12 weeks | Phase 19 + HRT1 |
| Session Types | SS0-SS4 | 8-12 weeks | Linear Types + Threads |

**Rationale:** Effect types are a natural extension of Phase 19's algebraic effects, providing type safety for effect usage. Session types enable type-safe concurrent programming. Both require rank-N type support.

### Phase 3: Union/Intersection & Contract Types (v4)

**Duration:** 8-12 weeks
**Priority:** Medium

| Feature | Phases | Duration | Dependencies |
|---|---|---|---|
| Intersection & Union Types | IT0-IT4 | 6-8 weeks | None |
| Contract Types | CT0-CT4 | 6-8 weeks | None |
| Sized Types | SZ0-SZ3 | 6-8 weeks | None |

**Rationale:** These features provide gradual typing, flexible APIs, and runtime verification. They can be implemented independently and provide immediate value.

### Phase 4: Deferred Features (v5+)

**Priority:** Low

| Feature | Reason for Deferral |
|---|---|
| Dependent Types | Very high complexity, unclear demand |
| Refinement Types | High complexity, limited C99 fit |
| GADTs (already planned) | Wait for v2 completion |

**Rationale:** These features have high complexity and unclear immediate value. They should be reconsidered when:
1. There is strong user demand
2. The other features are stable
3. There are clear use cases that cannot be addressed otherwise

---

## Feature Flag Strategy

All advanced type system features should be gated behind feature flags:

| Feature | Flag | Default | Notes |
|---|---|---|---|
| Linear Types | `-Xlinear` | Off | Enable when stable |
| Substructural Types | `-Xsubstructural` | Off | Includes linear, affine, relevant |
| Session Types | `-Xsessions` | Off | Requires threads |
| Effect Types | `-Xeffect-types` | Off | Extends Phase 19 |
| Union Types | `-Xunion-types` | Off | Can be default earlier |
| Intersection Types | `-Xintersection-types` | Off | Can be default earlier |
| Contract Types | `-Xcontracts` | Off | Runtime overhead |
| Sized Types | `-Xsized-types` | Off | Memory layout impact |

---

## Integration Testing Strategy

Each feature should include:

1. **Unit tests:** Individual type checking and codegen tests
2. **Integration tests:** Feature + existing features (typeclasses, effects, etc.)
3. **Performance tests:** Compile-time and runtime overhead benchmarks
4. **Interoperability tests:** FFI, C compatibility, ASan/UBSan clean

### Integration Test Matrix

| Feature | Typeclasses | Effects | Borrow Checker | RC | FFI | Threads |
|---|---|---|---|---|---|---|
| Linear Types | ✅ | ✅ | ✅ | ✅ | ✅ | ⚠️ |
| Substructural Types | ✅ | ✅ | ✅ | ✅ | ✅ | ⚠️ |
| Session Types | ✅ | ✅ | ⚠️ | ⚠️ | ✅ | ✅ |
| Effect Types | ✅ | ✅ | ✅ | ✅ | ✅ | ⚠️ |
| Union Types | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Intersection Types | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Contract Types | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Sized Types | ⚠️ | ⚠️ | ✅ | ⚠️ | ✅ | ⚠️ |

---

## Documentation Strategy

Each feature should include:

1. **User guide:** When to use, syntax, common patterns
2. **Language reference:** Formal semantics, type rules
3. **Cookbook:** Practical examples, best practices
4. **Migration guide:** How to adopt the feature in existing code

### Documentation Roadmap

| Phase | Documentation | Priority |
|---|---|---|
| Phase 1 | Linear Types Guide, Substructural Types Reference | High |
| Phase 2 | Effect Types Guide, Session Types Cookbook | High |
| Phase 3 | Union/Intersection Guide, Contract Types Reference | Medium |
| Phase 4 | Advanced Type System Reference Manual | Medium |

---

## Error Message Strategy

Each feature should have:

1. **Clear error codes:** `TUR_E01xx` for type system errors
2. **Actionable messages:** Explain what went wrong and how to fix it
3. **Context:** Show the relevant code and type information
4. **Suggestions:** Offer potential fixes

### Error Code Allocation

| Feature | Error Code Range | Examples |
|---|---|---|
| Linear Types | `TUR_E0100`-`TUR_E0149` | Linear value used twice, unconsumed linear value |
| Substructural Types | `TUR_E0150`-`TUR_E0199` | Affine value duplicated, relevant value unused |
| Session Types | `TUR_E0200`-`TUR_E0249` | Protocol mismatch, channel used after close |
| Effect Types | `TUR_E0250`-`TUR_E0299` | Effect not in row, effect row mismatch |
| Union Types | `TUR_E0300`-`TUR_E0349` | Union type mismatch, missing arm |
| Intersection Types | `TUR_E0350`-`TUR_E0399` | Intersection unsatisfiable, missing field |
| Contract Types | `TUR_E0400`-`TUR_E0449` | Contract violation, contract proof failed |
| Sized Types | `TUR_E0450`-`TUR_E0499` | Size mismatch, size overflow |

---

## Performance Considerations

### Compile-Time Performance

| Feature | Expected Overhead | Mitigation |
|---|---|---|
| Linear Types | Low | Usage tracking is local |
| Substructural Types | Low | Similar to linear types |
| Session Types | Medium | Protocol checking can be expensive |
| Effect Types | High | Row polymorphism adds unification complexity |
| Union Types | Medium | Subtyping checks |
| Intersection Types | Medium | Subtyping checks |
| Contract Types | Low | Contracts are runtime, not compile-time |
| Sized Types | Medium | Size arithmetic |

### Runtime Performance

| Feature | Expected Overhead | Mitigation |
|---|---|---|
| Linear Types | None | Zero-cost abstraction |
| Substructural Types | None | Zero-cost abstraction |
| Session Types | Low | Message passing primitives |
| Effect Types | Low | CPS transformation already exists |
| Union Types | Low | Tagged union representation |
| Intersection Types | None | Zero-cost abstraction |
| Contract Types | Medium | Runtime check insertion |
| Sized Types | Low | Size information for optimization |

### Memory Overhead

| Feature | Expected Overhead | Notes |
|---|---|---|
| Linear Types | None | No runtime representation |
| Substructural Types | None | No runtime representation |
| Session Types | Low | Channel struct overhead |
| Effect Types | None | Effect rows are compile-time only |
| Union Types | Low | Tag field in unions |
| Intersection Types | None | Zero-cost abstraction |
| Contract Types | Low | Runtime check code |
| Sized Types | None | Size is compile-time only |

---

## Migration Strategy

### Breaking Changes

Most features are **additive** and do not require breaking changes:
- Linear types: extend existing semantics
- Session types: new syntax, opt-in
- Effect types: extend Phase 19
- Union/Intersection types: new syntax, opt-in
- Contract types: new syntax, opt-in
- Sized types: new syntax, opt-in

### Non-Breaking Adoption

Users can adopt features gradually:
1. Enable feature flag
2. Add annotations to specific functions/types
3. Compiler provides warnings for potential issues
4. Gradually migrate codebase

### Deprecation Strategy

If a feature proves problematic:
1. Emit deprecation warning
2. Provide migration path
3. Remove in next major version

---

## Open Questions

### Linear Types

1. **Should `ref<T>` be redefined as linear?** ~~Currently `ref<T>` is move-only. Making it linear would provide stronger guarantees but might break existing code.~~
   **Decision:** Split into two types. `ref<T>` maps to `CK_UNIQUE` (at-most-one alias, drop freely -- the common case). A new `lref<T>` maps to `CK_LINEAR` (exactly-once, silent drop is an error -- for resource handles like `FileHandle`, `Socket`, `MutexGuard`). `CK_MOVE` is retired; see [uniqueness-types-plan.md](uniqueness-types-plan.md).
2. **How do linear types interact with `rc<T>`?** ~~`rc/clone` would need to consume the linear value and produce a shared value.~~
   **Decision:** Wrapping an `lref<T>` in `rc<T>` is forbidden. Shared ownership would break the exactly-once guarantee. The type checker rejects `rc/new` (and any `rc` constructor) applied to an `lref<T>` argument. New error `TUR_E0103`.
3. **Should there be a `^linear` annotation or should linearity be inferred?** ~~Inference is more ergonomic but less explicit.~~
   **Decision:** Explicit only. Linearity is opt-in via `lref<T>` or `^linear` annotation. No `--infer-linearity` flag. Keeps the contract clear at definition sites and avoids surprise errors from silent inference.

### Substructural Types

1. **Should affine, relevant, and linear types be separate or unified?** Separate distinct keywords (`^linear`, `^affine`, `^relevant`) are more readable than a single `^substructural(linear)` form. The unified elaborator infrastructure (a single `UsageState` machine) is still used internally; only the surface syntax is separate.
2. **How do substructural types interact with typeclasses?** A typeclass method declared with `^linear` parameters requires all instances to match that discipline -- the instance discipline must be at least as restrictive as the class declaration.

### Session Types

1. **Should sessions be linear by default?** Yes -- channels are `CK_LINEAR` by construction. Enabling `-Xsessions` implicitly enables `-Xlinear`.
2. **How do sessions interact with STM?** `choose`/`offer` are permitted inside `atomic` blocks (atomically committing a protocol branch decision); `send`/`recv` are forbidden. The elaborator rejects `send`/`recv` inside `atomic` to prevent a partially-advanced linear channel being stranded on transaction retry.
3. **Should sessions support timeouts?** Yes -- typed and protocol-aware. A `Timeout` constructor encodes both outcomes in the protocol type: `(Recv T (Timeout Q P))` continues as `Q` on message or `P` on timeout, returning the channel in both cases. Added in SS3.

### Effect Types

1. **Should effect rows be explicit or inferred?** Both -- explicit `forall [e]` quantification (ET2) and implicit generalisation of row variables are supported. Under `--strict-effects` (ER1, implied by `-Xeffect-types`) the elaborator emits a warning nudging authors toward explicit annotations.
2. **How do effect types interact with HRT?** Effect polymorphism (`forall [e]`) requires HRT Phase HRT1 (Rank-2 types). ET0--ET1 can proceed in parallel with HRT development; ET2 must wait for HRT1.
3. **Should there be effect subtyping?** Yes -- `effect_row_is_subset` is extended in ET4 to respect the stdlib effect hierarchy (e.g. a function performing `Write` satisfies a context requiring `IO`).

### Union/Intersection Types

1. **Should ADTs be syntactic sugar for unions?** Deferred to a v4 stretch goal. This would unify `defdata` and `(A | B)` syntax but is a larger refactor; implement union types first, then consider the sugar once they are stable.
2. **How do unions interact with pattern matching?** Direct support in `match` -- each arm narrows the union type; the elaborator checks exhaustiveness across all union members (error `TUR_E0301` for non-exhaustive matches).
3. **Should there be a `any` type?** Yes -- `any` is the top type, available when either `-Xunion-types` or `-Xintersection-types` is on. Making it always available without a flag may encourage untyped patterns and is deferred.
4. **Union subtyping and typeclasses?** Instance intersection -- typeclass methods available on all union members may be called directly; methods not in the intersection produce an error naming which member lacks the instance and suggesting a pattern match to narrow.

### Contract Types

1. **Should contracts be checked or trusted?** Checked in debug builds (`just build`) always; stripped in release builds (`just release`) by default. Pass `--keep-contracts` to retain them in release builds for safety-critical code.
2. **What is the contract failure handler?** Panic by default. A custom handler can be registered with `(set-contract-handler! f)` or scoped with `(with-contract-handler h body)`. See [contract-types-plan.md](contract-types-plan.md) §CT4.
3. **Should contracts be composable?** Yes -- contract conjunction (`{ x : T | (and p q) }`) is supported; contracts on higher-order function arguments propagate to the combinator's call sites. Contract predicates are restricted to pure expressions initially (no effects).
4. **Should contracts replace `stdlib/contract.tur` macros?** No -- they complement each other. `assert!`/`require!`/`ensure!` remain the imperative guard primitives; contract types are declarative type annotations layered on top.

### Sized Types

1. **Should sizes be static or dynamic?** Static sizes first (`StaticInt` type-level literals); dynamic sizes deferred. Static sizes cover the primary use cases (matrix dimensions, stack-allocated arrays, bit vectors) without runtime overhead.
2. **How do sized types interact with FFI?** Size information is used for C struct layout in `extern-c` declarations, allowing Turmeric to verify that a struct matches its declared C layout at compile time.
3. **Should sized types affect memory allocation?** Yes -- when the size is statically known, the elaborator may emit stack allocation (`alloca`) instead of heap allocation. This is a best-effort optimisation; heap allocation is the fallback.

---

## Resolved Decisions

| Decision | Resolution | Rationale |
|---|---|---|
| Feature flag strategy | All features behind flags | Allows incremental rollout, avoids breaking changes |
| Implementation order | Linear Types first | Highest value, lowest complexity, best alignment |
| Error message strategy | Clear codes + actionable messages | Improves developer experience |
| Documentation strategy | User guide + reference + cookbook | Comprehensive coverage for different learning styles |
| Testing strategy | Unit + integration + performance | Ensures correctness, compatibility, and performance |
| `ref<T>` vs. linear redefinition | Split: `ref<T>` → `CK_UNIQUE`, new `lref<T>` → `CK_LINEAR`; retire `CK_MOVE` | Avoids breaking existing code; `ref<T>` covers the common affine case; `lref<T>` is opt-in for resource handles |
| `lref<T>` + `rc<T>` interaction | Wrapping `lref<T>` in `rc<T>` is forbidden (type error `TUR_E0103`) | Shared ownership would break the exactly-once guarantee |
| Linear annotation vs. inference | Explicit only -- `lref<T>` or `^linear` annotation; no `--infer-linearity` | Keeps linearity contracts visible at definition sites |
| `CK_UNIQUE` aliasing | `^unique` values may not be aliased; `rc<T>` wrapping is forbidden (`TUR_E0202`) | Mirrors existing `ref<T>` guarantees; no per-field alias tracking |
| `CK_MOVE` retirement | Replaced by `CK_UNIQUE` (affine, at-most-one alias) | Cleaner two-primitive ownership model: unique + linear |

---

## References

### Linear Types
- [Linear Types for Haskell — GHC Proposal](https://github.com/ghc-proposals/ghc-proposals/blob/master/proposals/0111-linear-types.rst)
- [Pfenning & Rabanal — Linear Logic and Computation](https://www.cs.cmu.edu/~rwh/introspect/fwlf-icfp.pdf)
- [Wadler — Linear Types Can Change the World!](https://philipwadler.com/papers/linearity/linearity.pdf)

### Dependent Types
- [Idris — Dependent Types in Haskell](https://www.idris-lang.org/)
- [F* — Dependent Types for Verification](https://www.fstar-lang.org/)
- [Agda — Dependent Types with Coinduction](https://agda.readthedocs.io/en/latest/)

### Substructural Types
- [Pfenning & Davies — A Judgmental Reconstruction of Modal Logic](https://www.cs.cmu.edu/~rwh/introspect/modal.pdf)
- [Linear Haskell — Substructural Type System](https://hackage.haskell.org/package/linear-base)

### Session Types
- [Honda — Types for Dyadic Interaction](https://dl.acm.org/doi/10.1145/248206.248214)
- [Covington et al. — Session Types for C](https://dl.acm.org/doi/10.1145/1596553.1596586)

### Union & Intersection Types
- [Barbanera & Franzese — Intersection and Union Types: Syntax and Semantics](https://dl.acm.org/doi/10.1145/357052.357055)
- [TypeScript Handbook — Union and Intersection Types](https://www.typescriptlang.org/docs/handbook/2/everyday-types.html#union-types)

### Effect Types
- [Biernacki et al. — Eff Directly: An Effect System for OCaml](https://dl.acm.org/doi/10.1145/3371078)
- [Koka — Effect Types with Row Polymorphism](https://dl.acm.org/doi/10.1145/2887735)

### Refinement Types
- [Liquid Haskell — Refinement Types for Haskell](https://ucsd-progsys.github.io/liquidhaskell-blog/)
- [F7 — Refinement Types for F#](https://dl.acm.org/doi/10.1145/2660409.2660410)

### Contract Types
- [Racket — Contract System](https://docs.racket-lang.org/reference/contracts.html)
- [Findler & Felleisen — Contracts for Higher-Order Functions](https://dl.acm.org/doi/10.1145/351492.351503)

### Sized Types
- [Idris — Sized Types](https://idris-lang.org/papers/quantities/poppl14-quantities.pdf)
- [ATS — Sized Types for Memory Management](http://www.ats-lang.org/)

---

## Appendix A: Type System Feature Timeline

```
v1 (Current)
├── Typeclasses (Phase 15)
├── Borrow checking (Phase 12)
├── Reference counting (Phase 9)
├── Unique ownership (Phase 5)
└── Algebraic effects (Phase 19)

v2 (Planned)
├── Higher-Kinded Types (H0-H6)
├── Higher-Ranked Types (HRT0-HRT5)
├── GADTs (G0-G4)
├── STM (Phase 20-21)
└── Persistent collections (P1-P4)

v3 (Proposed - High Priority)
├── Linear Types (LT0-LT4)
├── Substructural Types (ST0-ST3)
├── Effect Types (ET0-ET4)
└── Session Types (SS0-SS4)

v4 (Proposed - Medium Priority)
├── Union Types (IT0-IT4)
├── Intersection Types (IT0-IT4)
├── Contract Types (CT0-CT4)
└── Sized Types (SZ0-SZ3)

v5+ (Deferred)
├── Dependent Types
└── Refinement Types
```

---

## Appendix B: Glossary

| Term | Definition |
|---|---|
| **Linear Type** | A type whose values must be used exactly once |
| **Affine Type** | A type whose values can be discarded but not duplicated |
| **Relevant Type** | A type whose values must be used but can be duplicated |
| **Dependent Type** | A type that depends on a runtime value |
| **Refinement Type** | A type with a runtime predicate constraint |
| **Session Type** | A type describing a communication protocol |
| **Effect Type** | A type that tracks effect usage |
| **Sized Type** | A type with size information |
| **Contract Type** | A type with runtime assertion |
| **Union Type** | A type representing a choice between alternatives |
| **Intersection Type** | A type representing a combination of requirements |
| **Substructural** | Type system without weakening or contraction |

---

## Appendix C: Related Documents

- [turmeric-plan.md](turmeric-plan.md) — Main design and implementation plan
- [higher-ranked-types-plan.md](archive/higher-ranked-types-plan.md) — HRT implementation
- [higher-kinded-types-plan.md](archive/higher-kinded-types-plan.md) — HKT implementation
- [gadts-plan.md](archive/gadts-plan.md) — GADT implementation
- [copy-borrow-move-lifetimes.md](archive/copy-borrow-move-lifetimes.md) — Ownership model analysis

---

*Last updated: 2026-05-11*
