# `rc/of` over a by-value Option/Result monomorph: `rc-payload` reads the first word, and the box has no drop glue

**Severity: medium** -- a SEGV on the DEFAULT path for a shape `tur check`
accepts, plus a leak.  Found 2026-09-03 probing the one place the
"drop glue on the monomorph" route would have earned a separate emitted
symbol (the `rc/of` control-block path), while closing
[value-struct-payload-sum-monomorph-box-has-no-owner](../archive/value-struct-payload-sum-monomorph-box-has-no-owner.md).
No in-tree program takes this path; the probe is the whole population.

## Repro

```turmeric
(load "stdlib/rc.tur")
(defstruct Rat [n : int d : int])

(defn mk [] : (Result Rat int)
  (ok (make-struct Rat :n 1 :d 2)))

(defn main [] : int
  (let [r (rc/of (mk))]
    (println (.n (ok-val (:: (rc-payload r) (Result Rat int)))))
    (rc/drop r)
    0))
```

Builds; segfaults at a null address in `main`.  Emitted:

```c
tur_adt_Result__Rat__int __ps_187 = (mk());
tur_adt_Result__Rat__int *__t188 = malloc(sizeof(tur_adt_Result__Rat__int));
*__t188 = __ps_187;
RcControlBlock *__t189 = rc_cb_alloc(0, 21, NULL);          /* no drop_fn */
rc_set_value(__t189, __t188, NULL);
int64_t __ps_190 = (rc_hypayload(r_1450));
tur_adt_Rat __ps_191 = ok_val__spec__...((*(tur_adt_Result__Rat__int *)(intptr_t)(__ps_190)));
```

## Two defects

1. **`rc-payload` returns the payload's first WORD.**  `stdlib/rc.tur`:

   ```c
   return *(int64_t *)rc_get_value(r);
   ```

   Right for `rc<int>` (the value cell holds the int64).  For an aggregate
   payload the cell holds the struct, so this returns its first 8 bytes --
   the `tag`, which is `0` for `Ok` -- while the consumer's readback expects
   the int64 CARRIER, i.e. the pointer to the box (`*(T *)(intptr_t)x`).
   Null deref.  A `Some` would read tag `1` and deref address 1.  The
   accessor is inline C and cannot be specialized on `A`; the carrier
   convention for an aggregate `A` is the box pointer, so the fix is on the
   emit side (a by-value twin for `rc-payload` that returns `rc_get_value(r)`
   itself when `A` is an aggregate -- the `*-byval` twin redirect Option C
   already does for the vec helpers), not in the stdlib body.

2. **No drop glue on the control block.**  `rc_cb_alloc(0, 21, NULL)`: the
   drop_fn is NULL, so `rc/drop` frees the value cell (`__t188`) at best and
   never the `Rat *` arm box inside it -- the payload the value-struct
   report's tag walk (`boxed_struct_payload_walk`) frees everywhere else.
   This is the one site where an emitted `drop_glue_tur_adt_Result__Rat__int`
   function (the monomorph analogue of `emit_adt_byval_drop_glue`, which
   only exists for non-parametric defs) would be the right shape: the
   control block wants a function pointer, not an inline walk.

## Fix directions

- (1) is a bridge-side fix: at `(rc-payload r)` with an aggregate `A`,
  bind the result as the pointer `rc_get_value` returns, exactly as the
  by-value readback already dereferences it.  Or give `rc-payload` a
  `*-byval` twin the redirect can retarget to.
- (2) is `emit_adt_byval_drop_glue` extended to a by-value sum MONOMORPH
  whose arm is `adt_field_is_ros_pointer_box`, emitted beside its typedef
  and passed as the `rc_cb_alloc` drop_fn.  The glue body is the same tag
  walk `boxed_struct_payload_walk` emits inline today.

Not started; the shape has no users.
