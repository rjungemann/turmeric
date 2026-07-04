---
title: VBM2b -- emitting a by-value monomorphized van Laarhoven lens body is
  blocked on the "M7-by-value gap" in the shared __spec machinery
severity: LOW-MEDIUM. Not a miscompile or a correctness bug -- purely the
  remaining performance work of Path B (van-laarhoven-monomorphization-plan).
  The wide-by-value functor lens already WORKS correctly via Path A's carrier
  box/unbox (`--enable=vl-wide-functor`); VBM2b would make it FREE (no box).
  The blocker is that the reusable per-spec body emit the plan told VBM2 to
  "lift and share" cannot yet emit a correct by-value HKT lens body.
status: OPEN. VBM2a (the cross-procedural spec resolution) landed
  2026-07-04 behind `--enable=vl-wide-mono` (registry-only, codegen unchanged).
  VBM2b (the per-spec by-value body emit) is deferred pending the M7-by-value
  gap below; VBM3 (dispatch redirect) and VBM4 (graduate `vl-wide-functor`)
  depend on VBM2b. Filed 2026-07-04.
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
