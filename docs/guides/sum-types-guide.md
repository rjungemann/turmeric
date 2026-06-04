---
title: Sum Types Guide -- defdata, Either, and Pattern Matching
category: Guide
description: When to reach for a sum type instead of a struct, how to declare one with defdata, how Either models "error or value", how match destructures arms (and checks exhaustiveness), and the FFI layout for inline-C interop.
---

# Sum Types Guide

Turmeric has first-class **algebraic sum types** via `defdata`. A sum type is a
value that is *exactly one of* a fixed set of named alternatives
(constructors), each carrying its own typed payload. `stdlib/either.tur` builds
the canonical binary sum, `Either L R`, on this machinery.

## Sum vs. struct: which do I reach for?

| Situation | Reach for |
|---|---|
| A record of fields that all coexist | `defstruct` |
| A value that is *one of* several shapes | `defdata` (sum) |
| "Either an error or a value" | `Either L R` (this guide) |
| Optional value, no error detail | `Option A` |
| Ok/err with a shared discriminant convention | `Result A B` |

`Option` and `Result` are `defstruct`-with-discriminant types: one struct, a
discriminant, and a single (Option) or shared (Result) payload region. A true
binary sum like `Either L R` instead gives each arm its **own** independently
typed payload slot in a tagged union, so a `Left l` and a `Right r` can carry
differently typed values without one shadowing the other.

## Declaring a sum with `defdata`

```turmeric
(defdata Either [L R]
  (Left L)
  (Right R))
```

- `[L R]` is the type-parameter vector (omit it for a non-generic sum).
- Each `(Ctor payload-types...)` clause is one constructor arm. Nullary arms
  (`(Red)`, `(None)`) carry no payload.
- Add `:copy` after the name (`(defdata Color :copy (Red) (Green) (Blue))`) to
  make values duplicated-on-use rather than moved. Only sound when every
  payload is itself copyable.

Constructors are ordinary call forms: `(Left 1)`, `(Right "x")`. They infer
the unmentioned type parameter from context.

### Ownership note

A sum value (without `:copy`) is **linear/affine**: passing it to a function or
matching on it *consumes* it. This is by design -- the idiomatic way to inspect
a sum is a single `match`, exactly as in Rust/ML. Predicates such as `left?`
therefore consume their argument; reach for `match` when you need the payload
afterwards.

## Pattern matching with `match`

`match` destructures a sum, binding each arm's payload to fresh variables that
carry the correct arm type in the body:

```turmeric
(defn describe [e : (Either int cstr)] : int
  (match e
    (Left n)  (do (println "left")  n)     ; n : int
    (Right s) (do (println s) 0)))          ; s : cstr
```

- A bare `_` is a wildcard arm (matches anything, binds nothing).
- A bare symbol arm captures the whole scrutinee.
- Literal arms (`0`, `"x"`, `true`) match primitive scrutinees.

### Exhaustiveness is a hard error

If a `match` over a known sum omits an arm and has no wildcard, the compiler
**rejects** it:

```
error: match: non-exhaustive patterns -- constructor 'Right' of 'Either' not covered
```

This is stricter than a warning on purpose: a missing arm is almost always a
bug. To intentionally handle only some arms, add a `_` wildcard catch-all:

```turmeric
(match e
  (Left _) "left"
  _        "other")          ; covers Right (and anything else)
```

A redundant arm (a constructor already covered earlier) is a *warning*, not an
error.

### Nested patterns

Constructor patterns currently bind each field to a **symbol** only; a nested
constructor pattern such as `(Right (Left a))` is not yet supported. Destructure
in two steps instead:

```turmeric
(match e
  (Left n)      ...
  (Right inner) (match inner            ; bind the inner sum, then match it
                  (Left a)  ...
                  (Right b) ...))
```

(See `docs/reported/nested-match-patterns.md` for the tracked enhancement.)

## Either as "error or value"

`Either` is the precise carrier for a computation that yields a value *or* an
error, keeping both fully typed -- unlike `Option` (which drops the error) or a
two-tuple sentinel (which drops the tag). By convention **Left is the error
arm** and **Right is the success arm**, which is why the `Functor` instance maps
over Right:

```turmeric
(load "stdlib/either.tur")

(defn safe-div [n : int d : int] : (Either int int)
  (if (= d 0)
    (Left 0)             ; error code
    (Right (/ n d))))    ; quotient

(match (safe-div 20 4)
  (Left _)  -1
  (Right q) q)            ; => 5
```

### Helper API (`stdlib/either.tur`)

`Either` is **not** auto-loaded (to keep it out of every program and avoid
constructor-name collisions). Pull it in explicitly:

```turmeric
(load "stdlib/either.tur")       ; or: (import either)
```

| Function | Purpose |
|---|---|
| `(left? e)` / `(right? e)` | discriminant predicates (consume `e`) |
| `(either e on-left on-right)` | eliminate to a single result |
| `(either-map e f)` | map `f` over a Right payload |
| `(either-map-left e f)` | map `f` over a Left payload |
| `(either-swap e)` | exchange the arms |
| `(from-left e dflt)` / `(from-right e dflt)` | extract a payload or default |
| `(safe-div n d)` | division as `(Either int int)` |
| `Functor [(Either E)]` | `fmap` over Right via `(.fmap e f)` |

The transformers that apply a closure (`either`, `either-map`, ...) return the
**erased** carrier `: int`, mirroring `result-map`/`free-bind`. Re-annotate with
`::` to recover the sum type when feeding the result into a typed consumer or a
`match`:

```turmeric
(let [m (:: (.fmap (Right 5) (fn [x : int] : int (* x 8))) (Either int int))]
  (from-right m 0))      ; => 40
```

`safe-div` keeps a precise `(Either int int)` return because both arms have
concrete `int` payloads, so its result is directly matchable without `::`.

## FFI: the C layout

A value of `(defdata Either [L R] (Left L) (Right R))` is an `int64` handle to a
heap-allocated tagged union:

```c
typedef struct tur_adt_Either {
    int tag;                        /* 0 = Left, 1 = Right (declaration order) */
    union {
        struct { int64_t _0; } Left;
        struct { int64_t _0; } Right;
    } as;
} tur_adt_Either;
```

Each payload is erased to `int64_t` at the C boundary (just as `Option[A]`
erases its slot); `match` restores the declared arm type. An inline-C block can
construct or destructure a sum by redeclaring this struct locally:

```turmeric
(defn either-tag [e : int] #{Unsafe} : int
  ```c typedef struct { int tag; union { struct { int64_t _0; } Left;
                                         struct { int64_t _0; } Right; } as; } E;
  return (int64_t)((E*)(intptr_t)e)->tag;
  ```)
```

The tag values follow constructor declaration order, so the discriminant
convention matches `Option`/`Result` (first arm = 0).

## Typeclass instances over a partially applied sum

`Either E` (one argument fixed) is a `* -> *` type constructor, so it can be a
Functor/Monad instance head. Because the typeclass (`Functor`) lives in another
module, the instance is non-orphan precisely because `Either` is defined in the
instance's own module:

```turmeric
(definstance Functor [(Either E)]
  (fmap [container f]
    (match container
      (Left l)  (Left l)
      (Right r) (Right (f r)))))
```

Dispatch happens via `(.fmap value f)`.

## See also

- `stdlib/either.tur` -- the reference module.
- `tests/fixtures/sum-either-*` -- runnable examples for every feature above.
- `docs/upcoming/sum-types-either-plan.md` -- design rationale and the ADR
  recording what was already in place when this landed.
