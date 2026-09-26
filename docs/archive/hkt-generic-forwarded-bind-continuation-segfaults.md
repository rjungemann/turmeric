# A higher-kinded generic that forwards a continuation to `bind` segfaults

> **RESOLVED 2026-09-25.** See [Resolution](#resolution) at the end. The
> analysis below is the original filing.

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

## Resolution

The continuation's declared type returns the erased `(M int)`, so every
consumer calls it through the carrier cast and reads its result as a pointer
to a boxed value. A function returning the by-value `(Option int)` answered in
registers, and `bind` dereferenced the tag. The fix is on the producer side,
at the call, which alone knows the function's real result type -- inside the
generic, other callers may legitimately pass carrier-returning closures into
the same slot.

- `sink_fn_result_is_hkt_erased` (`src/compiler/elab_call.c`) recognizes a
  parameter whose declared fn type returns a tyvar-headed application, and
  marks the argument's `EX_FN_TO_FAT` node `erased_result`.
- For a bare function, the emitter puts `ensure_boxres_fatshim`
  (`src/compiler/emit_module.c`) in slot 0: it calls the function through its
  real aggregate signature, heap-boxes the result as a constructor's carrier
  box is, notes its words for regions, and returns the box pointer.
- A capturing closure is already a fat handle, so it is wrapped
  (`inner_is_fat`): a `{ boxres shim, handle }` box whose shim fat-calls the
  handle's own slot 0.
- Only signatures whose parameters are all the `int64_t` carrier word are
  admitted; anything else keeps its previous shim.

Pinned by `tests/fixtures/hkt-generic-forwarded-continuation` (a top-level
function, a lambda and a capturing closure, at `Option` and `Result`,
including the short-circuit arms), under both harnesses. No snapshot moved.

Found while fixing this, and separate: a constrained generic that calls
another constrained generic does not compile (the inner call resolves to a
by-value spec whose aggregate is returned where the carrier is expected) --
the case `chain-twice` would have covered.
