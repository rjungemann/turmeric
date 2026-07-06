# forall-dict-pass: dispatch inside a directly-applied nested lambda (TUR-E0311)

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
`docs/archive/forall-dict-pass-nested-mapper-general-plan.md` cover every mapper
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
