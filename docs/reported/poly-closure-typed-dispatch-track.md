---
title: Poly-closure / typed fat-closure dispatch -- consolidated track
category: Bug Report
status: PARTIAL -- one prerequisite resolved; full `>>>` generalization still open
severity: latent silent miscompile (guarded by TUR-E0705 for the dispatching shape)
description: Consolidates three related reports about typed/polymorphic closure-returning combinators (`>>>`, `cmp`) miscompiling when float result types are involved. The end-state is a register-class-correct generic `>>>` that composes `:float -> :float` closures through xmm0 instead of riding the SysV register-class accident through int64 thunks. One prerequisite is resolved (capturing-closure return-type lowering); two remain open (fn-typed `^fat` param tyvar propagation, and per-spec retention of inner-body intermediate types under fat dispatch).
---

# Poly-closure / typed fat-closure dispatch -- consolidated track

This track gathers three reports that describe one end-state defect with three
distinct layers. Originals are preserved verbatim in
[`../archive/`](../archive/) -- link targets below. **Do not delete the originals**;
they hold the file:line root-cause analysis the fix work needs.

## The end state we are trying to reach

A typed, register-class-correct generic `>>>` (and adjacent `cmp`-shaped
combinators) in `stdlib/arrow.tur` that composes `(fn [:float] :float)`
closures end-to-end through `tur_thunk_double_double_t` / xmm0, without the
inner thunk body riding the `int64_t(*)(void*, int64_t)` ABI and producing
correct float output only by SysV register-class accident.

Tracking plan (the cross-referenced upstream artifact):
`docs/upcoming/poly-closure-result-specialization-plan.md`.

## The three layers

### Layer 1 -- the outermost symptom: `>>>` itself

[`../archive/history/arrow-compose-float-closure-int64-thunk-mismatch.md`](../archive/history/arrow-compose-float-closure-int64-thunk-mismatch.md)
-- the untyped stdlib `>>>` commits its inner closure body to the int64 thunk
ABI; when applied to `:float -> :float` closures the emitted C makes three
disagreeing function-pointer-type casts at the producer, inner body, and
consumer. Works today by SysV xmm0 luck; UB under any optimizer or non-x86_64
ABI.

**Status:** MITIGATED. `stdlib/arrow.tur` now exports `compose-float`
(monomorphic `:float -> :float`) which is register-class-correct; use it for
float pipelines. Direction B Stage A landed (2026-06-05) -- generic typed
closure-returning combinators now type-check at the elaboration boundary --
but `>>>` itself is **not** yet retyped pending Stages B-E.

### Layer 2 -- prerequisite: capturing-closure return-type lowering (RESOLVED)

[`../archive/history/boxed-fn-typed-closure-return-miscompiles.md`](../archive/history/boxed-fn-typed-closure-return-miscompiles.md)
-- a plain `defn` whose declared return type was a concrete non-boxed `(fn
[..] R)` and whose body yielded a capturing closure lowered to the wrong C
signature (the function's *result* type instead of the fat-closure carrier).
Hard `cc` error for `:float`/`:cstr`; `-Wint-conversion` "works by luck" for
`:int`.

**Status:** RESOLVED (2026-06-05). Fix in `src/compiler/elab_fns.c`:
`returns_boxed_closure` + non-boxed `TY_FN` return now marks the declared
result type `boxed`, lowering the signature/forward-decl/consumer in lockstep
to the `void *` carrier. Fixture
`tests/fixtures/boxed-fn-typed-closure-return/`. This was a Stage 0
prerequisite for Layer 1 Direction B and is now off the critical path.

### Layer 3 -- the open blocker: dispatching inner body erases intermediate types

[`../archive/history/poly-closure-inner-dispatch-result-erased.md`](../archive/history/poly-closure-inner-dispatch-result-erased.md)
-- per-monomorphization inner-body specialization works for the **dispatch-free**
shape (e.g. `(fn [t] : A val)`), verified by
`tests/fixtures/poly-closure-result-tyvar-float`. It does **not** work for the
**dispatching** shape (the actual `>>>` body, `(fn [x] (g (f x)))`): by the
time the inner `EX_CALL` for `(fv x)` reaches emit, its `e->type` is already
the int64 carrier, not `TY_TYVAR B`. `emit_resolve_type` can only substitute
named tyvars, so the spec clone still emits `int64_t(*)(void*, int64_t)`
dispatch casts even when the targets are `double(void*, double)`.

**Status:** OPEN. Guarded against silent miscompile by `TUR-E0705` in
`elab_call.c`, gated on `Binding.closure_return_dispatches`. A separate
sub-gap (`^fat f :(fn [A] B)` does not propagate `A/B/C` bindings because
nested tyvars in `(fn [A] B)` are stored as nameless `TY_STRUCT(def==NULL)`
placeholders by `elab_types.c`) compounds this -- a spike fix in
`type_expr_from_form` was reverted because it propagates bindings only to
crash immediately on the inner-dispatch erasure.

## Proposed fix sequencing (next actions)

1. **Layer 3 fix direction 3 (most targeted):** when the dispatched value is a
   captured binding with declared full type `(fn [..] R)`, resolve `R` through
   the spec bindings instead of reading the erased `e->type`. Requires the
   fn-typed-param tyvar preservation sub-fix first (re-stamp the placeholder
   `TY_STRUCT(def==NULL)` as a named `TY_TYVAR` in `type_expr_from_form`, add
   a `TY_FN` case in `call_type_has_named_tyvar`, add a fn-typed branch in
   call binding-collection).
2. **Then retype `>>>` itself** as the polymorphic typed combinator and
   regenerate `tests/fixtures/*/expected.c`. Drop the `TUR-E0705` guard for
   the now-handled shape.
3. **Validation:** existing `poly-closure-result-tyvar-float` stays green; new
   fixture composing two `:float -> :float` closures through stdlib `>>>`
   prints exact fractional output (e.g. `3.675`) and its `expected.c` shows
   `tur_thunk_double_double_t` end-to-end.

Heavier alternatives (per-spec re-elaboration of the inner body; retain tyvar
types on all intermediate sub-expressions) are spelled out in the Layer 3
original.

## Cross-references

- `docs/upcoming/poly-closure-result-specialization-plan.md` -- upstream plan
  tracking Stages B-E.
- `tests/fixtures/poly-closure-result-tyvar-float/` -- dispatch-free spec works.
- `tests/fixtures/boxed-fn-typed-closure-return/` -- Layer 2 regression.
- `tests/fixtures/generic-compose-int/` -- Direction B Stage A at `:int`.
- `tests/fixtures/arrow-compose-float/` -- monomorphic `compose-float` workaround.
