---
title: "forall-dict-pass: dispatch inside a directly-applied nested lambda (TUR-E0311)"
category: Reported
description: A dict-clone body whose nested mapper dispatched a constraint method
  inside a lambda applied in place -- `((fn [y] (show y)) x)` -- was rejected with
  TUR-E0311 instead of being lowered. RESOLVED (2026-07-06) by beta-reducing the
  directly-applied captureless lifted lambda in the mapper body so the dispatch
  surfaces at depth-0 where the existing nested-mapper lowering captures the dict.
---

# forall-dict-pass: dispatch inside a directly-applied nested lambda (TUR-E0311)

**Status:** RESOLVED (2026-07-06). The dict-clone nested-mapper lowering now
beta-reduces a directly-applied captureless lifted lambda in a mapper body, so
the stranded dispatch surfaces where the existing lowering captures the dict.
See the Resolution section at the end.

**Summary:** A dict-clone body whose nested mapper dispatches a constraint method
inside a lambda that is DIRECTLY APPLIED in place -- `((fn [y] (show y)) x)` --
is rejected with TUR-E0311 instead of being lowered. **Severity: low** (rare
shape; guarded, not miscompiled; two easy workarounds).

## Repro

```turmeric
(defopaque Identity [a] :int)
(defn mk-id [A] [x : A] : (Identity A) ```c return (int64_t)x; ```)
(defn run-id [A] [i : (Identity A)] : A ```c return (int64_t)i; ```)
(definstance Functor [Identity]
  (fmap [i g] (mk-id (g (run-id i)))))

(defclass Show [a] (show [x] : cstr))
(definstance Show [int]  (show [x] : cstr ```c return "i"; ```))
(definstance Show [bool] (show [x] : cstr ```c return x ? "t" : "f"; ```))

;; The mapper directly APPLIES a lifted inner lambda that dispatches Show.
(defn deep-lens [^f a] [^Functor f ^Show a g : (-> a (f a)) s : a] : (f cstr)
  (fmap (g s) (fn [x : a] : cstr ((fn [y : a] : cstr (show y)) x))))

(defn use-deep
    [l (forall [(f :: * -> *) a] [(Functor f) (Show a)] (-> (-> a (f a)) a (f cstr)))
     s : int] : cstr
  (run-id (l (fn [x : int] : (Identity int) (mk-id x)) s)))

(defn main [] : int (println (use-deep deep-lens 7)) 0)
```

=> `TUR-E0311: ... dispatches a typeclass method ... from inside a
directly-applied nested lambda that cannot be lowered ...`

Negative fixture: `tests/fixtures/errors/forall-dict-nested-lambda-direct-apply/`.

## Root cause

The nested-mapper dict-capture lowering
(`dict_clone_lower_nested_mappers` / `lower_one_mapper`, `src/compiler/elab_call.c`)
reaches a nested mapper only through a **poly-wrap** node -- an `EX_VAR` to a
lifted lambda (`poly_wrap_lifted_mapper`) or a fat `EX_CLOSURE`
(`poly_wrap_closure_mapper`) -- because that is the only form that becomes a
constructed closure whose env can carry a captured dict.

`((fn [y] (show y)) x)` is different: the inner lambda is lambda-lifted to a
top-level `__fn_N` and CALLED BY NAME (`fn_expr` of an `EX_CALL` is an `EX_VAR`
to it), not boxed as a closure value. `mapper_scan_dispatch` deliberately stops
at that lambda boundary (elab_call.c:5639), so the dispatch is never found for
conversion, and the residual falls through to the TUR-E0311 guard
(`dict_clone_dispatch_in_nested_lambda`, elab_call.c ~6210). This is a design
boundary of the by-value HKT lowering, not a miscompile -- Phases 1-3 of
`docs/archive/history/forall-dict-pass-nested-mapper-general-plan.md` cover every mapper
shape reachable through a poly-wrap.

## Fix directions

Lowering this shape requires rewriting a by-name direct call into a
closure-construct-and-fat-call:

1. Convert the directly-applied lifted lambda into a dict-capturing closure
   (reuse `convert_mapper_to_dict_closure`), then
2. rewrite the call site `(__fn_N x)` from a direct C call into an
   env-construct + fat-dispatch of that closure (the inverse of the poly-wrap
   rewrite `rewrite_poly_wrap_to_dict_closure`), and
3. thread the dict outward through each enclosing lambda as Phase 3 already does
   (`lower_one_mapper` union propagation via `caps_out`).

Alternatively, beta-reduce a directly-applied captureless lambda before lifting
(`((fn [y] e) x)` -> `let y = x in e`), which sidesteps the lift entirely and
lets the existing depth-0 in-body dispatch path handle it.

## Workarounds (available today)

- Bind the method directly in the body: `(let [tag (show s)] ...)`.
- Pass the inner lambda as a VALUE to another call rather than applying it in
  place (this is the Phase 3 shape and lowers fine -- see
  `tests/fixtures/van-laarhoven-lens-show-deep/`).

---

## Resolution (2026-07-06)

Took the second fix direction (beta-reduce), adapted to the actual elaborated
shape. The inner lambda is lifted *before* the dict-clone pass runs, so it cannot
be reduced "before lifting"; and it does not reach the mapper body as a bare
`(__fn_N x)` but as a let-hoisted alias -- `let [t __fn_N] (t x)`, where the
call holds its callee in `fn_binding` (not `fn_expr`). The fix beta-reduces it in
place at that point.

**Change** (`src/compiler/elab_call.c`): a new pre-step in `lower_one_mapper`
(before the existing dispatch scan) walks the mapper body with
`flatten_applied_lifted`. Structurally identical to `mapper_scan_dispatch` (same
node coverage, stops at deeper lambda boundaries), it additionally tracks let
aliases to captureless lifted lambdas. When it finds an application whose callee
resolves -- directly or through such an alias -- to a captureless lifted lambda
whose body dispatches one of the clone's constraints, it rewrites the `EX_CALL`
node in place into an `EX_LET` binding the lambda's params to the call's args
with the lambda's body as the let body. The spliced dispatch now sits at depth-0
in the enclosing mapper, so the existing Phase 1-3 lowering converts the mapper
to a dict-capturing closure and dispatches through the env -- and the poly-wrap,
once rewritten to `EX_CLOSURE`, is no longer descended by the TUR-E0311 guard.
Beta-reduction is semantically neutral (single application, each argument bound
once) and idempotent (the rewritten `EX_LET` no longer matches). The now-dead
`let [t __fn_N]` alias binding is left in place; `__fn_N` is emitted but unused,
and codegen handles it cleanly.

**Verified.** The repro compiles and prints `i` (`Show int`). A two-instance
variant (`use-int`/`use-bool` over the same `deep-lens`) prints `i` / `t` / `f`,
confirming the captured dict dispatches per instance at runtime rather than
baking a single representative. A struct-based (non-inline-C) variant runs
correctly under `--interpret` too (the fix is in shared elaboration), confirming
both paths benefit. `bash tests/run.sh`: 1951 passed, 0 failed. `bash
tests/run-turi.sh`: 1427 passed, 0 failed.

**Fixtures.** The negative fixture
`tests/fixtures/errors/forall-dict-nested-lambda-direct-apply/` (asserted
TUR-E0311) is removed and replaced by the positive
`tests/fixtures/van-laarhoven-lens-show-direct-apply/`, which exercises the shape
at both `Show int` and `Show bool` (expected `i` / `t` / `f`). Like its siblings
it uses the `defopaque [a] :int` Identity, so the interpreter harness skips it
via the inline-C carve-out.

**Discovered, separate:** a WIDE `:copy`-struct functor (rather than the
one-int64 `defopaque` carrier) carrying a second `Show` constraint emits an
undeclared dict symbol (`_un_undict_...`) and fails at `cc` -- even in the
depth-0 workaround form, so it is unrelated to this shape. Filed as
`docs/reported/wide-copy-struct-functor-show-dict-undeclared.md`.

Not pursued: the first fix direction (convert the lifted lambda to a
dict-capturing closure and rewrite the by-name call into an env-construct +
fat-dispatch). Beta-reduction eliminates `__fn_N` from the hot path entirely and
reuses the existing depth-0 lowering, so it is both simpler and lower-risk than
introducing a new closure-construct rewrite.
