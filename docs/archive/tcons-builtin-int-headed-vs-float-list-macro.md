---
title: `tcons` builtin int-headed while `(list ...)` builds float lists fine
category: Defect (expressiveness inconsistency)
severity: low
status: RESOLVED
discovered: 2026-06-21
resolved: 2026-06-22
found-by: turmeric-spices linalg U4 paydown (formerly "workaround #4" in
          docs/linalg-v0.21.0-rearchitecture-blocker.md)
verified-on: turmeric 0.22.0, main @ 97dcd86 (build-release)
fix-commit: stdlib/list.tur -- tcons made element-generic
related:
  - docs/archive/list-macro-tcons-int-headed-no-float-list-literal.md
  - docs/archive/history/cons-builtin-rejects-cstr-head.md (the `cons` builtin sibling)
---

## Summary

`tcons` was a hardcoded int-headed `defn` (`[h : int t : int] : int`, inline-C
cell builder), so `(tcons 1.5 (tnil))` was rejected at elaboration even though
`(list 1.5 2.5 3.5)` -- the intended way to build a list -- already builds and
round-trips a float list. The raw cons constructor lagged the macro it backs.

```turmeric
(tcons 1.5 (tnil))
;; before: error [TUR-E0001]: function 'tcons' arg 1: expected int, got float
;;         help: ... consider tuple2..tuple5 instead of (list ...)
```

Two problems, both now fixed:

1. **Inconsistency.** `(list 1.5 ...)` succeeded but `(tcons 1.5 (tnil))`
   failed. The element-type handling `(list ...)` gained (it routes through the
   polymorphic-head `tcons-of`) did not apply to `tcons`.
2. **Misleading diagnostic.** The `tcons` error pointed at `tuple2..tuple5`
   "for heterogeneous fixed-arity collections" -- but the user was building a
   *homogeneous* float list, which `(list ...)` supports. With `tcons` now
   accepting a float head, the misdirected hint no longer fires for this case.

### A note on the report's `tcons-of` example

The original report also cited `(tcons-of 1.5 2.5)` failing with `arg 2:
expected int, got float`. That is `tcons-of`'s *tail* slot, which is the `:int`
carrier by design (pass `0` or another cell pointer, not a value). `tcons-of`'s
*head* was already polymorphic; `(tcons-of 1.5 0)` always type-checked. The
residual bug was specifically `tcons`'s int-only head.

## Root cause

`stdlib/list.tur` -- `tcons` was:

```turmeric
(defn tcons [h : int t : int] : int
  ```c
  struct { int64_t head; int64_t tail; } *cell = malloc(sizeof(*cell));
  cell->head = h; cell->tail = t; return (int64_t)(intptr_t)cell;
  ```)
```

The inline-C `cell->head = h` fixes the head to `int64_t`, and the parameter
type `h : int` makes the elaborator reject any non-int head. `tcons-of`
(`[A] [h :A t :int] : (Cons A)`, body `(make-struct Cons ...)`) already lays
the head slot out at A's concrete C type via the typed-slots machinery, and
`(list ...)` right-folds through `tcons-of`.

## Fix

Make `tcons` element-generic with the same body shape as `tcons-of`:

```turmeric
(defn tcons [A] [h : A t : int] : (Cons A) (make-struct Cons :head h :tail t))
```

`make-struct` lowering keys off the function's declared return type, so the
return had to become `(Cons A)` (it cannot stay `:int` -- a `:int` return makes
the struct literal lower as `(int64_t){.head=...}`, a C error). Two
carrier-level helpers in `list.tur` that previously relied on `tcons` returning
the `:int` carrier (`__cons-fmap`, `list-concat`) now bind the `(Cons A)` result
to an `:int` local so the surrounding `if`'s branches share the carrier type and
the emitted C stays `-Wint-conversion`-clean:

```turmeric
(let [out (:: (tcons ...) :int)] out)
```

### Why not delegate `(tcons-of h t)` directly?

`tcons`'s body cannot just call `(tcons-of h t)`: calling the curried generic
`tcons-of` with a *bare type variable* head (`h : A`, A still abstract inside
`tcons`) collapses the partial application to the carrier and mis-parses as a
curried over-application -- `TUR-E0002: function 'tcons-of' returns int, which
is not callable`. Inlining the `make-struct` body sidesteps that limitation.
(Left as a separate, lower-priority compiler note: generic-to-generic curried
calls through a bare tyvar should resolve to the saturated form.)

## Verification

```turmeric
(tcons 1.5 (tcons 2.5 (tnil)))   ;; head reads back as 1.5, 2.5 (double slot)
(tcons 1 (tcons 2 (tnil)))       ;; int heads unchanged
(list 1.5 2.5 3.5)               ;; macro interop unchanged
```

- The float-headed cons list type-checks and round-trips.
- Int lists, `(list ...)`, `car`/`cdr`/`null?`/`length`, and the NonEmpty
  helpers in `refined.tur` are unaffected.
- Codegen for `tcons` shifted (generic `Cons` spec instead of an inline-C
  function), so the 85 affected fixture snapshots were regenerated in the same
  change.
- `bash tests/run.sh`: **1748 passed, 0 failed**.

The linalg blocker doc's "workaround #4" can be marked fully resolved: the cons
builtins now accept a float head, matching `(list ...)`.
