---
title: Existential Types
category: Language Features
description: Existential types: pack/open, typeclass constraints, hiding concrete types behind opaque boundaries
---

# Existential Types in Turmeric

This guide covers existential types: how to *hide* a concrete type behind an
opaque boundary while still allowing operations on it through typeclass
constraints. Existentials are the dual of higher-ranked types (`forall`):
where `forall` lets the caller pick the type, `exists` lets the callee pick
the type and only expose what the caller is allowed to do with it.

## What is an existential type?

`(exists [a] T)` means "there is some type `a` for which `T` holds, but
the value's holder does not know which one". The type is sealed behind
the existential boundary. Without constraints the holder can do nothing
useful with the inner value -- it is fully opaque.

With a typeclass constraint, the holder can call the constraint's methods
on the inner value without ever learning its concrete type:

```turmeric
; A boxed value paired with an evidence that its type implements Show.
(exists [a] [(Show a)] a)
```

## `pack` -- introduce an existential

`(pack value (exists [a] [(C a) ...] T))` boxes a value as an existential.
The compiler verifies at the pack site that the concrete type of `value`
has an instance for each constraint `(C a)`. The resulting record carries
the boxed value and a vtable pointer for each constraint.

```turmeric
(defclass Show [a]
  (show [x] :cstr))

(definstance Show [int]
  (show [x] :cstr
    ```c
    char *buf = (char *)malloc(24);
    snprintf(buf, 24, "%lld", (long long)x);
    return (const char *)buf;
    ```))

(defn main [] :int
  (let [e (pack 42 (exists [a] [(Show a)] a))]
    0))
```

If you omit the constraint, the existential has no usable methods -- it
is a pure information-hiding boundary:

```turmeric
(pack 42 (exists [a] a))   ; opaque -- nothing you can do with the inner value
```

## `open` -- eliminate an existential

`(open e [a v] body)` unboxes the existential and binds:

- `a` to a fresh *skolem* (an abstract type that unifies only with itself
  inside `body`)
- `v` to the inner value

The escape check then refuses any `body` whose result type can refer to
`a`. That is what makes the existential genuinely opaque: you cannot
return the inner value, alias it through a `let`, or smuggle it through
one branch of an `if`.

```turmeric
; ok: body returns a :cstr, not a value of type `a`.
(open e [a v]
  (.show v))

; rejected: body would leak the abstract type `a` out of its scope.
(open e [a v]
  v)
```

The error message names the abstract variable that escapes:

```
open: existential type variable 'a' escapes its scope
(the body returns the bound value 'v' or an alias)
```

## A worked example: Showable lists

A heterogeneous list of "things that can be shown" requires either an
existential type or a shared concrete type. With existentials each
element pairs the value with the witness for its `Show` instance, so a
list of mixed concrete payloads can still be printed:

```turmeric
(defclass Show [a]
  (show [x] :cstr))

(definstance Show [int]
  (show [x] :cstr
    ```c
    char *buf = (char *)malloc(24);
    snprintf(buf, 24, "%lld", (long long)x);
    return (const char *)buf;
    ```))

(definstance Show [bool]
  (show [x] :cstr
    (if x "true" "false")))

(defn main [] :int
  (let [e1 (pack 42   (exists [a] [(Show a)] a))
        e2 (pack true (exists [a] [(Show a)] a))]
    (println (open e1 [a v] (.show v)))
    (println (open e2 [a v] (.show v))))
  0)
```

The pack/open sites stay inline; passing a constrained existential
through a function parameter erases the constraint info and prevents
`open` from extracting the boxed value (use the value at the
construction site for now).

## The `stdlib/existential.tur` helpers

The `tur/existential` stdlib module provides two convenience macros for
the `Show`-constrained case. They are load-on-demand:

```turmeric
(load "stdlib/existential.tur")

(defn main [] :int
  (let [e (showable 42)]
    (println (show-it e)))
  0)
```

| Macro | Expands to |
|-------|------------|
| `(showable x)` | `(pack x (exists [a] [(Show a)] a))` |
| `(show-it e)`  | `(open e [a v] (.show v))` |

`Show` must be in scope at the call site, either via a local `defclass`
or by loading `stdlib/typeclass.tur`. The helpers are macros so the
existential type form appears literally at the pack/open site, which is
required by the elaborator.

## When to use existentials

- **Heterogeneous collections** of values that share a common operation
  set (`Show`, `Eq`, `Display`, ...) but not a concrete type.
- **Opaque handles** where the caller must not inspect the internal
  representation -- the constraint set defines the entire public API.
- **Hiding size indices** (planned, EX2): `Vec[A] = (exists [n] (SizedVec [n A]))`
  unifies length-indexed and length-erased vectors.

## What is not (yet) supported

- **Passing constrained existentials through `:ptr<void>` parameters.**
  The static type loses its constraint info and `open` can no longer
  extract the boxed value from the runtime record. Open at the
  construction site, or pass through a binding typed with the full
  existential form.
- **Heterogeneous-dispatch method calls.** Inside `open`, method
  dispatch on `v` currently resolves through the inner concrete type's
  static instance. The full vtable-indirected dispatch through the
  record's witnesses is reserved for a follow-on patch.
- **Reclaiming existential records.** Constrained packs allocate a
  `tur_existential_t` on the heap; the deferred memory-management work
  is tracked in [`../existential-gc-plan.md`](../existential-gc-plan.md).
