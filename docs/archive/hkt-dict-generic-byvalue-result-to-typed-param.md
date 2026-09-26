# A dictionary-passing generic's by-value result does not reach a typed parameter

> **RESOLVED 2026-09-26.** See [Resolution](#resolution) at the end. The
> analysis below is the original filing.

**Severity:** medium -- the program fails to compile with a C type error (no
wrong answer); `tur --interpret` prints the right answer. Found 2026-09-25
while writing the SC8b fixtures for typeclass-superclasses-plan; reproduces on
the compiler before that work, with every constraint spelled out.

## Repro

```turmeric
(defclass MyAp [^f] (mypure [x : a] : (f a)))
(defclass MyAlt [^f] (myalt [x : (f a) y : (f a)] : (f a)))
(defdata Tally :copy [A] (Tally A int))
(definstance MyAp [Tally] (mypure [x] (Tally x 0)))
(definstance MyAlt [Tally]
  (myalt [x y] (match x (Tally v n) (if (> n 0) x y))))

(defn or-default [^MyAlt F ^MyAp F] [x : (F int) d : int] : (F int)
  (myalt x (mypure d)))

(defn show-t [t : (Tally int)] : int
  (match t (Tally v n) (do (println (+ (* 100 v) n)) 0)))

(defn main [] : int
  (show-t (or-default (Tally 7 2) 5))
  0)
```

```
error: incompatible type for argument 1 of 'show_hyt'
```

Matching directly on the call works:
`(match (or-default (Tally 7 2) 5) (Tally v n) ...)` prints `702`.

A second shape looks like the same gap: feeding `fmap`'s result to `foldl` in
a `[^Functor T ^Foldable T]` generic over a user `(defdata Pair2 :copy [A]
(Pair2 A A))` is `invalid initializer` in the dictionary-passing body.

## Cause (probable)

The dictionary-passing generic returns the `int64_t` carrier. A `match` on the
call bridges the carrier to the by-value `tur_adt_Tally__int`, but an argument
position passing it to a parameter declared `(Tally int)` does not, and neither
does the let-style temp that holds `fmap`'s result before `foldl`.

## Fix directions

Apply the same carrier-to-by-value bridge at an argument position and at a
temp initializer that the `match` scrutinee already gets. Pin both shapes
under `run.sh`; `tests/fixtures/class-superclass-hkt-dict-passing` can then
use a typed consumer again.

## Resolution

Both shapes had one cause: a dict clone returns the int64 carrier, and two
consumers treated that word as if it were already the by-value aggregate.

- **Typed parameter.** The argument bridge that unboxes a carrier word for a
  by-value parameter (`src/compiler/emit_expr.c`, the SR1 "other direction"
  block) was restricted to multi-variant sums. That keeps it off by-value
  products whose crossings other rules own, and it is why `Option` worked while
  `Tally` did not. A call to a dict clone is the one argument whose C value is
  known to be the carrier, and no other rule unboxes it, so the bridge now
  admits any by-value ADT parameter for exactly that argument.
- **`fmap` into `foldl`.** An argument that is itself a method call dispatched
  through a dictionary (`emit_call_is_dict_param_dispatch`) is marked as
  already on the carrier, so the by-value seams no longer spill it into a
  concrete temp.

`tests/fixtures/class-superclass-hkt-dict-passing` uses a typed consumer again.
Pinned by `tests/fixtures/hkt-dict-generic-byvalue-result-to-typed-param`
(both shapes) under `run.sh` and `run-turi.sh`.
