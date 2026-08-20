# stdlib/args.tur types its handles and option defaults as bare :int

**Severity: medium** -- a "No Lazy `:int`" violation in first-party stdlib
API: the spec/result handles and the option-default slot are `:int`, and the
default is a cstr smuggled through `:int`, forcing every caller to define a
reinterpret helper. Found in the 2026-08-20 docs audit.

## Repro

`(args/spec-option spec "--count" "int" "1")` is a type error;
`tests/fixtures/args-defaults` works around it with a local
`(defn cstr->int [s : cstr] : int ...)`.

## Root cause

stdlib/args.tur:35-129 (`spec : int`, `dflt : int`), :379+ (`result : int`).

## Fix direction

`defopaque ArgSpec`/`ArgResult` newtypes; `dflt : (Option cstr)`.

## Guides to update when fixed

- docs/guides/cli-args-guide.md (Building-a-Spec note, the cstr->int helper,
  API table)
