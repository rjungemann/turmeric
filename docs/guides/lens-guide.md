---
title: Lenses
category: Standard Library
description: First-class functional lenses (view / set / over) via stdlib/lens.tur, the profunctor-by-record encoding, and why it stays the shipped default even though the van Laarhoven view/set/over now type-check.
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

So the lens itself is expressible. `stdlib/lens.tur` nonetheless still ships the
**profunctor-by-record** encoding -- a `Lens` is a concrete record of a getter
and a setter -- because two things the record encoding gets for free are still
missing from the van Laarhoven form here:

- **Composition.** Optic composition is the whole point of the van Laarhoven
  form, and it does not fully work yet. The pieces are landing: a constrained
  rank-2 lens can now *delegate* to another at its own abstract functor,
  forwarding the runtime dictionary (a nested `(f B)` annotation recovers the
  enclosing `f : * -> *`, and the nested `Functor f` obligation is discharged by
  forwarding the caller's dict). What is still missing is the *adapter lambda*
  that composition needs -- `(fn [p] : (f Point) (point-x g p))` handed to another
  lens -- which requires capturing the ambient dictionary into the lifted closure
  and preserving the rank-2 `(f X)` argument type. See
  [docs/reported/van-laarhoven-lens-composition.md](../reported/van-laarhoven-lens-composition.md).
- **General functors.** The working `view`/`set`/`over` rely on `Const`/`Identity`
  being *carrier-compatible* opaques (one int64 word), so `(f a)` flows through
  the type-erased carrier. A functor whose `(f a)` is a wider by-value aggregate
  is still the mode-B "No-go".

The record encoding needs none of that machinery, so it stays the shipped
default and `view`/`set`/`over` are ordinary function calls.

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
than once. When van Laarhoven composition lands (it needs dictionary forwarding
for the nested constrained rank-2 call --
[report](../reported/van-laarhoven-lens-composition.md)), the van Laarhoven form
and free composition become a real alternative; until then, hand-composition is
the idiom.

## Related

- [`stdlib/lens.tur`](../../stdlib/lens.tur) -- the module
- [constrained-hkt-forall plan](../upcoming/v1/constrained-hkt-forall-plan.md) --
  the van Laarhoven roadmap (slices 1-4) and the mode-A/mode-B decision
- [constrained-hkt-forall mode-B plan](../upcoming/v1/constrained-hkt-forall-mode-b-plan.md) --
  MB1-MB4: the dictionary passing + dispatch that makes the van Laarhoven
  `view`/`set`/`over` above run
- [hrt-guide.md](hrt-guide.md) -- the rank-2 `forall` mechanism lenses use
