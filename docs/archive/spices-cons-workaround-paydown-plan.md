---
title: spices-side paydown of cons / prelude workarounds (Defect B follow-up)
category: Plan
status: proposed
severity: low (cleanup; spices already build via local workarounds)
gates: none -- F1+F3+F4+F6 of `defmodule-export-scoping-track` already landed in compiler
---

# Spices-side paydown of `cons` / prelude workarounds

Compiler-side fixes for Defect B of
[`docs/reported/defmodule-export-scoping-track.md`](../reported/defmodule-export-scoping-track.md)
landed in two waves (F1+F4 on 2026-06-08, F3+F6 on 2026-06-10).  `tur build .`
now resolves `when`/`cond`/`for`/`min`/`max` inside a `defmodule` body and a
bare `(cons h t)` lowers to the per-TU `cons` builtin in both single-file and
project mode.

Several spices in `../turmeric-spices` evolved local workarounds before those
fixes landed.  The workarounds are still load-bearing only because they are
*present* -- the underlying defect they routed around is gone.  This plan
removes them so the spice tree exercises the real prelude/builtin path.

## Scope

Targets `../turmeric-spices` only.  No turmeric repo changes.

In-scope workarounds:

1. **`c-dsl/src/c-dsl/builtins.tur`** -- `__cons1` helper (the one-element
   cons-list constructor that papered over the missing user-callable `cons`).
   All four call sites (`spices/c-dsl/src/c-dsl/builtins.tur:169,178,187,196`)
   should call the stdlib `cons` directly.
2. **Any `(load "stdlib/macros.tur")` workaround** -- a grep across
   `spices/**/*.tur` currently finds none (good), but this plan double-checks
   during execution in case one is hiding in a non-canonical path.

Explicitly **out of scope** (these are *not* workarounds, they are the
idiomatic walk-a-cons-list-in-inline-C pattern and should stay):

- `struct __cdsl_cons { int64_t value; int64_t next; }` walker declarations
  in `c-dsl/src/c-dsl/{codegen,fns,typedef}.tur`.  These mirror the
  `__tur_cons_cell` walker pattern documented in `CLAUDE.md` (see "Cons-list
  manipulation in `#{Unsafe}` code") and are how Turmeric inline-C reads a
  cons list passed in via `& rest`.  The new `cons` builtin only changes how
  cells are *constructed*; the in-memory layout is unchanged
  (`{head, tail}` 16-byte cell), so the walkers keep working as-is.
- Vendored copies under `spices/*/spices/.../c-dsl/...` (vendored deps in
  `plot`, `test`, `notebook`, `plutovg`).  These are content-addressed
  snapshots; bumping them is a separate dep-roll PR after the c-dsl source
  change ships a tag.

## F2 / F5 (selective `:refer` import) -- still deferred

The compiler-side report lists F2 (`tur/macros` filesystem-path mismatch) and
F5 (wrap `list.tur`/`math.tur`/`bits.tur` in `defmodule`s) as open but
non-blocking.  No currently-broken spice relies on selective
`(import tur/math :refer [...])`, so this plan does **not** touch them.  If a
spice later wants `:refer`, the cheapest mitigation is a bare
`(import tur/math)` until F5 lands; that is a future plan, not this one.

## Steps

### Step 1 -- remove `__cons1` from `c-dsl`

1. `spices/c-dsl/src/c-dsl/builtins.tur:22-29`: delete the docstring + `defn`
   for `__cons1`.
2. `spices/c-dsl/src/c-dsl/builtins.tur:169,178,187,196`: rewrite each
   `(__cons1 X)` as `(cons X 0)`.  (Stdlib `cons` takes `head` and a tail
   list; `0` is nil.)
3. Confirm `bash tests/run.sh` and `tur build .` from `spices/c-dsl/` still
   pass.  The c-dsl test suite covers the `c-include-sys` codepath that the
   four call sites feed.

### Step 2 -- audit for stray `(load "stdlib/macros.tur")`

```sh
cd ../turmeric-spices
grep -rln 'load "stdlib/macros' spices/ | grep -v '/spices/.*/spices/'
```

Strip any hit; project mode now auto-loads the macro prelude via
`load_project_prelude`.  Vendored copies (paths matching `spices/*/spices/`)
are not edited here -- they roll forward with their next vendored snapshot.

### Step 3 -- verify each previously-affected spice builds clean

Per the compiler-side report, the affected list is
`tourist`, `httpd`, `stats`, `c-dsl`, `glsl`, `linalg`.  From each root:

```sh
cd ../turmeric-spices/spices/<name>
tur build .
```

Each must exit 0 with no `(load "stdlib/macros.tur")` workaround and (for
`c-dsl`) no `__cons1` definition.

### Step 4 -- glsl audit

`glsl` is on the affected list but was not searched in detail while writing
this plan.  Grep its tree for an analogous `__cons1`-shaped workaround
before the verification build:

```sh
grep -rn 'cons-list\|cons1\|__.*_cons' ../turmeric-spices/spices/glsl/src/
```

If a similar helper exists, fold it into Step 1's PR; if not, the
verification build in Step 3 is sufficient.

## Validation

- `tur build .` exits 0 in `tourist`, `httpd`, `stats`, `c-dsl`, `glsl`,
  `linalg` after the diff.
- `bash tests/run.sh` from the spices repo root (if present) is green.
- A `git grep '__cons1'` in `spices/c-dsl/src/` returns no hits.
- A `git grep 'load "stdlib/macros'` across `spices/` (excluding vendored
  subtrees) returns no hits.

## Deliverable

One PR in `rjungemann/turmeric-spices`, titled e.g. "Remove cons / prelude
workarounds now that compiler ships them" -- bundles Steps 1, 2, and any
finding from Step 4.  No turmeric-repo changes.

## Cross-references

- [`docs/reported/defmodule-export-scoping-track.md`](../reported/defmodule-export-scoping-track.md)
  -- Defect B + the F1/F3/F4/F6 fix descriptions this plan depends on.
- `CLAUDE.md` -- "Cons-list manipulation in `#{Unsafe}` code" justifies
  keeping the `__cdsl_cons` walker structs in place.
