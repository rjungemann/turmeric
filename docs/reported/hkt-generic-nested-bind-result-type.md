# A two-binding `do-m` in a constrained generic does not compile

**Severity:** medium -- a C type error at build time, no wrong answer;
`tur --interpret` prints the right answer. Found 2026-09-25 while writing the
SC8b step-5 fixture for typeclass-superclasses-plan; reproduces on the
compiler before that work with every constraint spelled out.

## Repro

```turmeric
(defn sum2 [^Monad M ^Applicative M] [a : (M int) b : (M int)] : (M int)
  (do-m x a
        y b
        (pure (+ x y))))

(defn main [] : int
  (println (match (sum2 (some 20) (some 22)) (Some v) v (None) -1))
  0)
```

```
error: incompatible types when assigning to type 'int64_t' from type
'tur_adt_Option__int'
```

A one-binding `do-m` (`(do-m x a (pure (* x 2)))`) and a single `bind` with a
lambda both compile. The `Result` instantiation fails the same way.

## Cause (probable)

`do-m` with two bindings is a `bind` whose continuation itself calls `bind`.
The inner `bind`'s result is typed as the by-value `(Option int)` monomorph in
the dictionary-passing spec (`sum2__dict_N__spec__...`) while the temp it is
assigned to is the `int64_t` carrier. The nested call is being given the
by-value result type that only a statically resolved call should get.

## Fix directions

Keep a nested dictionary-dispatched `bind` on the carrier inside a
dictionary-passing spec, or bridge its by-value result into the carrier temp.
Pin a two- and three-binding `do-m` at `Option` and `Result` under both
harnesses.
