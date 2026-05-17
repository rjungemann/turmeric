# GADTs in Turmeric -- A Practical Guide

Generalized Algebraic Data Types (GADTs) extend ordinary sum types by letting
each constructor specialize the type parameters it returns. The type-checker
learns those specializations when pattern-matching, so programs that previously
needed runtime tags or unsafe casts become statically verified.

This guide walks from plain sum types up to GADTs and equality witnesses. All
examples are valid Turmeric source. Activate GADT support by passing `-Xgadt`
to the compiler.

---

## 1. Plain ADTs First (`defdata` + `match`)

Before reaching for GADTs, get comfortable with plain sum types. Turmeric's
`defdata` form declares a sum type; `match` dispatches on its constructors.

```
(defdata Color :copy (Red) (Green) (Blue))

(defn color-to-int [c] :int
  (match c
    (Red)   1
    (Green) 2
    (Blue)  3))
```

Parameterized ADTs work the same way:

```
(defdata Option [a]
  (None)
  (Some a))

(defn option-or [opt default] :int
  (match opt
    (None)    default
    (Some x)  x))
```

The `:copy` modifier makes the type copyable (like `defstruct :copy`). Without
it the value follows move semantics.

Constructors can carry typed fields:

```
(defdata Shape
  (Circle int)
  (Rect   int int))

(defn area [s] :int
  (match s
    (Circle r)   (* r r)
    (Rect w h)   (* w h)))
```

Pattern matching is exhaustiveness-checked. If you omit a constructor the
compiler reports the missing arm.

---

## 2. Your First GADT (`defgadt`)

A GADT is declared with `defgadt`. The key difference from `defdata` is that
each constructor carries an explicit `: return-type` annotation:

```
; Requires: -Xgadt
(defgadt Expr [a]
  (Lit int         : (Expr int))
  (Add (Expr int) (Expr int) : (Expr int)))
```

Each constructor line reads as: "field types ... `: return type`". Here:

- `(Lit int : (Expr int))` -- `Lit` takes one `int` field and returns `(Expr int)`.
- `(Add (Expr int) (Expr int) : (Expr int))` -- `Add` takes two `(Expr int)`
  fields and also returns `(Expr int)`.

Without `-Xgadt` the compiler rejects `defgadt` entirely:

```
error: unknown form 'defgadt' (pass -Xgadt to enable)
```

Evaluation is written the same as for a plain ADT:

```
(defn eval-expr [e] :int
  (match e
    (Lit n)   n
    (Add l r) (+ (eval-expr l) (eval-expr r))))
```

---

## 3. Type Refinement in Match Arms

The power of GADTs shows up when constructors specialize different type
parameters. Consider a typed tag:

```
(defgadt Tag [a]
  (IntTag  : (Tag int))
  (BoolTag : (Tag bool)))
```

A function dispatching on the tag can return `a` without a cast:

```
(defn default-value [t] :int
  (match t
    (IntTag)  0
    (BoolTag) 0))
```

In the `IntTag` arm the type-checker knows `a = int`; in `BoolTag` it knows
`a = bool`. These *skolem equalities* are introduced automatically per arm and
are invisible to the programmer. No casts, no `option`, no runtime tags.

A larger example combining plain ADTs and GADTs in one program:

```
(defdata Color :copy (Red) (Green) (Blue))

(defgadt Expr [a]
  (Lit int         : (Expr int))
  (Add (Expr int) (Expr int) : (Expr int)))

(defn color-to-int [c] :int
  (match c
    (Red)   1
    (Green) 2
    (Blue)  3))

(defn eval-expr [e] :int
  (match e
    (Lit n)   n
    (Add l r) (+ (eval-expr l) (eval-expr r))))

(defn main [] :int
  (println (color-to-int (Red)))            ; 1
  (println (eval-expr (Add (Lit 10) (Lit 20))))  ; 30
  (println (color-to-int (Green)))          ; 2
  0)
```

You can also use multiple constructors that refine the same type variable in
different ways:

```
(defgadt Expr [a]
  (Lit int                         : (Expr int))
  (Add (Expr int) (Expr int)       : (Expr int))
  (Mul (Expr int) (Expr int)       : (Expr int)))

(defn eval-expr [e] :int
  (match e
    (Lit n)   n
    (Add l r) (+ (eval-expr l) (eval-expr r))
    (Mul l r) (* (eval-expr l) (eval-expr r))))

(defn main [] :int
  ; (2 + (3 * 4)) = 14
  (let [e (Add (Lit 2) (Mul (Lit 3) (Lit 4)))]
    (println (eval-expr e))
    0))
```

---

## 4. The `Equal` GADT and `coerce`

The standard library provides a built-in equality witness GADT:

```
; Built into the runtime -- you do not need to declare this yourself.
; (defgadt Equal [a b]
;   (Refl : (Equal a a)))
```

`(Refl)` is the only constructor. Because its return type is `(Equal a a)`,
constructing `(Refl)` proves that the two type parameters are the same type.

Use `coerce` to convert a value across a proven equality:

```
(defgadt Equal [a b]
  (Refl : (Equal a a)))

(defn main [] :int
  (match (Refl)
    (Refl)
      (do
        (println (coerce (Refl) 42))
        0)))
```

In the `(Refl)` arm the type-checker knows `a = b`, so `coerce` can safely
reinterpret the value without any runtime overhead.

Symmetry -- turning `(Equal a b)` into `(Equal b a)` -- follows from matching
on `Refl` and returning `Refl`:

```
(defn sym [eq] :(Equal b a)
  (match eq
    (Refl) (Refl)))
```

---

## 5. Guard Clauses (`when`) in Match

Guard clauses let you refine which arm fires based on a runtime predicate.
They work in both plain ADT and GADT matches:

```
(defdata Sign (Pos int) (Neg int) (Zero))

(defn classify [s] :int
  (match s
    (Pos n) when (> n 100) (do (println "big") 0)
    (Pos n)                (do (println "pos") 0)
    (Neg n)                (do (println "neg") 0)
    (Zero)                 (do (println "zero") 0)))
```

Arms are tried top to bottom. The first arm whose pattern matches *and* whose
guard is true fires. If no guard succeeds for a matched pattern the next arm
is tried.

---

## 6. Common Errors and How to Fix Them

| Mistake | Error message (excerpt) | Fix |
|---|---|---|
| Using `defgadt` without `-Xgadt` | `unknown form 'defgadt' (pass -Xgadt to enable)` | Add `-Xgadt` to the build command |
| Omitting the `: return-type` on a refining constructor | `constructor refines type variable -- explicit return type required` | Add `: (MyGadt ...)` after the field list |
| Missing a constructor in a match | `non-exhaustive match: missing constructor 'Foo'` | Add the missing arm or a wildcard `_` |
| Type mismatch from a skolem | `cannot unify 'int' with 'bool' (skolem from 'BoolTag')` | Check that the correct GADT arm is used for the operation |
| Skolem type escaping its scope | `skolem type variable 'a' escapes its match scope` | Use a universally polymorphic return type instead of the refined one |
| Unannotated GADT in a container | `ambiguous GADT type parameter -- annotation required` | Add an explicit type annotation, e.g. `: (vec (MyGadt int))` |

### Enabling the flag

Every file that uses `defgadt` or matches on a GADT must be compiled with
`-Xgadt`:

```
just build          # plain ADTs only (no flag needed)
./build/tur run -Xgadt my-file.tur
./build/tur build -Xgadt my-file.tur
```

---

## 7. Union Types and Gradual Typing (`-Xunion-types`)

Turmeric also supports structural union types as a complement to GADTs.
Enable them with `-Xunion-types`. Union types and GADTs are independent
features that can be used together.

### Declaring a union parameter

```
(defn describe [x : (int | bool)] :int
  (match x
    (n : int)  (do (println "int")  0)
    (b : bool) (do (println "bool") 0)))
```

The `(match x (n : int) body1 (b : bool) body2)` form dispatches on the
runtime tag. Pattern arms must be exhaustive across all union members.

### Typeclass dispatch on union values

When every member of a union has an instance for a typeclass method, you can
call `.method` directly without an explicit `match`. The compiler generates
the tag-dispatched call automatically:

```
(defclass Show [a]
  (show [x] :cstr))

(definstance Show [int]
  (show [x] "an-int"))

(definstance Show [bool]
  (show [x] "a-bool"))

(defn print-any [x : (int | bool)] :int
  (println (.show x))
  0)
```

If any union member lacks an instance the compiler emits an error at the
call site naming the missing member.

### The `any` top type and gradual typing

`any` is a top type -- every concrete type is a subtype of `any`. Values
boxed into `any` carry a runtime tag so their type can be recovered:

```
(defn consume [x : any] :int
  (println (type-of x))   ; prints "int", "bool", "cstr", etc.
  0)

(defn main [] :int
  (consume 42)      ; prints "int"
  (consume true)    ; prints "bool"
  (consume "hello") ; prints "cstr"
  0)
```

Use `(type-of x)` to retrieve the type name as a `cstr`. Use `(cast x T)`
to unsafely unbox an `any` value as type `T` (no runtime tag check -- use
only when you know the type):

```
(defn print-as-int [x : any] :int
  (println (cast x int))
  0)
```

---

## 8. Current Limitations

These features are not yet supported:

- **No dependent types.** Type parameters must be types, not values. You cannot
  index a GADT by a runtime integer directly; use a type-level Nat GADT instead.

- **No nested patterns in GADT arms.** You cannot write:

  ```
  (match e
    (Add (Lit 0) r) r   ; nested pattern -- not yet supported
    ...)
  ```

  Flatten with a let binding instead:

  ```
  (match e
    (Add l r)
      (match l
        (Lit 0) r
        _       (Add l r))
    ...)
  ```

- **No mutual recursion across files.** Mutually recursive GADTs must be
  defined in the same file.

- **The `(~ a b)` constraint syntax is not yet supported.** The form
  `(defn f [(~ a int) x :a] :int ...)` is parsed as a type annotation without
  a preceding parameter and is rejected. Use the `Equal` witness pattern instead.

- **Polymorphic recursion is not fully inferred.** If a GADT function is
  polymorphically recursive, add an explicit type annotation on the `defn`.

- **`equal-cong` requires HKT.** The congruence lemma
  `(defn equal-cong [^f eq : (Equal a b)] : (Equal (f a) (f b)) ...)` needs
  kind-`* -> *` type variables in `defn` signatures. This is deferred until
  the HKT kind system is available. See `docs/hkt-deferred-tasks.md`.

- **`cast` is unchecked.** `(cast x T)` does not verify the runtime tag
  matches `T`. Use `(type-of x)` first if you need a safe downcast.

- **Implicit union widening is not yet implemented.** A value of type `A`
  cannot yet be passed where `(A | B)` is expected without an explicit
  `EX_UNION_INJECT` coercion. This is tracked as IT1 in
  `docs/gadts-followup-tasks.md`.

---

## Quick Reference

```
; Declare a plain ADT
(defdata Color :copy (Red) (Green) (Blue))

; Declare a GADT  (requires -Xgadt)
(defgadt Expr [a]
  (Lit int                         : (Expr int))
  (Add (Expr int) (Expr int)       : (Expr int))
  (Mul (Expr int) (Expr int)       : (Expr int)))

; Pattern match -- exhaustiveness checked, guards optional
(defn eval-expr [e] :int
  (match e
    (Lit n)   n
    (Add l r) (+ (eval-expr l) (eval-expr r))
    (Mul l r) (* (eval-expr l) (eval-expr r))))

; Equality witness
(defgadt Equal [a b]
  (Refl : (Equal a a)))

; coerce a value using an equality proof
(coerce (Refl) some-value)

; Union type dispatch  (requires -Xunion-types)
(defn describe [x : (int | bool)] :int
  (match x
    (n : int)  0
    (b : bool) 1))

; Gradual typing  (requires -Xunion-types)
(defn show-type [x : any] :int
  (println (type-of x))
  0)
```

## See also

- `docs/guides/hrt-guide.md` -- Higher-ranked types; bidirectional checking
  that enables GADT skolem propagation
- `docs/guides/hkt-guide.md` -- Higher-kinded types; required for `equal-cong`
  and polymorphic GADT indices
- `docs/gadts-plan.md` -- Full implementation plan and phase history
- `docs/gadts-followup-tasks.md` -- Open items and blocked tasks
- `tests/fixtures/gadt-*/` -- Working GADT examples
- `tests/fixtures/union-types-*/` -- Union type and gradual typing examples
