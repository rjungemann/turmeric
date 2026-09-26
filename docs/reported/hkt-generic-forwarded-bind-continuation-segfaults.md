# A higher-kinded generic that forwards a continuation to `bind` segfaults

**Severity:** high -- a well-typed program crashes on the compiled path;
`tur --interpret` prints the right answer. Found 2026-09-25 while writing the
SC8b fixtures for typeclass-superclasses-plan; reproduces on the compiler
before that work, and with every constraint spelled out.

## Repro

```turmeric
(defn chain [^Monad M] [m : (M int) k : (fn [int] (M int))] : (M int)
  (bind m k))

(defn main [] : int
  (let [inc (fn [x : int] : (Option int) (some (+ x 1)))]
    (println (match (chain (some 41) inc) (Some v) v (None) -1)))
  0)
```

```
$ tur run repro.tur          # Segmentation fault
$ tur --interpret repro.tur  # 42
```

The same happens at `Result`. Writing the continuation as a lambda inside the
generic works:

```turmeric
(defn add-one [^Monad M ^Applicative M] [m : (M int)] : (M int)
  (bind m (fn [x : int] : (M int) (pure (+ x 1)))))
```

## Where to look

The generic is compiled by dictionary passing (`chain__dict_N`). The
difference between the two shapes is only where the continuation comes from:
a parameter typed `(fn [int] (M int))` versus a lambda built in the body. The
likely fault is the representation of the forwarded function value -- a
`tur_poly_fn_t` or fat handle on one side and what the `Monad` dictionary's
`bind` slot expects on the other. Start from the emitted C of the repro and
compare the continuation argument at the `bind` call with the lambda shape's.

## Fix directions

Make a forwarded function parameter reach the dictionary's `bind` in the same
representation a body lambda does. Then pin both shapes, at `Option` and
`Result`, under `run.sh` and `run-turi.sh`.
