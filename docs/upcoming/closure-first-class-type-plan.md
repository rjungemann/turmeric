---
title: First-Class Closure Type (Closure Repr Unification Phase 3, Option B)
category: Planning
description: Implementation plan for Option B of the closure-representation-unification work -- a first-class closure type distinct from :ptr<void>, so every closure value is uniformly a fat box and raw C-callbacks keep the bare-pointer representation. Replaces the per-call-site boxing heuristic (Option A) and subsumes the remaining Phase 1 holdouts (option-eq?/pair-eq-carrier?/mutmap-eq?) and Phase 2 (nullary :ptr<void> fat dispatch). Broken into incremental, suite-green sub-phases.
---

# First-Class Closure Type -- Plan (Phase 3, Option B)

> **Status:** Draft Plan
> **Last Updated:** 2026-06-03
> **Type:** compiler -- closure ABI / type system
> **Parent:** [closure-representation-unification-plan.md](closure-representation-unification-plan.md)
> **Decision:** Option B chosen over Option A (per maintainer, 2026-06-03).
> **Reports this resolves:**
> - [ptr-void-direct-call-representation-split.md](../reported/ptr-void-direct-call-representation-split.md) (all four cells)
> - [eq-synthesis-dispatcher-passes-bare-comparator-to-fat-sink.md](../reported/eq-synthesis-dispatcher-passes-bare-comparator-to-fat-sink.md)
> - [fat-fn-param-capturing-closure-gap.md](../reported/fat-fn-param-capturing-closure-gap.md) (case C, bare `^fat`)

---

## Goal

A closure value has **one** runtime representation -- a fat box
`{ int64_t thunk; captures... }` -- and **one** type. Every site that calls a
closure value fat-dispatches; no site guesses by arity or by an overloaded
`:ptr<void>`. Raw C function pointers (the `contract.tur` /
`tur_set_contract_handler` case, and any future `qsort`-style binding) keep the
bare `:ptr<void>` representation, which becomes "raw pointer only" again.

This subsumes:

- **Phase 1 holdouts** -- `option-eq?`, `pair-eq-carrier?`, `mutmap-eq?` can
  take a closure-typed comparator; the constrained-`Eq` synthesis dispatcher
  emits a fat closure value instead of a bare `(void*)(intptr_t)(__fn_NNN)`.
- **Phase 2** -- the nullary `:ptr<void>` direct call disappears as a category;
  closures are called through the closure type, uniformly fat for all arities.
- **Report C** -- a bare closure parameter is directly callable because its
  type *is* the closure type, not an overloaded `:ptr<void>`.

## Current representation (what we are replacing)

- A **capturing** `(fn ...)` is minted at exactly one chokepoint --
  `elab_fns.c:2901`, `EX_CLOSURE` with `TYPE_PTR_VOID`. Its runtime value is a
  fat box.
- A **captureless** `(fn ...)` is a bare C function pointer, `TY_FN`
  (`elab_fns.c`), no environment.
- `:ptr<void>` is overloaded as *both* "fat closure handle" *and* "raw C
  pointer," with no local disambiguator -- the root cause in
  [ptr-void-direct-call-representation-split.md](../reported/ptr-void-direct-call-representation-split.md).
- The `^fat` marker (`is_fat` on `Binding`, `arg_fat[]` on `Type.as.fn`) is a
  *parameter-side* opt-in that already fat-dispatches and auto-boxes captureless
  args -- but only for params written `:(fn ...)` and marked `^fat`. It does not
  give closure *values* a distinct type.

## Representation decision: a `boxed` flag on `TY_FN`

Rather than introduce a brand-new `TY_CLOSURE` TypeKind (which would force a new
arm into every exhaustive `switch (TypeKind)` -- type equality, copy/substruct
kind, Send/Sync, `type_c_name`, kind-check, drop/RC, the printer, the LSP,
turi/eval, ...), represent a first-class closure as `TY_FN` carrying a new
`bool as.fn.boxed` bit:

- `boxed == false` -- a bare function reference (captureless fn / C function
  pointer address). The thin calling convention `R (*)(A...)`.
- `boxed == true` -- a fat closure value. Always a `{ thunk, env... }` box;
  called through the fat protocol (thunk = slot 0, env = box), for all arities.

Why a flag on `TY_FN`, not `:ptr<void>`:

- `TY_FN` already threads the full signature (`arity`, `arg_types[]`,
  `ret_type`, `arg_fat[]`), which is exactly what fat dispatch needs to emit the
  correct typed thunk -- and what the *bare* `^fat` form lacked (the
  [bare-fat-param-non-int-result-miscompiles.md](../reported/bare-fat-param-non-int-result-miscompiles.md)
  hazard). A boxed `TY_FN` is never int-result-only.
- The existing `is_fat` direct-call emit path (`emit_expr.c`, "ER2" /
  TY_FN+is_fat) already fat-dispatches a `TY_FN` binding through slot 0 for all
  arities including nullary. `boxed` reuses that path; `is_fat` (param marker)
  becomes "this parameter's `TY_FN` is `boxed`."
- `:ptr<void>` is freed to mean raw pointer only, restoring the type's honesty.

`type_eq` treats `boxed` as **compatible-but-coercible**: a bare `TY_FN`
coerces to a boxed `TY_FN` of the same signature via `EX_FN_TO_FAT` (the
existing auto-shim); a boxed `TY_FN` never silently decays to bare.

## Sub-phases (each independently mergeable, `bash tests/run.sh` green)

### B-0 -- add the `boxed` bit, inert

Add `bool boxed;` to `Type.as.fn`, zero-initialised in `type_fn*`
constructors. Thread it through `type_eq` (equal only when both bare or both
boxed, *plus* the bare->boxed coercion already mediated by `EX_FN_TO_FAT` at
call sites), `type_c_name` (boxed -> the int64 carrier / `void *`, same as a
fat handle today), and the type printer (`:(fn [...] :T)` vs
`:(closure [...] :T)` surface). Nothing sets `boxed = true` yet, so codegen is
unchanged. Validates the field is plumbed without behavior change.

### B-1 -- mint capturing closures as boxed `TY_FN`

At the single chokepoint `elab_fns.c:2901`, give `EX_CLOSURE` a boxed `TY_FN`
type (signature recovered from the lambda) instead of `TYPE_PTR_VOID`. Update
the direct-call emit (`emit_expr.c`) so a boxed `TY_FN` value dispatches through
the fat protocol -- reusing the `is_fat` path -- for **all** arities (this is
where Phase 2's nullary fix lands for free). Keep a compatibility coercion
`boxed TY_FN -> :ptr<void>` (identity at the C level -- both are the int64
carrier) so the pervasive existing stdlib that still types closures as
`:ptr<void>` keeps compiling unchanged. This is the load-bearing step; gate it
hard on the full suite.

### B-2 -- nullary fat dispatch + the typed closure-param spelling  *(DONE)*

**Load-bearing fix (shipped).** A direct call through a fat sink (an `is_fat`
parameter -- `^fat`, with or without a `:(fn ...)` annotation) now dispatches
through the fat protocol (slot 0 of the box) for **all** arities, including
`n == 0`. The `n == 0` `:ptr<void>` path is gated on `is_fat`: a fat sink reads
slot 0, while a *raw* `:ptr<void>` callback still thin-calls. This is the
disambiguator the nullary `:ptr<void>` direct call lacked -- it is the correct
realization of the held "Phase 2," fixing both halves of report
[ptr-void-direct-call-representation-split.md](../reported/ptr-void-direct-call-representation-split.md)
at `n == 0` (captureless-bare-fn-stays-thin AND closure/`^fat`-dispatches-fat)
without the unconditional-fat regression that held Phase 2. The typed
`^fat f :(fn [] :T)` spelling carries the **result type**, so a non-int (`:float`)
nullary result is read through the correctly-typed thunk (`tur_thunk_double_t`),
sidestepping the bare-`^fat` result-type-blindness of
[bare-fat-param-non-int-result-miscompiles.md](../reported/bare-fat-param-non-int-result-miscompiles.md).
Regression fixture: `tests/fixtures/fat-param-nullary-closure`.

**Proven dead end (do NOT retry): blanket `:(fn ...)` ⇒ boxed.** Making *every*
parameter written `:(fn [...] :T)` implicitly fat is **not viable**: an
effect-typed higher-order parameter `:(fn [...] #{E} :T)` is *also* a `TY_FN`
and cannot be distinguished from a closure sink by type alone. The blanket
pre-pass boxed those effect params, breaking effect-row subtyping and
suppressing expected effect-mismatch errors -- 106 fixtures failed (effect
`build failed` / `errors/effect-*` no-longer-erroring), plus ~194 codegen
snapshots churned (far more `:(fn ...)` params exist than a stdlib grep
suggests). The first-class closure *parameter* spelling therefore stays the
explicit, opt-in `^fat f :(fn [...] :T)` (typed, carries the result type), not a
silent default flip. A dedicated raw-C-callback spelling and any wholesale
migration off `:ptr<void>` are deferred to B-4 behind an explicit marker, never
an implicit reinterpretation of `:(fn ...)`.

### B-3 -- migrate the Phase 1 holdouts + synthesis dispatcher

Type the comparator parameters of `option-eq?`, `pair-eq-carrier?`,
`mutmap-eq?` (and the comonad `extend` helpers) as closures. Update the
constrained-`Eq` per-call-site synthesis dispatcher to emit a boxed closure
value (box the synthesized `__fn_NNN` via the per-signature `__tur_fatshim_*`)
instead of a bare `(void*)(intptr_t)` cast -- closing
[eq-synthesis-dispatcher-passes-bare-comparator-to-fat-sink.md](../reported/eq-synthesis-dispatcher-passes-bare-comparator-to-fat-sink.md).
Re-add the capturing-comparator fixtures (`pair-eq-capturing-closure`,
`mutmap-eq-capturing-closure`, option-eq portion) retired in Phase 1.

### B-4 -- retire the `:ptr<void>`-as-closure overload

Migrate the remaining stdlib closure-callback `:ptr<void>` params to the
closure type, then make a direct call on a *raw* `:ptr<void>` an error again
(it is no longer a closure). `:ptr<void>` is raw-pointer-only. Delete the
arity-dependent `:ptr<void>` direct-call paths in `emit_expr.c`. This is the
clean end-state; it can trail the others.

## Migration strategy

`:ptr<void>` closure params and boxed `TY_FN` share the **same C carrier** (an
int64 holding the box pointer), so B-1's compatibility coercion lets boxed
closures flow into legacy `:ptr<void>` params with no codegen change -- the
suite stays green while sub-phases B-2..B-4 migrate call sites incrementally.
The overload is only *removed* in B-4, after every closure sink is closure-typed.

## Risks

- **Exhaustive switches.** Using a `boxed` flag (not a new TypeKind) avoids new
  `switch` arms, but every `TY_FN` consumer must be audited for whether `boxed`
  changes its behavior (drop/RC: a boxed closure owns its env box; copy kind;
  Send/Sync of captured env).
- **`type_eq` coercion direction.** Bare->boxed must coerce (auto-shim);
  boxed->bare must not (a fat box is not a function address). Getting this
  asymmetry wrong silently miscompiles -- gate with register-class-distinct
  (`:int`/`:float`) fixtures at every sub-phase.
- **Fixture-snapshot churn.** Each sub-phase that changes emitted C for the
  auto-loaded prelude touches every `expected.c`. Regenerate per the CLAUDE.md
  procedure (with per-fixture `flags`) in the same PR.
- **Raw C-callback misclassification.** B-2/B-4 must keep the `contract.tur`
  handler bare; the raw-callback spelling must exist before closure params
  become boxed by default.

## Validation

- Per sub-phase: `bash tests/run.sh` zero `FAIL`, leak detection on.
- Register-class-distinct (`:int` + `:float`) capturing-closure fixtures at each
  migrated site.
- The four-cell `:ptr<void>` matrix from
  [ptr-void-direct-call-representation-split.md](../reported/ptr-void-direct-call-representation-split.md)
  collapses: captureless and capturing, nullary and n-ary, all dispatch
  correctly through the closure type.
- `contract.tur` raw-callback round-trip stays bare and working.
- `tests/fixtures/stdlib-test-runner-callback` (the n==0 captureless case that
  blocked Phase 2) passes with `test-fn` closure-typed.
