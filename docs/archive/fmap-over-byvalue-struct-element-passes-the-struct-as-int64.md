---
title: "`fmap` over an `Option` of a by-value struct does not compile: the by-value spec invokes the mapper as `(Pt (*)(void *, int64_t))g.fn` and passes the `Pt` payload to the int64 slot"
category: Archive
description: "`(fmap (some (Pt 1.25 2.5)) sw)` with `Pt` a two-float defstruct fails in C: `incompatible type for argument 2 of '(tur_adt_Pt (*)(void *, int64_t))g.fn'`. The failure is inside `__inst_Functor_fmap_Option__spec__..._Pt`, the by-value specialisation's own body, and is identical whether the mapper is a global defn, a lambda, or a fn parameter -- so it is the spec's invocation cast, not the argument packing. The interpreter answers 2.5."
---

# `fmap` over a by-value struct element casts the mapper's argument to int64

**RESOLVED 2026-09-09** via fix direction 1 -- and the report's diagnosis was
half right. The spec's cast was not the wrong half; it already spelled the
one convention every thunk shape shares for a wide by-value ADT (argument as
an int64 box pointer, result by value). Two things disagreed with it:

- **The generated wrapper's RESULT.** `make_poly_wrapper_ex` carried a
  by-value result type on the wrapper only for a parametric monomorph
  (`irf->kind == TY_APP`, the M7 rule). A plain `defstruct` result (TY_ADT)
  stayed the int64 carrier, so the wrapper took the generic fat-return path
  and malloc-boxed the struct -- which the spec's by-value cast then read as
  the struct itself. The rule now admits a non-heap by-value product TY_ADT,
  the same shape test the wrapper's own by-value PARAM arm uses one screen
  up. A lambda's typed thunk already returned the struct by value, which is
  why only the global-defn shape would have misread even after the argument
  fix.
- **The spec's ARGUMENT.** The typed-carrier dispatch spelled the wide slot
  `int64_t` (B4 slice 2) but never boxed the value: that path was written for
  an argument that was ALREADY the carrier (a boxed monomorph element bound
  raw by a `match`), and a struct bound out of an `Option` is the aggregate
  itself. The dispatch now spills such a value and passes its address, as
  `fat_dispatch_box_arg` does for a fat parameter -- and to keep B4's raw
  carrier binders untouched, both B4 match-binder sites record their
  `int64_t` C type in the emitted-local registry, which the spill consults.
  A spec parameter emitted as the carrier (`emit_carrier_holds_byval`) passes
  through the same way. This is what `hkt-cata-wide-byvalue-carrier` pinned,
  and it regressed on the first cut until the registry record went in.

Pinned by `tests/fixtures/fmap-byvalue-struct-element`: a global defn, an
inline lambda and a forwarded fn parameter over `(Option Pt)`, a `none`, and
the same spec shape over `(Result Pt cstr)`, agreeing on both back ends.
Compiled 2910/0, interpreted 2002/0.

---

**Severity: medium.** Loud (a C compile error, never a wrong answer), but it
means `fmap`/`bind` over a container whose element is a by-value `defstruct`
does not work at all, on the compiled back end. The interpreter is fine.

Found while fixing `local-fn-value-into-rank2-slot-gets-a-by-name-wrapper`:
its by-value-struct control failed for a reason the fix did not touch, and the
pre-fix build fails the same way on a global defn.

## Repro (2026-09-09)

```turmeric
(defstruct Pt [x : float y : float])
(defn sw [p : Pt] : Pt (Pt (.y p) (.x p)))
(defn main [] : int
  (let [r (fmap (some (Pt 1.25 2.5)) sw)]
    (println (.x (unwrap-or r (Pt 0.0 0.0)))))
  0)
```

```
$ tur run p.tur
In function '__inst_Functor_fmap_Option__spec__tur_adt_Option__Pt_tur_adt_Option__Pt_int64_t':
error: incompatible type for argument 2 of '(tur_adt_Pt (*)(void *, int64_t))g.fn'
 |   tur_adt_Pt __ps_192 = (((tur_adt_Pt (*)(void*, int64_t))g.fn)(g.env, v_912));
$ tur interpret p.tur
2.5
```

Three mapper shapes -- a global defn, an inline lambda, and a fn-typed
parameter forwarded from an enclosing defn -- fail with the same line, so the
argument side is not the variable.

## Root cause -- read from the emitted C, not yet from the emitter

The by-value specialisation of `fmap` for `(Option Pt)` is emitted with a
`tur_poly_fn_t g` and invokes it as

```c
((tur_adt_Pt (*)(void*, int64_t))g.fn)(g.env, v_912)   /* v_912 : tur_adt_Pt */
```

The RESULT position was specialised to the concrete `tur_adt_Pt` (that half of
the invocation cast is right), but the ARGUMENT position kept the int64
carrier while the value being passed is the by-value struct the spec just
matched out of the container. So the spec is half-specialised: result by
value, argument by carrier. A float element takes a different path entirely
(the carrier base instance, with a float-retyped wrapper) and works; an `any`
element specialises both positions to `tur_tagged_t` and works. A by-value
struct is the case where the two halves disagree.

Whoever emits that invocation cast (the spec body emission for a rank-2
`:fn` method parameter -- look for where the concrete result type is
substituted into `R (*)(void *, int64_t)`) is substituting the result and not
the argument.

## Fix directions

1. **Specialise the argument position too.** When the spec's element type is a
   by-value aggregate, the invocation cast's parameter must be that aggregate
   (`(tur_adt_Pt (*)(void *, tur_adt_Pt))`), and every mapper shape already
   produces a thunk of exactly that C signature (the wrapper for a global
   defn, the typed thunk of a lambda, the fat box of a parameter). This is
   what the `any` element already gets.
2. **Or box the argument at the invocation**, passing a pointer to the struct
   through the int64 slot and having the mapper side unbox -- the shape the
   carrier base uses. Worse: it makes the by-value spec pay the carrier's
   boxing, which is what the spec exists to avoid.

A fixture wants the three mapper shapes over `(Option Pt)`, and the same over
`(Result Pt E)` or a `Vec` to check the spec emission is shared.

## Not this bug

`local-fn-value-into-rank2-slot-gets-a-by-name-wrapper` (archived) was the
argument PACKING for a local; this reproduces with a global defn and predates
that fix.
