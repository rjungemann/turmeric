# A `let`/`do` wrapping an `if` whose arms are carrier producers unboxes one time too many

**Severity: medium** (clean build break, `cc`-level diagnostic with no `.tur`
attribution). Found 2026-08-28 getting `turmeric-spices` CI green, against
`tur v0.40.0` / turmeric `5c9d533`.

**Verified 2026-08-28** on a freshly built `v0.40.0` (macOS arm64 / Apple
clang), with the four-variant narrowing and the source-level root cause below.

## Summary

This is the **residue of the 2026-08-21 fix** to
[`byvalue-product-tail-var-double-unboxed-nonparametric`](../archive/byvalue-product-tail-var-double-unboxed-nonparametric.md).
That fix added a position-sensitive guard (`emit_arm_is_recorded_byval_agg`) to
both `emit_if` arms. The `do`/`let` companion bridge, which the source itself
identifies as doing "the same bridge", did not get it -- so an `if` nested
inside a `let` or `do` is bridged once correctly by the arms and then a second
time by the enclosing control form.

## Repro

```turmeric
(defmodule ru (export)
  (defopaque Handle :int)

  (defn mk-ok  [n : int] : (Result Handle int) ```c return tur_ok_int(n); ```)
  (defn mk-err []        : (Result Handle int) ```c return tur_err_int(0); ```)
  (defn probe  [] : int ```c return 0; ```)

  ;; let around if -- one unbox too many
  (defn with-let [] : (Result Handle int)
    (let [c (probe)]
      (if (= c 0)
        (mk-err)
        (mk-ok 7))))

  ;; same thing with the `if` as the whole body -- fine
  (defn without-let [] : (Result Handle int)
    (if (= (probe) 0)
      (mk-err)
      (mk-ok 7)))

  (defn main [] : int
    (if (ok? (without-let)) 1 0)))
```

## Observed

`tur check` is silent. In `with-let`, each arm correctly unboxes the carrier the
inline-C helper returned -- and then the enclosing `let` unboxes the result
again:

```c
static tur_adt_Result__Handle__int ru__with_hylet() {
        tur_adt_Result__Handle__int __t163;
        {
            int64_t __ps_164 = (ru__probe());
            int64_t c_1373 = __ps_164;
            tur_adt_Result__Handle__int __t165;
            if ((c_1373) == (INT64_C(0))) {
                int64_t __ps_166 = (ru__mk_hyerr());
                __t165 = (*(tur_adt_Result__Handle__int *)(intptr_t)(__ps_166));
            } else {
                int64_t __ps_167 = (ru__mk_hyok(INT64_C(7)));
                __t165 = (*(tur_adt_Result__Handle__int *)(intptr_t)(__ps_167));
            }
            __t163 = (*(tur_adt_Result__Handle__int *)(intptr_t)(__t165));  /* <-- already a struct */
        }
        return __t163;
}
```

```
error: operand of type 'tur_adt_Result__Handle__int'
       (aka 'struct tur_adt_Result__Handle__int')
       where arithmetic or pointer type is required
```

`without-let` compiles and runs correctly, so the carrier->by-value seam itself
is sound; it is the extra control-form nesting that inserts the second unwrap.

## What narrows it

`let` is not the variable, and neither is `Result`:

| Variant | Result |
| --- | --- |
| `let` around `if`, arms are inline-C carrier producers | **error** |
| **`do`** around `if`, same arms | **error** |
| `let` around a bare **call** (no `if`) | compiles |
| `let` around `if`, arms built in Turmeric with `ok`/`err` | compiles |

So the trigger is: **a control form (`let`/`do`) wrapping an `if`/`match` whose
arms are carrier-emitting producers.** Rows 3 and 4 are what make it precise --
a direct call tail is handled correctly, and arms that already emit the by-value
aggregate are handled correctly. Only the combination of an arm that *needed*
bridging with an enclosing control form that bridges again fails.

## Root cause

`src/compiler/emit_expr.c:2106`, `bridge_control_value_to_byvalue_temp()`. Its
own comment names the relationship:

> emit_if_value applies the same bridge per arm inline; **this is the do/let
> companion.**

```c
static char *bridge_control_value_to_byvalue_temp(EmitCtx *ctx, Buf *body,
                                                   char *v, const Expr *last) {
    Type bv = fn_body_tail_byvalue_carrier_type(ctx, last);
    if (bv.kind != TY_UNKNOWN &&
        !fn_body_tail_emits_byvalue_carrier_abi(ctx, last))
        return emit_carrier_bridge(ctx, body, v, CK_CARRIER, CK_CONCRETE, bv);
    return v;
}
```

The guard is `fn_body_tail_emits_byvalue_carrier_abi(ctx, last)` -- an
**`Expr`-level** predicate. For an `EX_IF` it recurses into the arms
(`emit_expr.c:1351`), both arms are carrier-producing inline-C calls, so it
answers **false**, and the bridge fires.

But by the time this runs, `emit_if` has *already* bridged each arm into
`__t165`, whose declared C type is the by-value struct. The predicate is asking
"what would this Expr naturally emit" when the question that matters is "what
representation does the value in hand have *here*" -- exactly the distinction
the archived report worked out, and exactly why the 2026-08-21 fix went to the
merge site rather than into the predicate.

`emit_arm_is_recorded_byval_agg` (`emit_expr.c:2005`) answers the second
question by consulting the localvar side table, and it is called at
`emit_expr.c:3065` and `:3109` -- the two `emit_if` arms, and nowhere else. The
do/let companion 900 lines earlier never got it.

## Fix direction

Mirror the landed precedent. In `bridge_control_value_to_byvalue_temp`, consult
the position-sensitive check before bridging:

```c
    if (bv.kind != TY_UNKNOWN &&
        !emit_arm_is_recorded_byval_agg(ctx, v, bv) &&      /* <-- add */
        !fn_body_tail_emits_byvalue_carrier_abi(ctx, last))
        return emit_carrier_bridge(...);
```

The preconditions hold: `v` is `__t165`, a bare identifier, and its recorded C
type is the by-value struct, so the lookup answers correctly. This is the same
one-predicate change that fixed the `emit_if` arms, applied to the companion the
earlier fix did not reach.

`emit_arm_is_recorded_byval_agg` is declared `static` above line 2106, so no
forward declaration is needed.

The blast-radius argument from the archived resolution transfers directly: at
the vec/map multiword-element seams the value's recorded C type **is** the
carrier, so the predicate returns false and the bridge those fixtures need still
fires. That resolution named ten fixtures that a *type*-keyed widening broke and
a *position*-keyed one did not; this is the position-keyed kind. Re-run them to
confirm:

```
defopaque-struct-payload-through-unsafe-lift   generic-inline-c-struct-through-unsafe
map-move-typed-value                           map-multiword-struct-value
map-narrow-struct-value                        typeclass-assoc-type-method-return
typeclass-assoc-type-parametric-struct-element vec-multiword-struct-element
vec-multiword-struct-eq                        vec-multiword-struct-mutate
```

A regression fixture should cover **both** the `let` and the `do` wrapper (row 2
above), and assert field *values* rather than only that the program compiles --
the archived report makes that point, and a double-unbox that happened to
type-check would still read the wrong bytes.

## Where it bit

`spices/postgres/src/postgres/notify.tur`, `notify-poll`.

**Workaround:** make the `if` the whole function body, or hoist the block into
its own defn -- the same workaround the two earlier reports in this family
prescribe, still in use in `spices/secret`.

## Related

Same family, all resolved, all worth reading before touching this:

- [`byvalue-product-tail-var-double-unboxed-nonparametric`](../archive/byvalue-product-tail-var-double-unboxed-nonparametric.md)
  -- the direct predecessor; its fix is the one this extends.
- [`result-block-value-double-unboxed`](../archive/result-block-value-double-unboxed.md)
  -- the same double-unbox at the let/do tail, fixed 2026-08-18 in
  `expr_emits_byvalue_carrier_abi`.
- [`cps-result-unbox-dropped`](../archive/cps-result-unbox-dropped.md) -- the
  mirror image, where the conversion is omitted where it *is* needed.

The recurring theme across all four: the emitter keeps deciding "does this value
need unboxing" from the value's *type* or its *Expr shape* rather than from the
representation it actually has at that point. Each fix has moved one more site
to the position-sensitive question. Worth a sweep for the remaining
`fn_body_tail_emits_byvalue_carrier_abi` callers rather than a fifth round of
this.

## Guides to update when fixed

- docs/guides/value-representations-guide.md -- the carrier/by-value seam rules.
- docs/guides/inline-c-results-guide.md -- it recommends the
  `tur_ok_int`/`tur_err_int` builders that produce the arms in this repro.
