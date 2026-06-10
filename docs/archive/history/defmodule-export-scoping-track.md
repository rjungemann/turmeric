---
title: defmodule export scoping & project-mode build -- consolidated track
category: Bug Report
status: Defect A FIXED 2026-06-09 (`param_poly_types` now populated for `^fat` TY_FN params); Defect B FIXED 2026-06-10 (F1+F4 prelude macros / min-max landed earlier; F3+F6 now make the user-callable `cons` runtime constructor resolve in project mode). F2/F5 (selective `:refer` of math/bits) remain as non-blocking enhancements.
severity: high (blocks the `tur-signal` spice surface and 5+ other spices' project-mode builds)
description: Consolidates two reports about how the `defmodule`/`export` boundary and the `tur build .` (project / separate-compilation) entry points lose information that single-file mode preserves. Together they make a non-trivial fraction of the spice ecosystem unbuildable in the shipping configuration: typed `^fat` parameters in exported defns lose their `(fn ...)` annotation across the boundary; project mode skips the stdlib prelude auto-load that single-file mode performs.
---

# defmodule export scoping & project-mode build -- consolidated track

Two independent but adjacent defects that both surface as "the spice builds
under `tur emit-c`/`tur check` but breaks the moment a real consumer or
project-mode build engages." Originals preserved verbatim under
[`../archive/`](../archive/).

## Defect A -- `^fat ... :(fn ...)` annotation lost across the export boundary

[`../archive/defmodule-loses-fat-fn-type-annotation.md`](../archive/defmodule-loses-fat-fn-type-annotation.md)

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

[`../archive/prelude-macros-not-importable-inside-defmodule.md`](../archive/prelude-macros-not-importable-inside-defmodule.md)

`compile_to_c` (single-file mode, `src/main.c:589`) prepends ~20 stdlib files
to the user program and passes `stdlib_prefix` so `elab_toplevel.c:1131-1148`
promotes `tur/*` defmodules to globally visible "stdlib pre-module" status.
`compile_to_h` / `compile_to_implementation` (project mode, `src/main.c:865`,
`src/main.c:947`) skip the entire auto-load block and pass `stdlib_prefix = 0`.
Every `.tur` file is elaborated in a clean environment.

The misleading proximate diagnostic is `unknown function or operator: when`.

**Five independent sub-gaps compound this** (all enumerated in the original
under "Root cause(s)"):

| # | Gap | Affected | Fix sketch | Status |
|---|---|---|---|---|
| 1 | Project mode skips stdlib auto-load | All project-mode builds | F1 -- factor `stdlib_files[]` loop into a shared helper, call from `compile_to_h`/`compile_to_implementation` | DONE (`load_project_prelude`, macros-only) |
| 2 | `stdlib/macros.tur` declares `tur/macros` but lives at `stdlib/macros.tur`; resolver can't reach it | Any explicit `(import ...)` workaround | F2 -- either move files to `stdlib/tur/<name>.tur`, or register under both names | OPEN (non-blocking) |
| 3 | `cons` has no user-callable defn (only `__tur_cons_of` compile-time form) | `c-dsl`, `glsl` | F3 -- expose a user-callable `cons` | DONE (registered as a `BS_FUNC_CALL` builtin -> `cons(h,t)` helper, not a stdlib defn) |
| 4 | `min`/`max` do not exist anywhere | `stats` | F4 -- add as macros in `stdlib/macros.tur` | DONE |
| 5 | `list.tur`/`math.tur`/`bits.tur` are bare files, not `defmodule`s | Selective `:refer` import | F5 -- wrap each in `(defmodule tur/<name> ...)` with explicit `(export ...)` | OPEN (non-blocking) |
| 6 | Separate-compilation cannot emit auto-loaded `defmodule` bodies (`elab_module.c:317` blocks `EX_DEFMODULE` when `separate_compilation`) | defn-shaped prelude (`cons`) in project mode | F6 -- sidestepped: `cons` is a builtin whose C helper is emitted per-TU, so it needs no auto-loaded defmodule body | DONE (for `cons`) |

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

## Defect A fix (2026-06-09)

`src/compiler/elab_fns.c` (the `^fat` + `TY_FN` parameter-annotation branch
around line ~1144): `param_poly_types[n_params - 1]` is now populated
unconditionally for the plain (non-carrier) function-type annotation case,
not just under `-Xlinear`.  This was the missing assignment that left
`arg_full_types` NULL on the forward-declaration binding (the HRT5 early-update
path at line ~1900), so cross-file importers and same-defmodule
forward-references saw the zero-arg placeholder `(fn [] : ?)` instead of the
real `(fn [float] float)`.

Regression fixture: `tests/fixtures/defmodule-fat-fn-param-export/`.

## Defect B fix (2026-06-10)

Closed in two waves:

**F1 + F4 (earlier).** `load_project_prelude` (`src/main.c`) injects the
macro-only prelude (`stdlib/macros.tur`) before the user forms in both
`compile_to_h` and `compile_to_implementation`, so `when`/`cond`/`for`/`min`/`max`
resolve inside a `defmodule` body under `tur build .`.  `min`/`max` were added as
macros in `stdlib/macros.tur` (compile-time only, no separate-compilation
emission gap).  This unblocked `tourist`, `httpd`, `stats`, `linalg`.

**F3 + F6 (this change).** The remaining `c-dsl`/`glsl` blocker was the
user-callable `cons` runtime list constructor.  Rather than a stdlib `defn`
(which project mode would never auto-load, and which would hit the
separate-compilation defmodule-emission gap, F6), `cons` is now a **builtin**:

- `src/compiler/builtins.c` -- new table entry
  `{ "cons", 2, 2, :int, :int, BS_FUNC_CALL, "cons" }`.  `(cons h t)` lowers to a
  C call `cons(h, t)` and types as `:int`.
- `src/compiler/emit_module.c` -- `emit_cons_helper` writes a guarded
  `static int64_t cons(int64_t, int64_t)` that allocates a `{head,tail}` cell
  (the same layout as `__tur_cons_of` / `tcons`, so cells interoperate with the
  `list-head`/`list-tail` walkers in `stdlib/list.tur`).  It is emitted into the
  preamble of `emit_program` (single-file) **and** `emit_implementation`
  (project-mode `.c`), so the helper is present in every TU that references
  `cons`, in both compilation modes -- no auto-loaded defmodule body required.
- `src/compiler/elab_call.c` -- sets the new `g_uses_cons` global when the
  `cons` builtin resolves, so the helper is gated (non-`cons` programs are
  unchanged -- zero codegen-snapshot churn).
- The compile-time `cons` form in `src/compiler/elab_macros.c` is unaffected: it
  fires only during macro expansion (`ct_eval_builtin`), a distinct phase from
  runtime call resolution, so the `dot` macro and friends still work.

Regression coverage: `tests/fixtures/cons-builtin-list/` (single-file, exit 33)
and `build-project-prelude-cons` in `tests/run-build-project.sh` (project mode,
exit 33), alongside the existing `build-project-prelude-when` /
`build-project-prelude-minmax`.

F2 and F5 (the `tur/<name>` filesystem-path mismatch and wrapping
`list`/`math`/`bits` in `defmodule`s) remain open but block no reported spice
build -- they only matter for *selective* `(import tur/math :refer [...])`, which
no affected spice relies on.

## Follow-ups discovered while fixing Defect A

The fix surfaced two adjacent bugs that are *not* Defect A but are next door
to it; both have their own reports:

- `[[pap-defmodule-fat-fn-too-many-args]]` -- a sibling forward-reference
  inside a defmodule that partially applies a `^fat` fn-typed sibling
  synthesises a PAP wrapper with one extra arg vs the callee's C signature
  (`cc` rejects).  The Defect-A fix is what made this PAP path *try* to
  compile; before, the forward-ref errored out at elab.
- `[[sf-compose-typed-arrow-prints-garbage-floats]]` -- pre-existing
  (not regression): `stdlib/arrow.tur`'s typed `>>>` over float SFs prints
  uninitialised float bytes instead of the composed result.  Same shape as
  the historical XMM/int64 register-class mismatch bugs.

## Cross-references

- `[[tur-signal-rebuild-plan]]` -- Defect A is now cleared; ungated.
- `docs/upcoming/spices-v0.18-typing-migration-plan.md` -- gated on Defect B.
- `tests/fixtures/pair-signals-typed/`,
  `tests/fixtures/vec-typed-fat-closure-readback/` -- non-defmodule
  baselines that demonstrate the typed `^fat` shape works outside a
  `(defmodule ...)` wrapper.
- `tests/fixtures/defmodule-fat-fn-param-export/` -- new regression fixture
  for Defect A.
