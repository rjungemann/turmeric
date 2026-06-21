---
title: (list ...) element-homogeneity helper is typed at the int64 carrier, rejecting by-value aggregate elements (e.g. a list of Options)
category: Carrier <-> Concrete ABI -- polymorphic-defn monomorphization at the (list ...) homogeneity check
severity: Medium. Hard C compile error (not a silent miscompile) the moment a
  `(list ...)` literal holds two or more by-value aggregate elements (Option,
  Result, Pair, value-struct, ...). Blocks composing the element-polymorphic
  `(list ...)` surface (#473) with the by-value HKT thread -- e.g. building a
  `(Cons (Option A))` literal, which a JSON `Encode [Cons (Option A)]` / any
  nested-container spice needs. Scalar and `:heap`-pointer elements are fine;
  only by-value aggregates trip it.
status: OPEN -- found 2026-06-21 by the composition stress matrix in
  docs/carrier-concrete-abi-crossing-audit-plan.md (gap G1).
---

# `(list ...)` homogeneity helper collapses to the carrier for by-value aggregate elements

## One-line summary

The compile-time homogeneity check the `(list ...)` macro emits --
`tur-list-homog__ [A] [a :A b :A]` (`stdlib/list.tur:196`) -- monomorphizes
to its **carrier** spec `tur_hylist_hyhomog_un_un(int64_t, int64_t)` when the
adjacent elements are by-value aggregates, but the elements themselves are
emitted as the **concrete** by-value type (`Option__int`). C rejects the
mismatch before the list is ever built.

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
  ;; building the (Cons (Option int)) literal is the failure -- before any enc:
  (println (enc @Cons (:: (list (some 42) (some 7)) (Cons (Option int)))))
  0)
```

Result (`tur run`):

```
error: incompatible type for argument 1 of 'tur_hylist_hyhomog_un_un'
note: expected 'int64_t' but argument is of type 'Option__int'
  tur_hylist_hyhomog_un_un(some__spec__Option__int_int64_t(42),
                           some__spec__Option__int_int64_t(7));
```

A list of two bare `int`s, or two `:heap`-pointer `(Cons int)`s, compiles fine
-- those fit the int64 carrier. Only by-value aggregate elements (`Option`,
`Result`, `Pair`, value-structs) break.

## Root cause (direction)

`tur-list-homog__` is a polymorphic `defn` over a single type var `A`. The
`(list ...)` lowering chains it pairwise over the elements
(`stdlib/list.tur:202` `list-homog-chain__`; elab path `elab_call.c:3708`).
When `A` resolves to a by-value aggregate, the monomorphizer does **not** mint
a per-type spec (`tur_hylist_hyhomog__Option__int(Option__int, Option__int)`);
it falls through to the generic carrier spec `..._un_un(int64_t, int64_t)`.
The arguments, however, are lowered by-value (`some__spec__Option__int(...)`),
so source (concrete) and sink (carrier) disagree with no bridge -- the same
carrier<->concrete defect as the instance-body sites, but at the
**polymorphic-defn monomorphization** site rather than an `__inst_*` site.

## Fix directions

Prefer keeping the by-value thread end-to-end:

1. Monomorphize `tur-list-homog__` on the concrete aggregate element type so
   its spec params are `Option__int`, not `int64_t`. This is the same
   "specialize on the by-value element" the container constructors already do.

Alternatively (less preferred, re-introduces a carrier hop):

2. Emit the homogeneity-check args at the carrier ABI to match the helper's
   carrier signature.

The check body is a no-op (`(void)a; (void)b;`), so option 1 only needs the
*signature* to match the by-value args; there is no body logic to port.

## Validation

When fixed, the repro above compiles and the nested-container cell of the
stress matrix (`(Cons (Option A))` over int/cstr/float) can be promoted to a
fixture. Cross-reference: gap G1 in
`docs/carrier-concrete-abi-crossing-audit-plan.md`.
