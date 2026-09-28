# `#lang r7rs`: the prelude split prints wrong symbols on Windows

**Severity:** low while the split is off there, and it is: on Windows,
`prelude_split_applies` (src/main.c) declines unless `TUR_PRELUDE_SPLIT=1`.
Windows builds keep the one-unit build and its 3 s-per-program cost. The
split itself (docs/archive/r7rs-programs-compile-slowly.md) cuts that to
about 1 s everywhere else.

## Repro

MSYS2/UCRT64, from the first CI run of the split (rjungemann/turmeric#952,
`Windows build + suite 1/3`, 20 r7rs fixtures failing):

```sh
TUR_PRELUDE_SPLIT=1 ./build/tur run tests/fixtures/r7rs-type-errors-raise/input.tur
```

It prints `(|uired field| "car: not a pair" (5))` where the fixture expects
`(caught "car: not a pair" (5))`. `r7rs-vector-index-error` prints
`#(ray |!: out of memory|)` for `#(a b)`. Every quoted symbol comes out as the
tail of some unrelated string literal. The two units compile and link
without complaint, so the one-unit fallback (which retries on a cc or link
failure) never fires.

## Root cause (suspected, not confirmed on a Windows box)

Both units define the keyword records they reference, and the library unit
and the program unit share many of them (`'caught`, `'a`, ...).
`sym_codegen_emit` (src/compiler/emit_core.c) gives them
`__attribute__((weak)) const` in a split build, so that the linker folds each
name to one record (SYM2). ELF and Mach-O do fold them, and every symbol then
has one address. On PE/COFF, GNU ld implements a weak definition as a weak
external plus a default. A reference that should reach the folded record
lands on bytes elsewhere in `.rdata`, at an offset into another object's
string literals. That is what the output looks like: `uired field` is the
middle of `"required field"`.

## Fix directions

- **Stop relying on weak data in the split.** Fat boxes already take this
  shape. The library unit defines each keyword record it uses, with strong
  external linkage, and records its name. The program unit emits `extern`
  for those names and `static` records for keywords only it uses. The SYM5
  intern-table seed is unchanged, since first registration wins. This works
  on every platform. It also takes away the only reason the split needs
  weak symbols.
- Confirm the diagnosis first on a Windows host: `nm` both objects and the
  linked executable, and check whether `__tur_sym_*` resolves to one address.
