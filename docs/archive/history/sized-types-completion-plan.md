---
title: Sized Types -- Completion Plan (SZ4--SZ9)
category: Language Features
description: Phased plan to finish the sized-types implementation -- lift size indices from runtime-checked phantoms to statically-checked type-level naturals, and reconcile the documented-but-unparsed -Xsized-types flag
---

# Sized Types -- Completion Plan (SZ4--SZ9)

> **Status:** SZ4--SZ9 complete. The SZ0--SZ3 runtime layer was already shipped
> (see [archive/history/sized-types-plan.md](../archive/history/sized-types-plan.md),
> marked "SZ3 Complete") -- `stdlib/sized.tur`, `sized-buf.tur`,
> `sized-matrix.tur`, `sized-bits.tur`, and ~20 `sized-*` fixtures. This plan
> added: a real `-Xsized-types` flag implying `-Xgadt` (SZ4); reconciled docs
> (SZ5); a type-level size index (`SizeTerm`) that constructors refine instead
> of a phantom (SZ6, see [sized-types-index-spec.md](sized-types-index-spec.md));
> a **static** size check with diagnostic `TUR-E0260` and a documented runtime
> fallback (SZ7); index inference + `--dump-sizes` (SZ8); and a recorded 1.0
> disposition in the typing-gap matrix (SZ9). The remaining static-checking gap
> (type-index mismatch at arbitrary boundaries; inference through wrappers) is
> why `-Xsized-types` stays experimental for now -- see the SZ9 row in the
> matrix.
>
> **Prerequisites:** GADTs (G0--G4, `-Xgadt`). Sized types reuse the GADT
> skolem/index machinery (`SkolemEnv` in `src/compiler/types.h:182`,
> `is_gadt_type` at `types.h:624`); no new GADT elaborator work is assumed.
>
> **Flag:** `-Xsized-types` (implies `-Xgadt`). The flag is currently
> documented but **not parsed** -- `src/main.c` has no `strcmp(..., "-Xsized-types")`
> and `src/runtime/globals.h` has no `g_sized_types_enabled`. SZ4 makes the
> flag real so the rest can land incrementally behind it without disturbing
> the `-Xgadt`-gated runtime layer that ships today.
>
> **Snapshot:** `0.14.6`.
>
> **Last updated:** 2026-05-30

---

## Motivation

The advanced-type-system rationale lists sized types as a 1.0-class feature:
"track container dimensions as type-level naturals" so that matrix
multiplication, stack allocation, and array indexing are checked *at compile
time* with zero runtime cost
([advanced-type-system-rationale.md](../guides/advanced-type-system-rationale.md)
"Sized types"). The user guide
([sized-types-guide.md](../guides/sized-types-guide.md)) tells users to enable
the feature with `-Xsized-types` and describes size checking as a type-system
property.

The implementation only partially backs those promises:

- **The flag is vapor.** `-Xsized-types` appears in
  `compiler-flags-guide.md` (as "Planned") and in `sized-types-guide.md` (as
  if it exists), but no parsing code or global flag exists. The shipped stdlib
  is gated by `-Xgadt` instead. A user copying the guide's `-Xsized-types`
  invocation gets an unknown-flag path, not sized types.
- **Sizes are phantom, not indices.** In
  `(defgadt SizedVec [n a] (SVNil : (SizedVec Size int)) (SVCons int (SizedVec Size int) : (SizedVec Size int)))`
  the `n` index is never instantiated -- both constructors return the same
  `(SizedVec Size int)`. The type system cannot tell a length-3 vector from a
  length-4 one.
- **"Type checking" is runtime assertion.** `size-eq?`, `size-compat?`,
  `size-assert-eq!`, and `sized-matrix-assert-shape!` all reduce to
  `(= (size-eval ...) (size-eval ...))` evaluated at run time
  (`stdlib/sized.tur:301,439,482`). The one "compile-time" error fixture,
  `errors/sized-sz3-shape-mismatch`, only catches a generic `int`-vs-`Size`
  mismatch (TUR-E0001) -- it does not check that two dimensions are equal.

Goals:

- Make `-Xsized-types` a real, `-Xgadt`-implying flag so the docs stop lying.
- Lift size indices to the type level: a length-`n` `SizedVec` has a type that
  *mentions* `n`, and constructors refine it (`SVCons` takes `(SizedVec n)` and
  yields `(SizedVec (Add (Static 1) n))`).
- Make size equality/compatibility a **static** check with a dedicated
  diagnostic, falling back to a runtime check only where the size is not
  statically known (mirroring the contract -> refinement story).
- Keep the shipped runtime layer working: SZ0--SZ3 stdlib and fixtures must
  continue to pass, now additionally gated/aliased under `-Xsized-types`.

Non-goals (deferred, tracked elsewhere):

- Full dependent / Pi types and arbitrary value-indexed types -- see
  `post-mvp.md` and the rationale doc. Sized types stay restricted to
  natural-number size indices over the existing `Size` GADT.
- SMT-backed entailment for size arithmetic -- see
  [refinement-types-plan.md](../refinement-types-plan.md). SZ-static checking uses
  syntactic normalization (`size-normalize`/`size-simplify`, already in
  `stdlib/sized.tur`), not a solver. Non-trivial nonlinear equalities fall back
  to a runtime check rather than being proven.
- Monomorphization of size-indexed containers (`-O`); size indices are erased
  to the existing flat-buffer representation, no new runtime layout.

---

## Phase ordering at a glance

| Phase | Disposition | Why this order |
|---|---|---|
| SZ4 | Flag + plumbing | Make `-Xsized-types` real (implies `-Xgadt`); alias the shipped layer behind it. Unblocks everything; no semantics change. |
| SZ5 | Doc reconciliation | Fix the guide/flags-guide drift so the documented surface matches SZ4. Cheap; do before adding new semantics. |
| SZ6 | Type-level indices | Make size indices real type arguments that constructors refine (kill the phantom). Core of the feature. |
| SZ7 | Static size checking | Statically check size equality/compatibility with a dedicated diagnostic; runtime fallback when unknown. |
| SZ8 | Inference + elision | Infer size indices through `SVCons`/concat/slice so users rarely annotate. |
| SZ9 | Graduation feed-in | Feed the typing-gap flag matrix: decide `-Xsized-types`' 1.0 disposition once SZ6--SZ8 land. |

---

## Phase SZ4 -- Make `-Xsized-types` a real flag

The flag is documented but unparsed. Make it real and `-Xgadt`-implying, so
the shipped SZ0--SZ3 layer keeps working and later phases have a gate.

- **SZ4.1** Add `g_sized_types_enabled` to `src/runtime/globals.{h,c}`
  (default `false`), alongside the other `g_*_enabled` flags. *Done when:* the
  global exists and is referenced by the parser.
- **SZ4.2** Parse `-Xsized-types` in both flag paths in `src/main.c`
  (`wk_apply_flags` near line 4193 and the argv loop near line 7050), setting
  `g_sized_types_enabled = true` and `g_gadt_enabled = true` (implies
  `-Xgadt`, matching the `-Xsubstructural -> -Xlinear` precedent). *Done when:*
  `tur -Xsized-types emit-c file.tur` is accepted and the GADT machinery is on.
- **SZ4.3** Add the flag to the `--help` text and the flag-implication graph in
  `main.c`'s usage string. *Done when:* `tur --help` lists `-Xsized-types` and
  notes it implies `-Xgadt`.
- **SZ4.4** Make the `-Xgadt`-gated sized stdlib also reachable under
  `-Xsized-types` alone (since it implies `-Xgadt`, this should be automatic --
  add a fixture that proves it). *Done when:* a `sized-*` program compiles with
  only `-Xsized-types` on the command line.
- **SZ4.5** Fixtures: one happy fixture using `-Xsized-types` (not `-Xgadt`)
  in its `flags` file, and an `errors/` fixture confirming sized syntax is
  rejected with neither flag. *Done when:* both green and snapshotted.

---

## Phase SZ5 -- Reconcile sized-types documentation

The guides describe a flag and a checking model that SZ4 only partially makes
true. Align the docs with what SZ4 ships (and flag what SZ6--SZ7 will add).

- **SZ5.1** Update `compiler-flags-guide.md`: change `-Xsized-types` from
  "Planned / No phases have started" to its real status (flag exists,
  implies `-Xgadt`; runtime layer shipped, static checking in progress per
  this plan). Add it to the implication graph and stand-alone/implies lists.
  *Done when:* the guide's `-Xsized-types` entry matches `main.c`.
- **SZ5.2** Update `sized-types-guide.md`'s opening claim ("enabled with the
  `-Xsized-types` compiler flag") to be accurate, and clearly mark which
  checks are **runtime** today vs. **static** once SZ6--SZ7 land, so users are
  not misled about guarantees. *Done when:* the guide distinguishes runtime
  assertions from static checks.
- **SZ5.3** Cross-link this plan from the rationale doc's "Sized types" section
  and from the archived SZ0--SZ3 plan, so the "SZ3 Complete" marker points at
  the continuation. *Done when:* both docs link here.
- **SZ5.4** Confirm no fixture snapshots change (docs only) -- `tests/run.sh`
  green. *Done when:* zero `FAIL` lines.

---

## Phase SZ6 -- Type-level size indices (kill the phantom)

Make the size index a real type argument that constructors refine, so a
length-`n` vector's type mentions `n`. This is the substantive feature work and
builds directly on GADT skolem refinement.

- **SZ6.1** Specify the indexed `SizedVec` shape: `SVNil : (SizedVec (Static 0) a)`
  and `SVCons : a -> (SizedVec n a) -> (SizedVec (Add (Static 1) n) a)`, with
  `Size` as the index sort. Write down how the index threads through `match`
  via the existing `SkolemEnv`. *Done when:* the representation and refinement
  rule are documented.
- **SZ6.2** Allow `Size` expressions as GADT type indices in
  `type_expr_from_form` / the defgadt elaborator (`elab_call.c:558`), so
  `(SizedVec (Add (Static 1) n) a)` parses as a type with a `Size`-kinded
  index rather than collapsing to `Size`. *Done when:* the indexed `defgadt`
  elaborates without the phantom collapse.
- **SZ6.3** Refine the index in `match` arms: matching `SVCons` binds the tail
  at `(SizedVec n a)` and the scrutinee at `(SizedVec (Add (Static 1) n) a)`,
  reusing GADT skolem equalities. *Done when:* a function over `SizedVec` sees
  the refined index per arm.
- **SZ6.4** Erase indices in codegen: the emitted C is unchanged from the
  current flat representation (size indices are compile-time only, zero-cost).
  *Done when:* `emit-c` output for an indexed `SizedVec` matches the erased
  shape and the program runs identically.
- **SZ6.5** Fixtures: indexed-`SizedVec` construct/match round-trip, and an
  `emit-c` snapshot proving index erasure (no runtime size field added).
  *Done when:* all green and snapshotted.

---

## Phase SZ7 -- Static size checking

With real indices, make size equality/compatibility a compile-time check with
its own diagnostic, falling back to the existing runtime assertion only when a
size is not statically known.

- **SZ7.1** Add a dedicated diagnostic (e.g. `TUR-E02xx` "sized type mismatch:
  size N is not M") distinct from the generic TUR-E0001, and reserve it in the
  diagnostics table. *Done when:* the code exists and `tur --explain` describes
  it.
- **SZ7.2** Check size-index equality statically where both sizes normalize to
  constants (reuse `size-normalize`/`size-simplify` from `stdlib/sized.tur` at
  the type level): a mismatched dimension in e.g. matrix-multiply or a
  `size-assert-eq!` on two statically-known sizes is rejected at compile time.
  *Done when:* a statically-unequal use is rejected with the SZ7.1 diagnostic,
  no runtime check emitted.
- **SZ7.3** Define and document the fallback boundary: when a size is not
  statically known (e.g. derived from a runtime length), emit the existing
  runtime assertion instead of a static error -- never silently accept.
  *Done when:* the runtime-fallback path is documented and exercised.
- **SZ7.4** Migrate `errors/sized-sz3-shape-mismatch` (and add new error
  fixtures) so they assert a *real* dimension check, not just the int-vs-`Size`
  TUR-E0001. *Done when:* the error fixtures fail on dimension mismatch
  specifically, with the SZ7.1 code.
- **SZ7.5** Fixtures: static-reject (equal/unequal constant sizes), static-
  accept, and runtime-fallback for an unknown size. *Done when:* all green and
  snapshotted.

---

## Phase SZ8 -- Size inference and elision

Make the indexed types ergonomic: infer size indices so users rarely write
them, mirroring how lifetime/typeclass inference hides machinery.

- **SZ8.1** Infer the result index of `SVCons`/concat/replicate from operand
  indices (`n + 1`, `n + m`, `k`), so constructed vectors carry the right size
  without annotation. *Done when:* `(SVCons x (SVCons y SVNil))` infers
  `(SizedVec (Static 2) a)` with no annotation.
- **SZ8.2** Allow an unannotated `SizedVec` parameter to elaborate to a fresh
  size variable that unifies across the body (size-index inference), so generic
  functions over "any length" type-check. *Done when:* a length-polymorphic
  function elaborates without an explicit index.
- **SZ8.3** Document which size expressions are inferable (linear `Add`/`Mul`
  over `Static` and one variable) vs. which require annotation or fall back to
  runtime (nonlinear / multi-variable), so behavior is predictable. *Done
  when:* the supported inference shapes are written down.
- **SZ8.4** Fixtures: inferred-size construction, length-polymorphic function,
  and an un-inferable case that requires annotation. *Done when:* all green and
  snapshotted.

---

## Phase SZ9 -- Graduation feed-in (typing-gap matrix)

Sized types were absent from the TY1 flag-graduation matrix because the flag
did not exist. Once SZ6--SZ8 land, record an explicit 1.0 disposition for
`-Xsized-types`.

- **SZ9.1** Add `-Xsized-types` as a row in
  [typing-gap-plan.md](archive/typing-gap-plan.md) TY1 matrix with fixture coverage
  and current state. *Done when:* the matrix lists the flag.
- **SZ9.2** Record its 1.0 disposition (graduate-default-on vs. stay
  experimental) with a rationale tied to whether SZ6--SZ8 have closed the
  static-checking gap. *Done when:* the disposition is written and consistent
  with the TY1 "graduate only gap-free flags" stance.
- **SZ9.3** Note cross-dependencies: `-Xsized-types` implies `-Xgadt`, so its
  graduation tracks `-Xgadt`'s (already slated to graduate per TY1). *Done
  when:* the dependency is recorded.

---

## Exit criteria

- `-Xsized-types` is parsed, implies `-Xgadt`, and appears in `--help` and the
  flags guide; the docs no longer describe a non-existent flag (SZ4, SZ5).
- `SizedVec` (and the matrix/bitvec analogues) carry a real type-level size
  index that constructors refine; the phantom collapse is gone (SZ6).
- Size equality/compatibility is checked statically where sizes are known, with
  a dedicated diagnostic, and falls back to a runtime check -- never silent
  acceptance -- when they are not (SZ7).
- Size indices infer through common constructors so annotation is rare (SZ8).
- The shipped SZ0--SZ3 runtime layer and all existing `sized-*` fixtures still
  pass, now reachable under `-Xsized-types` (SZ4--SZ7).
- `-Xsized-types` has a recorded 1.0 disposition in the typing-gap matrix
  (SZ9).
- `bash tests/run.sh` reports zero `FAIL` lines and all fixture snapshots are
  regenerated per CLAUDE.md.

## See also

- [sized-types-index-spec.md](sized-types-index-spec.md) -- SZ6 representation + refinement rule (SizeTerm, SkolemEnv threading, erasure)
- [archive/history/sized-types-plan.md](../archive/history/sized-types-plan.md) -- the shipped SZ0--SZ3 runtime layer
- [sized-types-guide.md](../guides/sized-types-guide.md) -- user guide (to be reconciled in SZ5)
- [advanced-type-system-rationale.md](../guides/advanced-type-system-rationale.md) -- the intended sized-types design
- [typing-gap-plan.md](archive/typing-gap-plan.md) -- flag-graduation matrix (SZ9 feeds it)
- [refinement-types-plan.md](../refinement-types-plan.md) -- the SMT-backed static-checking story sized types deliberately avoid
