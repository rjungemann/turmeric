# Unascribed carrier-helper read collapses the element tyvar, mis-dispatching constrained-instance element calls

Severity: Low (a documented ascription idiom works around it; ergonomics only).

Status: **RESOLVED 2026-06-22.** The unascribed form `(tag (vec-get v i))` now
dispatches on the element type exactly like the documented `(:: (vec-get v i)
A)` ascription. Pinned by
`tests/fixtures/constrained-generic-instance-vec-element-unascribed/` (prints
`1`, `2`, `hello`, `F`). Full suite green: `1762 passed, 0 failed`. See
"Resolution" at the bottom.

## Summary

Inside a constrained generic instance -- `(definstance C [Vec] [(C A)] ...)` --
a class-method call whose receiver is an *unascribed* carrier-helper read
(`(tag (vec-get v i))`) dispatches to the baked representative instance instead
of the element's own instance. Adding the documented ascription
`(tag (:: (vec-get v i) A))` fixes it (see
`docs/reported/constrained-generic-instance-method-dispatch-ignores-constraint.md`),
but the unascribed form silently mis-dispatches with an `int` instance in scope.

## Minimal repro

```turmeric
(defclass Tag [a] (tag [x] : int))
(definstance Tag [int]  (tag [x] 1))
(definstance Tag [bool] (tag [x] 2))

(definstance Tag [Vec] [(Tag A)]
  (tag [x]
    (let [v (:: x (Vec A))]
      (if (>= (vec-len v) 1)
        (tag (vec-get v 0))     ;; unascribed -- receiver collapses to int
        0))))

(defn main [] : int
  (let [vb (:: (vec-new) (Vec bool))]
    (vec-push! vb true)
    (println (tag vb))          ;; EXPECT 2 (Tag[bool]); ACTUAL 1 (Tag[int])
    0))
```

Adding `(:: (vec-get v 0) A)` makes it print `2`.

## Root cause

`vec-get` is the generic carrier helper `(defn vec-get [A] [v : (Vec A) i :int]
:A ...)` (`stdlib/vec.tur`). When called with `v : (Vec A)` (the instance
constraint var), its `:A` result is summarized to the int64 carrier kind
(`TY_INT`) rather than the instantiated type-param tyvar `A`. So at the `tag`
call site the receiver's type is `TY_INT`:

- elaboration (`elab_typeclasses.c:elab_method_call`) sees an `int` receiver,
  matches `Tag[int]` exactly, and bakes it -- it never enters the
  `obj_is_abstract_tyvar` path; and
- emit (`emit_core.c:emit_reresolve_disp_type`) sees a `TY_INT` receiver with no
  tyvar to re-resolve, so the baked `int` representative stands.

Verified with a debug probe: the `tag` receiver's elaborated type kind is
`TY_INT` (3); with the explicit ascription it is `TY_TYVAR` (36), which the
re-resolver now honours.

The struct-field analog (`(.value x)` on `(Option A)`) does NOT have this
problem because a struct field's declared `full_type` preserves the tyvar, which
`emit_reresolve_disp_type` already reads
(`constrained-instance-element-dispatch`). Carrier *function* results carry no
such full-type channel.

## Fix directions

- Preserve the instantiated type-param tyvar on a generic carrier call's result
  -- alongside (not instead of) the int64 carrier summary kind -- so both elab
  dispatch and emit re-resolution see the element tyvar without an explicit
  ascription. This likely means giving `EX_CALL` a `result_full_type` channel
  (mirroring `StructField.full_type`) populated when the callee's declared
  return is a type-param, and consulting it in `emit_reresolve_disp_type` (a new
  case parallel to the `EX_GET_FIELD` recovery) plus the `obj_is_abstract_tyvar`
  test in `elab_method_call`.
- Cheaper interim: keep the ascription idiom but emit a hint/warning when a
  class-method receiver is an unascribed carrier-helper read whose declared
  return is a type-param under a constrained instance, so the silent
  mis-dispatch becomes visible.

## Resolution

Took the first fix direction, but without adding a new `EX_CALL` field: the
callee's `EX_CALL` already retains its `fn_binding`, whose `TY_FN` type carries
both `result_full_type` (the carrier helper's own type-param `R`, e.g. `A` for
`vec-get`'s `:A`) and `arg_full_types[]` (every parameter's declared full type,
including the parametric `(Vec R)`). That is the same channel
`StructField.full_type` provides for the `(.value x)` struct-field analog --
recovered on demand at the two dispatch sites instead of stored on a new field.

A shared recovery shape was added at both sites: find the parameter whose
declared full type mentions the result tyvar `R`, structurally match it against
the **actual** argument's type, and read the subtype at `R`'s position. For
`(tag (vec-get v 0))` with `v : (Vec A)`, matching `(Vec R)` against `(Vec A)`
yields the constraint var `A`, which then grounds per ABI specialization through
the existing constraint-var `param_idx` machinery.

1. **emit side** -- `emit_reresolve_disp_type` (`src/compiler/emit_core.c`): a
   new branch (parallel to the `EX_GET_FIELD` recovery) fires when the receiver
   is an unascribed `EX_CALL` whose callee `result_full_type` is a bare
   `TY_TYVAR`. It recovers the element tyvar via `emit_pattern_extract_classvar`
   against the actual arg type; the downstream `emit_resolve_type` +
   constraint-var grounding then re-dispatches the element call to the concrete
   `A` per spec. This handles the emit-side mis-dispatch (silent wrong instance
   with an `int` instance in scope).

2. **elab side** -- `elab_method_call` (`src/compiler/elab_typeclasses.c`): the
   `obj_is_abstract_tyvar` test now also returns true for an unascribed
   carrier-helper receiver whose recovered element is still a `TY_TYVAR`
   (`obj_is_unascribed_carrier_elem` + local `tc_pattern_extract_var`). This
   routes it to the carrier-compatible representative + dict tagging, so a
   constrained instance with **no** `int` instance (only `cstr`/`float`) no
   longer reports `TUR_E0020` at elaboration -- it defers to the emit-side
   re-resolution above, exactly as the documented `(:: e A)` ascription path
   already did.

Both changes are guarded on the recovered element being an abstract tyvar, so
they only engage inside a constrained generic instance body (where the
container's element is the constraint var). A concrete carrier read is
unaffected. The residual `-Wint-conversion` warning on the dead carrier base
clone is pre-existing and identical to the ascribed fixture
(`constrained-generic-instance-vec-element-dispatch`) -- not a regression.

Pinned by `tests/fixtures/constrained-generic-instance-vec-element-unascribed/`.
