---
title: Name Mangling Guide
category: Compiler Internals
description: How Turmeric turns source names into valid C identifiers -- the injective scheme, the legacy fold, and when each applies.
---

# Name Mangling Guide

## Why mangling exists

Turmeric identifiers can contain characters that are illegal in C (`-`, `/`, `?`,
`!`, `>`, `<`, ...). The compiler maps every source name to a valid C identifier
before emitting code. Two requirements govern that mapping:

1. **Injectivity** -- distinct source names must map to distinct C identifiers.
   Without this, `(defn foo-bar [] : int 1)` and `(defn foo_bar [] : int 2)`
   would both emit `foo_bar`, a hard C redefinition error.
2. **Invertibility** -- given a C symbol, the original source name should be
   recoverable (useful for ABI traces, `nm` output, crash backtraces).

## The injective scheme (linker-visible symbols)

All global, linker-visible symbols use the injective scheme defined in
`src/compiler/mangle.c` / `src/compiler/mangle.h`.

| Source byte | Output | Notes |
|-------------|--------|-------|
| `[A-Za-z0-9]` | itself | unchanged |
| `_` (literal underscore) | `_un` | was passthrough in old scheme |
| `-` (hyphen / kebab) | `_hy` | was `_` in old scheme |
| `/` (slash, if raw) | `_sl` | was `_` in old scheme |
| `>` `<` `=` `?` `!` ... | `_` + 2-letter mnemonic | e.g. `>` -> `_gt`, `?` -> `_qu` |
| any other byte | `_xHH` (hex) | escape hatch |
| `__` (double underscore) | **reserved structural separator** | module-path boundaries only |

### Why a single `_` always introduces an escape

Because a literal `_` in a source name encodes as `_un`, a lone `_` in the
output can never be data. Every `_` in a mangled identifier is the start of a
two-letter mnemonic (`_hy`, `_gt`, `_un`, ...) or the four-byte hex escape
`_xHH`. This makes the encoding self-delimiting and the demangler
unambiguous.

### Why `__` is exclusively structural

Because data can never produce two adjacent underscores (a literal `_` in the
source encodes as `_un`, and two adjacent `_un` encodings produce `_un_un`),
`__` is free to mean "module/namespace boundary". The module prefix convention
`<module>__<name>` (e.g. `geom/vector` -> `geom__vector__`, binding `add2` ->
`geom__vector__add2`) works exactly because of this invariant.

### Worked examples

| Source name | Injective C spelling | Note |
|-------------|----------------------|------|
| `foo-bar` | `foo_hybar` | kebab hyphen |
| `foo_bar` | `foo_unbar` | literal underscore |
| `eq?` | `eq_qu` | question-mark sigil |
| `>>>` | `_gt_gt_gt` | three `>` sigils |
| `list->vec` | `list_hy_gtvec` | distinct from `list/vec` |
| `list/vec` | `list_slvec` | raw slash (within a name) |
| `geom/vector` (module prefix) | `geom__vector__` | `/` -> `__` structural sep |
| `add2` in `geom/vector` | `geom__vector__add2` | full qualified name |

### Demangling

`tur_demangle(mangled, out, cap)` (in `src/compiler/mangle.c`) inverts the
encoding: copy alnum; on `__` emit `/`; on a lone `_` read the next two bytes
as a mnemonic or `x`+two hex digits. The decoded form is never longer than
the input, so `cap >= strlen(mangled) + 1` always suffices. Returns 0 on
malformed input.

## The legacy fold (function-local names, struct fields, file basenames)

Some contexts intentionally use the older lossy fold:

| Context | Scheme | Reason |
|---------|--------|--------|
| Function parameters and locals | legacy fold | inline-C bodies reference them by source spelling (`my-param` -> `my_param` in C) |
| Struct fields | legacy fold | same: inline-C accesses `obj->my_field` |
| `extern-c` bindings | legacy fold | the C function already has a fixed name |
| File base names (`foo-bar.c`) | legacy fold | filesystem names; injectivity is irrelevant, long mnemonics hurt readability |

The legacy fold is implemented as `tur_mangle_legacy_append` in `mangle.c`:
`[A-Za-z0-9_]` passes through, `-`/`/` become `_`, other sigils get their
two-letter mnemonic, remaining bytes get `_xHH`. Crucially, a literal `_` is
*not* re-encoded, so `foo_bar` and `foo-bar` both produce `foo_bar`.

**Locals cannot collide across the program** (they are block-scoped), so the
lossy fold is safe there. Struct fields are scoped to their struct, so no
linker collision is possible.

## The `tur_u_` guard prefix -- names C already owns

Both schemes above pass a pure `[A-Za-z0-9_]` name through byte for byte. That
is correct for injectivity but not sufficient for *validity*: some spellings are
already claimed on the C side, and emitting them verbatim produces a translation
unit that does not compile. Two such classes get a `tur_u_` prefix at the
mangling chokepoint:

| Class | Predicate | Symptom without the guard |
|---|---|---|
| libc / POSIX symbols the system headers declare | `tur_name_collides_libc` | `static int64_t read(...)` -- "static declaration of 'read' follows non-static declaration" |
| C reserved words, C89 through C23 | `tur_name_is_c_keyword` | `static int64_t double(int64_t);` -- a syntax error that derails the rest of the file |

The guard is applied by `raw_name_for_binding` (`emit_core.c`) and mirrored
byte-for-byte by `elab_mangle_binding_name` (`elab_core.c`), so a definition,
every call site, and an inline-C `__TUR_CNAME_` splice all resolve to the same
C name.

Where it applies differs slightly between the two:

- **libc collisions** only matter for a *bare* global. A module-qualified
  global is already `geom__read`, which no header declares, and an `extern-c`
  binding names the real libc symbol on purpose.
- **Keywords** also only matter unqualified (`geom__double` is a fine C
  identifier), but they additionally hit the legacy-fold contexts that libc
  collisions cannot: a **parameter or local** (`f(int64_t double)`) and a
  **struct field** (`int64_t int;`). `mangle_field_name` in `emit_core.c`
  carries the same guard for fields, ADT constructors, and dynvars.

  Guarding a parameter or field is safe for inline-C precisely because the
  unguarded spelling could never have been referenced -- an inline-C body
  naming `double` would not have parsed either.

A user name spelled literally `tur_u_double` cannot alias the guarded form of
`double`: under the injective scheme its literal underscores encode as `_un`,
so it mangles to `tur_unu_undouble`.

Note that `tur_demangle` does not strip the guard -- a guarded symbol decodes
with the prefix still attached. The guard sits *outside* the encoded region, and
both classes are rare enough that ABI traces reading `tur_u_double` are clearer
than a demangler that would have to guess whether `tur_u_` was data.

The keyword table in `mangle.c` is complete by construction (the standard fixes
the set); the libc table is grown as real collisions surface, since an
over-broad entry renames a user's function for no reason. Both are `bsearch`ed,
so both must stay sorted -- `tests/mangle_test.c` probes across each table so a
mis-sorted entry fails loudly rather than silently ceasing to match.

## Module/file-name split

`mangle_mod_basename` in `src/main.c` maps `/` -> `__`, `-` -> `_` for the
on-disk `.c`/`.h` filenames emitted by `tur build`. This is Option A from the
plan: filenames stay legacy-lossy, only *symbol* names go injective.

The linker-visible binding prefix (e.g. `my-mod__fn`) still uses the injective
scheme via `raw_name_for_binding`, so `my-mod/fn` and `my_mod/fn` emit
distinct symbols even though their generated filenames might collide.

## API reference

| Function | Location | Use for |
|----------|----------|---------|
| `tur_mangle_append(dst, pk, name, len)` | `mangle.c` | Append mangled bytes into a pre-allocated buffer |
| `tur_mangle_legacy_append(dst, pk, name, len)` | `mangle.c` | Append legacy-folded bytes |
| `tur_mangle_ident(name, out, cap)` | `mangle.c` | NUL-terminated convenience wrapper |
| `tur_demangle(mangled, out, cap)` | `mangle.c` | Inverse: C identifier -> source name |
| `tur_mangle_bound(src_len)` | `mangle.h` | Worst-case output length (4x input) |
| `tur_name_is_c_identifier(name, len)` | `mangle.h` | True if name needs no mangling |
| `tur_name_collides_libc(name, len)` | `mangle.c` | True if the name is a libc/POSIX symbol -- guard with `tur_u_` |
| `tur_name_is_c_keyword(name, len)` | `mangle.c` | True if the name is a C reserved word -- guard with `tur_u_` |

## Spice manifests

The `exports.manifest` file generated by `tur build --manifest <path>` stores
`<mod>/<name> -> <mangled> :: ...` lines, where `<mangled>` is the C symbol
as emitted by the injective scheme. The spice loader (`src/turi/spice_loader.c`)
reads these at runtime via `dlsym`. As long as the manifest is regenerated
whenever the `.so` is rebuilt (`tur repl` handles this automatically), no
manual update is needed when the mangling scheme changes.

## See also

- Scheme implementation: [`src/compiler/mangle.c`](https://github.com/rjungemann/turmeric/blob/main/src/compiler/mangle.c), [`src/compiler/mangle.h`](https://github.com/rjungemann/turmeric/blob/main/src/compiler/mangle.h)
- Unit test (oracle + round-trip + injectivity): [`tests/mangle_test.c`](https://github.com/rjungemann/turmeric/blob/main/tests/mangle_test.c) /
  `tur_mangle_unit` ctest target
- Regression fixtures: [`tests/fixtures/mangle-kebab-snake-coexist/`](https://github.com/rjungemann/turmeric/tree/main/tests/fixtures/mangle-kebab-snake-coexist) and
  [`tests/fixtures/mangle-arrow-name-vs-module/`](https://github.com/rjungemann/turmeric/tree/main/tests/fixtures/mangle-arrow-name-vs-module)
- Plan: [reversible-name-mangling-plan](https://github.com/rjungemann/turmeric/blob/main/docs/archive/history/reversible-name-mangling-plan.md)
