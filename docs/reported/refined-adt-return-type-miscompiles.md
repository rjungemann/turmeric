# A refined ADT return type miscompiles (returns the aggregate where the carrier is expected)

**Severity:** medium -- any `defn` whose declared RESULT is `#refine{ r : <ADT> | ... }`
fails to build; the emitted C does not compile. Parameter refinements over an ADT
(`[xs : #refine{ v : Lst | ... }]`) are fine; only the return position is affected.
Found 2026-09-30 while writing the reflected-measures RF4 fixture; unrelated to
`^reflect` (reproduces with no experiment on).

## Repro

```turmeric
(defdata Lst [] (Cons [hd : int tl : Lst]) (Nil))
(defn same [xs : Lst] : #refine{ r : Lst | true } xs)
(defn main [] : int
  (match (same (Cons 7 (Nil)))
    (Nil) (println 0)
    (Cons h _) (println h))
  0)
```

```
$ tur build repro.tur -o repro
.../repro_tur.c:8244:16: error: incompatible types when returning type 'tur_adt_Lst' but 'int64_t' {aka 'long int'} was expected
.../repro_tur.c:8271:32: error: invalid initializer
```

Replace the result annotation with a plain `: Lst` and it builds and prints `7`.

## Root cause (suspected)

The contract type is peeled to its base for the checker (the predicate is
enforced on the value), but the emitted C signature of the function takes the
peeled type's CARRIER (`int64_t`) while the body is emitted as the by-value
aggregate (`tur_adt_Lst`) -- the two halves of the return path disagree about
whether a refined ADT result is by-value or boxed. Compare how a plain `: Lst`
result decides representation (emit_fns / the by-value HKT path) with what the
refinement peel hands the same code. `TUR_W0380`'s neighbourhood (contract type
arg peeling) is the analogous place for type ARGUMENTS; this is the result slot.

## Fix directions

- Peel the refinement for representation purposes at the same point the
  plain result type is classified, so `#refine{ r : Lst | p }` and `: Lst`
  emit the identical signature and return sequence; the runtime check then
  wraps the aggregate value, not a carrier word.
- Add a fixture: a refined ADT result that is matched by the caller
  (`refine-adt-return`), plus the `--no-contracts` variant.
