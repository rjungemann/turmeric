# The miniKanren example contains no miniKanren, and `stdlib/logic.tur` has no example coverage

**Severity:** low as a docs/accuracy issue, medium as a coverage gap -- it is
why the workload behind the ADT-allocation report has nothing exercising it.

**Status:** OPEN.

Found while censusing construction sites for
[SR0](../upcoming/sum-representation-plan.md).

## What the example actually does

`examples/minikanren/src/main.tur` advertises itself as:

```turmeric
;; miniKanren-style relational example
;;
;; This example demonstrates a tiny relational program with:
;;   - relation predicates (`parento`, `grandparento`)
;;   - bidirectional querying (find parents, children, and pairs)
;;   - a simple "logic style" workflow in plain Turmeric
```

The implementation is a hardcoded fact table tested with `cond`:

```turmeric
(defn parento [parent : int child : int] : int
  (cond
    (and (= parent 0) (= child 1)) 1  ;; abe -> homer
    (and (= parent 5) (= child 1)) 1  ;; mona -> homer
    ...
    :else 0))
```

and "querying" is a nested loop over `PERSON_COUNT` calling that predicate.

None of miniKanren is present: no logic variables, no unification, no
substitution, no fresh, no streams, no backtracking, no `conde`. "Bidirectional
querying" is enumerating a 6x6 space and filtering. The word "logic" appears
exactly once in the file, in the comment above.

That is a fine *program*; it is not a miniKanren, and a reader who opens it
expecting to learn how relational programming works in Turmeric will not find
out.

## The coverage gap that matters more

Turmeric ships `stdlib/logic.tur`, which is the real thing -- `Term`
(`TInt`/`TVar`/`TPair`/`TNil`), `Subst`, `UnifyResult`, `Lookup`, `Stream`, and
unification over them. **The example named after it does not import it**, and
no other example does either. Its only users are eight `logic-*` fixtures
(`logic-unify-basic`, `logic-query`, `logic-fresh`, ...), each a handful of
lines.

This is load-bearing for the allocation work.
[multi-variant-adts-always-heap-allocate](multi-variant-adts-always-heap-allocate.md)
measured ~85% of executed instructions inside `malloc` on `logic.tur`'s shapes,
and `Term`/`Subst`/`Stream` are the recursive sums that
[the SR plan](../upcoming/sum-representation-plan.md) prices at 1.8x/18x. Every
one of those numbers comes from **one synthetic benchmark**
(`benchmarks/bench-logic-subst.tur`) written for the purpose. There is no real
program in the tree that exercises the hot path the plan is built around, which
is a large part of why SR0's census could not close its gate.

## Fix directions

1. **Rewrite the example against `stdlib/logic.tur`** -- actual `fresh`,
   `unify`, `conde`, and a stream-driven query. This fixes the naming problem
   and creates the missing workload in one move, and it is the option that
   makes the SR plan's numbers checkable against something real.
2. **Rename the current example** to what it is (`relational-facts`,
   `fact-table-query`) and add a separate logic-programming example.
3. **At minimum**, correct the header comment so it does not claim to
   demonstrate a "logic style workflow" it does not implement.

Note the example does not currently run at all, for an unrelated reason --
see [dash-main-entry-point-never-invoked](dash-main-entry-point-never-invoked.md).
Fixing that first is what makes any of the above observable.
