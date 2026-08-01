---
status: open
severity: medium
area: compiler (HKT method result typing / continuation-wrapper ABI pairing)
---

# Chaining two `bind`s over `Result` segfaults; one `bind` is fine

## Summary

`result-monad-bind-typed-boundary-miscompiles` (archived, RESOLVED) fixed a
**single** `bind` over `(Result A B)` crossing a typed boundary. Chaining a
second `bind` inside the first one's continuation still segfaults at runtime.
It checks clean (`tur check` exits 0) and compiles clean; only the run dies.

This matters for the docs because `do-m` desugars to nested `.bind`, so any
`do-m` over `Result` with **two or more** binds crashes. `Option` is
unaffected at both depths, so the "`Option` and `Result` behave identically at
the same call shape" bar from the parent report is met at depth 1 and not at
depth 2. It is why `docs/guides/effects-vs-monads.md` still ships a `do-m`
example over `Option` with no `Result` counterpart.

## Repro

    ;; SEGFAULT -- two binds
    (defn half-r [x : int] : (Result int int)
      (if (= x (* 2 (/ x 2))) (ok (/ x 2)) (err 1)))
    (defn quarter-sum-r [x : int] : (Result int int)
      (do-m a (half-r x)
            b (half-r a)
            (ok (+ a b))))
    (defn main [] : int
      (let [r (quarter-sum-r 20)] (println (if (ok? r) (ok-val r) -1)))
      0)

    $ ./build/tur check q.tur   # exit 0
    $ ./build/tur run q.tur     # exit 139, no output

The explicit nested-`bind` spelling fails identically:

    (bind (half-r x) (fn [a] (bind (half-r a) (fn [b] (ok (+ a b))))))

## What narrows it

| Shape | Result |
| --- | --- |
| one `bind`, typed `(Result int int)` defn boundary | works (`20`) |
| one `bind`, typed `(Result int cstr)` defn boundary | works (`20`) |
| two `bind`s, one per `defn` (sequential, not nested) | works (`21`) |
| two `bind`s nested, defn boundary | **SIGSEGV** |
| two `bind`s nested, `(:: ... (Result int int))` ascription, no defn | **SIGSEGV** |
| two `bind`s nested over `(Option int)` (the guide's `do-m` example) | works (`15`, `-1`) |

So it is the **nesting**, not the boundary form and not the error type:
a `bind` appearing inside another `bind`'s continuation is the trigger, and
each `bind` in isolation crosses its boundary correctly.

## Likely direction

The parent report's fix pairs the continuation wrapper's return ABI with the
entry point the dispatch selects (`ctx->poly_wrap_callee_carrier`, set at
call-arg emission and consulted at the EX_POLY_WRAP spill gate). That flag
reads like it is scoped to one call-arg emission; an inner `bind` emitted
while the outer one's arg is in flight is the obvious way for the pairing to
be decided once and applied to the wrong wrapper, or clobbered. Start by
checking whether the flag needs to be saved/restored around nested
`EX_POLY_WRAP` emission rather than set globally on the emit context.

## Guide upkeep -- DELETE, do not amend

This defect is documented in a published guide. When it is fixed, the guide
text must be **removed**, not annotated. Do not leave a "this used to crash"
note behind; guides state current behavior only.

1. In [docs/guides/effects-vs-monads.md](../guides/effects-vs-monads.md),
   **delete the whole `### do-m over Result is limited to one bind` subsection**
   from "Sharp edges". It is written to be self-contained so it lifts out in one
   piece, leaving the surrounding sharp-edge list reading correctly with no
   edit to its neighbors.
2. In the same guide, add the `Result` counterpart beside the `Option`
   `quarter-sum` `do-m` example, which currently stands alone *because* of this
   bug. After the fix the two should sit side by side with no commentary about
   either having once been unavailable.
3. In
   [docs/guides/value-representations-guide.md](../guides/value-representations-guide.md),
   move this row from the open-cells table into the closed-cells table with a
   one-line resolution note.

Cross-check afterward: `grep -rn "one bind\|nested-bind-over-result" docs/guides/`
should return nothing.
