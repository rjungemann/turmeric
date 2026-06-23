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
status: RESOLVED 2026-06-21. The dead homogeneity call is now elided in
  emit_stmt when an argument is a by-value (non-heap) aggregate; the
  elaboration-time homogeneity check (and its TUR-E0001 on a genuinely
  heterogeneous list) is unchanged, and scalar/float/cstr/heap-pointer lists
  keep their existing codegen (zero snapshot churn). Fixture:
  tests/fixtures/list-homog-byvalue-aggregate-element. `bash tests/run.sh`
  green (1738 passed, 0 failed). See "Resolution" below.
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

## Resolution (2026-06-21)

Neither fix direction above was taken. Investigation showed the homogeneity
helper **cannot** be monomorphized the way the container constructors are: it
has an **inline-C body** (`(void)a; (void)b;`), and an inline-C signature is
fixed at the carrier -- there is no per-type C body to regenerate. More to the
point, the call is **dead at runtime**: the homogeneity it enforces is a pure
elaboration-phase concern (a genuinely heterogeneous list is still rejected
with `TUR-E0001` during `tur check`, before any emission), and the body does
nothing. The emitted carrier call was therefore pointless -- and the only
reason it ever "worked" for scalars is that an int/float/cstr element coerces
into the int64 carrier (float via an 8-byte bit-cast). A by-value aggregate
(`Option__int`) cannot, which is the only case that broke.

**Fix (`src/compiler/emit_stmt.c`, `EX_CALL` case).** When the callee is
`tur-list-homog__` and an argument's resolved type is a by-value (non-`:heap`)
aggregate (`type_uses_carrier_abi(at) && !type_is_heap_struct(at)`), emit
nothing -- the element is still constructed once by `list-build__`. Scalar,
float, cstr, and `:heap`-pointer elements keep coercing into the carrier and
keep their existing emitted call, so **no existing fixture snapshot churns**.
This also removes a latent redundant evaluation of every list element (the
homogeneity chain previously re-emitted each element 2-3 times).

**Fixture.** `tests/fixtures/list-homog-byvalue-aggregate-element` -- a
3-element `(Cons (Option int))` literal that now constructs and round-trips
through the typed accessors (`42 / 7 / 100`). `bash tests/run.sh` green
(1738 passed, 0 failed). Heterogeneous lists are still rejected at elaboration
(verified: `(list 1 "x")` -> `TUR-E0001`).

### Newly-exposed downstream gaps (NOT this report)

Fixing the homogeneity check let a `(Cons (Option int))` *build*. Two distinct,
separately-tracked crossings remain before such a list is fully consumable:

- **Generic int-carrier list helpers over a by-value-aggregate head segfault.**
  `Cons` is `:heap` with `(tail :int)`, so `(:: xs :int)` + `list-length`
  walks the chain as `{ int64 head; int64 tail; }`. A 16-byte by-value
  `Option__int` head shifts `tail`, so the generic walk reads a bogus pointer
  and crashes. The *typed* path (`.head` / ascribed `.tail`) works. Filed:
  `docs/reported/heap-cons-byvalue-aggregate-head-breaks-int-carrier-list-helpers.md`.
- **Nested instance-method dispatch on the element (G2).** `(enc @Cons (... (Cons (Option int))))`
  dispatches `Enc [Cons]`'s `(enc (.head xs))` into `Enc [Option]`, which is
  gap G2 (`docs/reported/constrained-instance-dispatch-nested-parametric-element-carrier-collapse.md`).

These are why the *original* repro at the top of this report (which used
`enc @Cons`) still does not fully run -- the homogeneity error was only the
first of the chain. Cross-reference: gap G1 (now closed) in
`docs/carrier-concrete-abi-crossing-audit-plan.md`.
