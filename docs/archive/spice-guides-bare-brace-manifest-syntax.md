# Spice guides document `build.tur` maps with bare braces, which no longer parse

**Status:** resolved 2026-07-29 -- all three fix directions landed, plus two
defects the sweep uncovered.

**Severity:** high (docs) -- every manifest snippet in the spice guides and the
README is a hard parse error if pasted. This is the first thing a new user
copies, and `README.md` is the front door.


## Resolution (2026-07-29)

All three fix directions landed together, because (1) and (3) are what stop the
docs from rotting again and (2) alone would just reset the clock.

### (1) The hint is alive again

`report_non_map` (`src/compiler/pkg.c`) no longer gates on
`got->tag == F_CONTRACT_TYPE`. Every wrong shape now gets the spelling, and a
bare brace gets an extra clause naming what the reader actually saw:

```
build.tur:3:11: error: build.tur: :spices must be a map -- use `#map{...}`
(a bare `{...}` is curly-infix arithmetic, not a map)
```

Detection is on the source text at the span (`sf->src[span.off_start] == '{'`),
since curly-infix lowers to an ordinary call form and leaves no distinguishing
tag behind. Regression: `build-project-manifest-bare-brace-hint-{spices,
cmake-deps,build-opts}` in `tests/run-build-project.sh`.

### (2) The sweep -- 129 sites

README.md + 7 guides. Not a blind replace: the rewrite runs per fenced block,
only inside blocks that are manifests, and only on `{` preceded by a map-valued
manifest key or a quoted entry key -- so the curly-infix arithmetic in the same
guides is untouched. Hanging alignment of continuation lines is preserved (the
opening token grew by 4 columns, so every line aligned under the first key moved
with it).

**`tur.lock` snippets deliberately kept `#{...}`.** They depict generated
output, and the lockfile writer emits `#{`. Documenting `#map{` there would have
been a new inaccuracy in place of the old one.

`docs/archive/` and `docs/upcoming/` were left alone (53 more sites): those are
historical planning records, not instructions anyone follows.

### (3) The checker

`tools/check-guide-pairs.py` now extracts every fenced block whose first form is
`(defpackage ...)` -- `turmeric`, `sweet-exp`, and `lisp` fences, the last
because that is how README.md fences its manifest -- writes it to a temp dir as
`build.tur` / `build.tur.sweet`, and runs `tur fetch --dry-run` over it. No
network, and only manifest-level diagnostics count as failures (existence checks
like `:c-sources entry not found` are facts about the temp dir, not the doc).

Two things had to change for it to actually check anything:

- **`no-check` does not opt out of the manifest check.** That marker means "no
  sweet-exp companion", which is true of nearly every manifest snippet -- so
  honoring it here would have skipped exactly the blocks that rotted. The new
  opt-out is `no-manifest-check`.
- **`tur` is resolved from `./build/tur`** when it is not on `$PATH`. It
  previously fell back to skipping the parse check silently, which is its own
  small version of this same bug: a checker that passes because it did nothing.
  Turning it on immediately surfaced a real pre-existing pair mismatch --
  `reactor-guide.md` spelled the sweet-exp half `{client != -1}` where `!=` is
  not a bound name (the operator is `not=`). Fixed.

`just check-guides` now runs over `README.md docs/guides/`, and reports
`830 pairs ok, 21 manifests ok`.

## Two defects the sweep uncovered

Standardizing the docs on `#map{...}` walked straight into a path nothing had
exercised, because the docs had never told anyone to write it.

### `tur add` silently deleted every existing dependency (data loss)

`pkg_defpackage_add_spice` read the existing map with
`old_map->tag == F_MAP ? len : 0`. `#map{...}` is `F_MAP_LITERAL`, so the splice
saw an empty map and wrote back a `:spices` containing only the newly added
entry:

```
;; before
:spices #map{"geom" #map{:url "https://example.invalid/geom" :ref "v1"}}
;; after `tur add https://example.invalid/math --ref v2`  -- geom is GONE
:spices #{"math" #{:url "https://example.invalid/math" :ref "v2"}}
```

Fixed to accept both tags, and to rebuild with the tag the manifest already had
so an add no longer respells the user's `#map{` as `#{`. Regression:
`tur-add-preserves-existing-spices-{maplit,legacy}` in
`tests/spice-resolver-tests.sh`.

Had the doc sweep landed alone, it would have pointed every reader at the one
manifest spelling that loses their dependency list on the next `tur add`.

### `tur format` could not break a `#map{...}` across lines

`F_MAP_LITERAL` was unconditionally inline in `fmt_form`, so a formatted
multi-dep manifest collapsed onto one very long line. It now takes the same
width-based break as `#{...}` (`fmt_map_broken`, which learned the `#map{`
opener). `#set{...}` / `#row{...}` stay inline -- short elements, no established
broken shape. No fixture churn: only over-width literals move.

## Verification

- `bash tests/run.sh` -- 2412 passed, 0 failed
- `bash tests/run-build-project.sh` -- 38 passed, 0 failed
- `bash tests/spice-resolver-tests.sh` -- 66 passed, 0 failed
- `bash tests/run-fmt.sh` -- 18 passed, 0 failed
- `bash tests/run-flags.sh` -- 78 passed, 0 failed
- `python3 tools/check-guide-pairs.py README.md docs/guides/` -- 830 pairs ok,
  21 manifests ok, 0 failed

## Not done

The `:exports` / `:bin` / `:options` slots have no equivalent of the manifest
snapshot check for *spice READMEs* (`--spices` mode reaches them, but the
sibling repo is not present in every checkout). And `tur add-cmake` still
round-trips the whole manifest through `pkg_manifest_write`, which regenerates
in the writer's spelling and drops comments -- pre-existing, orthogonal to this
report, and worth its own pass.

---

## Original report

## Summary

The guides spell manifest map slots with bare braces:

```turmeric
:spices {
  "geom" {:url "https://github.com/alice/tur-geom" :ref "v0.2.1"}
}
```

Bare `{...}` is SRFI-105 curly-infix in every dialect now, so the manifest
parser rejects it. The accepted spellings are `#{...}` (legacy) and
`#map{...}` (canonical).

## Repro

Paste the `README.md:112` snippet into a `build.tur` and build it:

```sh
$ tur build ./myproj
build.tur:4:11: error: build.tur: :spices must be a map
4 |   :spices {
  |           ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
```

Same for `:cmake-deps`, `:build-opts`, `:bin`, `:exports`, and the nested
per-entry maps.

## Scope

`grep -nE '^\s*:(cmake-deps|spices|build-opts|bin|exports|options) \{|^\s*"[^"]*" \{:'`
over the live docs:

| Sites | File |
| --- | --- |
| 34 | `docs/guides/package-management-guide.md` |
| 31 | `docs/guides/developing-spices-guide.md` |
| 20 | `docs/guides/consuming-spices-guide.md` |
| 6 | `docs/guides/web-stack-guide.md` |
| 5 | `README.md` |
| 5 | `docs/guides/stats-guide.md` |
| 3 | `docs/guides/tur-watch-guide.md` |
| 3 | `docs/guides/notebook-guide.md` |

107 total. Many files carry both a plain and a `sweet-exp` companion block
with the same defect, so the fix must touch both halves of each pair.

The tooling is already correct and is the reference for the right output:
`tur add` writes `:spices #{` (`src/compiler/pkg.c:3713`), the lockfile writer
emits `#{` (`pkg.c:749`, `pkg.c:1089`), and the E0620 hint text spells
`:exports #map{` (`src/compiler/diag.c:2001`). Only the prose is stale.

## Root cause

Two independent gaps let this rot silently.

1. **Nothing compiles guide snippets.** `tools/doctest.py` extracts only
   stdlib `;;;` Example blocks; `tools/check-guide-pairs.py` checks that a
   plain block has a sweet-exp companion and never parses either
   (`check-guide-pairs.py:92`). A ` ```turmeric ` fence in a guide is
   unvalidated prose. Manifests are the worst case because they are the
   snippets most likely to be copied verbatim.

2. **The one diagnostic that could teach the fix has a dead hint.**
   `report_non_map` (`src/compiler/pkg.c:138`) appends
   "use `#{...}` for map syntax; bare `{...}` is a contract type" only when
   `got->tag == F_CONTRACT_TYPE`. Contract types moved to `#refine{...}` and
   bare `{` is now unconditionally curly-infix (`reader.c:3028`, and see the
   comment at `reader.c:43`), so a bare-brace manifest value never carries
   that tag and the hint never fires. The user gets a bare
   ":spices must be a map" with no indication of what a map looks like.

## Fix directions

1. Revive the hint in `report_non_map` -- drop the `F_CONTRACT_TYPE` condition
   and always suggest `#{...}` / `#map{...}`. One-line change, and it makes
   every stale copy of the docs self-correcting. Do this first; it is
   independent of the doc sweep.
2. Sweep the 107 sites to `#map{...}` (canonical, and now accepted at every
   slot). Not a blind find-and-replace: bare `{...}` is legitimate
   curly-infix arithmetic elsewhere in these same guides, so match on the
   manifest-keyword prefixes above and review each hit. Do plain and
   sweet-exp companions together.
3. Consider teaching `check-guide-pairs.py` to run `pkg_manifest_read` over
   any fenced block whose first form is `(defpackage ...)`. That closes gap
   (1) for the highest-value category of snippet without needing a general
   doc-compiler.

## Related

- `docs/archive/exports-map-syntax-tighten-plan.md` -- the `#fx{...}` half of
  the same confusion; its follow-up audit landed TUR-E0620 across all map
  slots and made `#map{...}` accepted everywhere (previously `:cmake-deps`
  and friends took only `#{...}`, which is why the canonical spelling is
  safe to standardize on now).
