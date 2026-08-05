---
status: resolved
severity: medium
discovered: 2026-07-30
resolved: 2026-07-31
area: codegen (container element protocol, by-value struct elements)
---

# `Vec` of by-value struct elements: `tur check` passes, cc rejects the emitted C

> **RESOLVED** (2026-07-31, representation-consolidation increment 3 -- the
> container element protocol). The width fork in the container-element
> decision is gone: a non-heap by-value ADT product of ANY width is now
> heap-boxed when stored into a heap container slot and deref-unboxed on
> read-back, exactly as the wide (> 8 byte) elements always were. One shared
> predicate, `type_is_boxed_container_elem` (`src/compiler/types.c`), is
> consulted by every decision that has to agree:
>
> - **push side, generic inline-C sink** (`src/compiler/emit_expr.c`, the
>   CONV-S1 seam-4 block): a narrow by-value element flowing into an
>   inline-C `val : A` carrier param with a heap-container sibling argument
>   (the `(Vec A)` receiver) takes `emit_carrier_bridge_escaping` (malloc +
>   copy) instead of the dangling stack spill; transient consumers with no
>   container sibling keep the cheap spill unchanged.
> - **push side, spec'd HAMT insert** (same file, the multiword-element-
>   boxing block): narrow values box via `tur_hamt_box_key` under the same
>   container-sibling discriminator, so the map can release them.
> - **read side, let/merge positions** (`fn_body_tail_byvalue_carrier_type`):
>   the tyvar-recovered concrete element is reported as a carrier producer
>   for any-width by-value products, so the carrier->concrete deref-unbox
>   bridge fires (`tur_adt_FzB b = *(tur_adt_FzB *)(intptr_t)carrier;`).
> - **read side, direct field projection** (the EX_GET_FIELD receiver
>   recovery, `recv_call_carrier_byval`): `(.a (:: (vec-get v 0) FzB))`
>   with no intervening let routes through the carrier-pointer deref for
>   any-width elements too.  Found by the fuzz acceptance run (all 34
>   first-round failures were this one site); pinned by the fixture's
>   `read-direct` row.
> - **ownership probes**: the `tur-wide-byval?` and `tur-vec-elem-wide?`
>   emit-time folds follow the same predicate, so map release / vec-free
>   stay in lockstep with the insert-side boxing.
>
> Two consequential fixes rode along:
>
> - **Double-deref guard** (`emit_expr.c`, both let-binding init paths): a
>   binding whose init is a control form (e.g. the `map-get` macro
>   expansion) that ALREADY bridged into a by-value merge temp must not be
>   re-bridged -- the guard consults the temp's RECORDED emitted C type (the
>   localvar side table) instead of re-deriving from the tail. This also
>   fixed the previously-unpinned WIDE shape
>   `(let [b (:: (map-get m k) Point)] ...)`, broken the same way.
> - **Interpreter retag** (`src/turi/eval.c`, `try_retag_carrier_struct`):
>   1-field defstruct-lowered records now retag on carrier read-back
>   (gated on `from_struct_lowering`, so a `defopaque` int newtype is never
>   dereferenced), matching the compiled protocol on the turi path.
>
> Regression fixtures: `tests/fixtures/vec-byvalue-struct-element/` (narrow
> int + float elements, same-frame and ESCAPING-frame reads after a deep
> stack-clobbering call) and `tests/fixtures/map-narrow-struct-value/`
> (narrow int + float and wide values through let-bound map-get reads, with
> map-free exercising the release path). The
> `tests/type-fuzz-src.py` `vec_box_byvalue` wrapper is in the default
> generation rotation.

## Summary

A `(Vec T)` whose element `T` is a plain (non-`:heap`) `defstruct` type-checks
fine, but reading an element back produces invalid C -- with or without the
documented `(:: (vec-get v i) T)` ascription idiom. The `:heap` variant of the
same struct works (with an incompatible-pointer warning). Found by the
`tests/type-fuzz-src.py` probe phase.

## Repro

    $ cat > /tmp/vb.tur <<'REPRO'
    (defstruct FzB [a : int])
    (defn main [] : int
      (let [v (:: (vec-new) (Vec FzB))]
        (vec-push! v (FzB 31))
        (let [b (:: (vec-get v 0) FzB)]
          (println (.a b))))
      0)
    REPRO
    $ ./build/tur check /tmp/vb.tur ; echo rc=$?
    rc=0
    $ ./build/tur run /tmp/vb.tur
    /tmp/tur-build/vb_tur.c: ... error: invalid initializer
         tur_adt_FzB b_1303 = __ps_158;
    tur: cc invocation failed (status 256)

Without the let-bound ascription, field access on the raw `(vec-get v 0)`
fails the same way as `request for member 'a' in something not a structure
or union`.

## Controls

| Variant | Result |
|---|---|
| element is `:heap` struct, same code | runs, prints 31 (with `-Wincompatible-pointer-types` warning) |
| element is `(Option int)` (parametric heap container) | runs -- pinned by `tests/fixtures/vec-push-heap-struct-element-carrier-cast` |
| element is scalar (`int`, `float` via `(:: ... :float)`) | runs |
| element is a WIDE (> 8 byte) by-value struct | runs -- the boxed protocol already covered it |

## Root cause

`vec-push!`/`vec-get` traffic in the int64 element carrier. The container
element protocol had a width fork: wide (> 8 byte) by-value ADTs were
heap-boxed (`type_is_wide_byval_adt` at every decision site), heap pointers
rode the carrier losslessly, scalars rode inline -- and a NARROW by-value
struct fell between the stools: the push side stack-spilled it (a dangling
address once the frame returns) and the read side had no un-spill at all,
initializing the by-value aggregate straight from the `int64_t` slot. The
same lossy erasure round trip as
`result-monad-bind-typed-boundary-miscompiles`, at a different boundary.

## Guide upkeep

Done with the resolution: the representation inventory in
[docs/guides/value-representations-guide.md](../../guides/value-representations-guide.md)
documents the width-independent container element protocol, and this
report's row moved out of the missing-cells table.
