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

