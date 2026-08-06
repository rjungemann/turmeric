# turi elaborates a top-level expression's subforms in global scope

**Severity:** low -- both paths still reject the program; only the diagnostic
(and, for `def`/`define`, which binding a subform creates) differs.

## Summary

For a bare top-level *expression* -- not a `defn`, not a `def`, just an
expression form sitting at file scope -- the compiled path (`tur check` /
`tur build`) elaborates the subforms in a **pushed** scope, while the
interpreted path (`tur --interpret`, `turi`) elaborates them in the **global**
scope. Anything whose meaning is selected by `e->scope == &e->global` therefore
diverges between the two engines.

## Repro

```turmeric
;; tl.tur
(if true (def x 1) 2)
```

```
$ tur check tl.tur
tl.tur:1:10: error: `def` here has nothing to scope over: a `def` in an
expression position binds a name no later form can see. ...

$ tur --interpret tl.tur
tl.tur:1:1: error: if branches have mismatched types: then=nil else=int
```

Compiled: `elab_def` sees non-global scope and emits the expression-position
diagnostic. Interpreted: `elab_def` sees global scope, happily creates the
global `x`, and returns an `EX_DEF` of type nil -- so the failure surfaces one
level up as an unrelated type mismatch, and a global got defined from inside an
`if` condition on the way.

The same shape with `define` in the condition (`(if (define x 1) x 0)`) is what
first surfaced this: the shared fixture `tests/fixtures/errors/define-bad-position`
passed under `tests/run.sh` and failed under `tests/run-turi.sh` purely because
of this scope difference.

## Root cause

`elab_def` (`src/compiler/elab_fns.c`, the `e->scope != &e->global` branch) is
correct; the disagreement is upstream, in *where each engine considers the
global scope to end*. The compiled path wraps loose top-level statements so
their subforms elaborate one scope in; the interpreter evaluates each top-level
form directly at global scope. Every `e->scope == &e->global` test inherits the
difference -- `def`/`define` position is just the one with a user-visible
diagnostic attached.

## Fix directions

Make the two engines agree on the scope a bare top-level expression's subforms
elaborate in. The compiled path's behaviour is the one to converge on: a `def`
inside an `if` condition has nothing to scope over no matter which engine runs
it, and the interpreter silently minting a global there is the worse of the two
outcomes.

A narrower alternative, if the scope boundary turns out to be load-bearing in
the interpreter: have `elab_def` reject when it is a *subform* rather than a
top-level statement, which needs a "this call is in statement position" bit
threaded from the top-level driver rather than the scope pointer alone.

## Workaround

Put the probe inside a function body, where both engines agree. That is what
`tests/fixtures/errors/define-bad-position` now does.

## Found while

Executing `docs/upcoming/def-define-consolidation-plan.md` phases D1-D3.
