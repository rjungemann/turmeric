---
title: "S4 -- `Vec` element type not inferred forward from `vec-push!` (the `vec-new` + `vec-push!` idiom stays polymorphic)"
category: Type checking / inference -- forward flow from `vec-push!` into a fresh `vec-new` binding
severity: Low/Medium. Does not block (a one-token ascription at construction is
  the workaround), but the diagnostic is misleading and the ascription is
  verbose for existential element types. Only the `vec-new` + `vec-push!` idiom
  is affected; `vec-of` and a Vec stored in a typed `defstruct` field both
  infer their element type fine.
status: OPEN
reported-by: turmeric-spices Claude (spice-uplift work, branch claude/tender-ramanujan-xm7bgg)
verified-on: turmeric 0.21.0, this tree (post #452 / #456 / #458)
---

# S4 -- `Vec` element type not inferred from `vec-push!`

## One-line summary

`(vec-new)` binds a fresh `(Vec A)` with an unconstrained element tyvar, and
subsequent `vec-push!` calls do **not** unify `A` with the pushed element type.
The binding stays polymorphic and fails to match a concrete `(Vec T)` parameter
(or an existential element type) at the use site.

## Reproduction (verified on this tree)

```turmeric
(defn sum [v : (Vec int)] : int
  (__go v (vec-len v) 0 0))
(defn __go [v : (Vec int) n : int i : int acc : int] : int
  (if (>= i n) acc (__go v n (+ i 1) (+ acc (vec-get v i)))))

(defn main [] : int
  (let [rs (vec-new)]
    (vec-push! rs 10)         ;; pushes an int ...
    (vec-push! rs 20)
    (sum rs)))               ;; sum : (Vec int) -> int
```

```
error [TUR-E0001]: function 'sum' arg 1:
  expected (type-app Vec int), got (type-app Vec tyvar 'A')
```

The first `vec-push!` of an `int` does not pin `A := int`; the binding `rs`
keeps its fresh element tyvar all the way to the `sum` call.

## Expected

The first `vec-push!` of an `int` should pin `A := int` by forward flow from
the pushes -- the same element type `vec-of` infers when the elements are
present at construction. After that, `rs : (Vec int)` and the `sum` call type
checks.

## Workaround (in landed spice code)

Ascribe the element type at construction:

```turmeric
(let [rs (:: (vec-new) (Vec int))]
  (vec-push! rs 10)
  (sum rs))                  ;; OK -- check passes
```

Verified: with the `(:: (vec-new) (Vec int))` ascription the same program type
checks (`tur check` exits 0). For an existential element type the ascription is
the verbose `(:: (vec-new) (Vec (exists [a] [(Renderer a)] a)))`.

This is exactly the idiom documented in the plot spice
(`turmeric-spices` `spices/plot/src/plot/core.tur:805-815`):

> Ascribe the vec's element type at construction:
> `(:: (vec-new) (Vec (exists [a] [(Renderer a)] a)))`
> vec-new/vec-push! do not infer the element type from the pushes.

The ascription could be deleted at those sites once forward inference lands.

## Notes / scope

- Only bites the `vec-new` + `vec-push!` idiom. `vec-of` (elements present) and
  a Vec stored in a typed `defstruct` field both infer the element type fine.
- Most visible with existential element types, where the ascription is long.
- Fix direction: when a `vec-push!` is applied to a binding whose Vec element
  is still an unconstrained tyvar, unify that tyvar with the pushed value's
  type (forward flow), rather than leaving it open until the consuming call.
