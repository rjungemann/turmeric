---
title: Multi-file split path drops ADT monomorph typedefs from the header
severity: LOW. Does not affect the `tests/run.sh` gate (suite builds single-file).
status: RESOLVED 2026-07-02. emit_header now runs the full body scan +
per-ctor inline-by-value field pre-registration before flushing ADT typedefs.
---

# Multi-file split path drops ADT monomorph typedefs from the header

## Resolution (2026-07-02)

Fixed in `src/compiler/emit_header` (`src/compiler/emit_module.c`), taking the
report's second fix direction ("have `emit_header` run the same full type-scan
`emit_program` does").  The header is the sole emitter of the concrete ADT-app
typedefs for the separate-compilation path -- the `.c` `#include`s it and never
flushes its own set -- but its registration previously only walked *exported
signatures*.  Before the flush, `emit_header` now:

1. runs `scan_adt_apps_in_expr` over every flattened item (the same full body
   walk `emit_program` does), and
2. mirrors `emit_program`'s per-ctor pre-registration of inline-by-value
   parametric fields (`type_c_name` on each `TY_APP` field that
   `adt_field_is_inline_byval`), which is how the `Endo__int` / `Schema__int`
   concrete monomorphs and the `Option__struct` placeholder they carry get
   registered.

`./build/tur build tests/fixtures/defstruct-fn-field-single-arg` (the repro,
directory-descent without a manifest -> split path) now compiles and the
resulting binary prints `42`.

The `tests/run.sh` gate builds every fixture single-file, so it does not
exercise the split path; this fix is verified by the manual directory build
above rather than a run.sh fixture.

## Summary

`tur build <dir>` on a fixture directory **without** a `build.tur` manifest goes
through the multi-file separate-compilation path (`emit_header` -> `input.h`,
`emit_program`/`emit_implementation` -> `input.c`).  That path collects a
**smaller** set of ADT monomorph typedefs than the single-file `emit-c` /
`emit_program` path, so some monomorphs are referenced but never declared.

## Repro

```sh
./build/tur build tests/fixtures/defstruct-fn-field-single-arg
# input.c: unknown type name 'tur_adt_Endo__int'
# input.c: implicit declaration of function 'ctor_Option__struct'
```

Diff of emitted ADT typedefs (single-file vs split):

```
emit-c (single file)         split (input.h + input.c)
  tur_adt_Endo__int            (missing)
  tur_adt_Schema__int          (missing)
  tur_adt_Option__struct       (missing)
  tur_adt_Option__Zipper__struct (missing)
```

`Endo__int` and `Schema__int` are **fully concrete** monomorphs (no tyvar /
placeholder), so this is not the mangler bug -- it is a monomorph-collection gap
specific to `emit_header`.

## Why it does not affect the suite

`tests/run.sh` builds each fixture **single-file** (`tur ... emit-c` for the
snapshot compare and `tur build <input.tur>` for the run), which uses
`emit_program` -- the same complete monomorph collection as `emit-c`.  The split
path is only reached by `tur build <dir>` (directory descent) and project mode.
The full by-value suite is green (1874/0) with the split path in this state.

## Root cause (direction, not yet pinpointed)

`emit_header` (`src/compiler/emit_module.c:9563`) resets the ADT-app registries
(`type_codegen_reset_adt_apps`) and re-scans, but its scan appears to miss
monomorphs that only become reachable through a lowered `defstruct`'s fields
(`Endo` is a lowered record ADT with an `fn` field; `Option__struct` is the
placeholder Option carried by that field).  The single-file `emit_program` scan
picks them up.  Likely a difference in which expressions/types each path feeds to
the ADT-app collector, or an ordering issue (header emitted before the later
monomorphs are discovered).

## Fix directions

- Share one monomorph-collection pass between `emit_program` and `emit_header`
  so the split header sees the identical set, or
- Have `emit_header` run the same full type-scan `emit_program` does before
  emitting ADT typedefs.

Low priority: no real consumer hits the `tur build <dir>`-without-manifest path
for these fixtures today, and the suite gate is unaffected.
