---
title: Single-level constrained instance over a container whose ELEMENT is a parametric container collapses the element to the int64 carrier and misdispatches the inner method
category: Carrier <-> Concrete ABI -- witness-side element collapse + dispatch re-resolution, parametric-container element
severity: Medium-high. SILENT miscompile / hard C error. `(enc @Cons (:: xs
  (Cons (Option int))))` -- a single-level `Enc [Cons]` whose element `(Option
  int)` is itself a parametric container -- mints the spec named `..._Cons__int`
  (the element `(Option int)` collapsed to `int`) and dispatches the inner
  `(.head xs)` (a by-value `Option__int`) through the carrier representative
  `__inst_Enc_enc_int(int64_t)`: `error: incompatible type for argument 1 of
  '__inst_Enc_enc_Option'` / `'__inst_Enc_enc_int'`. The mirror of gap G2.
status: OPEN -- found 2026-06-21 while fixing G2; verified pre-existing
  (fails identically against the pre-G2 build). Gap G9 in
  docs/carrier-concrete-abi-crossing-audit-plan.md.
---

# `(Cons (Option A))` element collapses to the carrier at the witness, then misdispatches

## One-line summary

G2 fixed `(Option (Cons A))` -- the OUTER container is parametric, the inner
element collapsed. This is the **mirror**: `(Cons (Option A))`, where the inner
ELEMENT of a single-level `Enc [Cons]` is itself a parametric container. The
`@Cons` witness collapses the `(Option int)` element to `int` (the spec is named
`__inst_Enc_enc_Cons__spec__..._Cons__int`, not `..._Cons__Option__int`), so the
spec body's `(.head xs)` -- a by-value `Option__int` -- is dispatched on the
int64 carrier representative.

## Minimal repro (self-contained)

```turmeric
(defclass Enc [a] (enc [x] : cstr))
(definstance Enc [int]
  (enc [x] : cstr
    ```c
    char *buf=(char*)malloc(32); snprintf(buf,32,"%lld",(long long)x); return buf;
    ```))
(definstance Enc [Option]
  [(Enc A)]
  (enc [x] : cstr (if (.is-some x) (enc (.value x)) "null")))
(definstance Enc [Cons]
  [(Enc A)]
  (enc [xs] : cstr (enc (.head xs))))

(defn main [] : int
  (do
    ;; element is itself a parametric container:
    (println (enc @Cons (:: (list (:: (some 5) (Option int))
                                  (:: (some 9) (Option int)))
                            (Cons (Option int)))))
    0))
```

Result (`tur run`):

```
error: incompatible type for argument 1 of '__inst_Enc_enc_int'
   return __inst_Enc_enc_int((__t ? (Option__int){.is_some=__t->is_some,
                                                  .value=__t->value} : (Option__int){0}));
note: expected 'int64_t' but argument is of type 'Option__int'
```

## Root cause (direction)

Two crossings stacked, both on the **single-level witness** path (not the G2
nested-instance-body path):

1. **Witness-side element collapse.** The `@Cons` witness dispatch records the
   element `A -> (Option int)` but the minted `Enc [Cons]` spec collapses it to
   `int` (clone name `..._Cons__int`), so the receiver cell layout is
   `Cons__int` where the head is really an `Option__int` aggregate. This is the
   value-side chokepoint (`emit_var_spec_arg_type`) not preserving a parametric
   element type through the witness binding.
2. **Inner dispatch on the collapsed element.** Even with the cell typed
   correctly, `(.head xs) : Option__int` must dispatch `enc` to
   `__inst_Enc_enc_Option__spec__..._Option__int`, not the carrier
   `__inst_Enc_enc_int`. This is the dispatch chokepoint
   (`emit_reresolve_disp_type` + the G2 by-value-spec redirect) applied one
   level in.

G2's fix routes the dispatch when the *instance body* dispatches on a parametric
container (`(Option (Cons A))`). This mirror needs the same routing for the
*witness-minted* `Enc [Cons]` spec whose element is parametric -- i.e. the
witness must preserve `(Option int)` as the Cons element (so the spec is
`..._Cons__Option__int` with a `Cons__Option__int` cell), and the inner `(.head
xs)` dispatch must re-resolve to the Option instance's by-value spec.

## Related

- **G2** (`docs/archive/constrained-instance-dispatch-nested-parametric-element-carrier-collapse.md`):
  the outer-parametric case, now fixed. This is its inner-parametric mirror.
- **G1/G4**: the *construct* and *consumer* sides of `(Cons (Option A))` --
  building such a list, and walking it through the int-carrier list API. This
  G9 is the *encode/dispatch* side of the same value.
- Belongs on the value-side + dispatch-side chokepoints of
  `docs/carrier-crossing-recovery-routing-plan.md` (R1), not as a witness-path
  special case.

## Validation

When fixed, the repro prints the encoded inner option and the `(Cons (Option
A))` cell of the composition stress matrix promotes to a fixture.
Cross-reference: gap G9 in `docs/carrier-concrete-abi-crossing-audit-plan.md`.
