---
title: Migrate range.tur onto the Bound GADT (default -Xgadt + typeclasses)
category: Planning
description: Fold stdlib/range-bound.tur's `Bound A` GADT into range.tur's internal endpoint representation, replacing the malloc'd `{bool; int64_t}` RangeBound sentinel and the hand-rolled `range->str` / equality predicates. Two prerequisites are graduated here: making `-Xgadt` default-on (so range.tur can use `defgadt` without forcing the flag on every consumer) and adding `Show`/`Eq`/`Ord` typeclass instances for `Range` and `Bound`. Unblocked by `defgadt :copy` (shipped 2026-06-04).
---

# Migrate `range.tur` onto the `Bound` GADT -- Plan

> **Type:** stdlib internal-representation migration + experimental-flag graduation
> **Prerequisite (met):** `defgadt :copy` -- shipped 2026-06-04, see
> [docs/reported/defgadt-copy-and-shared-bounds.md](../reported/defgadt-copy-and-shared-bounds.md).
> **Parent:** phase R3 of
> [stdlib-refinement-collections-plan](./stdlib-refinement-collections-plan.md).

## Why

Phase R3 of the refinement-collections plan introduced the `Bound A` GADT
(`Inclusive` / `Exclusive` / `Unbounded`) in `stdlib/range-bound.tur` as an
**opt-in layer**: a pair of bidirectional shims (`bound->range-bound` /
`range-bound->bound`) bridges the GADT to `range.tur`'s untouched pointer
representation. R3 deliberately did **not** rewrite `range.tur`'s internals,
for two reasons recorded in its deviations note:

1. **Move tracking.** GADT values were affine, but range bounds are *shared* --
   a single bound pointer is read by `range-bound-inclusive?`,
   `range-bound-value`, stored in a `Range`, and re-read by every predicate.
   **-- Resolved.** `defgadt :copy` now makes a GADT a plain, freely-readable
   value type. `Bound` in `range-bound.tur` is already declared `:copy`.
2. **The `-Xgadt` flag.** Putting `defgadt Bound` in `range.tur` would force
   `-Xgadt` on every range consumer -- including the `for` macros and the
   `#r{...}` range-literal reader -- since `range.tur` is auto-loaded
   transitively by a large fraction of stdlib. This is the remaining blocker
   and the first half of this plan.

With (1) closed, the only thing standing between `range-bound.tur`'s GADT and
`range.tur`'s internals is the flag gate. Once `-Xgadt` is default-on, the
sentinel-int `RangeBound` struct, its four inline-C accessors, and the
stringly-typed `inclusive : int` constructor argument can be replaced by the
GADT plus `match`; and the ad-hoc formatting/equality helpers can be replaced
by `Show` / `Eq` / `Ord` typeclass instances. That is the second half.

## Goals

- `-Xgadt` becomes default-on (graduated from experimental), so `defgadt` and
  GADT pattern matching work without a flag. The flag name stays accepted as a
  deprecated no-op.
- `range.tur`'s internal endpoint representation is the `Bound` GADT, not the
  malloc'd `{bool is_inclusive; int64_t value}` struct.
- `Range` and `Bound` carry `Show`, `Eq`, and `Ord` (on `Bound` values)
  typeclass instances, replacing the hand-written predicates.
- **No public API breakage.** Every existing constructor (`closed-range`,
  `open-range`, `at-least-range`, ...) and predicate (`range-contains?`,
  `range-encloses?`, `range-overlaps?`, ...) keeps its name, arity, and
  observable behaviour. `range-bound.tur`'s shims and the `range-bound-gadt`
  fixture continue to pass unchanged (the shims become near-identity once the
  internals are the GADT).

## Non-goals

- Removing the `-Xgadt` token or the other GADT-implying flags
  (`-Xsized-types`). Graduation means default-on + accepted-as-no-op, mirroring
  the `-Xcallcc` precedent in `src/main.c` (`call/cc/escape are now real +
  ungated`).
- Parameterising `Bound` over a non-`int` element type. The carrier stays
  `(Bound int)`; range endpoints are integers (float ranges live in
  `float-range.tur` and are out of scope -- see Risks).
- A general typeclass pass over the rest of stdlib. Only `Range` / `Bound`
  instances are in scope.

## Design

### Track A -- graduate `-Xgadt` to default-on

The flag is consulted in exactly two compiler sites today:

- `src/compiler/elab_structs.c:1689` -- `elab_defgadt` hard-errors when
  `!g_gadt_enabled`.
- `src/compiler/elab_toplevel.c:730` -- the pre-pass skips registering GADTs
  when `!g_gadt_enabled`.

Plan:

1. Default `g_gadt_enabled = true` at its definition. Keep the `-Xgadt` token
   parsing in `src/main.c` (both `wk_apply_flags` and the argv loop) but make
   it a no-op that optionally emits a one-shot "deprecated: GADTs are enabled
   by default" notice, exactly as `-Xcallcc` does.
2. Drop the two `!g_gadt_enabled` guards (they become dead) **or** leave them
   in place defensively -- since the global now defaults true they never fire.
   Prefer removing the `elab_defgadt` hard-error block so the diagnostic text
   ("recompile with -Xgadt") does not bit-rot; leave the toplevel skip as a
   harmless `if (false)`-equivalent only if removing it is risky.
3. Audit `-Xsized-types`, which sets `g_gadt_enabled = true` as a side effect.
   That assignment becomes redundant but harmless; leave it.
4. Sweep fixtures: every `tests/fixtures/*/flags` file that contains only
   `-Xgadt` can drop it (the fixture still passes either way, since the token
   is now a no-op). This is cosmetic and can be batched; do **not** remove
   `-Xgadt` from fixtures that also pass other flags in the same file without
   re-running them.

**Compatibility:** because the token is still accepted, every existing build
command, fixture `flags` file, and `range-bound.tur`'s `-Xgadt` requirement
keeps working. The change is purely "the gate is open by default."

### Track B -- fold the GADT into `range.tur` + typeclass instances

Once Track A lands, `range.tur` can `defgadt` directly. Two sub-steps:

#### B1 -- replace the internal `RangeBound` with `Bound`

Today `range.tur` models an endpoint as a heap pointer to
`{bool is_inclusive; int64_t value}` (or `0` for unbounded), via four inline-C
functions (`range-bound-new`, `range-bound-inclusive?`, `range-bound-value`,
`range-bound-flip`) at `stdlib/range.tur:37-71`. Replace them with the `:copy`
`Bound` GADT:

```turmeric
(defgadt Bound [A]
  :copy
  (Inclusive int : (Bound int))
  (Exclusive int : (Bound int))
  (Unbounded     : (Bound int)))
```

- `range-bound-new 1 v` -> `(Inclusive v)`; `range-bound-new 0 v` ->
  `(Exclusive v)`; the unbounded sentinel `0` stays `0` at the *Range* level
  (a `Range` field is still "0 = unbounded, else a Bound"), or -- cleaner --
  the unbounded case becomes `(Unbounded)` and `Range` holds two `Bound`s with
  no null sentinel. **Decision required** (see Open Questions): null-sentinel
  vs. total `Unbounded` constructor. The total form is the point of the GADT;
  prefer it unless the `#r{...}` reader lowering makes the sentinel cheaper.
- `range-bound-inclusive?` / `range-bound-value` become `match` over `Bound`.
- `range-bound-flip` becomes a two-arm `match` returning the opposite
  constructor.
- The four inline-C `defn`s are deleted; `range-bound.tur`'s shims collapse to
  near-identity (and may be retained one release for source compatibility, then
  deprecated).

To avoid a flag day, B1 keeps the *Range* container representation
(`{int64_t lower; int64_t upper}`) and only swaps what each field points at,
so `range-new` / `range-lower` / `range-upper` are unchanged.

#### B2 -- `Show` / `Eq` / `Ord` instances

Replace hand-rolled formatting and comparison with typeclass instances:

- `(definstance Show [Bound] ...)` -- renders `[3`, `(3`, or unbounded.
- `(definstance Show [Range] ...)` -- renders `[1, 5)` etc., replacing any
  ad-hoc range-to-string helper.
- `(definstance Eq [Bound] ...)` -- structural equality, replacing pointer- or
  field-comparison helpers used inside `range-lower-min` / `range-upper-max` /
  the `empty-range?` / `singleton-range?` predicates.
- Optionally `(definstance Ord [Bound] ...)` to express the lower/upper
  min/max selection (`range-lower-min` and friends at
  `stdlib/range.tur:534-570`) as `Ord` comparisons rather than nested `if`s.

Pattern to follow: the `Show`/`Eq` instances for `Pair` (`stdlib/pair.tur`),
`Option` (`stdlib/option.tur`), and `Result` (`stdlib/result.tur`).

## Phasing

1. **A1** -- default `g_gadt_enabled = true`; make `-Xgadt` a no-op with a
   deprecation notice; remove/neutralise the two guards. Run the full suite
   (every GADT fixture must still pass *without* its flag mattering). Add a
   fixture that uses `defgadt` with **no** `-Xgadt` flag to lock in the default.
2. **A2** *(cosmetic, optional, batchable)* -- strip now-redundant `-Xgadt`
   from `flags` files. Pure churn; can trail the substantive work or be
   skipped.
3. **B1** -- swap `range.tur`'s endpoint internals to the `Bound` GADT; keep
   `range-bound.tur`'s shims green. Regenerate any affected codegen snapshots.
4. **B2** -- add `Show`/`Eq`/`Ord` instances; rewrite the formatting/comparison
   helpers to use them. Add fixtures exercising `(show some-range)` and
   `Bound` equality/ordering.
5. **Docs** -- update `docs/guides/gadts-guide.md` (drop "Requires: -Xgadt"
   from examples, note the flag is default/deprecated), the
   `compiler-flags-guide.md` entry for `-Xgadt`, and run `tur run docs`.

Each phase is independently shippable; A1 is the keystone and the only one
that touches the compiler.

## Risks

- **`-Xgadt` default-on widens the language for *all* programs.** GADT
  pattern-matching and `defgadt` registration now happen unconditionally.
  Mitigation: the syntax is purely additive (a new top-level form + match
  arms); no existing program changes meaning. The `-Xcallcc` graduation is the
  precedent that this is safe and low-churn.
- **The malformed-`defgadt` NULL-deref crash becomes reachable without a flag.**
  [defgadt-malformed-pattern-segfault.md](../archive/history/defgadt-malformed-pattern-segfault.md)
  documents a SEGV on a malformed constructor; today it requires `-Xgadt`.
  After A1 it is reachable by default, which **raises its severity**. Fix that
  report (option 1: don't leave a half-built `AdtDef` registered) as a hard
  prerequisite of, or alongside, A1. Do not ship A1 with that crash open.
- **`range.tur` is load-bearing.** It is auto-loaded transitively across
  stdlib and drives the `for` macros and the `#r{...}` reader. B1 must keep the
  `Range` container layout and every public name identical. Gate B1 behind the
  full `range-*` fixture set (`range-constructors`, `range-predicates`,
  `range-encloses`, `range-intersection`, `range-span`, `range-gap`,
  `range-connected-overlaps`, `range-from-range[-step]`, the `range-reader-*`
  set, and `range-bound-gadt`).
- **`float-range.tur` shares vocabulary.** It is **out of scope** -- `Bound`'s
  carrier is `int`. Confirm B1 does not accidentally rename a symbol
  `float-range.tur` depends on.
- **Codegen snapshots.** B1 changes emitted C for range.tur consumers. Follow
  the Fixture Snapshots rule: regenerate `tests/fixtures/*/expected.c` in the
  same commit and confirm zero `FAIL`.

## Open questions

1. **Null sentinel vs. total `Unbounded`.** Does the `#r{...}` reader lowering
   (`src/compiler/reader_macros.c`, `#r{` dispatch) construct bounds in a way
   that makes a `0`-pointer sentinel materially cheaper than a heap-allocated
   `(Unbounded)` cell? If the reader emits calls to the public constructors,
   the GADT is transparent and the total form wins. Resolve before B1.
2. **Retain or deprecate the `range-bound.tur` shims?** Once range.tur *is* the
   GADT, `bound->range-bound` / `range-bound->bound` are near-identities. Keep
   them one release for source compat, or fold `range-bound.tur` into
   `range.tur` and leave a re-exporting stub? Lean: keep the stub one release.
3. **`Ord [Bound]` semantics for mixed inclusive/exclusive at equal value.**
   `(Inclusive 5)` vs. `(Exclusive 5)` ordering only matters for the
   min/max-bound selection; define it to match the current `range-lower-min` /
   `range-upper-max` tie-breaking exactly, captured by a fixture.

## Acceptance

- `defgadt` compiles and GADT `match` works with **no** `-Xgadt` flag; the flag
  is still accepted (no-op + deprecation notice).
- The malformed-`defgadt` crash report is closed (not merely flag-gated away).
- `range.tur`'s endpoint internals are the `Bound` GADT; the four inline-C
  `range-bound-*` accessors are gone or reduced to GADT `match`.
- `Range` / `Bound` have `Show` + `Eq` (+ `Ord` on `Bound`) instances; the
  hand-rolled equivalents are removed.
- Every existing `range-*` and `range-bound-gadt` fixture passes unchanged;
  new fixtures cover flagless `defgadt`, `(show range)`, and `Bound`
  equality/ordering.
- `bash tests/run.sh` passes with zero `FAIL` lines (leak detection on).
- `tur run docs` regenerated; guide examples drop the `-Xgadt` requirement.

## Cross-references

- Direct prerequisite: `defgadt :copy` --
  [docs/reported/defgadt-copy-and-shared-bounds.md](../reported/defgadt-copy-and-shared-bounds.md).
- Hard co-requisite of A1:
  [defgadt-malformed-pattern-segfault.md](../archive/history/defgadt-malformed-pattern-segfault.md).
- Parent: phase R3 of
  [stdlib-refinement-collections-plan](./stdlib-refinement-collections-plan.md).
- Flag-graduation precedent: `-Xcallcc` no-op handling in `src/main.c`.
