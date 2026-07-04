---
title: Lenses
category: Standard Library
description: First-class functional lenses (view / set / over) via stdlib/lens.tur, the profunctor-by-record encoding shipped by default, and the van Laarhoven form available behind experiment flags.
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

## The two encodings

Turmeric supports two lens encodings. The **profunctor-by-record** form ships as
the default in `stdlib/lens.tur` and needs no experiment flags. The **van
Laarhoven** form -- the classic Haskell optic

```
type Lens s a = forall f. Functor f => (a -> f a) -> (s -> f s)
```

-- is available behind the `--enable=forall-*` / `hkt-hrt` / `forall-dict-pass`
experiments and supports `view`/`set`/`over`, generic focus inference, and
composition with ordinary function composition. It carries the caller's
`Functor` dictionary through the poly carrier at runtime, so the lens body
dispatches `fmap` on whichever instance the caller picks (`Const` for `view`,
`Identity` for `set`/`over`).

The record encoding stays the default because it needs no experiments to run
and it works with **any** `Functor`-like use, not just those whose `(f a)` fits
the poly carrier. See [Functor width](#functor-width) below.

### Composition -- the one tradeoff of the record form

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
than once.

The van Laarhoven form composes freely (`l1 . l2` is just function
composition); reach for it when you need optic composition and can accept the
experiment flags.

## Functor width

The van Laarhoven form threads `(f a)` through a one-int64 poly carrier. Two
shapes of functor work through that carrier:

- **Carrier-compatible functors** -- an opaque or `:heap` type whose `(f a)`
  fits in one int64 word (e.g. `(defopaque Const [r a] :int)`,
  `(defopaque Identity [a] :int)`). These ride the carrier directly at no
  runtime cost beyond the dict dispatch.
- **Wide by-value functors** -- a `:copy` struct or flat-product ADT whose
  `(f a)` is wider than one word. These work with **no flag** (graduated
  2026-07-04): codegen boxes the aggregate into the carrier at each lens
  crossing and unboxes it back on the other side. `view`/`set`/`over`, generic
  focus inference, and composition all thread through unchanged. The box pays
  one heap alloc + copy + free per crossing.

For the zero-overhead path, add **`--enable=vl-wide-mono`**: every lens call
site whose lens uniquely resolves is redirected to a by-value monomorphized
body that spells `(f a)` by value with **no heap box** on either the `(f S)`
result or the `(f A)` functor-wrapping result. Lens uses that do not resolve
uniquely fall back to the (boxed) Path A carrier bridge. See
[../upcoming/van-laarhoven-monomorphization-plan.md](../upcoming/van-laarhoven-monomorphization-plan.md)
(Path B).

## Related

- [`stdlib/lens.tur`](../../stdlib/lens.tur) -- the module
- [constrained-hkt-forall plan](../upcoming/constrained-hkt-forall-plan.md) --
  the van Laarhoven roadmap and the mode-A/mode-B decision
- [constrained-hkt-forall mode-B plan](../upcoming/constrained-hkt-forall-mode-b-plan.md) --
  the dictionary passing + dispatch the van Laarhoven form runs on
- [van-laarhoven-wide-functor-carrier-plan](../upcoming/van-laarhoven-wide-functor-carrier-plan.md) --
  the wide-by-value functor carrier bridge (Path A, now always-on)
- [van-laarhoven-monomorphization-plan](../upcoming/van-laarhoven-monomorphization-plan.md) --
  the zero-overhead by-value monomorphization (Path B, `--enable=vl-wide-mono`)
- [hrt-guide.md](hrt-guide.md) -- the rank-2 `forall` mechanism lenses use
