# No re-export form -- forwarding a name from an imported module requires a wrapper defn

**Severity: low** (expressiveness hole, documented). Found in the 2026-08-20
docs audit.

## Repro

No `(export-from ...)` handling anywhere in src/compiler/elab_module.c; the
module-system guide's Limitations section promises "a future
`(export-from other-module foo bar)`".

## Fix direction

Accept `(export-from <mod> name...)` inside `defmodule`, resolving each name
through the imported module's export table and registering it in the
exporter's, reusing the existing mangled symbol (no wrapper emission).

## Guides to update when fixed

- docs/guides/module-system-guide.md (Limitations)
- docs/guides/developing-spices-guide.md (if it gains a re-export mention)
