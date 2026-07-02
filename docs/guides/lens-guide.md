---
title: Lenses
category: Standard Library
description: First-class functional lenses (view / set / over) via stdlib/lens.tur, the profunctor-by-record encoding, and why the van Laarhoven form is deferred.
---

# Lenses

A **lens** is a first-class getter/setter for a part `A` of a whole `S`. It lets
you read, replace, and transform a nested field without hand-writing the
rebuild-the-whole boilerplate at every use site. Turmeric ships lenses in
[`stdlib/lens.tur`](../../stdlib/lens.tur).

```turmeric
(load "stdlib/lens.tur")

(defstruct Point :copy :heap [x : int y : int])

(defn point-x [] : (Lens Point int)
  (lens
    (fn [p : Point] : int (.x p))
    (fn [nx : int p : Point] : Point (make-struct Point :x nx :y (.y p)))))

(defn main [] : int
  (let [px (point-x)
        p  (make-struct Point :x 3 :y 9)]
    (println (view px p))                                     ; 3
    (println (.x (set px 42 p)))                              ; 42
    (println (.x (over px (fn [v : int] : int (* v 10)) p)))) ; 30
  0)
```

## The API

| Combinator | Type | Meaning |
| --- | --- | --- |
| `lens`   | `(fn [S] A) -> (fn [A S] S) -> (Lens S A)` | build a lens from a getter and a setter |
| `view`   | `(Lens S A) -> S -> A`                     | read the focused `A` |
| `set`    | `(Lens S A) -> A -> S -> S`                | replace the focused `A` |
| `over`   | `(Lens S A) -> (fn [A] A) -> S -> S`       | map a function over the focused `A` |

`view l s` is `((.lget l) s)`; `set l a s` is `((.lput l) a s)`; `over l f s`
is `set l (f (view l s)) s`.

## Value semantics

A `Lens` is `:copy`, so a single lens value can be `view`/`set`/`over`'d
repeatedly. The whole `S` a lens focuses is usually a `:copy` struct too, so it
can be reused across calls; if the setter rebuilds the record (the common case),
make it `:copy :heap` so it is pointer-carried and cheap to thread. A move-only
`S` can still be used, but only linearly (each whole consumed once).

## The encoding, and why not van Laarhoven

The classic Haskell optic is the **van Laarhoven** form:

```
type Lens s a = forall f. Functor f => (a -> f a) -> (s -> f s)
```

whose elegance is that optics compose with ordinary function composition and
`view`/`set`/`over` fall out by instantiating `f` to `Const`/`Identity`. That
form is **not expressible on Turmeric today**, for two reasons the compiler
surfaces directly:

1. **The lens body must dispatch `fmap` on an abstract functor.** Inside the
   lens, `fmap` runs over `(f a)` where `f` is chosen by the *caller*
   (`view` picks `Const`, `set`/`over` pick `Identity`). Turmeric's HRT is
   type-erased: a rank-2 poly fn is compiled once and called through the int64
   carrier, so the lens body cannot statically resolve `fmap`, and there is no
   runtime typeclass-dictionary passed through the carrier to resolve it
   dynamically. That dictionary path is **mode B** of the
   [constrained-hkt-forall plan](../upcoming/v1/constrained-hkt-forall-plan.md),
   deliberately deferred (slice 2 shipped the static, mode-A form).
2. **The curried rank-2 result is a function.** `l g` returns `s -> f s`, which
   is then applied -- a poly-carrier call whose result is itself a closure --
   which the current rank-2 machinery does not thread.

So `stdlib/lens.tur` uses the **profunctor-by-record** encoding instead: a
`Lens` is a concrete record of a getter and a setter. It needs none of the
higher-kinded / constrained-quantifier machinery, and `view`/`set`/`over` are
ordinary function calls.

### The one tradeoff: composition

The record encoding gives up composing optics with ordinary function
composition. A generic `lens-compose` would need its setter to read the whole
`s` (to view the intermediate part) *and* write it back -- using `s` twice --
which the linearity checker rejects for an abstract move-only `S`. Compose by
hand at concrete, copyable whole types:

```turmeric
;; Line -> start:Point -> x:int
(defn line-start-x [] : (Lens Line int)
  (lens
    (fn [l : Line] : int (.x (.start l)))
    (fn [nx : int l : Line] : Line
      (make-struct Line
        :start (make-struct Point :x nx :y (.y (.start l)))
        :end   (.end l)))))
```

where the whole types (`Line`, `Point`) are `:copy` so `l`/`s` can be used more
than once. If mode-B dictionary passing lands, the van Laarhoven form (and free
composition) becomes expressible; until then, hand-composition is the idiom.

## Related

- [`stdlib/lens.tur`](../../stdlib/lens.tur) -- the module
- [constrained-hkt-forall plan](../upcoming/v1/constrained-hkt-forall-plan.md) --
  the van Laarhoven roadmap (slices 1-4) and the mode-A/mode-B decision
- [hrt-guide.md](hrt-guide.md) -- the rank-2 `forall` mechanism lenses would use
