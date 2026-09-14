# A Saffron function that both performs and touches a vector emits uncompilable C

**Severity: high -- build breaker.** In a `#lang saffron` file, a function
containing a `perform` (so it is CPS-lowered) plus a vector element read or a
`[...]` literal emits C that the host compiler rejects: the CPS arm passes the
16-byte `tur_tagged_t` where the vector helpers take an `int64_t` carrier word.
`cc invocation failed`, so nothing runs.

Found writing `docs/guides/introducing-saffron.md`; it is why that guide's
fixture keeps its `handle` forms in helper functions rather than in `main`.

## Repro A -- element read inside a performing function

```turmeric
#lang saffron
(defeffect Ask [] : int)
(defn f [v]
  (do (perform (Ask))
      (vec-get v 0)))
(defn main []
  (println (handle (f [7.1 2.5]) (Ask [] k) (resume k 41)))
  0)
```

```
In function 'f_pf0':
error: incompatible types when assigning to type 'tur_tagged_t' from type 'int64_t'
tur: cc invocation failed (status 256)
```

## Repro B -- a literal in a function that also handles

```turmeric
#lang saffron
(defeffect Ask [] : int)
(defn work [] (+ (perform (Ask)) 1))
(defn main []
  (println (vec-len [1 2 3]))
  (println (handle (work) (Ask [] k) (resume k 41)))
  0)
```

```
In function 'main__cps':
error: incompatible type for argument 2 of 'vec_hypush_ex'
note: expected 'int64_t' but argument is of type 'tur_tagged_t'
```

Both are the same mismatch at the two ends of a vector: the push that builds
the literal and the read that takes an element back out.

## Not the whole surface

Neither `vec-len` nor a user function over a vector trips it -- `(vec-len v)`
and `(sum [1 2 3])` inside a performing function both compile and run. It is
specifically the sites that move an *element* word: `vec_hypush_ex` on the way
in, the element read on the way out.

## Fix directions

The direct emitter boxes/unboxes a Saffron element at these sites; the CPS arm
(`src/passes/cps.c` and the `__cps` / `_pf<N>` emission in
`src/compiler/emit_*`) reuses the carrier-shaped helper signature without the
adaptation. Whatever `emit_expr.c` does for the element seam on the direct path
needs to happen on the CPS path too. A fixture pair -- one performing function
reading an element, one building a literal -- belongs beside
`tests/fixtures/docs-introducing-saffron-examples`.
