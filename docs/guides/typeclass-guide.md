---
title: Typeclass Guide
category: Type System
description: Comprehensive guide to Turmeric's typeclass system -- defining typeclasses with defclass, implementing instances with definstance, constraints, associated types, functional dependencies, and default implementations.
---

# Typeclasses in Turmeric

Typeclasses are Turmeric's mechanism for **ad-hoc polymorphism** -- picking a different implementation of a function based on the type of its arguments. While parametric polymorphism (generics) treats all types identically, typeclasses let you define a single interface (`eq?`, `show`, `fmap`) and provide specialized implementations for each type.

Turmeric's typeclasses are resolved entirely at **compile time** using static dictionary passing. The compiler lowers each instance to a C dictionary struct and a global singleton pointing to the concrete implementations, resulting in zero virtual-dispatch (vtable) or runtime tag-lookup overhead.

---

## At a Glance

- **`defclass`** -- Declares a typeclass name, its type parameters, and its method signatures.
- **`definstance`** -- Implements a typeclass for a specific type or type constructor, optionally requiring constraints.
- **Idempotency** -- Re-running or reloading a `definstance` replaces the existing entry in the dispatch table, making it safe for interactive REPL-based development.
- **Shadowing Warnings (TUR-W0039)** -- If a typeclass method shares a name with an ordinary function (`defn`) in the same scope, the compiler emits a warning. Both coexist, but rename one if the clash is accidental.

---

## Defining Typeclasses with `defclass`

A typeclass declares an interface consisting of one or more method signatures.

```turmeric
(defclass Eq [a]
  (eq? [x y] : bool))
```
```sweet-exp
defclass Eq [a]
  eq? [x y] :bool
```

For classes with multiple methods, declare them sequentially within the `defclass` body:

```turmeric
(defclass Ord [a]
  (lt? [x y] : bool)
  (gt? [x y] : bool))
```
```sweet-exp
defclass Ord [a]
  lt? [x y] :bool
  gt? [x y] :bool
```

Typeclasses can also abstract over **type constructors** (such as `Vec`, `Option`, or user-defined containers) instead of plain types. These are called Higher-Kinded Typeclasses. The compiler uses specific annotations to distinguish their kinds:
- `[^f]` -- a unary type constructor of kind `* -> *` (e.g., `Functor`, `Monad`).
- `[^^f]` -- a binary type constructor of kind `* -> * -> *` (e.g., `Bifunctor`).

```turmeric
(defclass Functor [^f]
  (fmap [container g] : int))
```
```sweet-exp
defclass Functor [^f]
  fmap [container g] :int
```

---

## Implementing Instances with `definstance`

A `definstance` provides concrete implementations for the declared methods of a typeclass.

### Monomorphic Instances

For basic types, declare the instance directly:

```turmeric
(definstance Eq [int]
  (eq? [x y] (= x y)))
```
```sweet-exp
definstance Eq [int]
  eq? [x y] =(x y)
```

You can also use inline C blocks within instance methods to write high-performance native implementations:

```turmeric
(definstance Eq [cstr]
  (eq? [x y]
    : bool
    ```c
    if (x == NULL && y == NULL) return true;
    if (x == NULL || y == NULL) return false;
    return strcmp((const char *)x, (const char *)y) == 0;
    ```))
```
```sweet-exp
definstance Eq [cstr]
  eq? [x y] :bool
    ```c
    if (x == NULL && y == NULL) return true;
    if (x == NULL || y == NULL) return false;
    return strcmp((const char *)x, (const char *)y) == 0;
    ```
```

---

## Parametric Instances and Constraints

Often, a container type can implement a typeclass only if its elements also implement that typeclass. You can specify these dependencies using **constraints** placed in a second list inside `definstance`.

For example, two `Option` values are equal if their wrapped values are equal:

```turmeric
(definstance Eq [Option]
  [(Eq A)]
  (eq? [x y]
    (if (= (.is-some x) (.is-some y))
      (if (.is-some x) (eq? (.value x) (.value y)) true)
      false)))
```
```sweet-exp
definstance Eq [Option]
  [(Eq A)]
  eq? [x y]
    if (= (.is-some x) (.is-some y))
      if (.is-some x) (eq? (.value x) (.value y)) true
      false
```

Multiple constraints can be specified within the brackets:

```turmeric no-check
(definstance Eq [Result]
  [(Eq A) (Eq B)]
  (eq? [x y]
    (if (= (.is-ok x) (.is-ok y))
      (if (.is-ok x)
        (eq? (.ok-val x) (.ok-val y))
        (eq? (.err-val x) (.err-val y)))
      false)))
```
```sweet-exp
definstance Eq [Result]
  [(Eq A) (Eq B)]
  eq? [x y]
    if (= (.is-ok x) (.is-ok y))
      if (.is-ok x)
        eq?(.ok-val x) (.ok-val y)
        eq?(.err-val x) (.err-val y)
      false
```

### Trailing-Parameter Instance Heads (Holes)

Some container types take multiple parameters, but you may want to implement a unary typeclass like `Functor` over only one of them. Turmeric supports **holes (`_`)** to fix some parameters while leaving others free.

For instance, `Result` is a binary constructor with two parameters: `[A B]` representing the OK type and Error type. To map a function only over the OK payload (the right-biased convention), we can fix the Error type `B` and leave the OK type free:

```turmeric no-check
(definstance Functor [(Result _ B)]
  (fmap [container g]
    (if (.is-ok container)
      (ok (g (.ok-val container)))
      (err (.err-val container)))))
```
```sweet-exp
definstance Functor [(Result _ B)]
  fmap [container g]
    if (.is-ok container)
      ok(g(.ok-val container))
      err(.err-val container)
```

---

## Constrained Functions

A `defn` asks for an instance with a caret constraint in its parameter
vector: `^Class` names the class, the next symbol declares the type
variable it constrains, and the parameters that follow use that variable.

```turmeric
(defn display [^Show a x : a] : void
  (println (show x)))

(defn same? [^Eq a x : a y : a] : bool
  (eq? x y))
```
```sweet-exp
defn display [^Show a x : a] : void
  println(show(x))
```

Bare parameters that directly follow the binder take its type, so the
annotation may be left off: `[^Show a x]` reads as `[^Show a x : a]`, and
`[^Eq a x y]` as `[^Eq a x : a y : a]`. The run ends at the first parameter
that carries its own annotation, which always wins -- `[^Show a x n : int]`
is `x : a` and `n : int`. Several constraints on one variable are written in
sequence (`[^Eq ^Show a x]`); a constructor variable (`[^f] [^Functor f
xs : (f int)]`) is never a value parameter's type and does not start a run.

Each call instantiates the variable at the argument's type and dispatches
the class methods on that type: `(display (Red))` reaches `Show [Color]`
and `(display 3)` reaches `Show [int]`, with a by-value `defdata` or
`defstruct` argument passed as the aggregate it is.

---

## Associated Types

An **Associated Type** allows a typeclass to declare a placeholder type member using `(type Name : Type)`. Each concrete instance binds this member to a specific type with `(type Name = <type>)`.

This is incredibly useful for container/collection typeclasses where the container itself determines the element type.

### Declaring and Binds

In a `defclass` body, declare the associated type with `: Type`:

```turmeric no-check
(defclass Container [t]
  (type Elem : Type)
  (make-empty [self : t] : int))
```
```sweet-exp
defclass Container [t]
  type Elem :Type
  make-empty [self : t] :int
```

In a `definstance` body, bind it to a concrete type:

```turmeric no-check
(definstance Container [(Vec int)]
  (type Elem = int)
  (make-empty [self] 0))

(definstance Container [(Vec cstr)]
  (type Elem = cstr)
  (make-empty [self] 0))
```
```sweet-exp
definstance Container [(Vec int)]
  type Elem = int
  make-empty [self] 0

definstance Container [(Vec cstr)]
  type Elem = cstr
  make-empty [self] 0
```

### Type-Level Projections

You can project associated types in type annotations using the syntax `(AssociatedTypeName TypeArgument)`. At compile time, the projection resolves to the precise type bound by the matching instance:

```turmeric no-check
;; (Elem (Vec int)) reduces to int; (Elem (Vec cstr)) reduces to cstr.
(defn take-elem-int [x : (Elem (Vec int))] : int
  (+ x 1))

(defn take-elem-cstr [x : (Elem (Vec cstr))] : cstr
  x)
```
```sweet-exp
;; (Elem (Vec int)) reduces to int; (Elem (Vec cstr)) reduces to cstr.
defn take-elem-int [x : (Elem (Vec int))] : int
  (+ x 1)

defn take-elem-cstr [x : (Elem (Vec cstr))] : cstr
  x
```

If a class takes multiple type parameters, project across all of them:

```turmeric no-check
(defn bump-acc-int [x : (Acc (Vec int) int)] : int
  (+ x 1))
```
```sweet-exp
defn bump-acc-int [x : (Acc (Vec int) int)] : int
  (+ x 1)
```

### Method Signature Propagation

When a typeclass method uses an associated type in its signature (either as a parameter or return type), the concrete instance methods **inherit those types under the instance substitution**. This means you do not have to repeat the annotations in the `definstance` methods:

```turmeric no-check
(defclass StorageOps [S]
  (type Elem : Type)
  (sop-get [^borrow s : S idx : int] : Elem))

;; The compiler automatically knows that `sop-get` returns `Pos`
(definstance StorageOps [(Dense Pos)]
  (type Elem = Pos)
  (sop-get [s idx] (dense-get s idx)))
```
```sweet-exp
defclass StorageOps [S]
  type Elem :Type
  sop-get [^borrow s : S idx : int] :Elem

;; The compiler automatically knows that `sop-get` returns `Pos`
definstance StorageOps [(Dense Pos)]
  type Elem = Pos
  sop-get [s idx] (dense-get s idx)
```

---

## Functional Dependencies

For multi-parameter typeclasses, you can declare **Functional Dependencies** (fundeps) using the `| (a -> b)` syntax. This declares that knowing the type parameter `a` uniquely determines the type parameter `b`.

This helps the compiler infer type variables during method resolution and enables **Return-Only Dispatch** -- resolving a dispatch type parameter even if it only appears in the return type.

```turmeric no-check
;; s (storage backend) uniquely determines e (element type)
(defclass StorageOps [s e] | (s -> e)
  (sop-get [^borrow self : s idx : int] : e))
```
```sweet-exp
;; s (storage backend) uniquely determines e (element type)
defclass StorageOps [s e] | (s -> e)
  sop-get [^borrow self : s idx : int] :e
```

When implementing instances of a class with functional dependencies, the compiler enforces **coherence and uniqueness**:

```turmeric no-check
(defopaque Box [A] : int)

(defclass Collect [c e] | (c -> e)
  (cinsert [self : c x : e] : c))

;; This is valid
(definstance Collect [(Box int) int]
  (cinsert [self x] self))

;; ERROR: This violates the functional dependency!
;; (Box int) has already been pinned to determine `int`, not `cstr`.
(definstance Collect [(Box int) cstr]
  (cinsert [self x] self))
```
```sweet-exp
defopaque Box [A] : int

defclass Collect [c e] | (c -> e)
  cinsert [self : c x : e] : c

;; This is valid
definstance Collect [(Box int) int]
  cinsert [self x] self

;; ERROR: This violates the functional dependency!
;; (Box int) has already been pinned to determine `int`, not `cstr`.
definstance Collect [(Box int) cstr]
  cinsert [self x] self
```

---

## Default Method Implementations

Typeclasses can define default implementations for their methods. If a `definstance` does not provide an explicit implementation for a method, it falls back to the default body declared in the `defclass`.

To declare a default body, simply provide forms after the return type in the `defclass` method signature:

```turmeric
(defclass Ord [a]
  (lt? [x y] : bool)
  (lte? [x y] : bool (or (lt? x y) (= x y))))
```
```sweet-exp
defclass Ord [a]
  lt? [x y] :bool
  lte? [x y] :bool
    or (lt? x y) (= x y)
```

When implementing the instance, we can choose to implement `lt?` and completely omit `lte?`, inheriting the default logic:

```turmeric
(definstance Ord [int]
  (lt? [x y] (< x y)))
```
```sweet-exp
definstance Ord [int]
  lt? [x y] <(x y)
```

---

## Under the Hood: Low-Level Dictionary Passing

For those curious about how this lowers, every `definstance Class [TypeArgs]` undergoes several compilation phases (managed in `src/compiler/elab_typeclasses.c`):

1. **Dictionary Struct lowering**: The compiler generates a C structure type `dict_<Class>_<TypeArgs>` representing the class dictionary. The struct fields are function pointers mirroring each method's return type and parameter C types.
2. **Global Singleton emission**: A global singleton `dict_<Class>_<TypeArgs>_singleton` is initialized. Its fields point to the local instance-level implementations (`__inst_<Class>_<Method>_<TypeArgs>`).
3. **Dispatch Resolution**: When calling a typeclass method, the compiler locates the appropriate dictionary singleton at compile time and emits a standard direct/indirect function pointer call (`EX_DICT`).

For a deep dive into return overriding, the closure-handle convention, and C types resolution, please see [Typeclass Dictionary Internals](typeclass-internals-guide.md).
