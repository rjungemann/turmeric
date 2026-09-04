# Serializable-continuations docs describe a typed wrapper surface that does not exist

**Severity: medium** (RESOLVED 2026-09-02) -- `serial-resume`, `serial-cont->bytes`,
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

## Resolution (2026-09-02)

Both halves of the fix direction, split by what the runtime can honestly
back:

- **Built** (`stdlib/serial.tur`): `serial-cont->bytes [k : serial-cont] :
  ptr<void>` (the typed spelling of `save-cont!`), `serial-resume
  [k : serial-cont v : int] : int` (`(k v)`), and `bytes->serial-cont
  [b : ptr<void>] : (Result serial-cont cstr)`, which validates the whole
  record stream -- frame count, tags, call-frame names against this program's
  `__sk_registry`, env kinds and bounds -- before calling
  `tur_serial_cont_deserialize`, so a foreign or damaged buffer is an `Err`
  with a static message (`unknown frame (written by a different program?)`,
  `truncated frame`, `bad frame tag`, ...) instead of an abort or a resumed
  garbage chain. `save-cont!`/`resume-cont!` stay as the SemVer-pinned
  workflow surface.
- **Rewritten around the shipped surface**
  (`docs/guides/serializable-continuations-guide.md`): the `(serial-shift [k]
  body)` binder became the real `(serial-shift handler default)`; the
  `serial-continuation<T>` struct, `ResourceSerializable`, `SchemaMismatch`,
  the circular-reference detector and `defworkflow-step` are gone, replaced by
  the `serial-cont` handle, the stable-representation pattern, the
  `bytes->serial-cont` error list, the by-value marshalling rule, and the
  handler-that-does-not-resume workflow shape. The web-continuations guide and
  tutorial use the same handler form and `serial-cont` type; the checkpointing
  guide lists the trio. All turmeric/sweet-exp pairs parse-equal
  (`tools/check-guide-pairs.py`: 28/28).

Pinned by `tests/fixtures/serial-typed-surface`: capture, direct
`serial-resume`, rebuild from bytes and from a file (15), and four `Err`
paths (bad tag, unregistered frame name, truncated stream, missing file).
