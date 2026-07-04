---
title: VBM2b -- emitting a by-value monomorphized van Laarhoven lens body is
  blocked on the "M7-by-value gap" in the shared __spec machinery
severity: LOW-MEDIUM. Not a miscompile or a correctness bug -- purely the
  remaining performance work of Path B (van-laarhoven-monomorphization-plan).
  The wide-by-value functor lens already WORKS correctly via Path A's carrier
  box/unbox (`--enable=vl-wide-functor`); VBM2b would make it FREE (no box).
  The blocker is that the reusable per-spec body emit the plan told VBM2 to
  "lift and share" cannot yet emit a correct by-value HKT lens body.
status: IN PROGRESS. VBM2a (cross-procedural spec resolution) landed 2026-07-04.
  VBM2b (per-spec by-value body emit) is now WIRED behind `--enable=vl-wide-mono`
  and gets most of the way: it emits `point_x__mono_<hash>` returning
  `(Identity Point)` BY VALUE, and mints by-value `fmap` / `mk-id` instance
  twins so the `(f S)` RESULT heap box is eliminated. It does not yet fully
  compile: the last cascade level -- the by-value RECEIVER of the nested
  instance-method body (`run-id : Identity int -> int` inside the `fmap` twin) --
  is not minted, so emit_fn_def spills the by-value receiver as `int64_t __t77 =
  i` and calls the carrier `run_hyid`, a C type error. The two `vl-wide-mono`
  fixtures therefore build-red pending that one emitter feature (below). All
  non-`vl-wide-mono` fixtures are unaffected (the whole path is gated). Filed
  2026-07-04; VBM3/VBM4 depend on finishing VBM2b.
---

# VBM2b: by-value lens body emit is blocked on the M7-by-value gap

## Summary

Path B slice VBM2 (`docs/upcoming/van-laarhoven-monomorphization-plan.md`) has
two halves:

- **VBM2a -- cross-procedural spec resolution (LANDED).** VBM1 recorded an
  *abstract* lens spec at the `(l g s)` pin inside the enclosing fn (e.g.
  `set-px`), where the lens is the abstract param `l`; the concrete lens FnDef
  Path B must specialize is not resolvable there (plan OQ #1/#2). VBM2a's
  `mono_specs_resolve_program` (`src/compiler/mono_specs.c`) walks the
  elaborated program, joins each abstract spec to the concrete lens passed at
  every top-level call of its enclosing fn, and records a concrete key
  `(lens_fn, functor, focus, whole)` -- collapsing the two abstract specs
  (`set-px`/`over-px` both pin `f := Identity`, focus `int`, whole `Point`) to
  one concrete emit target `lens=point-x`. `view-px` pins the
  carrier-compatible `Const` and is correctly not registered. Reviewable with
  `--dump-mono-specs` (fixture `van-laarhoven-lens-wide-mono-resolve`).

- **VBM2b -- per-spec by-value body emit (BLOCKED, this report).** For each
  concrete key, emit one `__mono_<hash>` lens body in which `(f a)`, `(f b)`,
  `(f S)`, and the functor-wrapping `g : (-> A (f A))` are spelled BY VALUE with
  the concrete `f` substituted, and the `fmap` dispatch becomes a direct call to
  the by-value instance method. This is not yet emitted.

## Current state (2026-07-04): how far VBM2b gets

`emit_program` (src/compiler/emit_module.c) now, under `--enable=vl-wide-mono`,
drives the shared ABI-spec body emit for each concrete lens spec:

1. Interns a spec for the lens FnDef with the HKT tyvar `f` pinned to the
   concrete functor, names it `point_x__mono_<hash>`, result type by value.
2. Scans the lens body with the MB2.5 carve-out (emit_module.c:2209) OPENED for
   `is_vl_wide_mono` specs, so the `fmap` dispatch mints a by-value `fmap`
   instance twin (`__inst_Functor_fmap_Identity__spec__...`) returning
   `(Identity Point)` by value + a by-value `mk-id` twin -- **the `(f S)` result
   box is gone**.
3. Retypes the twin's receiver to the by-value `(f a)` the lens body passes, then
   recursively scans the twin bodies, then forward-declares + emits everything.

The emitted `point_x__mono` and the `fmap`/`mk-id` twins are correct by-value C.
The ONE remaining defect: the `fmap` twin body calls `run-id` on its by-value
receiver `i`, but no by-value `run-id : Identity int -> int` twin is minted, so
emit_fn_def emits the carrier spill `int64_t __t77 = i; ... run_hyid(&__t77)` --
which is ill-typed (`int64_t` from a `tur_adt_Identity__int`). That is the
by-value-RECEIVER half of the M7 gap: the shared body emit does not yet lower a
nested instance-method call's by-value struct receiver to a by-value inner twin.
Closing it (mint + route the by-value `run-id`, and teach emit_fns.c's receiver
spill to use the by-value type) finishes VBM2b.

## Root cause (why VBM2b can't just reuse the __spec machinery)

The plan's VBM2 "Work" bullet says to lift the existing class-instance `__spec`
emit helper and share it. But the reusable ABI-spec pipeline
(`emit_abi_register_call` / `emit_abi_intern_spec`, `src/compiler/emit_module.c`)
deliberately CARVES OUT the higher-kinded lens dispatch it would need to
monomorphize. See `src/compiler/emit_module.c:2209` (the MB2.5 carve-out):

> a class-method call on a HIGHER-KINDED constrained variable inside a
> constrained rank-2 poly-fn (or its dict-clone) is dispatched through the
> runtime dict param at emit, NOT monomorphized. Minting a by-value instance
> spec for it here produces a DEAD clone ... that is also ill-typed for a
> by-value aggregate functor (its `(f a)` result temp collapses to the int64
> carrier while the ctor returns the aggregate -- **the M7-by-value gap**).

So the shared machinery, if pointed at the lens body with `f := Identity`, would
produce an ill-typed clone whose `(f a)` temp is the int64 carrier while the
`Identity` constructor returns the two-word aggregate. VBM2b therefore requires
either:

1. **Close the M7-by-value gap** -- teach the ABI-spec body emit to carry a
   wide by-value aggregate `(f a)` result through a spec body (by-value temp +
   by-value instance method), then open the MB2.5 carve-out for the
   wide-by-value-functor lens shape under `g_opt_vl_wide_mono`; or
2. **A bespoke by-value emitter** for the three WF crossings scoped to the van
   Laarhoven lens shape (by-value `g` param, direct by-value `fmap` instance,
   by-value `(f S)` result) driven off the VBM2a concrete registry -- avoiding
   the general gap but re-deriving per-body substitution.

Option 1 is the more general fix and aligns with the wider carrier-retirement
track (`docs/upcoming/tur-m7-hkt-flag-retirement-plan.md`); option 2 is
narrower and lower-risk but less reusable.

## Minimal repro

`tests/fixtures/van-laarhoven-lens-wide-mono-resolve/input.tur` (the standard
`Identity`/`Point`/`point-x` lens). With
`--enable=...,vl-wide-functor,vl-wide-mono --dump-mono-specs`:

```
; van-laarhoven by-value monomorphization specs: 2 abstract, 1 concrete
mono-spec-abstract <h> fn=set-px  lens-param=l f=Identity focus=int whole=Point
mono-spec-abstract <h> fn=over-px lens-param=l f=Identity focus=int whole=Point
mono-spec <h> lens=point-x f=Identity focus=int whole=Point   <- VBM2a resolved
```

The concrete key `lens=point-x` is the body VBM2b must emit as
`point_x__mono_<hash>` returning `tur_adt_Identity__Point` by value. Today
codegen still takes the Path A carrier box/unbox (`set_hypx`/`over_hypx` unbox a
`*(tur_adt_Identity__Point *)` out of the int64 carrier the lens clone returns).

## Fix directions (file:line)

- `src/compiler/emit_module.c:2209` -- the MB2.5 carve-out to open (gated on
  `g_opt_vl_wide_mono` + `emit_type_is_byvalue_adt(pinned_f)`) once the gap is
  closed.
- `src/compiler/emit_module.c:1344` (`emit_abi_intern_spec`) + the body emit
  loop (`~emit_module.c:9027`) -- the shared spec-body emit to drive for the
  resolved lens FnDef with `f := Identity`.
- `src/compiler/emit_core.c:172` (`emit_resolve_type`) -- the tyvar->concrete
  substitution that must yield a by-value `Identity Point` (not the carrier) for
  the HKT tyvar.
- `src/compiler/emit_core.c:1199/1246`
  (`emit_concrete_inst_method_fndef`/`_name`) -- resolving the by-value `fmap`
  instance the spec body calls directly.
- `src/compiler/mono_specs.c` -- the VBM2a concrete registry that names each
  `(lens_fn, functor, focus, whole)` body to emit.

Absent VBM2b, Path A remains the shipped answer for the wide-by-value functor
lens (correct, one box/copy/free per crossing), exactly as before VBM2a.
