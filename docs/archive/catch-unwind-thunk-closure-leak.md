---
status: resolved
severity: low
discovered: 2026-07-07
discovered-by: catch-unwind-result-box-leak follow-up
resolved: 2026-07-07
area: compiled backend / runtime (catch-unwind, closures)
---

# `catch-unwind` still leaks its thunk fat-closure (and a let-bound caught Result box) per call

## Resolution (2026-07-07)

Both residual leaks are now closed on the native path.

**1. Thunk fat-closure.** `emit_expr.c` `EX_CATCH_UNWIND` / `EX_CATCH_PANIC_OF`
now `free`s the thunk fat box after `tur_catch_unwind_box` has consumed it, but
only when the thunk argument is a closure literal this call site owns -- an
`EX_CLOSURE`, an auto-shimmed bare fn (`EX_FN_TO_FAT`), or a boxed poly method
(`EX_POLY_TO_FAT`).  A bare-variable thunk (`(catch-unwind my-shared-thunk)`) is
left untouched: it may alias a closure the caller still owns.  `catch_thunk_owns_fat_box`
gates this.  The thunk is invoked synchronously and never stored in the result
box, so the free is not premature.  For `catch-panic-of` the free is emitted
before the re-raise check, so it runs on both the caught and the propagate
paths.  Because statement-position `catch-unwind` flows through the same
`emit_value` dispatch, the discard case is covered too.

**2. Let-bound caught Result box.** `emit_let_value` now `tur_result_box_free`s a
let-bound caught Result at scope exit when it provably does not escape, using a
box-aware escape analysis (`catch_box_binding_escapes`, a mode of the shared
`binding_escapes_impl`).  The analysis whitelists reads of the box through the
read-only accessors `ok?` / `err?` / `ok-val` -- none of which retain the box or
hand back box-owned-and-freed memory -- and treats every other use (return,
store, capture, `err-val`, an unknown call) as an escape that disables the free.
`err-val` is deliberately excluded: it returns the box's panic-payload pointer,
which `tur_result_box_free` frees, so a caught payload extracted via `err-val`
could dangle.  A let whose caught Result is inspected only with `ok?`/`err?`/
`ok-val` and then dropped is now fully reclaimed (box + err payload).

Measured on the report's repros (valgrind): the discard case and the
`(let [r ...] (if (ok? r) (ok-val r) 0))` case both drop from `definitely lost`
to `0 bytes in use at exit`, with no `Invalid read/write` (no use-after-free).

### Remaining (deliberate, sound tradeoff)

A let-bound caught Result inspected via `err-val` is still not freed at scope
exit -- freeing the box also frees the panic payload the extracted `err-val`
pointer aliases, so the conservative analysis leaves it to leak rather than risk
a use-after-free.  This is bounded per catch site and rare (the common inspect
paths are `ok?`/`ok-val`).  The stackless-catch-unwind lowering keeps its own
residual aggregate-box leak, tracked in
`docs/upcoming/catch-unwind-aggregate-followups-plan.md` (Part B); the native
frees added here do reach a stackless fixture's outer native catch, verified
UAF-free.

---


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
