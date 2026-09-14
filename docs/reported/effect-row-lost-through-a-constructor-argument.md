# An effectful call as a constructor argument loses its effect row

**Severity: high.** `(Box (g))` where `g` performs: the caller's inferred effect
row comes back empty, the enclosing handler is reported unreachable
(TUR-W0033), and the program aborts `tur: unhandled effect` at run time.

**Not Saffron-specific** -- this reproduces in plain typed Turmeric with every
signature annotated, which is what separates it from
[saffron-effect-row-lost-through-unannotated-call](../archive/saffron-effect-row-lost-through-unannotated-call.md)
(resolved). Found while fixing that one, as the last shape that still failed
after it.

## Repro -- plain Turmeric

```turmeric
(defeffect Ask [] : int)
(defstruct Box [v : int])
(defn g  [] : int (perform (Ask)))
(defn mk [] : int (.v (Box (g))))
(defn main [] : int
  (println (handle (mk) (Ask [] k) (resume k 41)))
  0)
```

```
warning [TUR-W0033]: handler clause for 'Ask' is unreachable: the body does not perform 'Ask'
tur: unhandled effect (tag 2)
Aborted
```

The same call one level out of the constructor -- `(let [n (g)] (.v (Box n)))`
-- is expected to work; confirm before assuming, since that is the shape any fix
has to preserve.

## Where to look

`collect_effects_in_expr` (src/passes/effect_check.c) HAS an `EX_MAKE_STRUCT`
arm that walks `field_values`, so the row walk is not the obvious suspect the
way it was for the Saffron nodes. Check first whether the constructor call is
an `EX_MAKE_STRUCT` at all here or an `EX_CALL` to a generated constructor
binding whose own FnDef carries no row -- `cps_collect_calls` treats a
CONSTRUCTOR call as a leaf deliberately
(docs/archive/history/cps-coloring-overcolors-nonnode-calls.md), and that
exemption reads as a candidate: it is correct for the constructor's own body,
which invokes nothing, but the ARGUMENT is an ordinary expression and its call
still needs an edge.

If that is it, the fix is to keep the leaf exemption for the callee while still
descending the argument list -- which the EX_CALL arm already does below the
exemption, so read carefully before changing it.

## Why it was not fixed alongside its Saffron twin

That fix was two things: missing traversal arms for nodes that did not exist
when the walks were written, and an elaboration hoist gated on
`unit_has_user_effect`. Neither applies here -- the nodes are old and the
dialects share this behaviour -- so this is a different defect that happens to
present identically, and lumping them would have hidden that it affects typed
code too.
