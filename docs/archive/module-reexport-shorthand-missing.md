# No re-export form -- forwarding a name from an imported module requires a wrapper defn

**Severity: low** (expressiveness hole, documented). Found in the 2026-08-20
docs audit.
**Status: RESOLVED** -- `(export-from <mod> name ...)` is implemented.

## Repro

No `export-from` handling anywhere in src/compiler/elab_module.c; the
module-system guide's Limitations section promised "a future
`(export-from other-module foo bar)` form". Listing an imported name in a
plain `(export ...)` failed with *"exported symbol 'low-add' is not defined
in this module"*, since the marking pass requires
`binding->defining_module_name == mod->name`.

## Resolution

`(export-from <mod> name ...)` inside `defmodule`, parsed in the same
export/import consumption loop:

- `expr.h` -- `DefModule` gains parallel `reexports` / `reexport_srcs` arrays.
- `elab_internal.h` / `elab_core.c` -- `sym_export_from`.
- `elab_module.c` -- parse arm; a validation pass after body elaboration; and
  the export-collection step in `elab_load_module` extended so consumers of
  the re-exporting module actually see the names (both bindings and macros).

**No wrapper is emitted.** The consumer resolves to the *defining* module's
`Binding` and calls its mangled symbol directly, so a re-export costs nothing
at runtime regardless of how many hops it travels. Verified on the fixture's
three-hop chain: the emitted C holds exactly one `chain__low__low_hyadd` and
no `chain__mid__` / `chain__hi__` copy, with the call site dispatching to the
original.

### Design decisions

**The source module must be imported.** `export-from` deliberately does not
load. A second load path with its own resolution rules is how two modules end
up disagreeing about which file a name came from; the error names the missing
import.

**Exported, not merely defined.** Re-exporting a module-private name would
smuggle it into the public surface through a third party, which is what the
export list exists to prevent. The two failures get distinct diagnostics --
"defined by 'X' but not exported from it" versus "not exported by module 'X'"
-- because the first is one edit and the second is a hunt.

**Chains work.** The check is "does the source module *export* this name",
consulting its loaded export table (which itself carries re-exports), not
"did the source module *define* it". The first draft used the latter and
broke every chain at its second link: `hi` re-exporting from `mid` a name
`low` defined.

## Adjacent bug found and fixed: one macro, two import paths

Macro re-export did not work at first, and the cause turned out to be
independent of this feature. Macro registration is global, and the
import-time collision check rejected **any** re-registration of a name. So a
plain diamond -- top imports `low` and `mid`, and `mid` also refers `low`'s
macro -- reported `low`'s macro as conflicting with itself:

```
error: macro 'twice' from module 'dia/low' conflicts with an existing macro
```

Reproduced with no `export-from` involved, so it predates this work. A macro
is identified by (defining module, name), and a module cannot define two
macros with one name, so matching both means one definition reached twice --
now kept rather than rejected. A genuine collision (two different modules,
same macro name) is still an error, caught at `defmacro` time since
registration is global; the import-time branch survives as a second line of
defence and gained a note naming the other module.

## Tests

- `tests/fixtures/module-export-from` -- three-hop chain
  (`chain/low -> chain/mid -> chain/hi -> input.tur`) re-exporting both a
  `defn` and a macro twice.
- `tests/fixtures/module-same-macro-two-paths` -- the diamond above, pinning
  the macro fix independently of `export-from`.
- `tests/fixtures/errors/export-from-not-exported`,
  `.../export-from-unknown-name`, `.../export-from-module-not-imported`,
  `.../export-from-no-names` -- one per diagnostic, including the two that
  must read differently from each other.
- `tests/check-export-from-no-wrapper.sh` (ctest `tur_export_from_no_wrapper`)
  -- the no-wrapper invariant. The behaviour fixture prints the right numbers
  whether or not a wrapper is emitted, so the codegen shape needs its own
  guard.

Suites: run.sh 2669 passed / 0 failed; run-turi.sh 1840 passed / 0 failed.

## Guides updated

- docs/guides/module-system-guide.md -- Limitations bullet removed; new
  `(export-from ...)` subsection under Exports with a facade-module example
  in both dialects and the four rules above; the macros subsection gained the
  same-macro-two-paths note.
