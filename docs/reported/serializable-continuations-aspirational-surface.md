# Serializable-continuations docs describe a typed wrapper surface that does not exist

**Severity: medium** -- `serial-resume`, `serial-cont->bytes`,
`bytes->serial-cont`, the `serial-continuation<T>` struct, the
`(serial-shift [k] body)` binder syntax, `ResourceSerializable`, and the
SchemaMismatch/circular-reference error surface are all documented but
unimplemented. Found in the 2026-08-20 docs audit.

## Repro

`(bytes->serial-cont b)` -> unknown function. Every fixture (`cont-flavors`,
`context-*`, `cps-oracle-*`) uses only the shipped surface: `serial-reset`/
`serial-shift` special forms with `k : ptr<void>`, `stdlib/workflow.tur`'s
`save-cont!`/`resume-cont!` (+ `workflow-suspend`/`workflow-resume` aliases),
and `stdlib/serial.tur`'s `Serializable`/`Bytes`/`cont-to-file`/
`cont-from-file`. The C-level `serial_cont_to_bytes` (src/runtime/serial.c:279)
has no Turmeric binding by the documented names. Capture is further limited to
the single-scalar-hole context grammar (workflow.tur note;
docs/archive/history/serial-shift-unsupported-context-miscompile.md).

## Fix direction

Either build the typed wrapper surface over the existing runtime
(`serial-resume` as sugar for `resume-cont!`, a `serial-continuation<T>`
newtype over the DK chain, a Result-returning deserialize) or rewrite the
guide's Overview/Surface API/Examples around the shipped functions. The
sections that map 1:1 onto real code (class signature, instances, file
helpers) were already corrected in the audit.

## Guides to update when fixed

- docs/guides/serializable-continuations-guide.md (primary)
- docs/guides/checkpointing-guide.md
- docs/guides/web-continuations-guide.md
- docs/guides/web-continuations-tutorial.md
