# `perform` in a function with an `any` parameter has no CPS lowering

**Severity: low** -- a narrow shape (gradual typing meets effects), and the
diagnostic is loud rather than silent. But its advice does not apply, which
costs the reader a while. Found 2026-08-29 while building the negative cases
for `any-struct-box-leak-per-widen`.

## Repro

```turmeric
(defeffect Ask [] :int)

(defn with-any [v : any] : int (perform (Ask)))

(defn main [] : int
  (println (handle (with-any 3) (Ask [] k) (resume k 5)))
  0)
```

```
error: this effect operation has no lowering here: the enclosing function left
the CPS backend's supported subset, and the direct emitter cannot lower
`perform`. The usual cause is a loop inside a `handle` clause -- hoist that
work into a helper function and call it from the clause. This is a compiler
limitation, not a mistake in this expression.
```

The parameter is not used. Changing its type to `:int` compiles and prints `5`,
so the `any` parameter alone is what pushes the function out of the supported
subset.

## Root cause

Not established. `any` lowers to `tur_tagged_t`, a 16-byte by-value struct, and
the CPS backend's atom/slot machinery admits a narrow set of parameter
representations (see the Tier C by-value-aggregate discussion around
`slot_box_ty` in `src/compiler/emit_cps_ir.c`) -- a by-value aggregate
parameter that is neither a carrier word nor a recognised monomorph is the
likely exclusion. Worth confirming before fixing.

## Why the diagnostic misleads

It names one cause ("a loop inside a `handle` clause") and prescribes hoisting
into a helper. Here there is no loop and no `handle` clause, and hoisting is
exactly what the code already does -- `with-any` *is* the helper. A reader
follows the advice, changes nothing, and has to go looking. Widening the
message to name the actual exclusion, or dropping the specific advice when the
shape does not match, would be worth as much as the lowering fix.

## Fix directions

1. Diagnostic first: report the real reason (an unsupported parameter
   representation) rather than the loop-in-handle guess.
2. Admit `tur_tagged_t` parameters to the CPS subset -- it is a fixed 16-byte
   by-value aggregate, so it should slot like any other narrow monomorph.

## Notes

Encountered as a blocked test case, not as a user report: the effect-free gate
in the `any` frame-box optimization
(`docs/reported/any-struct-box-leak-per-widen.md`) refuses a callee that can
suspend, and this is why that gate cannot currently be exercised end-to-end.
The gate stays in regardless -- it is cheap, and it is exactly what would go
wrong if this lowering gap were closed without it.
