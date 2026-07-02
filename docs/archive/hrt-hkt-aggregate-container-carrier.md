# Higher-kinded rank-2: by-value aggregate containers don't fit the erased carrier

> **RESOLVED.** By-value aggregate containers now flow through the erased poly
> carrier. A **forall** carrier (uniform int64) heap-boxes an aggregate arg at
> the invocation and derefs it in the poly-wrapper (`poly_agg_arg_mask`), and
> boxes an aggregate result via the carrier-spill shim (gated by
> `poly_wrap_.boxes_aggregate`) which the call site unboxes; a **typed** `:fn`
> carrier / monad continuation is left byte-for-byte unchanged (consumed by value
> via the concrete-cast call site). The `TUR-E0306`/`E0307` validation stays;
> `TUR-E0297` is retired. See `emit_agg_box`/`emit_agg_unbox` (emit_expr.c),
> `make_poly_wrapper` + the `elab_poly_call` result-type propagation
> (elab_call.c), and `ensure_aggregate_spill_shim` (emit_module.c). Fixtures:
> `hrt-hkt-aggregate-container/`, `hrt-rank2-aggregate-arg/`.

**Summary.** Slice 3 of `constrained-hkt-forall-plan` lets a rank-2 `forall`
parameter quantify a higher-kinded `f : * -> *` and instantiate it to a
concrete container at call sites. This works end-to-end for *carrier-compatible*
containers (parametric opaque / heap constructors, whose value is the int64
carrier), but a **by-value aggregate product** container (a `defstruct`/flat
`defadt`, e.g. the stdlib `Option`) does not fit the erased
`tur_poly_fn_t = {void*env; int64_t(*fn)(void*,int64_t)}` carrier: the wrapper
thunk would receive/return the aggregate through an `int64_t` slot and the C
compiler rejects it (`incompatible type`, `aggregate value used where an
integer was expected`).

Slice 3 rejects this case cleanly with **TUR-E0297** at the instantiation site
rather than emitting broken C. This report tracks lifting that restriction.

**Severity:** medium (a real expressiveness hole for the lens/optic use-cases
that want `(f a)` over ordinary by-value containers; worked around by rejecting,
not miscompiling).

## Minimal repro

```turmeric
;; Option is a defstruct -> single-variant by-value product.
(defn olen [x : (Option int)] : int 7)

(defn use-c [g (forall [(f :: * -> *)] (-> (f int) int))] : int
  (g (some 5)))

(defn main [] : int (println (use-c olen)) 0)
```

```
error: rank-2 call: by-value aggregate container is not yet supported through
the erased poly carrier ... (TUR-E0297)
```

A parametric opaque container works today:

```turmeric
(defopaque Box [a] :int)              ;; int-carried, carrier-compatible
(defn blen [x : (Box int)] : int 7)
(defn use-c [g (forall [(f :: * -> *)] (-> (f int) int))] : int
  (g (mkbox 5)))                       ;; OK -- flows through the carrier
```

## Root cause

- Carrier: `tur_poly_fn_t` has one int64 payload slot (`emit_module.c` preamble
  literals). A rank-2 arg is boxed into it by `EX_POLY_WRAP` /
  `make_poly_wrapper` (`elab_call.c`), whose thunk is
  `int64_t __poly_N(void *env, int64_t x0)`.
- `type_uses_carrier_abi` (`emit_core.c:341-373`): generic parametric ADT/
  `TY_APP` and `:heap` ADT apps are int64-carrier-compatible; a by-value flat
  product (`adt_is_byvalue_product` / `adt_app_is_byvalue_product`,
  `types.c:2312`/`1558`) is **not** -- it flows by value and needs a box/deref
  bridge.
- The B4 "wide by-value ADT as int64 carrier element" machinery already exists
  (heap-box at the constructor store `types.c:1225-1229`; the `int64_t` cast in
  the poly-call at `emit_expr.c:2739`; the byval<->carrier bridge predicate
  `emit_type_is_byvalue_adt` `emit_expr.c:359-382`). The gap is wiring the poly
  **wrapper thunk** to deref a boxed wide-byval *argument* at entry and re-box
  its aggregate *return* -- today only match-binder reads and by-value-product
  *fields* deref.

## Fix directions

Wire the box/deref bridge into `make_poly_wrapper` / `EX_POLY_WRAP` for
by-value aggregate `(f a)` arguments and returns:

1. At the pass site, when the passed function's container parameter is a
   by-value aggregate, generate a wrapper whose int64 arg is a heap-box pointer,
   deref+copy it into the aggregate the inner fn expects (reuse the B4
   reconstruct at `emit_expr.c:2694-2712`).
2. Symmetrically box an aggregate return through the carrier-spill shim
   (`ensure_aggregate_spill_shim`, `emit_expr.c:6253-6273`) -- already handled
   for `TY_APP` results in `make_poly_wrapper` (`elab_call.c:5121-5135`); extend
   to non-parametric wide by-value `TY_ADT` results.
3. Drop the TUR-E0297 guard in `hrt_validate_hk_actual` (`elab_call.c`) once the
   above lands, and add positive fixtures over `(Option int)` / `(list int)`.
