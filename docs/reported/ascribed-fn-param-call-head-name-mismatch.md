# Calling an ascribed `:fn` param emits a call-head temp whose declaration and use disagree on its C name

**Severity: medium** -- a hard cc error (`'__call_head_N' undeclared`) on a
documented spelling, with no fixture covering it. Filed 2026-08-27, found
while building the regression fixture for
[fat-dispatch-wide-byvalue-aggregate-argument](../archive/fat-dispatch-wide-byvalue-aggregate-argument.md);
unrelated to aggregates -- a scalar reproduces it.

## Repro

```turmeric
(defn apply-un [f : fn v : int] : int
  ((:: f (fn [int] int)) v))

(defn mk [bump : int] : ptr<void>
  (let [lb bump]
    (:: (fn [x : int] (+ lb x)) :ptr<void>)))

(defn main [] : int
  (println (apply-un (mk 100) 6))
  0)
```

```
tur_poly_fn_t _un_uncall_unhead_un1344_1345 = f;   /* the DECLARATION */
... __call_head_1344.fn(__call_head_1344.env, ...) /* the USE */
error: '__call_head_1344' undeclared
```

## Root cause

`elab_call_head_expr` (`elab_call.c:1487`) hoists the callable head into a
synthetic binding named `__call_head_<N>`, and the two emission ends name it
through different rules:

- **Use** (`emit_call_name` -> `raw_name_for_binding`, `emit_core.c:1792`):
  the binding's type is a non-boxed `TY_FN`, so `name_for_binding`'s
  bare-function-reference branch returns the raw name -- and the raw path's
  "compiler-synthesized `__` names that are already pure C identifiers are
  emitted verbatim" rule keeps `__call_head_1344` as-is.
- **Declaration**: the let-binder emission lands on the id-suffixed mangling
  tail of `name_for_binding` (`tur_mangle_append` + `_<id>`), which escapes
  every `_` as `_un` and appends the binding ordinal:
  `_un_uncall_unhead_un1344_1345`.

Same binding, two names. The `-Wimplicit` era would have linked anyway; today
it is a clean build break.

## Fix directions

- Cheapest: name the temp something that both paths spell identically -- the
  verbatim rule keys on "pure C identifier with `__` prefix", so making the
  DECLARATION side apply the same short-circuit (or making the temp's name
  carry no underscores at all) closes the gap.
- Sturdier: one function owns the binding-to-C-name decision for synthetic
  bindings, consulted by both the binder emission and `raw_name_for_binding`,
  so the two cannot diverge again.

## Coverage

No fixture exercises `((:: f (fn [...] ...)) args)` on a bare `:fn` param.
Whatever fixes this should add one; the trimmed `apply-un` leg in
`tests/fixtures/fat-dispatch-wide-byval-arg/input.tur`'s header comment is the
ready-made shape.
