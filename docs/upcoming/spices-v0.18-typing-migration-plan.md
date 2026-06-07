---
title: Spices Migration to Post-v0.17 Advanced Typing + Mangling
category: Planning
description: Audit and remediation plan for breakage in `../turmeric-spices` caused by the typing, closure ABI, mangling, and typeclass changes that have landed on `main` after the v0.17.0 cut. v0.18.0 itself shipped before most of the typing work merged; this plan covers the union "post-v0.17.0 main" surface that spices will hit on the next prebuilt-binary rebase.
---

# Spices Migration to Post-v0.17 Advanced Typing + Mangling -- Plan

## Summary

`v0.18.0` (tagged 2026-06-02) shipped before the advanced-typing wave landed
on main. The relevant compiler changes -- reversible name mangling (#275),
typed closure invocation ABI (#276), Category + Kleisli ArrowZero (#290),
`stdlib/arrow.tur` re-consolidation (#277), TUR-D0001 fn-type colon
deprecation (#270/#285), `^fat` params emitted as `int64_t` in inline-C
(#286), int<->ptr<void> carrier bridging (#287/#293), `definstance`
idempotence (#278), and TUR-W0039 method/defn clash (#266) -- all merged
*after* `v0.18.0` but are present on `main` today and will appear in the
next tagged release. Spices currently install a prebuilt `tur` via
`scripts/install-tur.sh`; they will break as soon as that tarball advances
past `v0.18.0`.

A live build sweep with the current main `tur` (commit `c60ba4ca`) against
`/Users/rjungemann/Projects/turmeric-spices/spices/*` resolves the breakage
into **three root causes** (ordered by spice impact):

1. **Inline-C mangling drift** -- #275 changed the mangling of `-`, `/`,
   and `_`; every hand-written C identifier that names a Turmeric global
   must be rewritten. Hits `frame`, `watch`, `sdf-raylib`.
2. **Prelude macros unreachable from `defmodule`** -- `stdlib/macros.tur`
   and friends are bare load files, not `defmodule`s, so
   `(import stdlib/macros :refer [when])` does not work. Hits `tourist`,
   `httpd`, `c-dsl`, `glsl`, `stats`. Spice-side fix is one-line
   `(load ...)`; the durable compiler-side fix is tracked separately in
   [`docs/reported/prelude-macros-not-importable-inside-defmodule.md`](../reported/prelude-macros-not-importable-inside-defmodule.md).
3. **Source-side issues surfaced by the sweep** -- the `linalg`
   decompositions are a WIP file with 4 unmatched parens that has never
   built (see
   [report](../reported/linalg-decomp-qr-parser-unterminated-list.md)
   for the diagnostic-UX residue), and `math` carries an `extern-c malloc`
   redeclare incompatible with newer Apple SDKs. Neither is a typing-wave
   regression.

Plus the long-standing third-party-header failures (`json`, `regex`,
`http`) that block validation but are not in scope for typing-wave
remediation. No spice trips TUR-D0001 today (no `(fn [:T] ...)`
colon-in-fn-type forms in spice source). `^fat` usage in `spices/signal/`
and `^fat` callers in `spices/frame/` need a re-validation pass once the
prelude breakage is unblocked.

## Compiler changes affecting spices (v0.17.0..HEAD on main)

Sorted by spice impact. Each bullet lists the merge SHA and the spice failure
class it produces.

- **#275 `77e73c9e` -- Reversible (injective) name mangling.**
  Globals now spell `-` as `_hy`, `/` as `_sl`, and `_` as `_un`; `_` is no
  longer a fold. **Any inline-C body that hand-writes a Turmeric global's C
  symbol** (i.e. anything other than calls back into compiler-generated thunks)
  must be rewritten to the new spelling. This is the largest single cause of
  spice breakage. See `docs/guides/name-mangling-guide.md`.
- **#286 `5dccd210` -- `^fat` params emitted as `int64_t` in inline-C.**
  Inline-C function bodies whose Turmeric signature declares a `^fat`
  parameter now receive that parameter as `int64_t`, not `void *`. Inline-C
  that stores `f` straight into an `int64_t` slot is now warning-clean; inline-C
  that dereferences the param *as a function pointer* must cast through
  `(void *)(intptr_t)`.
- **#276 `839b31ab` -- closure-representation unification + typed closure
  invocation.** `:fn` carriers and `^fat` arguments are dispatched through
  `TUR_APPLY*` / `TUR_APPLY*_T`. Hand-written inline-C that bypasses these
  helpers and casts a closure value to `int64_t (*)(int64_t)` segfaults on
  capturing closures.
- **#287 `9c9a90bf` / #293 `9588cda7` -- int<->ptr<void> carrier bridge at
  closure/SF call sites.** Signal-functor combinators that pass `:ptr<void>`
  closures through generic-typed parameters now get an automatic
  `(int64_t)(intptr_t)` carrier cast. Spices that previously hand-rolled the
  cast (the `(let [sentinel 0] ...)` workaround, or explicit `:ptr<void>`
  ascriptions) should keep working but can be simplified.
- **#277 `1b17d582` -- `stdlib/arrow.tur` re-consolidation.**
  `stdlib/arrow-class.tur` was deleted; its six defclasses and four
  `(->)` instances live in `stdlib/arrow.tur` again. Spices that imported
  `stdlib/arrow-class.tur` directly must be updated.
- **#270 `bf3445e5` -- fn-type colon-rewriter codemod + stdlib sweep.**
  Ships `tools/rewrite_fn_type_colons.py`. Useful in-tree even though no
  current spice trips the deprecation.
- **#285 `1a4bff30` -- TUR-D0001 deprecation: colons inside `(fn ...)` types.**
  `(fn [:int] :int)` now warns; `--Werror=deprecated` promotes it to a hard
  error. Spices are clean today; record this as a "stay clean" gate.
- **#290 `21d11393` -- `Category` typeclass + honest Kleisli `ArrowZero`.**
  New `Category` superclass + Kleisli instances under `stdlib/kleisli.tur`.
  Spices that define their own `Arrow`-shaped typeclass instances must declare
  the `Category` superclass dictionary or accept the diagnostic.
- **#278 `3aee822c` (+ `44a91fe0`) -- `definstance` idempotence.**
  Re-loading the same instance is now a no-op. Cross-spice transitive imports
  of arrow/functor instances no longer double-register.
- **#266 `4e34c13b` -- TUR-W0039: method-vs-defn name clash warning.**
  Any spice that defines a free defn whose name collides with a typeclass
  method (e.g. `arr`, `>>>`, `<<<`, `first`, `second`, `app`, `loop`) gets a
  warning. With #277 in place, `first`/`second`/`arr` are the usual culprits
  in `signal/`, `frame/`, and `linalg/`.
- **#264 `abdbbb90` -- unsafe-block capture scan descends into ascription
  nodes.** A previously-silent unsafe capture inside `(:: expr :T)` now fires
  the unsafe-capture diagnostic correctly. Spices with `^fat` lambdas whose
  bodies ascribe a return value (`spices/signal/src/signal/compose.tur` does
  this) need a re-check.
- **#284 `6dabda95` -- prelude-helper diagnostic hints.**
  "unknown function or operator" errors on `min`/`max`/numeric helpers now
  emit a "stdlib/math.tur is a bare load file" hint. Useful but does not
  fix the underlying expressiveness gap -- see
  [the report](../reported/prelude-macros-not-importable-inside-defmodule.md).
- **#272 `c350b804` -- first-class `:fn` finalized.** `:fn` is now the
  recommended carrier for dispatch-erased typeclass receivers (e.g.
  `Comonad.extend`). No spice migration needed yet; flagged for stdlib-adjacent
  spices (`signal`, `frame`).

## Per-spice findings

Audit method: built each spice with `tur build .` (main HEAD `c60ba4ca`).
The "Root cause" column maps each failure to the bucket from the Summary;
"unrelated" failures (missing third-party headers, etc.) are reported because
they block validation of typing work but are not in scope for this plan.

| Spice | Status | Root cause | Notes |
| --- | --- | --- | --- |
| `signal` | FAIL | prelude / `vec-get` unresolved | `src/signal/compose.tur:39` -- `vec-get` not found despite `defmodule`; reproducible without `(load "stdlib/vec.tur")` in this file. Also exercises `^fat` (`compose.tur:20-21`, `core.tur:53,67,82-83`, `shaper.tur`, `filter.tur`, `osc.tur`). After fixing the import, re-run to surface any #286 ABI fallout. |
| `frame` | FAIL | #275 mangling + source codegen quirk | `frame__column.c:842,857,872,887,906,931` -- inline-C calls `frame__column____builder_grow` (old fold of `--builder-grow`); new spelling is `frame__column___un_unbuilder_hygrow`. Also `frame__print.c:496` emits a malformed `#include <stdlib.h> #include <stdint.h> if (h) free(...);` on one line -- the *source* (`print.tur:492`) puts both directives on one inline-C line; the codegen relays it verbatim. Source-side fix; not a codegen bug. |
| `watch` | FAIL | #275 mangling | `watch__watch.c:762,769,772,787,790,804` -- inline-C calls `watch__event__watch_event_free` (old fold of `watch-event-free`) and `watch__watch____watcher_tree_mk_event` (old fold of `--watcher-tree-mk-event`). New spellings: `watch__event__watch_hyevent_hyfree`, `watch__watch___unun_unwatcher_hytree_hymk_hyevent`. Multiple inline-C sites in `src/watch/watch.tur` (the macro `TUR_PEND_PUSH` at line 701, plus the dispatch loop). |
| `tourist` | FAIL | prelude / `when` unresolved | `src/tourist/dsl.tur:164` -- `when` unbound. Also extensive inline-C cons walking in `app.tur`, `routing.tur`, `middleware.tur` (lines 27/34, 55/63/69, 37/47/54) using `__tur_cons` -- structurally fine, but needs revalidation after the prelude fix. |
| `httpd` | FAIL | prelude / `when` unresolved | `src/httpd/server.tur:488` -- `when` unbound. |
| `c-dsl` | FAIL | prelude / `cons` unresolved | `src/c-dsl/builtins.tur:159` -- `cons` unbound. |
| `glsl` | FAIL | prelude / `cons` unresolved | `src/glsl/stdlib.tur:47` -- `cons` unbound. |
| `stats` | FAIL | prelude / `min` unresolved | `src/stats/test.tur:123` -- `min` unbound. |
| `linalg` | FAIL | source bug (WIP file, 4 unmatched opens) | `src/linalg/decomp.tur` has never compiled -- `qr` (lines 185--282) and `lu` (lines 97--169) each have 2 unmatched `(`. Two-commit history confirms it was a WIP draft. Source-side fix: finish or delete the decompositions. Diagnostic-UX residue (caret on outermost form instead of deepest unclosed paren) captured in [`docs/reported/linalg-decomp-qr-parser-unterminated-list.md`](../reported/linalg-decomp-qr-parser-unterminated-list.md). |
| `sdf-raylib` | FAIL | #275 mangling (+ unrelated `raylib.h`) | `sdf__eval.c:64,68,72,76,82,88,91,95,101` -- recursive inline-C calls `sdf__eval__sdf_eval` (old fold of `sdf-eval`); new spelling `sdf__eval__sdf_hyeval`. `raylib/integration.tur` separately needs `raylib.h` on the include path. |
| `math` | PASS | extern-c `malloc` redeclare | `math__vec3.c:17`, `math__vec4.c:15`, `math__mat4.c`, `math__quat.c`, `math__vec2.c` -- `(extern-c malloc [^int size] :int)` conflicts with the system `<stdlib.h>` prototype. Not strictly a typing-wave regression but blocks math's CI; replace with an inline-C `#include <stdlib.h>` block. |
| `ansi` | PASS (warnings) | -- | Compiles; unused-function warning in `ansi__image.c:92`. Safe baseline. |
| `json` | unrelated | missing `yyjson.h` | Cannot reach the typing layer until the header dep is supplied. |
| `regex` | unrelated | missing `pcre2.h` | Same -- skip. |
| `http` | unrelated | missing `mbedtls/*`, `yyjson.h` | Skip. |
| `template`, `plot`, `notebook`, `sqlite`, `raylib`, `postgres`, `valkey`, `osc`, `rtaudio`, `rtmidi`, `wav`, `png`, `plutovg`, `scscm`, `tidal`, `raygui`, `test`, `opengl`, `tls`, `zlib` | not built | -- | Sweep covered the high-signal spices; remaining ones either depend on the above (`plot` depends on `frame`/`math`/`linalg`) or wrap third-party C libraries. Re-run after P1 + P2 land. |

## Phased remediation

The phases mirror the three Summary buckets, then add follow-on validation
and stay-clean gates. Phases are ordered by dependency: P1+P2 must land
before P3 can meaningfully exercise the `^fat` / closure-ABI surface.

### P1 -- Inline-C mangling rewrite sweep (blocks `frame`, `watch`, `sdf-raylib`)

The injective mangler (#275) is the single largest source of breakage. Every
hand-written C identifier that names a Turmeric global must be rewritten.

- [ ] Add a one-shot helper script in `turmeric-spices/scripts/` that walks
      `spices/*/src/**/*.tur`, finds inline-C blocks, and reports identifiers
      matching `<spice>__<mod>__[a-z_]+` so the diff is reviewable by eye.
      (A fully automatic rewrite is risky because some idents really are
      legacy-fold-correct -- e.g. references to `extern-c` C functions, which
      #275 left on the legacy fold.)
- [ ] `spices/watch/src/watch/watch.tur`: rewrite the seven inline-C call
      sites at lines 266, 598, 692, 762, 769, 772, 787, 790, 804 (and any
      sibling sites surfaced by the script) from
      `watch__event__watch_event_free` -> `watch__event__watch_hyevent_hyfree`
      and `watch__watch____watcher_tree_mk_event` ->
      `watch__watch___unun_unwatcher_hytree_hymk_hyevent`.
- [ ] `spices/frame/src/frame/column.tur`: rewrite all inline-C references
      to `frame__column____builder_grow` -> `frame__column___un_unbuilder_hygrow`,
      plus any companion `--*` private helpers.
- [ ] `spices/sdf-raylib/src/sdf/eval.tur`: rewrite recursive inline-C
      `sdf__eval__sdf_eval` -> `sdf__eval__sdf_hyeval`.
- [ ] `spices/frame/src/frame/print.tur:492`: split the fused
      `#include <stdlib.h> #include <stdint.h> if (h) free(...);` inline-C
      body onto separate lines. The codegen is correctly relaying the source
      verbatim; this is a source-side fix.
- [ ] Sweep `spices/tourist/src/tourist/{app,routing,middleware}.tur` and
      `spices/watch/tests/{tree_burst,drain_burst}_test.tur` for any other
      Turmeric-symbol C-name references. The hand-rolled
      `typedef struct { ... } __tur_cons;` blocks are fine (they alias the
      runtime struct shape, not a mangled global).

### P2 -- Prelude import / `(load ...)` fixups

Resolve the "unknown function or operator" failures with a tactical
spice-side fix while the durable compiler-side resolution is tracked in
[`docs/reported/prelude-macros-not-importable-inside-defmodule.md`](../reported/prelude-macros-not-importable-inside-defmodule.md).

- [ ] `spices/tourist/src/tourist/dsl.tur`: add `(load "stdlib/macros.tur")`
      above the `defmodule`, or rewrite the `when` at line 164 to an explicit
      `if`.
- [ ] `spices/httpd/src/httpd/server.tur:488`: same fix -- `when` -> `if`,
      or top-of-file `(load "stdlib/macros.tur")`.
- [ ] `spices/c-dsl/src/c-dsl/builtins.tur:159`: `cons` is a compiler builtin
      in some contexts but unbound here under `defmodule`; either
      `(load "stdlib/list.tur")` or rewrite the literal `(cons x 0)` chain
      via `list-of`.
- [ ] `spices/glsl/src/glsl/stdlib.tur:47`: same `cons` issue, same fix.
- [ ] `spices/stats/src/stats/test.tur:123`: `(load "stdlib/math.tur")` (the
      compiler's own #284 hint points here for `min`/`max`/numeric helpers).
- [ ] After each fix, re-run `tur build .` for that spice and capture any
      *downstream* errors that the prelude fault was masking. The signal /
      tourist / httpd codebases all sit on top of complex `^fat` and inline-C
      machinery, so it is very likely a second round of issues surfaces.

### P3 -- `^fat` and closure-ABI revalidation (`signal`, `frame`, `tourist`, `httpd`)

Once P1 + P2 unblock the compile, exercise the `^fat` + typed-closure paths.

- [ ] `spices/signal/`: full build + tests. The library is the densest user
      of `^fat` (`compose`, `core`, `envelope`, `shaper`, `osc`, `filter`).
      Verify #286 (`int64_t` inline-C params), #287/#293 (int<->ptr<void>
      carrier bridging at SF call sites), and #276 (typed closure invocation)
      all hold. Specifically check that `(fn [^fat sig] ...)` lambdas whose
      bodies ascribe a `:ptr<void>` return value (`compose.tur:39` style) no
      longer trip the #264 unsafe-capture scan.
- [ ] `spices/frame/`: `arrow-export` / `arrow-import` (the Apache Arrow
      C-data-interface bridge, unrelated to `stdlib/arrow.tur`) round-trip
      tests. Confirm the typed-closure ABI hasn't shifted the function-pointer
      slot layout under the hood.
- [ ] `spices/tourist/` and `spices/httpd/`: re-run middleware-composition
      tests; the M8 compose-middleware machinery is the closest analogue to
      `signal`'s `^fat` chaining and is the most likely place a regression
      hides.

### P4 -- Stay-clean gates (typeclass surface + deprecation)

Catch regressions before they ship. Bundles the typeclass/arrow audit and
the TUR-D0001 gate, since both are about preventing future drift, not
unblocking current failures.

- [ ] Grep `spices/*/src/**/*.tur` for `(import stdlib/arrow-class*)` or
      `(load "stdlib/arrow-class.tur")` and rewrite to `stdlib/arrow.tur`.
      Today: no hits, but verified that several spices import `arrow` via the
      bare name (`spices/frame/`, `spices/signal/examples`, `spices/plot/`).
      Confirm that bare `(import arrow)` still resolves to the consolidated
      module.
- [ ] Grep for `definstance Arrow*` / `definstance Functor*` in spices and
      audit each against the post-#290 Category superclass: any spice-defined
      arrow instance needs an accompanying `definstance Category` (or to
      explicitly opt out, per the Kleisli pattern). Today: no spice defines
      its own arrow instance; record the gate so we catch new ones.
- [ ] TUR-W0039 audit: grep for spice `defn arr`, `defn >>>`, `defn first`,
      `defn second`, `defn app`, `defn loop`. None exist today; add a
      sticky note in each spice's `CLAUDE.md` so future authors do not
      regress.
- [ ] TUR-D0001 stay-clean: no spice currently uses the colon-in-fn-type
      form. Add a `--Werror=deprecated` CI step in
      `turmeric-spices/.github/workflows/ci.yml` so any future regression
      fails the build instead of silently warning.

### P5 -- `extern-c` ABI cleanup (`math`)

Out-of-band but blocks `math/`'s CI under newer Apple SDKs.

- [ ] `spices/math/src/math/{vec2,vec3,vec4,mat4,quat}.tur`: replace
      `(extern-c malloc [^int size] :int)` with an inline-C `#include <stdlib.h>`
      block that allocates and returns `(int64_t)(intptr_t)p`. The current
      form conflicts with Xcode 15+'s `__sized_by_or_null` decorated prototype.

### P6 -- Track upstream compiler-side resolutions

These are not spice work but unblock cleaner spice code once they land.

- [ ] Optional reader-side diagnostic improvement: anchor "unterminated
      list" at the deepest unclosed paren instead of the outermost form.
      Tracked in
      [`docs/reported/linalg-decomp-qr-parser-unterminated-list.md`](../reported/linalg-decomp-qr-parser-unterminated-list.md);
      not a blocker for this plan since the `linalg` failure turned out
      to be a source bug.
- [ ] Prelude-as-`defmodule` conversion -- track
      [`docs/reported/prelude-macros-not-importable-inside-defmodule.md`](../reported/prelude-macros-not-importable-inside-defmodule.md).
      Once `stdlib/macros.tur`, `stdlib/math.tur`, etc. become real
      `defmodule`s, revisit P2 and replace each `(load ...)` workaround with
      a proper `(import ... :refer [...])`.

## Validation

For each spice listed above:

1. Pull the post-typing-wave `tur` (either the next prebuilt tarball that
   includes `c60ba4ca` or newer, or rebuild from `main` and point `TUR_BIN`
   at `build/tur`).
2. `cd spices/<name> && tur build .` -- must complete with zero `error:` lines.
   Warnings are acceptable in this phase if they are pre-existing
   "unused function" diagnostics; new TUR-W0039 hits must be acknowledged.
3. `tur run` whatever per-spice test target the spice's `build.tur` exposes
   (most have `tests/*_test.tur`). Capture failures and reroute back through
   P1-P5 as needed.
4. Full sweep: at the repo root,
   `for d in spices/*/; do (cd "$d" && tur build . > /tmp/$(basename $d).log 2>&1 || echo "FAIL $d"); done`
   should print no `FAIL` lines (excluding the `requires.*` skip set:
   `json`, `regex`, `http`, and `linalg` until its parser report is
   resolved).
5. Once green, refresh `scripts/install-tur.sh`'s default `TUR_VERSION` to
   the tag containing the typing wave.

## Out of scope

- Converting bare `stdlib/macros.tur`, `stdlib/math.tur`, `stdlib/bits.tur`,
  etc. into proper `defmodule`s with `(export ...)` lists -- tracked in
  [`docs/reported/prelude-macros-not-importable-inside-defmodule.md`](../reported/prelude-macros-not-importable-inside-defmodule.md).
- Finishing or deleting the WIP `linalg/decomp.tur` -- it is a
  spice-side source bug. The diagnostic-UX residue is captured in
  [`docs/reported/linalg-decomp-qr-parser-unterminated-list.md`](../reported/linalg-decomp-qr-parser-unterminated-list.md).
- Migrating spice `signal/` off `^fat` carriers and onto first-class `:fn`
  carriers (#272). The two ABIs coexist; the migration is an ergonomics win,
  not a correctness gate.
- Replacing `spices/json/`, `spices/regex/`, `spices/http/` third-party
  header dependencies. These failures are install/SDK issues, not typing-wave
  regressions.
