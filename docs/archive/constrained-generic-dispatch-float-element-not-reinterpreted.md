# Float element value is bit-mangled across constrained-generic dispatch (follow-up to #490)

**Found by:** turmeric-spices Track C (U2 json containers)
**Verified on:** turmeric 0.22.0, main @ 1941256
**Severity:** Medium. Blocked json `Encode`/`Decode` container instances for
FLOAT element arrays/objects; int/bool/cstr/struct elements already worked
after #490.

## Status: FIXED (this branch)

## Summary

#490 made a constrained generic instance (or generic helper defn) dispatch a
class method on the *concrete* element type recovered via `(:: e A)`. For
int/bool/cstr that both dispatches and carries the value correctly. For a
`float` element the dispatch picked the right instance, but the float VALUE
arrived as its raw int64 carrier bits -- it was not reinterpreted as a double.
The element read `(:: (vec-get v i) A)` with `A = float` did not perform the
bit-reinterpret that the concrete `(:: ... :float)` ascription does, so the
method body saw garbage (encoding `1.5` yielded `4.6e18`, the bit pattern of
1.5 cast int->double).

## Repro (no inline C)

```turmeric
(defclass InUnit [a] (in-unit? [x] : bool))
(definstance InUnit [float] (in-unit? [x] (if (> x 1.0) (< x 2.0) false)))

;; generic helper: read a float element via (:: e A), call the method
(defn in-unit-vec? [A] [(InUnit A)] [v : (Vec A)] : bool
  (in-unit? (:: (vec-get v 0) A)))

(defn main [] : int
  (let [v (:: (vec-new) (Vec float))]
    (vec-push! v 1.5)
    (if (in-unit-vec? v)
        (println "generic: true") (println "generic: false"))   ;; want true
    (if (in-unit? (:: (vec-get v 0) :float))
        (println "direct: true") (println "direct: false"))     ;; want true
    0))
```

Before the fix:

```
generic: false      <-- 1.5 arrived bit-mangled, predicate failed
direct: true        <-- correct when ascribed (:: ... :float)
```

After the fix both print `true`.

## Root cause

Two coordinated sites in `src/compiler/emit_expr.c`, both on the
`(:: (vec-get v i) A)` ascription whose target is the constraint tyvar `A`:

1. **Argument-strip (the method-call arg loop, ~line 3014).** The wrapper for a
   method-call argument is stripped of its `EX_ASCRIBE` before `emit_value`
   unless `preserve_ascribe_for_bridge` is set. The preserve gate only covered
   `(:: x :int)` byval/heap carrier bridges; a `TY_TYVAR`-targeted ascription
   was always stripped, so the underlying `(vec-get v i)` int64 carrier call was
   emitted raw and handed to the `double` parameter -- a NUMERIC int64->double
   conversion, not a bit reinterpret.

2. **`EX_ASCRIBE` emit (~line 5404).** Even when reached, a `TY_TYVAR`-targeted
   ascription was classified `ascribe_to_opaque` (a pure relabel: "the value
   already IS the carrier") and returned `inner_val` unchanged. That is right
   for int/cstr/struct elements -- their carrier bits ARE the value -- but
   wrong for a float, whose carrier bits must be reinterpreted to a `double`.

The concrete `(:: ... :float)` form worked because the method-arg float bridge
(`matched_spec->arg_types[i].kind == TY_FLOAT`, ~line 3242) emits the union
reinterpret; the abstract-tyvar path #490 added never hit that bridge.

## Fix

`src/compiler/emit_expr.c`:

- Argument-strip loop: extend `preserve_ascribe_for_bridge` to keep a
  `(:: <int64-carrier> A)` wrapper when `A` resolves (through the active spec)
  to a float width, so the ascription reaches the `EX_ASCRIBE` emit.
- `EX_ASCRIBE` emit: before the `ascribe_to_opaque` relabel, when the target is
  a `TY_TYVAR` over an int64-carrier inner and resolves to a float width, emit
  the carrier->concrete bridge (`emit_carrier_bridge`, which produces the
  `((union { int64_t s; double d; }){.s = ...}).d` reinterpret).

Only the float widths (`TY_FLOAT`/`TY_FLOAT32`/`TY_FLOAT64`) take this path;
int/bool/cstr/struct carriers are unchanged.

## Regression fixture

`tests/fixtures/constrained-generic-dispatch-float-element/` -- exercises a
float element through the generic ascription idiom with a *value-dependent*
predicate (1.5 in-unit, 3.25 out-of-unit), so a bit-mangled carrier is caught
(the pre-#490-residual `Enc [float]` fixtures returned a constant and could
not). Full `bash tests/run.sh`: 1751 passed, 0 failed.
