# Fixture-Churn Paydown Plan

> **Status:** Proposed -- not started.
> **Last Updated:** 2026-06-05
> **Type:** test infrastructure + batched codegen cleanup
> **Sibling plans:**
> - [reversible-name-mangling-plan.md](reversible-name-mangling-plan.md) -- T6 is a primary Phase 2 payload
> - [poly-closure-result-specialization-plan.md](poly-closure-result-specialization-plan.md) -- Stage B+C is a primary Phase 2 payload
> - [stdlib-inline-c-deworkaround-plan.md](stdlib-inline-c-deworkaround-plan.md) -- recent exemplar of "regen 75+ snapshots in one go"

---

## Overview

`tests/fixtures/*/expected.c` are exact codegen snapshots. There are ~1442
fixtures and the suite asserts byte-for-byte equality with the compiler's
emitted C. Any change to the preamble, name mangling, ABI lowering, or
calling convention ripples across hundreds-to-thousands of snapshots.

The recurring failure mode: a small, otherwise-uncontroversial codegen
cleanup is **deferred** in PR review with "fixture churn -- punt for
later." This happens nearly every PR. The deferred items accumulate into
a long tail of paper-cut codegen debt that nobody wants to be the one to
land. Examples from `docs/upcoming/`:

- **Reversible name mangling** (T6) -- defers re-spelling every
  kebab-case identifier (`list-concat` -> `list_hyconcat`) because the
  snapshot diff would be larger than the A3 sigil regeneration.
- **Per-monomorphization closure-body specialization** (Stage B+C) --
  defers inner-body cloning because it churns `arrow-instance-*`,
  `sf-compose-typed`, `fat-closure-float-*`, `module-spec-*`.
- **`defrecord` + field-accessor syntax + bidirectional container
  inference** (Phase D/E of the archived test-suite-idioms plan) --
  flagged as "substantial reader/codegen/ABI change with broad snapshot
  churn."
- **Tag-accessor macro sweep** (archived Phase C #3) -- `tag == 0/1/2/3`
  inline-C dispatch in logic/Free fixtures couples fixture source to
  ADT layout; not retired because the sweep itself churns fixtures.

The fix is not to keep deferring. It is to (a) make regen cheap, (b)
reduce how much fixtures depend on exact codegen in the first place,
and (c) land the deferred work as a coordinated batch rather than
nibbling at it.

---

## Phase 0 -- Make regen and review cheap (prerequisite)

Goal: reduce the cost of "regenerate 1442 snapshots and review the diff"
from a multi-hour ceremony to a routine PR step. Until this lands,
Phases 1-2 are not worth attempting.

### 0.1 `tur run regen-snapshots` recipe

Bake the regen loop from `CLAUDE.md` (Fixture Snapshots strict rule) into
the `Justfile` as a first-class recipe.

- `tur run regen-snapshots` -- regenerate every `expected.c` in place.
- `tur run regen-snapshots -- --check` -- exit non-zero if any snapshot
  would change. Wire this into CI as a guard so accidental codegen
  drift never lands silently.
- Handle both naming conventions (`input.tur` and
  `$(basename $dir).tur`).

### 0.2 Snapshot-diff summarizer

A `tools/snapshot-diff-summary.py` that takes a git diff over
`tests/fixtures/**/expected.c` and classifies each changed file:

- **preamble-only** -- diff is confined to the header/macro preamble.
- **mangling-only** -- diff is confined to identifier renames matching
  a known mangling rewrite (regex-driven).
- **body** -- anything else; flagged for human review.

Output: `"1380 preamble-only, 42 mangling-only, 20 body -- review the
20."` Turns a 1442-file diff into a 20-file review.

### 0.3 Two-commit convention, enforced

The strict rule already says snapshots ride with the codegen change.
Make it mechanical:

- PR template: "Codegen-touching change? Commit 1 = codegen, commit 2 =
  regen. Paste snapshot-summary output."
- CI check: if any `src/codegen/**` file changed and `expected.c` files
  did **not**, fail with a link to `tur run regen-snapshots`.

**Exit criteria for Phase 0:** regen + summarize + review for a
preamble-level change takes under 5 minutes wall-clock.

---

## Phase 1 -- Reduce fixture sensitivity to codegen

Goal: cut the blast radius of future codegen changes. Many fixtures
don't actually need to assert on emitted C -- they assert on runtime
behavior and `expected.c` is dead weight that just churns.

### 1.1 Audit fixtures for unnecessary `expected.c`

Sweep `tests/fixtures/**/` and identify fixtures where:

- `expected.stdout` (or equivalent runtime assertion) exists, AND
- the codegen has no historically interesting structure (not a
  regression fixture for a codegen bug), AND
- the fixture name doesn't carry "codegen" / "emit" / "snapshot" /
  "regression" semantics.

For those, delete `expected.c`. The runtime assertion remains the
source of truth.

Deliverable: a one-shot `tools/audit-fixture-snapshots.py` that prints
candidates plus rationale, run interactively, with a list of fixtures
proposed for snapshot removal. Land the deletions in batches by
category.

### 1.2 Tag-accessor macro sweep

Adopt `TUR_TAG(x)` and `TUR_PAYLOAD(x)` macros in the preamble, then
rewrite the hand-written `tag == 0/1/2/3` inline-C dispatch in
logic/Free fixtures to use them. Decouples fixture source from ADT
memory layout, so future ADT lowering tweaks don't churn these fixtures.

(This is the deferred Phase C #3 item from
`docs/archive/test-suite-idioms-plan.md`.)

### 1.3 Stabilize the preamble surface

Quick audit: is anything in the preamble there only by accident
(e.g. `#include` lines that no fixture uses)? Pruning unused preamble
shrinks the per-snapshot diff for every future preamble change.

**Exit criteria for Phase 1:** the number of `expected.c` files drops
by ~30-50%, and remaining ones are intentional codegen assertions.

---

## Phase 2 -- Batched codegen cleanups

With Phase 0 tooling in place and Phase 1 surface reduction done,
land the long tail of deferred codegen cleanups as a **single
coordinated regeneration window** rather than nibbled across many PRs.

Sequence within Phase 2:

### 2.1 Reversible name mangling (T6)

Land the full mangling sweep from
[reversible-name-mangling-plan.md](reversible-name-mangling-plan.md).
This is the largest single source of identifier-level snapshot churn;
regenerate once.

### 2.2 Per-monomorphization closure-body specialization (Stage B+C)

Land Stages B+C from
[poly-closure-result-specialization-plan.md](poly-closure-result-specialization-plan.md).
Affects `arrow-instance-*`, `sf-compose-typed`, `fat-closure-float-*`,
`module-spec-*` -- known up front, so bundle in the same window.

### 2.3 `defrecord` + field accessors + bidirectional container inference

Pick up Phases D/E from the archived test-suite-idioms plan **only if
Phase 1 reduced the relevant fixture surface enough to make this
tractable in the same window**. Otherwise defer to a follow-up Phase 2b.

### 2.4 Regen + ship

One PR. One regen pass. One review of the snapshot-diff summary
output. Land it.

**Exit criteria for Phase 2:** every codegen cleanup currently
deferred "due to fixture churn" has either landed or has an explicit
written reason why it is no longer wanted.

---

## Phase 3 -- Policy (ongoing)

Once Phase 0 makes regen cheap, the policy changes:

- **Small codegen cleanups regenerate snapshots in the same PR.** No
  more "punt the snapshot regen to a follow-up." Add this to
  `CLAUDE.md` under Fixture Snapshots.
- **"Fixture churn" is not, on its own, a reason to defer a fix.** It
  may be a reason to coordinate timing (don't land two large regens
  on the same day, give in-flight branches time to rebase), but not to
  shelve the work indefinitely.
- **Only batch a cleanup into a Phase 2-style window if it is itself
  large enough to warrant coordination** (touches >500 fixtures, or
  has interacting semantic changes that benefit from a single regen).

---

## Sequencing recommendation

1. Phase 0 first -- unblocks everything else. ~1 day.
2. Phase 1 in parallel with scoping Phase 2's actual payload. ~2-3 days.
3. Phase 2 as one coordinated window once 0+1 are in. ~1 week including
   review. Announce the window so in-flight branches can rebase past it.
4. Phase 3 -- amend `CLAUDE.md` once the policy holds in practice for
   ~2 weeks.

## Non-goals

- Switching to fuzzy/structural snapshot matching. Exact-match is a
  feature: it catches accidental codegen drift. The plan reduces the
  *cost* of intentional changes, not the strictness of detection.
- Per-fixture opt-out flags for snapshot checking. The Phase 1 sweep
  either keeps the snapshot or removes it; no middle ground that adds
  config surface.
- Rewriting the snapshot format. The plain `.c` files are
  human-reviewable; that's a feature.
