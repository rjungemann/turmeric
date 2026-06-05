---
title: docs/guides Refresh for Post-v0.17 Compiler + Stdlib Changes
category: Planning
description: Audit and remediation plan for `docs/guides/` after the closure-ABI, name-mangling, fn-type colon deprecation, arrow consolidation, Category/Kleisli, definstance idempotence, and TUR-W0039 changes landed between `v0.17.0` and current `main`. Identifies stale snippets, dangling references, missing coverage, and lays out a phased rewrite with validation steps.
---

# `docs/guides/` Refresh for Post-v0.17 Compiler + Stdlib Changes -- Plan

## Summary

`v0.17.0` was tagged before the wave of closure/typing/mangling work that
shipped on `main` over the past ~50 commits (`git log v0.17.0..HEAD --oneline`).
Most of the user-visible guides under `docs/guides/` still read against the
pre-wave compiler: a handful of guides reference removed/split stdlib modules,
several others use the now-deprecated `(fn [:T] :U)` colon-in-fn-type syntax in
narrative prose, the `^fat` guide and the C-integration guide pre-date the
"emit `^fat` params as `int64_t` in inline-C" rule (#286) and the unified
closure representation (#276/#283/#285/#287/#292/#293), and the
`name-mangling-guide` largely already covers reversibility but needs a sanity
pass for downstream knock-ons (inline-C examples elsewhere). None of these are
codegen-correctness regressions in the *guides themselves*, but several are
BLOCKER-grade for a reader who tries to copy-paste a snippet against current
`tur` and gets a TUR-D0001, a link error, or wrong inline-C scaffolding.

This plan inventories the affected guides, classifies severity, and stages a
mechanical-first / narrative-second / ABI-third rewrite that can land
incrementally without holding a release gate.

## Compiler / stdlib changes affecting guides (v0.17.0..HEAD on `main`)

Each bullet lists merge SHA and the user-visible guide surface it touches.

- **#270 `1a4bff30` -- TUR-D0001: leading colons inside `(fn ...)` types
  deprecated (Phase 3).** Codemod (`bf3445e5`) swept stdlib and most docs,
  but at least one guide example still uses the deprecated form. The
  codemod is shipping in the box, so any prose example that documents the
  *type syntax* itself must show the new form.
- **#275 `77e73c9e` -- Reversible / injective Turmeric-to-C name mangling.**
  `name-mangling-guide.md` was rewritten with this change. Knock-on: every
  guide that hand-mangles a Turmeric global inside an inline-C example
  (notably `c-integration-guide.md`, `developing-spices-guide.md`,
  `fat-closure-annotation-guide.md`, `parser-combinators-tutorial.md`)
  needs a verification pass against the new `_hy` / `_sl` / `_un` scheme.
- **#276 `839b31ab` (+ #283 `af714eaf`, #285, #286 `5dccd210`, #287
  `9c9a90bf`, #292 `2ac1265e`, #293 `4898adce`, `9588cda7`) -- Closure
  representation unification, typed closure invocation, int<->ptr<void>
  carrier bridging.** Touches `fat-closure-annotation-guide.md`,
  `arrows-guide.md`, the closure-heavy sections of `c-integration-guide.md`
  and `typeclass-internals-guide.md`. Most of the "you must annotate
  non-int returns" guidance still holds, but the rationale, the failure
  mode, and the ABI carrier diagram need a refresh against the unified
  representation.
- **#286 `5dccd210` -- `^fat` params emitted as `int64_t` in inline-C
  function bodies.** Directly contradicts the `fat-closure-annotation-guide`
  / `c-integration-guide` prior phrasing that `^fat` params were opaque
  to inline-C; we now have a concrete C type to spell.
- **#277 `1b17d582` -- Arrow surface re-consolidated into a single
  `stdlib/arrow.tur` (no more `stdlib/arrow-class.tur`).** `arrows-guide.md`
  already documents the "two surfaces in one module" story but needs a
  light pass for any leftover `arrow-class` mentions and to confirm the
  import line and the typeclass table.
- **#290 `21d11393` -- Category typeclass + honest Kleisli ArrowZero.**
  `arrows-guide.md` already references Category / Kleisli but should be
  cross-checked against the shipped module surface; `polymorphism-guide.md`
  and `typeclass-internals-guide.md` may want a pointer.
- **#278 `3aee822c` -- `definstance` idempotent (reload-safe).** Nothing
  guide-visible breaks, but `repl.md` and `typeclass-internals-guide.md`
  should mention it so reload-loop users stop being surprised.
- **#266 `4e34c13b` -- TUR-W0039 method-vs-defn name clash warning;
  #267 `404a58c2` method-vs-defn coexist namespace fix.** The arrows-guide
  already links to the report; the typeclass-internals / polymorphism
  guides should grow a "TUR-W0039 when you shadow a typeclass method"
  callout.
- **#272 `c350b804` -- First-class `:fn` finalized.** Plan archived. The
  `fat-closure-annotation-guide`, `typeclass-internals-guide`, and any
  guide that talks about "function values" should be checked for stale
  "this doesn't work yet" hedges.
- **#264 `abdbbb90` -- Unsafe-block capture scan descends into ascription.**
  Niche; only affects `c-integration-guide.md` / `developing-spices-guide.md`
  if they happen to claim ascription hides a capture.
- **#291 `d4635a21` -- Recursive defn return type in defmodule
  (F_TYPE_ANN unwrap).** Niche; check `module-system-guide.md` for any
  "you can't recurse with an explicit return type inside defmodule"
  caveat that is now stale.
- **`81202be5` -- Range/GADT typeclass migration A2 flag-strip.**
  `gadts-guide.md` / `gadts-cookbook.md` should be checked for any
  experimental-flag prose.
- **#280 `2918cf32` -- stdlib-session-typed-channels plan complete and
  archived.** `session-types-guide.md` is the canonical doc now; any
  remaining "see the plan" cross-link should retarget to the guide, and
  conversely the archived plan should backlink to the guide.

## Per-guide findings

Findings come from `grep -rn '(fn \[:' docs/guides/`, `grep -rln '\^fat'
docs/guides/`, a `git log v0.17.0..HEAD -- docs/guides/` review, and a
manual read of the highest-touch files.

| Guide | What's stale / wrong / missing | Driving change | Severity |
|-------|--------------------------------|----------------|----------|
| `arrows-guide.md` | Already updated for "two surfaces in one module" and `Category` / `Kleisli`. Cross-check: any lingering `stdlib/arrow-class.tur` references (none found in the file itself, but the README still routes here so the README's tagline should reflect Category). | #277, #290 | MINOR |
| `fat-closure-annotation-guide.md` | (a) Pre-dates #286: should add an explicit "inside inline-C bodies, `^fat` params are visible as `int64_t`" subsection with an example. (b) "result type" section #4a still phrases the bare-`^fat` carrier in pre-unification terms; should be re-anchored on the unified-representation language used in the typed-closure work. (c) The "future-work: bare-fat non-tail result" pointer at line ~160 should be re-checked against the latest v1 plan path. | #276, #283, #285, #286, #287, #292, #293, `9588cda7` | MAJOR |
| `c-integration-guide.md` | Inline-C examples around closures, the cons-cell walker, and the "calling a Turmeric global from C" section all need a verification pass against (i) the reversible mangler (`_hy`/`_sl`/`_un`), (ii) `^fat` params being `int64_t` in inline-C, (iii) the unified closure handle. The "function values" / "callbacks from C" prose may carry stale hedges from before first-class `:fn` shipped. | #275, #276, #286, #272 | MAJOR |
| `typeclass-internals-guide.md` | One concrete bug: line 55 still spells a curried return as `(fn [:int] (fn [:int] :int))` (deprecated colon-in-fn-type form). Should also (a) describe `definstance` idempotence (#278), (b) document TUR-W0039 when a method shadows a free `defn`, (c) describe the closure-handle carrier story that ships post-#276. | #270 (TUR-D0001), #276, #278, #266 | BLOCKER (the colon example contradicts the deprecation it teaches) |
| `polymorphism-guide.md` | Add a Category/Kleisli pointer and a TUR-W0039 callout. Verify the typeclass-resolution example still matches post-#278 idempotent `definstance` semantics. | #290, #266, #278 | MINOR |
| `name-mangling-guide.md` | Already rewritten under #275. Sanity-pass only: confirm the demangler section's referenced fixtures still exist (they do), confirm there's a forward link to the `c-integration-guide`'s inline-C calling-convention section. | #275 | NIT |
| `session-types-guide.md` | The stdlib-session-typed-channels plan is now archived (#280, `2918cf32`). The guide already documents `stdlib/schan.tur`; what's missing is a "this guide supersedes the archived `stdlib-session-typed-channels-plan`" note + retargeting any older cross-links elsewhere in the tree. | #280 | MINOR |
| `parser-combinators-tutorial.md` | Heavy `^fat` consumer (15+ mentions). Needs the same post-#286 pass as the fat-closure guide: explicit `int64_t` callout for inline-C, re-anchored prose around the unified representation. The `ptr<void>` / `^fat :ptr<void>` annotation pattern in sections 5+ should be reverified. | #276, #283, #285, #286, #287, #292, #293 | MAJOR |
| `developing-spices-guide.md` | Inline-C interop and any "calling a spice's exported function from C" snippet needs the reversible-mangler pass. Add a cross-link to `name-mangling-guide.md` for the reversible scheme rules. Should also mention TUR-D0001 so spice authors don't reintroduce colon-in-fn-type forms. | #275, #270 | MAJOR |
| `module-system-guide.md` | Verify whether any "explicit return type on a recursive `defn` inside `defmodule`" caveat is still stated; if so, remove it (fixed by #291). | #291 | MINOR |
| `gadts-guide.md`, `gadts-cookbook.md` | Check for any "behind an experimental flag" or "needs `-Xrange-gadt`" prose that no longer applies post-`81202be5`. | `81202be5` | MINOR |
| `repl.md` | Add a one-line note that `definstance` is now idempotent so `(reload)` no longer double-registers an instance. | #278 | NIT |
| `frame-guide.md` | Uses `arrow-export` / `arrow-import` / `arrow-export-column` (the Apache-Arrow interop API, not the typeclass arrow). Not a stale-API hit, but the prose may want a one-line disambiguation now that `arrows-guide.md` is the canonical "arrows" doc. | (none -- disambiguation only) | NIT |
| `README.md` (guides index) | Tagline for `fat-closure-annotation-guide.md` and the arrows entry should reflect Category/Kleisli additions. Add a row pointing at `name-mangling-guide.md` if missing from the index. | #275, #290 | MINOR |
| `effects-system-guide.md`, `error-handling-guide.md`, `compiler-flags-guide.md`, `httpd-middleware-guide.md`, `httpd-async-guide.md`, `opaques-guide.md`, `substructural-types-guide.md`, `sum-types-guide.md` | Touched since v0.17.0 by content commits (not refactor sweeps). No blocker patterns found in spot checks; flag for read-through-only verification under P4. | (mixed) | NIT |

### Mechanical pattern audit (`grep` results)

- **`(fn [:` deprecated-syntax hits in `docs/guides/`:**
  one hit, `typeclass-internals-guide.md:55` (`(fn [:int] (fn [:int] :int))`). Fix mechanically.
- **`stdlib/arrow-class.tur` / `arrow-class` references:**
  none in guide bodies (only in the archived `stdlib-arrow-typeclass-reintroduction-plan.md`, which is correct). No remediation needed.
- **`parse-first-arg` / `parse-arg` deprecated CLI patterns:**
  none in `docs/guides/`. `cli-args-guide.md` already centers on `*args*` and `stdlib/args.tur`. No remediation needed.
- **`^fat` mentions:**
  three guides (`parser-combinators-tutorial.md`, `fat-closure-annotation-guide.md`, `README.md`). All need the post-#286 `int64_t` callout. `README.md` only needs a tagline tweak.
- **`TUR-W0039` mentions in guides:**
  none. Should be referenced from `typeclass-internals-guide.md` and `polymorphism-guide.md`.
- **TUR-D0001 mentions in guides:**
  none. At minimum `typeclass-internals-guide.md`, `developing-spices-guide.md`, and the syntax/type-annotations guides should call it out (or link to `style-guide.md` which can host the rule).

## Phased plan

### P1 -- Mechanical sweep (low-risk, scriptable)

- Run the fn-type colon codemod (`bf3445e5`) over `docs/guides/` and confirm
  the one known hit (`typeclass-internals-guide.md:55`) plus any others
  the codemod surfaces are rewritten.
- Re-grep `(fn \[:` and confirm zero hits across `docs/guides/`.
- Update `docs/guides/README.md` taglines for arrows (mention Category/Kleisli)
  and fat-closures (mention `int64_t` in inline-C).

Exit gate: `grep -rn '(fn \[:' docs/guides/` is empty; `tur check` on extracted
guide snippets (see Validation) reports no TUR-D0001 warnings.

### P2 -- Arrow / typeclass narrative pass

- `arrows-guide.md`: read-through against `stdlib/arrow.tur` and
  `stdlib/kleisli.tur` heads; reconcile any drift, confirm the "two surfaces"
  table matches what's exported today.
- `polymorphism-guide.md`, `typeclass-internals-guide.md`: add Category/Kleisli
  pointers, the TUR-W0039 callout, the `definstance` idempotence note.
- `repl.md`: one-line `(reload)` + idempotent-`definstance` note.

Exit gate: the typeclass-internals guide describes the post-#276 closure-handle
story and the post-#278 idempotence story; arrows-guide and polymorphism-guide
cross-link each other on Category.

### P3 -- Closure ABI / inline-C / `^fat` rewrite

- `fat-closure-annotation-guide.md`: add the explicit "`^fat` params are
  visible as `int64_t` inside inline-C bodies" subsection (#286). Re-anchor
  section #4a on the unified-representation language. Verify the deferred-work
  cross-link still points at a live v1 plan.
- `c-integration-guide.md`: verify every inline-C example against the
  reversible mangler; add an explicit "calling a Turmeric global from inline-C"
  worked example using the new `_hy`/`_sl`/`_un` spelling; add a "callbacks
  that take a `^fat` param look like `int64_t f` in C" example. Remove any
  stale first-class-`:fn` hedges (#272).
- `parser-combinators-tutorial.md`: spot-check all `^fat` sections against
  the unified-rep language; no behavioral change expected, but the rationale
  prose needs to stop describing the old split-representation failure mode.
- `developing-spices-guide.md`: add a "TUR-D0001: do not write
  `(fn [:T] :U)`" callout and a cross-link to `name-mangling-guide.md` for
  inline-C interop.

Exit gate: every inline-C code block in the four guides above is verified
against the current mangler and the `^fat` -> `int64_t` rule; a manual
`tur check` of representative blocks passes.

### P4 -- Cross-link and read-through pass

- Retarget any `stdlib-session-typed-channels-plan` cross-link in the tree
  to `docs/guides/session-types-guide.md`; add a "see the guide" note to the
  archived plan's frontmatter.
- Read-through `effects-system-guide.md`, `error-handling-guide.md`,
  `compiler-flags-guide.md`, `httpd-*-guide.md`, `opaques-guide.md`,
  `substructural-types-guide.md`, `sum-types-guide.md`,
  `module-system-guide.md`, `gadts-*.md` for any prose that still hedges on
  removed flags or unfinished features. Fix what's stale, leave what still
  applies.
- Confirm `README.md` index entries reflect the post-wave taglines.

Exit gate: zero dangling references to archived plans from any guide; zero
"experimental flag" prose for features that have shipped.

## Validation

- **TUR-D0001 hit count.** `grep -rn '(fn \[:' docs/guides/` must return
  zero after P1. CI doesn't yet lint Markdown for this; this is a manual gate.
- **Snippet check.** Extract fenced Turmeric code blocks from the four MAJOR
  / BLOCKER guides (`fat-closure-annotation-guide.md`,
  `c-integration-guide.md`, `typeclass-internals-guide.md`,
  `parser-combinators-tutorial.md`) into scratch files and run `./build/tur
  check` on each. The tutorial blocks may legitimately reference symbols not
  in stdlib; treat unresolved-name as informational, treat TUR-D0001 / parse
  errors / type errors as failures.
- **Link check.** Run a Markdown link checker against `docs/guides/`. Pay
  attention to references to `docs/upcoming/` plans: every link should hit
  either a live `docs/upcoming/` doc or a `docs/archive/` doc; no 404s. The
  session-typed-channels archival is the canonical example to verify.
- **Round-trip the mangler claims.** Pick one or two inline-C examples from
  `c-integration-guide.md` that call a Turmeric global by mangled name, copy
  them into a scratch fixture, build, and confirm they link. This catches
  the case where a guide example was written against the pre-#275 fold and
  is now spelling a non-existent C symbol.
- **Read-through against `CLAUDE.md`.** `CLAUDE.md` mandates the no-interior-
  colons rule and the `*args*` / `stdlib/args.tur` rule; the guides must
  agree.

## Out of scope

- HTML mirror under `docs/guides/*.html` (regenerated by `tur run docs`; no
  manual edits).
- Tutorial-grade overhaul of `quickstart.md`, `snake-game-tutorial.md`,
  `repl-tutorial.md`, etc. -- these are not affected by the v0.17..HEAD
  changes in any obvious way and a tutorial rewrite is its own project.
- Reorganizing the `docs/guides/` taxonomy. The plan is a content refresh,
  not a restructure.

## Pre-flight checklist for whoever picks this up

- [ ] `git fetch origin main` and re-baseline against current `HEAD`.
- [ ] Re-run `grep -rn '(fn \[:' docs/guides/` to catch new hits added
      since this plan was written.
- [ ] Re-run `git log v0.17.0..HEAD -- docs/guides/` to see what's already
      been touched and merge with this plan's per-guide table.
- [ ] Decide whether to ship as one PR (small) or one per phase (cleaner
      review).
