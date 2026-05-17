# Generalized Algebraic Data Types (GADTs) Implementation Plan for Turmeric

> **Status:** Draft — Not Started  
> **Prerequisite:** Phase 15 (Typeclasses) must be complete; HRT phases HRT0–HRT2 required (bidirectional checking + skolems); plain ADTs (Phase G0) must land first  
> **Target:** v4 or later  
> **Related:** [higher-ranked-types-plan.md](higher-ranked-types-plan.md) §Non-Goals item 4; [higher-kinded-types-plan.md](higher-kinded-types-plan.md) §Non-Goals item 3

---

## Executive Summary

Generalized Algebraic Data Types (GADTs) extend conventional algebraic data types (ADTs) by allowing each constructor to specialize — or *refine* — the type parameters of the data type it returns. In a plain ADT every constructor shares the same polymorphic return type; in a GADT each constructor may return a more specific instantiation, and the type-checker *learns* those refinements when pattern-matching.

The practical consequence: programs that were previously enforced only by programmer discipline become enforced by the type system. A typed expression interpreter, for example, can be made *impossible* to evaluate at the wrong type without a runtime cast.

**Primary motivators:**

1. Typed abstract syntax trees — represent well-typed programs as values (the classic "tagless-final" and "PHOAS" patterns)
2. Type-safe heterogeneous collections indexed by a phantom type
3. Length-indexed and shape-indexed vectors (`Vec n a` where `n` is a type-level natural)
4. Proof-carrying data — constructors that embed type-equality witnesses
5. Type-safe printf / format strings parameterized over their argument list
6. Typed delimited continuations and effect tags with precise return types

**Decision rule:** Ship if ≥2 of: (1) library authors need typed AST representations, (2) users need type-safe heterogeneous containers without unsafe coercions, (3) the effect system would benefit from GADT-indexed operation types.

**Key constraint:** GADTs require bidirectional type checking (from HRT) for the refinement to propagate through `match` arms. Unguarded type inference in GADT match is undecidable in general; Turmeric uses *annotation-guided* checking: the scrutinee type is always known (inferred or annotated) and refinement flows downward into each arm.

---

## Motivating Examples

Each example is paired with the non-GADT alternative to show the expressiveness gap.

### 1. Typed expression interpreter

**Without GADTs** you carry a runtime tag and do unsafe casts:

```clojure
; untyped — runtime tag needed
(defdata Expr
  (IntLit int)
  (BoolLit bool)
  (Add Expr Expr)
  (IsZero Expr))

(defn eval-expr [e : Expr] : (option any)
  (match e
    (IntLit n)   (some n)
    (BoolLit b)  (some b)
    (Add l r)
      (match [(eval-expr l) (eval-expr r)]
        [(some a) (some b)]  (some (+ (cast int a) (cast int b)))
        _  (none))
    (IsZero e)
      (match (eval-expr e)
        (some n)  (some (= (cast int n) 0))
        _         (none))))
```

**With GADTs** the type parameter `a` tracks what type each expression evaluates to:

```clojure
; typed — no casts, no option, no runtime tag
(defgadt Expr [a]
  (IntLit  : (-> int             (Expr int)))
  (BoolLit : (-> bool            (Expr bool)))
  (Add     : (-> (Expr int) (Expr int)   (Expr int)))
  (IsZero  : (-> (Expr int)              (Expr bool))))

(defn eval-expr [e : (Expr a)] : a
  (match e
    (IntLit n)    n
    (BoolLit b)   b
    (Add l r)     (+ (eval-expr l) (eval-expr r))
    (IsZero e)    (= (eval-expr e) 0)))
```

> **The GADT insight:** In the `Add` arm the type-checker knows `a = int`; in `IsZero` it knows `a = bool`. No casts needed. The return type `a` is automatically refined per branch.

---

### 2. Length-indexed vectors (type-level naturals)

```clojure
; Type-level naturals as GADTs
(defgadt Nat []
  (Zero : Nat)
  (Succ : (-> Nat Nat)))

; Length-indexed vector — length is in the type
(defgadt Vec [n a]
  (VNil  :                             (Vec Zero a))
  (VCons : (-> a (Vec n a) (Vec (Succ n) a))))

; safe-head is total — VNil is rejected at compile time
(defn safe-head [v : (Vec (Succ n) a)] : a
  (match v
    (VCons x _)  x))

; zip requires equal lengths — enforced by the type
(defn vzip [xs : (Vec n a), ys : (Vec n b)] : (Vec n [a b])
  (match [xs ys]
    [VNil VNil]                     VNil
    [(VCons x xt) (VCons y yt)]     (VCons [x y] (vzip xt yt))))
```

> `safe-head` cannot be called on an empty `VNil` — the type `(Vec Zero a)` does not match `(Vec (Succ n) a)` and the compiler rejects it.

---

### 3. Type-safe printf format strings

```clojure
; A format descriptor — each constructor refines 'args', the list of arg types
(defgadt Fmt [args]
  (FDone  :                               (Fmt []))
  (FLit   : (-> cstr (Fmt rest)           (Fmt rest)))
  (FInt   : (-> (Fmt rest)                (Fmt (cons int rest))))
  (FStr   : (-> (Fmt rest)                (Fmt (cons cstr rest)))))

; printf is total over any format descriptor
(defn printf [fmt : (Fmt args)] : (apply-args args unit)
  ...)

; Usage — argument count and types are checked at compile time
(printf (FInt (FLit " + " (FInt (FLit " = " FDone)))) 3 4)
; → "3 + 4 = "   (error: missing third int argument caught at compile time)
```

---

### 4. Proof-carrying equality witnesses

```clojure
; Type equality evidence — `(Equal a b)` is a proof that a = b
(defgadt Equal [a b]
  (Refl : (Equal a a)))   ; only constructor; a = b forces a and b to unify

; Using equality to coerce safely
(defn coerce-with [eq : (Equal a b), x : a] : b
  (match eq
    (Refl x)))   ; in this arm a = b, so x : a = x : b

; Symmetry
(defn sym [eq : (Equal a b)] : (Equal b a)
  (match eq
    (Refl Refl)))

; Transport: if a = b then (f a) = (f b)
(defn transport [eq : (Equal a b), fa : (f a)] : (f b)
  (match eq
    (Refl fa)))
```

---

### 5. Typed effect tags (interaction with Phase 19 effects)

```clojure
; Effect operations parameterized by their result type
(defgadt Op [a]
  (Read  :                (Op cstr))
  (Write : (-> cstr       (Op unit)))
  (Alloc : (-> int        (Op (vec int)))))

; A handler that is total over Op — no default case needed
(defn handle-io [op : (Op a)] : a
  (match op
    (Read)         (read-line)
    (Write s)      (println s)
    (Alloc n)      (vec/new n)))
```

---

## Usage Tutorial

### Step 1 — Plain ADTs first

Before using GADTs, write a plain sum type with `defdata`:

```clojure
; A plain ADT — all constructors return (Color)
(defdata Color
  (Red)
  (Green)
  (Blue))

; A parameterized ADT — all constructors return (Option a)
(defdata Option [a]
  (None)
  (Some a))

; Pattern match using (match)
(defn option-map [f : (-> a b), opt : (Option a)] : (Option b)
  (match opt
    (None)    (None)
    (Some x)  (Some (f x))))
```

---

### Step 2 — Your first GADT

Switch from `defdata` to `defgadt` and add explicit return-type annotations on constructors:

```clojure
; Enable GADTs with the compiler flag
; ./build/tur build -Xgadt my-file.tur

(defgadt Tag [a]
  (IntTag  : (Tag int))
  (BoolTag : (Tag bool))
  (PairTag : (-> (Tag a) (Tag b) (Tag [a b]))))

; A function that knows the result type from the tag
(defn default-value [t : (Tag a)] : a
  (match t
    (IntTag)        0
    (BoolTag)       false
    (PairTag ta tb) [(default-value ta) (default-value tb)]))
```

If you omit a return-type annotation on a constructor, the compiler treats it as a plain ADT constructor (all type parameters are unconstrained). If the compiler detects that the annotation is needed for type refinement, it emits:

```
error: constructor 'IntTag' in defgadt refines type variable 'a' — explicit return type required
  hint: add `: (Tag int)` after the constructor name
```

---

### Step 3 — Type refinement in match arms

In each `match` arm for a GADT, the type-checker introduces *skolem equalities* from the constructor's return type. These are invisible to the programmer:

```clojure
(defgadt Witness [a]
  (WInt  : (Witness int))
  (WBool : (Witness bool)))

(defn show-witness [w : (Witness a), v : a] : cstr
  (match w
    ; In this arm: a ~ int, so (v : a) is treated as (v : int)
    (WInt)   (int->cstr v)
    ; In this arm: a ~ bool, so (v : a) is treated as (v : bool)
    (WBool)  (bool->cstr v)))
```

The refinement `a ~ int` in the `WInt` arm means the compiler can verify that `(int->cstr v)` is valid without any runtime tag or cast.

---

### Step 4 — GADT constructors with field parameters

Constructors can carry payload fields alongside the return-type annotation:

```clojure
(defgadt Result [e a]
  (Ok  : (-> a          (Result e a)))
  (Err : (-> e          (Result e a))))

; Exhaustive match over both arms
(defn result-map [f : (-> a b), r : (Result e a)] : (Result e b)
  (match r
    (Ok v)   (Ok (f v))
    (Err e)  (Err e)))
```

---

### Step 5 — Mutual recursion and nested GADTs

```clojure
; Mutually recursive GADT: terms and types of a simply-typed lambda calculus
(defgadt Ty [a]
  (TInt  : (Ty int))
  (TBool : (Ty bool))
  (TFn   : (-> (Ty a) (Ty b) (Ty (-> a b)))))

(defgadt Term [a]
  (Var   : (-> cstr          (Term a)))
  (Lam   : (-> cstr (Ty a) (Term b) (Term (-> a b))))
  (App   : (-> (Term (-> a b)) (Term a) (Term b)))
  (Num   : (-> int             (Term int)))
  (If    : (-> (Term bool) (Term a) (Term a) (Term a))))
```

---

### Step 6 — Explicit equality witnesses

When you need to pass type equality evidence explicitly (e.g., returning it from a function), use the built-in `(Equal a b)` GADT:

```clojure
; Equal is provided by the standard library
; (defgadt Equal [a b] (Refl : (Equal a a)))

(defn int-eq-int : (Equal int int)
  Refl)

; Transport a value across a proven equality
(defn cast-equal [eq : (Equal a b), x : a] : b
  (match eq
    (Refl x)))
```

---

### Step 7 — Interaction with typeclasses

GADT constructors respect typeclass constraints:

```clojure
; Require Show for the payload type
(defgadt ShowBox [a : Show]
  (Box : (-> a (ShowBox a))))

(defn unbox-show [s : (ShowBox a)] : cstr
  (match s
    (Box v)  (show v)))

; Collect heterogeneous showable values
(def items : (vec (ShowBox _))
  [(Box 42) (Box true) (Box "hello")])

(map unbox-show items)  ; → ["42", "true", "hello"]
```

---

### Common mistakes and error messages

| Mistake | Error | Fix |
|---|---|---|
| Using `defgadt` syntax without `-Xgadt` | `unknown form 'defgadt' (pass -Xgadt to enable)` | Add `-Xgadt` to the build command |
| Missing return-type annotation on a refining constructor | `constructor refines type variable — explicit return type required` | Add `: (MyType ...)` after the constructor |
| Non-exhaustive match on a GADT | `non-exhaustive match: missing constructor 'Foo'` | Add the missing arm |
| Attempting to unify rigid GADT skolem with unrelated type | `type mismatch: cannot unify 'int' with 'bool' (skolem from 'BoolTag')` | Ensure the correct arm of the match is used |
| Returning a skolem type outside its match arm | `skolem type variable 'a' escapes its match scope` | Use a universally polymorphic return type |
| Storing a GADT in a container without type annotation | `ambiguous GADT type parameter — annotation required` | Add `: (Vec (MyGadt concrete))` annotation |

---

## Phase Overview

| Phase | Deliverable | Exit Criterion | Estimated Effort |
|---|---|---|---|
| G0 | Plain ADTs — `defdata` + `match` | Sum types with exhaustiveness checking; tagged-union C codegen | Medium (2–3 weeks) |
| G1 | GADT syntax and AST | `defgadt` with constructor return-type annotations; no type refinement yet | Small (0.5–1 week) |
| G2 | Type refinement in `match` | Skolem equalities per arm; bidirectional checking propagates refinements | Hard (3–5 weeks) |
| G3 | Type equality witnesses | Built-in `Equal` GADT; `coerce`; interaction with HRT skolems and HKT kinds | Medium (2–3 weeks) |
| G4 | Integration & polish | Stdlib patterns, error messages, documentation, performance benchmarks | Medium (1–2 weeks) |

---

## Prerequisites Checklist

Before starting Phase G0, verify:

- [ ] Phase 15 (Typeclasses, kind-`*`) is complete and stable
- [ ] `TY_STRUCT` covers product types; a new `TY_ADT` can be added without conflict
- [ ] `Expr` node enum in `src/expr.h` has space for `EX_MATCH`, `EX_DEFDATA`, `EX_DEFGADT`
- [ ] `src/elab.c` has bidirectional checking (from HRT — HRT0–HRT2 complete) before G2
- [ ] `src/types.h` can represent type equality constraints (a `TY_EQ_WITNESS` node) before G3
- [ ] Diagnostics (`src/diag.c`) can show type-level diffs with GADT skolem names

---

## Phase G0 — Plain ADTs (Sum Types)

**Goal:** Add `defdata` for user-defined sum types and `match` for pattern matching. This phase has no type-refinement semantics; every constructor returns the declared ADT type. This is a prerequisite for all subsequent GADT phases and is independently useful.

### Tasks

#### Surface syntax (`src/reader.c`, `src/forms.c`)
- [ ] Recognize `defdata` as a type-definition keyword
  ```clojure
  (defdata Name [:copy]
    (Ctor1)
    (Ctor2 FieldType1 FieldType2)
    ...)
  ```
- [ ] Support type parameters: `(defdata Option [a] (None) (Some a))`
- [ ] `[:copy]` modifier (same semantics as `defstruct :copy`) for copy-able ADTs
- [ ] Recognize `match` as a special form:
  ```clojure
  (match scrutinee
    (Ctor1)          body1
    (Ctor2 x y)      body2
    _                default-body)
  ```
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
- [ ] Emit a C tagged union: `struct tur_adt_Name { int tag; union { struct { ... } Ctor1; struct { ... } Ctor2; } as; }`
- [ ] Constructor functions emit a struct initializer with the correct tag
- [ ] `match` emits a `switch (x.tag)` with a `case` per constructor, binding field names to local variables
- [ ] Integrate with existing copy/move/drop infrastructure (CopyKind propagation)
- [ ] For copy ADTs, emit `tur_adt_Name_copy`; for move ADTs, ensure single-owner discipline

#### Memory management
- [ ] ADT with all copy-able fields defaults to `CK_COPY`
- [ ] ADT containing any move-only field is `CK_MOVE`
- [ ] Recursive ADT fields (e.g., list nodes) must use `ref<T>` or `rc<T>` to avoid infinite-sized structs

### Fixtures
- [ ] `adt-basic.tur` — `Color` enum, exhaustive match
- [ ] `adt-param.tur` — `Option [a]`, `(option-map f (Some 3))`
- [ ] `adt-nested.tur` — `(match (Some (Some 42)) (Some (Some n)) n ...)`
- [ ] `adt-nonexhaustive.tur` — non-exhaustive match caught at compile time
- [ ] `adt-copy.tur` — copy-able ADT assigned twice without error
- [ ] `adt-move.tur` — move ADT triggers borrow error on second use
- [ ] `adt-recursive.tur` — linked list `(defdata List [a] (Nil) (Cons a (ref (List a))))`

### Exit criterion
Sum types declared, constructed, and pattern-matched correctly; exhaustiveness enforced; C codegen compiles cleanly; copy/move semantics correct; all G0 fixtures green.

---

## Phase G1 — GADT Syntax and AST

**Goal:** Introduce `defgadt` as a syntactic variant of `defdata` in which each constructor carries an explicit return-type annotation. This phase only parses and represents the refined type in the AST — no type checking of the refinements yet.

### Tasks

#### Surface syntax (`src/reader.c`, `src/forms.c`)
- [ ] Recognize `defgadt` keyword
  ```clojure
  (defgadt Name [type-params...]
    (Ctor1 : return-type)
    (Ctor2 FieldType : return-type)
    ...)
  ```
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

### Fixtures
- [ ] `gadt-syntax-basic.tur` — `defgadt` with return-type annotations parses
- [ ] `gadt-syntax-multi.tur` — multiple type parameters in GADT
- [ ] `gadt-syntax-error.tur` — missing annotation on a constructor in `defgadt` is rejected
- [ ] `gadt-syntax-flag.tur` — `defgadt` without `-Xgadt` gives hint

### Exit criterion
GADT definitions parse; `EX_DEFGADT` nodes printed correctly in diagnostics; validation pass catches malformed constructors; no runtime codegen differences from G0 yet.

---

## Phase G2 — Type Refinement in Pattern Matching

**Goal:** Implement type-directed refinement in `match` arms for GADT scrutinees. When a GADT constructor with a specialized return type is matched, the type-checker introduces *skolem equalities* that refine free type variables in the current scope for the duration of that arm's body.

**Depends on:** HRT0–HRT2 (bidirectional checking, skolem variables, rigid type variables).

### Tasks

#### Bidirectional checking for GADT match (`src/elab.c`)
- [ ] In `match` elaboration, detect if the scrutinee has a `TY_ADT` with `is_gadt = true`
- [ ] For each arm, call `elab_match_arm_gadt`:
  1. Look up the `CtorDef` and its `result_type`
  2. Unify `result_type` with the scrutinee type under a *fresh local skolem environment*
  3. Each type argument in `result_type` that is a type variable gets bound as a skolem equality (e.g., `a ~ int`)
  4. Elaborate the arm body under the extended environment with skolem equalities
  5. At arm exit, discard the skolem equalities
- [ ] Ensure the inferred type of each arm body is consistent with the overall match result type (which may itself be refined)
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

### Fixtures
- [ ] `gadt-refine-basic.tur` — `Witness` tag GADT; `show-witness` compiles and runs
- [ ] `gadt-refine-expr.tur` — typed expression interpreter; `eval-expr` evaluates correctly
- [ ] `gadt-refine-nat.tur` — `Nat` / `Vec n a` length-indexed vector; `safe-head` rejected on `VNil`
- [ ] `gadt-refine-escape.tur` — skolem variable escaping arm caught at compile time
- [ ] `gadt-refine-exhaustive.tur` — unreachable arm produces warning; missing reachable arm produces error

### Exit criterion
Type refinement propagates correctly through GADT match arms; `eval-expr` and `safe-head` compile and run; unreachable-arm warnings emitted; skolem escape caught; C codegen compiles cleanly; all G2 fixtures green.

---

## Phase G3 — Type Equality Witnesses

**Goal:** Introduce first-class type equality evidence as the built-in `Equal` GADT, a `coerce` form, and a `(~)` constraint notation for functions that demand type equality. This enables programs to carry equality proofs as values, enabling safe coercions, transport lemmas, and richer indexed data structures.

**Depends on:** G2 complete.

### Tasks

#### Built-in `Equal` GADT (`stdlib/equal.tur`, `src/elab.c`)
- [ ] Define `Equal` in the standard library with compiler-magic support:
  ```clojure
  (defgadt Equal [a b]
    (Refl : (Equal a a)))
  ```
- [ ] The `Refl` constructor is special: it forces `a = b` at the call site (unification under the current skolem environment)
- [ ] Matching on `Refl` introduces `a ~ b` as a skolem equality in the arm body
- [ ] `Equal` is recognized by the elaborator as the canonical equality witness type

#### `coerce` form (`src/forms.c`, `src/elab.c`)
- [ ] Introduce `(coerce eq x)` where `eq : (Equal a b)` and `x : a`, producing `x : b`
- [ ] Elaboration: check `eq` has type `(Equal a b)`, check `x` has type `a`, produce type `b`
- [ ] Codegen: `coerce` is a no-op at runtime — identical memory layout for `a` and `b` in all cases
- [ ] Error if `eq` is not an `Equal` value: `coerce requires an (Equal a b) proof as first argument`

#### `(~)` constraint notation (`src/reader.c`, `src/elab.c`)
- [ ] Allow `(~ a b)` as a constraint in `defn` parameter lists:
  ```clojure
  (defn only-int [^(~ a int) x : a] : int
    x)
  ```
- [ ] Elaborate `(~ a b)` constraints as equality assumptions (not typeclass dictionaries)
- [ ] Interaction with typeclass constraints: `(~ a int)` and `^Eq a` may coexist

#### Symmetry, transitivity, and congruence (`stdlib/equal.tur`)
- [ ] `(defn equal-sym [eq : (Equal a b)] : (Equal b a) ...)`
- [ ] `(defn equal-trans [eq1 : (Equal a b), eq2 : (Equal b c)] : (Equal a c) ...)`
- [ ] `(defn equal-cong [eq : (Equal a b)] : (Equal (f a) (f b)) ...)` — requires HKT (deferred to G4 integration)

#### Interaction with HRT
- [ ] An `(Equal a b)` proof inside a `forall` creates a rank-2 equality:
  ```clojure
  (defn heterogeneous-eq
    [f : (forall [a b] (-> (Equal a b) a b))]
    ...)
  ```
- [ ] Skolems from GADT refinement and skolems from `forall` (HRT) share the same rigid variable infrastructure
- [ ] No conflict between HRT `∀-intro` skolems and GADT arm skolems (different scopes)

#### Interaction with HKT (deferred refinement, G4)
- [ ] `(Equal (f a) (f b))` requires `f` to be a type constructor of kind `* -> *` — requires HKT kinds
- [ ] `equal-cong` is deferred to G4 (Integration) once HKT kind system is available

### Fixtures
- [ ] `gadt-equal-refl.tur` — `Refl` used to prove `(Equal int int)`
- [ ] `gadt-equal-coerce.tur` — `coerce` transports a value across a proof
- [ ] `gadt-equal-sym.tur` — symmetry of `Equal`
- [ ] `gadt-equal-trans.tur` — transitivity of `Equal`
- [ ] `gadt-equal-constraint.tur` — `(~)` constraint in `defn` signature
- [ ] `gadt-equal-error.tur` — `coerce` without valid proof rejected

### Exit criterion
`Equal` GADT usable; `coerce` is a type-safe zero-cost cast; `(~)` constraint notation works in `defn`; symmetry and transitivity stdlib functions compile and pass tests; all G3 fixtures green.

---

## Phase G4 — Integration & Polish

**Goal:** Production-ready GADT support: stdlib patterns, comprehensive error messages, performance validation, and documentation.

### Tasks

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
  ```clojure
  (defn equal-cong [^Functor f, eq : (Equal a b)] : (Equal (f a) (f b)) ...)
  ```
- [ ] Requires kind-annotated type variable `f : * -> *`

#### ADTs as union sugar (stretch)
- [ ] Desugar `(defdata Option [a] (None) (Some a))` to a union type `(None | (Some a))` internally
- [ ] Desugar `(defdata Result [e a] (Ok a) (Err e))` to `(Ok a | Err e)` internally
- [ ] Only applies when both `-Xgadt` and `-Xunion-types` flags are active
- [ ] Plain `defdata` without the union-types flag continues to emit the existing tagged-union C struct
- [ ] Exhaustiveness checking and pattern matching behaviour are unchanged from the existing ADT path
- [ ] See [intersection-union-types-plan.md](../intersection-union-types-plan.md) IT4 for the union-types side of this work

#### Guard clauses in `match`
- [ ] Add `when` guard syntax to `match` arms:
  ```clojure
  (match e
    (IntLit n) when (> n 0)  (do-positive-thing n)
    (IntLit n)               (do-general-thing n)
    ...)
  ```
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

### Fixtures
- [ ] `gadt-stdlib-nat.tur` — type-level `Nat` arithmetic compiles and evaluates
- [ ] `gadt-stdlib-vec.tur` — `VNil`/`VCons` operations; `safe-head` rejects empty vector at compile time
- [ ] `gadt-guard.tur` — `when` guard clauses in GADT match
- [ ] `gadt-integration.tur` — GADT + HRT + typeclasses + closures + defer
- [ ] `gadt-asan.tur` — no memory errors under ASan for GADT-heavy programs

### Exit criterion
All stdlib patterns compile and execute; documentation complete; performance benchmarks acceptable; `equal-cong` available once HKT lands; all G4 fixtures green.

---

## Non-Goals

1. **Dependent types** — Types that depend on *runtime values* (e.g., a vector indexed by a runtime integer). GADTs index only on *type-level* quantities. Full dependent types are a separate, larger feature.
2. **Recursive type schemes (μ-types)** — Isorecursive types with explicit `fold`/`unfold` are out of scope.
3. **Coercions between unrelated GADTs** — `coerce` only works with an `Equal` proof; no unsafe cast backdoor.
4. **Overlapping GADT instances** — If two constructors have the same return type (no refinement), the usual non-overlapping rule applies.
5. **GADT inference without scrutinee type** — If the scrutinee type is completely unknown, GADT refinement cannot proceed. The programmer must annotate.
6. **Higher-rank GADT constructor fields** — `(Ctor : (-> (forall [a] ...) (MyGadt b)))` is deferred until HRT and GADT phases are both stable.
7. **Pattern matching in `let` / `defn` arguments** — Irrefutable pattern binding (`let [(Some x) (compute)]`) is deferred to G4+.

---

## Resolved Questions

1. **`defdata` vs. `defgadt` as separate forms:** Two separate keywords.
   - Rationale: Keeps plain ADTs simple and avoids confusing new users with GADT annotations when they just want a sum type. Tooling can also distinguish the two without inspecting constructor annotations.

2. **Return-type annotation required on all constructors in `defgadt`:** Yes, all constructors require `: return-type`.
   - Rationale: Mixing annotated and unannotated constructors would make the exhaustiveness logic ambiguous. If no annotation is needed, use `defdata`.

3. **Runtime representation of GADTs:** Identical to plain ADTs — a C tagged union.
   - Rationale: Refinement is purely a compile-time artifact. No additional runtime overhead vs. plain ADTs.

4. **`-Xgadt` feature flag:** Required to enable `defgadt`.
   - Rationale: Consistent with `-Xhrt` / `-Ximpredicative` opt-in model; allows rolling out without breaking existing code.

5. **`coerce` as a built-in form rather than a library function:** Built-in.
   - Rationale: `coerce` needs to be a zero-cost reinterpretation at the type level; a library function would need unsafe FFI or an intermediate allocation. A built-in can emit the identity directly.

---

## Open Questions

1. **Should `Equal` be a compiler builtin or a user-definable GADT?** A fully user-definable GADT `(defgadt Equal [a b] (Refl : (Equal a a)))` is cleanest, but requires the compiler to recognize `Refl` as a special constructor for `coerce` to work. A hybrid (user-syntax, compiler-special) is the likely approach. Decision needed before G3.

2. **Recursive GADT fields without `ref` or `rc`:** The `Vec` and `Term` examples contain recursive fields. Should the compiler automatically box recursive GADT fields, or require explicit `ref`? Auto-boxing is ergonomic; explicit `ref` is consistent with the rest of the ownership model. Assess after G0.

3. **Interaction with algebraic effects (Phase 19):** The `Op` GADT example (§Motivating Example 5) uses a GADT to type effect operations. Should `defeffect` be lowered to a `defgadt` internally, or remain a separate concept? Evaluate after G2 is stable.

4. **Guard exhaustiveness:** When `when` guards are present (G4), the exhaustiveness checker becomes undecidable in general (guard conditions are arbitrary expressions). Options: (a) treat guarded arms as always incomplete (conservative), (b) add a `(complete-when ...)` annotation to assert coverage. Decide in G4.

5. **`tur_type_descriptor_t` alignment with `tur_poly_t` (HRT4):** GADT constructors with polymorphic fields will eventually need `tur_poly_t` descriptors. Coordinate field layout with the HRT4 plan before finalizing the `CtorDef` struct.

---

## Rollout Plan

1. **`-Xgadt` feature flag** — All GADT features behind this flag; plain ADTs (`defdata`) do not require it
2. **G0 as standalone release** — Plain ADTs are broadly useful and can ship independently of GADT type refinement
3. **G1 + G2 experimental** — Ship together as "GADT v1" behind `-Xgadt` in a minor version
4. **G3 in the following minor** — Equality witnesses add complexity; separate release once G2 stabilizes
5. **G4 polish** — Iterative improvements to error messages and stdlib; can overlap with G3
6. **Default-on** — Enable `-Xgadt` by default in a future major version once `defdata` + `defgadt` + `match` are proven stable

---

## Estimated Timeline

| Phase | Duration | Dependencies |
|---|---|---|
| G0 | 2–3 weeks | Phase 15 complete; `TY_STRUCT` / `StructDef` patterns understood |
| G1 | 0.5–1 week | G0 complete |
| G2 | 3–5 weeks | G1 complete; HRT0–HRT2 complete (bidirectional checking + skolems) |
| G3 | 2–3 weeks | G2 complete |
| G4 | 1–2 weeks | G3 complete; HKT H0–H1 recommended for `equal-cong` |
| **Total** | **9–14 weeks** | |

---

## References

- [Higher-Ranked Types Plan](higher-ranked-types-plan.md) — skolem variables, bidirectional checking (prerequisite for G2)
- [Higher-Kinded Types Plan](higher-kinded-types-plan.md) — kind system, needed for `equal-cong` in G4
- [Turmeric Phase 15: Typeclasses](turmeric-plan.md) — typeclass system (prerequisite for G0)
- [Simple Unification-Based Type Inference for GADTs — Peyton Jones et al. 2006](https://www.microsoft.com/en-us/research/publication/simple-unification-based-type-inference-for-gadts/)
- [Wobbly Types: Type Inference for GADTs — Peyton Jones et al. 2004](https://www.microsoft.com/en-us/research/publication/wobbly-types-type-inference-for-gadts/)
- [Complete and Easy Bidirectional Typechecking for Higher-Rank Polymorphism — Dunfield & Krishnaswami 2013](https://arxiv.org/abs/1306.6032)
- [GHC User's Guide — GADTs Extension](https://ghc.gitlab.haskell.org/ghc/doc/users_guide/exts/gadt.html)
- [Fun with Phantom Types — Hinze 2003](https://www.cs.ox.ac.uk/ralf.hinze/publications/With.pdf)
- [Type-Safe Observable Sharing in Haskell — Gill 2009](https://dl.acm.org/doi/10.1145/1596638.1596641) — motivation for typed ASTs
