# Generalize nested-mapper dict capture: N dicts, capturing mappers, deeper nesting

**Status:** OPEN.
**Predecessor:** `docs/archive/forall-dict-pass-nested-lambda-dispatch-plan.md`
(landed the *primary* case -- a captureless mapper dispatching a SINGLE
constraint class -- and narrowed the TUR-E0311 guard to the residual this plan
covers).
**Guarded by:** `TUR-E0311` in `make_dict_clone` (elab_call.c).  Negative
fixture: `tests/fixtures/errors/forall-dict-nested-lambda-multiclass/`.

## What already works

A van Laarhoven mapper `(fn [x] (show x))` inside a dict-clone body is lowered:
`convert_mapper_to_dict_closure` (elab_call.c) turns the captureless lifted
mapper into a closure that CAPTURES the constraint's runtime dict, and its
method call dispatches through the env-loaded dict (`env->dict`) via
`emit_call_name` (emit_core.c, keyed on `ctx->cur_dict_env_class` /
`cur_dict_env_binding`).  The current converter is deliberately narrow:

- it captures **exactly one** dict (a single constraint class), and
- it only promotes a **captureless** mapper (reached through an `EX_VAR` to an
  `is_lifted_lambda` FnDef), and
- it only handles a dispatch **directly in the mapper body** (depth 0).

`mapper_scan_dispatch` classifies anything else as `complex`, and
`make_dict_clone` then rejects it with TUR-E0311. This plan lifts each of those
three restrictions.

## Phase 1 -- N-dict env (multi-class mapper)

Goal: a mapper that dispatches **two or more** distinct constraint classes from
one nested lambda -- e.g. `(fn [x] (combine (show x) (rank x)))` under
`(Functor f, Show a, Ord a)` -- is lowered by capturing one dict per class.

The env already holds one captured dict; this generalizes it to a vector.

### Task 1.1 -- widen the FnDef mark from scalar to vector
- `expr.h`: replace `FnDef.dict_env_class` (single `TypeClass *`) and
  `dict_env_binding` (single `Binding *`) with parallel vectors
  `dict_env_classes[MAX_FN_CONSTRAINTS]` / `dict_env_bindings[...]` plus
  `uint8_t n_dict_env`. Keep `dict_env_mapper_ty` as-is.

### Task 1.2 -- collect ALL dispatched classes
- `mapper_scan_dispatch` (elab_call.c): instead of setting `*complex` on the
  second distinct class, ACCUMULATE the set of dispatched classes (dedup by
  class identity), still on a bare-`TY_TYVAR` receiver and still bailing to
  `complex` on a DEEPER lambda (Phase 3) or a non-constraint class. Return the
  class set.

### Task 1.3 -- capture N dicts
- `convert_mapper_to_dict_closure`: capture one dict binding per dispatched
  class (look each class up in the clone's constraint order -> `dparams[c]`),
  build a Closure whose `captures` lists all N dict bindings, and populate the
  `dict_env_classes[]` / `dict_env_bindings[]` vectors. The existing EX_CLOSURE
  env-build emits one struct field per capture, so the env already carries N
  dict fields with no emit change.

### Task 1.4 -- dispatch to the right captured dict per class
- `emit_internal.h` / `emit_fns.c`: install the full
  `cur_dict_env_classes[]` / `cur_dict_env_bindings[]` vector while emitting the
  mapper body (parallel to the `dict_dispatch_*` vector already there).
- `emit_core.c` `emit_call_is_dict_env_dispatch` + `emit_call_name`: match the
  call's `dict_arg` class against ANY of `cur_dict_env_classes[0..n)` and
  dispatch through the corresponding captured binding's `env->dict` load. Keep
  the TY_TYVAR-receiver gate.

### Phase 1 acceptance
- `tests/fixtures/van-laarhoven-lens-show-rank/` (new, positive): the
  `(Functor f, Show a, Ord a)` mapper `(fn [x] (combine (show x) (rank x)))`
  compiles and runs, dispatching `show`/`rank` through their own captured dicts.
- Retire `tests/fixtures/errors/forall-dict-nested-lambda-multiclass/` (it now
  compiles) or repoint it at a still-residual shape.

## Phase 2 -- capturing mappers

Goal: a mapper that ALREADY captures a value (the `set`/`over` shape
`(fn [a] (mk-id b))` extended to also call `show a`) dispatches through a
captured dict.

Such a mapper is an `EX_CLOSURE` in the body (not an `EX_VAR` to a lifted
lambda), so `poly_wrap_lifted_mapper` returns NULL today and the converter is
skipped. Instead of promoting a captureless FnDef, **augment the existing
closure**:

- Add an `EX_CLOSURE` recognizer alongside `poly_wrap_lifted_mapper` that
  returns the closure's FnDef.
- Extend the converter to APPEND the dict binding(s) to the existing
  `Closure.captures` (growing the env struct) rather than prepending a fresh env
  param -- the closure already has its env param and env-build. Set the
  `dict_env_*` vectors on the same FnDef.
- No poly-wrap rewrite is needed (it is already `is_closure`); only the env
  struct and the capture list grow.

### Phase 2 acceptance
- A `set`/`over`-style capturing mapper that also dispatches `show` on the focus
  compiles and runs; the env carries both the captured value and the dict.

## Phase 3 -- deeper nesting

Goal: a dispatch **two lambdas deep** -- `(fn [x] ((fn [y] (show y)) x))` or a
mapper that passes an inner lambda to another call -- threads the dict through
each intermediate lambda's env.

The dict binding captured by the innermost mapper must also be captured by every
enclosing lambda on the path (each lambda's env forwards it inward). This is the
recursive generalization:

- After converting an inner mapper to capture dict `D`, the enclosing lambda now
  references `D` (through the inner closure's construction), so it must capture
  `D` too. Walk outward from each converted mapper, adding `D` to every
  enclosing lifted lambda / closure on the path up to the dict-clone body proper
  (where `D` is a real parameter).
- Bound the depth with the existing `mapper_scan_dispatch` recursion guard.

### Phase 3 acceptance
- A fixture with a two-deep dispatch compiles and runs; each intermediate env
  carries the dict.

## Phase 4 -- retire the guard

Once Phases 1-3 land, the TUR-E0311 residual guard in `make_dict_clone` should
reject nothing reachable. Either delete it, or keep it as a defensive
assertion for a genuinely unsupported shape (document which). Archive this plan
and the predecessor's residual note.

## Risks / notes

- **Env-slot vs class identity.** With N dicts, dispatch keys on class identity
  (Task 1.4), so slot ORDER in the env is free -- but the capture list order and
  the `dict_env_classes[]`/`bindings[]` parallelism must stay consistent.
- **Shared body, multiple clones.** All three phases keep mutating the SHARED
  `orig->body`; the conversion stays idempotent (guard on `n_dict_env > 0`) and
  the memoized dict bindings (predecessor Phase 1) keep the captured bindings
  valid across every clone. `rewrite_poly_wrap_to_dict_closure` already handles
  a mapper boxed at two sites -- extend it for the N-dict/capturing shapes.
- **Capturing-mapper env growth** interacts with the existing MB4 fat-box
  boundary for `g`; verify a capturing mapper that also crosses the poly carrier
  still emits a valid `tur_poly_fn_t`.
