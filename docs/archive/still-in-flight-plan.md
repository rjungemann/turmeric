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

> Full plan: [../archive/history/fn-type-bare-identifier-plan.md](../archive/history/fn-type-bare-identifier-plan.md)

Outer-annotation analogue ([spaced-type-annotation-migration-plan](#spaced-type-annotation-migration))
is complete; this is the inner-type-expression version.

- [x] Phase 1 -- parser accepts bare-identifier types inside
      `(fn ...)` everywhere; regression fixture
      `tests/fixtures/fn-type-bare-identifier/` covers unary,
      binary, nullary, curried, mixed, parenthesised inner, declared
      user type. (Fixture present and green in `tests/run.sh`.)
- [x] Phase 2 -- `tools/rewrite_fn_type_colons.py` codemod written
      (position-driven: defn/fn params + return, defstruct fields,
      let bindings, defalias, method sigs, `(:: _ T)`/`(the T _)`
      ascriptions, nested `(fn ...)`; handles `:kw`, bare `: (T)`,
      and effect sets; never touches lambda values). Corpus regression
      harness at `tests/codemod/run-fn-type-colons.sh`. **Swept:**
      all of `stdlib/`, `docs/guides/`, AND `../turmeric-spices/` (the
      last stray in `developing-spices-guide.md` rewritten 2026-06-10);
      `python3 tools/rewrite_fn_type_colons.py --check stdlib/ docs/guides/`
      is clean and the same check is clean against `../turmeric-spices/`.
      **Intentionally left:** `tests/fixtures/` sources keep the legacy
      colon spelling until Phase 4 (they back the lenient-path coverage),
      and the lone legacy form under `docs/upcoming/v1/fn-type-colons-
      sweet-exp-plan.md` is the intentional before/after illustration
      (do not rewrite it). The rewrite is annotation-spelling only, so
      codegen snapshots are byte-identical. **Sweet-exp coverage (done
      2026-06-10):** the codemod now carries the
      `spaced-types-rewrite.py`-style implicit-sequence walk, so sweet-exp
      top-level forms are rewritten too -- enabled by a `#lang sweet-exp`
      directive, for every `.tur.sweet` file, and for ```sweet-exp markdown
      fences. Corpus cases `sweet-basic`/`sweet-effects`/
      `sweet-lambda-untouched`/`sweet-md` under
      `tests/codemod/fn-type-colons/` cover it (including the
      "missed, never corrupted" guarantee for a nested lambda value), and a
      re-sweep of `docs/guides/`/`stdlib/` is a clean no-op. Type forms
      reachable only by deeper indentation than the top-level scan visits
      are still missed (never corrupted); revisit only if a real source
      needs it. See
      [`fn-type-colons-sweet-exp-plan.md`](../upcoming/fn-type-colons-sweet-exp-plan.md).
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
- [x] Phase 4 -- removed the `F_KEYWORD`/`F_TYPE_ANN` colon branch from
      the `(fn ...)` type-expression handler (`warn_fn_type_colon` ->
      `reject_fn_type_colon` in `src/compiler/elab_types.c`; still emits
      TUR-D0001 so `tur explain` keeps working, but now unconditionally
      `DIAG_ERROR`). Swept the remaining legacy spellings in
      `tests/fixtures/` inputs via the existing codemod plus two manual
      fixes for ascription-position fn types (`(:: _ (fn ...))`) the
      codemod missed (`arrow-instance-nullary`, `fat-closure-ascription`).
      `contract.tur`/`macros.tur` already had no fn-type colon spellings;
      `../turmeric-spices/` was swept in Phase 2.  Repurposed the lenient
      fixture: removed `tests/fixtures/fn-type-colon-deprecation/`
      (warning gone), renamed `errors/fn-type-colon-werror/` ->
      `errors/fn-type-colon-rejected/` with the legacy spelling restored
      and `--Werror=deprecated` dropped from flags. Plan archived to
      [`../archive/history/fn-type-bare-identifier-plan.md`](../archive/history/fn-type-bare-identifier-plan.md).

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

