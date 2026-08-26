# Rendered guides link to docs that were never rendered -- four dead links on the site

**Severity: low** (docs) -- four links in three published guides 404 on
turmeric-lang.com. Surfaced by `tools/genpack.py`'s link pass (OD1), which
reports every cross-link it cannot resolve inside the docs pack.

## Repro

```sh
just docs
```

The pack build ends with:

```
  note: 4 cross-link(s) in 3 page(s) point outside the pack and were left
        pointing at the website:
        guides/macros-guide.html: ../upcoming/hold/row-types-followups-plan.md,
                                  ../upcoming/macro-system-direction-plan.md
        guides/performance-guide.html: memory-management-guide.md
        guides/stateful-refinements-guide.html:
                                  ../archive/borrow-param-passed-as-unique-mut-undiagnosed.md
```

Each resolves to a 404 on the live site:

- `https://turmeric-lang.com/docs/html/guides/memory-management-guide.html`
- `https://turmeric-lang.com/docs/html/upcoming/macro-system-direction-plan.md`

## Root cause

Two distinct problems that the link pass reports together.

**1. A link to a guide that does not exist.** `docs/guides/performance-guide.md:365`
says `See [memory-management-guide.md](memory-management-guide.md).` There is no
`docs/guides/memory-management-guide.md`, and there is no record of one having
been removed -- the link looks like it was written against a planned guide.
`render_guide`'s link rewrite (tools/genguides.py) turns any local `.md` href
into a sibling `.html` without checking the target exists, so this has shipped
as a dead link since it was written.

**2. Links from `docs/guides/` into the rest of `docs/`.** `../upcoming/...md`
and `../archive/...md` resolve, from a page at `/docs/html/guides/`, to
`/docs/html/upcoming/...` and `/docs/html/archive/...`. Those paths have never
existed: `docs/html/` only ever contains `api/`, `guides/`, and `spices/`.
These were dead before OD5 narrowed the `web/public/docs` symlink and are
equally dead after it -- OD5 did not cause them, and the narrowing is not what
needs reverting.

## Fix direction

1. `performance-guide.md`: either write the missing guide or repoint the link.
   The surrounding paragraph is about RC and arena allocation, so
   `substructural-types-guide.md` or `opaques-guide.md` is the likely intent --
   worth confirming with the author rather than guessing.
2. The `../upcoming/` and `../archive/` links: these are references to internal
   planning notes from published guides, which is a smell in itself. Either
   drop them, or point them at GitHub blob URLs
   (`https://github.com/rjungemann/turmeric/blob/main/docs/upcoming/...`), which
   is what the guides already do for `CLAUDE.md`.
3. Once the tree is clean, run the pack build with `--strict-links` in CI so a
   new dead cross-link fails rather than being reported and scrolled past:

   ```sh
   python3 tools/genpack.py web/public/docs-pack/ --strict-links
   ```
