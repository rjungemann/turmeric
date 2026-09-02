# The CPS let-binder bridge claims parity with a site that has since outgrown it

**Severity: low** -- LATENT. The divergence is real and verified by reading;
**no repro was found**, and the gate is inert for every shape reached so far.
Filed for the next person who hits a double-unbox in a CPS-transformed body, so
they start here instead of re-deriving it.

**Status:** OPEN. Filed 2026-09-02 by the sweep that
[control-form-around-if-double-unboxes-carrier-arms](../archive/control-form-around-if-double-unboxes-carrier-arms.md)
asked for in its "Related" section -- "worth a sweep for the remaining
`fn_body_tail_emits_byvalue_carrier_abi` callers rather than a fifth round of
this."

## The finding

`emit_cps_ir.c:6507` is an explicit mirror of the direct emitter's
`init_carrier_to_byval` bridge, and says so:

> `cps-result-unbox-dropped`: mirror the direct emitter's `init_carrier_to_byval`
> bridge (emit_expr.c, EX_LET). ... **Same gate as the direct site**, so it is
> inert whenever the init already yields the aggregate.

It is no longer the same gate. The direct site (`emit_expr.c`, the EX_LET
binding loop) carries a **position check** the mirror does not:

```c
/* emit_expr.c -- the direct site */
bool init_val_recorded_byval_agg = false;
if (emit_str_is_bare_ident(iv)) {
    const char *lvty2 = emit_localvar_lookup_ctype(iv);
    init_val_recorded_byval_agg =
        lvty2 && strcmp(lvty2, bind_c) == 0 &&
        strcmp(lvty2, "int64_t") != 0 && strchr(lvty2, '*') == NULL;
}
bool init_carrier_to_byval = !bind_is_ptr_repr &&
    strcmp(bind_c, "int64_t") != 0 &&
    init_bv.kind != TY_UNKNOWN &&
    !init_val_recorded_byval_agg &&                        /* <-- absent in the mirror */
    !fn_body_tail_emits_byvalue_carrier_abi(ctx, init);
```

```c
/* emit_cps_ir.c:6507 -- the mirror */
if (rhs && bct && strcmp(bct, "int64_t") != 0 &&
    strchr(bct, '*') == NULL &&
    init_bv.kind != TY_UNKNOWN &&
    !fn_body_tail_emits_byvalue_carrier_abi(ce->ctx, t->as.letraw.e)) {
```

`fn_body_tail_emits_byvalue_carrier_abi` is an **Expr-level** predicate -- "what
would this Expr naturally emit". The missing term is the **position-level**
question -- "what representation does the value in hand have HERE". That is
precisely the distinction four resolved reports in this family have each moved
one more site onto, and this mirror is a site that has not moved.

## Why no repro was found

Two conditions must hold together, and the shapes tried satisfied only one:

1. The binder's C type (`bct`) must be the by-value aggregate rather than the
   carrier -- otherwise the gate is inert before the predicate is consulted.
2. The init value must be a value something ALREADY bridged (a merge temp), so
   bridging again is the second unwrap.

An effect-`perform` body does route through the CPS path (143 `__cps` functions
emitted for a three-line program), and its env struct really does hold
`tur_adt_Result__Handle__int f0` -- but the `let` binder itself came out
`int64_t r_1433`, the carrier, so condition 1 failed and no bridge was
attempted. A `^fat` higher-order call did not reach the `letraw` path at all.
Getting both conditions at once is what the next investigation needs.

## Repro attempts that did NOT trigger it

Recorded so the next person does not repeat them:

```turmeric
; CPS path taken, binder is the carrier -- gate inert, output correct.
(defeffect Ask [] :int)
(defn go [take-ok : bool] : int
  (let [r (if take-ok (mk-ok 7) (mk-err 11))]
    (+ (perform (Ask)) (if (ok? r) 100 200))))
```

```turmeric
; A ^fat higher-order call does not reach the letraw path.
(defn apply1 [^fat f : (fn [int] int) x : int] : int (f x))
```

## Fix direction

Add the missing term, which means giving `emit_cps_ir.c` access to the same
position question the direct site asks. `emit_arm_is_recorded_byval_agg` is
`static` in `emit_expr.c`, and the direct site hand-rolls the check inline
rather than calling it -- so there are really two cleanups here, and doing them
together is what makes the mirror stay a mirror:

1. **Extract the check once.** The direct site's `init_val_recorded_byval_agg`
   is `emit_arm_is_recorded_byval_agg` with the binder's C name in place of the
   type lookup. One shared non-static helper taking `(value, expected C type)`
   would serve the `emit_if` arms, `bridge_control_value_to_byvalue_temp`, the
   direct let-binding init, and this mirror.
2. **Then add the term to the mirror**, and the comment's "same gate as the
   direct site" becomes true again.

Do NOT add the term speculatively without a repro: the gate currently guards
`cps-result-unbox-dropped`'s fixtures, and a change to a path with no failing
case is unverifiable in the direction that matters.

## Related

Every one of these moved one site from the type/Expr question to the position
question. This report is the remaining site:

- [`control-form-around-if-double-unboxes-carrier-arms`](../archive/control-form-around-if-double-unboxes-carrier-arms.md)
  -- the sweep that found this; note its own fix needed a second half the
  report did not anticipate (the `emit_if` merge temp was never RECORDED).
- [`byvalue-product-tail-var-double-unboxed-nonparametric`](../archive/byvalue-product-tail-var-double-unboxed-nonparametric.md)
- [`result-block-value-double-unboxed`](../archive/result-block-value-double-unboxed.md)
- [`cps-result-unbox-dropped`](../archive/cps-result-unbox-dropped.md) -- the
  report that ADDED the mirror being questioned here.

## Guides to update when fixed

- [docs/guides/value-representations-guide.md](../guides/value-representations-guide.md)
  -- its closed-cells table is where this family is tracked.
