# A thin fn cell re-pointed at a CAPTURING closure segfaults when called

**RESOLVED 2026-09-19, by direction 1.** A `^mut` fn cell is fat from the
start: its `let` / `def` init is shimmed to the fat representation through
the same helper the `any` widen uses (`elab_fn_value_to_fat`; a link-time
constant for a lifted lambda, so no allocation) and the binding is marked
boxed, so every call dispatches through slot 0 -- a boxed GLOBAL now joins
the fat-dispatch branch in emit_expr.c, which was gated on non-global
bindings -- and every later `set!` normalises a thin value the same way. A
defensive diagnostic covers a fat value stored into a cell that stayed thin;
measured, that shape does not arise -- every `^mut` fn cell is fat after the
init rule except one initialised from a fn parameter, and such a parameter is
carrier-typed (`ptr<void>`), so it takes the separately filed alias gap
below rather than this branch. Pinned by
`tests/fixtures/fn-cell-capturing-closure` (local and global cells, thin and
capturing inits and stores, a `def` from a closure-returning call).

**Found and fixed alongside -- a use-after-free the report's own repro
exposed once it compiled:** the global closure captured a match binder `t`
of the by-value recursive local `xs`, and `xs` was freed at scope exit
anyway (the second line printed 1 for a 2-link tail; ASan heap-use-after-
free). The scope-exit spine drop consulted only the local's own moved /
consumed state, never where its aliases went. It now asks the strict
alias-aware walk the parameter inference uses (`localowned_binding_is_
confined`, emit_core.c): an alias reaching a store, a closure, a let, a
return or an unproven callee refuses the drop, and the scope's value may
carry an alias out only when it is a non-pointer scalar. Refusing costs a
leak; the old answer was a use-after-free. Pinned by
`tests/fixtures/closure-retains-match-binder-of-dropped-local`; the leak
harness (102 fixtures) is unchanged.

**Found, not fixed, filed separately:** a `let` alias of a fn PARAMETER
called through the alias (`(let [h g] (h))`) does not compile -- pre-existing,
independent of `^mut`. See
[let-alias-of-fn-param-call-undeclared](../reported/let-alias-of-fn-param-call-undeclared.md).

**Severity: medium.** `tur check` and `tur build` both pass; the program
segfaults at the call. Local and global cells alike. Found 2026-09-19 while
closing [global-mut-cell-of-byvalue-adt-or-fn-emits-bad-c](global-mut-cell-of-byvalue-adt-or-fn-emits-bad-c.md),
whose thin-fn shapes are fixed; this is the residue.

## Repro

```turmeric
(defn main [] : int
  (let [^mut f (fn [] : int 0)]
    (let [t 5] (set! f (fn [] : int (+ t 1))))
    (println (f)))
  0)
;; Segmentation fault
```

Same with a `(def ^mut g-fn (fn [] : int 0))` global: `(set! g-fn (fn [] :
int (llen t)))` inside a `let` that binds `t`, then `(g-fn)`.

## Root cause (probable)

The cell is typed `(fn [] int)` THIN from its initializer -- a non-capturing
lambda is a bare code pointer, and the binder spells the cell `int64_t (*f)()`
(a local) or, since 2026-09-19, `static R (*g)(A...)` (a global). The `set!`
stores a CAPTURING closure, which is a fat `{ thunk, env... }` box; the call
then jumps into the box as code. The elaborator accepts the store because the
closure's declared fn type equals the cell's; nothing records that the VALUE
is boxed while the CELL is thin.

## Fix directions

1. At `set!` (and at `let` when any later store in the scope is a capturing
   closure), mark the cell `boxed`/fat when a boxed closure is stored into it,
   and spell the cell as the int64 carrier with fat dispatch at every call --
   the fat-normalisation the H7-era parameter work applies to fn parameters,
   extended to mutable cells.
2. Or reject the store statically: a capturing closure into a thin cell is a
   representation change, and the diagnostic would name it.
