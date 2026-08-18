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
