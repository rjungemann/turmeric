# Contract Types — Implementation Plan (CT0–CT4)

> **Status:** Draft — Not Started
>
> **Target:** v4
>
> **Prerequisites:** None -- can be implemented independently. Existing `contract.tur` runtime assertions provide a foundation but are library-level only.
>
> **Related:** [../../guides/advanced-type-system-rationale.md](../../guides/advanced-type-system-rationale.md)
> (§10 Contract Types, §6 Refinement Types)
> [contracts-plan.md](contracts-plan.md)
>
> **Last updated:** 2026-05-15

---

## Motivation

Contract types add **runtime-checked predicates** to types as a first-class language feature. A contract type `T { p }` represents values of type `T` that satisfy predicate `p`, verified at runtime when the value crosses a checked boundary.

The stdlib already provides `assert!`, `require!`, and `ensure!` macros in `stdlib/contract.tur`. This plan promotes contracts from library conventions to compiler-integrated type annotations, enabling:

- **Defensive programming** -- pre/post-conditions verified automatically at function boundaries
- **Gradual typing** -- `any`-typed values can be narrowed with runtime checks
- **FFI boundary validation** -- C function calls can carry typed contracts
- **API contracts** -- library authors express invariants directly in types rather than prose

Contract types intentionally do not require an SMT solver. Predicates are arbitrary Turmeric expressions evaluated at runtime. This distinguishes them from refinement types (§6 of the feasibility plan), which aim for static proof. Contract types trade static completeness for simplicity and zero new compiler infrastructure.

| Property | Today | Goal |
|---|---|---|
| Runtime assertions (`assert!`, `require!`) | **Library macros** (stdlib/contract.tur) | CT0: syntax in type annotations |
| Function pre/post-conditions | **Informal** (doc convention) | CT1: `:pre` / `:post` clauses |
| Contract propagation through operations | **Manual** | CT1: elaborator inserts checks |
| Contract inference / optimisation | **None** | CT3: eliminate provably-true checks |
| FFI contract annotations | **None** | CT4: `extern-c` with `:contract` |
| Gradual typing via contracts | **None** | CT4: `(cast x : T)` runtime downcast |

---

## Proposed Syntax

```clojure
;; Contract type: int that must be non-negative
(deftype Nat { x : int | (>= x 0) })

;; Inline contract in a function signature
(defn sqrt [x : { y : double | (>= y 0) }] : double
  ...)

;; Pre- and post-condition on a function
(defn divide [x : int, y : int] : int
  :pre  (!= y 0)
  :post (= (* result y) x)
  (/ x y))

;; Contract on a struct field
(defstruct BoundedBuffer [
  data  : (vec int)
  index : { i : int | (and (>= i 0) (< i (vec/len data))) }])

;; FFI contract
(extern-c some_c_function [x : int] : int
  :contract (>= x 0))
```

---

## Motivating Examples

### Example 1: Bounds-checked vector access

```clojure
;; The contract documents and enforces the requirement at the call site
(defn vec-get-checked [v : (vec a), i : { x : int | (and (>= x 0) (< x (vec/len v))) }] : a
  (vec/get-unsafe v i))  ; no redundant check inside

;; At the call site, the elaborator inserts the predicate check
(defn example [v : (vec int)] : int
  (vec-get-checked v 3))
;; Emits (at runtime):
;;   assert((3 >= 0) && (3 < vec_len(v)), "contract violated: i in bounds")
;;   vec_get_unsafe(v, 3)
```

### Example 2: Gradual typing with contracts

```clojure
;; Accept any type; contract narrows at runtime
(defn ensure-positive [x : any] : { y : int | (>= y 0) }
  :contract (>= x 0)
  x)

(defn pipeline [] : { y : int | (>= y 0) }
  (ensure-positive 42))
```

### Example 3: API boundary validation via FFI

```clojure
;; Contract is checked before the C call and on return
(extern-c sqlite3_column_int [stmt : ptr, col : int] : int
  :pre  (and (!= stmt null) (>= col 0))
  :post (>= result 0))
```

### Example 4: Pre/post-conditions

```clojure
(defn safe-sqrt [x : double] : double
  :pre  (>= x 0.0)
  :post (>= result 0.0)
  (sqrt x))

;; At call site, :pre is checked before the call,
;; :post is checked on the return value.
```

---

## Interaction with Existing Features

| Feature | Interaction | Notes |
|---|---|---|
| `stdlib/contract.tur` | CT0 is a syntax sugar layer over the existing macros | `assert!` becomes the runtime primitive |
| Borrow checker | Orthogonal -- contracts are runtime, borrow checking is compile-time | No interference |
| Typeclasses | Contracts can call typeclass methods in predicates | Predicate expression is a normal Turmeric expression |
| Algebraic effects | Contracts may use effects (e.g. logging) | Controlled by the `:contract-effect` annotation |
| Codegen | Contracts insert check expressions before/after function calls | Debug vs. release builds may strip checks |
| `any` type (intersection-union-types-plan.md) | Contract types enable gradual typing for `any` values | `(cast x : T)` is a contract check |
| Refinement types (feasibility §6) | Contract types are runtime; refinement types are static | Orthogonal; contracts do not require SMT |

---

## Architecture

```
src/types.h         -- TY_CONTRACT; ContractExpr representation
src/reader.c        -- Parse { x : T | p } syntax; :pre / :post / :contract clauses
src/elab.c          -- Contract insertion at call sites and scope boundaries
src/codegen.c       -- Emit runtime check expressions
stdlib/contract.tur -- Existing assert!/require!/ensure! macros (unchanged)
src/error.h/.c      -- Error codes TUR_E0400-TUR_E0449
```

---

## Phase CT0 — Contract Type Foundations

**Goal:** Parse and represent contract types; hook into existing `assert!` infrastructure.

- [x] Add `TY_CONTRACT` to `TypeKind` in `src/types.h`:

  ```c
  typedef struct ContractType {
      Type*  base_type;    /* The underlying type T */
      char*  var_name;     /* Bound variable x in { x : T | p } */
      Expr*  predicate;    /* Predicate expression p */
  } ContractType;
  ```

- [x] Parse `{ x : T | p }` in `src/reader.c` (braces, `:`, `|` are new type-level syntax)
- [x] Parse `:pre expr`, `:post expr`, `:contract expr` clauses on `defn` and `extern-c`
- [x] Store contracts in the `FnDef` node alongside parameter types
- [x] Contract predicates are **ordinary Turmeric expressions** -- no special predicate language

---

## Phase CT1 — Contract Checking

**Goal:** Insert runtime checks in generated code at function entry, exit, and call sites.

- [x] **Precondition (`:pre`):** inserted at the top of the function body, before any user code
- [x] **Postcondition (`:post`):** inserted before each return site; the result is bound to `result` in the predicate expression
- [x] **Inline contract (`{ x : T | p }`):** inserted at the point where the value crosses the type boundary (function call, assignment, return)
- [x] Contract failure handling: `panic` by default; a custom handler can be registered with `(set-contract-handler! f)`
- [x] Contract predicates have access to the enclosing scope (they are closures over the surrounding environment)
- [x] The inserted check calls `assert!` from `stdlib/contract.tur`, which already handles error formatting

### Contract insertion sites

| Site | When checked |
|---|---|
| Function entry | `:pre` predicates; `{ x : T | p }` on parameters |
| Function return | `:post` predicates; `{ x : T | p }` on return type |
| Let binding | `{ x : T | p }` on the bound variable's type |
| `extern-c` call | `:pre` before call; `:post` on return value |

### Error codes

| Code | Message |
|---|---|
| `TUR_E0400` | Contract violated: `{predicate}` is false for value `{value}` |
| `TUR_E0401` | Postcondition violated: `{predicate}` is false for result `{value}` |

---

## Phase CT2 — Contract Inference

**Goal:** Propagate and simplify contracts through operations.

- [ ] If `x : { i : int | (>= i 0) }` and `y : { i : int | (>= i 0) }`, then `(+ x y) : { i : int | (>= i 0) }` (preserve non-negativity through addition -- conservative inference only)
- [x] Standard type widening: `{ x : T | p }` is a subtype of `T`; the plain `T` is always a valid use of the contract type (with the contract stripped -- the check was already done)
- [ ] Contract conjunction: `{ x : T | p } ∩ { x : T | q }` simplifies to `{ x : T | (and p q) }`
- [ ] Inference is **local and conservative** -- no global analysis; do not infer contracts that were not written

---

## Phase CT3 — Contract Optimisation

**Goal:** Eliminate redundant checks at compile time.

- [ ] If the elaborator can prove the predicate is always true for a given value (e.g. literal `42` satisfies `(>= x 0)`), elide the runtime check
- [ ] Dead-code elimination: contracts on unreachable branches are removed
- [x] **Release mode** (`just release`): strip all contract checks by default; add `--keep-contracts` flag to retain them in release builds
- [x] **Debug mode** (`just build`): all contracts active
- [x] The optimisation is best-effort; it is always safe to leave a contract check in place

---

## Phase CT4 — Integration

**Goal:** FFI contracts, gradual typing, stdlib, and error UX.

- [x] `extern-c` contract annotations (`:pre`, `:post`) -- parsed and stored on `ExternC`; codegen-side emission deferred to CT4-B
- [ ] `(cast x : T)` runtime downcast from `any` (requires intersection-union-types-plan.md):
  - Equivalent to `{ y : any | (instance-of? y T) }` with a user-friendly error
  - Returns `(option T)` on failure rather than panicking (configurable)
- [x] Update `stdlib/contract.tur` to expose the contract handler API:
  - `(set-contract-handler! (fn [msg location] : unit ...))`
  - `(with-contract-handler h body)` -- scoped handler override
- [ ] `tur explain TUR_E0400`, `TUR_E0401` entries with source location and predicate text
- [x] Integration tests: `contract-pre`, `contract-post`, `contract-type` fixtures all pass

---

## Complexity Assessment

| Aspect | Complexity | Notes |
|---|---|---|
| Type system changes | Medium | `TY_CONTRACT` node; contract subtype relation |
| Elaborator changes | Medium | Contract insertion pass; `:pre`/`:post` on `FnDef` |
| Codegen changes | Medium | Check expression emission; debug/release switch |
| C emission | Medium | `assert!` macro expansion; handler call |
| Error messages | Medium | Include predicate text and actual value; source location |

---

## Feature Flag

```sh
turc -Xcontracts myfile.tur
```

In release mode (`just release`), contracts are compiled out unless `--keep-contracts` is passed. In debug mode (`just build`), contracts are always active regardless of the feature flag.

---

## Implementation Priority

**Medium** — v4, can be implemented independently.

Contract types have no dependencies on other advanced type-system features and can be implemented any time after the elaborator is stable. They provide immediate value for defensive programming and FFI safety with moderate compiler complexity.

---

## Open Questions

1. **Contract effects:** ~~Should contract predicates be allowed to perform effects (e.g. logging)? If so, the contract's effect row must be accounted for in the enclosing function's type. Start by restricting contracts to pure predicates.~~
   **Decision:** Pure predicates only for now (Option D). The elaborator enforces `#{}` on all contract predicate expressions. If real usage reveals a need for effectful predicates, add row tracking in a later phase. Revisit once CT0--CT4 are stable and usage patterns are established.
2. **Release-mode stripping:** ~~Some teams want contracts in release builds for safety-critical code. Should `-Xcontracts` imply keep-in-release, or should that be a separate flag?~~
   **Decision:** Separate flags always (Option B). `-Xcontracts` enables contract syntax and debug-mode checking only. `--keep-contracts` is an explicit opt-in to retain checks in release builds (`just release`). The default is strip-in-release -- the safe choice for performance-sensitive systems code. Per-contract `^always` granularity may be layered on top in a later phase without breaking this design.
3. **Composability of contracts on higher-order functions:** If `f : (-> { x : int | p } int)` is passed to a higher-order combinator, does the contract on `f`'s argument type propagate into the combinator's call sites? Likely yes, at the call site.
4. **Overlap with `stdlib/contract.tur`:** The existing `assert!`, `require!`, `ensure!` macros already cover many use cases. Contract types should complement rather than replace them. Document the distinction: macros are imperative guards; contract types are declarative type annotations.

---

## Prior Art

- **Racket:** First-class contract system -- the closest reference implementation
- **Eiffel:** Design by Contract (pre/post-conditions as a core language concept)
- **Python (pydantic):** Runtime type checking with data validation
- **Haskell (Liquid Haskell):** Refinement types with runtime fallback
- **Java (`@NotNull`, `@Positive`):** Annotation-driven contract checking
- **TypeScript (runtime guards):** Manual runtime type narrowing

---

## References

- [Findler & Felleisen -- Contracts for Higher-Order Functions](https://dl.acm.org/doi/10.1145/351492.351503)
- [Racket Contract System](https://docs.racket-lang.org/reference/contracts.html)
- [../../guides/advanced-type-system-rationale.md §10](../../guides/advanced-type-system-rationale.md)
