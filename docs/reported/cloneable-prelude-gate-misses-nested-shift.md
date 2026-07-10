# Cloneable prelude gate misses a `cloneable-shift` nested under an operator

**Severity:** low (contrived nesting; a compile error, not a silent miscompile;
affects both backends).

## Summary

A `(cloneable-reset (cloneable-shift ...))` nested inside an operator argument
(e.g. `(+ 1 (cloneable-reset ...))`) does not get the cloneable-continuation
runtime prelude emitted, so `tur_cloneable_cont` is an unknown type at build.

## Minimal repro

```turmeric
(defn f [n : int] : int
  (+ 1 (cloneable-reset (cloneable-shift (fn [k] (+ n 5)) 0))))
(defn main [] : int (println (f 10)) 0)
```

```
error: unknown type name 'tur_cloneable_cont'
```

Fails on the default backend and (via the same gate) under
`--enable=cps-backend`.

## Root cause

The cloneable runtime prelude is gated on `emit_cps_program_uses_cloneable_dk`
(`src/compiler/emit_cps.c`), whose walker (`uses_cloneable_dk` /
`cps_expr_contains_cloneable_shift` in `src/passes/cps.c`) does not descend into
every operator/value form, so a `cloneable-shift` buried under an `EX_BUILTIN`
argument (or other unhandled form) is not detected and the prelude is skipped.
This is the same class of gap that `uses_callcc` had (fixed by making that
walker complete).

## Fix directions

- Make the cloneable-shift detection walker complete (descend into `EX_BUILTIN`,
  `EX_CALL` args, `EX_MATCH`, `EX_HANDLE`, casts/wrappers, ...), mirroring the
  `uses_callcc` completeness fix. Additive: it only ever emits the prelude when a
  `cloneable-shift` is actually present.
