---
title: Lenses
category: Standard Library
description: First-class functional lenses (view / set / over) via stdlib/lens.tur, the profunctor-by-record encoding, and why it stays the shipped default even though the experimental van Laarhoven form now supports view/set/over, generic focus inference, and composition.
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

## The encoding, and why still profunctor-by-record

The classic Haskell optic is the **van Laarhoven** form:

```
type Lens s a = forall f. Functor f => (a -> f a) -> (s -> f s)
```

whose elegance is that optics compose with ordinary function composition and
`view`/`set`/`over` fall out by instantiating `f` to `Const`/`Identity`.

The van Laarhoven `view`/`set`/`over` now **do** type-check and run on Turmeric.
The two compiler reasons this section used to cite as blockers have both been
resolved by the mode-B slices of the
[constrained-hkt-forall plan](../upcoming/v1/constrained-hkt-forall-plan.md):

1. **Dispatching `fmap` on an abstract functor.** The lens body runs `fmap` over
   `(f a)` for a caller-chosen `f` (`view` picks `Const`, `set`/`over` pick
   `Identity`). Mode B threads the resolved typeclass **dictionary** through the
   int64 carrier so the body dispatches `fmap` on the caller's instance at
   runtime (MB1/MB2). A *generic* `view`/`set`/`over` that infers its focus type
   from the lens argument also works now (the focus tyvar binds through the
   rank-2 forall parameter). Both are demonstrated end to end by the fixtures
   `tests/fixtures/van-laarhoven-lens-concrete/` and
   `tests/fixtures/van-laarhoven-lens-generic/` (each returns 3/30/4/99).
2. **The curried rank-2 result is a function.** `l g` returning `s -> f s`, then
   applied, is threaded by MB3 (`--enable=hrt-curried-result`).

And van Laarhoven optics now **compose**, too: a composed lens focuses through an
adapter lambda handed to another lens
(`(defn line-a-x [^f] [^Functor f g ...] (line-a (fn [p] : (f Point) (point-x g p)) s))`),
and `view`/`set`/`over` thread through the composition -- the adapter captures the
caller-chosen functor's dictionary, so the inner lens dispatches the right
instance at runtime (fixture `tests/fixtures/van-laarhoven-lens-compose/`, 7 / 700
/ 2 / 0 / 42). Same-focus *delegation* between lenses works as well
(`van-laarhoven-lens-delegate/`).

So the full van Laarhoven form -- `view`/`set`/`over`, generic focus inference,
and composition -- is expressible. `stdlib/lens.tur` nonetheless still ships the
**profunctor-by-record** encoding (a `Lens` is a concrete record of a getter and a
setter) as the default, for two reasons:

- **General functors.** Any `Functor` instance works -- not only
  carrier-compatible opaques. `Const`/`Identity` as one-int64 opaques ride the
  mode-B carrier directly. A functor whose `(f a)` is a WIDE by-value aggregate
  (a `:copy` struct / flat-product ADT wider than one int64 word) now works too,
  behind `--enable=vl-wide-functor`: codegen boxes the aggregate into the carrier
  at each lens crossing and unboxes it back, across `view`/`set`/`over`, generic
  focus inference, and composition (fixtures
  `van-laarhoven-lens-wide-{identity,generic,compose,mixed}/`, each matching its
  opaque twin's numbers). That box pays one heap alloc + copy + free per crossing
  until the zero-overhead by-value HKT monomorphization lands (Path B of
  [../upcoming/v1/van-laarhoven-wide-functor-carrier-plan.md](../upcoming/v1/van-laarhoven-wide-functor-carrier-plan.md)).
  With the flag OFF a wide functor is still rejected up front with **TUR-E0309**
  (never a segfault), tracked as
  [../reported/van-laarhoven-functor-must-be-int-carrier.md](../reported/van-laarhoven-functor-must-be-int-carrier.md).
- **Maturity.** The van Laarhoven path runs behind the `--enable=forall-*` /
  `hkt-hrt` / `forall-dict-pass` experiments; the record encoding needs none of
  that machinery and is stable today.

The record encoding stays the shipped default and `view`/`set`/`over` are ordinary
function calls; the van Laarhoven form is available (and demonstrated by the
fixtures) for code that opts into the experiments.

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
than once. This is the idiom for the record encoding. The experimental van
Laarhoven form now supports free composition directly (see
[the archived resolution](../archive/van-laarhoven-lens-composition.md)), so code
that opts into the `--enable=forall-*` experiments can compose optics with
ordinary function composition instead.

## Related

- [`stdlib/lens.tur`](../../stdlib/lens.tur) -- the module
- [constrained-hkt-forall plan](../upcoming/v1/constrained-hkt-forall-plan.md) --
  the van Laarhoven roadmap (slices 1-4) and the mode-A/mode-B decision
- [constrained-hkt-forall mode-B plan](../upcoming/v1/constrained-hkt-forall-mode-b-plan.md) --
  MB1-MB4: the dictionary passing + dispatch that makes the van Laarhoven
  `view`/`set`/`over` above run
- [hrt-guide.md](hrt-guide.md) -- the rank-2 `forall` mechanism lenses use
