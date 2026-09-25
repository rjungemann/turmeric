# A `none` from a constrained generic segfaults at a typed `Option` parameter

> **RESOLVED 2026-09-25.** See [Resolution](#resolution) at the end. The
> analysis below is the original filing.

**Severity:** high -- a well-typed program crashes on the compiled path;
`tur --interpret` prints the right answer. Found 2026-09-25 while writing the
SC8b step-5 fixture for typeclass-superclasses-plan; reproduces on the
compiler before that work with every constraint spelled out.

## Repro

```turmeric
(defn add-one [^Monad M ^Applicative M] [m : (M int)] : (M int)
  (bind m (fn [x : int] : (M int) (pure (+ x 1)))))

(defn show-o [o : (Option int)] : int
  (match o (Some v) (do (println v) 0) (None) (do (println -1) 0)))

(defn main [] : int
  (show-o (add-one (:: (none) (Option int))))
  0)
```

```
$ tur run repro.tur          # Segmentation fault
$ tur --interpret repro.tur  # -1
```

All three of these work: the same call with `(some 41)` (prints `42`); the
same `none` call matched in place,
`(match (add-one (:: (none) (Option int))) (Some v) v (None) -1)`; and a
`Result` `err` passed to a typed `(Result int int)` parameter.

## Where to look

The difference from the working inline `match` is the bridge from the generic's
carrier result to the by-value `(Option int)` argument. The `none` carrier is
the one value of `Option` that is not a pointer to a box, which points at a
bridge that dereferences the carrier unconditionally. Compare the emitted C of
the typed-argument call with the inline-match form. It is likely the same
bridge as
[hkt-dict-generic-byvalue-result-to-typed-param](../reported/hkt-dict-generic-byvalue-result-to-typed-param.md),
which fails to compile for a user type rather than crashing at `none`.

## Fix directions

Make the carrier-to-by-value bridge at an argument position handle the
`none` carrier (or route it through the same conversion a `match` scrutinee
gets). Pin `none` and `some` at a typed parameter, under `run.sh` and
`run-turi.sh`; `tests/fixtures/stdlib-monad-entails-applicative` can then use a
typed `Option` consumer.

## Resolution

The dereference was in the carrier -> by-value bridge, in both of its emitters
(`emit_agg_unbox` in `src/compiler/emit_expr.c` and the pointer-carrier arm of
the carrier -> concrete conversion in `src/compiler/emit_core.c`). For a sum
whose tag-0 constructor is nullary -- `Option`'s `None` -- both now call a
per-monomorph helper, `ensure_agg_unbox_nullsafe` (`src/compiler/emit_module.c`),
that answers the zeroed value (tag 0, no payload) for a 0 carrier and
dereferences otherwise. That is the reading every `match` already gives a
NULL scrutinee. The helper is a function rather than a `?:` or `({ ... })`
for the JIT reason `ensure_any_carrier_bridge` records, and so the carrier is
evaluated once. Any other type keeps the plain dereference: a 0 carrier there
is a bug, and a crash is more honest than a zeroed value.

Pinned by `tests/fixtures/hkt-generic-none-to-typed-param` (`none` and `some`
from two generics, passed straight to a typed parameter and through a typed
`let`), under both harnesses. `stdlib-monad-entails-applicative` uses a typed
`Option` consumer again. No snapshot moved.
