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

[`../archive/history/load-inside-defmodule-silently-loses-names.md`](../archive/history/load-inside-defmodule-silently-loses-names.md)

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

[`../archive/history/fat-shim-void-ptr-calls-bare-not-fat.md`](../archive/history/fat-shim-void-ptr-calls-bare-not-fat.md)

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

### File-scope inline-C struct redefinitions across non-identical blocks (FIXED)

[`../archive/history/cascade-struct-redef-non-identical-blocks.md`](../archive/history/cascade-struct-redef-non-identical-blocks.md)

The previous file-scope inline-C dedup keyed on whole-block normalized text,
which only collapsed byte-identical blocks across modules. The tourist
cascade exposed the next layer: two modules each emitting a top-level block
with the same `struct __httpd_resp { ... };` decl but different surrounding
`#include` preludes and sibling decls. The block keys diverged, both blocks
were emitted, and cc rejected the TU with `redefinition of '__httpd_resp'`.
**Fix (Proposal #1, per-declaration dedup):** `inline_c_split_chunks` in
`src/compiler/emit_module.c` tokenizes each file-scope inline-C block into
chunks -- one per preprocessor directive (line starting with `#`) and one
per top-level `;`-terminated decl -- tracking string/char literals,
line/block comments, and `{}` depth so braces in struct bodies do not split
a decl. `inline_c_emit_block_deduped` dedupes chunks individually (under
the existing whitespace-normalized key) and emits only unseen chunks.
Applies to both the `cprelude` and `impl` dedup sites. Regression coverage:
`tests/fixtures/inline-c-file-scope-per-decl-dedup/` (two modules with
different `#include` sets sharing a struct decl plus a sibling decl) and
the existing byte-identical case in
`inline-c-file-scope-struct-dedup/` still passes. End-to-end validated by
`tur run ../turmeric-spices/spices/tourist/tests/fixtures/cascade/cascade.tur`
(4 tests, all green).

### `cons` builtin rejected `:cstr` head (FIXED)

[`../archive/history/cons-builtin-rejects-cstr-head.md`](../archive/history/cons-builtin-rejects-cstr-head.md)

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

[`../archive/history/unterminated-list-caret-anchors-outermost.md`](../archive/history/unterminated-list-caret-anchors-outermost.md)

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

[`../archive/history/tur-run-alias-breaks-snapshot-ci-guard.md`](../archive/history/tur-run-alias-breaks-snapshot-ci-guard.md)

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

[`../archive/history/project-mode-defstruct-typedef-missing.md`](../archive/history/project-mode-defstruct-typedef-missing.md)

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

[`../archive/history/project-mode-rc-runtime-preamble-missing.md`](../archive/history/project-mode-rc-runtime-preamble-missing.md)

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

[`../archive/history/defmodule-export-scoping-track.md`](../archive/history/defmodule-export-scoping-track.md)

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

[`../archive/history/load-not-expanded-in-imported-or-project-modules.md`](../archive/history/load-not-expanded-in-imported-or-project-modules.md)

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

[`../archive/history/parametric-struct-by-value-carrier-inconsistency.md`](../archive/history/parametric-struct-by-value-carrier-inconsistency.md)

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

### Generic inline-C struct arg monomorphises to `int64` (FIXED)

[`../archive/history/generic-inline-c-struct-arg-monomorphises-to-int64.md`](../archive/history/generic-inline-c-struct-arg-monomorphises-to-int64.md)

A `defn` taking a generic struct value through an inline-C body was
monomorphising the argument slot to `int64_t` instead of the concrete
struct type at the call site. **Fix (2026-06-10):** per-instantiation
inline-C emission in `src/compiler/emit_module.c` now propagates the
monomorphised arg type into the emitted C signature, so the struct
arrives by value with its real layout.

### ECS macro symbol synthesis missing (`str->sym`) (FIXED)

[`../archive/history/ecs-macro-symbol-synthesis-missing.md`](../archive/history/ecs-macro-symbol-synthesis-missing.md)

ECS-style macros that need to fabricate fresh symbols from string parts
had no `str->sym` builtin to call -- the expansion tree could be built
manually with `dot-sym` but not parameterised by macro-arg substrings.
**Fix (2026-06-11):** `str->sym` builtin landed at
`src/compiler/elab_macros.c:321-326`, available inside macro bodies for
symbol synthesis.

### ECS sparse-set backward shift lost wrapping entries (FIXED)

[`../archive/history/ecs-sparse-backward-shift-loses-wrapping-entries.md`](../archive/history/ecs-sparse-backward-shift-loses-wrapping-entries.md)

The linear-probing sparse-set in `ecs/sparse.tur` lost entries whose
probe sequence wrapped past the table end when a backward shift was
triggered by deletion. **Fix (2026-06-11):** ported `ecs/sparse.tur` to a
full Robin Hood implementation; wrapping is handled correctly across
deletions and resizes.

### pack/open phantom-opaque body type collapses (FIXED -- 2026-06-12)

[`../archive/history/pack-open-phantom-opaque-body-type-collapses.md`](../archive/history/pack-open-phantom-opaque-body-type-collapses.md)

`(pack v (exists [n] (SizedBuf n)))` followed by `(open ... [n buf] ...)`
bound `buf` at bare `SizedBuf` instead of `(SizedBuf n)`, so any downstream
call expecting `(type-app SizedBuf tyvar)` rejected with TUR-E0001. The
SizedVec/GADT path carried the index through the constructor chain; the
defopaque path went through a different lowering that dropped the applied
form on projection. **Fix:** `elab_open` (`src/compiler/elab_types.c`)
now preserves the applied form when the existential body's head is a
defopaque, so `(SizedBuf n)` projects to `(SizedBuf n)`. SizedVec path
unchanged. Witness: `tests/fixtures/sized-buf-existential-pack-open`.

### Two nested `open` binders produced indistinguishable skolems (FIXED -- 2026-06-12)

[`../archive/history/open-binder-skolems-not-distinguishable.md`](../archive/history/open-binder-skolems-not-distinguishable.md)

After the pack/open fix above, nested opens produced `(SizedBuf <skolem_a>)`
and `(SizedBuf <skolem_b>)` represented identically as `TY_STRUCT def=NULL`.
`type_eq` treated them as equal, so a cross-skolem call
`(sized-buf-copy! a b)` from separate opens accepted instead of rejecting
under TUR-E0260. **Fix (Direction A):** step 1 (type_eq TY_TYVAR by name),
prereq shims at 7 of 11 def==NULL sites, step 2a (named TY_TYVAR at parse
time), step 2b (per-open skolem substitution via `subst_tyvar_name` in
`elab_open`), plus a printer improvement so cross-skolem diagnostics show
distinct tyvar names. Cross-open calls now reject with TUR-E0001 showing
both skolem names. Witness:
`tests/fixtures/errors/sized-buf-existential-cross-open-reject/`.

### `open` body sees polymorphic helpers but codegen drops them (FIXED -- 2026-06-12)

[`../archive/history/open-monomorphizes-polymorphic-fn-only-partially.md`](../archive/history/open-monomorphizes-polymorphic-fn-only-partially.md)

Type-checking of `(sized-buf-free buf)` inside `(open packed [n buf] ...)`
succeeded, but the C-codegen monomorphizer did not emit the
`sized_hybuf_hyfree` instantiation, producing a C file that referenced an
undeclared function. Helpers reachable only through an open were skipped
because the open's abstract skolem did not seed the worklist. **Fix:** two
new cases (`EX_EXISTS_PACK`, `EX_EXISTS_OPEN`) added to
`emit_abi_scan_expr` in `src/compiler/emit_module.c` -- the scanner now
recurses into the packed value and the open body. The fixture
`tests/fixtures/sized-buf-existential-pack-open` was updated to call
`sized-buf-free buf` inside the open body and the `requires.no-leak-check`
marker dropped.

### typeclass instance method captures pass-by-ptr struct param as value in `unsafe`-body env (FIXED -- 2026-06-12)

[`../archive/history/typeclass-method-struct-arg-closure-codegen.md`](../archive/history/typeclass-method-struct-arg-closure-codegen.md)

A `definstance Encode [User]` (or any typeclass instance) whose method
body was wrapped in `(unsafe ...)` and accessed 3+ fields of a struct
parameter miscompiled: the typeclass dispatch shim passed the struct
as `const User *` (Phase D's pass-by-ptr rule for >16-byte structs),
but the `unsafe`-body's closure env declared its capture slot as
`User` (struct value). The env fill `__henv->x = x;` and writeback
`x = __henv->x;` then refused to compile. Surfaced while landing the
P2a `derive-json` minimal slice (`derive-json User (id int) (name cstr)
(active bool)` -- the 3-field case). **Fix:** `src/compiler/emit_effects.c`
env-struct emit now checks `type_struct_pass_by_ptr(b->type)` AND
whether `b` is one of `ctx->fn_params`; when both hold, the env field
is emitted as `const T *` rather than `T`, matching the param shim.
Let-bound captures still store struct values by value. Regression
fixture: `tests/fixtures/typeclass-unsafe-passbyptr-struct-arg/`.

### typed Decode-typeclass surface: three interacting issues blocking (Result a B)-returning instance methods (FIXED -- 2026-06-12, three prereqs)

[`../archive/history/typeclass-method-parameterized-result-carrier-mismatch.md`](../archive/history/typeclass-method-parameterized-result-carrier-mismatch.md)

The P2a `derive-json` Decode side wanted `(defclass Decode [a] (decode
: doc -> off -> (Result a cstr)))` with instances on `int`, `cstr`,
etc. The shape hit three interacting issues; each had to land before
the surface compiles. **Prereq 1**
(`src/compiler/emit_module.c`): `emit_abi_scan_expr` got an
`EX_HANDLE` case, mirror of the prior `EX_EXISTS_OPEN` fix --
polymorphic helpers called only through `(unsafe ...)` now monomorphize
correctly (no more undeclared `ok_hyval`). **Prereq 3**
(`stdlib/result.tur`): `ok` and `err` made polymorphic
(`(defn ok [A B] [x : A] : (Result A B) ...)`), mirroring
`stdlib/pair.tur:28`. 73 codegen snapshots regenerated (prelude no
longer emits `static int64_t ok(int64_t)` unconditionally).
**Prereq 2** (`src/compiler/emit_expr.c` around line 2452): the
specialized-call carrier->concrete bridge had a `TY_INT`-only gate
that missed parameterized-struct `TY_APP` arguments. Widened to also
accept aggregate types whose elab kind uses the carrier ABI and which
are NOT by-value producers; routes through `emit_carrier_bridge` with
`CK_CARRIER -> CK_CONCRETE` to deref the heap-pointer carrier before
passing to a by-value struct param. Zero codegen regens for this one
(only fires on previously-broken expressions). Regression fixture:
`tests/fixtures/typeclass-method-parameterized-result-decode/`.

### return-dispatch typeclass ascription not honored when return is `(Result a B)` (FIXED -- 2026-06-13 as Prereq 5)

[`../archive/history/return-dispatch-ascription-result-wrapped-not-honored.md`](../archive/history/return-dispatch-ascription-result-wrapped-not-honored.md)

After Prereqs 1-4 landed, the typed `Decode` follow-up surfaced a
deeper elaborator gap: bare-`a` return-dispatch correctly picks
instances by ascription (`(:: (show 42) :cstr)` -> `Show [cstr]`),
but wrapping the return in `(Result a cstr)` made both ascriptions
silently resolve to the same (first) instance. Root cause was two
issues compounding: (a) `parse_typeclass_method` resolved return-type
forms without passing the class's type parameters, so a class tyvar
nested in `(Result a cstr)` parsed as an undefined opaque rather than
TY_TYVAR; (b) `elab_try_return_dispatch` then set the call's elab
type to the bare unified `bound` instead of substituting `a := bound`
into the method's full return type. **Fix:** thread `class_type_params`
through the two `type_expr_from_form` calls in
`parse_typeclass_method` (with a NULL-tolerant `type_param_kinds`
fallback at `elab_types.c:403`), and at `elab_typeclasses.c:3085`
use `*e->expected_type` (the ascribed type, which already substitutes
`a := bound`) when an ascription is present. Regression fixture:
`tests/fixtures/typeclass-return-dispatch-result-wrapped/`.

## Interpreter (turi) parity (landed)

### Deep non-tail recursion overflowed the C stack before the depth guard fired (FIXED -- Direction A)

[`../archive/history/turi-deep-recursion-c-stack-overflow.md`](../archive/history/turi-deep-recursion-c-stack-overflow.md)

The interpreter's recursion-depth guard (`eval_depth >= max_eval_depth`) was
dead code: `max_eval_depth` was a hardcoded **4096**, but each `eval_expr`
frame burns ~10 KB of C stack (Debug/ASan), so a ~12.5 MB stack overflowed at
peak `eval_depth` ~1250 -- the native stack always blew first, killing deep
non-tail recursion with a raw SIGSEGV (rc 139, no diagnostic) instead of the
intended clean error. **Fix (2026-06-12, Direction A -- the report's "smallest
fix"):** `turi_env_new` now derives `max_eval_depth` from
`getrlimit(RLIMIT_STACK)` divided by a conservative per-frame cost
(`turi_default_max_eval_depth`, `src/turi/env.c`), targeting ~50% of the
measured crash depth (a 2x margin). Deep non-tail recursion now prints
`eval: recursion limit exceeded` (rc nonzero) in **both** Debug/ASan and
Release -- never a SIGSEGV. Measured threshold on a 12.5 MB stack: ~300 logical
levels succeed, deeper errors cleanly. Sandbox depth (`TURI_DEFAULT_SANDBOX_DEPTH
= 256`) is unchanged. Validation: the report's `sum-to 5000` repro now errors
cleanly; `run-turi.sh` 979/0, eval/sandbox ctests 14/14, compiled suite 1596/0.
**Trade-off / follow-up:** Direction A caps achievable depth *below* the C-stack
ceiling rather than lifting it, so the parity gap (a few-hundred-deep recursion
runs compiled but not interpreted) is narrowed, not closed, and
`tests/fixtures/escape-deep-capture` stays `requires.compiled`. The
explicit-stack/trampoline rework that removes the native-stack dependency
entirely (Direction D) is planned in
[`../../upcoming/v1/turi-eval-trampoline-plan.md`](../../upcoming/v1/turi-eval-trampoline-plan.md).

### Context-capturing serial-shift / cloneable-shift implemented in the interpreter (FIXED)

[`../archive/history/turi-capturing-shift-unimplemented.md`](../archive/history/turi-capturing-shift-unimplemented.md)

The tree-walking interpreter handled only the *abortive* shift/reset; the
context-capturing `serial-shift` / `cloneable-shift` (which hand a resumable
continuation to their receiver) fell through to an "unhandled expression kind"
error, so such programs ran only on the compiled path. **Fix (2026-06-12):**
`EX_SERIAL_RESET` / `EX_CLONEABLE_RESET` now reify the delimited context **at
runtime** (`ts_capture_and_run`, `src/turi/eval.c`) -- walking the reset body
down the unique shift-reaching child through the same grammar the compiled
`collect_ctx` accepts (single-hole int `+ - * /` binops, 1-/2-arg call frames,
pure `let`, an `if` with one shift-bearing arm, a `do`-prelude + ignore-value
tail), evaluating each non-hole operand once at capture time and recording it as
a frame. The continuation is that frame array boxed as an int64 handle; resume
folds the frames innermost-first, so it is multi-shot for cloneable and
in-process marshalable for serial. The `tur_{cloneable,serial}_cont_*` builtins
and `stdlib/workflow.tur`'s `save-cont!`/`resume-cont!` are wired as interpreter
natives over it; an uncapturable context raises the compiled path's `TUR-E0706`.
13 of 14 context fixtures + the 2 `*-not-capturable` negatives pass under
`--interpret`; `EX_SERIAL_SHIFT`/`EX_CLONEABLE_SHIFT` left `turi-carve-out.txt`.
**Remaining carve-out (tracked elsewhere):** `serial-context-do-struct`
(`requires.compiled`) needs inline-C struct-accessor execution + a `Serializable`
instance the tree-walker cannot run -- the inline-C-evaluator gap, see
[`turi-inline-c-silent-miscompiles.md`](../archive/history/turi-inline-c-silent-miscompiles.md);
`call/cc*` (`EX_CALLCC`) stays a separate CPS-transform carve-out.

### `errors/` diagnostic divergences under `--interpret` -- all 9 closed (FIXED)

[`../archive/history/turi-error-fixture-diag-divergences.md`](../archive/history/turi-error-fixture-diag-divergences.md)

With `tests/fixtures/errors/*` wired into `run-turi.sh`, 9 negative fixtures
emitted a different (or no) diagnostic under `tur --interpret` than on the
compiled path. **Fix (2026-06-12, across four passes):** the 3 reporting-stage
cases (unbound call head + load hint, heterogeneous-map, runtime error from
`main`) were routed through the shared diagnostic / `cmd_eval` stderr path; the
`#lang` not-implemented reject and the `TUR-E0106` lifetime-cycle pass were added
to `turi_eval_impl`; the reader-macro registry was made strict for file-eval
(REPL stays lenient); and the 2 `serial-context-{,do-}not-capturable` cases now
emit `TUR-E0706` once `ts_capture_and_run` landed. `TURI_ERRORS_DENY` is now
empty -- every `errors/` negative fixture's diagnostic matches under the
interpreter.

### inline-C `free` matcher over-claimed `*_free(` bodies (FIXED -- 2026-06-12)

[`../archive/history/turi-inline-c-free-matcher-overclaims.md`](../archive/history/turi-inline-c-free-matcher-overclaims.md)

`try_exec_simple_inline_c`'s Pattern 1 (`free`) used a loose substring
match on `free(`, mis-claiming any body that called `tur_hamt_iter_free(`,
`tur_hamt_free(`, `xfree(`, etc. -- silently reducing them to
`free(arg0)`, a UAF on map/HAMT iteration state and a silent miscompile of
the surrounding logic. **Fix:** Pattern 1 now requires a *standalone*
`free(` token (`ic_has_standalone_free`) and refuses bodies that also
return a value or fat-dispatch a closure (`!has_return && !has_fptr`).
Shipped alongside the `native_map_eq_raw[_k]` natives that make `map-eq?`
actually evaluate under `--interpret`.

### 25 inline-C fixtures silently miscompiled under `--interpret` -- all 20 remaining now refuse-rather-than-guess (FIXED -- W4, 2026-06-12)

[`../archive/history/turi-inline-c-silent-miscompiles.md`](../archive/history/turi-inline-c-silent-miscompiles.md)

The `ic_exec_*` matchers in `src/turi/eval.c` were tightened to refuse on
any shape they cannot evaluate faithfully. All 20 previously-silent
miscompiles now flip to a clean `rc=1` "inline-C not supported" error --
no more rc=0 wrong answers. Tightenings: **`ic_exec_constructor`** (11
fixtures incl. the `backtrack-*` cluster, `arrow-instance-loop-nonrecursive`,
`workstealing-*`) declines bodies with loops, a second allocation,
`__atomic`/`TUR_APPLY`, or chasing a stale matcher; plus matchers for
struct-accessor, conditional snprintf, and others. The
serial-context-do-struct carve-out
([`turi-capturing-shift-unimplemented.md`](../archive/history/turi-capturing-shift-unimplemented.md))
still needs inline-C struct-accessor execution and is tracked separately.

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
