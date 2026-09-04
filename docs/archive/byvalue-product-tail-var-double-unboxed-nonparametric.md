# A bare-var tail of a NON-parametric by-value product is still double-unboxed

**Severity:** medium (emits C that cannot compile, with no `.tur` attribution;
the parametric half of the same bug is fixed, so this is the residue)
**Found:** 2026-08-18, checking whether the scope narrowing in
[`result-block-value-double-unboxed`](../archive/result-block-value-double-unboxed.md)
left work behind. It did.

## Summary

A `let`/`do` block that evaluates to a by-value product, returned through an
`if` branch as a bare var, gets the boxed-carrier -> struct conversion applied
to a value that is already a struct. That was fixed for **parametric** apps
(`(Result H cstr)`, `(Box2 int cstr)`) via `type_is_byvalue_product_app`. It is
still live for a **non-parametric** product:

```turmeric
(defstruct Pt [x : int y : int])

(defn mk [n : int] : Pt (make-struct Pt n 0))
(defn side [x : int] : int x)

(defn via-block [n : int] : Pt
  (if (= n 0)
    (mk 0)
    (let [r (mk n)
          _ (side n)]
      (do
        (side n)
        r))))                      ;; bare-var tail

(defn main [] : int (do (side (.x (via-block 3))) 0))
```

```
error: operand of type 'tur_adt_Pt' (aka 'struct tur_adt_Pt')
       where arithmetic or pointer type is required
```

## Why it was not just widened

This is the interesting part, and the reason the fix stopped where it did.

`expr_emits_byvalue_carrier_abi`'s `EX_VAR` arm answers "is this var already a
concrete aggregate, so the caller must not deref-unbox it". Extending that
answer to non-parametric products **regresses 10 fixtures**, measured:

```
defopaque-struct-payload-through-unsafe-lift
generic-inline-c-struct-through-unsafe
map-move-typed-value
map-multiword-struct-value
map-narrow-struct-value
typeclass-assoc-type-method-return
typeclass-assoc-type-parametric-struct-element
vec-multiword-struct-element
vec-multiword-struct-eq
vec-multiword-struct-mutate
```

They fail with the mirror error (`assigning to 'tur_adt_Point' from
incompatible type 'int64_t'`), because at the vec/map element seams and the
assoc-type return a non-parametric product genuinely **does** ride the carrier
and needs the bridge. Reporting it as already-by-value suppresses a bridge it
requires.

So the same type answers differently in two positions:

| position | `tur_adt_Pt` is | needs |
|---|---|---|
| bare-var tail of an `if`-branch block | already a struct | no bridge |
| vec/map element, assoc-type return | the int64 carrier | bridge |

A broader **type** test cannot separate those -- the type is identical. The
narrowing to `TY_APP` works today only because a parametric app happens never
to appear in the second position, which is a coincidence of the current seams
rather than a principle.

## Fix direction

The predicate needs to be **position-sensitive**, not type-sensitive: the
question is "what representation does the value in hand actually have here",
which is what `emit_localvar_lookup_ctype` already answers for a bare
identifier elsewhere in this file. `emit_expr.c`'s let-binding path uses
exactly that trick to avoid a double-deref:

```c
bool init_val_recorded_byval_agg = false;
if (emit_str_is_bare_ident(iv)) {
    const char *lvty2 = emit_localvar_lookup_ctype(iv);
    init_val_recorded_byval_agg =
        lvty2 && strcmp(lvty2, bind_c) == 0 &&
        strcmp(lvty2, "int64_t") != 0 && strchr(lvty2, '*') == NULL;
}
```

Consulting the recorded C type of the emitted local, rather than re-deriving
an answer from the type, is the shape that generalizes. The obstacle is that
`expr_emits_byvalue_carrier_abi` runs on the `Expr` before the arm's value
string exists, so it has no identifier to look up -- the check would have to
move to the `emit_if` merge site, which *does* have the emitted arm text
(`t` / `el`), alongside the existing `fn_body_tail_emits_byvalue_carrier_abi`
call.

That is a larger change to a path with 10 fixtures' worth of blast radius, so
it wants its own pass rather than riding along with a spice's bug fix.

## Workaround

Give the tail a shape the call predicates already answer for -- return the
call directly rather than through a bare var:

```turmeric
(if (= n 0) (mk 0) (mk n))          ;; fine
```

or hoist the block into its own defn, which is what
`spices/secret/src/secret/kdf.tur` and `hex.tur` do for the same reason.

---

## Resolution (2026-08-21)

Fixed exactly along the report's own fix direction, and it turned out to be
small -- the analysis was the expensive part, and the report had already done
it.

### The change

`emit_arm_is_recorded_byval_agg()` asks the question the report identified:
not "is this TYPE by-value" but "what representation does the value in hand
actually have HERE". At the `emit_if` merge the arm's emitted text exists, so
the localvar side table can be consulted directly:

```c
static bool emit_arm_is_recorded_byval_agg(EmitCtx *ctx, const char *v, Type bv) {
    if (!v || bv.kind == TY_UNKNOWN || !emit_str_is_bare_ident(v)) return false;
    const char *lv = emit_localvar_lookup_ctype(v);
    if (!lv) return false;
    const char *want = emit_type_c_name(ctx, bv);
    return want && strcmp(lv, want) == 0 &&
           strcmp(lv, "int64_t") != 0 && strchr(lv, '*') == NULL;
}
```

It gates the carrier->concrete bridge in **both** `emit_if` arms. The report's
repro only exercises the else arm, but the then arm is the same code and got
the same guard.

This is the `init_val_recorded_byval_agg` shape from the let-binding path,
which is what the report pointed at. The obstacle it named --
"`expr_emits_byvalue_carrier_abi` runs on the `Expr` before the arm's value
string exists, so it has no identifier to look up" -- is why the check went to
the merge site rather than into that predicate.

### The ten-fixture blast radius did not materialize

That was the report's stated reason for not widening the type test, and it was
correct about the type test. It does not apply to a position-sensitive check:
all ten named fixtures pass unchanged --

```
defopaque-struct-payload-through-unsafe-lift   generic-inline-c-struct-through-unsafe
map-move-typed-value                           map-multiword-struct-value
map-narrow-struct-value                        typeclass-assoc-type-method-return
typeclass-assoc-type-parametric-struct-element vec-multiword-struct-element
vec-multiword-struct-eq                        vec-multiword-struct-mutate
```

-- because at those seams the value's recorded C type IS the carrier, so the
predicate returns false and the bridge they need still fires. The report's
table of "the same type answers differently in two positions" is exactly why
consulting the position works where consulting the type cannot.

Suite: 2690 passed, **0 failed**. No snapshot regenerated.

### Before / after

```c
/* before */  __t161 = (*(tur_adt_Pt *)(intptr_t)(__t163));  /* __t163 IS a tur_adt_Pt */
/* after  */  __t161 = __t163;
```

### Tests

`tests/fixtures/byvalue-product-tail-var-nonparametric/` carries the report's
repro (else arm), the mirrored then-arm shape, and a parametric
`(Box2 int int)` case so the already-fixed half cannot regress alongside the
new predicate. It asserts field VALUES, not just that the program compiles --
a double-unbox that happened to type-check would still read the wrong bytes.
`expected.c` has zero `(*(tur_adt_... *)(intptr_t)` merge sites.

### The workaround is no longer needed

The report's advice -- return the call directly, or hoist the block into its
own defn, as `spices/secret/src/secret/kdf.tur` and `hex.tur` do -- still
works, but is no longer required for this shape. Those spice files live in the
sibling `turmeric-spices` checkout and were not touched here.
