# An explicitly-annotated `rc<int>` parameter generalizes to a bare tyvar

**Severity: high** -- it makes every rc builtin a hard error on a parameter,
and it blocks the E3a owning-cloneable-capture channel
(`docs/upcoming/cps-backend-owning-env-teardown-e3-plan.md`) entirely.

## One-line summary

A function parameter annotated `r : rc<int>` is resolved as a bare type
variable, not `rc<int>`: any rc builtin applied to it fails with
`requires rc<T> ..., got tyvar`, and the function's recorded signature carries
`TY_TYVAR` (36) for that arg, not `TY_RC` (9).

## Minimal repro

```turmeric
(defn f [r : rc<int>] : int (rc/strong-count r))
(defn main [] : int
  (let [x (rc/of 5)]
    (println (f x))
    0))
```

```
error: rc/strong-count requires rc<T> or a constrained existential, got tyvar
 1 | (defn f [r : rc<int>] : int (rc/strong-count r))
   |                             ^^^^^^^^^^^^^^^^^^^
```

Every rc builtin behaves the same on the param (`rc/clone`, `rc/drop`,
`rc/strong-count`). Moving the param into a concrete struct
(`make-struct B :r r`) does **not** pin it either -- `(make-struct B :r (rc/clone r))`
still errors on the inner `rc/clone`. A `^borrow` annotation
(`[^borrow r : rc<int>]`) does not change it.

Contrast: a **local** rc is fine -- `(let [r (rc/of 99)] (rc/strong-count r))`
resolves `r` as `TY_RC` and compiles. So the defect is specific to a
**parameter** whose declared type is `rc<T>`.

## Symptom in the compiler

`build_marshal_reset` (`src/passes/cps_ir.c`, the 2-arg call-frame arm ~1159)
reads `fb->type.as.fn.arg_kinds[env_idx]` for the callee of a continuation
frame. For a callee declared `[v : int r : rc<int>]` this is `TY_TYVAR` (36),
not `TY_RC` (9) -- confirmed by tracing (`argk0=3=TY_INT`, `argk1=36=TY_TYVAR`).
Even adding a concrete monomorphizing call `(f 0 (rc/of 1))` elsewhere does not
change the recorded param kind: the function's own scheme is generalized.

## Root cause (direction, not yet pinned to a line)

The generalization pass that builds a top-level function's type scheme is
generalizing the `rc<int>` **annotation** into a fresh type variable instead of
keeping the annotated concrete type. An explicitly-annotated parameter type
should never generalize -- the annotation fixes it. Likely in the fn-def
elaboration / scheme-generalization path (`elab_fns.c` and the generalize step
it calls); the annotated-param type is being treated as an inferred (thus
generalizable) type rather than a fixed one. Needs pinning to the exact
generalize call and a guard: do not generalize a parameter whose source carried
an explicit type annotation (or: only generalize the still-free variables
*inside* the annotation, e.g. the `T` of `rc<T>` when `T` is itself unbound, not
the whole param).

## Why it blocks E3a (owning-cloneable-capture)

E3a admits an owning `rc` captured into a multi-shot cloneable continuation and
gives its captured frame env clone/drop teardown. An rc reaches a continuation
frame only as the non-hole operand of a 2-arg call frame `(f [] r)` -- i.e.
through a **user function with an rc parameter**. Because that parameter
generalizes to a tyvar:

1. The frame body cannot touch the rc through any rc builtin (hard `got tyvar`
   error) -- so a borrow-scheme frame (read the rc, never drop it) cannot even be
   written.
2. `build_cloneable` sees `arg_kinds[env] = TY_TYVAR`, not `TY_RC`, so it cannot
   recognize the frame as owning from the param kind. (Gating on the *operand*
   type -- reliably `TY_RC` -- sidesteps this half, but not #1.)

The E3a gate scaffolding (`owning-cloneable-capture` experiment,
`g_opt_owning_cloneable_capture`) is landed and inert. The elab admission
(relax TUR-E0014 under the gate) and the `build_cloneable` owning-env relaxation
(operand-gated + `^borrow`-gated, the provably-sound borrow subset) were
prototyped and reverted pending this fix -- they are correct-direction but lead
to no compilable shape while the param generalizes. Re-apply them once a
parameter annotated `rc<T>` reliably resolves to `TY_RC`.

## Secondary design note (for when this unblocks)

Even with concrete rc params, the cloneable frame teardown scheme depends on
whether the frame **consumes** or **borrows** the captured rc, and the two
schemes are duals (consuming: `env_clone` only, the frame's own drop balances
the per-copy incref; borrowing: `env_clone` + `env_drop` + incref-at-capture).
`build_cloneable` cannot see whether the *called* function drops its rc param,
so the sound first cut is the **`^borrow`** subset: a `^borrow` rc param is a
type-system guarantee the callee never drops it, so the borrow scheme is always
correct. Widen to the consuming case only with an interprocedural
"does this fn consume its rc param" analysis.
