# Globally-installed spices cannot be consumed as libraries via :global deps

**Severity: low** (expressiveness hole; documented as "v2") -- a spice
installed with `tur install` cannot be declared as a manifest dependency.
Found in the 2026-08-20 docs audit.

## Repro

`(defpackage app :spices #map{"notebook" #map{:global true}})` + `tur fetch` --
the `:global` key is not parsed (no handling in src/compiler/pkg.c's
manifest-dep loop; only `:url`/`:ref`/`:subdir`/`:path`/`:optional` shapes
exist).

## Root cause

Never implemented; design sketch in docs/archive/global-spice-install-plan.md
("Imports from global spices").

## Fix direction

Add the `:global true` dep shape to `pkg_manifest_read`'s `:spices` parsing,
resolve against `tur_global_spices_dir()` (src/compiler/install.c), validate
version, record the resolved SHA in tur.lock; add a `:global-policy` knob.

## Guides to update when fixed

- docs/guides/developing-spices-guide.md ("Global Spices as Libraries (v2)")
- docs/guides/consuming-spices-guide.md
