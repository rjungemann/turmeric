# `rc/of` over a by-value Option/Result monomorph: `rc-payload` reads the first word, and the box has no drop glue

**RESOLVED 2026-09-04.** Both defects fixed; fixture
`tests/fixtures/rc-of-byvalue-aggregate-payload` (leak-checked). Two things the
filing got wrong are corrected below: the scope ("no in-tree program takes this
path" -- rc.tur's own Foldable/Functor API does) and the fix route for (1) (the
`*-byval` twin redirect it named is a dead stub).

**Severity: medium** -- a SEGV on the DEFAULT path for a shape `tur check`
accepts, plus a leak.  Found 2026-09-03 probing the one place the
"drop glue on the monomorph" route would have earned a separate emitted
symbol (the `rc/of` control-block path), while closing
[value-struct-payload-sum-monomorph-box-has-no-owner](../archive/value-struct-payload-sum-monomorph-box-has-no-owner.md).

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

## Fix directions (as filed)

- (1) is a bridge-side fix: at `(rc-payload r)` with an aggregate `A`,
  bind the result as the pointer `rc_get_value` returns, exactly as the
  by-value readback already dereferences it.  Or give `rc-payload` a
  `*-byval` twin the redirect can retarget to.
- (2) is `emit_adt_byval_drop_glue` extended to a by-value sum MONOMORPH
  whose arm is `adt_field_is_ros_pointer_box`, emitted beside its typedef
  and passed as the `rc_cb_alloc` drop_fn.  The glue body is the same tag
  walk `boxed_struct_payload_walk` emits inline today.

Not started; the shape has no users.

---

# Resolution, 2026-09-04

## The scope was wider than filed, in two directions

**"No in-tree program takes this path" is wrong.** `stdlib/rc.tur`'s own
`Functor` and `Foldable` instances are written in terms of `rc-payload`
(`(fmap [container g] (rc/of (g (rc-payload container))))`, and the two folds).
So `(foldl r 0 f)` over an `rc<Rat>` -- a plain by-value struct, no sums, no
parametric anything -- segfaulted through the stdlib API. Verified against the
pre-fix compiler by reverting the two changed files and re-running. The fixture
carries that case.

**And it is not specific to a parametric monomorph.** A NON-parametric `:copy`
sum reaches the identical shape: its ctor returns the ADT by value, `rc/of`
takes the wrapper-cell arm, and `rc-payload` dereferences the cell. Both spellings
are in the fixture; the parametric one in the filing was the narrower case.

## (1) The named fix route no longer exists

The filing offered "give `rc-payload` a `*-byval` twin the redirect can retarget
to". `emit_abi_try_byval_twin_redirect` (emit_module.c) is a dead stub -- it
`return false`s unconditionally, retired by structdef-retirement DS-D, because
it keyed on a struct-headed receiver app that no `Type` can form any more. A fix
built on it would have been a no-op that looked plausible in review.

The bridge-side alternative does not work either, for a reason worth stating:
the call site is already CORRECT. The caller emits
`*(tur_adt_Result__Rat__int *)(intptr_t)(x)`, which is exactly right for an
aggregate carrier. Only the callee was wrong, and the callee is one inline-C
body shared by every `A` -- it cannot be specialized, and it has no way to ask
about `A` at all.

**So the fact has to travel in the value.** `rc/of` records it in the control
block: `cb->reserved[2]` means "cb->value IS the payload's int64 carrier, do not
dereference it", set for an adopted ctor pointer, a `:heap` handle, and a
wrapper cell whose payload is an aggregate. `rc-payload` branches on it.

A spare `reserved` byte rather than a new `RCK_*` kind, because the fact is
orthogonal to the kind -- an `RCK_STRUCT` block with a walker holds an aggregate
too -- and folding them together would make the two mutually exclusive. Zero is
the scalar default, so a block allocated by anything that has not been taught
about this reads exactly as it did before.

## (2) Per-site glue, not a monomorph twin

The filing's route was `emit_adt_byval_drop_glue` extended to a monomorph. What
landed is smaller: `boxed_struct_payload_walk` -- the same tag walk the carrier
path already emits inline -- written out as a per-`rc/of`-site
`static void __tur_rc_dropglue_N(void *v)` and passed as the `drop_fn`. The
control block wants a function POINTER, which is the one thing an inline walk
cannot be; that is the whole reason this site needed a symbol at all, and it
does not need a *reusable* one.

Emitted only when there is no glue already (a def with rc/ref/weak fields takes
the existing `rc_cb_alloc_struct` branch) and only for the wrapper cell (the
adopted-pointer and `:heap` arms hand `rc_set_value` a pointer another release
path owns).

## Two things left deliberately

- **`rc_cb_alloc(0, 21, ...)` -- the `21` is `TY_APP`, not a value type.** The
  `value_type` byte only feeds `default_drop_fn_for_type`, which dispatches on
  `RC_VT_REF`/`RC_VT_RC`/`RC_VT_WEAK`; 21 matches none of them and neither does
  the `TY_ADT` 19 it "should" be, so it is inert. Left as-is rather than
  churning emitted C for no behaviour change.
- **The `drop_glue_tur_adt_<Name>` lookup in `rc/of` is guarded on
  `type.kind == TY_ADT`,** so a PARAMETRIC ADT with rc/ref/weak fields still
  gets no glue. Fixing that properly needs the monomorph glue twin this
  resolution avoided (the base glue expects the base layout and cannot be handed
  a monomorph pointer). Not reachable from the repro here, and worth its own
  report if a program hits it.
