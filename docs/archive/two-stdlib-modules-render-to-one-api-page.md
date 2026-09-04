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

---

## Resolution (2026-08-26)

Fixed in `tools/gendocs.py`. `just docs` now writes **148 pages for 148
modules** (was 147 for 148) and the pack emits 148 fragments.

Neither option in the fix direction, in the end. Option 1 (give
`stdlib/test/capability.tur` a real `(defmodule ...)`) turned out to be a
semantic change, not a naming one: `defmodule` wraps the whole file body in a
form and namespaces it, and that file is pulled in textually by
`tests/fixtures/capability-stdlib-roundtrip/input.tur` via
`(load "stdlib/test/capability.tur")` -- a `load`, not an `import`. Wrapping it
would have broken the fixture to fix a docs bug.

Option 2 was taken, but **scoped to the fallback**, which is where the report
correctly identifies the fragility: the pseudo-name now carries the
subdirectory. A file that *declares* a module name keeps that name and its URL
untouched, so there is no redirect story to write for `turi/eval` or any other
declared module -- the objection to option 2 applied to declared names, and
those are exactly the ones this leaves alone.

```
stdlib/capability.tur       -> tur/capability        (unchanged)
stdlib/test/capability.tur  -> tur/test/capability   (was tur/capability)
stdlib/seq/core.tur         -> tur/seq/core          (was tur/core)
```

The five `stdlib/seq/*` pages move as a side effect. That is a correction, not
collateral: `stdlib/seq/core.tur` was publishing itself as `tur/core`, a name
that claims to be a core module. Nothing in the tree links to the old page
names (checked across `.md`, `.html`, `.js`, `.py`), and none of the affected
modules had an entry in `stdlib/docstrings.tur`, so no `(doc 'tur/...)` lookup
changes.

### A second, worse collision found in the same function

`stdlib/turi/eval.tur` was publishing itself as **`myplugin/core`**, on a
`myplugin-core.html` page, with no page at all for `turi/eval`.

The `defmodule` scan was a plain regex over every line, comments included. That
file's module docstring shows

```turmeric
;;   (defmodule myplugin/core
```

at line 5 as an *example*, and its own declaration sits at line 20. First match
wins. This is the same class of defect as the report's -- a module whose page
silently is not what it says -- and strictly worse, since the name was not even
one of the two candidates. Step 1 now scans through `strip_line_comment`, which
drops `;` comments while respecting string literals.

### The warning is now an error (the report's closing ask)

Two sites, since the site build and the pack build enumerate separately:

- `main()` checks `module_page_name` collisions across all parsed modules
  *before* rendering anything, and exits 1 naming both files. Previously the
  site path did not check at all -- the only signal was a page count one short
  of the module count, which is how this went unnoticed.
- `emit_pack_api`'s existing warning is now a `SystemExit`.

Verified both ways: two files that collide exit 1 with a message naming both;
the clean tree exits 0.
