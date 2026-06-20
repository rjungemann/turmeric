---
title: "S6 -- self-recursive `defn` with a by-value `:copy` struct accumulator types the recursive call as the carrier `int`"
category: Type checking + codegen -- recursive-call return type read from the carrier ABI before the declared signature
severity: Medium. A top-level recursive `defn` that returns / accumulates a
  by-value `:copy` struct fails to type-check (the recursive call is seen as the
  int carrier, so an `if` whose other arm is the struct reports a branch
  mismatch); ascribing past the type error then mis-emits at the carrier
  boundary. An `int` accumulator recurses fine -- the struct return is the
  trigger. Worked around by threading a scalar carrier and building the struct
  once after the loop.
status: OPEN
reported-by: turmeric-spices Claude (spice-uplift work, branch claude/tender-ramanujan-xm7bgg)
verified-on: turmeric 0.21.0, this tree (post #452 / #456 / #458)
---

# S6 -- self-recursive `defn` with a by-value `:copy` struct accumulator

## One-line summary

A top-level recursive `defn` whose accumulator/return is a by-value `:copy`
struct types the **recursive call** as the carrier `int`, not as the declared
return type. An `if` whose other arm is the struct therefore reports a branch
mismatch (`then=Box else=int`). Ascribing the recursive call past the type
error then mis-emits at the carrier boundary (`aggregate value used where an
integer was expected`).

## Reproduction (verified on this tree)

```turmeric
(defstruct Box :copy [lo : int  hi : int])
(defn add-box [a : Box b : Box] : Box
  (make-struct Box (+ (.lo a) (.lo b)) (+ (.hi a) (.hi b))))

(defn go [n : int  i : int  acc : Box] : Box
  (if (>= i n) acc
    (go n (+ i 1) (add-box acc (make-struct Box i (* 2 i))))))
```

```
error: if branches have mismatched types: then=Box else=int
```

The `then` arm `acc` is `Box`; the `else` arm -- the recursive `(go ...)` call,
declared `: Box` -- is seen as `int`.

Ascribing the recursive call past the type error (`(:: (go ...) Box)`) then
fails in codegen:

```
/tmp/tur-build/s6b_tur.c: error: aggregate value used where an integer was expected
```

Control: the same recursion with an `int` accumulator/return type-checks and
builds with no issue, so the by-value struct return is the trigger.

## Expected

The recursive call's result type is the declared return type (`Box`), so both
`then` and `else` are `Box` and the `if` type-checks. Codegen returns the
struct by value, the same as a top-level recursive `defn` returning a freshly
`make-struct`'d value.

## Workaround (in landed spice code)

Thread a **scalar carrier** through the recursion instead of the struct -- a
malloc'd `ptr<void>` record updated in place -- and build the struct once after
the loop. In plot this is `__rbb-union-corners!` accumulating a
`{x0,x1,y0,y1;valid}` carrier, with `bbox-from-raw` packing it into a `BBox` at
the end (`turmeric-spices` `spices/plot/src/plot/core.tur:879-902`). The comment
at `:881-888` records exactly this:

> The accumulator is the raw `{x0,x1,y0,y1;valid}` carrier (an int handle), not
> a `BBox` value: a self-recursive `defn` that threads a by-value `:copy` struct
> through its accumulator currently mis-types the recursive call as the carrier
> int (then=BBox/else=int) and mis-emits it. Folding a scalar carrier sidesteps
> that; `renderers-bbox` packs the result into a `BBox` once at the end via
> `bbox-from-raw`.

The carrier dance could be replaced with a direct `: Box` accumulator once the
recursive call's return type is read from the declared signature.

## Notes / scope

- An `int` accumulator recurses fine; the by-value `:copy` struct return is the
  trigger.
- Likely the recursive callee's return type is read from the carrier ABI (the
  int64 result slot) before the declared signature is bound, so the call
  expression is typed `int` at the recursive use site. The ascription
  mis-emit is the same carrier boundary surfacing in codegen.
- Related in spirit to the result-position carrier-ABI gaps tracked in
  `docs/reported/instance-method-return-not-unified.md`, but here it is the
  *recursive self-call*'s read-back type, not a body-vs-declared mismatch.
