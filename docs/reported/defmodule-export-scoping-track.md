---
title: defmodule export scoping & project-mode build -- consolidated track
category: Bug Report
status: OPEN -- both reports active; affects every spice that ships through `(defmodule ... (export ...))`
severity: high (blocks the `tur-signal` spice surface and 5+ other spices' project-mode builds)
description: Consolidates two reports about how the `defmodule`/`export` boundary and the `tur build .` (project / separate-compilation) entry points lose information that single-file mode preserves. Together they make a non-trivial fraction of the spice ecosystem unbuildable in the shipping configuration: typed `^fat` parameters in exported defns lose their `(fn ...)` annotation across the boundary; project mode skips the stdlib prelude auto-load that single-file mode performs.
---

# defmodule export scoping & project-mode build -- consolidated track

Two independent but adjacent defects that both surface as "the spice builds
under `tur emit-c`/`tur check` but breaks the moment a real consumer or
project-mode build engages." Originals preserved verbatim under
[`archive/`](archive/).

## Defect A -- `^fat ... :(fn ...)` annotation lost across the export boundary

[`archive/defmodule-loses-fat-fn-type-annotation.md`](archive/defmodule-loses-fat-fn-type-annotation.md)

Inside `(defmodule mod (export name) ...)`, a defn declared with
`(defn name [^fat arg : (fn [param-types] ret-type)] ret) ...)` registers and
typechecks at the defn site, but any **caller** (cross-file importer, or even
a sibling defn inside the same defmodule that forward-references it) sees the
parameter typed as the zero-arg placeholder `(fn [] : ?)`. Concrete repro:

```
src/signal/core.tur:53        (defn sample [^fat sig : (fn [float] float) ...]
examples/01_constant_and_time.tur:12:22
  error [TUR-E0001]: function 'sample' arg 1: expected (fn [] : ?), got ptr<void>
```

Same shape lifted out of `(defmodule ...)` elaborates fine
(`tests/fixtures/pair-signals-typed/`, `vec-typed-fat-closure-readback/`).
The defect is in the export-registration / signature-projection step: the
inline `:(fn ...)` annotation is dropped when `^fat` is also present.

**Impact:** every `tur-signal` SF API (`sample`, `map-signal`, `pair-signals`,
oscillators/filters/shapers, `effects-chain`) is uncallable. There is no
workaround that keeps `(defmodule ... (export ...))` shape.

**Proposed fix:** ensure the `^fat`-with-annotation case round-trips through
the same path the non-`^fat` `name : type` params already use successfully
(`__chain-loop`'s plain `: int` params survive). A diagnostic dump of the
registered signature for `signal/core/sample` right after parse will pin the
projection step.

**Validation:** Phase 1 example runs and matches hand-computed output;
`test_core`/`test_osc`/`test_filter`/`test_shaper`/`test_envelope`/`test_compose`
all PASS; probe defn taking `^fat sig : (fn [float] float)` declared inside
any `(defmodule probe/m1 ...)` is callable from an importer.

## Defect B -- project-mode build skips stdlib prelude auto-load

[`archive/prelude-macros-not-importable-inside-defmodule.md`](archive/prelude-macros-not-importable-inside-defmodule.md)

`compile_to_c` (single-file mode, `src/main.c:589`) prepends ~20 stdlib files
to the user program and passes `stdlib_prefix` so `elab_toplevel.c:1131-1148`
promotes `tur/*` defmodules to globally visible "stdlib pre-module" status.
`compile_to_h` / `compile_to_implementation` (project mode, `src/main.c:865`,
`src/main.c:947`) skip the entire auto-load block and pass `stdlib_prefix = 0`.
Every `.tur` file is elaborated in a clean environment.

The misleading proximate diagnostic is `unknown function or operator: when`.

**Five independent sub-gaps compound this** (all enumerated in the original
under "Root cause(s)"):

| # | Gap | Affected | Fix sketch |
|---|---|---|---|
| 1 | Project mode skips stdlib auto-load | All project-mode builds | F1 -- factor `stdlib_files[]` loop into a shared helper, call from `compile_to_h`/`compile_to_implementation` |
| 2 | `stdlib/macros.tur` declares `tur/macros` but lives at `stdlib/macros.tur`; resolver can't reach it | Any explicit `(import ...)` workaround | F2 -- either move files to `stdlib/tur/<name>.tur`, or register under both names |
| 3 | `cons` has no user-callable defn (only `__tur_cons_of` compile-time form) | `c-dsl`, `glsl` | F3 -- add `(defn cons ...)` wrapping `__tur_cons_of` in `stdlib/list.tur` |
| 4 | `min`/`max` do not exist anywhere | `stats` | F4 -- add as macros in `stdlib/macros.tur` |
| 5 | `list.tur`/`math.tur`/`bits.tur` are bare files, not `defmodule`s | Selective `:refer` import | F5 -- wrap each in `(defmodule tur/<name> ...)` with explicit `(export ...)` |
| 6 | Separate-compilation cannot emit auto-loaded `defmodule` bodies (`elab_module.c:289` blocks `EX_DEFMODULE` when `separate_compilation`) | F1 for defn-shaped prelude | F6 -- defer; restrict F1 to macro-providing files initially |

**Affected spices** (current `../turmeric-spices`): `tourist`, `httpd`, `stats`,
`c-dsl`, `glsl`, `linalg` (knock-on). `bash tests/run.sh` is unaffected
because fixtures all go through single-file mode -- which is *why* this has
gone unnoticed.

**Smallest viable fix:** F1 (macros-only) + F4 (`min`/`max` as macros) closes
`tourist`, `httpd`, `stats`, `linalg` immediately and leaves `c-dsl`/`glsl`
blocked on F3.

**Validation:** add `tests/fixtures/defmodule-prelude-when/` and
`tests/fixtures/defmodule-prelude-min/` built through the manifest-driven
project-mode harness (not single-file emit-c, so the regression actually
exercises the bug); five spices build with `tur build .` from each root with
no `(load "stdlib/macros.tur")` workaround.

## How the two interact

Both defects bite the same workflow ("I am authoring a spice using
`defmodule` and shipping it to consumers"). Defect B breaks the build before
any code runs; Defect A breaks the runtime API the build was producing.
Fixing one without the other still leaves the spice ecosystem half-broken --
they should be sequenced together rather than tackled in isolation.

## Cross-references

- `[[tur-signal-rebuild-plan]]` -- gated on Defect A clearing.
- `docs/upcoming/spices-v0.18-typing-migration-plan.md` -- gated on Defect B.
- `tests/fixtures/pair-signals-typed/`,
  `tests/fixtures/vec-typed-fat-closure-readback/` -- non-defmodule
  baselines that demonstrate the typed `^fat` shape works outside a
  `(defmodule ...)` wrapper.
