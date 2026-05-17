# Intersection & Union Types — Implementation Plan (IT0–IT4)

> **Status:** IT0–IT4 complete (IT4 partial: `any` top type done; boxing codegen, `cast`/`type-of`, and ADT-as-union sugar deferred)
>
> **Target:** v4
>
> **Prerequisites:** None (can be implemented independently); GADTs (v2) recommended for ADT-as-union sugar.
>
> **Related:** [advanced-type-system-feasibility-plan.md](advanced-type-system-feasibility-plan.md)
> (§7 Intersection & Union Types)
>
> **Last updated:** 2026-05-16 (IT4 partial)

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
| ADTs as union sugar | **Not supported** | IT4 (stretch) — tracked in [gadts-plan.md](archive/gadts-plan.md) G4 |

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

- [x] Add `TY_UNION` to `TypeKind` in `src/types.h`:

  ```c
  typedef enum TypeKind {
      /* ... existing kinds ... */
      TY_UNION,         /* A | B | C */
      TY_INTERSECTION,  /* A & B & C  (Phase IT2) */
  } TypeKind;
  ```

- [x] `TY_UNION` node stores a list of member types (order-independent)
- [x] Parse `(A | B | C)` syntax in `src/elab.c`; flatten nested unions
- [x] Pretty-print union types in error messages

---

## Phase IT1 — Union Type Checking

**Goal:** Implement subtyping and pattern matching for union types.

- [x] **Subtyping:** `A <: (A | B)` and `B <: (A | B)`
  - A value of type `A` can be passed where `(A | B)` is expected
- [x] **Function application:** a function `(-> (A | B) C)` accepts arguments of type `A` or `B`
- [x] **Pattern matching on unions:**
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

- [x] Activate `TY_INTERSECTION` parsing: `(A & B & C)` syntax; flatten nested intersections
- [x] `TY_INTERSECTION` node stores a list of member types
- [x] Pretty-print intersection types in error messages

---

## Phase IT3 — Intersection Type Checking

**Goal:** Implement subtyping and elimination for intersection types.

- [x] **Subtyping:** `(A & B) <: A` and `(A & B) <: B`
  - A value of intersection type carries all member types; you can use it as any member
- [x] **Function application:** `(-> (A & B) C)` requires arguments that satisfy both `A` and `B`
- [x] **Intersection introduction:** a value that is simultaneously `A` and `B` can be typed as `(A & B)` where the elaborator can prove membership in both
- [x] **Intersection elimination:** from a value of type `(A & B)` you can project either `A` or `B`
- [x] Typeclass intersection: `(int & Serializable)` means the value is an `int` with a `Serializable` dictionary (open types are not rejected at construction time)

### Error codes

| Code | Message |
|---|---|
| `TUR_E0350` | Intersection type unsatisfiable: no value can be both `{A}` and `{B}` |
| `TUR_E0351` | Value of type `{actual}` does not satisfy intersection member `{missing}` |

---

## Phase IT4 — Integration and Polish

**Goal:** Add the `any` type, stdlib utilities, and ADT-as-union sugar.

- [x] Add `any` as the **top type** (supertype of all types):
  - `A <: any` for all `A` — any value accepted where `any` is expected
  - `(A | B | ... | any)` simplifies to `any` in `type_union_build()`
  - Available when `-Xunion-types` or `-Xintersection-types` is on
  - Represented as `int64_t` at codegen level (pointer-sized types require boxing, deferred)
- [x] `tur explain TUR_E0300` / `TUR_E0301` / `TUR_E0350` / `TUR_E0351` entries
- [ ] **Deferred — boxing codegen:** Proper `any`-typed values carrying pointer-sized payloads (cstr, struct, ADT) require a boxing wrapper struct; left for a follow-up phase
- [ ] **Deferred — gradual typing stdlib:**
  - `(cast x : T)` -- runtime downcast from `any`; returns `(option T)`
  - `(type-of x)` -- returns a runtime type tag
- [ ] **Deferred — tagged union codegen:**
  - Tagged union in C: `struct { int tag; union { A a; B b; } data; }`
  - Tags assigned at compile time; `match` compiles to a `switch` on the tag
- [ ] **Deferred — typeclass instance intersection on unions** (see Open Questions §3)
- [ ] **Deferred — ADT-as-union sugar:** `(defdata Option [a] (none | (some a)))` desugars to a union type — **tracked in [gadts-plan.md](archive/gadts-plan.md) Phase G4** (requires both `-Xgadt` and `-Xunion-types`)
- [ ] **Deferred — performance benchmarks:** tagged union emission overhead vs. existing ADT codegen

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

## Language Comparison

Turmeric's design follows the **TypeScript model**: structural, anonymous, closed unions and intersections with implicit widening and pattern-based narrowing. Here is how each prior-art language differs.

### TypeScript / Flow

- Union types are zero-cost -- TypeScript erases types to JavaScript at compile time; there is no discriminant tag and no memory overhead.
- Narrowing happens anywhere in control flow (`typeof`, `instanceof`, user-defined type guards), not only in `match`.
- Impossible intersections (`string & number`) simplify to `never` rather than being statically rejected.
- Intersection on record types *merges* their fields: `{ x: int } & { y: bool }` gives `{ x: int; y: bool }`.

**Turmeric diverges:** union types require a runtime discriminant tag (C has no type erasure); impossible intersections are statically rejected (`TUR_E0350`); narrowing is `match`-only; intersection on two concrete struct types is rejected, not merged.

### Scala 3

- Union and intersection are grounded in the DOT calculus -- a formal subtype lattice with full algebraic laws (commutativity, associativity, distributivity across `|` and `&`).
- `A & B` is the lattice *meet* and `A | B` is the lattice *join*; the compiler can always compute them for any two types.
- Explicit variance annotations (`+A`, `-A`) on type parameters make the system sound when union/intersection types appear inside generics.
- Intersection types merge trait members, not just constraints.

**Turmeric diverges:** no formal lattice laws are specified; variance is unaddressed; intersection is constraint-only, not trait-merge.

### OCaml Polymorphic Variants

- Unions are **open** (row-typed): a function can be typed to accept a *subset* of tags, and callers may pass a strict superset. This enables composable, extensible union APIs without changing existing code.
- The runtime tag is a hash of the constructor name -- no sequential discriminant int, no alignment padding to the largest member.
- Union member types are inferred from usage rather than declared up front.

**Turmeric diverges:** unions are **closed** -- the member set of `(int | cstr)` is fixed. You cannot write a function parameterized over "a union that includes at least `int`." This makes exhaustiveness checking straightforward but sacrifices the extensibility that polymorphic variants provide.

### Ceylon

- Flow-sensitive typing everywhere: `if (x is Foo)` narrows `x` to `Foo` in the true branch -- no explicit `match` required.
- Used union types to express nullability: `T?` is sugar for `T | Null`, eliminating a separate `Option` type.
- Types were reified at runtime (JVM), enabling `is T` checks without a separate tag.

**Turmeric diverges:** narrowing is `match`-only; `T?` is not sugar for `T | Null`; types are not reified (no general `is T` outside `match`).

### Rust

- No union or intersection types at the type-system level; enums are the union substitute (nominal, closed -- equivalent to Turmeric `defdata`).
- Trait bounds are the intersection substitute but only at function signatures (`impl Trait + Trait2`), not as first-class types.
- `dyn Trait` is a fat pointer (vtable), not a tagged union.

**Turmeric is more expressive here:** `(int & Serializable)` is a first-class type, not merely a parameter-level constraint.

### Summary table

| Design axis | Turmeric | TypeScript | OCaml variants | Scala 3 |
|---|---|---|---|---|
| Union openness | Closed | Closed | Open (row-typed) | Closed |
| Narrowing | `match` only | Anywhere in control flow | `match` only | `match` + flow |
| Intersection semantics | Constraint-only | Field merge | N/A | Full lattice meet |
| Runtime cost | Tagged union struct | Zero (type erasure) | Hashed tag word | JVM erased |
| Impossible intersections | Statically rejected | Reduced to `never` | N/A | Reduced to `Nothing` |
| Variance with generics | Not addressed | Unsound by design | Inferred | Explicit (`+/-`) |
| Algebraic laws | Not specified | Not specified | Structural | Full DOT lattice |

---

## Known Limitations and Tradeoffs

### 1. Tagged union overhead -- widening has a runtime cost

TypeScript's union types are zero-cost (erased). Turmeric emits
`struct { int tag; union { A a; B b; } data; }`. Every union-typed value pays
one extra `int` for the tag plus alignment padding to the largest member. A
value of type `(int | cstr | bool)` is larger than `int64_t` on the stack and
in struct fields, with a `switch` at every `match` site. This matters for
arrays, struct fields, and cache pressure.

Implicit widening (IT1) -- passing a bare `42 : int` where `(int | cstr)` is
expected -- requires constructing the tagged union at the call site (writing the
tag and copying the value). The elaborator must insert explicit coercion nodes;
it is not a free annotation.

### 2. Narrowing is `match`-only -- no flow-sensitive typing

Without flow-sensitive narrowing, `type-of` checks outside `match` do not
change the elaborated type of the scrutinee:

```clojure
;; x is still (int | cstr) in both branches; no narrowing occurs
(if (= (type-of x) "int")
  (+ x 1)   ; elaboration error: (int | cstr) is not int
  ...)

;; The correct form requires match
(match x
  (n : int)  (+ n 1)
  (s : cstr) ...)
```

`(cast x : T)` returns `(option T)`, which is safe but forces a nested `match`
to unwrap.

### 3. Intersection on concrete struct types is rejected, not merged

TypeScript and Scala 3 merge the fields of `{ x: int } & { y: bool }` into
`{ x: int; y: bool }`. Turmeric statically rejects intersections of two known-
disjoint concrete types (`TUR_E0350`). Intersection is useful only when at
least one side is a typeclass constraint. Programmers coming from TypeScript
will expect record merging and find this surprising.

### 4. Closed unions limit library extensibility

Because union types are closed, a library returning `(int | ParseError)` cannot
be transparently composed with a library returning `(bool | ParseError)` to
produce `(int | bool | ParseError)` -- an explicit adapter or a new union type
is required. OCaml's polymorphic variants solve this at the cost of significant
inference complexity.

### 5. `any` is sound but less ergonomic than TypeScript's `any`

TypeScript's `any` allows unsound casts to any type without handling failure.
Turmeric's `(cast x : T)` returns `(option T)`, which is the correct choice for
a systems language but is closer to TypeScript's `unknown` in ergonomics.
Gradual-typing boundary code will be more verbose.

### 6. Instance intersection on unions defers failures to instance resolution

When `x : (int | cstr)`, only typeclass methods implemented by *both* `int` and
`cstr` may be called without a `match`. For unions involving unresolved type
variables or typeclasses, the plan permits the intersection to succeed at the
type-checking site and fail later during instance resolution. This deferred
failure is a type-checks-but-fails-to-link scenario that can be difficult to
diagnose. Future work should consider bounding this or making the failure
site more predictable.

### 7. Variance with generics is unaddressed

The plan does not specify variance for type constructors containing union or
intersection types. Passing `vec<(int | cstr)>` where `vec<int>` is expected,
or vice versa, could introduce unsoundness. TypeScript has known unsoundness in
this area (arrays are covariant by design choice). This should be addressed
before union/intersection types are enabled by default.

---

## References

- [Barbanera & Franzese -- Intersection and Union Types: Syntax and Semantics](https://dl.acm.org/doi/10.1145/357052.357055)
- [TypeScript Handbook -- Union and Intersection Types](https://www.typescriptlang.org/docs/handbook/2/everyday-types.html#union-types)
- [advanced-type-system-feasibility-plan.md §7](advanced-type-system-feasibility-plan.md)
