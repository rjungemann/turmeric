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

## Resolution (2026-08-13)

Both engines now agree, and they agree on **rejection** -- which is the opposite
of what this report's root-cause section predicted, for a reason the report did
not have. Three corrections to the filing, in ascending order of importance.

### 1. The discriminator is the fold, not the engine

The report frames this as "compiled vs interpreted", rooted in where each engine
considers global scope to end. It is really the **synthesized-main fold**
(`elab_toplevel.c`), which rewrites loose top-level statements into
`(defn main [] : int (do <stmts> 0))` and is deliberately suppressed under
`g_interpret_mode` to keep `tur eval` / REPL value semantics.

The consequence is that the *compiled path disagrees with itself*. The fold only
runs when the file declares no `main`, so:

```
$ cat a.tur
(if true (def x 1) 2)
$ tur check a.tur
error: `def` here has nothing to scope over ...          # fold applied

$ cat b.tur
(if true (def x 1) 2)
(defn main [] : int 0)
$ tur check b.tur
error: if branches have mismatched types ...             # fold aborted
```

Same expression, same compiler, different diagnostic -- decided by an unrelated
line elsewhere in the file. "Converge on the compiled path's behaviour" was not
available as stated, because there was no single compiled behaviour to converge
on.

### 2. The divergence is accept/reject, not just wording

The report says "both paths still reject the program; only the diagnostic
differs", and that is true of its own repro only by luck: `(if true (def x 1) 2)`
has mismatched branch types, so the interpreter rejects it for an unrelated
reason. Give the branches matching types and the disagreement is total:

```
$ echo '(if true (def x 1) (def y 2))' > c.tur
$ tur check c.tur                 # error: nothing to scope over
$ tur --interpret c.tur           # accepted, silently
```

Severity was filed as low on the strength of "both reject". It was not low.

### 3. The accepting path miscompiles

This is the part that inverts the fix. Where the program is accepted, the `def`
elaborates as a **global** -- a later `main` can name it and the type checker is
content -- but codegen emits it as a **local** of the enclosing statement, so
the reference fails in the emitted C:

```
$ printf '(if true (def answer 42) (def fallback 7))\n(defn main [] : int answer)\n' > d.tur
$ tur run d.tur
d.c:7190:21: error: 'answer_1326' undeclared (first use in this function)
tur: cc invocation failed (status 256)
```

Confirmed pre-existing (reproduced on the parent commit). So the fold's
"nothing to scope over" error was the only thing standing between this shape and
a raw C compiler error -- and it stood there by accident, in one of the two
configurations. Adding a `main` to the file removed the diagnostic and restored
the miscompile.

That settles which way to converge: **reject**, on both engines, in every
configuration. The report's *narrower alternative* -- thread a statement-position
bit rather than rely on the scope pointer -- is what landed, and it turns out to
be the primary fix rather than the fallback.

### What landed

`Elab` gains `toplevel_stmt`, the file-scope form currently being elaborated.
`def_form_is_statement_position()` accepts a `def` iff it is that form, or is
reachable from it through `do` chains -- `do` is a statement sequence, and the
existing diagnostic already advertises it as a valid body, so
`(do (def x 1) (def y 2))` at file scope keeps working and is pinned by
`tests/fixtures/toplevel-def-statement-positions`.

The failure direction is deliberately loud: a missed hand-off makes a legal
`def` be *rejected*, which a fixture catches, rather than letting an illegal one
through silently. That is not hypothetical -- the first cut broke six fixtures,
each a statement list the driver did not know about, and each is now handed off
explicitly:

| Site | Statement list |
|---|---|
| `elab_toplevel.c` Pass 2 | the entry unit |
| `elab_module.c` defmodule body | a `(defmodule ...)` body |
| `elab_module.c` imported-module loop | an imported file (this is why `stdlib/math.tur` broke) |
| `elab_call.c` macro expansion | the expansion inherits the call's position, so a top-level macro emitting `(do (defn ..) (def ..))` -- the ECS `defsystem` shape -- still works |

The fold also gains a carve-out (`form_has_nested_def`): a statement containing a
nested `def` is no longer folded into `main`. This is the same remedy the
existing `?`/`return` carve-out applies, in the opposite direction -- there,
folding *suppresses* a correct rejection; here it *created* a spurious one. It is
load-bearing for message consistency: without it the fold configuration would
relocate the `def` into a function body and emit the fn-body wording, and the
two configurations would disagree again, which is the exact defect this report is
about.

The file-scope case gets its own message, because "Put it at the top level" is
nonsense advice to someone who did:

```
error: `def` is inside a top-level expression, which cannot bind a global: it
elaborates as one but is emitted as a local, so a later reference fails to
compile. Lift it out to its own top-level form, or wrap the statements in
`(do ...)` -- a top-level `do` is a statement sequence and a `def` inside one
does bind a global
```

### Verification

Both engines now produce the same message for `(if c (def x 1) 2)`,
`(if c (def x 1) (def y 2))`, and `(when c (def x 1))`, in both the fold and
no-fold configurations -- six previously-disagreeing combinations.

- `tests/fixtures/errors/toplevel-def-in-expr-position` -- the rejection, run by
  both harnesses.
- `tests/fixtures/toplevel-def-statement-positions` -- the legal positions,
  including the `do` chain.
- `tests/fixtures/errors/define-bad-position` -- unchanged; the fn-body wording
  still applies there. Its workaround comment is updated, since the file-scope
  spelling is no longer the untestable half.

`tests/run.sh`: 2592 passed, 0 failed. `tests/run-turi.sh`: 1779 passed, 0
failed. The 25 `tur_repl*` / `tur_eval*` / spice / project ctest targets pass.
