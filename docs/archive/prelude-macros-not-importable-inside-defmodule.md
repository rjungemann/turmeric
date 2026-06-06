---
title: Prelude macros (`when`, `cond`, `for`, `cons`, `min`) are unavailable inside `defmodule` under project-mode (`tur build .`) compilation
category: Reported
severity: medium
description: Five spices (`tourist`, `httpd`, `c-dsl`, `glsl`, `stats`) and one with a knock-on parser failure (`linalg`) fail to build with `tur build .` because the project-mode compiler entry points (`compile_to_h` / `compile_to_implementation` in `src/main.c`) do not auto-load the stdlib prelude that single-file mode (`compile_to_c`) loads. Several distinct sub-issues compound the problem: the prelude `defmodule`s use a `tur/<name>` declared name while living at `stdlib/<name>.tur` on disk (so `(import ...)` can't reach them either), `cons` has no user-callable defn anywhere in the tree, `min`/`max` are not defined in stdlib at all, and `list.tur` / `math.tur` / `bits.tur` are bare files rather than `defmodule`s. The original report (this file, pre-rewrite) treated only the bare-file aspect; the rewrite below captures everything turned up during investigation.
---

# Prelude macros not importable inside `defmodule` (revised)

## Summary

`tur build .` (project / separate-compilation mode) does **not** auto-load any
of the stdlib prelude files (`macros.tur`, `safe.tur`, `contract.tur`, ...) that
single-file mode (`tur emit-c <file>`, `tur build <file>`) auto-loads. As a
result, a spice that uses `when`, `cond`, `for`, `min`, `cons`, etc. inside a
`(defmodule ...)` body builds fine via `emit-c` but fails the moment `tur
build .` walks the manifest. The proximate diagnostic is the misleading:

```
src/tourist/dsl.tur:164: unknown function or operator: when
```

There are four independent gaps; closing the report cleanly requires fixing
all of them.

## Affected spices

Reproducible against the current `../turmeric-spices` checkout:

| Spice | First failing site | Missing name | Kind |
|---|---|---|---|
| `tourist` | `src/tourist/dsl.tur:164` | `when` | prelude macro |
| `httpd` | `src/httpd/server.tur:488` | `when` | prelude macro |
| `stats` | `src/stats/test.tur:123` | `min` | not defined anywhere |
| `c-dsl` | `src/c-dsl/builtins.tur:159` | `cons` | not user-callable |
| `glsl` | `src/glsl/stdlib.tur:39` | `cons` | not user-callable |
| `linalg` | (knock-on) `src/linalg/decomp.tur` parser failure | `when` | prelude macro |

`bash tests/run.sh` is unaffected because the fixtures all go through
single-file mode.

## Minimal repro

```turmeric
;; demo/src/demo/foo.tur
(defmodule demo/foo
  (export f)
  (defn f [x : int] : int
    (when (> x 0) x)))

;; demo/build.tur
(defpackage demo :version "0.1.0" :exports [demo/foo])
```

```sh
$ tur emit-c demo/src/demo/foo.tur   # succeeds (single-file mode auto-loads macros.tur)
$ tur build demo                     # FAILS: "unknown function or operator: when"
```

## Observed vs expected

- **Observed**:
  - `tur emit-c <file>` auto-loads ~20 stdlib files (see `stdlib_files[]` in
    `src/main.c:645`), so `when`/`cond`/`for` and other prelude names are
    globally visible.
  - `tur build .` (project mode) routes through `compile_to_h` /
    `compile_to_implementation`, which **do not** auto-load anything. Each
    `.tur` file is elaborated in a clean environment, with `stdlib_prefix = 0`
    and no `tur/` promotion logic running (`src/compiler/elab_toplevel.c:1131`).
  - The `(load "stdlib/macros.tur")` workaround the previous version of this
    report suggested also **does not work** in project mode (verified: same
    "unknown function or operator: when" error).
- **Expected**: project mode and single-file mode should agree about which
  names are visible at the top level of a user file. Whatever channel makes
  `when` available inside a single-file `(defmodule ...)` should make it
  available inside a project-mode `(defmodule ...)` too.

## Root cause(s)

### 1. Project mode skips the stdlib auto-load loop

`compile_to_c` (`src/main.c:589`) builds `all_stdlib_forms` from the
`stdlib_files[]` list and prepends them to the user program. It then passes
`stdlib_prefix = total_stdlib_forms` to `run_core_passes`, which is what
`elab_toplevel.c` keys off when promoting `tur/*` module exports to globally
visible "stdlib pre-module" status (`elab_toplevel.c:1131-1148`).

`compile_to_h` and `compile_to_implementation` (`src/main.c:865`, `src/main.c:947`)
skip the entire auto-load block and pass `stdlib_prefix = 0` (default).
No promotion runs, so prelude macros are simply not in scope.

### 2. Prelude `defmodule` name does not match its filesystem path

`stdlib/macros.tur` declares `(defmodule tur/macros ...)`, but the file lives
at `stdlib/macros.tur` -- not `stdlib/tur/macros.tur`. The module resolver
maps `(import tur/macros ...)` to `<root>/tur/macros.tur`, which doesn't exist.
`(import macros ...)` resolves the file by basename but then reports
`symbol 'when' is not exported from module 'macros'` because the file's
declared module name is `tur/macros`, not `macros`. As a result, **no
`(import ...)` form can reach the prelude from a user file** -- neither
qualified nor unqualified.

Same issue applies to `stdlib/safe.tur` (declares `tur/safe`),
`stdlib/contract.tur` (declares `tur/contract`), and the `tur/macros`
auto-load comment in `src/compiler/elab_toplevel.c:1131` ("modules under the
`tur/` namespace as implicitly imported") only fires because single-file mode
loads them by file path before the resolver is involved.

### 3. `cons` has no user-callable defn

`cons` is only registered as a compile-time form in `elab_macros.c:328` (for
use inside macro bodies) and as the variadic-rest builder
`__tur_cons_of` in `emit_module.c:2135`. There is no `(defn cons ...)` in
stdlib at all, and `(cons 1 0)` in a user defn body fails with
`unknown function or operator: cons` even in single-file mode. The c-dsl and
glsl spices treat `cons` as a runtime list constructor, but that
functionality has never been exposed -- the `tcons` defn in
`stdlib/list.tur:30` is similar but typed and not aliased to `cons`.

### 4. `min` / `max` do not exist

`stdlib/math.tur` contains `sqrt`, `fabs`, `floor`, `ceil`, `pow`,
`int->float`, `float->int`, `printf-float6` -- and nothing else. There is no
`min` or `max` defn or macro anywhere in the tree. The stats spice's
`stats/test.tur:123` reaches for `(min ...)` and gets a clean "unknown
function or operator" because the name truly does not exist.

### 5. `list.tur`, `math.tur`, `bits.tur` are bare files

These are top-level `(defn ...)` files without a `(defmodule ...)` wrapper or
`(export ...)` list, so even after the auto-load gap is fixed, they cannot be
selectively imported via `(import ... :refer [...])`. `list.tur` is in the
single-file auto-load list (`src/main.c:699`); `math.tur` and `bits.tur` are
not.

### 6. Separate-compilation will not emit auto-loaded `defmodule` bodies

`elab_module.c:289` blocks `elab_register_file_def` for `EX_DEFMODULE` when
`e->separate_compilation` is true. So even if `compile_to_h` were taught to
auto-load `tur/math`, a user call to `(min ...)` would reference
`tur_math__min` from the user module's `.c` while the body of `tur/math`
would never be emitted anywhere, producing an unresolved symbol at link time.
Macros are unaffected (compile-time-only); the gap only bites for stdlib
**defns** brought in by auto-load under separate compilation.

## Proposed fix directions

These can land independently; numbered so the next session can sequence them.

### F1. Teach project mode to auto-load the prelude

Refactor the `stdlib_files[]` load loop out of `compile_to_c` into a shared
helper and call it from `compile_to_h` and `compile_to_implementation` too.
The `tur/` promotion logic in `elab_toplevel.c:1131` then runs in project
mode and makes prelude macros globally visible.

For separate compilation, restrict the auto-load to **macro-providing**
files only (initially `stdlib/macros.tur`) to sidestep F6 below. Defn-only
prelude files (`safe.tur`, `math.tur`, ...) stay opt-in.

### F2. Fix the module-name / filesystem-path mismatch

Two options:
1. Move `stdlib/macros.tur` -> `stdlib/tur/macros.tur` (and similarly for
   every `tur/<name>` defmodule). Most discoverable, but ripples through the
   auto-load array, every `tur_stdlib_path` call site, and the gendocs
   `--out` paths.
2. Register every loaded defmodule under **both** its declared name and its
   filename-derived alias. Smaller blast radius; introduces an alias-table
   concept the rest of the resolver currently lacks.

### F3. Add a user-callable `cons` defn

Define `(defn cons [h : int t : int] : int ...)` in `stdlib/list.tur` (which
is already in the single-file auto-load list). The body can wrap
`__tur_cons_of` via inline-C. Wrap `list.tur` in `(defmodule tur/list ...)`
with `cons` exported.

### F4. Add `min` and `max`

Add them as macros in `stdlib/macros.tur` so they work polymorphically across
any type with `<=` / `>=`:

```turmeric
(defmacro min [a b] (if (<= a b) a b))
(defmacro max [a b] (if (>= a b) a b))
```

Macros sidestep F6 (compile-time-only, no codegen needed). This also matches
how `cond` / `when` / `unless` already live in `stdlib/macros.tur`.

### F5. Wrap `math.tur` / `bits.tur` in `defmodule`

Once F1 + F2 land, convert these to `(defmodule tur/math ...)` and
`(defmodule tur/bits ...)` with explicit `(export ...)` lists so they can
also be selectively imported via `(import tur/math :refer [...])`.

### F6. Separate-compilation strategy for stdlib defns

The hardest part, deferrable. Options:
- Mark every auto-loaded defmodule's defns as **always-inlined** (`static inline`)
  so each user `.c` carries its own copy. Cheap, no library, but bloats every
  `.o`.
- Pre-compile the prelude defmodules to `libtur_stdlib.a` and have
  `tur build` link against it automatically. More work, cleaner output.
- Keep auto-load limited to macro-only prelude files in F1; require explicit
  `(import tur/math :refer [...])` for defn-shaped prelude (and let the
  existing per-module compilation path emit the body for the explicitly
  imported module).

The simplest path forward: F1 (macros-only) + F4 (min/max as macros) closes
`tourist`, `httpd`, `stats`, `linalg` immediately and leaves c-dsl/glsl
blocked on F3.

## Validation plan

After F1 + F4 land:

1. Build all five reported spices with `tur build .` from each spice root
   and confirm no `(load "stdlib/macros.tur")` workaround appears in source.
2. Add a fixture at `tests/fixtures/defmodule-prelude-when/` that uses
   `(when ...)` inside a `(defmodule ...)` body and is built via the
   project-mode test harness (an existing manifest-driven fixture path, not
   single-file emit-c, so the regression check actually exercises the bug).
3. Add a fixture at `tests/fixtures/defmodule-prelude-min/` for `min` / `max`.
4. Run `bash tests/run.sh 2>&1 | grep "^FAIL"` and confirm zero failures
   (regenerating affected codegen snapshots per the CLAUDE.md sweep if the
   shared prelude changes their output).

For F3 (c-dsl / glsl) the fixture would assert `(cons 1 0)` returns a value
walkable by `head` / `tail`.

## History

This report previously diagnosed the problem as `stdlib/macros.tur` being a
bare file. That was incorrect -- `macros.tur` is already wrapped in
`(defmodule tur/macros ...)`. The real defect is in the project-mode build
entry points (and the secondary issues above). Rewritten 2026-06-05.
