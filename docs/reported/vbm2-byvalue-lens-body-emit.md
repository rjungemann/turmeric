---
title: VBM2b + VBM3 -- by-value monomorphized van Laarhoven lens now emits AND
  is live; the (f S) result box is eliminated end-to-end
severity: RESOLVED (VBM2a/VBM2b/VBM3). The by-value lens body + fmap/run-id/mk-id
  twins emit as correct C, and the lens call sites are redirected to it, so the
  `(f S)` result heap box is gone on the live path; full suite green. Kept OPEN
  only for VBM4 (graduate `vl-wide-functor` / delete Path A on the wide branch)
  and the residual `(f A)` `g` box (a small optimization, below).
status: VBM3 DONE (2026-07-04). VBM2a (resolution) + VBM2b (by-value body emit) +
  VBM3 (dispatch redirect) all landed behind `--enable=vl-wide-mono`.
  `emit_program` emits `point_x__mono_<hash>` returning `(Identity Point)` BY
  VALUE with by-value `fmap`/`run-id`/`mk-id` twins, and the poly-call emit
  REDIRECTS every `(l g s)` whose consumer's lens param uniquely resolves to that
  mono lens straight to `point_x__mono` -- so `set-px`/`over-px` now emit
  `run_id__spec(point_x__mono(g, s))` with NO carrier dict dispatch and NO
  `emit_agg_box`/`emit_agg_unbox` around the `(f S)` crossing (the
  `dict_Functor_Identity_singleton` has zero uses).  `view-px` (Const,
  carrier-compatible) is untouched.  Full suite: 1940 passed, 0 failed; fixtures
  return 3/30/4/99.  Residual: `g` still returns its `(f A)` through the int64
  carrier (the mono body unboxes it once), so `__fn_1305`'s small `(f A)` box
  survives -- VBM4's by-value `g` removes it.  Filed 2026-07-04.
---

# VBM2b: by-value lens body emits (box eliminated); VBM3 redirect remains

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
  instance twin. **DONE** -- emits correct, compiling, box-free C; suite green.

## How VBM2b emits (2026-07-04, complete)

`emit_program` (`src/compiler/emit_module.c`, the block after the main ABI-spec
emit loop), under `--enable=vl-wide-mono`, for each concrete spec resolved by
VBM2a:

1. Interns an `EmitAbiSpecialization` for the lens FnDef with the HKT tyvar `f`
   bound to the concrete functor (`emit_abi_intern_spec`), names it
   `point_x__mono_<hash>`, result type by value. A new
   `EmitAbiSpecialization.is_vl_wide_mono` flag (`emit_internal.h`) marks it.
2. Scans the lens body (`emit_abi_scan_expr`) with the MB2.5 HKT carve-out
   (`emit_module.c` ~2209) OPENED for `is_vl_wide_mono` specs, so the `fmap`
   dispatch is monomorphized: it mints a by-value `fmap` instance twin returning
   `tur_adt_Identity__Point` BY VALUE plus a by-value `mk-id` twin.
3. Retypes each minted instance-method twin's receiver to the by-value `(f a)`
   the lens body passes, AND binds the twin's receiver element tyvar (`a := int`,
   recovered from the class method signature `fmap : (f a) -> (a -> b) -> (f b)`,
   whose `param_types[0]` keeps the `(f a)` shape the carrier FnDef lost -- the
   mint had bound only the result element `b := Point`). Then recursively scans
   the twin bodies so the nested `run-id` also mints a by-value twin
   (`run_id__spec__int64_t_tur_adt_Identity__int(tur_adt_Identity__int i) { return
   i.wrapped; }`). Forward-declares + emits everything; a realloc-safe re-fetch
   guards the spec pointer across interning.

Four shared-emit sites are gated on `is_vl_wide_mono` so the by-value world isn't
re-carrier'd: the WF3 poly-call result unbox (`emit_expr.c` ~2916), the
poly-agg arg deref (~4188), the carrier-producer->by-value-param double-unbox
(~4483), and a tyvar-headed-app resolution in `emit_carrier_bridge`
(`emit_core.c`) that spills a spec-body `(f a)` receiver at its concrete by-value
type instead of the int64 carrier.

The emitted C (the whole point of Path B -- **no `(f S)` heap box**):

```c
static int64_t
run_id__spec__int64_t_tur_adt_Identity__int(tur_adt_Identity__int i) {
    return (int64_t)(i).wrapped;                    // by-value receiver
}
static tur_adt_Identity__Point
__inst_Functor_fmap_Identity__spec__...(tur_adt_Identity__int i, tur_poly_fn_t g) {
    return mk_id__spec__...Point...(                // returns Identity Point BY VALUE
        ((tur_adt_Point *(*)(void*, int64_t))g.fn)(
            g.env, run_id__spec__int64_t_tur_adt_Identity__int(i)));
}
static tur_adt_Identity__Point point_x__mono_...(int64_t g, int64_t s) {
    ... build setter env (the only malloc -- the closure env, as in Path A) ...
    return __inst_Functor_fmap_Identity__spec__...(  // by-value fmap twin, NO unbox
        ((tur_adt_Identity__int (*)(void*, int64_t))...g...)(g.env, s->x),  // g by value
        setter);
}
```

`set-px`/`over-px`/`view-px` return **3 / 30 / 4 / 99**; full suite **1940 passed,
0 failed**.

## VBM3 (dispatch redirect) -- DONE, via an in-place redirect (no consumer mono)

An earlier revision of this report expected VBM3 to need value-based consumer
monomorphization (specializing `set-px`/`over-px` per concrete lens + by-value
`g` closures).  It did not, because of one design change to the mono body:

- **The mono body takes the ordinary Path A `g` (carrier).** Instead of casting
  `g.fn` to return `(f A)` by value, `point_x__mono`'s `g` call is cast to return
  the int64 carrier and the box is deref'd ONCE (`*(Identity int *)(g(...))`),
  feeding the by-value `fmap` twin.  So `point_x__mono` accepts the EXACT `g`
  `set-px`/`over-px` already build -- no by-value `g` closure needed.  (Gated in
  the fat-closure dispatch emit, `emit_expr.c`, on `is_vl_wide_mono`.)

Given that, the redirect is a local poly-call rewrite -- no new consumer bodies:

1. VBM2a records, per abstract spec, the lens param's `Binding *` and (from the
   resolve walk) the concrete lens it uniquely resolves to (marking `ambiguous`
   if a consumer's param takes more than one distinct lens).  See
   `mono_spec_redirect_for_binding` (`mono_specs.c`).
2. In the `is_poly_call` emit (`emit_expr.c`), a `(l g s)` whose `fn_binding` is
   such a lens param is emitted as `<lens>__mono_<hash>((int64)g, (int64)s)` --
   dropping the dict (slot 0) -- and returned directly, skipping the whole
   carrier dispatch + `(f S)` box/unbox scaffold.  The shared `emit_vl_mono_name`
   (`emit_module.c`) keeps the call symbol identical to the body's.
3. The mono body's forward decl is emitted into `fwd_decls` (assembled before
   `file`) so the redirect call in the earlier `set_hypx` body resolves.

Result: `set_hypx` emits `return run_id__spec(point_x__mono(g, s))` -- no
`l.fn(...)`, no `dict_Functor_Identity_singleton` use, no `(f S)` box.  The Path A
carrier clone (`point_hyx_un_undict_...`) is now dead (still emitted, unused).

## What remains: VBM4 + the residual `(f A)` box

- The `g` closure (`__fn_1305`/`__fn_1314`) still `malloc`s its `(f A)` =
  `Identity int` box, which `point_x__mono` unboxes once.  A by-value `g` (WF2
  inverted) removes that last small box -- a follow-on optimization.
- VBM4 graduates `vl-wide-functor`: delete the Path A box on the wide branch and
  TUR-E0309, and retire the now-dead carrier lens clone on the redirected path.

## Minimal repro

`tests/fixtures/van-laarhoven-lens-wide-mono/input.tur` (and the resolve variant)
with `--enable=...,vl-wide-functor,vl-wide-mono`. `tur emit-c` shows the by-value
mono body; `tur build`/`tur run` returns 3/30/4/99. `--dump-mono-specs` shows the
VBM2a resolution:

```
; van-laarhoven by-value monomorphization specs: 2 abstract, 1 concrete
mono-spec-abstract <h> fn=set-px  lens-param=l f=Identity focus=int whole=Point
mono-spec-abstract <h> fn=over-px lens-param=l f=Identity focus=int whole=Point
mono-spec <h> lens=point-x f=Identity focus=int whole=Point   <- VBM2a resolved
```

## Where it lives (file:line)

- `src/compiler/emit_module.c` (VBM2b block, after the main ABI-spec emit loop) --
  the mono-body emit + carve-out opening + receiver retype + element-tyvar bind +
  recursive twin scan + forward-decl.
- `src/compiler/emit_module.c` ~2209 -- the MB2.5 carve-out, OPENED for
  `is_vl_wide_mono` specs.
- `src/compiler/emit_internal.h` -- `EmitAbiSpecialization.is_vl_wide_mono`.
- `src/compiler/emit_expr.c` ~2916 / ~4188 / ~4483 -- the three unbox/deref gates
  keyed on `is_vl_wide_mono`.
- `src/compiler/emit_core.c` (`emit_carrier_bridge`) -- the tyvar-headed-app
  resolution that types a spec-body `(f a)` receiver spill by value.
- `src/compiler/mono_specs.c` / `.h` -- the VBM2a concrete registry + emit-info
  accessors (`mono_spec_concrete_emit_info`) that hand the lens FnDef + functor
  Type + tyvar to the emit block.

VBM3 (the remaining slice) redirects the lens call sites to the emitted mono
body; until it lands, Path A drives dispatch (the mono body is emitted but
unused), so behaviour is identical to before -- the box is eliminated in the
emitted body, and made live by VBM3.
