---
status: open
severity: medium
discovered: 2026-07-26
area: compiler (name mangling, src/compiler/emit_module.c / name_mangle)
---

# A function named after a C keyword emits an unmangled identifier

## Summary

`(defn double [x : int] : int ...)` is a legal Turmeric definition. It lowers to
`static int64_t double(int64_t);` in the emitted C, which is a syntax error. The
mangler does not reserve C keywords, so any Turmeric name that happens to be one
produces a wall of cc errors with nothing wrong in the user's source.

Found while writing a throwaway helper called `double` for
`tests/fixtures/rc-tur-typeclass-instances`.

## Repro

    $ printf '(defn double [x : int] : int (* x 2))\n\n(defn main [] : int (println (double 21)) 0)\n' > /tmp/k.tur
    $ ./build/tur build /tmp/k.tur -o /tmp/k
    error: two or more data types in declaration specifiers
    error: 'int64_t' redeclared as different kind of symbol
    ...

`tur check` exits 0 -- the failure is entirely at the C stage.

Measured error counts for a one-line `(defn <kw> [x : int] : int x)`:

| name | cc errors |
|---|---|
| `return` | 2 |
| `struct` | 6 |
| `int` | 26 |
| `double` | 3 |
| `char` | 533 |
| `void` | 533 |
| `float` | 533 |

The large counts are cascade damage -- one bad declaration derails the rest of
the translation unit, so the reported line has no relation to the user's code.

## Impact

Low probability, bad experience. `double`, `float`, `int`, `char`, `short`,
`long`, `signed`, `unsigned`, `void`, `struct`, `union`, `enum`, `const`,
`static`, `extern`, `register`, `volatile`, `auto`, `inline`, `restrict`,
`return`, `if`, `else`, `while`, `for`, `do`, `switch`, `case`, `default`,
`break`, `continue`, `goto`, `sizeof`, `typedef`, and the C99/C11 additions
(`_Bool`, `_Static_assert`, ...) are all plausible domain words. `double` in
particular is an ordinary thing to call a numeric helper.

Names that are *identifiers* in the emitted preamble rather than keywords
(`main`, `free`, `malloc`, `value`) are a separate and probably wider class of
collision; this report is only about the keyword set.

## Fix directions

1. Add a reserved-word set to the mangler and suffix on hit (`double` ->
   `double_` or the existing escape scheme's `_kw` form). Cheap, contained, and
   consistent with how the mangler already escapes other unrepresentable
   characters -- see `docs/guides/name-mangling-guide.md`.
2. Cover the C keyword list including C99/C11 additions, plus the emitted
   preamble's own type names (`RcControlBlock`, `tur_poly_fn_t`, ...) if the same
   collision class is in scope.
3. A fixture per bucket (a keyword-named `defn`, `defstruct`, and field) so the
   set cannot silently drift.

Alternatively reject the name at elaboration with a real diagnostic pointing at
the definition. That is strictly worse than mangling -- it makes a legal
Turmeric name illegal for a C-backend reason -- but it is far better than the
current cascade.
