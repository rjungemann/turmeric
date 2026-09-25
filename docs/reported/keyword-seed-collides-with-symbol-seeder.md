# The keyword `:seed` does not compile: `__tur_sym_seed` is taken

**Severity:** medium. A program whose unit carries the runtime symbol
registry fails at the C compile step if it spells the keyword `:seed`.
Every `#lang r7rs` program carries the registry (`string->symbol`). `seed`
is an ordinary word for a map key or an option name.

## Repro

```scheme
#lang r7rs
(import (scheme base) (scheme write) (turmeric stdlib/map))
(define m #map{:seed 1})
(write (map-count m))
```

```
$ ./build/tur build seed.tur -o seed
.../seed_tur.c:5750:13: error: '__tur_sym_seed' redeclared as different kind of symbol
```

A plain Turmeric program with `#map{:seed 1}` compiles and runs (checked),
because its unit emits no seeder.

## Root cause

When a unit has the runtime symbol registry, src/compiler/emit_core.c:6291
emits its seeding function as `static void __tur_sym_seed(void)`, and
registers it at :6297. Each interned keyword or symbol is a
`static const struct { ... } __tur_sym_<name>`, so the keyword `:seed`
declares the same identifier first (seen at line 5746 of the emitted C,
followed by the function at 5748).

## Fix directions

- Give the seeder a name no symbol can mangle to: one that does not start
  with the `__tur_sym_` prefix (for example `__tur_symtab_seed`). Update the
  `static_init_register` call to match, then regenerate the snapshots.
- Check the other `__tur_sym_*` helpers the emitter writes for the same
  collision.
