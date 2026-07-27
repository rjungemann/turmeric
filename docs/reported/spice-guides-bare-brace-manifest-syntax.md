# Spice guides document `build.tur` maps with bare braces, which no longer parse

**Severity:** high (docs) -- every manifest snippet in the spice guides and the
README is a hard parse error if pasted. This is the first thing a new user
copies, and `README.md` is the front door.

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
