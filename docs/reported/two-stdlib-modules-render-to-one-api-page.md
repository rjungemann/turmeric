# Two stdlib files claim `tur/capability` and render to one API page -- one is lost

**Severity: low** (docs completeness) -- `docs/html/api/tur-capability.html` is
written twice per `just docs` run; whichever file renders second wins, and the
other's API page silently does not exist on turmeric-lang.com. Found while
building the docs pack (OD1), which reported 146 modules parsed but 145 pages.

## Repro

```sh
just docs
```

The API step now prints:

```
  warning: stdlib/test/capability.tur and stdlib/capability.tur both render to
           api/tur-capability.html; the last one wins
```

and `docs/html/api/` holds a single `tur-capability.html` -- the test module's,
since it renders last. The index page still shows both cards (it groups by
subdirectory), so two links point at one page and one of them is wrong.

## Root cause

Neither file has a `(defmodule ...)` form, so `parse_tur_file`
(tools/gendocs.py:244) falls back to `'tur/' + file_stem`. Both files are named
`capability.tur`, so both become `tur/capability`, and `module_page_name`
(tools/gendocs.py:1269) derives the output filename from the module name alone
-- the subdirectory is not part of it.

The fallback is the load-bearing part: it is keyed on the *basename*, so any
two same-named files anywhere under `stdlib/` collide, and everything under
`stdlib/test/` is a same-named sibling waiting to happen. `render_index_page`
groups by `subdir`, which is why the index shows both cards while only one page
exists.

The docs pack inherits the collision by construction (one page, one slug) and
now reports it: `emit_pack_api` warns when two modules resolve to the same
fragment path (tools/gendocs.py:1810). The warning is the only signal today;
nothing fails.

## Fix direction

Two options, in preference order:

1. **Give the files real module names.** A `(defmodule tur/test-capability ...)`
   in `stdlib/test/capability.tur` makes the page `tur-test-capability.html`
   and the collision disappears with no generator change. It also removes the
   silent dependence on a filename-derived pseudo-name, which is what makes
   this fragile for every future file under `stdlib/test/`.
2. **Key the page name on the module's `rel_path`** when it lives in a
   subdirectory. This changes existing site URLs for every subdirectory module,
   so it wants a redirect story; not worth it for one collision.

Either way, promote the warning to an error once the tree is clean, so a third
colliding file is caught at `just docs` time rather than by a page count.
