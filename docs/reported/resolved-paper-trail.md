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

### Poly-closure / typed fat-closure dispatch -- consolidated track (FIXED)

[`../archive/history/poly-closure-typed-dispatch-track.md`](../archive/history/poly-closure-typed-dispatch-track.md)

Consolidated three reports on typed/polymorphic closure-returning combinators
(`>>>`, `cmp`) miscompiling at float result types -- the inner thunk body rode
the `int64_t(*)(void*, int64_t)` ABI and produced correct float output only by
SysV register-class accident. **All three layers landed:** Layer 2
(capturing-closure return-type lowering) via source-level boxing in
`elab_fns.c`; Layer 3 (fn-typed `^fat` param tyvar propagation + per-spec
inner-body retention under fat dispatch) via Direction 3 recovery in
`emit_expr.c` (#297); and Layer 1 (the combinator itself) via Stage E (#300),
which rewrote `stdlib/arrow.tur`'s `>>>` to the polymorphic typed spelling
`(defn >>> [A B C] [^fat f :(fn [A] #{} B) ^fat g :(fn [B] #{} C)] : ptr<void>
...)` and deleted the monomorphic `compose-float` workaround. A `:float ->
:float` pipeline now emits `tur_thunk_double_double_t` end-to-end (xmm0). The
`TUR-E0705` guard was narrowed to `closure_return_dispatches_untyped`, so it
fires only for the genuinely-untyped dispatch shape, never the typed `>>>`/`cmp`
form. Validation fixture: `tests/fixtures/poly-closure-compose-float/` (the
previously-E0705 dispatching shape) prints exact fractional output
(`3.675`, `1.75`). Originals preserved verbatim under `../archive/history/`
(`arrow-compose-float-closure-int64-thunk-mismatch.md`,
`boxed-fn-typed-closure-return-miscompiles.md`,
`poly-closure-inner-dispatch-result-erased.md`).

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

### `^fat` let-binding of a runtime `ptr<void>` fat closure re-shimmed as a bare fn (FIXED)

[`../archive/fat-shim-void-ptr-calls-bare-not-fat.md`](../archive/fat-shim-void-ptr-calls-bare-not-fat.md)

A `^fat` let-binding carrying a runtime `ptr<void>` fat closure but annotated
with a concrete `(fn ...)` type (e.g. `^fat sf : (fn [ptr<void>] #{} ptr<void>)
(make-scale 2.0)`) was retyped to `TY_FN`. When that binding was then passed to
a `>>>` whose receiver selects the arrow typeclass instance, `arrow_fat_shim`
in `src/compiler/elab_typeclasses.c` mistook the `TY_FN` value for a bare
non-capturing function and wrapped it in a `__tur_fatshim_void___void__` box.
That shim reads slot 1 (the original fat closure) and calls it as a **bare
one-arg** function, but the fat-closure thunk expects two arguments (env + arg),
**segfaulting** at the composed call site. This blocked `tur-signal` Phase 5
`effects-chain` from folding a Vec of SFs with `>>>`. **Fix (PR #302/#305):**
add an `is_fat` guard in `arrow_fat_shim`, mirroring the existing guard in
`elab_call.c` (`two-level-sf-closure-return-miscompiles-out-binding`): when the
argument is an `EX_VAR` whose binding is `is_fat` with an unboxed `TY_FN` type,
it is already a fat closure carried as `int64_t` -- retype it to `ptr<void>` so
it flows through the already-fat pass-through branch instead of being
double-boxed. Regression fixture:
`tests/fixtures/fat-shim-void-ptr-arrow-compose/` (the report's minimal repro;
prints `6` instead of segfaulting). A recursive `>>>`-fold over `^fat`
let-bindings -- the `__sf-fold` shape the report flagged as the real blocker --
also composes correctly. The captureless sibling of this report is the next
entry.

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

### Inline-C struct redefinitions at file scope broke multi-block codegen (FIXED)

[`../archive/history/inline-c-struct-redef-at-file-scope.md`](../archive/history/inline-c-struct-redef-at-file-scope.md)

Two modules in one TU each emitted a byte-identical top-level ```` ```c ... ``` ```` block
declaring the same carrier struct (the idiom tourist/httpd use to share a
layout across modules). The duplicate file-scope `struct __foo { ... };`
collided in `cc`, breaking any cascade build that imported both modules.
**Fix (proposed #2):** de-duplicate identical file-scope inline-C blocks
within a single TU via `inline_c_dedup_seen` in
`src/compiler/emit_module.c`, applied in both the `emit_program`
(`cprelude`) and `emit_implementation` (Pass 1a) emit paths. Regression
fixture: `tests/fixtures/inline-c-file-scope-struct-dedup/`. (Note: the
original root-cause sketch assumed leading decls were hoisted to file
scope; corrected in the archived report -- function-body inline-C is
emitted *inside* the generated `static` function and never collides.)

### httpd/tourist cascade `mbedtls/net_sockets.h` not found (FIXED -- spice-side)

[`../archive/history/cascade-mbedtls-header-not-found.md`](../archive/history/cascade-mbedtls-header-not-found.md)

`tur run` of the tourist cascade fixture failed in `cc` with
`fatal error: 'mbedtls/net_sockets.h' file not found` on hosts without
mbedTLS installed -- the `http` spice's client unconditionally `#include`d
mbedtls headers even for plain `http://` requests, and similarly
`http/response`'s `response-json` unconditionally pulled in `<yyjson.h>`.
**Fix (proposed #2, in `../turmeric-spices`):** in
`spices/http/src/http/client.tur`, rewrote the plain-HTTP path to use
raw POSIX sockets (`socket`/`connect`/`send`/`recv`) and gated the
mbedTLS includes + entire TLS branch behind
`__has_include(<mbedtls/ssl.h>)`; `https://` now returns a runtime err
with a rebuild hint when mbedTLS is absent. Same shape applied to
`response-json` in `spices/http/src/http/response.tur` for the yyjson
dependency. Plain `http-get` compiles and runs on bare hosts. The
cascade fixture itself still fails on a residual struct-redef pattern
tracked in [`cascade-struct-redef-non-identical-blocks.md`](cascade-struct-redef-non-identical-blocks.md).

### `cons` builtin rejected `:cstr` head (FIXED)

[`../archive/cons-builtin-rejects-cstr-head.md`](../archive/cons-builtin-rejects-cstr-head.md)

The user-callable `cons` builtin (added in `bc2074ad` to make project-mode
`defmodule` reach the runtime list constructor without stdlib auto-load)
was registered `arity 2..2 arg=int result=int`, so `(cons "hello" 0)`
raised `unknown function or operator 'cons'` -- blocking the
spices-cons-workaround paydown plan. **Fix:** the builtin's spec arg type
is now `TY_UNKNOWN` and `elab_call.c` (`cons_wildcard`) bypasses the
strict per-arg type check for the `cons` builtin, since the cell layout
(`{int64_t head; int64_t tail;}`) is already pointer-agnostic and codegen
casts via `(int64_t)(intptr_t)` -- so any 64-bit-sized head (`:cstr`,
opaque handle, pointer, int) round-trips. Regression-covered by
`tests/fixtures/cons-builtin-cstr-head/`.

### "unterminated list" caret ribbon spans opener-to-EOF (FIXED)

[`../archive/unterminated-list-caret-anchors-outermost.md`](../archive/unterminated-list-caret-anchors-outermost.md)

`read_seq` in `src/compiler/reader.c` emitted its "unterminated list
(missing ')')" diagnostic with a `Span` running from the opener to the
current EOF offset; on real files (e.g. `linalg/decomp.tur` qr defn,
~110 lines) this produced a multi-screen `^^^^^^...` ribbon that buried
the actual location. The anchor itself was already at the deepest
unclosed `(` (read_seq recurses, so only the innermost emits), but the
*length* of the span was the problem. **Fix:** anchor at the opening
delimiter only (`start_off..start_off + 1`) so the caret is a single
character at the deepest unclosed `(`. No new failures in
`tests/run.sh` (one fixture actually moved from FAIL to PASS).
Companion paper-trail follow-up to the linalg/decomp.tur
rediagnosis-as-source-bug.

### `tur run` aborts on Justfile `alias`, silently disabling snapshot-drift guard (FIXED)

[`../archive/tur-run-alias-breaks-snapshot-ci-guard.md`](../archive/tur-run-alias-breaks-snapshot-ci-guard.md)

`tur run`'s embedded Justfile parser treated `alias NAME := TARGET` as an
unsupported feature and aborted the whole-file parse, so every recipe
after the alias was unreachable -- which silently disabled the Phase 0.3
snapshot-drift CI guard `./build/tur run regen-snapshots -- --check`.
**Fix (proposed #1):** `src/compiler/justrun.c` now parses aliases into a
new `JAlias` table on `JFile` (cap `JR_MAX_ALIASES = 64`); `find_recipe`
resolves alias name -> target recipe in a single hop and the
recipe-not-found "available" hint lists aliases alongside recipes. The
`check_unsupported` branch for `alias` is gone. Regression-covered by
`tests/run-tur-run-alias.sh` (registered as the `tur_run_alias` ctest):
asserts the alias resolves to its target, recipes *after* the alias remain
reachable (the original blast radius), and the original target name still
resolves directly.

### Project-mode codegen: `defstruct` typedef missing from header/impl (FIXED)

[`../archive/project-mode-defstruct-typedef-missing.md`](../archive/project-mode-defstruct-typedef-missing.md)

In project mode (`tur build <dir>` / separate compilation), generated
headers/implementations omitted the `typedef struct Name { ... } Name;`
declaration for every non-opaque struct -- single-file `emit-c` was fine,
but any project-mode spice with a `defstruct` failed to link in `cc`.
**Fix:** `emit_header` (`src/compiler/emit_module.c:5375-5428`) now emits
the typedef for every non-opaque struct def, and the `emit_implementation`
`EX_DEF` arm early-outs on struct defs so the spurious `Name Name_N;`
variable declaration is suppressed. Regression-covered by
`build-project-defstruct-typedef` in `tests/run-build-project.sh`; the
`spices/signal` `ADSRParams` repro builds end-to-end through
`signal__envelope.c`.

### Project-mode codegen: RC/frame runtime preamble + struct drop/walk glue missing (FIXED -- #321)

[`../archive/project-mode-rc-runtime-preamble-missing.md`](../archive/project-mode-rc-runtime-preamble-missing.md)

The broader sibling of the defstruct typedef report: any project-mode module
using reference counting (`rc/of`, `rc<T>` fields, auto-drop) failed to
compile in `cc` because the RC/frame runtime preamble and per-struct
drop/walk glue were absent from the generated header/impl, even though
single-file `emit-c` emitted them. **Fix (#321):** rebased onto #320
("separate-compilation runtime scaffolding") -- the overlapping fixed-runtime
emissions are now **idempotent**: `emit_closure_fat_runtime` takes a
`guarded` flag (wraps in `#ifndef TUR_RT_CLOSURE_FAT` under separate
compilation), and the preamble's `tur_poly_fn_t` reuses #320's
`TUR_POLY_FN_T_DEFINED` guard so the shared `tur_runtime.h` and #320's
per-module copies dedupe to one definition. Single-file output stays
byte-identical (both flags off). The whole-program executable reroute
auto-loads stdlib via the single-file path; the `prelude-cons` regression
renames its local cell accessors off the stdlib names
(`list-head` -> `cell-head`) to avoid the now-correct redefinition flag.
Full suite: 1535 passed, 0 failed.

### defmodule export scoping & project-mode build -- consolidated track (FIXED)

[`../archive/defmodule-export-scoping-track.md`](../archive/defmodule-export-scoping-track.md)

Consolidated two adjacent defects breaking spice project-mode builds: (A) typed
`^fat` parameter annotations were lost across the `defmodule`/`export`
boundary because `param_poly_types` wasn't populated for `^fat TY_FN` params,
and (B) project mode skipped the stdlib prelude auto-load that single-file
mode performed (prelude macros `cons`, `head`, `tail`, plus runtime
constructors). **Fix:** Defect A landed 2026-06-09 (`param_poly_types`
populated for `^fat TY_FN` params, recovering the typed annotation through
separate compilation); Defect B landed 2026-06-10 across F1/F4 (prelude
macros + min/max) and F3/F6 (the user-callable `cons` runtime constructor
now resolves under project mode). F2/F5 (selective `:refer` of math/bits)
remain as non-blocking enhancements rather than defects. Together they
unblock the `tur-signal` surface and 5+ other spices' project-mode builds.

### `(load ...)` inside imported / project-mode modules -- unreachable scaffolding (FIXED)

[`../archive/load-not-expanded-in-imported-or-project-modules.md`](../archive/load-not-expanded-in-imported-or-project-modules.md)

Top-level `(load "...")` was not expanded inside imported or project-built
`defmodule` bodies, so `stdlib/arrow.tur`'s `>>>` (and any other module
whose runtime preamble was load-pulled) was unreachable from a `defmodule`
that was imported or project-built. Cascaded into typeclass-ordering and
runtime-preamble holes under separate compilation. **Fix:** import-path
`load` expansion fixed; project-mode typeclass ordering fixed; project-mode
bare-defn `load` fixed; project-mode `load` of a runtime-preamble-dependent
file (poly-fn / higher-kinded dict / spliced ADT / `^fat` combinators) now
emits the missing per-module scaffolding (`tur_poly_fn_t`, base `tur_adt_*`
typedef + ctors, fn-ptr typedefs, `__tur_fatshim*` / `__tur_poly_to_fat*` /
`TUR_APPLY*`); `arrow.tur` is self-contained re: `tuple.tur`'s `Tuple2`;
imported typeclass instances no longer leak into the importer's TU under
separate compilation. All regression-covered by `tests/run-build-project.sh`.

### Generic parametric-struct-by-value lowering -- internally inconsistent in separate compilation (FIXED)

[`../archive/parametric-struct-by-value-carrier-inconsistency.md`](../archive/parametric-struct-by-value-carrier-inconsistency.md)

For a parametric struct (`n_type_params > 0`) erased to the `int64_t` carrier,
`EX_MAKE_STRUCT` emitted an invalid `(int64_t){.e1=..}` compound literal while
`EX_GET_FIELD`'s `through_carrier` path read it as a heap pointer
`((Name *)(intptr_t)v)->field` -- the two sides disagreed on carrier
representation, so the generic (unspecialized) form of e.g. `tuple2`/
`tuple2-1st` could not be emitted as valid, self-consistent C under separate
compilation. **Fix (2026-06-10):** separate compilation now mirrors
whole-program -- the invalid generic carrier body is pruned (`emit_implementation`
skips it via `emit_abi_fn_skip_generic`) and the monomorphized struct-app
typedef is emitted in the header ahead of the spec-clone decls
(`emit_header` registers spec result/arg types before the struct-app flush).
Regression-covered by `build-project-parametric-struct-by-value` in
`tests/run-build-project.sh`; the report's `tuple.tur` repro also builds.

### `tur build --shared .` produced `lib..so` / `lib..so.manifest` (FIXED)

[`../archive/history/tur-build-shared-cwd-lib-double-dot.md`](../archive/history/tur-build-shared-cwd-lib-double-dot.md)

Running `tur build --shared` with a cwd-relative target (`.` or `./`)
produced shared libraries named `lib..so` / `lib.so` with matching
`.manifest` sidecars, because `default_output_name` (`src/main.c:1389`)
ran `basename_of(input)` directly and got back `"."` or `""`. **Fix
(#56874ff3, hardened in #309):** when the basename resolves to a "current
directory" sentinel, `default_output_name` now calls `realpath()` and
uses the basename of the resolved absolute path, with a `resolved_dir`
flag suppressing the trailing-extension strip so directory names like
`my.project` survive intact. The companion manifest-`:name` preference
landed alongside the build-output-directory plan.

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
