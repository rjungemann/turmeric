# A thin fn cell re-pointed at a CAPTURING closure segfaults when called

**Severity: medium.** `tur check` and `tur build` both pass; the program
segfaults at the call. Local and global cells alike. Found 2026-09-19 while
closing [global-mut-cell-of-byvalue-adt-or-fn-emits-bad-c](../archive/global-mut-cell-of-byvalue-adt-or-fn-emits-bad-c.md),
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
