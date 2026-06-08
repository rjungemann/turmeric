# Strip numbered section headings from guides

## Motivation

20 guides under `docs/guides/` carry hand-numbered section headings like
`## 1. Why combinators?` or `### 3.2 Bar`. The numbering adds churn (any
insert/delete renumbers every following heading), shows up in the rendered
TOC and the `gendocs.py` output, and competes with the title text for
scanability. Guides that aren't numbered read just as well. Strip the
leading `N.` / `N.M[.K]` prefix from headings and update the in-prose
cross-references so the docs still resolve.

## Scope

In-scope:

- All `.md` files under `docs/guides/`.
- Heading lines matching `^#{1,6} <number>(\.<number>)*\.? <title>`.
- TOC anchor links of the form `[Title](#N-slug...)` that point to the
  renumbered headings (these break the moment the heading is renamed).
- Prose cross-references like "Section 8 explains ...", "(§1.1)",
  "see section 4" -- replaced with the actual section title (or a
  Markdown link to the renamed anchor).

Out of scope:

- Numbered ordered lists in prose (`1. foo\n2. bar`) -- only headings.
- `docs/upcoming/`, `docs/archive/`, `docs/design/`, `docs/reported/`.
  Plans and rationale docs often have load-bearing numbered sections;
  leave them alone for now.
- Headings whose number is semantically part of the title (e.g. a year
  or version: `## 2026 Roadmap`, `## Phase 2`). Inspect during the
  review pass; do not strip these.

## Affected files (20)

```
docs/guides/c-integration-guide.md
docs/guides/custom-effects-tutorial.md
docs/guides/datalog-02-minimal-impl.md
docs/guides/datalog-03-query-api.md
docs/guides/datalog-04-indexing.md
docs/guides/frame-guide.md
docs/guides/gadts-cookbook.md
docs/guides/gadts-guide.md
docs/guides/httpd-tls-guide.md
docs/guides/module-system-guide.md
docs/guides/package-management-guide.md
docs/guides/parser-combinators-tutorial.md
docs/guides/performance-guide.md
docs/guides/polymorphism-guide.md
docs/guides/snake-game-tutorial.md
docs/guides/state-machines-guide.md
docs/guides/type-erasure-guide.md
docs/guides/web-continuations-guide.md
docs/guides/web-continuations-tutorial.md
docs/guides/web-emscripten-tutorial.md
```

Generate the list with:

```sh
grep -rlE '^#{1,6} [0-9]+(\.[0-9]+)*\.? ' docs/guides/
```

## Procedure

### Step 1: heading rewrite

For each in-scope file, apply the substitution

```
s/^(#{1,6}) [0-9]+(\.[0-9]+)*\.?\s+/\1 /
```

to every heading line. So:

```
## 1. Why combinators?           -> ## Why combinators?
### 3.2 The shape of Parser<a>   -> ### The shape of Parser<a>
## 10. Wrapping a handler        -> ## Wrapping a handler
```

Implementation: a per-file Edit pass, not a sweeping `sed -i`. After each
file rewrite, eyeball the diff for false positives -- a year, a version
number, a step count that is part of the title.

### Step 2: anchor-link rewrite (TOCs)

GitHub-style slugs drop the leading `N-`/`N-N-` when the heading number
goes away. So:

```
[What are algebraic effects?](#1-what-are-algebraic-effects)
->
[What are algebraic effects?](#what-are-algebraic-effects)
```

Known TOC site: `docs/guides/custom-effects-tutorial.md` (15 entries).
Find all of them with:

```sh
grep -rEn '\(#[0-9]+(-[0-9]+)*-' docs/guides/
```

Rewrite by stripping the leading `N-` (or `N-N-`) chunk inside the
`(#...)` anchor.

### Step 3: prose cross-reference rewrite

Known references (from `grep -rEn '\b[Ss]ection [0-9]+|\([Ss]ee §|§[0-9]'
docs/guides/`):

| File | Line | Current text | Replacement strategy |
| --- | --- | --- | --- |
| `parser-combinators-tutorial.md` | 198 | "(Section 8 explains why.)" | Replace with the actual section title: "(Comparing to `stdlib/parsec` explains why.)" or link to `#comparing-to-stdlibparsec`. |
| `parser-combinators-tutorial.md` | 380 | "(Section 8 explains the underlying ABI.)" | Same -- name the section or link to it. |
| `parser-combinators-tutorial.md` | 615-616 | "continuation `^fat` (section 5) ... See section 8." | Replace both with section names or `#anchor` links. |
| `c-integration-guide.md` | 382 | "(Section 2.2)" | Rewrite to name the section or link. |
| `c-integration-guide.md` | 706 | "(see §1.1)" | Same. |
| `web-emscripten-tutorial.md` | 459 | "// reuse the singleton from Section 4" | Inline-comment in code -- rewrite to name the section. |
| `sized-types-guide.md` | 357 | "[sized-types-index-spec.md](...) section 6" | This points at a *different* doc (`docs/sized-types-index-spec.md`, out of scope). Leave it. |

Prefer naming the target section over linking by anchor -- anchor-only
references are brittle and easy to miss in future renames. Use a Markdown
link only when the cross-reference must be clickable.

### Step 4: validate

1. Re-run the heading grep -- it must come back empty for `docs/guides/`:
   ```sh
   grep -rE '^#{1,6} [0-9]+(\.[0-9]+)*\.? ' docs/guides/
   ```
2. Re-run the anchor grep for stale `(#N-...)` links:
   ```sh
   grep -rEn '\(#[0-9]+(-[0-9]+)*-' docs/guides/
   ```
3. Re-run the prose grep and confirm only the out-of-scope hit remains:
   ```sh
   grep -rEn '\b[Ss]ection [0-9]+|\([Ss]ee §|§[0-9]' docs/guides/
   ```
4. `bash tests/run.sh 2>&1 | grep '^FAIL'` -- guides are not compiled,
   but a sanity run catches accidental edits to fixtures.
5. Spot-render two or three affected guides (e.g. open them in the web
   docs, or `pandoc` to HTML) and confirm headings + TOC look right.

## Rollback

All edits are pure Markdown content changes in `docs/guides/`. `git
restore docs/guides/` cleanly reverts.

## Risk and review notes

- Per the global "no global rename" feedback rule: do the rewrite
  file-by-file with Edit calls, not a single repository-wide `sed -i`.
  Eyeball every diff.
- The currently modified file `docs/guides/parser-combinators-tutorial.md`
  is in the affected list. Coordinate with the in-flight edit before
  starting Step 1 on that file.
- Run Step 1, Step 2, Step 3 in that order per file so the file is
  internally consistent at every commit boundary if the work is split.
