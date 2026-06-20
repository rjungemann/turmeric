---
title: "S5 -- `letrec`-bound self-recursive closure mis-emits an undeclared binding in C"
category: Codegen / closure lifting -- `letrec` self-reference name resolution
severity: Medium. Type-checking passes; the failure is purely in codegen.
  A `letrec` whose bound `fn` calls itself emits a C reference to the lifted
  binding that is never declared in that scope, so `cc` fails with
  `'<name>' undeclared`. Worked around by hoisting to a top-level recursive
  `defn`, so it does not block, but it defeats the whole point of `letrec`
  (self-reference) for the local-loop case.
status: OPEN
reported-by: turmeric-spices Claude (spice-uplift work, branch claude/tender-ramanujan-xm7bgg)
verified-on: turmeric 0.21.0, this tree (post #452 / #456 / #458)
---

# S5 -- `letrec` self-recursive closure mis-emits in C

## One-line summary

A `letrec` binding whose value is a `fn` that references the binding name in
its own body type-checks, but codegen emits a C identifier for the binding that
is never declared in the emitted scope. The C compiler rejects it with
`'<name>' undeclared (first use in this function)`.

## Reproduction (verified on this tree)

```turmeric
(defn sum-vec [rs : (Vec int)] : int
  (let [n (vec-len rs)]
    (letrec [go (fn [i : int acc : int] : int
                  (if (>= i n) acc (go (+ i 1) (+ acc (vec-get rs i)))))]
      (go 0 0))))

(defn main [] : int
  (let [rs (:: (vec-new) (Vec int))]
    (vec-push! rs 10)
    (sum-vec rs)))
```

`tur check` passes (exit 0). `tur build` fails in the generated C:

```
/tmp/tur-build/s5_tur.c:2992:50: error: 'go_1010' undeclared (first use in this function)
note: each undeclared identifier is reported only once for each function it appears in
```

The lifted closure refers to `go_1010` (its own binding) inside its body, but
that name is never declared in the scope the body is emitted into.

## Expected

The `letrec` binding is in scope inside its own body -- that is the defining
difference between `letrec` and `let` -- and the emitted C should name the
lifted function/closure consistently so the self-call resolves. The program
should build and run (it sums the Vec).

## Workaround (in landed spice code)

Hoist the loop to a top-level recursive `defn` and thread the captured values
(`rs`, `n`) as explicit parameters; top-level self-recursion emits correctly.
This is what the plot spice does -- `__renderers-bbox-go` is a top-level `defn`
specifically to avoid this, per the comment at
`turmeric-spices` `spices/plot/src/plot/core.tur:817-819`:

> Iterate with a top-level recursive `defn` (see __renderers-bbox-go), not a
> `letrec`-bound closure: a self-recursive `letrec`/`fn` currently mis-emits
> its own binding in C ('go' undeclared).

The hoist could be folded back into a local `letrec` once codegen names the
self-reference correctly.

## Notes / scope

- Independent of existentials / Vec -- reproduces with a plain `(Vec int)`
  accumulator (above). The Vec is incidental; the trigger is the
  `letrec` + self-recursive `fn` shape.
- Type-checking passes; the failure is purely in codegen / closure lifting --
  the lifted closure's own binding name is not declared in its emission scope.
- Fix direction: ensure the lifted name for a `letrec`-bound closure is
  declared (forward-declared if necessary) in the scope its body is emitted
  into, so the self-call references a live C symbol.
