---
title: Resolved / paper-trail reports -- consolidated index
category: Bug Report
status: RESOLVED -- no further action; preserved for traceability per CLAUDE.md "Reporting Bugs" rule
description: Index of bug reports that landed fixes, were paper-trail removals, or were rediagnosed as source bugs in a sibling repo. Each entry links to the verbatim original under `../archive/` and records the resolution shape so the next time the area is touched the fix isn't reinvented or the report mistaken for an open task.
---

# Resolved / paper-trail reports

These reports are closed. Originals preserved verbatim under
[`../archive/`](../archive/). Listed here so they remain greppable and link-walkable
without cluttering the active `docs/reported/` index with completed work.

## Compiler / language fixes (landed)

### `load` inside a `defmodule` body silently lost loaded names (FIXED)

[`../archive/load-inside-defmodule-silently-loses-names.md`](../archive/load-inside-defmodule-silently-loses-names.md)

A `(load "path")` placed inside a `defmodule` body was first silently
accepted as a no-op (loaded names never injected; use-site errored with
"unknown function or operator"), then -- as a stopgap -- made a hard error
(#303). **Fix (Option A):** the load-expansion preprocessor
`load_expand_forms` in `src/compiler/elab_toplevel.c` now descends into
`(defmodule ...)` forms, splicing a body-level load's forms into the
module scope exactly as a top-level load splices into the compilation
unit (sharing the compilation-global visited set, so each path expands at
most once). A load in genuine expression position (`defn`/`let`/`do`
body) remains a hard error -- `elab_load` in
`src/compiler/elab_module.c` carries the updated diagnostic. Regression
fixtures: `tests/fixtures/load-inside-defmodule-injects-names/` (positive)
and `tests/fixtures/errors/load-inside-defn/` (negative); the obsolete
`errors/load-inside-defmodule` negative fixture was removed. The `^fat`
let-binding `>>>` segfault seen while validating is the orthogonal
`fat-shim-void-ptr-calls-bare-not-fat.md`, not a load-scope bug.

### captureless closure not boxed at a `ptr<void>` `^fat` boundary (FIXED)

[`../archive/captureless-closure-not-boxed-at-fat-ptr-void-boundary.md`](../archive/captureless-closure-not-boxed-at-fat-ptr-void-boundary.md)

A closure that captures nothing (e.g. the SF returned by a nullary `(invert)`)
is codegen'd as a **bare C function pointer**, not a `{ thunk, env }` fat box.
When such a value was ascribed to the carrier with `(:: <captureless-fn>
:ptr<void>)` and passed to a `^fat` parameter, `elab_call`'s `^fat` handler saw
a `TY_PTR_VOID` argument, assumed it was already a fat box, and passed it
through unshimmed. The consumer then fat-dispatched the code address as slot 0
and **segfaulted**. This was the captureless sibling of
`fat-shim-void-ptr-calls-bare-not-fat.md` (PR #302, which fixed only
*capturing* closures). **Fix:** in `src/compiler/elab_call.c`, strip the erased
`(:: <bare-fn> :ptr<void>)` ascription when the inner value is an unboxed
`TY_FN`, so it reaches the existing bare-fn auto-shim branch and is boxed via
`EX_FN_TO_FAT` -- the mirror image of the already-fat ascription strip directly
above it. A *capturing* closure's inner is a boxed `TY_FN` / `TY_PTR_VOID` and
is left untouched. Regression fixture:
`tests/fixtures/fat-captureless-closure-ptr-void/` (captureless SF + capturing
control through the `__apply-sf` direct-dispatch shape, both print `-0.5`).
Unblocks `tur-signal` Phase 5 `>>>` with the captureless Tier-1 shapers
(`invert`, `abs-sf`).

### use-after-move on local let-bound `:float` (FIXED)

[`../archive/history/use-after-move-on-local-let-bound-float-vs-captured.md`](../archive/history/use-after-move-on-local-let-bound-float-vs-captured.md)

A `let`-bound `:float` used twice in a single `if` tripped `TUR-E0005`
("binding was moved"). Root cause was **not** local-vs-captured asymmetry and
**not** float-specific: the builtin spec table in `src/compiler/builtins.c`
initialised `result_type` with a designated initializer leaving `.copy_kind`
zeroed -- and `CK_UNIQUE/CK_MOVE == 0`. Every builtin arithmetic result (int
and float alike) was being typed move-only. **Fix:** stamp the canonical
`copy_kind` for the result's TypeKind in `elab_call.c` after `builtin_lookup`.
Regression fixture: `tests/fixtures/use-after-move-float-let-vs-captured/`.

### One-off script print + annotation ergonomics (FIXED -- 3 findings)

[`../archive/history/one-off-script-print-and-annotation-ergonomics.md`](../archive/history/one-off-script-print-and-annotation-ergonomics.md)

Three papercuts around the freestanding `tur run /tmp/foo.tur` loop, all
resolved:

1. Misplaced effect annotation (`: int #{Unsafe}`) used to misdiagnose as
   "map literals not yet supported"; now a real ordering-specific diagnostic
   in `elab_fns.c`. Fixture
   `tests/fixtures/errors/effect-annotation-after-return-type/`.
2. `unknown function 'float->int'` now suggests the exact `(load
   "stdlib/math.tur")` line via `stdlib_load_hint_file` in `elab_call.c`.
   Fixtures `tests/fixtures/errors/unknown-helper-load-hint/`,
   `stdlib-float-convert-load/`. (Premise correction recorded in the original:
   `stdlib/math.tur` and `stdlib/bits.tur` are bare files, not `defmodule`s,
   so `(load ...)` is the only mechanism that works.)
3. `(load "stdlib/<file>")` now falls back to the resolved stdlib root the
   same way `import` does; `/tmp/foo.tur` resolves off-tree. See
   `elab_toplevel.c` Phase M and `tests/run-offtree-load.sh` (registered as
   `tur_offtree_load` ctest).

## Spice-side cleanups (paper trail)

### `__dsp_pair_*_float` bit-cast helpers removed

[`../archive/history/dsp-pair-bit-cast-helpers-obsolete.md`](../archive/history/dsp-pair-bit-cast-helpers-obsolete.md)

The pre-rebuild `tur-signal` spice's `dsp.tur` hand-rolled `Pair64
{int64_t first, second}` and `memcpy(&v, &bits, 8)` to read floats out of a
`(Pair Sample Sample)`, under the assumption typed pairs couldn't survive
fat-closure dispatch. The G4 readiness fixture
`tests/fixtures/pair-signals-typed/` proved the typed pair does round-trip
cleanly. `dsp.tur` deleted; `signal/shaper`'s `mix`/`add`/`multiply` now use
`stdlib/pair.tur`'s `pair-fst`/`pair-snd`.

### `svf-low-pass` removed (no consumers, was a half-stub)

[`../archive/history/svf-low-pass-removed-no-consumers.md`](../archive/history/svf-low-pass-removed-no-consumers.md)

Pre-rebuild `signal/synth.tur` exposed `svf-low-pass freq q` whose body wrapped
the 1-pole `low-pass alpha` and discarded `q` -- the "half-stub primitive"
anti-pattern the rebuild bans. Removed; zero consumers in the spices tree. A
genuine state-variable filter with resonance is Tier 2 under the rebuild plan
and lands behind a real consumer.

## Rediagnosed elsewhere

### `linalg/decomp.tur` unterminated list (source bug, not compiler)

[`../archive/history/linalg-decomp-qr-parser-unterminated-list.md`](../archive/history/linalg-decomp-qr-parser-unterminated-list.md)

Initial sweep flagged `tur build linalg/` failing on `decomp.tur:185`.
Investigation: the source genuinely has **4 unmatched `(`** across `qr`
(missing 2) and `lu` (missing 2). The file has only two commits in
`turmeric-spices` history and has never built. Compiler reporting is correct.
**Remaining compiler-side residue (low priority UX nit):** the "unterminated
list" diagnostic anchors the caret at the outermost still-open form spanning
to EOF; pointing at the **deepest** unclosed `(` would be more actionable.
Self-contained reader-side improvement worth a few lines next time the parser
is touched. Source fix lives in `turmeric-spices`.

## Cross-references

- `[[tur-signal-rebuild-plan]]` -- "Bug reports owed" items 1 and 3 are the
  two paper-trail entries above.
- `docs/upcoming/spices-v0.18-typing-migration-plan.md` -- linalg
  reclassification.
