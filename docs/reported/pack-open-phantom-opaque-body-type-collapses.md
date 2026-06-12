---
title: pack/open body type collapses for phantom-indexed defopaque (open projects bare type, drops type-app applied form)
category: Elaborator gap / existential machinery
severity: Latent expressiveness hole. The native existential operators `pack`/`open` work for `SizedVec` (a defgadt whose `n` is constructor-derived) but silently lose the applied-form indexing when the body type names a phantom defopaque such as `(SizedBuf n)`. The opened binder projects bare `SizedBuf` instead of `(SizedBuf n)`, so any downstream operation that expects `(type-app SizedBuf tyvar)` rejects the value. Surfaces when validating the generic stdlib pattern from P4 of ecs-e2c-sized-dense-needs-bounded-world.md.
description: `(pack v (exists [n] (SizedBuf n)))` followed by `(open ... [n buf] ...)` binds `buf` at type `SizedBuf` (bare), not `(SizedBuf n)`. The SizedVec case works because the GADT constructor chain carries the `n` index through the type, but the defopaque path goes through a different lowering that drops the applied form on the projection. Symptom: `TUR-E0001 expected (type-app SizedBuf tyvar), got SizedBuf` on any subsequent call that uses `buf` against a signature mentioning `(SizedBuf n)`.
status: RESOLVED 2026-06-12. `elab_open` (`src/compiler/elab_types.c`) now preserves the applied form when the existential body's head is a defopaque, so `(SizedBuf n)` projects to `(SizedBuf n)` (was bare `SizedBuf`). The SizedVec path (TY_ADT head, bare-nominal call sites) is unchanged. Witness: `tests/fixtures/sized-buf-existential-pack-open`; suite 1552 pass / 82 fail (no regressions).
---

# pack/open body type collapses for phantom-indexed defopaque

> **RESOLVED 2026-06-12.** Fix landed in `src/compiler/elab_types.c`
> `elab_open` (around line 2535): instead of unconditionally peeling
> `(SizedBuf n)` to bare `SizedBuf` via `ex2_peel_phantom_app`, we now
> branch on the head kind. For a defopaque head (`TY_STRUCT` with a
> real `def`), keep the body type intact so signatures declared as
> `(Op n)` -- e.g. `sized-buf-len`, `sized-buf-copy!` -- unify their
> `n` against the open's bound binder. For an ADT head (SizedVec /
> defgadt path), peel as before so the 14+ stdlib entry points
> declared with bare-nominal param types stay green. The SizedVec
> contrast in T1 is therefore the *reason* both paths now coexist,
> not a regression risk. Validation: new accept fixture
> `tests/fixtures/sized-buf-existential-pack-open` prints `5`;
> existing `tests/fixtures/sized-handle-existential-pack-open`
> (SizedVec) stays green; suite at 1552 pass / 82 fail (+1, no new
> failures).
>
> Two follow-up gaps surfaced while writing the validation fixtures
> and are filed separately rather than expanding this fix:
>
> 1. **Codegen monomorphization of polymorphic stdlib helpers reached
>    from inside `open`.** A call like `(sized-buf-free buf)` inside
>    an open body type-checks but the monomorphizer does not emit
>    `sized_hybuf_hyfree`, leaving the C link step undefined. The
>    accept fixture works around this by carrying
>    `requires.no-leak-check` and leaving the free out of the body.
>    See the new gap report
>    [open-monomorphizes-polymorphic-fn-only-partially.md](open-monomorphizes-polymorphic-fn-only-partially.md).
> 2. **Skolem-distinctness for nested `open` binders.** Two `(open ...
>    [n_i x_i] ...)` bind two `n_i` that are both `TY_STRUCT` /
>    `def=NULL` and so `type_eq` cannot distinguish them. A
>    cross-skolem call like `(sized-buf-copy! a b)` where `a`/`b` come
>    from different opens type-checks instead of rejecting under
>    TUR-E0260. See
>    [open-binder-skolems-not-distinguishable.md](open-binder-skolems-not-distinguishable.md).

## Summary

The native `pack`/`open` existential operators support `(exists [n]
(SizedVec n int))` correctly: opening the value binds the second name
at type `(SizedVec n int)` with the fresh `n` in scope. The analogous
shape over a phantom-indexed defopaque -- `(exists [n] (SizedBuf n))` --
opens the second name at the bare `SizedBuf` head, with the `n`
application dropped. Any downstream call whose signature mentions
`(SizedBuf n)` then fails with a head-vs-app type mismatch.

## Minimal repro

```turmeric
(load "stdlib/sized.tur")
(load "stdlib/sized-buf.tur")

(defn main [] : int
  (let [packed (pack (sized-buf-new-zeroed 5) (exists [n] (SizedBuf n)))]
    (open packed [n buf]
      (println (sized-buf-len buf))
      (sized-buf-free buf)))
  0)
```

Compiled with `-Xsized-types`:

```
input.tur:7:31: error [TUR-E0001]: function 'sized-buf-len' arg 1:
  expected (type-app SizedBuf tyvar), got SizedBuf
```

`sized-buf-len`'s declared param type is `(SizedBuf n)`. The opened
binder `buf` is at bare `SizedBuf`, so head unification matches but the
expected `tyvar` arg has no `SizedBuf` arg on the actual to unify with.

`stdlib/sized-buf.tur` declares `(defopaque SizedBuf [n] :int)` -- a
single-parameter phantom defopaque, the simplest shape that should
unify with `[n]` in the existential binder.

## Observed vs expected

- Observed: opened binder type is the bare nominal head `SizedBuf`, no
  applied form, no captured fresh `n`.
- Expected: opened binder type is `(SizedBuf n)` with `n` bound to a
  fresh abstract size index, mirroring the SizedVec path.

## Contrast: SizedVec works

The same shape on `SizedVec` (a defgadt whose constructors give `n` a
witness chain) elaborates cleanly:

```turmeric
(load "stdlib/sized.tur")
(load "stdlib/sized-handle-existential.tur")

(defn main [] : int
  (let [packed (pack-sized (SVCons 1 (SVCons 2 (SVCons 3 (SVNil))))
                           [n] (SizedVec n int))]
    (open-sized packed n sv
      (println (sized-vec-len sv))))   ; => 3
  0)
```

This is the validated path covered by
`tests/fixtures/sized-handle-existential-pack-open` and the older
`tests/fixtures/ex2-4-vec-existential`. The defopaque path needs to
match it.

## Root-cause hypothesis

The `pack` value-side type-check probably sees `value` at type
`SizedBuf` (the make-side never widens the carrier to `(SizedBuf n)`
because the `n` is phantom) and unifies that against the body type
`(SizedBuf n)` by stripping the application on the body. The
existential then stores the *head* as its body type, so `open` projects
the head -- not `(SizedBuf n)`.

The SizedVec path dodges this because the constructor chain
`(SVCons int (SizedVec n) : (SizedVec (Add 1 n)))` makes the value's
type an applied form, so the unification keeps the application
structure on both sides.

File pointers (to validate against during a fix):
- existential / pack / open machinery in the elaborator (`src/compiler/elab_*.c`,
  likely the `EX_PACK`/`EX_OPEN` handlers and the existential
  type-application reconciler)
- phantom-defopaque elaboration in `elab_types.c`

## Proposed fix directions

1. **Preserve applied form on phantom defopaque pack.** When packing a
   value whose declared type is a bare phantom defopaque against a body
   type `(Op n)` with `n` in the binder, treat the value's type as
   `(Op ?)` for the unification and bind `n` to the fresh abstract size
   index. The opened binder then carries `(Op n)`.
2. **Project applied form on open.** Even if the pack stored the bare
   head, the open could re-apply the binders to the body when projecting
   the second name -- effectively `(body-head $binders...)`. Less
   invasive than (1) but only works for the trivial case where every
   binder applies positionally to the head.

(1) is the principled fix; (2) is a stopgap.

## Validation

- A new accept fixture
  `tests/fixtures/sized-buf-existential-pack-open` runs the minimal
  repro above and prints `5` (after the fix).
- The existing `tests/fixtures/sized-handle-existential-pack-open`
  SizedVec fixture stays green (no regression on the
  GADT-constructor-chain path).
- An error-side fixture asserts the size index unifies inside the
  open body, e.g. inside `(open ... [n buf] (sized-buf-copy! buf buf))`
  type-checks while `(sized-buf-copy! buf other-buf)` (where
  `other-buf` carries a different fresh index from a separate open)
  fails with TUR-E0260.

## Tasks

Ordered, sized to be picked off one at a time. T1--T3 are the
investigation; T4--T6 land the fix; T7--T9 close out the suite, docs,
and downstream consumers. Each task names the file(s) and the
acceptance signal.

### T1 -- Confirm the divergence point with debug traces

- [ ] Add temporary `fprintf(stderr, ...)` taps around the `EX_PACK`
      value/body unify and the `EX_OPEN` projection in
      `src/compiler/elab_*.c` (grep `EX_PACK`, `EX_OPEN`). Print the
      *Type* (head + applied args) of:
      1. the pack value's declared type,
      2. the existential's body type,
      3. the unifier's substitution after pack,
      4. the type bound to the second open binder.
- [ ] Run the SizedBuf minimal repro vs the SizedVec contrast.
- [ ] Record where the SizedBuf path loses the `(... n)` application:
      pack-side widening, body normalisation, the open projection, or
      the type-app printer. Note the function + line.

Acceptance: a one-paragraph trace report appended to this doc under
"Diagnosed at". Strip the taps before any commit.

### T2 -- Identify the make-side type of `sized-buf-new-zeroed`

- [ ] Check the return type of `sized-buf-new-zeroed` after ascription
      (`(:: ... :SizedBuf)` -- see `stdlib/sized-buf.tur` around the
      `__sized-buf-new-zeroed-raw` wrapper). Determine whether the
      elaborator treats the result as bare `SizedBuf` or as
      `(SizedBuf ?)` with a fresh tyvar.
- [ ] Repeat for `sized-buf-new`. Both wrappers are involved in the
      sized-types accept fixtures and behave the same.

Acceptance: a one-line answer "bare nominal" vs "applied with fresh
tyvar" added to the root-cause section of this doc.

### T3 -- Check whether other phantom defopaques exhibit the same bug

- [ ] Pick a second phantom defopaque (any `(defopaque X [k] :int)`
      in stdlib or fixtures) and write a 6-line repro mirroring the
      SizedBuf one.
- [ ] Note in this doc whether the gap is SizedBuf-specific (would
      indicate a `sized-buf.tur` ascription issue) or generic (would
      indicate an elaborator-side fix).

Acceptance: a sentence in this doc saying "Generic phantom-defopaque
gap, confirmed against `<other-type>`" or "SizedBuf-specific".

### T4 -- Pick a fix direction

- [ ] Read both "Proposed fix directions" with the T1--T3 evidence in
      hand.
- [ ] Decide between (1) preserve applied form on pack and (2)
      re-apply binders on open. Default to (1); choose (2) only if T1
      shows the existential storage is shared with other forms where
      (1) would be invasive.
- [ ] Record the decision (and one-line rationale) in this doc.

Acceptance: "Chosen direction: N" line added to this doc.

### T5 -- Implement the fix

For direction (1):

- [ ] In the `EX_PACK` handler, when the body type's head is a
      defopaque and the value's declared type is the bare nominal
      head of the same defopaque, instantiate the value-side to
      `(Op fresh1 fresh2 ...)` for each binder position before
      unifying. Use the existing fresh-tyvar helper in the elaborator.
- [ ] Ensure the resulting existential body type retains the applied
      form so `EX_OPEN`'s projection inherits the application.
- [ ] Re-use the SizedVec path's projection code unchanged.

For direction (2):

- [ ] In the `EX_OPEN` handler, after binding the existential's
      witness binders to fresh skolems, rebuild the projected handle's
      type by re-applying the binders positionally to the body head.

Acceptance: the SizedBuf repro in this doc elaborates without
TUR-E0001 and prints `5`. The SizedVec fixture stays green.

### T6 -- Add accept + reject fixtures

- [ ] Create `tests/fixtures/sized-buf-existential-pack-open/`:
      `input.tur` from the "Minimal repro" section above (printing
      `5`), `flags` = `-Xsized-types`, `expected.stdout` = `5`.
- [ ] Create `tests/fixtures/errors/sized-buf-existential-cross-open-reject/`:
      two separate `open` blocks bind two SizedBufs at distinct fresh
      `n`s, then call `sized-buf-copy!` across them. Expect
      `TUR-E0260` (cross-parameter size mismatch). Use the existing
      `sized-buf-cross-param-reject` fixture as a structural template.
- [ ] Run `bash tests/run.sh 2>&1 | grep "^FAIL"` and confirm no new
      regressions.

Acceptance: both fixtures land green; the FAIL list is unchanged
from the pre-fix baseline.

### T7 -- Regenerate codegen snapshots

- [ ] Run the codegen snapshot regen recipe from CLAUDE.md ("Fixture
      Snapshots -- STRICT RULE") if the fix touches `emit_*.c` or
      changes generated C for any existing fixture.
- [ ] Commit the snapshot deltas alongside the fix; no separate
      "regen" PR.

Acceptance: `bash tests/run.sh 2>&1 | grep "^FAIL"` is clean and
`git diff tests/fixtures/*/expected.c` reflects only the intended
churn.

### T8 -- Promote the macro doc

- [ ] In `stdlib/sized-handle-existential.tur`, drop the SizedBuf-
      caveat phrasing from the file-level docstring and add SizedBuf
      to the "Example" line of `pack-sized` (the macros themselves
      don't change; the docstring just tracks reality).
- [ ] In `docs/reported/ecs-e2c-sized-dense-needs-bounded-world.md`
      under "P4 -- An existential lift", remove the
      "phantom-defopaque elaborator gap" caveat and link back to
      this report's resolution note.

Acceptance: a casual reader of the macro file no longer sees the
caveat, and the P4 section of the ECS report is unconditional.

### T9 -- Mark this report resolved

- [ ] Replace the status frontmatter line with `RESOLVED <date>`.
- [ ] Add a "Resolved" header at the top of the body summarising the
      chosen direction and the landing commit.
- [ ] Move the file to `docs/archive/history/` per the next archive
      churn sweep (do not move it eagerly -- batch with the next
      sweep so the archive history stays grouped per release).

Acceptance: the file's frontmatter reads RESOLVED; the SizedBuf
existential round-trip is a first-class supported path.

## Related

- `docs/reported/ecs-e2c-sized-dense-needs-bounded-world.md` -- P4
  explicitly motivates this work (sized-world `(world-new 1024)`
  lifting into `(exists [n] (World n))` for the for-each unifier).
  Once this gap closes, P4 will work transparently on any sized world
  declared as a phantom-indexed defopaque.
- `stdlib/sized-handle-existential.tur` -- the generic `pack-sized` /
  `open-sized` macros (P4 deliverable).
- `stdlib/vec-existential.tur` -- the SizedVec-specific predecessor
  that does work today.
- `tests/fixtures/sized-handle-existential-pack-open` -- the SizedVec
  validation of the macro pattern.
