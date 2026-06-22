# constrained generic typeclass-method dispatch ignores the element instance (silent mis-dispatch; sometimes TUR_E0020)

Severity: High for the feature (constrained generic container typeclass
instances -- `Encode`/`Decode` for `[Vec]`, `[Option]`, `[Map]`); does not
affect existing single-type instances.

Status: **mostly resolved** (the documented-idiom path now dispatches
correctly; two residual sub-cases remain -- see "Remaining limitations").

## Summary

Inside a constrained generic instance -- e.g. `(definstance C [Vec] [(C A)] ...)`
-- a call to the class method on an element of the constrained type variable `A`
did NOT dispatch through the `(C A)` constraint. It resolved to one fixed
instance (the `int`/first one), so:

  * with an `int` instance in scope, it SILENTLY returned wrong results, and
  * with no `int` instance (e.g. only `cstr` + `float`), the same construct
    failed to compile with `TUR_E0020_AMBIGUOUS_DISPATCH`
    ("receiver type is erased (int64_t)").

`Eq [Vec]` dispatched correctly because its `eq? [x : a y : a] : bool` puts the
class var in argument positions and is handled by a bespoke per-call-site
synthesis (`try_synth_recursive_eq`, F3-5). The single-`a`-argument shape
(`tag : a -> int`, json `encode : a -> cstr`) had no equivalent engagement.

## Repro (now passing -- see fixture)

```turmeric
(defclass Tag [a] (tag [x] : int))
(definstance Tag [int]  (tag [x] 1))
(definstance Tag [bool] (tag [x] 2))

(definstance Tag [Vec] [(Tag A)]
  (tag [x]
    (let [v (:: x (Vec A))]
      (if (>= (vec-len v) 1)
        (tag (:: (vec-get v 0) A))   ;; ascribe the carrier read to A
        0))))

(defn main [] : int
  (let [vb (:: (vec-new) (Vec bool))]
    (vec-push! vb true)
    (println (tag vb))               ;; 2 (Tag[bool]); was 1 (Tag[int])
    0))
```

`Enc` with only `cstr` + `float` instances (no `int`) -- previously `TUR_E0020`,
now encodes per element type. Both are pinned by
`tests/fixtures/constrained-generic-instance-vec-element-dispatch/`.

## Root cause

The element receiver `(vec-get v i)` returns the carrier helper's `:A`, which
collapses to the int64 carrier (`TY_INT`) at elaboration. The documented idiom
(stdlib `vec-of` docstring) is to recover the element type with an explicit
ascription `(:: (vec-get v i) A)` to the constraint var. Two defects defeated
that idiom:

1. **emit side** -- `emit_reresolve_disp_type`
   (`src/compiler/emit_core.c`): the dispatch-type recovery stripped all
   `EX_ASCRIBE` wrappers from the receiver *before* its `TY_TYVAR` check, so an
   `(:: ... A)` ascription-to-constraint-var was discarded and the inner
   carrier `int` was read -- baking the `int` (or first) representative into
   every ABI specialization. The companion field-extraction path
   (`(.value x)`, `constrained-instance-element-dispatch`) worked only because
   a struct field's declared `full_type` preserves the tyvar.

2. **elab side** -- the abstract-tyvar dispatch in `elab_method_call`
   (`src/compiler/elab_typeclasses.c`, the `obj_is_abstract_tyvar` branch)
   selected a representative instance only when an `int` instance existed
   (the carrier-compatible base clone takes the int64 carrier). With no `int`
   instance the dispatch fell through to the generic search, where a tyvar
   receiver with >1 name-matching instance reported `TUR_E0020`.

## Fix

1. `emit_reresolve_disp_type` now honours an ascription-to-constraint-tyvar
   receiver: before stripping, it scans the `EX_ASCRIBE` chain for a `TY_TYVAR`
   ascribed type and uses it as the dispatch type. `emit_resolve_type` then
   grounds that tyvar through the active ABI specialization, so the inner
   element call re-dispatches to the concrete `A` per spec.

2. The `obj_is_abstract_tyvar` branch falls back to any *carrier-compatible
   scalar* instance (`cstr`/`bool`/`nil`/sized-int -- values that ride the
   int64 carrier slot) as the representative when no `int` instance exists.
   Floats and aggregates are not eligible (they would make the base clone
   ill-typed). The base clone stays valid C; emit re-resolution specializes
   each element call.

Both fixes are additive: case 2 only changes programs that previously emitted
`TUR_E0020`. Full suite green (1749 passed, 0 failed).

## Remaining limitations (open)

Tracked as dedicated reports:

* **Element call nested in a lifted closure** -- the per-element call inside a
  `letrec`/`fn` accumulator loop is emitted in a lambda-lifted function that is
  not re-specialized per the enclosing instance specialization, so the baked
  representative leaks through. See
  `docs/reported/constrained-instance-element-dispatch-leaks-into-lifted-closures.md`.

* **Unascribed carrier-helper reads** -- `(tag (vec-get v i))` without the
  `(:: e A)` ascription mis-dispatches because the `:A` return collapses to the
  int64 carrier at elaboration. Ascription is the documented idiom and is
  required here. See
  `docs/reported/unascribed-carrier-helper-read-collapses-element-tyvar.md`.
