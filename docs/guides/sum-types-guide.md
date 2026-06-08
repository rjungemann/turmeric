---
title: Sum Types and `Either`
category: Language
description: Declaring binary and n-ary sum types with `defdata`, pattern matching with `match`, exhaustiveness behaviour and the `#{NonExhaustive}` opt-out, the FFI layout, and the stdlib `Either` module.
---

# Sum Types and `Either`

A *sum type* (a.k.a. tagged union or algebraic data type) is a value that is
*one of* several shapes, each carrying its own payload. Turmeric spells these
with `defdata` and takes them apart with `match`.

```turmeric
(defdata Shape
  (Circle :int)            ; radius
  (Rect   :int :int))      ; width, height

(defn area [s : int] : int
  (match s
    (Circle r)   (* 3 (* r r))
    (Rect  w h)  (* w h)))
```

Sweet-exp equivalent:

```turmeric
#lang sweet-exp

defdata Shape
  Circle(:int)          ; radius
  Rect(:int :int)       ; width, height

defn area [s : int] : int
  match s
    Circle(r)   *(3 *(r r))
    Rect(w h)   *(w h)
```

The canonical binary sum is `Either L R` from `stdlib/either.tur`: a value is
either `(Left l)` carrying an `L`, or `(Right r)` carrying an `R`.

## When to use a sum vs. a struct

| Use a... | When the value is... |
|----------|----------------------|
| `defstruct` (record) | *all of* several fields at once (a point has both an x **and** a y) |
| `defdata` (sum) | *one of* several alternatives (a result is a success **or** an error) |

Reach for a sum whenever you would otherwise smuggle a "which case is this?"
discriminant alongside a payload whose meaning depends on it -- an error code
plus a maybe-valid value, a parse result that is either a value or a failure,
a protocol step that branches. The sum makes the discriminant and the payload
inseparable, so the type checker forces you to handle every case.

## Declaring a sum: `defdata`

```turmeric
(defdata Either :copy [L R]
  (Left L)
  (Right R))
```

- **Name** -- the type constructor (`Either`).
- **`:copy` / `:move`** (optional) -- value discipline. `:copy` makes a value
  freely reusable (right for immutable, resource-free sums like `Either`);
  the default `:move` enforces single-use ownership. Put it right after the
  name.
- **Type parameters** `[L R]` (optional) -- a parametric sum. Each constructor
  may use these in its payload types.
- **Constructors** `(Left L) (Right R)` -- one parenthesised arm each. A
  constructor may take zero payloads (`(Nil)`), concrete payloads
  (`(Circle :int)`), or type-parameter payloads (`(Left L)`).

Constructors become ordinary callables: `(Left 3)`, `(Right "ok")`.

## Pattern matching: `match`

`match` dispatches on the constructor and binds each payload:

```turmeric
(match e
  (Left  l)  (handle-error l)
  (Right r)  (use r))
```

Sweet-exp equivalent:

```turmeric
#lang sweet-exp

match e
  Left(l)   handle-error(l)
  Right(r)  use(r)
```

- Bind a payload to a name (`l`, `r`) to use it in that arm's body.
- Use `_` to ignore a payload: `(Left _)`.
- Sub-match a payload that is itself a sum by matching again on the bound
  variable (or, where supported, by nesting the pattern directly).

```turmeric
(match outer
  (Left n)   n
  (Right r)  (match r
               (Nada)   -1
               (Just k) k))
```

Sweet-exp equivalent:

```turmeric
#lang sweet-exp

match outer
  Left(n)   n
  Right(r)  match r
              Nada()   -1
              Just(k)  k
```

## Exhaustiveness

A `match` whose scrutinee is a known sum type must cover **every**
constructor (or include a wildcard). A missing arm is a **hard error**:

```
match: non-exhaustive patterns -- constructor 'Left' of 'Either' not covered
```

This is deliberate: the compiler will not let a new constructor silently slip
past existing matches.

### Opting out: `#{NonExhaustive}`

When you have *proven* a case cannot occur by other means, place the
`#{NonExhaustive}` marker immediately after `match` (before the scrutinee) to
suppress the diagnostic:

```turmeric
(defn unwrap-right [e : int] : int
  (match #{NonExhaustive} e
    (Right r) r))      ; Left is statically impossible here, by construction
```

Sweet-exp equivalent:

```turmeric
#lang sweet-exp

defn unwrap-right [e : int] : int
  match #{NonExhaustive} e
    Right(r)  r         ; Left is statically impossible here, by construction
```

The marker is the *only* escape hatch; without it a non-exhaustive match does
not compile. Use it sparingly and leave a comment explaining why the missing
arm is unreachable.

> Note: this guide's plan ([sum-types-either-plan](../archive/history/sum-types-either-plan.md)) originally specced
> exhaustiveness as a *warning*. The implementation keeps it a hard **error**
> (the stronger, pre-existing behaviour) and adds `#{NonExhaustive}` as the
> deliberate opt-out -- see that plan's ADR for the rationale.

## The `Either` module

`stdlib/either.tur` is not auto-loaded; pull it in explicitly:

```turmeric
(load "stdlib/either.tur")
```

It provides:

| Form | Meaning |
|------|---------|
| `(Left l)` / `(Right r)` | constructors |
| `(left? e)` / `(right? e)` | arm predicates |
| `(from-left dflt e)` / `(from-right dflt e)` | extract a payload or fall back to `dflt` |
| `(either on-left on-right e)` | eliminate by applying one of two functions |
| `(either-map f e)` | map `f` over a `Right`, pass `Left` through (right-biased) |
| `(either-map-left f e)` | map `f` over a `Left`, pass `Right` through |
| `Functor [(Either E)]` | right-biased `fmap` instance |

`Either` is **right-biased** by convention: `Right` is the "success"/"main"
arm and `Left` the "error"/"other" arm, so `either-map` and the `Functor`
instance act on `Right` and leave `Left` untouched -- matching Haskell's
`Either`.

```turmeric
(load "stdlib/either.tur")

(defn inc [x : int] : int (+ x 1))

(defn main [] : int
  (println (from-right -1 (fmap (Right 41) inc)))  ; => 42
  (println (from-left  -1 (fmap (Left 9)  inc)))   ; =>  9  (Left untouched)
  0)
```

### Worked migration: parsing with error detail

`str->int` / `cstr->parse-int` fold every failure -- a NULL pointer, an empty
string, trailing garbage -- into the sentinel `0`, indistinguishable from a
legitimately parsed `0`. `str->int-checked` (also in `stdlib/str.tur`) returns
an `Either` instead, so the failure detail survives:

```turmeric
(load "stdlib/str.tur")

(from-right -1 (str->int-checked "42"))   ; => 42
(right? (str->int-checked "0"))           ; => true   (a real zero, not failure)
(left?  (str->int-checked "12x"))         ; => true   (Left 1: trailing garbage)
(left?  (str->int-checked ""))            ; => true   (Left 0: empty/NULL)
```

## FFI layout

A `defdata` lowers to a single, type-erased tagged struct. For `Either`:

```c
typedef struct tur_adt_Either {
    int tag;                       /* 0 = Left, 1 = Right */
    union {
        struct { int64_t _0; } Left;
        struct { int64_t _0; } Right;
    } as;
} tur_adt_Either;
```

- `tag` is the discriminant: `0` for the first constructor, `1` for the
  second, and so on for n-ary sums.
- Payloads are erased to `int64_t` at the C boundary (the same way `Option[A]`
  and `Result[A B]` erase theirs); the elaborator restores the proper type on
  extraction.
- Each constructor has a `ctor_<Name>` factory (`ctor_Left`, `ctor_Right`)
  that heap-allocates the struct, sets the tag, and returns an `int64_t`
  handle.

Inline-C blocks can therefore construct and destructure sum values directly:

```turmeric
(defn right-payload [e : int] #{Unsafe} : int
  ```c
  tur_adt_Either *p = (tur_adt_Either *)(intptr_t)e;
  return p->as.Right._0;
  ```)
```

## Writing a typeclass instance over a sum

A type constructor with a free parameter can carry a typeclass instance. For a
binary sum, fix one parameter with a partially-applied head -- `(Either E)`
leaves the kind-(`* -> *`) functor over the `Right` arm:

```turmeric
(definstance Functor [(Either E)]
  (fmap [container fn]
    ```c
    tur_adt_Either *p = (tur_adt_Either *)(intptr_t)container;
    if (p->tag != 1) return container;          /* Left: identity */
    return ctor_Right(fn.fn(fn.env, p->as.Right._0));
    ```))
```

The instance is **not** an orphan when it lives in the module that defines the
sum (here, `stdlib/either.tur`), even though `Functor` is declared elsewhere --
the owning module of *either* the class or a type argument may host the
instance.

## See also

- [sum-types-either-plan](../archive/history/sum-types-either-plan.md) -- the design plan and ADR.
- [`stdlib/result.tur`](../../stdlib/result.tur) / `stdlib/option.tur` --
  the unary-payload tagged structs that predate `Either`.
- [[data-literals-guide]] -- literal construction syntax that composes with
  sum payloads.
