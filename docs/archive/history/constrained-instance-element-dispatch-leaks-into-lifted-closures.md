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

## Resolution (2026-06-22)

Fixed via fix direction 2 -- re-emit such closures once per enclosing
element-type specialization. New machinery in `emit_module.c`:

- `emit_find_dispatch_spec_closure` / `emit_subtree_dispatches_on_spec_tyvar`
  detect a lambda-lifted closure embedded in a constrained-instance body whose
  own body dispatches a typeclass method on a tyvar that the active spec grounds
  to a concrete element (the documented `(tag (:: (vec-get v i) A))` idiom). The
  walker descends into `let`/`letrec` bindings (where the closure literal lives)
  and `EX_BUILTIN` args (where the `(+ acc (tag ...))` accumulator nests).
- `emit_abi_register_call` grows an `inner_dispatch` branch parallel to the
  existing `inner_passed` (M6/G6c) path: it interns a per-spec clone of the
  closure, linked to the outer spec via `inner_closure_spec_idx`. The clone's
  bindings are augmented with the grounded constraint var (`A -> bool`) -- the
  outer spec binds only the class var (`a -> Vec__bool`), and the clone's
  `spec->fn` is the closure (no `owner_instance`), so the re-resolver's
  `param_idx` recovery cannot fire inside it; binding `A` directly lets
  `emit_resolve_type(A)` ground the element call.
- `emit_abi_intern_spec` gained a `match_bindings` flag so distinct-element
  clones (identical int64 carrier signature, differing only in element binding)
  stay distinct specs instead of deduping into one; the existing Gap H
  `__h<n>` clone-name disambiguator then gives them distinct C names.
- `emit_expr.c` retargets both the closure's recursive self-call (clone body
  active, `fn_name_override` set) and the enclosing spec's direct invocation
  (`(go 0 0)`, linked via `inner_closure_spec_idx`) to the clone, mirroring the
  EX_CLOSURE construction's existing `thunk_sym_override`.

Scan-time liveness is handled by scanning the clone body under its own spec
(the `inner_passed || inner_dispatch` scan), so `emit_abi_register_call`'s
reresolve-liveness mark keeps the concrete element instance live.

Regression test: `tests/fixtures/constrained-instance-closure-element-dispatch`
(bool/float/int `Vec` elements through one fold-closure instance body -> 4, 21,
2). Full suite green (1762 passed, 0 failed).
