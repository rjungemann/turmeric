---
status: open
severity: low
discovered: 2026-07-07
discovered-by: catch-unwind-result-box-leak follow-up
area: compiled backend / runtime (catch-unwind, closures)
---

# `catch-unwind` still leaks its thunk fat-closure (and a let-bound caught Result box) per call

## Summary

Two residual per-catch-site leaks remain after the result-box / payload leak was
fixed (`docs/archive/catch-unwind-result-box-leak.md`):

1. **Thunk fat-closure (16 bytes / catch).** `(catch-unwind (fn [] ...))`
   materializes the `(fn ...)` argument into a 2-slot fat closure
   (`malloc(2 * sizeof(int64_t))` at the call site) that is never freed. It is
   `definitely lost` on every catch site, so a catch in a loop leaks unboundedly
   (16 KB over 1000 iterations). This is what valgrind actually reports for the
   discard repros -- the box itself is DCE'd when discarded.

2. **Let-bound caught Result box.** The statement-position fix only frees a
   caught Result whose value is *provably discarded*. A `let`-bound Result that
   is used (`ok?`/`ok-val`/`err-val`) and then goes out of scope without
   escaping is not freed -- the 24-byte `tur_result_box_t` (and, on the err
   branch, the 32-byte payload record) leaks once its last use is past.

## Minimal repro

```turmeric
(defn main [] : int
  (catch-unwind (fn [] : int 5))   ; result discarded
  0)
;; ==> definitely lost: 16 bytes in 1 blocks  (the fat-closure thunk)
```

```turmeric
(defn main [] : int
  (let [r (catch-unwind (fn [] : int 5))]
    (if (ok? r) (ok-val r) 0)))
;; r used then dropped at let-scope end -> box (24 B) leaks
```

## Root cause

1. The thunk is lowered as an ordinary fat closure argument
   (`emit_expr.c` `EX_CATCH_UNWIND`, via the closure/`FN_TO_FAT` path). Nothing
   frees it after `tur_catch_unwind_box` consumes it. This is really the general
   "single-use closure argument is never freed" closure-lifetime gap, not
   specific to `catch-unwind` -- but `catch-unwind`'s thunk is the common case
   where it bites, because the thunk is always fresh and never escapes the call.

2. Freeing a *used* let-bound box needs the escape/last-use check the original
   report flagged as an open design question, so a returned/stored Result is not
   freed early.

## Fix directions

- **Thunk:** free the thunk fat-closure after the catch when the argument is a
  fresh closure literal (`EX_FN` / `EX_CLOSURE` / `EX_FN_TO_FAT`), not a bare
  variable (freeing `(catch-unwind my-shared-thunk)` would free a closure the
  caller still owns). A capturing thunk also has a heap env to free, so this
  wants the general closure-drop machinery rather than an ad-hoc `free` -- prefer
  solving it as part of single-use-closure-argument lifetime, not a
  `catch-unwind` special case.
- **Let-bound box:** the escape/last-use analysis from the original report --
  free the box at the end of the binding's scope when it provably does not
  escape.

Both are bounded per catch site and long-standing (not regressions); severity
low. See the resolved parent report for the box/payload half that already
landed.
