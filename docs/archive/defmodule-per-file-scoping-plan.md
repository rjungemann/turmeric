---
title: defmodule "first form" check -- per-file scoping
category: Planning
description: The "defmodule must be the first form in the file" check currently fires across the whole compilation unit, not per file. This breaks the "spice loaded from a stdlib helper" pattern surfaced by the M6 httpd-compress work. Generalise the existing per-stdlib-file reset so any (load ...)-imported file is its own scope for the check.
---

# defmodule "first form" check -- per-file scoping -- Plan

> **Status:** Draft Plan
> **Last Updated:** 2026-06-03
> **Type:** Compiler -- elaborator diagnostic scope fix
> **Related:**
> - `src/compiler/elab_toplevel.c` -- the elaborator hosts both the check
>   (around line 896) and the existing per-auto-loaded-stdlib reset
>   (around line 934, "Phase M7").
> - `docs/archive/httpd-compression-zlib-spice-plan.md` (M6) -- the
>   plan whose execution surfaced the breakage; the `tur/zlib` spice
>   shipped with `defmodule` dropped as a workaround.
> - `../turmeric-spices/spices/zlib/src/tur/zlib.tur` -- where the
>   workaround lives today; the fix lets us restore `(defmodule tur/zlib
>   ...)` without re-introducing the load-order trap.

---

## Goal

Restore the natural reading of the error message:

> `defmodule must be the first form in the file`

so that it actually means **the file**, not **the whole compilation
unit**.  Two concrete outcomes:

1. A defmodule-wrapped spice file remains valid regardless of what
   loaded before it via `(load ...)`.  In particular, the M6 path --
   `(load "stdlib/httpd-compress.tur")` ->
   `(load "stdlib/httpd.tur")` -> transitively `(load "stdlib/json.tur")`
   -> `(load "../turmeric-spices/spices/zlib/src/tur/zlib.tur")` --
   must succeed with `(defmodule tur/zlib ...)` intact.

2. The intra-file rule stays: writing a defn or other top-level form
   **above** the defmodule in the same source file is still an error,
   because that is what the message says and it is a useful correctness
   check (the user almost certainly meant to put it inside the body).

---

## What's broken today

`src/compiler/elab_toplevel.c` around line 896 walks every user-range
top-level form looking for `defmodule`.  If the first defmodule it
finds is not at offset `stdlib_prefix` (the first user form), it emits
the error and stitches a note onto the form at `stdlib_prefix`.

That works for `tur build foo.tur` when `foo.tur` itself is the only
user file, because `stdlib_prefix` ends exactly where the auto-loaded
stdlib ends and the user file begins.  It breaks once explicit
`(load ...)` directives splice additional files into the user-form
range: every defn from those loaded files counts as a "user form
before defmodule" even when defmodule is the first form *of its own
file*.

There is already a precedent for the right fix sitting six lines
below the check.  Phase M7 added a reset:

```c
/* Phase M7: Each auto-loaded stdlib file is conceptually its own
 * file, so reset has_defmodule after each stdlib defmodule. */
if (i < stdlib_prefix && items[i] && items[i]->kind == EX_DEFMODULE) {
    e.has_defmodule = false;
}
```

This already accepts that "the file" is the right unit, but only
applies it to auto-loaded stdlib.  The plan generalises that intuition
to every `(load ...)`-spliced file.

---

## Phases

### Phase D0 -- Reproduce + lock the regression

Add a failing fixture under `tests/fixtures/errors/` is the wrong home
(this is the *happy* path that should compile).  Instead add a happy
fixture `tests/fixtures/elab-defmodule-after-load/`:

```
;; input.tur
(load "tests/fixtures/elab-defmodule-after-load/helper.tur")
(load "tests/fixtures/elab-defmodule-after-load/wrapped.tur")
(main-tag)
```

Where `helper.tur` carries a bare `(defn helper-tag [] :nil ...)` and
`wrapped.tur` opens with `(defmodule wrapped (export main-tag) (defn
main-tag ...))`.  On main today this fails with
`defmodule must be the first form in the file`; after the fix it must
PASS with the expected stdout produced by `main-tag`.

Also keep a negative fixture asserting the intra-file rule still
fires: `tests/fixtures/errors/elab-defmodule-not-first/` whose
`input.tur` puts a defn above defmodule in the same file -- expected
exit code 1 with the same diagnostic.

### Phase D1 -- Track per-file form boundaries through pass 1

The elaborator already records each form's `span`, and `span` carries
the source filename.  Augment the pass that builds the `forms` array
so it can answer the question "for form index `i`, what is the index
range of the file that contributed it?"  Two cheap shapes work:

- **Side array.**  Allocate a parallel `uint32_t file_id[nforms]`,
  assign each contiguous run from the same source file the same id.
  (Spans already carry filenames; this is a one-pass O(n) labeling.)
- **File-boundary index.**  Allocate a `uint32_t file_starts[]` with
  the start index of each file-run.  Lookup is a binary search; same
  total cost.

Either shape is fine.  Side array is simpler and matches the existing
`stdlib_prefix` style (it's effectively `file_id[i] >= user_file_id`).

### Phase D2 -- Fix the check

Rewrite the loop at `elab_toplevel.c:896-912` to operate per file:

```c
/* Phase M0+: Validate defmodule position per source file.  The
 * first form of each (load ...)-spliced file may be a defmodule;
 * any later defmodule in the same file is an error.  defmodule in
 * file N does NOT see forms contributed by file N-1 as "earlier
 * forms" -- those live in a different file. */
uint32_t cur_file = UINT32_MAX;
uint32_t file_start_idx = 0;
for (uint32_t i = stdlib_prefix; i < nforms; i++) {
    if (file_id[i] != cur_file) {
        cur_file = file_id[i];
        file_start_idx = i;
    }
    Form *f = forms[i];
    if (f->tag != F_LIST || f->as.list.len == 0) continue;
    Form *head = f->as.list.items[0];
    if (head->tag != F_SYM || head->as.sym != e.sym_defmodule) continue;
    if (i != file_start_idx) {
        diag_emit(DIAG_ERROR, head->span,
                  "defmodule must be the first form in the file");
        diag_emit(DIAG_NOTE, forms[file_start_idx]->span,
                  "this form comes before defmodule in the same file; "
                  "move it inside the defmodule body or below it");
        rc = -1;
    }
    /* Continue scanning -- a later file with its own defmodule must
     * also be checked, and a second defmodule within the same file
     * is still an error (existing "one defmodule per file" rule). */
}
```

Two behavioural notes:

1. The original loop `break`s after the first defmodule.  The
   per-file version must continue, so it can flag a defmodule that
   is misplaced in a *later* loaded file.
2. The "one defmodule per file" rule already enforced by the M7 reset
   block (`e.has_defmodule = false` between files) keeps working;
   inside a single file two defmodules still trip the intra-file
   guard.

### Phase D3 -- Generalise the M7 reset

The `i < stdlib_prefix` reset block at line 934 only handles auto-
loaded stdlib.  Extend it to fire at every file boundary in the
user range, using the same `file_id` array:

```c
/* Each (load ...)-spliced file is its own defmodule scope. */
if (i + 1 < nforms && file_id[i + 1] != file_id[i]) {
    e.has_defmodule = false;
}
```

This keeps the elaborator's running "have I seen a defmodule in this
file?" state honest across user-side loads, not just auto-stdlib.

### Phase D4 -- Re-enable defmodule in tur/zlib

In the spices repo, restore the wrapping that M6's side-change
stripped:

```turmeric
(defmodule tur/zlib
  (export gzip-encode gzip-decode
          deflate-raw inflate-raw
          gzip-buf-data gzip-buf-len gzip-buf-free)

  ;; (defn gzip-encode ... ) etc., as today.
)
```

Verify the M6 fixture (`tests/fixtures/httpd-mw-compress/`) and the
spice's own `tests/tur/zlib/roundtrip_test.tur` both still compile.

### Phase D5 -- Docs

Single-paragraph update to whichever doc covers `defmodule`'s rules
(grep for "must be the first form" in `docs/guides/`) clarifying that
"the file" means the source file, and that `(load ...)`-spliced files
each get a fresh scope for the check.  No new doc -- just a
correction to whatever exists today.

---

## Out of scope

- **Promoting defmodule semantics across files.**  This plan does
  not propose making defmodule compose across files (e.g. two files
  contributing to the same module).  The fix is purely diagnostic
  scope -- defmodule still describes a single file's surface.
- **Auto-discovering defmodule from spice manifests.**  The
  manifest's `:exports` already lists the export surface; this fix
  leaves that orthogonal.

---

## Risk

- **Low.**  The change is local to the elaborator's top-level pass
  and reuses the per-file reasoning the M7 stdlib reset already
  proved out.  The intra-file rule -- the only behaviour users
  actually rely on for correctness feedback -- is preserved
  verbatim.
- **Test coverage caveat.**  No existing fixture exercises a user-
  side `(load ...)` of a defmodule-wrapped file after a non-
  defmodule file in the same unit.  Phase D0 closes that gap before
  any compiler edits land.

---

## Open questions

1. **D-OQ1.**  Should the intra-file diagnostic also gain a hint
   like *"if you meant to load a separate defmodule file, put it in
   its own file"*?  Cheap to add; helps users coming from the M6
   failure mode.
2. **D-OQ2.**  Is the "one defmodule per file" rule worth a
   dedicated error code in `docs/diagnostics/`?  Currently it shares
   the same message as the position check, which makes the two
   failure modes indistinguishable in CI logs.
