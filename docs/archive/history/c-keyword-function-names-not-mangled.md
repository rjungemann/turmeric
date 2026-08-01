---
status: resolved
severity: medium
discovered: 2026-07-26
resolved: 2026-07-29
area: compiler (name mangling, src/compiler/mangle.c + emit_core.c / elab_core.c / types.c)
---

# A function named after a C keyword emits an unmangled identifier

## Resolution (2026-07-29)

Fixed along fix direction 1, reusing the `tur_u_` guard prefix the libc-collision
guard already established (`codegen-user-defn-collides-with-libc-pipe2`).

- `tur_name_is_c_keyword` in `src/compiler/mangle.c` -- the complete C89-through-C23
  reserved-word set plus `asm`/`typeof`, `bsearch`ed like the libc table.
  `TUR_NAME_GUARD_PREFIX` in `mangle.h` is now the shared spelling of `tur_u_`,
  replacing the two open-coded `memcpy(p + k, "tur_u_", 6)` sites.
- Guard applied at three chokepoints:
  - `raw_name_for_binding` (`emit_core.c`) + its mirror `elab_mangle_binding_name`
    (`elab_core.c`) -- covers bare globals (`double`) *and* the legacy-fold branch
    for parameters and locals (`f(int64_t double)`), which the libc guard does not
    reach. Module-qualified globals are untouched: `geom__double` is a fine C
    identifier.
  - `mangle_field_name` (`emit_core.c`) -- struct/ADT fields (`int64_t int;`),
    constructors, and dynvars.
- Two straggler copies of the same fold had to be brought into lockstep, or the
  typedef and its use sites named different types: `append_c_ident_mangled` and
  `adt_byval_c_name` in `src/compiler/types.c`. `adt_byval_c_name` now delegates to
  `append_c_ident_mangled` instead of open-coding a third copy. (An ADT *type* name
  was never a bare collision -- it is always spelled `tur_adt_<name>` -- but all
  three manglers still have to agree on it.)

Verified with a sweep of every keyword in all three positions. What remains
failing in that sweep is unrelated and pre-existing: `if`, `for`, `while`, `do`,
`case`, `return`, `true`, `false`, `int`, `bool` are Turmeric special forms or
builtin type names, so those definitions are rejected (or shadowed at the call
site) long before codegen. That is a Turmeric-level naming question, not the C
cascade this report is about.

Fixtures: `tests/fixtures/c-keyword-defn-name/`, `c-keyword-param-name/`,
`c-keyword-struct-field/` -- one per bucket, as fix direction 3 asked for. Unit
coverage for the predicate (including sort-order probes, since a mis-sorted
`bsearch` table silently stops matching) in `tests/mangle_test.c`.

Zero fixture churn -- no existing fixture used a keyword name, and the guard is a
no-op for every other spelling. `bash tests/run.sh`: 2402 passed, 0 failed.
Guide updated: [name-mangling-guide.md](../../guides/name-mangling-guide.md#the-tur_u_-guard-prefix----names-c-already-owns).

Not addressed (explicitly out of scope in the report itself): collisions with
*identifiers* the emitted preamble declares (`RcControlBlock`, `tur_poly_fn_t`,
`malloc`, ...). The libc denylist covers the common libc half of that class and
is grown on demand.

## Original report

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
