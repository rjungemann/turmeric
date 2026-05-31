---
name: churn-docs
description: Curates docs/archive/ -- moves obsolete plans into docs/archive/history/ and breaks off guide-worthy material into docs/guides/ with proper frontmatter. Use when the archive has accumulated completed/superseded plan docs, or when reusable material is buried in a plan and deserves a guide.
---

# Churn Docs

`docs/archive/` collects planning/design documents that drive feature work.
Once a plan is complete or superseded, it should move to `docs/archive/history/`
so the archive index reflects what is actually *in-flight*. Concepts and
how-to material that live inside those plans often deserve to be lifted into
`docs/guides/` so users (and future contributors) can find them.

This skill does both passes in order.

## Inputs

If the user passes a filename or pattern (e.g. `/churn-docs tur-tls`), restrict
the sweep to matching files in `docs/archive/`.

Otherwise, scope the sweep to files added or moved into `docs/archive/` **since
the last tagged version**. This keeps each churn pass aligned with a release
boundary instead of re-scanning every plan in the archive on every run.

Compute the scoped set like this:

```sh
LAST_TAG=$(git describe --tags --abbrev=0)
git diff --name-only --diff-filter=AR "$LAST_TAG"..HEAD -- 'docs/archive/*.md' \
  | grep -v '^docs/archive/README.md$' \
  | grep -v '^docs/archive/history/'
```

`--diff-filter=AR` catches both freshly-added plans and plans that were renamed
into `docs/archive/` (the common pattern when a `docs/<plan>.md` lands and then
gets moved to the archive once the feature ships). If the scoped set is empty,
report that and exit -- there's nothing to churn for this release yet. The user
can always override with an explicit filename/pattern to sweep older plans.

## Pass 1 -- Move obsolete plans to history/

For each file in the scoped set (skip `README.md`, skip the `history/` subdir):

1. **Read the plan.** Look for completion signals:
   - Phase/task tables where every row is checked off or marked complete.
   - "Status:" / "Result:" sections saying "shipped", "landed", "complete",
     "superseded by ...", "abandoned".
   - References from `git log --oneline -- docs/archive/<file>` and from
     guides that already say "see [guide-x.md]" -- a plan that's been
     extracted into a guide is usually done.
   - Cross-check with `CHANGELOG.md` and recent commits: if the feature
     shipped in a tagged release, the plan is historical.
2. **Be conservative.** When in doubt, leave the file in `docs/archive/`
   and ask the user. False positives (moving a still-active plan) are
   worse than false negatives. List candidates and confirm with the user
   before any `git mv` when more than ~3 files are involved.
3. **Move with `git mv`** so history is preserved:
   ```sh
   git mv docs/archive/<plan>.md docs/archive/history/<plan>.md
   ```
4. **Update `docs/archive/README.md`** -- this file is the index. Remove
   the moved plan from its "Active" section and add it under the
   appropriate "Historical / Recently completed" block, following the
   one-line bullet style already used there:
   ```
   - **[plan-name.md](history/plan-name.md)** -- <what shipped>; see [../guides/<x>-guide.md](../guides/<x>-guide.md)
   ```
   Keep entries grouped by sweep (the README already has dated headings
   like "Post-v0.14.6 sweep"); add to the most recent group or start a
   new one if this churn corresponds to a release boundary.

## Pass 2 -- Extract guide-worthy material into docs/guides/

A plan is "guide-worthy" when it contains content a *user* would want to
read -- not just the implementation plan. Look for:

- **Concept explanations** -- "What is X / when do you use it / how does
  it compare to Y".
- **Worked examples** -- code samples with explanation that aren't just
  test fixtures.
- **API reference** -- function signatures, options, flags users will
  invoke.
- **Decision/usage guidance** -- "when to reach for this", "gotchas",
  "performance characteristics".

Implementation-only content (TODO checklists, phase tables, internal
data-structure diagrams, "M1 lands codegen") stays in the plan and does
**not** belong in a guide.

### When extracting:

1. **Check first** whether a guide already exists for this topic
   (`ls docs/guides/`). If so, *augment* the existing guide rather than
   creating a new one -- the goal is one canonical guide per concept.
2. **Naming**: `docs/guides/<topic>-guide.md` (or `<topic>-tutorial.md`
   for step-by-step learning material). Match existing naming -- e.g.
   `httpd-tls-guide.md`, `dynamic-vars-guide.md`.
3. **Frontmatter is required**, in this exact form:
   ```markdown
   ---
   title: <Title Case Guide Name>
   category: <one of the existing categories -- see below>
   description: <one sentence, ~120 chars, what the guide covers>
   ---
   ```
   Existing categories to choose from (use `grep -h '^category:' docs/guides/*.md | sort -u`
   to confirm the current set):
   - Getting Started
   - Language Basics
   - Language Features
   - Type System
   - Concurrency and Async
   - Concurrency and State
   - Advanced Control Flow
   - Effects and Continuations
   - Tooling
   - Editor Integration
   - Web and Deployment
   - Performance
   - Other
   If a topic genuinely doesn't fit, reuse the closest match rather than
   inventing a new category.
4. **Style**, matching the existing guides:
   - Open with a one-paragraph what-and-why.
   - Lead with a "Quick Start" or "When to use" section.
   - Use `turmeric` fenced code blocks for code samples.
   - Cross-link related guides with relative paths
     (`[reactor-guide.md](reactor-guide.md)`).
   - ASCII only -- use `--` not en/em dashes (CLAUDE.md fixture rule
     applies to docs too for consistency).
5. **Leave a pointer in the source plan.** Add a `> Extracted to
   [../guides/<x>-guide.md](../guides/<x>-guide.md)` note at the top of
   the plan, then move the plan to `history/` per Pass 1.
6. **Update the guides index.** `docs/guides/README.md` lists guides by
   category. Add the new guide there.
7. **Update `docs/archive/README.md`** -- add an entry under the
   "Extracted Guides" table mapping `guide-name.md` -> origin plan(s).

## Verification

After both passes:

1. `git status` -- expect renames into `history/`, edits to two README
   files, and any new guide files.
2. Spot-check one moved file with `git log --follow` to confirm history
   is intact.
3. `grep -r "docs/archive/<moved-plan>" docs/ src/ CLAUDE.md` -- fix any
   stale references that don't point at the new `history/` path.
4. Report a summary to the user: N files moved, M guides created /
   augmented, K stale references fixed. Ask before committing -- this
   skill never commits on its own.

## Anti-patterns

- **Don't move a plan to `history/` just because it's old.** "No recent
  commits" is not the same as "complete". Look for explicit completion
  signals.
- **Don't create a guide for every plan you archive.** Most plans are
  internal and have no user-facing surface. A guide that just paraphrases
  a phase table is noise.
- **Don't duplicate content.** If material already lives in a guide,
  link to it from the (now historical) plan rather than copying it into
  a new guide.
- **Don't rewrite the archive README from scratch.** Edit in place,
  preserving the existing section structure and sweep groupings.
- **Don't skip the frontmatter.** The docs site's table-of-contents
  generator reads `title` / `category` / `description` -- a guide
  without frontmatter won't render correctly.
