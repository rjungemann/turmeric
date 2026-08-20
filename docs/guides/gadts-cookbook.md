---
title: GADTs Cookbook
category: Advanced Types
description: GADTs cookbook: practical patterns and recipes
---

# GADTs Cookbook

Practical patterns for using Generalized Algebraic Data Types (GADTs) in
Turmeric. GADTs, union types, and every other type-system feature shown in
this cookbook are enabled by default; no `-X` flags are required.

---

## Typed AST Interpreter

The classic GADT motivation: an expression tree whose type parameter tracks
the result type, so the interpreter is total and type-safe.

```turmeric
; A two-type expression language: integers and booleans.
(defgadt Expr [a]
  (IntLit  int                  : (Expr int))
  (BoolLit bool                 : (Expr bool))
  (Add     (Expr int) (Expr int) : (Expr int))
  (IsZero  (Expr int)            : (Expr bool)))

; The interpreter returns int -- the type parameter 'a' is refined per arm.
(defn eval-expr [e] : int
  (match e
    (IntLit  n)   n
    (BoolLit b)   (if b 1 0)
    (Add     l r) (+ (eval-expr l) (eval-expr r))
    (IsZero  n)   (if (= (eval-expr n) 0) 1 0)))

(defn main [] : int
  ; (2 + (IsZero 0)) -- only valid at type (Expr ???) mismatch, caught statically
  (println (eval-expr (IntLit 42)))                          ; 42
  (println (eval-expr (Add (IntLit 3) (IntLit 4))))         ; 7
  (println (eval-expr (IsZero (IntLit 0))))                  ; 1
  (println (eval-expr (IsZero (Add (IntLit 1) (IntLit 0))))) ; 0
  0)
```

```sweet-exp
; A two-type expression language: integers and booleans.
defgadt Expr [a]
  (IntLit  int                  : (Expr int))
  (BoolLit bool                 : (Expr bool))
  (Add     (Expr int) (Expr int) : (Expr int))
  (IsZero  (Expr int)            : (Expr bool))

; The interpreter returns int -- the type parameter 'a' is refined per arm.
defn eval-expr [e] :int
  match e
    (IntLit  n)
    n
    (BoolLit b)
    if(b 1 0)
    (Add     l r)
    +(eval-expr(l) eval-expr(r))
    (IsZero  n)
    if(=(eval-expr(n) 0) 1 0)

defn main [] :int
  ; (2 + (IsZero 0)) -- only valid at type (Expr ???) mismatch, caught statically
  println(eval-expr((IntLit 42)))
  println(eval-expr(Add((IntLit 3) (IntLit 4))))
  println(eval-expr(IsZero((IntLit 0))))
  println(eval-expr(IsZero(Add((IntLit 1) (IntLit 0)))))
  0
```

**What makes this a GADT:** `Add` returns `(Expr int)` and `IsZero` returns
`(Expr bool)`. In the `IsZero` arm the elaborator knows `a = bool`, so the
return type is refined without a cast. In the `IntLit` arm it knows `a = int`.

**Without GADTs** you would need a runtime `option` or `either` to represent
the two result types, plus an unsafe cast or pattern match to retrieve the
value.

---

## Length-Indexed Vectors

Use a type-level natural number to track vector length at compile time. Safe
`head` and `vzip-with` become expressible without `option`.

> **Caveat -- the index is phantom today.** The declarations below sketch the
> full length-indexed design. In current Turmeric the `n` parameter is a
> *phantom*: it does not yet prove non-emptiness or equal lengths at compile
> time (true compile-time length safety awaits a dependent-types phase), which
> is why the stdlib module ships `gvec-head-or` with an explicit default
> rather than a bare `head`.

```turmeric
; Type-level naturals (canonical constructor syntax: fields, then ': return-type')
(defgadt Nat []
  (Zero : (Nat))
  (Succ (Nat) : (Nat)))

; Length-indexed int vector: (Vec n) has exactly n elements.
; (Uses int elements; see stdlib/gadt-vec.tur for the shipped module.)
(defgadt Vec [n]
  (VNil            : (Vec Zero))
  (VCons int (Vec n) : (Vec (Succ n))))

; Length is computable at compile time -- no bounds check needed.
(defn vec-len [v] : int
  (match v
    (VNil)       0
    (VCons _ tl) (+ 1 (vec-len tl))))

; Head is only callable on non-empty vectors.
; The return type is int, not (option int).
(defn vec-head [v] : int
  (match v
    (VCons x _) x))

; Only vectors of the same length can be zipped -- the type guarantees it.
(defn vzip-add [xs ys] : int
  (match xs
    (VNil)        0
    (VCons x xtl)
      (match ys
        (VCons y ytl) (+ (* x y) (vzip-add xtl ytl)))))

(defn main [] : int
  (let [v (VCons 1 (VCons 2 (VCons 3 (VNil))))]
    (println (vec-len v))      ; 3
    (println (vec-head v))     ; 1
    (println (vzip-add v v)))  ; 1*1 + 2*2 + 3*3 = 14
  0)
```

```sweet-exp
; Type-level naturals (canonical constructor syntax: fields, then ': return-type')
defgadt Nat []
  (Zero : (Nat))
  (Succ (Nat) : (Nat))

; Length-indexed int vector: (Vec n) has exactly n elements.
; (Uses int elements; see stdlib/gadt-vec.tur for the shipped module.)
defgadt Vec [n]
  (VNil            : (Vec Zero))
  (VCons int (Vec n) : (Vec (Succ n)))

; Length is computable at compile time -- no bounds check needed.
defn vec-len [v] :int
  match v
    (VNil)
    0
    (VCons _ tl)
    +(1 vec-len(tl))

; Head is only callable on non-empty vectors.
; The return type is int, not (option int).
defn vec-head [v] :int
  match v
    (VCons x _)
    x

; Only vectors of the same length can be zipped -- the type guarantees it.
defn vzip-add [xs ys] :int
  match xs
    (VNil)
    0
    (VCons x xtl)
    match ys
      (VCons y ytl)
      +(*(x y) vzip-add(xtl ytl))

defn main [] :int
  let [v VCons(1 VCons(2 VCons(3 (VNil))))]
    println(vec-len(v))
    println(vec-head(v))
    println(vzip-add(v v))
  0
```

**Stdlib:** `stdlib/gadt-vec.tur` provides `gvec-nil`, `gvec-cons`,
`gvec-len`, `gvec-sum`, `gvec-head-or`, `gvec-tail`, `gvmap`, and
`gvzip-with` as a reusable module (a phantom-typed `GVec` of int elements;
true compile-time length safety awaits a dependent-types phase). Import it
with `(load "stdlib/gadt-vec.tur")`.

---

## Type-Safe Printf Format Strings

Encode the expected argument list in the type so the wrong number or type of
arguments is a compile-time error.

```turmeric
; A format descriptor -- 'args' is a phantom type tracking argument types.
; This is a simplified version: real type-level lists need HKT.
; Here we show the two-argument case explicitly.
(defgadt Fmt2 [a b]
  ; format string expecting (int, int)
  (FIntInt  : (Fmt2 int int))
  ; format string expecting (int, bool)
  (FIntBool : (Fmt2 int bool)))

; A printf that is safe for exactly the arguments the format specifies.
(defn sprintf2 [fmt a b] : int
  (match fmt
    (FIntInt)   (do (println a) (println b) 0)
    (FIntBool)  (do (println a) (if b (println 1) (println 0)) 0)))

(defn main [] : int
  ; Both arguments are correct -- compiles fine.
  (sprintf2 (FIntInt)  42  99)
  (sprintf2 (FIntBool) 10  true)
  0)
```

```sweet-exp
; A format descriptor -- 'args' is a phantom type tracking argument types.
; This is a simplified version: real type-level lists need HKT.
; Here we show the two-argument case explicitly.
defgadt Fmt2 [a b]
  ; format string expecting (int, int)
  (FIntInt  : (Fmt2 int int))
  ; format string expecting (int, bool)
  (FIntBool : (Fmt2 int bool))

; A printf that is safe for exactly the arguments the format specifies.
defn sprintf2 [fmt a b] :int
  match fmt
    (FIntInt)
    do
      println(a)
      println(b)
      0
    (FIntBool)
    do
      println(a)
      if(b println(1) println(0))
      0

defn main [] :int
  ; Both arguments are correct -- compiles fine.
  sprintf2((FIntInt)  42  99)
  sprintf2((FIntBool) 10  true)
  0
```

**Practical note:** A full type-safe printf encoding the full argument list
`(cons int (cons bool nil))` requires type-level lists, which Turmeric does
not provide. The pattern above shows the core idea with fixed arity.

---

## Equality Witnesses

The built-in `Equal` GADT lets you carry proofs of type equality across
function boundaries. Use `coerce` to convert values across proven equalities.

```turmeric
; Equal is built-in; you can also declare it yourself:
; (defgadt Equal [a b]
;   (Refl : (Equal a a)))

; Symmetry: (Equal a b) -> (Equal b a)
; In the Refl arm a = b, so Refl also has type (Equal b a).
(defn sym [eq] :(Equal b a)
  (match eq
    (Refl) (Refl)))

; Transitivity: (Equal a b) -> (Equal b c) -> (Equal a c)
; Both Refl arms unify a=b and b=c, giving a=c.
(defn trans [ab bc] :(Equal a c)
  (match ab
    (Refl)
      (match bc
        (Refl) (Refl))))

; coerce uses an equality proof to safely reinterpret a value.
; No cast instruction is emitted -- it is zero-overhead.
(defn use-eq [eq x] : int
  (match eq
    (Refl)
      ; In this arm a = int (from the Refl refinement)
      ; so x : a is the same as x : int
      (+ x 1)))

(defn main [] : int
  ; Construct a proof that int = int
  (let [proof (Refl)]
    (println (use-eq proof 41))   ; 42
    (println (coerce proof 100))) ; 100 -- zero overhead
  0)
```

```sweet-exp
; Equal is built-in; you can also declare it yourself:
; defgadt Equal [a b]
;   (Refl : (Equal a a))

; Symmetry: (Equal a b) -> (Equal b a)
; In the Refl arm a = b, so Refl also has type (Equal b a).
defn sym [eq] :(Equal b a)
  match eq
    (Refl)
    (Refl)

; Transitivity: (Equal a b) -> (Equal b c) -> (Equal a c)
; Both Refl arms unify a=b and b=c, giving a=c.
defn trans [ab bc] :(Equal a c)
  match ab
    (Refl)
    match bc
      (Refl)
      (Refl)

; coerce uses an equality proof to safely reinterpret a value.
; No cast instruction is emitted -- it is zero-overhead.
defn use-eq [eq x] :int
  match eq
    (Refl)
    ; In this arm a = int (from the Refl refinement)
    ; so x : a is the same as x : int
    +(x 1)

defn main [] :int
  ; Construct a proof that int = int
  let [proof (Refl)]
    println(use-eq(proof 41))
    println(coerce(proof 100))
  0
```

### Using `Equal` to implement a typed heterogeneous container

```turmeric
; A box that remembers the type of its contents via a witness.
(defgadt TypeBox [a]
  (MkBox : int -> (TypeBox int))
  (MkBoolBox : bool -> (TypeBox bool)))

; Open a TypeBox knowing it holds an int.
(defn open-int-box [box eq] : int
  (match box
    (MkBox n)
      (match eq
        ; In this arm a = int, so coerce is safe
        (Refl) (coerce eq n))
    (MkBoolBox _) -1))

(defn main [] : int
  (let [b (MkBox 99)]
    (println (open-int-box b (Refl))))  ; 99
  0)
```

```sweet-exp
; A box that remembers the type of its contents via a witness.
defgadt TypeBox [a]
  (MkBox : int -> (TypeBox int))
  (MkBoolBox : bool -> (TypeBox bool))

; Open a TypeBox knowing it holds an int.
defn open-int-box [box eq] :int
  match box
    (MkBox n)
    match eq
      ; In this arm a = int, so coerce is safe
      (Refl)
      coerce(eq n)
    (MkBoolBox _)
    -1

defn main [] :int
  let [b MkBox(99)]
    println(open-int-box(b (Refl)))
  0
```

---

## GADT + Union Types Together

GADTs and union types can be combined. A function can accept either a GADT
value or a plain value through a union type:

```turmeric
; A typed tag for int-or-bool dispatch
(defgadt Tag [a]
  (IntTag  : (Tag int))
  (BoolTag : (Tag bool)))

; default-value uses the tag to produce a zero value.
(defn default-value [t] : int
  (match t
    (IntTag)  0
    (BoolTag) 0))

; A function accepting either an Int tag or a plain int through a union.
(defn accept-either [x : ((Tag int) | int)] : int
  (match x
    (v : (Tag int)) (default-value v)
    (n : int)       n))

(defn main [] : int
  (println (accept-either (IntTag)))   ; 0
  (println (accept-either 42))          ; 42
  0)
```

```sweet-exp
; A typed tag for int-or-bool dispatch
defgadt Tag [a]
  (IntTag  : (Tag int))
  (BoolTag : (Tag bool))

; default-value uses the tag to produce a zero value.
defn default-value [t] :int
  match t
    (IntTag)
    0
    (BoolTag)
    0

; A function accepting either an Int tag or a plain int through a union.
defn accept-either [x : ((Tag int) | int)] :int
  match x
    (v : (Tag int))
    default-value(v)
    (n : int)
    n

defn main [] :int
  println(accept-either((IntTag)))
  println(accept-either(42))
  0
```

---

## See also

- [gadts-guide.md](gadts-guide.md) -- Full language reference for GADTs and union types
- [tur/gadt-vec API](../api/tur-gadt-vec.html) -- Production-quality length-indexed vector stdlib
- [tur/equal API](../api/tur-equal.html) -- `Equal` witness, `sym`, `trans`, `subst`
- [`tests/fixtures/gadt-*/`](https://github.com/rjungemann/turmeric/tree/main/tests/fixtures/) -- All GADT test fixtures
- [`tests/fixtures/union-types-*/`](https://github.com/rjungemann/turmeric/tree/main/tests/fixtures/) -- Union type and gradual typing fixtures
