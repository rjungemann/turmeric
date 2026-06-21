---
title: An applied UNARY constructor instance head (Option cstr) is hardcoded to kind '* -> * -> *', promoting a STAR-declared class to higher kind and rejecting its sibling fully-applied instances
severity: medium -- a STAR-declared typeclass that mixes a concrete instance (`[cstr]`) with an applied-unary-head instance (`[(Option cstr)]`) fails with a spurious TUR-E0012 kind mismatch. Blocks composing container instances (Encode/Decode [Option]) alongside primitive instances in the same class.
status: resolved
discovered: 2026-06-21
resolved: 2026-06-21
surfaced-by: composing Enc [(Option cstr)] alongside Enc [cstr] in one class, while wiring the json container typeclasses (Encode/Decode [Option]) for derive-json in turmeric-spices spices/json.
---

# Applied unary instance head wrongly promotes a STAR-declared class

## One-line summary

`elab_definstance` parsed an applied instance head `(Ctor arg)` by hardcoding
the constructor's kind to `KIND_ARROW2` (`* -> * -> *`, "assume binary"),
then recording the application result as `KIND_ARROW` (`* -> *`).  For a
genuinely *binary* constructor (`(Result int)`) that is correct -- a partial
application that leaves one parameter.  For a *unary* constructor
(`(Option cstr)`) it is wrong: the head is fully applied, so its kind is `*`,
not `* -> *`.

`kind_infer_from_instances` then saw a non-STAR arg kind for the
`(Option cstr)` instance and **promoted** the STAR-declared class `Enc` to
kind `* -> *`.  The belt-and-suspenders validation in `kind_check_expr`
subsequently rejected the sibling `[cstr]` instance:

```
error [TUR-E0012]: kind mismatch (TUR-E0012): instance of 'Enc' provides a
kind-'*' type for parameter 1 which expects kind '* -> *'
```

(reported against the `[cstr]` instance line, even though `(Option cstr)`
is the culprit).

## Minimal repro

```turmeric
(defclass Enc [a] (enc [x] : cstr))
(definstance Enc [cstr] (enc [x] : cstr x))
(definstance Enc [(Option cstr)] (enc [x] : cstr
  (if (.is-some x) (enc (.value x)) "null")))
```

## Root cause

`src/compiler/elab_typeclasses.c`, the applied-head `(Ctor arg)` branch:

```c
*fn_type = ctor_b->type;
fn_type->hkt_kind = KIND_ARROW2;        /* hardcoded binary */
...
type_args[i].hkt_kind = KIND_ARROW;     /* hardcoded ARROW2-applied-once */
```

The constructor's real arity (`Option` has 1 type param, `Result` has 2) was
ignored.

## Fix

Derive the constructor kind from its def's `n_type_params` when the head
resolves to a known struct/ADT, and compute the application result kind via
`kind_apply_one`:

```c
uint32_t ctor_arity = (ctor_b->type.kind == TY_ADT)
    ? ctor_b->type.as.adt_.def->n_type_params
    : ctor_b->type.as.struct_.def->n_type_params;
fn_type->hkt_kind = (ctor_arity > 0) ? kind_for_arity(ctor_arity)
                                     : KIND_ARROW2;   /* opaque fallback */
...
type_args[i].hkt_kind = kind_apply_one(fn_type->hkt_kind);
```

So `(Option cstr)` => fn kind `* -> *`, applied once => `*` (fully applied,
no promotion); `(Result int)` => fn kind `* -> * -> *`, applied once =>
`* -> *` (unchanged).  An opaque (def == NULL) head keeps the legacy binary
assumption, matching the implicit `[result int]` combine path.

## Test

`tests/fixtures/definstance-applied-unary-head-kind/` -- a STAR-declared
class with both a `[cstr]` and a `[(Option cstr)]` instance; asserts the set
elaborates and dispatches correctly over both a concrete value and an Option
local.

## Follow-on

End-to-end composition over a struct *field* of the applied-unary type still
hits a separate instance-method-receiver ABI mismatch (the by-value aggregate
field read vs. the carrier-ABI method parameter) -- tracked in
docs/reported/instance-method-byvalue-struct-field-receiver-abi-mismatch.md.
