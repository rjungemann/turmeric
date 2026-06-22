# Unascribed carrier-helper read collapses the element tyvar, mis-dispatching constrained-instance element calls

Severity: Low (a documented ascription idiom works around it; ergonomics only).

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
