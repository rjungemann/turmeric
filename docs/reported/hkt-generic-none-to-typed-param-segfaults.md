# A `none` from a constrained generic segfaults at a typed `Option` parameter

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
[hkt-dict-generic-byvalue-result-to-typed-param](hkt-dict-generic-byvalue-result-to-typed-param.md),
which fails to compile for a user type rather than crashing at `none`.

## Fix directions

Make the carrier-to-by-value bridge at an argument position handle the
`none` carrier (or route it through the same conversion a `match` scrutinee
gets). Pin `none` and `some` at a typed parameter, under `run.sh` and
`run-turi.sh`; `tests/fixtures/stdlib-monad-entails-applicative` can then use a
typed `Option` consumer.
