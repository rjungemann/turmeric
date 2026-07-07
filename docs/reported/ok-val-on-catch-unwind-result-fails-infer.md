# `ok-val` / `err-val` on a `catch-unwind` result fails to infer

**Severity: medium** (blocks value extraction from a caught result; the boolean
predicates `ok?`/`err?` work, so callers can branch but not read the value).

## Summary

`(ok-val r)` / `(err-val r)`, where `r` is the result of `(catch-unwind ...)`,
fails elaboration with TUR-E0001: the accessor expects
`(Result A B)` but sees `int`. The predicates `(ok? r)` / `(err? r)` on the same
`r` typecheck fine. So a `catch-unwind` result can be *branched on* but its
`ok`/`err` payload cannot be *extracted* through the normal accessors.

## Minimal repro

```turmeric
(defn main [] : int
  (let [r (catch-unwind (fn [] : int 42))]
    (if (ok? r) (ok-val r) 0)))   ;; <- (ok-val r) errors; (ok? r) is fine
```

```
error [TUR-E0001]: function 'ok-val' arg 1: expected
  (type-app (type-app Result tyvar 'A') tyvar 'B'), got int
```

Removing the `(ok-val r)` (e.g. `(if (ok? r) 1 0)`) compiles cleanly, as does
every existing `panic-catch-unwind-*` fixture -- none of them extract the value,
they only predicate on `ok?`/`err?`.

## Likely root cause (not pinpointed)

`catch-unwind`'s elaborated result type appears to be the bare `int` **carrier**
of the result box rather than a `Result<int, Panic>` type application. `ok?` /
`err?` tolerate the carrier (they likely dispatch on the carrier int directly),
but `ok-val` / `err-val` are declared over `(Result A B)` and reject the `int`.
The fix is to give `(catch-unwind thunk)` the surface type
`Result<ThunkResult, Panic>` (or to make the accessors accept the carrier the
same way the predicates do). Start at the `EX_CATCH_UNWIND` result-type
assignment in the elaborator and the `ok-val`/`err-val` signatures.

## Why it matters now

The general stackless catch-unwind lowering
([compiled-catch-unwind-general-lowering-plan.md](../upcoming/compiled-catch-unwind-general-lowering-plan.md),
phase G5) wants `AFTER` to consume the caught value. There is nothing to test
that against until this infers, so this is a prerequisite for G5's value
extraction. It is a pure language/elaboration gap -- independent of the compiled
backend or the experiment flags.
