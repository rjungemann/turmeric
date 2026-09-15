# An effectful call as a constructor argument loses its effect row

**RESOLVED 2026-09-15.** The filing's two leads were both wrong, and ruling
them out is what located it. `collect_effects_in_expr` was never the suspect
and never the cause: `--dump-effects` printed `mk : #{Ask}` on the failing
program, correctly, the whole time. `cps_collect_calls`'s CONSTRUCTOR leaf
exemption was not it either -- the filing already suspected it might not be
("read carefully before changing it"), and it is right: that arm descends into
the argument list below the exemption, exactly as written.

The bug was one pass over: `--dump-cps-coloring` printed `mk uncolored` beside
the correct row. **That disagreement between the two dumps is the whole
diagnosis**, and it is the cheapest first probe for anything in this family.

`(Box (g))` is an `EX_MAKE_STRUCT`, and neither walk in `src/passes/cps.c` --
`cps_directly_uses_control` (the control seed) nor `cps_collect_calls` (the
call-graph edges) -- had an arm for it, nor for the `EX_GET_FIELD` reading it
back. Each walk was written as a switch over the kinds that existed at the
time, with `default:` meaning "no children", so **every node kind added since
has been a silent hole**. This is the third filing of that one shape:
`cps-coloring-walk-has-no-arm-for-union-inject`,
`saffron-any-return-defeats-the-frame-box-rule`, and this.

So the SEED walk (`cps_directly_uses_control`) now falls back to a shared child
enumeration (`cps_visit_children`) covering every kind that carries an evaluated
operand -- safe there, because that walk only answers "is a control op in here",
so a missing arm could only ever produce a false negative.

**The EDGE walk deliberately did NOT get the same treatment**, and finding out
why is the more useful half of this fix. `cps_collect_calls` sets `has_indirect`
for an unresolved callee (CPS0.1 rule 3), so descending into a node it never
descended into before colors any function that merely calls a callback there.
Measured on `tests/fixtures/typed/result-basic`, the blanket version colored 13
more functions -- `result-map`, `option-map`, `option-eq?` and six typeclass
instances -- because every stdlib HOF calls its callback inside a `match`, which
the walk had never seen into.

And a newly colored function LEAKS: the CPS emission path does not emit the
`tur_region_free` the direct path does, so `typed/result-basic` went from clean
to 16 leaked bytes in `ctor_Result_Ok`. `tests/run.sh` cannot see that (it
compiles fixtures unsanitized and only diffs stdout); CI's `tur_leak_check` is
what caught it. So the edge walk got exactly the two arms this defect needs,
`EX_MAKE_STRUCT` and `EX_GET_FIELD`, which leave the coloring on that fixture
byte-identical to `main`. The rest is filed as
[cps-edge-walk-misses-nodes-and-colored-frames-leak](../reported/cps-edge-walk-misses-nodes-and-colored-frames-leak.md).

Coloring `mk` then exposed a second layer the filing could not have seen,
because nothing had ever got that far: a ctor call carries no `fn_binding`, so
the CPS translation took its indirect-callee arm and rejected the non-atomic
argument outright (`indirect call (non-atomic args)`). The elaboration hoist
that already binds control-bearing operands out of the Saffron dynamic nodes
(`elab_hoist_control_operands`) now covers the constructor call and the field
read too -- which is to say the fix literally produces
`(let [n (g)] (.v (Box n)))`, the spelling the filing named as the shape a fix
had to preserve.

A second measurement worth keeping: the hoist must not count a CONSTRUCTOR call
as a call needing hoisting. A ctor invokes nothing and can never reach a
`perform` (the same reasoning as `cps_collect_calls`'s ctor leaf exemption), and
counting one wrapped a pure ctor in a `let` -- which loses the `size_index`
stamped on the EX_CALL node a line above the hoist site, so TUR-E0260 silently
stopped firing on `errors/sized-cross-param-reject` and
`errors/sized-return-claim-gadt-reject`. Only on the INTERPRETED path: `tur
check` still reported it, so `run.sh` stayed green and only `run-turi.sh`
caught it.

Pinned by `tests/fixtures/effect-row-through-constructor-arg`. Blast radius:
2 of ~2995 fixtures moved, both codegen snapshots gaining the hoist's temp,
regenerated in the same change.

Two adjacent CPS-backend limits a colored constructor argument merely *reaches*
are NOT this report and are filed separately as
[colored-call-inside-match-evicts-the-cps-backend](../reported/colored-call-inside-match-evicts-the-cps-backend.md):
a `match` anywhere in a colored function, and a `handle` whose result is a
by-value defdata ADT. The measurement that separates them is that the `let`
control spelling hits both identically.

---

*Original report follows.*

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
