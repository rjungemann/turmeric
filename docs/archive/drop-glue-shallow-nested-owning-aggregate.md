# By-value drop glue is shallow -- a nested owning aggregate field is never torn down

**Status: RESOLVED (2026-07-20)** for the core defect -- non-parametric by-value
nested owning aggregates. Both fix directions (1) and (2) landed:

- **Transitive `needs_drop_glue`** (`src/compiler/elab_structs.c`): a field whose
  inner def is a non-`:heap`, non-parametric by-value product that itself
  `needs_drop_glue` now flips the owner's flag and records the inner def on a new
  `CtorField.drop_inner_def` slot. (`full_type` stays NULL for such a carrier
  field -- recording a carrier-ADT `full_type` would misclassify field *reads* --
  so a dedicated slot carries the inner def for the drop path only.)
- **Nested-aggregate drop/walk case** (`src/compiler/emit_module.c`,
  `emit_adt_byval_drop_glue`): a `drop_inner_def` field emits
  `drop_glue_tur_adt_<Inner>((void*)s->field)` (releases the boxed sub-aggregate's
  owners *and* frees the box -- a plain heap allocation uniquely owned under the
  same move discipline a direct rc field relies on) and, in walk glue,
  `walk_glue_tur_adt_<Inner>(...)`. Order-independent via forward decls.

Verified end-to-end under ASan/LSan+UBSan: the rc-wrapped byval repro (whose drop
glue actually *runs* -- see note below), a 3-level transitive nest, and the
direct-rc + nested-sibling case (consequence #2) all drop to **0 leaked, no
double-free**. Regression fixture:
`tests/fixtures/drop-glue-nested-owning-aggregate/`. Full suite 2214/0, no
snapshot churn.

**Scope limits deliberately left (separate, narrower concerns):**

- A `:heap` inner is excluded (its typed-pointer teardown is the deferred
  by-value/`:heap` local-drop work). Note the original repro's `H5` is `:heap`,
  so its *own* drop glue is never invoked regardless -- the verifiable target is a
  **byval** aggregate wrapped in `rc/of`, where the rc control block's `drop_fn`
  is the byval drop glue and so actually runs.
- A parametric applied monomorph inner (`(Pair rc<int> int)`) is excluded --
  its drop glue is a mangled monomorph name, not the base `tur_adt_<name>`;
  threading that is separate work.
- A forward-referenced inner (nested type defined *after* the owner) is not seen
  transitively (single-pass), matching the existing `record_full` forward-stub
  caveat.
- Fix direction (4) -- loosening the owning-cloneable-capture admission
  predicate in `cps_ir.c` / `emit_cps_ir.c` now that the base drop is complete --
  is an E3 follow-on left to that machinery; the predicate still conservatively
  rejects (no regression).

Original report follows.

---

**Severity:** medium (bounded per-value leak; not a crash or miscompile).
General codegen -- **not** CPS/effect-specific (reproduces with no effects at
all). Surfaced while building the E3 owning-cloneable-capture work: it is the
base-language boundary that bounds which owning shapes the consuming-capture
env_clone can admit (a shape whose base drop leaks would leak per resume too),
so those admission predicates in `src/passes/cps_ir.c` /
`src/compiler/emit_cps_ir.c` deliberately reject a nested-aggregate owning field.

## Summary

The by-value ADT/struct drop glue (`drop_glue_tur_adt_<Name>`) only tears down
a field that is **directly** `rc` / `ref` / `weak`. A field that is itself an
owning by-value aggregate (a nested `defstruct` / by-value ADT product that
transitively owns an `rc`/`ref`/`weak`) is neither counted by `needs_drop_glue`
nor recursed into by the drop body. Two consequences:

1. If a type's *only* owning content is a nested aggregate field,
   `needs_drop_glue` stays `false` and **no drop glue is emitted at all** -- the
   value (and its nested owner) leaks in full.
2. Even when a type gets drop glue for its own direct owning field, a *sibling*
   nested-aggregate field is still skipped -- a partial teardown.

## Minimal repro (no effects)

```turmeric
;; Own: a by-value aggregate with a DIRECT rc field -> needs_drop_glue = true,
;; gets drop_glue_tur_adt_Own.
(defstruct Own [r : rc<int> tag : int])

;; H5: a :heap carrier whose ONLY owning content is the NESTED aggregate `inner`.
;; `inner` is not directly rc/ref/weak, so needs_drop_glue stays FALSE and NO
;; drop_glue_tur_adt_H5 is emitted -- inner.r leaks when an H5 is dropped/freed.
(defstruct H5 :heap [inner : Own tag : int])

(defn main [] : int
  (let [o (make-struct Own :r (rc/of 7) :tag 3)
        h (make-struct H5 :inner o :tag 9)]
    (.tag h)))
```

`tur emit-c` emits `drop_glue_tur_adt_Own` but **no** `drop_glue_tur_adt_H5`.
Built with ASan/LSan linked (`TUR_CC_FLAGS="... -fsanitize=address,undefined"`,
which the default `tests/run.sh` `TUR_CC_FLAGS` does **not** carry, so the suite
never catches this), the program reports:

```
Direct leak of 16 byte(s) -- ctor_H5           (the H5 header, never freed)
Indirect leak of 16 byte(s)                    (inner.r's rc control block)
SUMMARY: AddressSanitizer: 32 byte(s) leaked in 2 allocation(s).
```

## Root cause

Two sites, both keyed on a field being *directly* one of the owning kinds:

- **`needs_drop_glue` gate** -- `src/compiler/elab_structs.c:1262` and `:1373`:
  ```c
  if (fkind == TY_RC || fkind == TY_REF || fkind == TY_WEAK)
      def->needs_drop_glue = true;
  ```
  A nested aggregate field stores as an inline by-value aggregate (or an int64
  carrier), i.e. `fkind` is `TY_ADT`/`TY_STRUCT`/`TY_INT`, never one of the three
  owning kinds, so it never flips the flag. A type whose only owner is nested
  gets `needs_drop_glue == false` and is skipped by
  `emit_adt_byval_drop_glue`'s early return (`emit_module.c:5263`).

- **drop body** -- `src/compiler/emit_module.c:5270`-`5282`
  (`emit_adt_byval_drop_glue`): the per-field loop only handles `TY_RC`
  (decref), `TY_WEAK` (weak-decref), and `TY_REF`/`TY_LREF` (`free`). There is
  no `TY_ADT`/`TY_STRUCT` case that calls the nested type's own
  `drop_glue_tur_adt_<Inner>(&s->inner)`, so even a type that *does* get drop
  glue leaks any nested-aggregate sibling field.

## Fix directions

1. Make `needs_drop_glue` **transitive**: a field whose resolved type is a
   by-value aggregate that itself `needs_drop_glue` should flip the owner's
   flag. The field's `full_type` already carries the inner def on the inline
   path (`adt_field_is_inline_byval`), so the inner `AdtDef` is reachable.
2. Add a nested-aggregate case to `emit_adt_byval_drop_glue`'s loop: for an
   inline by-value aggregate field, emit `drop_glue_tur_adt_<Inner>(&s->field)`
   (taking the address of the inline sub-aggregate, not `free`-ing it -- the
   sub-aggregate is inlined, not separately heap-allocated), and enumerate its
   strong children in `walk_glue_*` likewise. A carrier (boxed) nested aggregate
   would instead decref/free through its box.
3. Ordering: run the nested field's drop in the same reverse-field order the
   existing loop uses, so a field's teardown precedes an earlier field's.
4. Until this lands, the owning-cloneable-capture consuming path correctly
   **rejects** a nested-aggregate owning field (it would leak once per multi-shot
   resume), and this leak stays latent for direct code that constructs such a
   value. It is invisible to `bash tests/run.sh` because the harness compiles
   emitted programs without `-fsanitize=address` (correctness/stdout only).
