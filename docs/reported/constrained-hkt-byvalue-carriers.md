---
status: open
severity: medium
discovered: 2026-07-29
area: compiler (constrained HKT dispatch / poly carrier)
---

# Constrained HKT polymorphism rejects by-value carriers

## Summary

Mode-B dictionary passing works: a constrained kind-polymorphic function
(`[^m] [^Monad m x : (m int)]`) is compiled once and dispatches through a
dictionary the caller resolves. One gap remains for everyday use: **the poly
carrier is one machine word**, so the abstract `m` must be an int-carrier
`defopaque`. A by-value ADT carrier -- which is what the stdlib `Option` and
`Result` are -- segfaults through it.

That rules out the two monads a user is most likely to reach for, so the feature
currently only abstracts over hand-rolled int-carrier constructors.

> **Gap 1 (`pure`/`empty` on the abstract `m`) is fixed** as of 2026-07-29 --
> return-directed methods now resolve against the constraint. See
> [../archive/history/constrained-hkt-pure-return-dispatch.md](../archive/history/constrained-hkt-pure-return-dispatch.md).
> Fixtures: `hkt-constrained-pure-return-dispatch`,
> `hkt-constrained-pure-two-instances`.

## Repro

    $ cat > /tmp/w.tur <<'EOF'
    (defn dbl [v : int] : int (* v 2))
    (defn poly-bind [^m] [^Monad m x : (m int)] : (m int)
      (bind x (fn [v] (some (dbl v)))))
    (defn use-opt [g (forall [(m :: * -> *)] [(Monad m)] (-> (m int) (m int)))] : int
      (unwrap-or (g (some 5)) -1))
    (defn main [] : int (println (use-opt poly-bind)) 0)
    EOF
    $ ./build/tur run /tmp/w.tur
    Segmentation fault

Expected: `10`. The identical program with an int-carrier `defopaque` in place
of `Option` prints `10` -- see `tests/fixtures/hkt-constrained-pure-two-instances`
for the working shape.

## Root cause

The poly carrier slot is `int64_t`. An int-carrier `defopaque` rides it exactly;
a by-value ADT (`tur_adt_Option__int`, a real struct) does not, and is
reinterpreted rather than converted at the crossing. This is the same
carrier-vs-by-value confusion behind
[result-monad-bind-typed-boundary-miscompiles.md](result-monad-bind-typed-boundary-miscompiles.md),
reached by a different route.

Note the van Laarhoven **lens** boundary already solved this shape -- see
`docs/archive/history/van-laarhoven-functor-must-be-int-carrier.md`, resolved
2026-07-04 by boxing at the lens crossings (Path A), with a zero-overhead
monomorphizing Path B under `--enable=vl-wide-mono`. That work did not
generalize to plain constrained calls, and its boxing is the obvious thing to
reuse here.

## Fix directions

1. Reuse the van Laarhoven Path A boxing at the general poly-carrier crossing.
2. Or extend the `__spec__` monomorphization so a by-value carrier is
   specialized rather than squeezed through the word-sized carrier -- closer to
   Path B, and it would also fix the `Result`-through-`do-m` report above.
3. Fixture: `hkt-constrained-pure-two-instances` instantiated at a by-value ADT
   carrier instead of the two `defopaque`s.

## Also: the middle-vector spelling miscompiles

`(defn f [^m] [(Monad m)] [ma : (m int)] ...)` -- the `definstance`-style
constraint vector rather than the in-parameter `^Monad m` -- elaborates and
reaches monomorphization, then emits bad C:

    error: incompatible types when assigning to type 'int64_t' from type 'tur_adt_Option__int'
    (in dup__spec__tur_adt_Option__int_tur_adt_Option__int)

Two accepted spellings for the same thing, only one of which works, is worth
resolving one way or the other -- either make the middle vector work or reject
it with a diagnostic pointing at `^Monad m`. (This is the same by-value carrier
fault as above, surfacing at the specialization seam rather than at runtime.)
