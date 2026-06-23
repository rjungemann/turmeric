---
title: Applicative `ap` -- fn-in-container by-value monomorphization
category: Planning -- ABI / Codegen, HKT dispatch residual
description: The last open sub-piece of the archived closure-result-monomorphization plan -- `ap`'s `ff : (f (fn a b))` argument. `bind` already mints by-value `__spec` clones with no arg spill; `ap` still rides its carrier base because the function-in-container case has no by-value representation. Crossing count is not the driver (the parent plan proved closure-result work alone is net-neutral); architectural parity with `bind`/`fmap`/`bimap`/`pure` is.
---

# Applicative `ap` -- fn-in-container by-value monomorphization

## Why this plan exists

This is the residual sub-piece of
`docs/archive/closure-result-monomorphization-plan.md` (archived
2026-06-22, re-scoped to CLOSED). That plan shipped by-value `__spec`
clones for `bind` and every aggregate-returning-continuation method,
reaching architectural parity with `fmap` / `bimap` / `pure`. The one
method left on its carrier base is **Applicative `ap`**, because of its
`ff : (f (fn a b))` argument -- a function packaged inside a container.

The parent plan's net-neutral finding (crossings stayed 102 -> 102 after
the bind work) is the reason the closure-result plan was closed: zero
crossings is not reachable via closure-result monomorphization alone.
This sub-plan is therefore **not justified by crossing-count reduction**.
It is justified by architectural symmetry: every other HKT method in
stdlib now monomorphizes to a by-value spec, and `ap`'s exception is a
known weakness in the by-value path's expressiveness for
function-in-container arguments.

## The specific defect

`ap`'s signature is roughly:

```
ap : (f (fn a b)) -> f a -> f b
```

For `Option`, `ff : Option (fn a b)`. The by-value Option representation
is `Option__int { tag, value }` where `value` is the int64 carrier; a
function element collapses to `Option__opaque` (verified by attempt #1
in the archived plan -- the minted
`__inst_Applicative_ap_Option__spec__Option__int_Option__opaque_Option__int`
mismatched the call site). There is no typed by-value Option whose
element slot is a typed function thunk.

Concretely the open work is:

1. **Represent a typed-fn element inside a by-value container.** An
   `Option<fn a b>` (or `Result<E, fn a b>`) needs a representation whose
   `value` slot is the typed thunk `tur_thunk_<b>_<a>_t`, not an
   `int64`/`opaque`. Today only scalar / aggregate elements have a
   by-value Option/Result.
2. **Ground `ap`'s `(f b)` from the function element's result type.**
   Mirroring `bind`'s grounding (`elab_fns.c:4020-4051`,
   `m7_body_constructs_byvalue` /  `m7_byvalue_grounded`) but driven by
   the fn-element's `b`, not by a continuation body.
3. **Mint and route a by-value `ap __spec` clone.** Spec body invokes the
   contained thunk through the typed signature (no carrier box/unbox),
   then constructs the result Option by value.

## What's already in place

- **`bind` shipped by-value (the template).** `m7_body_constructs_byvalue`
  / `m7_byvalue_grounded` at `src/compiler/elab_typeclasses.c:1741, 5810,
  5967, 6052`. Grounding fix at `src/compiler/elab_fns.c:4020-4051`.
- **Typed thunk machinery.** `tur_thunk_<R>_<A>_t` typedefs, fat-shim
  generation, `phase_f_concrete` in `emit_expr.c:2483-2492` -- already
  used for typed-result continuations driven by `bind`'s grounding.
- **Closure-thunk bridge stays.** `src/compiler/emit_expr.c:2478-2481`
  documents why bridging cannot be added at the
  `make_poly_wrapper`/spill-shim boundary post-construct-monomorphization
  ("produced a wild-pointer deref"). The same constraint applies here:
  any new fn-in-container representation must not re-introduce a
  closure-result bridge at that site.
- **Dead-instance elimination.** `emit_instance_is_live`
  (`emit_module.c:4149`) ensures the carrier base of `ap` is only emitted
  when needed; once a by-value `ap __spec` exists for every live
  instance/element pair the carrier base becomes dead naturally.

## Phases

### Phase 0 -- representation design

- [ ] Decide the by-value Option/Result-of-fn representation. Two
      candidates: (a) parametrize the existing `Option__<elem>` struct on
      a typed-thunk element (extends the `*__spec` naming axis); (b)
      separate `Option_Fn__<R>__<A>` shape sourced from
      `make_poly_wrapper`. Pick whichever avoids re-introducing a
      closure-result bridge at construct/destructure.
- [ ] Write a one-page disposition under
      `docs/reported/ap-fn-in-container-representation.md` -- ABI shape,
      construct/destructure path, name mangling, fat-shim interaction.

### Phase 1 -- emit the typed-fn-element container

- [ ] Codegen: emit the chosen struct shape on demand.
- [ ] Construct/destructure: `(some f)` for `f : (fn a b)` produces the
      typed-fn-element form; `(.value ff)` reads it back as the typed
      thunk (no opaque cast).
- [ ] Gate strictly: only when the surrounding spec asks for the typed
      form (so a bare closure stored in a heterogeneous carrier slot is
      unaffected -- the heterogeneous closure path must keep its carrier
      result).

### Phase 2 -- ground and mint by-value `ap`

- [ ] Elab: ground `ap`'s `(f b)` from the fn element's `b`. Reuse
      `m7_body_constructs_byvalue` / `m7_byvalue_grounded` for the
      decision; add a fn-element-aware sibling if needed.
- [ ] Emit: by-value `ap __spec` -- takes `ff` and `fa` by value,
      invokes the typed thunk inside `ff` directly, constructs the
      result Option by value.
- [ ] Validation: `hkt-stdlib-option-result-instances` + any spice
      fixture that uses `ap`. Confirm `__inst_Applicative_ap_*` now
      lands as a by-value `__spec` clone, not a carrier-base call.

### Phase 3 -- archive

- [ ] Confirm the carrier base of `ap` is now dead for every live
      instance/element pair (or document the residual cases as
      legitimately type-erased boundaries).
- [ ] Move this plan to `docs/archive/`.

## Risks (read before starting)

- **Net-neutral on crossings.** Per the archived parent plan, this work
  will likely not move the audit crossing count. Frame the PR as
  architectural-parity work, not crossing reduction.
- **Representation blast radius.** The fn-in-container representation
  affects every site that constructs/destructures a container holding
  a function. Phase 1's gating is the crux -- never change a
  heterogeneous container's element ABI globally.
- **`make_poly_wrapper` bridge constraint.** The closure-thunk bridge at
  `emit_expr.c:2478-2481` cannot be re-added at construct-monomorphization
  time. Phase 0's representation choice must be checkable against this
  constraint before any codegen lands.
- **Attempt #1 in the archived parent failed here.** A naive grounding
  that collapses the fn element to `Option__opaque` mis-mints the spec
  signature (`cc: incompatible type for argument 1`). The fix is the
  typed-fn-element representation, not a smarter int-default.

## Non-goals

- **Zero crossings.** Tracked elsewhere (the parent end-to-end
  monomorphization track, now archived).
- **Re-deleting the carrier bridge.** The bridge stays per the archived
  parent plan's final disposition.
- **Other Applicative methods.** Only `ap` is on the carrier base today;
  `pure` already monomorphizes.

## Validation

- `bash tests/run.sh` green (10-min timeout) at every phase.
- `hkt-stdlib-option-result-instances` emits `__inst_Applicative_ap_*`
  as a by-value `__spec` clone.
- Suite-wide `TUR_M3_AUDIT=1` sweep: no new crossings introduced; the
  `ap` carrier base becomes unreferenced for live instances.
