# Constrained-instance element dispatch leaks the representative instance into lambda-lifted closures

Severity: Medium (blocks the common accumulator-loop shape of constrained
generic container instances; the non-closure body shape works -- see
`docs/reported/constrained-generic-instance-method-dispatch-ignores-constraint.md`).

## Summary

When a constrained generic instance body -- `(definstance C [Vec] [(C A)] ...)`
-- performs its per-element class-method call from *inside a `letrec`/`fn`
closure* (the natural shape for a fold/accumulator loop), the element call
dispatches to the baked representative instance instead of the element's own
instance, even when the element read is correctly ascribed to the constraint
var `A`. With an `int` instance in scope this is a silent wrong result.

The same body written without a closure (element call directly in the method
body) now dispatches correctly, so this is specifically a
closure-monomorphization gap.

## Minimal repro

```turmeric
(defclass Tag [a] (tag [x] : int))
(definstance Tag [int]  (tag [x] 1))
(definstance Tag [bool] (tag [x] 2))

(definstance Tag [Vec] [(Tag A)]
  (tag [x]
    (let [v (:: x (Vec A))  n (vec-len v)]
      (letrec [go (fn [i : int  acc : int] : int
                    (if (>= i n) acc
                      (go (+ i 1) (+ acc (tag (:: (vec-get v i) A))))))]
        (go 0 0)))))

(defn main [] : int
  (let [vb (:: (vec-new) (Vec bool))]
    (vec-push! vb true) (vec-push! vb false)
    (println (tag vb))      ;; EXPECT 4 (2 + 2 via Tag[bool])
    0))                     ;; ACTUAL  2 (1 + 1 via Tag[int])
```

`tur check` is clean; `tur run` prints `2`.

Contrast -- the same logic with the element call lifted out of the closure
(into the method body, or via a tail-recursive helper that is itself a
top-level constrained generic) dispatches correctly and prints `4`.

## Root cause

The `go` closure is lambda-lifted to a single top-level function emitted once.
The constrained-instance re-dispatch machinery
(`emit_core.c:emit_reresolve_method_call` /
`emit_reresolve_disp_type`) only re-resolves a class-method call when the
emitter is inside an *ABI specialization of the enclosing instance method*
(`ctx->current_abi_specialization`). A lambda-lifted closure body is emitted in
its own function context, NOT under the instance-method specialization, so the
re-resolver never fires and the carrier representative (`__inst_Tag_tag_int`)
that elaboration baked into the call survives into the emitted closure.

Confirmed in the emitted C: the lifted closure `__fn_<n>` contains
`__inst_Tag_tag_int(vec_hyget(...))` and is shared between the carrier base
clone and every `Vec` specialization -- it is never re-specialized per element
type.

## Fix directions

- Thread the active outer specialization's tyvar bindings (the
  `EmitAbiSpecialization` mapping `A -> bool` for the `Vec__bool` spec) into
  closures defined within a constrained-instance method body, so the
  re-resolver can ground the constraint var while emitting the closure body; or
- Re-emit such closures once per enclosing specialization (a per-(closure,
  spec) clone keyed like the instance-method spec), so each element-type spec
  carries its own monomorphized closure with the correct element instance baked
  in.

The scan-time companion (`emit_reresolve_method_fndef`) that marks the
re-dispatched instance live must be extended the same way so the concrete
element instance is not pruned as dead.
