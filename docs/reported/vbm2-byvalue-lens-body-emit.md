---
title: VBM2b -- by-value monomorphized van Laarhoven lens body emits + eliminates
  the (f S) result box, but one nested-receiver cascade level (the M7-by-value
  gap) remains before it compiles
severity: LOW-MEDIUM. Not a miscompile of shipping code -- the whole path is
  gated behind `--enable=vl-wide-mono`, and the wide-by-value functor lens
  already WORKS via Path A's carrier box/unbox (`--enable=vl-wide-functor`).
  VBM2b makes it FREE (no box). The residual is the last step of the by-value
  emit: a nested instance-method call's by-value struct receiver is not lowered
  to a by-value twin, so the two gated fixtures build-red.
status: IN PROGRESS. VBM2a (cross-procedural spec resolution) landed 2026-07-04
  (green). VBM2b (per-spec by-value body emit) is WIRED behind
  `--enable=vl-wide-mono` and gets most of the way: it emits
  `point_x__mono_<hash>` returning `(Identity Point)` BY VALUE and mints by-value
  `fmap` + `mk-id` instance twins, so **the `(f S)` result heap box is
  eliminated**. It does not yet fully compile: the `fmap` twin calls `run-id` on
  its by-value receiver, but no by-value `run-id : Identity int -> int` twin is
  minted, so `emit_fn_def` spills the by-value receiver as `int64_t __t77 = i`
  and calls the carrier `run_hyid` -- a C type error. The two `vl-wide-mono`
  fixtures build-red pending that one emitter step; ALL non-`vl-wide-mono`
  fixtures are unaffected (the path is fully gated). The target shape is
  hand-validated (see below): the fully by-value lens returns 3/30/4/99 with zero
  heap boxes. VBM3/VBM4 depend on finishing VBM2b. Filed 2026-07-04.
---

# VBM2b: by-value lens body emits; one nested-receiver cascade level remains

## Summary

Path B slice VBM2 (`docs/upcoming/van-laarhoven-monomorphization-plan.md`) has
two halves:

- **VBM2a -- cross-procedural spec resolution (LANDED, green).** VBM1 recorded an
  *abstract* lens spec at the `(l g s)` pin inside the enclosing fn (e.g.
  `set-px`), where the lens is the abstract param `l`; the concrete lens FnDef
  Path B must specialize is not resolvable there (plan OQ #1/#2). VBM2a's
  `mono_specs_resolve_program` (`src/compiler/mono_specs.c`) walks the elaborated
  program, joins each abstract spec to the concrete lens passed at every
  top-level call of its enclosing fn, and records a concrete key
  `(lens_fn, functor, focus, whole)` -- collapsing the two abstract specs
  (`set-px`/`over-px` both pin `f := Identity`, focus `int`, whole `Point`) to
  one concrete emit target `lens=point-x`. `view-px` pins the carrier-compatible
  `Const` and is correctly not registered. Reviewable with `--dump-mono-specs`
  (fixture `van-laarhoven-lens-wide-mono-resolve`).

- **VBM2b -- per-spec by-value body emit (WIRED, one step short).** For each
  concrete key, emit one `<lens>__mono_<hash>` body in which `(f a)`, `(f S)` and
  the functor-wrapping `g : (-> A (f A))` are spelled BY VALUE with the concrete
  `f` substituted, and the `fmap` dispatch is a direct call to a by-value
  instance twin. This IS now emitted (`point_x__mono_<hash>` + by-value `fmap` /
  `mk-id` twins, result box gone) -- but does not yet compile because the twin's
  own nested `run-id` receiver is not lowered by-value.

## How far VBM2b gets today (2026-07-04)

`emit_program` (`src/compiler/emit_module.c`, the block after the main ABI-spec
emit loop), under `--enable=vl-wide-mono`, for each concrete spec resolved by
VBM2a:

1. Interns an `EmitAbiSpecialization` for the lens FnDef with the HKT tyvar `f`
   bound to the concrete functor (`emit_abi_intern_spec`), names it
   `point_x__mono_<hash>`, result type by value. A new
   `EmitAbiSpecialization.is_vl_wide_mono` flag (`emit_internal.h`) marks it.
2. Scans the lens body (`emit_abi_scan_expr`) with the MB2.5 HKT carve-out
   (`emit_module.c` ~2209) OPENED for `is_vl_wide_mono` specs, so the `fmap`
   dispatch is monomorphized: it mints a by-value `fmap` instance twin
   (`__inst_Functor_fmap_Identity__spec__...` returning `tur_adt_Identity__Point`
   BY VALUE) and a by-value `mk-id` twin. **The `(f S)` result heap box is
   eliminated** -- no `malloc`, no `*(Identity Point *)` unbox.
3. Retypes each minted instance-method twin's receiver to the by-value `(f a)`
   the lens body actually passes (the substituted `g` returns the aggregate by
   value), BEFORE recursively scanning the twin bodies, then forward-declares
   (`emit_abi_forward_decl`) and emits everything. A realloc-safe re-fetch guards
   the spec pointer across interning (the array can move under
   `emit_abi_scan_expr`).

The emitted `point_x__mono` and the `fmap` / `mk-id` twins are correct by-value
C. What it produces (abbreviated):

```c
static tur_adt_Identity__Point
__inst_Functor_fmap_Identity__spec__...(tur_adt_Identity__int i, tur_poly_fn_t g) {
    int64_t __t77 = i;                                   // <-- BUG: int64_t, should be
                                                         //     tur_adt_Identity__int
    return mk_id__spec__...Point...(
        ((tur_adt_Point *(*)(void*, int64_t))g.fn)(g.env, run_hyid(&__t77)));  // carrier run-id
}
static tur_adt_Identity__Point point_x__mono_...(int64_t g, int64_t s) {
    ... build setter env ...
    return __inst_Functor_fmap_Identity__spec__...(          // by-value fmap twin, NO unbox
        (*(tur_adt_Identity__int *)(...g(g.env, s->x)...)),  // g returns by-value (f int)
        setter);
}
```

## Exact remaining blocker

The `fmap` twin body calls `run-id` on its by-value receiver `i`. No by-value
`run-id : Identity int -> int` twin is minted, so the call stays on the carrier
`run_hyid` and `emit_fn_def` spills the receiver as `int64_t __t77 = i` (an
`int64_t` initialized from a two-word `tur_adt_Identity__int`) -- a C type error.

Root cause of the miss: `run-id` is called *inside the generic `Functor Identity`
instance body* (`(fmap [i g] (mk-id (g (run-id i))))`), where `run-id`'s type
param `A` is an unresolved tyvar -- the call node carries no concrete
`abi_bindings`. During the recursive scan of the `fmap` twin, the mint path
(`emit_abi_register_call`) does not propagate the active twin spec's element
binding (`A := int`) down to that nested call, so it never sees a concrete
by-value receiver to specialize. Confirmed empirically: the scan mints exactly
`__inst_Functor_fmap_Identity__spec__...` and `mk_id__spec__...Point...` and
nothing for `run-id`.

This is the by-value-RECEIVER half of the documented M7-by-value gap: the shared
body emit lowers a by-value RESULT (return-position dispatch) but not a by-value
struct RECEIVER threaded into a nested generic call.

## Two ways to finish it

1. **General (preferred): mint + route the by-value `run-id` twin.** Make the
   recursive scan resolve a nested generic call's tyvars through the active
   spec's bindings (the twin knows `A := int`), so `run-id`'s by-value receiver
   is seen and `emit_abi_intern_spec` mints
   `run_id__spec__int_tur_adt_Identity__int`. The existing carrier->by-value twin
   redirect (`emit_abi_try_byval_twin_redirect`, `emit_module.c` ~2275) then
   retargets the `run_hyid` call to it and the spill disappears. This generalizes
   to any lens whose instance body threads its by-value receiver through nested
   accessors.

2. **Narrow correctness patch: type the receiver spill by-value.** Independently,
   the spill `int64_t __t77 = i` is simply mistyped -- if it were
   `tur_adt_Identity__int __t77 = i`, then `run_hyid((int64_t)(intptr_t)(&__t77))`
   passes a valid pointer to a real `Identity int` and the carrier `run-id`
   dereferences it correctly (no inner twin needed; the receiver rides a stack
   temp, not a heap box). This compiles and is correct, but leaves one small
   carrier hop for `(f a)` -- acceptable, since the expensive `(f S)` box is
   already gone. Find the spill in the ABI-spec-body arg emit (the `&temp`
   receiver bridge in `emit_fns.c` / `emit_expr.c`) and use the receiver's
   resolved by-value type for the temp.

Either closes VBM2b; option 1 is the more complete monomorphization.

## Hand-validation (the target shape is correct)

The fully by-value lens was validated by grafting the ideal shape into the
emitted C (setter closure reused, `g` result unboxed once, `fmap` inlined by
value): `set-px`/`over-px`/`view-px` return **3 / 30 / 4 / 99** with **zero heap
allocations on the lens path** -- no `emit_agg_box`, no `*(Identity Point *)`
unbox. So the design is sound; only the codegen step above is outstanding.

## Minimal repro

`tests/fixtures/van-laarhoven-lens-wide-mono/input.tur` (and the resolve variant)
with `--enable=...,vl-wide-functor,vl-wide-mono`. `tur build`/`tur run` fails at
the C stage on the `int64_t __t77 = i` line; `tur emit-c` succeeds (the defect is
in the emitted C, not the compiler, which is stable -- the earlier
interning-realloc use-after-free is fixed). `--dump-mono-specs` shows the VBM2a
resolution:

```
; van-laarhoven by-value monomorphization specs: 2 abstract, 1 concrete
mono-spec-abstract <h> fn=set-px  lens-param=l f=Identity focus=int whole=Point
mono-spec-abstract <h> fn=over-px lens-param=l f=Identity focus=int whole=Point
mono-spec <h> lens=point-x f=Identity focus=int whole=Point   <- VBM2a resolved
```

## Fix directions (file:line)

- `src/compiler/emit_module.c` (VBM2b block, after the main ABI-spec emit loop) --
  the mono-body emit + carve-out opening + recursive twin scan + receiver retype.
- `src/compiler/emit_module.c:2209` -- the MB2.5 carve-out, OPENED for
  `is_vl_wide_mono` specs (done).
- `src/compiler/emit_module.c` ~2275 (`emit_abi_try_byval_twin_redirect`) -- the
  carrier->by-value twin redirect that would retarget `run_hyid` once a by-value
  `run-id` twin exists.
- `src/compiler/emit_module.c:2191` (`emit_abi_register_call`) -- where nested
  generic-call tyvar resolution must consult the active spec's bindings so the
  by-value `run-id` twin is minted (option 1).
- The `&temp` receiver spill in the ABI-spec body arg emit
  (`emit_fns.c` / `emit_expr.c`) -- type the temp by-value (option 2).
- `src/compiler/mono_specs.c` / `.h` -- the VBM2a concrete registry + emit-info
  accessors (`mono_spec_concrete_emit_info`) that hand the lens FnDef + functor
  Type + tyvar to the emit block.

Absent the final step, Path A remains the shipped answer for the wide-by-value
functor lens (correct, one box/copy/free per crossing), exactly as before VBM2a.
