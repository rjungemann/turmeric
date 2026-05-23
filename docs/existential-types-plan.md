# Existential Types Plan

> **Status:** Draft Plan
> **Last Updated:** 2026-05-22
> **Type:** Language Design

---

## Overview

Existential types let you hide a type variable behind an opaque boundary:
`exists a. F a` means "there is some type `a` satisfying `F`, but I am not
telling you which one." The outside world can only use the type through an
agreed-upon interface (a typeclass constraint or an explicit operation set).

This plan covers the motivation, design options, and complexity of adding
existential types to Turmeric. It is a **design-only draft** -- no
implementation is started here.

---

## Motivation

### 1. Hiding size indices (`Vec[A]` vs. `SizedVec[n A]`)

`docs/upcoming/typed-collections-plan.md` defines `Vec[A]` and `SizedVec[n A]`
as completely separate types (Phase TC2 decision). With existential types, a
third option becomes viable:

```turmeric
;; Option B from typed-collections-plan.md:
(deftype Vec [A] = (exists n. SizedVec[n A]))
```

A `Vec[A]` would be a `SizedVec` whose length index is sealed away. You could
convert in both directions:

```turmeric
;; Reveal the size (existential open):
(open-vec [v :Vec[A]] ([n] [sv :SizedVec[n A]])
  ;; n is a fresh abstract Nat; sv is the underlying SizedVec
  ...)

;; Pack any SizedVec back into a Vec (existential introduction):
(defn vec-of-sized [sv :SizedVec[n A]] :Vec[A]
  (pack sv as Vec[A]))
```

This would make `Vec[A]` a first-class abstraction over length-indexed
vectors, enabling code that works with both.

### 2. Type-erased heterogeneous collections

Without existentials, a list of "things that can be shown" requires a concrete
shared type. With existentials:

```turmeric
(deftype Showable = (exists a. (Show a) => a))

(let [items (list (pack 1    as Showable)
                  (pack "hi" as Showable)
                  (pack 3.14 as Showable))]
  (list-map show items))
```

### 3. Abstract handles / capabilities

Existentials are a natural model for opaque handles where the caller must not
inspect internals:

```turmeric
(deftype Handle[Op] = (exists s. (Op s) => s))
```

This overlaps with the existing `capability.tur` and `:linear` work, and could
provide a cleaner foundation for both.

---

## Implementation Options

Three approaches are ordered from least to most complex.

---

### Option A -- Struct-encoded existentials (no new type-checker machinery)

Encode existentials as structs carrying a value plus a vtable of operations.
No `exists` keyword. The programmer writes the encoding by hand; stdlib
provides helper macros.

```turmeric
;; Manual encoding of (exists a. (Show a) => a):
(defstruct Showable
  (value  :int)                    ; the boxed value (erased type)
  (show-f :ptr<void>))             ; fn [x :int] :cstr

(defmacro pack-showable [x]
  `(Showable ~x (fn [v] (show (unbox v)))))
```

**Complexity:** Low. Requires no changes to the type-checker or parser.
Already partially expressible with existing `defstruct` and function pointers.

**Limitations:**
- Every existential type needs its own hand-written struct.
- No compiler verification that the vtable is correctly typed.
- Cannot express `exists n. SizedVec[n A]` this way -- the hidden variable is
  a type-level Nat, not a value, so there is no vtable to store.

**Verdict:** Covers use cases 2 and 3, but not use case 1 (the
`Vec[A]`/`SizedVec[n A]` unification).

---

### Option B -- `pack`/`open` with typeclass constraints (medium complexity)

Add `exists` as a first-class type constructor, with `pack` (introduction) and
`open` (elimination). The hidden type variable must carry at least one
typeclass constraint.

```turmeric
;; Type syntax:
(exists a. (Constraint a) => T)

;; Introduction -- pack:
(pack value as (exists a. (Show a) => a))

;; Elimination -- open (binding form):
(open expr ([a] [x : T])
  body)   ; a and x are bound here; a must not escape body's type
```

The "no escape" rule is the key invariant: inside the `open` block you have a
fresh abstract type `a` and a value `x : T[a]`, but `a` cannot appear in the
type of any value that flows *out* of the block. The type-checker must enforce
this with a scope check on the existential variable.

**Type-checker changes required:**
- Extend the type grammar with `(exists a. ...)`.
- Extend unification to treat existential variables as skolem constants during
  elimination (they unify with nothing except themselves within their scope).
- Add a scope escape check: after `open`, no reference to `a` may appear in
  the inferred type of `body`.
- Extend `defclass` resolution to thread the hidden constraint witness through
  `pack`/`open` automatically.

**Parser changes:** `pack`, `open`, and `exists` as new keywords.

**Runtime:** No runtime cost beyond what boxing already provides. `pack` is a
no-op at runtime (the value is already boxed as `int64_t`); the constraint
witness is a pointer to a vtable allocated at `pack` time.

**Does this cover use case 1?** Partially. `(exists n. SizedVec[n A])` hides a
*type-level Nat* (`n`), not a value. If `n` is a phantom index (as in the
current `SizedVec` GADT), the hidden variable carries no runtime representation
and there is no vtable. The `open` form would still need to bind `n` as an
abstract phantom type. This is expressible in Option B if the type-checker
treats phantom type variables as valid existential variables -- but it requires
care to avoid the escape of `n` through the length of any array that comes out
of the `open` block.

**Complexity:** Medium. Scope checking for existential variables is the hardest
part; it is well-understood (standard in System F and GHC's implementation) but
requires a non-trivial addition to the Turmeric type-checker.

---

### Option C -- First-class existential types with type functions (high complexity)

Full `exists a. F a` where `F` can be any type function, including ones not
constrained by a typeclass. Subsumes Options A and B.

Adds:
- Type-level lambda / type functions (if not already present).
- Higher-kinded existentials: `exists (f :: * -> *). f A`.
- Interaction with GADTs: GADT constructors already carry implicit existentials
  (e.g. `SVCons` hides the predecessor Nat). Full existentials would make these
  explicit and usable at the type level.

**Complexity:** High. Requires:
- A kind system (to type-check type functions).
- Unification in the presence of higher-kinded variables.
- Careful interaction with the existing GADT elaboration.
- Likely incompatible with full type inference -- annotations required at all
  pack/open sites (same caveat as GHC's `ExistentialQuantification`).

**Verdict:** Deferred. This is the right long-term direction but should follow
dependent types work, not precede it.

---

## Recommended Path

| Phase | Approach | Prerequisite |
|-------|----------|--------------|
| EX0 | Option A -- struct-encoded existentials + macro helpers | None |
| EX1 | Option B -- `pack`/`open` with typeclass constraints | Typeclass system complete |
| EX2 | Revisit Option B for phantom type variables (Vec/SizedVec use case) | TC2 (typed collections) shipped |
| EX3 | Option C -- full existential types | Dependent types phase |

EX0 can ship without any language changes and closes use cases 2 and 3.
EX1 closes them more cleanly and enables the constrained-existential pattern
used widely in Haskell and OCaml. EX2 is what enables Option B from
`typed-collections-plan.md`.

---

## Complexity Summary

| Concern | Option A | Option B | Option C |
|---------|----------|----------|----------|
| Parser changes | None | `pack`, `open`, `exists` keywords | Type-level lambda syntax |
| Type-checker changes | None | Skolemization + scope escape check | Kind system + HK unification |
| Runtime changes | None | Vtable allocation at `pack` | None beyond Option B |
| Inference impact | None | Annotations required at pack/open | Annotations required everywhere |
| Covers Vec/SizedVec unification | No | Partially (EX2) | Yes |
| Risk | Low | Medium | High |

The main implementation risk in Option B is the **scope escape check** --
ensuring that the hidden type variable does not leak out of an `open` block.
This is conceptually simple (a free-variable check on the inferred type of the
body) but interacts subtly with let-generalization, GADTs, and any future
dependent-type elaboration.

---

## Relation to Other Plans

- `typed-collections-plan.md` -- TC2 `Vec[A]` and `SizedVec[n A]` are kept
  separate until EX2 lands. EX2 enables Option B: `Vec[A] = exists n. SizedVec[n A]`.
- `refinement-types-plan.md` -- refinements and existentials are orthogonal but
  interact at the boundary where a refinement narrows the hidden type.
- `currying-plan.md` -- curried functions as existentials (`exists a b. a -> b`)
  is a known encoding; keep compatible.
