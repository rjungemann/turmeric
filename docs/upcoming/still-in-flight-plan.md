---
title: Still-in-Flight Plans -- Outstanding Tasks
category: Planning
description: Consolidated outstanding work from in-flight plans that survived the post-v0.18.0 archive churn. Each section is a short punch list with a link back to the full plan in docs/archive/.
---

# Still-in-Flight Plans -- Outstanding Tasks

Each section below tracks one plan that is still active after the
post-v0.18.0 archive sweep. The full design lives in
`docs/archive/`; the bullets here are just the remaining work.

When a section's punch list is empty, move the plan from
`docs/archive/` to `docs/archive/history/` (per the churn-docs skill)
and delete the section from this file.

---

## Typed closure invocation ABI

> Full plan: [../archive/closure-typed-invocation-abi-plan.md](../archive/closure-typed-invocation-abi-plan.md)

Companion to the representation-unification plan; threads declared
arg/return types all the way to the C invocation site.

- [x] Phase 1 -- `TUR_APPLY{0..4}_T(R, A_i..., f, args...)` macros
      emitted (`emit_module.c`); legacy `TUR_APPLY{0..4}` are int64
      aliases of the `_T` forms; signature-keyed typed fat-shims
      (`ensure_typed_fatshim`) and the typed `__tur_poly_to_fat`
      generalisation (`ensure_typed_poly_to_fat`) land.
- [x] Phase 2 -- inline-C lowering, typeclass dispatch, and
      `EX_FN_TO_FAT`/`EX_POLY_TO_FAT` emit typed calls keyed off
      `result_full_type` + `param_types` via
      `ensure_typed_thunk_typedef` / `use_typed_thunk_abi`.
- [x] Phase 3 -- stdlib migrations: `httpd.tur` `mw-cors` (`^fat`)
      and `list.tur` `__cons-fmap` (`^fat` + direct call) done; the
      `seq-call-fn{0,1,2}` / `seq-call-bool-fn1` helpers (in
      `stdlib/seq/`, not `select.tur`) now dispatch through
      `TUR_APPLY0/1/2` and `TUR_APPLY1_T(bool, ...)` instead of
      bespoke slot-0 casts.
- [ ] Phase 4 -- migrate `turmeric-spices/signal` (`__signal_call1`
      returns `:float` and drops `int64_t sig_val; memcpy(...)`
      blocks). Sibling-repo work; deferred until the
      `../turmeric-spices` checkout is present.
- [x] Round-trip fixtures cover `:float`, `:ptr`, `:bool` returns and
      a `:cstr` argument (`tur-apply-t-fatshim-float`,
      `poly-to-fat-float-roundtrip`, `bare-fat-float-result`,
      `option-result-c-abi`, ...).

## `defmodule` per-file scoping

> Full plan: [../archive/defmodule-per-file-scoping-plan.md](../archive/defmodule-per-file-scoping-plan.md)

The `tur/zlib` workaround shipped (M6 dropped `defmodule`); the
diagnostic-scope fix itself is not implemented.

- [x] D0 -- happy fixture `tests/fixtures/elab-defmodule-after-load/`
      + negative fixture `tests/fixtures/errors/elab-defmodule-not-first/`.
- [x] D1 -- per-file form boundaries tracked via `span.file_id` on
      each form (no side array needed; the existing span field serves as
      the file-of-origin key).
- [x] D2 -- check at `elab_toplevel.c` rewritten to operate per file
      using `span.file_id`; continues past first defmodule so later
      loaded files are also validated.
- [x] D3 -- M7 reset generalised: fires at every `span.file_id`
      boundary in the user range, not just after auto-stdlib defmodules.
- [ ] D4 -- restore `(defmodule tur/zlib ...)` in
      `../turmeric-spices/spices/zlib/src/tur/zlib.tur`; verify
      `tests/fixtures/httpd-mw-compress/` and the spice roundtrip
      test still compile. (Deferred: requires `../turmeric-spices` checkout.)
- [x] D5 -- `docs/guides/module-system-guide.md` updated to clarify
      "the file" means the source file; `(load ...)`-spliced files get
      a fresh scope for the check.

## Drop leading colons inside `(fn ...)` types

> Full plan: [../archive/fn-type-bare-identifier-plan.md](../archive/fn-type-bare-identifier-plan.md)

Outer-annotation analogue ([spaced-type-annotation-migration-plan](#spaced-type-annotation-migration))
is complete; this is the inner-type-expression version.

- [x] Phase 1 -- parser accepts bare-identifier types inside
      `(fn ...)` everywhere; regression fixture
      `tests/fixtures/fn-type-bare-identifier/` covers unary,
      binary, nullary, curried, mixed, parenthesised inner, declared
      user type. (Fixture present and green in `tests/run.sh`.)
- [~] Phase 2 -- `tools/rewrite_fn_type_colons.py` codemod written
      (position-driven: defn/fn params + return, defstruct fields,
      let bindings, defalias, method sigs, `(:: _ T)`/`(the T _)`
      ascriptions, nested `(fn ...)`; handles `:kw`, bare `: (T)`,
      and effect sets; never touches lambda values). Corpus regression
      harness at `tests/codemod/run-fn-type-colons.sh`. **Swept:**
      all of `stdlib/` (`either`, `gadt-vec`, `list`, and `arrow`) and
      `docs/guides/`; `python3 tools/rewrite_fn_type_colons.py --check
      stdlib/ docs/guides/` is now clean.
      **Remaining:** `tests/fixtures/` sources are intentionally left
      on the legacy colon spelling until Phase 4 (they back the
      lenient-path coverage), and the lone legacy form left under
      `docs/upcoming/` is the intentional before/after illustration in
      `v1/fn-type-colons-sweet-exp-plan.md` (do not rewrite it). The
      rewrite is annotation-spelling only, so codegen snapshots are
      byte-identical. **Known gap:** the codemod only finds fn types
      inside paren-delimited declarations, so sweet-exp top-level forms
      (`.tur.sweet` files, ```sweet-exp doc blocks) are skipped -- port
      the `spaced-types-rewrite.py` implicit-sequence walk before
      sweeping those.
- [x] Phase 3 -- `TUR-D0001` deprecation warning fires when a leading
      colon (the fused `F_KEYWORD` `:int` form **or** the spaced-but-
      redundant `F_TYPE_ANN` `: int` form) is consumed in a `(fn ...)`
      type param/result slot (`warn_fn_type_colon` in
      `src/compiler/elab_types.c`). Default DIAG_WARNING; promoted to a
      hard error under `--Werror=deprecated` (reuses `g_werror_deprecated`,
      mirroring `^deprecated` use sites). Does **not** fire on bare
      identifiers, on the `(-> ...)` arrow type, or on value-position
      `(fn [x] ...)` lambdas. `tur explain TUR-D0001` carries the
      migration hint. Coverage: happy fixture
      `tests/fixtures/fn-type-colon-deprecation/` (warns + still runs)
      and negative fixture `tests/fixtures/errors/fn-type-colon-werror/`
      (`--Werror=deprecated` -> hard error).
- [ ] Phase 4 -- remove the `F_KEYWORD`/`F_TYPE_ANN` colon branch from
      the `(fn ...)` type-expression handler (delete
      `warn_fn_type_colon` and reject instead); sweep the remaining
      legacy spellings in `tests/fixtures/` inputs, macro expansions
      (`contract.tur`/`macros.tur`), and `../turmeric-spices/`; convert
      the lenient-path fixture `tests/fixtures/fn-type-bare-identifier/`
      into an `errors/` fixture asserting the hard diagnostic. Gated on
      at least one minor release with the warning live.

## HTTPD compression + `tur/zlib` spice

> Full plan: [../archive/httpd-compression-zlib-spice-plan.md](../archive/httpd-compression-zlib-spice-plan.md)

Track Z (spice) and Track M6 (`stdlib/httpd-compress.tur`) both
unstarted.

- [ ] Z0 -- scaffold `../turmeric-spices/spices/zlib/` mirroring the
      `png`/`json` layout; pin upstream zlib via `:cmake-deps`.
- [ ] Z1 -- `src/zlib.tur` exposes `gzip-encode` / `gzip-decode` /
      `deflate-raw` / `inflate-raw` + `GzipBuf` accessors over
      `:ptr<void>` (binary-safe).
- [ ] Z2 -- `tests/zlib-roundtrip/` fixture (encode large input,
      decode, byte-for-byte equality).
- [ ] Z3 -- spice README.
- [ ] M6-0 -- `stdlib/httpd-compress.tur` scaffold loading
      `tur/zlib` with a clear load-time error if the spice is
      missing.
- [ ] M6-1 -- `mw-compress` factory: parse `Accept-Encoding`, gate
      on `min-bytes` threshold (default 256; revisit per OQ3),
      compress + set `Content-Encoding`.
- [ ] M6-2 -- spice-gated fixture (`requires.spices` marker).
- [ ] M6-3 -- brief docs entry.
- [ ] Resolve OQs: `(load "tur/zlib")` vs `(import ...)` spice
      form (M6-OQ1); `httpd-resp-body-bytes!` setter vs
      length-prefixed `httpd-resp-body!` (M6-OQ2); default
      `min-bytes` threshold (M6-OQ3).

## `tur-signal` spice broken build

> Full plan: [../archive/history/signal-spice-broken-build.md](../archive/history/signal-spice-broken-build.md) *(resolved)*

Spice does not currently compile; Phase 0 of the expansion plan
needs to be split.

- [ ] Phase 0a -- restore the build. Either reintroduce
      `__arrow_call1` as a stdlib-side fat-dispatch shim, or rewrite
      every call site to direct closure invocation `(sv t)` plus
      `^fat` annotation; add missing `(import ...)` forms to
      `synth.tur` and `tests/signal/arrow_tests.tur`. Validation:
      `./build/tur check` clean on every spice source + test file.
- [ ] Phase 0b -- closure-ABI cleanup: replace raw
      `int64_t(*)(int64_t)` inline-C casts in `dsp.tur` with
      fat-dispatch helpers (`TUR_APPLY1` or typed Turmeric
      trampoline).
- [ ] Phase 0c -- `:float` sample migration (the originally-named
      Phase 0). Gate on compiler support for `:float`-returning
      closures through fat dispatch.
- [ ] Confirm DSP filter tests (`low-pass` / `high-pass` / `gain`)
      run a multi-sample sequence rather than a single dispatch.

## Spaced-type annotation migration

> Full plan: [../archive/history/spaced-type-annotation-migration-plan.md](../archive/history/spaced-type-annotation-migration-plan.md)

Phases 1-5 are complete (named-let, let/let*, codemod, repo + spices
rewrite, docs). Only enforcement and optional deprecation remain.

- [x] Phase 6 -- CI step added (`.github/workflows/ci.yml`
      `check-spaced-types` job) running
      `tools/spaced-types-rewrite.py --check stdlib/ docs/guides/`.
      Three remaining stdlib files (`httpd.tur`, `comonad.tur`,
      `docstrings.tur`) were swept clean before the gate was added.
      Two compiler bugs surfaced and fixed in the same pass:
      (a) pass-1 forward-decl didn't recognise `: nil`
      (`F_TYPE_ANN(F_NIL)`) as `TY_NIL` -- fixed in `elab_toplevel.c`;
      (b) `defclass` method-param parsing didn't recognise `: fn`
      (`F_TYPE_ANN(F_SYM("fn"))`) as the poly-closure carrier -- fixed
      in `elab_typeclasses.c`. Mirror the hook in `../turmeric-spices`
      and lift the codemod into `tur fmt` remain optional/deferred.
- [ ] Phase 7 (optional, deferred) -- once the ecosystem is fully
      migrated and CI prevents regressions, emit a deprecation
      warning for fused `F_KEYWORD` in recognised type-annotation
      positions. Not committed; revisit after several release
      cycles.

