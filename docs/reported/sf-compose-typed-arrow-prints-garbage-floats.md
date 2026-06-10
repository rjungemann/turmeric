---
title: sf-compose-typed fixture prints garbage floats (`2.1e-314`) instead of `8` / `10`
severity: silent miscompile -- compiles cleanly, runs to completion, prints uninitialised bytes
status: open (pre-existing -- predates the Defect-A elab fix in this session)
discovered: 2026-06-09
discovered-in: tests/fixtures/sf-compose-typed -- "stdout mismatch" failure in run.sh
---

# `>>>` arrow combinator over typed float SFs prints uninitialised floats

## Summary

`tests/fixtures/sf-compose-typed` composes `add-1` and `scale-2` via
`stdlib/arrow.tur`'s generic typed `>>>` combinator and samples the result
twice.  The expected outputs are `8` and `10`; the actual outputs are
`2.12355e-314` (or similar subnormal garbage) on both samples.

This fixture is in `tests/run.sh`'s sweep -- it shows up as a `stdout
mismatch` FAIL.  The failure predates the session's Defect-A
(`param_poly_types` for `^fat` TY_FN) work; reverting that fix produces the
same garbage output, so this is not a regression of any current change.

## Repro

```sh
./build/tur run tests/fixtures/sf-compose-typed/input.tur
```

Observed:

```
2.12355e-314
2.12355e-314
```

Expected:

```
8
10
```

## Repro source (verbatim from the fixture)

```turmeric
(load "stdlib/arrow.tur")

(defn scale-2 [x : float] : float (* x 2.0))
(defn add-1   [x : float] : float (+ x 1.0))
(defn call-f [^fat f :(fn [:float] #{} :float) x : float] : float (f x))

(defn main [] : int
  (let [h1 (>>> add-1 scale-2)]
    (println (call-f h1 3.0))    ;; (3.0 + 1.0) * 2.0 = 8
    (println (call-f h1 4.0)))   ;; (4.0 + 1.0) * 2.0 = 10
  0)
```

## Suspected cause

`stdlib/arrow.tur`'s `>>>` is declared with a type-variable signature:

```turmeric
(defn >>> [A B C]
  [^fat f :(fn [A] #{} B) ^fat g :(fn [B] #{} C)] : ptr<void>
  (let [fv f gv g] (fn [x : A] : C (gv (fv x)))))
```

When monomorphised to `A=B=C=float`, the inner `(fn [x : A] : C ...)` must
preserve the float carrier through the captured `fv` / `gv` invocations.  The
garbage-float symptom is exactly the signature of an XMM/integer register
mismatch -- the closure returns from `fv` via the int64 slot but `gv` reads
its float argument from XMM0, picking up whatever the caller left there.

Both the Phase-CCL/CCL3 fn-first-class plumbing and the `^fat` poly→fat
shim selection have history of exactly this class of bug; the typed
`>>>` path probably routes through a typed-shim that picks the int64 slot
because the type-variable monomorphisation does not propagate the float
result type all the way down.

This pattern -- single-file `(load ...)` of `stdlib/arrow.tur` plus a typed
generic combinator -- is the canonical "use the high-level arrow library"
shape, so this matters for the SF / signal ecosystem even though the
turmeric-spices signal spice itself sidesteps `>>>` and uses hand-rolled
composition.

## Validation

- Output of the fixture must be `8\n10\n`.
- A diagnostic dump of the lifted thunk's C return type for
  `>>>`-monomorphised-at-float should show `double` end-to-end (not
  `int64_t`).
- `bash tests/run.sh` no longer lists `sf-compose-typed` as a FAIL.

## Cross-references

- `[[defmodule-export-scoping-track]]` -- adjacent in spirit (typed `^fat`
  carrier preservation across boundaries) but a distinct root cause.
- `tests/fixtures/poly-to-fat-float-named-fn/`,
  `tests/fixtures/poly-to-fat-float-roundtrip/` -- nearby passing fixtures
  that probably miss whatever monomorphisation path `>>>` hits.
- `docs/reported/fn-first-class-float-carrier-gap.md` -- prior known shape
  of this register-class mismatch class of bug.
