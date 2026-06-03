# SourceFile Uninitialized `xform_map` Crash Fix

> **Status:** Done
> **Last Updated:** 2026-06-03
> **Type:** compiler / diagnostics memory safety

---

## Overview

The `tur_eval_sandbox` ctest target crashed with a SEGV (under
ASan/UBSan) whenever a diagnostic snippet had to be rendered for source
that was evaluated from an in-memory string (the `turi_eval` / REPL path)
rather than from a file. The crash showed up as:

```
src/compiler/reader.c:3034: runtime error: member access within misaligned
  address 0xbebebebebebebebe for type 'const struct SweetMap', which
  requires 8 byte alignment
AddressSanitizer: SEGV ... in sweet_map_translate_offset
  #1 render_snippet_ex   src/compiler/diag.c:1479
  #2 render_snippet      src/compiler/diag.c:1673
  ...
  #5 elab_call           src/compiler/elab_call.c:1352   (unknown function)
  #8 turi_eval_impl      src/turi/eval.c
```

`0xbebebebebebebebe` is the byte pattern ASan writes into freshly
`malloc`'d (but not yet initialized) memory, which is the tell that the
fault was a **read of uninitialized memory**, not a use-after-free.

## Root cause

`SourceFile` (see `src/compiler/diag.h`) carries optional sweet-expression
transform fields:

```c
const char     *orig_src;
size_t          orig_len;
const SweetMap *xform_map;
```

When `xform_map != NULL`, `render_snippet_ex` (`diag.c:1475`) treats the
file as sweet-exp-transformed and calls
`sweet_map_translate_offset(f->xform_map, ...)` to map transformed offsets
back to the user's original source.

Several `SourceFile` allocation sites only set the five "core" fields
(`path`, `src`, `len`, `file_id`, `reader_type`) and left
`orig_src` / `orig_len` / `xform_map` holding whatever uninitialized
memory the arena (or stack) handed back. Most of the time that garbage
happened to be benign, but on the string-eval path a diagnostic
(`unknown function or operator 'println-int'`) triggered snippet
rendering, `xform_map` read as a non-NULL garbage pointer, and
`sweet_map_translate_offset` dereferenced it.

The sites that allocate via `arena_alloc` are not zeroed (the arena does
not clear memory), so partial initialization leaves these fields
indeterminate.

## Fix

Zero-initialize the whole `SourceFile` before setting the core fields, so
`xform_map` / `orig_src` start as `NULL` and the diagnostic renderer
correctly takes the non-transformed path. Three sites were missing this:

| File | Site | Fix |
| --- | --- | --- |
| `src/turi/eval.c` | `turi_eval_impl` eval-source registration | `memset(sfile, 0, sizeof(*sfile))` |
| `src/compiler/reader.c` | `read_cblock`-adjacent preload `sf` | `*sf = (SourceFile){0}` |
| `src/turi/repl.c` | `:type` command stack `sfile` | `SourceFile sfile = {0}` |

The remaining allocation sites (`main.c`, `elab_module.c`,
`elab_toplevel.c`) already zero-initialized with `(SourceFile){0}`, and
`reader.c`'s sweet-exp `xfile` copies an existing fully-initialized
`SourceFile` and then sets the transform fields explicitly -- those were
already correct.

## Verification

- `ctest -R tur_eval_sandbox` passes (previously SEGV).
- Full `ctest` suite: 44/44 pass.
- `bash tests/run.sh`: 1297 passed, 0 failed.

## Note: not a "strange workaround"

This is a straight memory-safety fix, not a test-side workaround. It is
documented here only because it was a non-obvious latent bug (benign until
a diagnostic forced the transform path) that future `SourceFile`
allocation sites must avoid: **always zero a freshly arena-allocated
`SourceFile` before populating it**, because the optional sweet-exp
transform fields are consulted by the diagnostic renderer.
