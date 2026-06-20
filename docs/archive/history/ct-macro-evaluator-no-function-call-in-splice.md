---
title: The compile-time macro evaluator cannot call functions or expand nested macros inside a `~@` splice expression
severity: medium -- forces per-arity macro cascades for any template that needs to GENERATE its spliced sequence (map over a list, recurse over it). This is the actual remaining blocker for collapsing the ECS `defworld--0..5` cascade into one variadic-over-components macro; the splice-into-vector mechanism it was paired with is now fixed.
status: RESOLVED 2026-06-17
discovered: 2026-06-17
surfaced-by: turmeric-spices ECS work (E2d) -- attempting the variadic `defworld` collapse after the `~@`-splice-into-vector fix landed.
---

> **RESOLVED 2026-06-17.** Both minimal repros now compile, expand, and run.
> The fix is in `src/compiler/elab_macros.c`:
>
> 1. **Compile-time `map`** -- `ct_eval_call` now handles `(map fn seq)` by
>    applying a CT `fn` value (a literal `(fn ...)`) to each element of a
>    list/vec/nil and collecting the results into a list. It lives in
>    `ct_eval_call` (not the `ct_eval_builtin` table) because the function
>    argument must stay a `CT_VAL_FN` rather than be forced through
>    `ct_value_to_form`. A `(map f xs)` whose `f` is an ordinary runtime
>    function falls through and is rebuilt as a data/runtime call, so a
>    runtime map in a template is not hijacked.
> 2. **Nested-macro expansion in a splice** -- a new `expand_macro_head` flag
>    on `CtEnv`, set only by `ct_eval_splice_inner` (the evaluator used at
>    every `~@` splice site), lets a list whose head names a user macro be
>    expanded at compile time. This enables `~@(chain (rest xs))`-style
>    recursion. Crucially the flag is **not** set when evaluating ordinary
>    arguments, so a macro-built data form threaded through `list`/`cons`
>    (e.g. an accumulated `(map-assoc ...)` from `hamt-of`) is left as data
>    for ordinary elaboration rather than being prematurely expanded (and
>    CT-`let`-inlined, which produced `&<literal>` lvalue errors).
> 3. **`substitute_params` deferral** -- the eager list-splice path no longer
>    flattens a `~@X` whose substituted `X` is a CT computation (a builtin /
>    `map` call, a macro call, or anything containing CT builtins). It keeps
>    the `~@` wrapper so the post-substitution `ct_eval_quasiquote` pass
>    computes `X` and splices its RESULT, instead of flattening the
>    un-evaluated call tokens (which is what leaked `map`/`chain` as unbound
>    symbols).
>
> Regression fixtures: `tests/fixtures/macro-map-in-splice/` and
> `tests/fixtures/macro-nested-macro-in-splice/`. Full suite green
> (1666 passed, 0 failed), including all `#map{...}`/`hamt-of` data-literal
> fixtures.
>
> **Note:** the full ECS `defworld` collapse (validation item 2 below) needed
> one more, independent piece -- `defstruct` accepting grouped `[name : type]`
> field-spec sub-vectors -- which is now also fixed (see
> `docs/archive/defstruct-grouped-field-spec-vectors.md`). With both in place a
> single variadic `defworld` expands and runs end to end.

# CT macro evaluator: no function call / nested-macro expansion in a `~@` splice

## One-line summary

The compile-time evaluator that runs macro bodies (`ct_eval_*` in
`src/compiler/elab_macros.c`) has a small fixed set of builtins
(`first`/`rest`/`cons`/`list`/`vec`/`=`/...) but **cannot call `map` (it is
unbound) and cannot expand a nested user macro from inside a `~@` splice
expression**.  So a template cannot *generate* the sequence it splices --
it can only splice an already-built value handed in as a macro parameter.

## Minimal repros (both fail; both in plain list context, so this is NOT the
## vector-splice bug)

`map` is unbound at macro-eval time:

```turmeric
(defmacro emit-prints [items]
  `(do ~@(map (fn [x] `(println ~x)) items)))
(defn main [] : int (emit-prints ("a" "b" "c")) 0)
;; error: unbound symbol 'map'
```

A nested user macro cannot be called from a splice (so recursion over the
list is impossible):

```turmeric
(defmacro chain [xs]
  (if (empty? xs)
    `()
    `((println ~(first xs)) ~@(chain (rest xs)))))   ;; <- (chain ...) here
(defmacro run-all [xs] `(do ~@(chain xs) 0))
(defn main [] : int (run-all (1 2 3)))
;; error: unbound symbol 'chain'
```

Splicing a value passed *in* works fine (this is what the vector-splice fix
enabled), e.g. `(defmacro mk [name flds] \`(defstruct ~name [~@flds]))` with
`(mk W (a : int b : int))`.  The gap is purely the inability to *compute* the
spliced sequence inside the template.

## Why it matters

The ECS `defworld` wants to map each caller-supplied component name to a
`[c : (Dense c)]` field and splice the lot:

```turmeric
(defmacro defworld [name comps]
  `(defstruct ~name [gens : int ~@(map (fn [c] `[~c : (Dense ~c)]) comps)]))
```

Both ingredients are missing: `map` is unbound, and the recursive-helper
alternative (`~@(world-fields (rest comps))`) can't call `world-fields` from
the splice either.  So the `defworld--0..5` per-arity cascade has to stay.

## Root cause (suspected)

`ct_eval_builtin` (`src/compiler/elab_macros.c:318`) enumerates the supported
compile-time builtins by name; `map` is not among them, and there is no
fallback that resolves a name to a user macro/`defn` and applies it.
`ct_eval_call` (`:440`) applies `CT_VAL_FN` closures (so `(fn ...)` values
exist), but a bare `(map ...)` / `(chain ...)` head that is neither a builtin
nor a bound `CT_VAL_FN` in the macro env resolves to nothing -> "unbound
symbol".

## Proposed fix directions

1. Add a `map` builtin: it would need to evaluate its function argument to a
   `CT_VAL_FN` and apply it per element.  Because `ct_eval_builtin` receives
   already-form-evaluated args, this likely wants handling in `ct_eval_call`
   (where `CT_VAL_FN` is available) rather than the form-level builtin table.
2. Allow a splice head to resolve to a user `defmacro` and expand it (enabling
   recursion over the list), and/or to a CT-evaluable `defn`.
3. Either unblocks the variadic `defworld` collapse.

## Validation when fixed

- Both minimal repros above compile, expand, and run.
- The ECS `defworld` macro collapses from `defworld--0..5` to one
  variadic-over-components form.

## Cross-references

- `docs/archive/quasiquote-splice-into-vector-unsupported.md` -- the
  `~@`-into-vector mechanism (RESOLVED); this is the remaining, independent
  blocker for the `defworld` collapse.
- `docs/archive/macro-template-type-position-rejects-unquoted-compound.md` --
  the type-position unquote fix (RESOLVED).
