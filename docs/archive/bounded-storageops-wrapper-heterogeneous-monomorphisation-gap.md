---
title: A bounded `[S] [(StorageOps S)]` wrapper fails cc -- calls the unspecialised instance symbol, and Dense/Sparse specialisations collide on one int64-carrier signature
category: Typeclass-bounded polymorphism / heterogeneous monomorphisation (gap H)
severity: Medium. A function quantified over `[S] [(StorageOps S)]` (the
  natural "generic over any storage backend" shape, and what cross-world /
  direction-2 sized scheduling needs) type-checks but does not link: the
  bounded body emits a call to the *unspecialised* instance symbol while the
  instance is emitted under a mangled concrete suffix, so `cc` fails with an
  undefined reference; and when more than one backend instance is forced, the
  `(Dense A)` and `(Sparse A)` specialisations collapse to the *same*
  int64-carrier C signature and redefine each other. Monomorphic dispatch
  (a helper typed against a concrete `(Dense int)` / `(Sparse int)`) works end
  to end; the bounded wrapper does not.
status: RESOLVED
---

# Bounded `[S] [(StorageOps S)]` wrappers don't monomorphise (gap H)

## Resolution

Fixed in the constrained-generic monomorphisation emit path. A bounded
`[S] [(StorageOps S)]` wrapper now monomorphises end-to-end at multiple carrier
backends, and the repro from this report (struct-returning *and* bool-returning
wrappers exercised at a `Dense` and a `Sparse` backend) links, runs, and routes
each clone to its own instance method. Regression:
`tests/fixtures/typeclass-bounded-wrapper-heterogeneous-dispatch/`.

Two distinct defects, two fixes:

1. **Name desync** -- the wrapper's interior dispatch call was reconstructed
   from a single instance-head *component* (`__inst_StorageOps_sop_hyget_Dense`),
   which drops the rest of a parametric / multi-parameter instance head's
   type-arg suffix (the instance is actually emitted as
   `__inst_StorageOps_sop_hyget_Dense_PosPos`). `emit_reresolve_method_call`
   (`src/compiler/emit_core.c`) now first looks up the *concrete* instance
   matching the resolved dispatch type and returns its method impl's
   authoritative FnDef binding name -- the same spelling `EX_INSTANCE_DEF` wires
   into the dict singleton -- via the new `emit_concrete_inst_method_name`
   helper. Single-component reconstruction remains the fallback for the
   ground-type cases it already handled.

2. **Clone-name collision** -- `(Dense Pos)` and `(Sparse Vel)` are distinct
   spec keys (`type_eq` keeps them apart), but both lower to the int64 carrier,
   so `emit_abi_clone_name` rendered the identical `..._int64_t_int64_t` spelling
   for a bool-returning wrapper and the two C clones redefined each other.
   `emit_abi_intern_spec` (`src/compiler/emit_module.c`) now appends a
   deterministic `__h<n>` discriminator when a freshly-built clone name collides
   with an already-interned spec's name. Non-colliding names are untouched, so
   existing snapshots are unaffected.

This unblocks `sized-defsystem`/`Stage` direction-2 (world-type-polymorphic
systems) in the ECS spice.

## One-line summary

A typeclass-bounded wrapper over `StorageOps` does not get a per-instance
monomorphic clone. The bounded body references the bare instance method symbol
(e.g. `__inst_StorageOps_storage_hyhas_qu_Dense`) while the actual instance is
emitted as `..._Dense__ltstruct_gt` (mangled with its concrete head), so the
call is an undefined reference at link; and two backends' specialisations
(`(Dense A)` / `(Sparse A)`) both lower to the identical int64-carrier C
signature, so forcing both yields a redefinition. This is the same
monomorphisation gap that keeps `defcomponent-class`'s typeclass-bounded
wrappers off the shipped surface.

## Where it bites

- `../turmeric-spices/spices/ecs/src/ecs/storage-ops.tur` -- the "Polymorphic-
  wrapper surface deferred" docstring records exactly this: a `[S]
  [(StorageOps S)]` body "emits a call to the unspecialised instance symbol
  while the instance is monomorphised against `<struct>`, so `cc` fails with an
  undefined reference."
- `docs/upcoming/ecs-spice-plan.md` -- residual follow-up #2: "Sized-scheduler
  direction 2 (cross-world / heterogeneous scheduling -- a `System` / `Stage`
  polymorphic in the world type). Stays gated on gap-H world-type
  polymorphism." Direction 1 (monomorphic single-world scheduling) shipped via
  `sized-defsystem-scheduled`; direction 2 needs this gap closed.

## Minimal repro (shape)

```turmeric
(defclass StorageOps [S]
  (type Elem : Type)
  (storage-has? [^borrow s : S idx : int] : bool))

(definstance StorageOps [(Dense A)]  (type Elem = A) (storage-has? [s idx] (dense-has?  s idx)))
(definstance StorageOps [(Sparse A)] (type Elem = A) (storage-has? [s idx] (sparse-has? s idx)))

;; Bounded wrapper -- the "generic over any backend" shape:
(defn any-has? [S] [(StorageOps S)] [^borrow s : S idx : int] : bool
  (storage-has? s idx))

;; Used at two distinct backends:
(any-has? (dense-handle) 0)
(any-has? (sparse-handle) 0)
```

- `cc` undefined reference: the body of `any-has?` calls
  `__inst_StorageOps_storage_hyhas_qu_Dense` (the unspecialised/base spelling)
  but the emitted impl is `__inst_StorageOps_storage_hyhas_qu_Dense__ltstruct_gt`.
- Redefinition: the `(Dense A)` and `(Sparse A)` clones both reduce to
  `bool f(int64_t, int64_t)` (handles ride the int64 carrier), so the two
  monomorphisations are not distinguished by signature.

## Root cause (direction)

The bounded-generic monomorphisation path (Track A) specialises a constrained
generic against a *concrete receiver type*, but a `[S] [(StorageOps S)]`
wrapper called at two different instance heads needs **heterogeneous**
monomorphisation: one clone of `any-has?` per `S`, each routing to that `S`'s
instance method clone, with clone names that stay distinct even though every
storage handle lowers to the int64 carrier. Today:

1. The bounded body's dispatch call is lowered to the base instance symbol
   rather than the per-`S` mangled clone (name desync between the call site in
   the wrapper and the emitted impl).
2. The per-`S` specialisations are keyed by lowered C signature, which is
   `*(int64_t, int64_t)` for every carrier-handle backend, so distinct `S`
   collide instead of producing separate clones.

This is *not* retired by Track A's completion (PR #444): Track A landed
end-to-end monomorphisation for the single-receiver constrained-generic case;
the heterogeneous-backend wrapper (multiple instance heads behind one bound,
all carrier-lowered) is the remaining gap-H slice.

## Fix directions (sketch)

- Emit one clone of the bounded wrapper per concrete `S` it is instantiated at
  (keyed on the instance identity / head symbol, not the lowered C signature),
  and rewrite the wrapper's interior `StorageOps` dispatch to the matching
  per-`S` instance-method clone name (the same mangled suffix the instance is
  emitted under, e.g. `_Dense__ltstruct_gt`).
- Disambiguate specialisation identity by instance head symbol so two
  carrier-lowered backends do not collide.
- Coordinate with the parametric-element projection work
  (`docs/reported/storageops-parametric-instance-struct-element-carrier-collapse.md`):
  a struct `Elem` needs both the wrapper clone *and* the by-value element spec.

## How to validate

- The repro links and runs with both a Dense and a Sparse handle.
- `storage-ops.tur` can drop its "Polymorphic-wrapper surface deferred"
  caveat and ship a `[S] [(StorageOps S)]` helper exercised at two backends.
- Unblocks `sized-defsystem`/`Stage` direction-2 (world-type-polymorphic
  systems) in the ECS spice.

## Related

- `docs/upcoming/ecs-spice-plan.md` (residual follow-ups #1 and #2).
- `docs/archive/constrained-generic-mixed-abi-carrier-base.md`,
  `docs/archive/end-to-end-monomorphization-plan*.md` (Track A substrate).
- `../turmeric-spices/spices/ecs/src/ecs/storage-ops.tur`.
