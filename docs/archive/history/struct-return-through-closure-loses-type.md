# Fix paper trail: by-value struct results through the closure ABI

**One-line summary:** by-value struct/ADT results now survive the type-erased
closure ABI (lambdas, partial application, first-class function values, and
constructor currying); resolves
`docs/archive/struct-return-through-closure-loses-type.md` and the
`struct-constructor-currying-plan`. Severity was medium.

## Root causes (three, compounding)

1. **Lambda return type never resolved a struct/ADT name.** `elab_fn`'s bare
   single-symbol return-type ladder (`src/compiler/elab_fns.c`) handled
   `ptr<...>`, type params, and primitives, then fell through to "named type
   variable" -- so `(fn [a] : Person ...)` recorded the return as a `TY_TYVAR`
   named `Person`, not the struct. `elab_defn` had the full alias/ADT/struct
   ladder; the lambda path did not. Even a *direct* `((fn [a] : Person ..) 1)`
   failed to resolve `.name` on the result.

2. **Result-type patches clobbered partial applications.** In `elab_call`
   (`src/compiler/elab_call.c`) the LT4 struct patch, the G3 ADT patch, and the
   exists/forall patch unconditionally overwrote the call-expression type with
   the callee's `result_full_type` when `result_kind` was the aggregate. For an
   *under-applied* call the real result is a closure (`TY_PTR_VOID`), so the
   patch mis-typed the partial application as the by-value aggregate. A let
   binding `f` initialized from `(mkp "Bob")` then had a `TY_STRUCT` type, and
   `(f 40)` was even hijacked by CTOR-V0 (which treats a struct-typed name as a
   constructor) into `(make-struct f 40)` -- surfacing as a bogus
   "make-struct 'Person': expected 2 field value(s), got 1".

3. **Partial-apply thunk dropped the StructDef.** `elab_partial_apply` built the
   thunk body's result type with `type_from_kind(result_kind)`, a def-less
   `TY_STRUCT`. `emit_type_c_name` lowers a def-less struct kind to `int64_t`,
   so the thunk was declared returning `int64_t` while its body returned the
   real struct by value -- `cc`: "returning type Person but int64_t expected".

## Fixes

- **CURRY-V0a** (`elab_fns.c`, `elab_fn`): added defalias / ADT / struct
  resolution to the bare single-symbol return-type branch, setting
  `return_full_type` to the nominal type, mirroring `elab_defn`.
- **CURRY-V0b** (`elab_call.c`, `elab_call`): gated the struct / ADT /
  exists-forall result-type patches on `call_expr->type.kind` already being the
  carrier kind (i.e. a genuine full application); an under-applied call's
  `TY_PTR_VOID` closure result is left untouched.
- **CURRY-V1** (`elab_call.c`, `elab_partial_apply`): the thunk's inner-call /
  body result type now carries the full `result_full_type` (with def) for
  struct/ADT results, so emit lowers the thunk's C return type to the by-value
  struct. The thunk is invoked through its typed `tur_thunk_<R>_..._t` slot, so
  no boxing is needed.
- **CURRY-V2** (`elab_call.c`, new `synthesize_struct_ctor` + CTOR-V0 routing):
  an under-applied positional `(Name a)` partial-applies a synthesized, cached
  global `__ctor_<Name>` function whose body is `(make-struct Name p0 ...)`
  (params typed from the struct fields). Named `__ctor_<Name>` (never `Name`) to
  avoid the value-binding/typeclass-ABI clash that made the archived plan drop a
  same-named binding. Parameterized structs decline (return NULL) and keep the
  make-struct arity error. A temporary inner `Scope` used during body
  elaboration is `scope_free`d (it heap-allocates its bindings array -- caught
  by the ASan/LSan gate).

## Tests

- `tests/fixtures/lambda-returns-struct/` -- stored + direct-applied lambda
  returning a struct.
- `tests/fixtures/struct-returning-fn-as-value/` -- the report's repro 2
  (partial application of a named struct-returning defn).
- `tests/fixtures/struct-curry-ctor/` -- chained `((Person "Ada") 36)`, stored
  `(let [mk (Person "Bob")] (mk 40))`, and a full-application control.
- Full suite: `bash tests/run.sh` -> 1801 passed, 0 failed.
