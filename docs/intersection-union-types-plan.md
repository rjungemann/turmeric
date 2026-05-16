# Intersection & Union Types — Implementation Plan (IT0–IT4)

> **Status:** Draft — Not Started
>
> **Target:** v4
>
> **Prerequisites:** None (can be implemented independently); GADTs (v2) recommended for ADT-as-union sugar.
>
> **Related:** [advanced-type-system-feasibility-plan.md](advanced-type-system-feasibility-plan.md)
> (§7 Intersection & Union Types)
>
> **Last updated:** 2026-05-15

---

## Motivation

Intersection types (`A & B`) represent values that satisfy **both** `A` and `B`. Union types (`A | B`) represent values that satisfy **either** `A` or `B`. Together they enable:

- **Gradual typing** — mix typed and untyped (`any`) code with a clear upgrade path
- **Flexible API design** — functions that accept or return heterogeneous types without a wrapper ADT
- **Type-safe duck typing** — combine typeclass constraints with concrete types
- **Natural ADT encoding** — `Option a` as `(none | a)`, `Result e a` as `(err e | ok a)`

| Property | Today | Goal |
|---|---|---|
| Union types `(A \| B)` | **Not supported** | IT0–IT1 |
| Pattern matching on unions | **Not supported** | IT1 |
| Intersection types `(A & B)` | **Not supported** | IT2–IT3 |
| `any` type for gradual typing | **Not supported** | IT4 |
| ADTs as union sugar | **Not supported** | IT4 (stretch) |

---

## Proposed Syntax

```clojure
;; Union type -- value is one of the alternatives
(deftype IntOrString []
  (int | cstr))

;; Inline union in a function signature
(defn print-value [x : (int | cstr | bool)] : unit
  (match x
    (i : int)  (println i)
    (s : cstr) (println s)
    (b : bool) (println (if b "true" "false"))))

;; Intersection type -- value satisfies all constraints
(deftype ReadWrite []
  (Readable & Writable))

;; Intersection in a function signature
(defn save-int-or-str [x : (int & Serializable)] : unit
  (save x))
```

---

## Motivating Examples

### Example 1: Gradual typing

```clojure
;; Accept any type (dynamic, untyped boundary)
(defn debug-print [x : any] : unit
  (println x))

;; Narrow gradually as types become known
(defn typed-print [x : (int | cstr)] : unit
  (debug-print x))
```

### Example 2: Type-safe duck typing with intersection

```clojure
(defclass Serializable [a]
  (serialize [x : a] : cstr))

;; Accept only values that are both int and Serializable
(defn save-int [x : (int & Serializable)] : unit
  (file/write (serialize x) "output.txt"))
```

### Example 3: ADTs encoded as union types

```clojure
;; Option as a union
(deftype Option [a]
  (none | a))

;; Result as a union
(deftype Result [e a]
  (err e | ok a))
```

---

## Interaction with Existing Features

| Feature | Interaction | Notes |
|---|---|---|
| Typeclasses | Intersection `(A & Cls)` combines concrete type and constraint | Requires careful elaboration |
| Borrow checker | Union/intersection doesn't affect ownership | Orthogonal |
| GADTs | GADTs can be encoded as union types | Alternative to `defdata` syntax |
| Pattern matching | Union types need exhaustiveness checking per variant | Extend `match` elaborator |
| Codegen | Union types emit tagged unions (C `union` + discriminant) | Similar to existing ADT codegen |
| `any` type | Top of the union lattice | Subsumes all types |

---

## Architecture

The relevant source files:

```
src/types.h         -- TypeKind enum; Type struct; subtype relation
src/reader.c        -- Parser; new `|` and `&` infix type operators
src/elab.c          -- Elaborator; type application, pattern matching
src/typecheck.c     -- Unification; subtyping rules
src/codegen.c       -- Tagged union emission
src/error.h/.c      -- Error codes and messages
```

---

## Phase IT0 — Union Type Foundations

**Goal:** Parse and represent union types in the type system.

- [ ] Add `TY_UNION` to `TypeKind` in `src/types.h`:

  ```c
  typedef enum TypeKind {
      /* ... existing kinds ... */
      TY_UNION,         /* A | B | C */
      TY_INTERSECTION,  /* A & B & C  (Phase IT2) */
  } TypeKind;
  ```

- [ ] `TY_UNION` node stores a list of member types (order-independent)
- [ ] Parse `(A | B | C)` syntax in `src/reader.c`; flatten nested unions
- [ ] Pretty-print union types in error messages

---

## Phase IT1 — Union Type Checking

**Goal:** Implement subtyping and pattern matching for union types.

- [ ] **Subtyping:** `A <: (A | B)` and `B <: (A | B)`
  - A value of type `A` can be passed where `(A | B)` is expected
- [ ] **Function application:** a function `(-> (A | B) C)` accepts arguments of type `A` or `B`
- [ ] **Pattern matching on unions:**
  ```clojure
  (match x
    (n : int)  ...
    (s : cstr) ...)
  ```
  - Each arm narrows the type; the elaborator checks exhaustiveness across all union members
- [ ] **Widening:** a value of type `A` can be widened to `(A | B)` implicitly at call sites and return positions

### Error codes

| Code | Message |
|---|---|
| `TUR_E0300` | Union type mismatch: expected `{expected}`, got `{actual}` |
| `TUR_E0301` | Non-exhaustive pattern match on union type `{type}` -- missing arm for `{variant}` |

---

## Phase IT2 — Intersection Type Foundations

**Goal:** Parse and represent intersection types.

- [ ] Activate `TY_INTERSECTION` parsing: `(A & B & C)` syntax; flatten nested intersections
- [ ] `TY_INTERSECTION` node stores a list of member types
- [ ] Pretty-print intersection types in error messages

---

## Phase IT3 — Intersection Type Checking

**Goal:** Implement subtyping and elimination for intersection types.

- [ ] **Subtyping:** `(A & B) <: A` and `(A & B) <: B`
  - A value of intersection type carries all member types; you can use it as any member
- [ ] **Function application:** `(-> (A & B) C)` requires arguments that satisfy both `A` and `B`
- [ ] **Intersection introduction:** a value that is simultaneously `A` and `B` can be typed as `(A & B)` where the elaborator can prove membership in both
- [ ] **Intersection elimination:** from a value of type `(A & B)` you can project either `A` or `B`
- [ ] Typeclass intersection: `(int & Serializable)` means the value is an `int` with a `Serializable` dictionary

### Error codes

| Code | Message |
|---|---|
| `TUR_E0350` | Intersection type unsatisfiable: no value can be both `{A}` and `{B}` |
| `TUR_E0351` | Value of type `{actual}` does not satisfy intersection member `{missing}` |

---

## Phase IT4 — Integration and Polish

**Goal:** Add the `any` type, stdlib utilities, and ADT-as-union sugar.

- [ ] Add `any` as the **top type** (supertype of all types):
  - `A <: any` for all `A`
  - `(A | B | ... | any)` simplifies to `any`
  - Used for gradual typing boundaries
- [ ] Gradual typing utilities in stdlib:
  - `(cast x : T)` -- runtime downcast from `any`; returns `(option T)`
  - `(type-of x)` -- returns a runtime type tag
- [ ] Integration with typeclasses: typeclass instances are resolved for union members individually
- [ ] Codegen for union types:
  - Tagged union in C: `struct { int tag; union { A a; B b; } data; }`
  - Tags assigned at compile time; `match` compiles to a `switch` on the tag
- [ ] (Stretch) ADT-as-union sugar: `(defdata Option [a] (none | (some a)))` desugars to a union type
- [ ] `tur explain TUR_E0300` / `TUR_E0301` / `TUR_E0350` / `TUR_E0351` entries
- [ ] Performance benchmarks: tagged union emission overhead vs. existing ADT codegen

---

## Complexity Assessment

| Aspect | Complexity | Notes |
|---|---|---|
| Type system changes | Medium | Two new `TypeKind` variants; subtype lattice |
| Elaborator changes | Medium | Subtyping rules; exhaustiveness for union patterns |
| Codegen changes | Medium | Tagged union emission (similar to existing ADT codegen) |
| C emission | Medium | Union type C representation |
| Error messages | Medium | Union/intersection mismatch; missing pattern arms |

---

## Feature Flags

Union and intersection types are gated behind separate flags until stable:

```sh
turc -Xunion-types          myfile.tur
turc -Xintersection-types   myfile.tur
```

Both can be enabled together. The `any` type is available when either flag is on.

---

## Implementation Priority

**Medium** — v4, can be implemented independently of Linear Types and Effect Types.

Union types are the more immediately useful half (gradual typing, heterogeneous APIs); intersection types follow naturally. Start with union types in IT0–IT1 and ship them before tackling IT2–IT3.

---

## Open Questions

1. **Should ADTs become syntactic sugar for union types?** This would unify `defdata` and `(A | B)` syntax, but is a larger refactor. Consider as a v4 stretch goal after union types are stable.
2. **`any` type scope:** Should `any` require an explicit flag or be always available? Gradual typing without `any` has limited value; making it always available may encourage untyped patterns.
3. **Union subtyping and typeclasses:** ~~If `x : (int | cstr)`, which typeclass instances are in scope? Only the intersection of instances common to both, or must the programmer pattern-match first?~~
   **Decision:** Instance intersection (Option C). Typeclass methods available on *all* union members may be called directly on the union-typed value without a match. If a method is not in the intersection, the elaborator emits a helpful error naming which union member(s) lack the instance and suggesting a pattern match to narrow the type. The elaborator computes the instance intersection at the union type site.
4. **Intersection unsatisfiability:** ~~Should the compiler reject `(int & cstr)` statically (provably empty) or allow it and emit a runtime error?~~
   **Decision:** Static rejection for provably empty intersections (Option A). The elaborator rejects intersections of known-disjoint types (distinct primitives, distinct concrete structs) at construction time with `TUR_E0350`. Intersections involving typeclasses or type variables whose compatibility cannot be determined statically are permitted and may fail later during instance resolution. The check is bounded -- catch the obvious cases early, don't attempt full satisfiability solving.

---

## Prior Art

- **TypeScript:** Union (`A | B`) and intersection (`A & B`) types -- closest model
- **Flow:** Union and intersection types
- **Scala 3:** Union and intersection types
- **Ceylon:** Union and intersection types
- **OCaml:** Polymorphic variants (similar semantics to union types)
- **Haskell (GADTs):** Can encode union types via GADT constructors

---

## References

- [Barbanera & Franzese -- Intersection and Union Types: Syntax and Semantics](https://dl.acm.org/doi/10.1145/357052.357055)
- [TypeScript Handbook -- Union and Intersection Types](https://www.typescriptlang.org/docs/handbook/2/everyday-types.html#union-types)
- [advanced-type-system-feasibility-plan.md §7](advanced-type-system-feasibility-plan.md)
