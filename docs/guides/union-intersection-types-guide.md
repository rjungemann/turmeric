---
title: Union and Intersection Types Guide
category: Language Features
description: Union (`A | B`) and intersection (`A & B`) types, `any`, gradual typing
---

# Union and Intersection Types Guide

> **Feature flags:** `-Xunion-types` and `-Xintersection-types`
>
> IT0--IT4 are complete. Some IT4 items are deferred (boxing codegen, `cast`, `type-of`,
> tagged union C emission). See [Deferred](#deferred) below.

Union types (`A | B`) and intersection types (`A & B`) extend the Turmeric type system with
structural type combinations. Together they enable gradual typing, flexible APIs, and
type-safe duck typing without wrapper ADTs.

---

## Enabling

Both features are gated behind separate flags:

```sh
turc -Xunion-types          myfile.tur   # union types only
turc -Xintersection-types   myfile.tur   # intersection types only
turc -Xunion-types -Xintersection-types  # both
```

The `any` type is available when either flag is active.

---

## Union Types

A union type `(A | B)` represents a value that is **either** `A` or **`B`**. The compiler
emits a tagged-union C struct at runtime.

### Syntax

```turmeric
;; Named union type
(deftype IntOrString []
  (int | cstr))

;; Inline in a function signature
(defn print-value [x : (int | cstr | bool)] : unit
  (match x
    (i : int)  (println i)
    (s : cstr) (println s)
    (b : bool) (println (if b "true" "false"))))
```

Nested unions are flattened: `(int | (cstr | bool))` becomes `(int | cstr | bool)`.

### Pattern Matching on Union Types

Use `match` to narrow a union-typed value. The elaborator checks exhaustiveness across
all union members:

```turmeric
(defn describe [x : (int | cstr)] : cstr
  (match x
    (n : int)  (str "number: " n)
    (s : cstr) (str "string: " s)))
```

Omitting any member is a compile-time error (`TUR_E0301`).

### Subtyping

A value of type `A` can be passed anywhere `(A | B)` is expected (widening). This is
handled implicitly at call sites and return positions:

```turmeric
(defn accepts-union [x : (int | cstr)] : unit ...)

(accepts-union 42)       ;; int widens to (int | cstr)
(accepts-union "hello")  ;; cstr widens to (int | cstr)
```

### Typeclass Methods on Unions

When `x : (int | cstr)`, typeclass methods implemented by **all** union members may be
called directly without a `match`. Methods not in the intersection require an explicit
`match` to narrow first:

```turmeric
;; Show is implemented by both int and cstr
(show x)   ;; ok -- in the instance intersection

;; Arithmetic is only on int; requires narrowing
(match x
  (n : int)  (+ n 1)
  (s : cstr) ...)
```

---

## Intersection Types

An intersection type `(A & B)` represents a value that satisfies **both** `A` and `B`.
The primary use is combining a concrete type with typeclass constraints.

### Syntax

```turmeric
;; Named intersection type
(deftype ReadWrite []
  (Readable & Writable))

;; Inline in a function signature
(defn save [x : (int & Serializable)] : unit
  (file/write (serialize x) "output.bin"))
```

### Subtyping

From a value of intersection type you can project either member:

- `(A & B) <: A` and `(A & B) <: B`
- A function expecting `A` accepts a value of type `(A & B)`

### Typeclass Intersection

Intersection is most useful when one side is a typeclass:

```turmeric
(defclass Serializable [a]
  (serialize [x : a] : cstr))

(defn serialize-int [x : (int & Serializable)] : cstr
  (serialize x))
```

The value is an `int` with a `Serializable` dictionary attached. The elaborator
resolves the instance at the intersection type site.

### Unsatisfiable Intersections

Intersections of known-disjoint concrete types are rejected statically (`TUR_E0350`):

```turmeric
;; Compile error: int and cstr are disjoint
(defn bad [x : (int & cstr)] : unit ...)
```

Intersections involving typeclasses or type variables that cannot be determined disjoint
at compile time are permitted and fail during instance resolution.

---

## The `any` Type

`any` is the **top type**: every type is a subtype of `any`. It is available when either
union or intersection flag is active.

```turmeric
(defn debug-print [x : any] : unit
  (println x))

(debug-print 42)      ;; ok
(debug-print "hello") ;; ok
(debug-print true)    ;; ok
```

`any`-typed values are represented as `int64_t` at codegen level. Pointer-sized payloads
(`cstr`, struct, ADT) require a boxing wrapper -- this is deferred (see below).

Union simplification: `(int | cstr | any)` simplifies to `any`.

---

## Gradual Typing

Union types and `any` enable a gradual typing path:

```turmeric
;; Start untyped
(defn debug-print [x : any] : unit
  (println x))

;; Narrow gradually as types become known
(defn typed-print [x : (int | cstr)] : unit
  (debug-print x))
```

---

## ADTs as Union Sugar (Deferred)

`(defdata Option [a] (none | (some a)))` desugaring to a union type is tracked in
the GADT plan (Phase G4). It requires both `-Xgadt` and `-Xunion-types`.

---

## Error Codes

| Code | Message |
|---|---|
| `TUR_E0300` | Union type mismatch: expected `{expected}`, got `{actual}` |
| `TUR_E0301` | Non-exhaustive pattern match on union type `{type}` -- missing arm for `{variant}` |
| `TUR_E0350` | Intersection type unsatisfiable: no value can be both `{A}` and `{B}` |
| `TUR_E0351` | Value of type `{actual}` does not satisfy intersection member `{missing}` |

---

## Known Limitations

### Tagged Union Overhead

TypeScript's union types are zero-cost (erased). Turmeric emits
`struct { int tag; union { A a; B b; } data; }`. Every union-typed value pays
one extra `int` for the tag plus alignment padding to the largest member. This
matters for arrays, struct fields, and cache pressure.

Widening (passing `42` where `(int | cstr)` is expected) requires constructing
the tagged union at the call site -- it is not a free annotation.

### Narrowing is `match`-Only

There is no flow-sensitive narrowing outside `match`. `type-of` checks in `if`
conditions do not narrow the elaborated type:

```turmeric
;; Does not narrow -- elaboration error inside the branch
(if (= (type-of x) "int")
  (+ x 1)   ;; error: (int | cstr) is not int
  ...)

;; Correct: use match
(match x
  (n : int)  (+ n 1)
  (s : cstr) ...)
```

### Intersection is Constraint-Only

TypeScript and Scala 3 merge struct fields across intersections
(`{ x: int } & { y: bool }` gives `{ x: int; y: bool }`). Turmeric statically
rejects intersections of two known-disjoint concrete types. Intersection is only
useful when at least one side is a typeclass.

### Closed Unions

Union types are closed -- the member set is fixed at definition time. A library
returning `(int | ParseError)` cannot be transparently composed with one returning
`(bool | ParseError)` without an explicit adapter.

### Variance with Generics

Variance for type constructors containing union or intersection types is not yet
specified. Passing `(vec (int | cstr))` where `(vec int)` is expected may produce
unexpected behaviour and will be addressed before these features are enabled by
default.

---

## Deferred

The following IT4 items are not yet implemented:

| Item | Notes |
|---|---|
| Boxing codegen | Pointer-sized `any` payloads (cstr, struct, ADT) need a boxing wrapper struct |
| `(cast x : T)` | Runtime downcast from `any`; returns `(option T)` |
| `(type-of x)` | Returns a runtime type tag |
| Tagged union C codegen | `struct { int tag; union { A a; B b; } data; }` emission |
| ADT-as-union sugar | `defdata` desugaring to union (tracked in GADT plan G4) |
| Instance intersection on unions | Deferred failure during instance resolution may be hard to diagnose |

---

## See Also

- [Type Annotations Guide](type-annotations-guide.md) -- compound type syntax: `(-> a b)`, `(vec T)`, `forall`
- [GADTs Guide](gadts-guide.md) -- `defgadt`, type refinement, union types interaction
- [Error Handling Guide](error-handling-guide.md) -- `Result` and `Option` types
- [Contract Types Guide](contract-types-guide.md) -- runtime-checked predicates on types
