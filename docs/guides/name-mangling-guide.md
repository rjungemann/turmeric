---
title: Name Mangling -- Turmeric to C identifiers
category: Compiler Internals
description: How Turmeric binding names become C identifiers. The global-symbol scheme is injective and self-delimiting (a literal `_` encodes as `_un`, `-` as `_hy`, `/` as `_sl`); struct fields, dynvars, and function-locals keep the legacy `-`/`/` -> `_` fold for inline-C interop. Includes the demangler algorithm and the inline-C calling convention.
---

# Name Mangling -- Turmeric to C identifiers

Turmeric identifiers may contain characters a C identifier cannot (`-`, `/`,
`>`, `?`, `!`, ...). The compiler mangles them into valid C identifiers in
`src/compiler/mangle.c`. There are **two** schemes, applied to different
categories of name, plus a small set of verbatim/escape-hatch cases.

## 1. Injective scheme -- linker-visible global symbols

Top-level `defn`/`def` names (the C function/global symbols a program links
against) use the **injective, self-delimiting** encoding:

| Source byte | Encoding | Notes |
|-------------|----------|-------|
| `[A-Za-z0-9]` | itself | unchanged |
| `_` (literal) | `_un` | so a lone `_` always *introduces* an escape |
| `-` (hyphen) | `_hy` | |
| `/` (slash) | `_sl` | when a raw `/` reaches the mangler |
| operator sigils (`>` `<` `+` `=` `?` `!` ...) | `_` + 2-letter mnemonic | `>` -> `_gt`, `?` -> `_qu`, ... |
| any other byte | `_xHH` | two uppercase hex digits |
| `__` (double underscore) | structural separator | module/prefix boundary only |

Module-qualified globals keep `__` as the structural separator
(`geom/vector` `add2` -> `geom__vector__add2`); because a literal `_` is now
`_un`, two adjacent underscores can never arise from data, so `__` is
unambiguous.

### Why injective

The old scheme folded `-`, `/`, and a literal `_` **all** to `_`, so distinct
Turmeric names collided in C:

```turmeric
(defn foo-bar [] : int 1)
(defn foo_bar [] : int 2)   ;; old: both -> C `foo_bar` (redefinition error!)
```

The injective scheme spells them `foo_hybar` and `foo_unbar` -- they coexist.
See `tests/fixtures/mangle-kebab-snake-coexist/` and
`tests/fixtures/mangle-arrow-name-vs-module/`.

### Demangling

Because the encoding is self-delimiting, `tur_demangle` (in `mangle.c`) is the
exact inverse: scan left to right; copy alnum; on `__` emit `/`; on a lone `_`
read either `x` + two hex digits, or a two-letter mnemonic. A round-trip +
injectivity unit test lives in `tests/mangle_test.c` (`tur_mangle_unit`).

## 2. Legacy fold -- fields, dynvars, and function-locals

Struct field names, dynamic-variable names, and **function-local** bindings
(parameters and locals) keep the **legacy** fold: `[A-Za-z0-9_]` passes
through, `-`/`/` become `_`, sigils still get mnemonics. This is *deliberate*
and is the plan's "consciously documented" exception:

- These names are referenced from **inline-C** by their stable legacy spelling
  (a field `is-some` is read as `opt->is_some`; a parameter `val-cmp` is read as
  `val_cmp`). The injective scheme would change that spelling.
- Field names routinely **coincide** with parameter names (a `Zipper` field
  `left-len` and a `zipper-new` parameter `left-len`). Since parameters are
  legacy-folded, an injective field name would desync the two for the same
  inline-C token.
- Fields and locals are struct/function-scoped and **never** cause linker
  collisions, so injectivity buys nothing here.

## 3. Verbatim and escape-hatch cases

- **`extern-c` names** are real C symbols. They use the legacy fold via
  `mangle_field_name` (so `tur_hamt_new` stays itself and `tvar/new` becomes
  `tvar_new`), consistently at the prototype, every call site
  (`raw_name_for_binding` special-cases `is_extern_c`), and any inline-C
  reference.
- **`(export-as "name")`** pins an exact C name (`c_export_name`), bypassing
  mangling entirely -- the stable-name escape hatch.
- **Compiler-synthesized `__`-prefixed pure-C-identifier names**
  (anonymous lambdas `__fn_N`, instance dicts `__inst_*`, internal helpers like
  `__fiber_set_cancelled`) are emitted **verbatim** -- their use sites reference
  them by that exact spelling. The module prefix is still applied, so two
  module-private `__h` helpers stay distinct (`alpha____h` vs `beta____h`).
  Names that merely *start* with `__` but still hold kebab/sigil bytes
  (e.g. `__tg-async-entry`) are NOT pure C identifiers and go through the
  injective scheme like any other global.

## Calling Turmeric globals from inline-C

Inline-C bodies that call a Turmeric global must use its **mangled** C name.
Under the injective scheme a kebab/slash global re-spells:

```turmeric
(defn run-server [h : ptr<void>] : nil ...)
;; inline-C must call it as run_hyserver, NOT run_server:
(defn start [] : nil
  ```c run_hyserver(the_handle); ```)
```

To avoid hardcoding the spelling, inline-C can use the
`__TUR_CNAME_<source-name>__` splice, which routes `<source-name>` through the
same injective mangler the emitter uses:

```turmeric
  ```c __TUR_CNAME_run-server__(the_handle); ```
```

(`extern-c` names and `__`-prefixed pure-C-id helpers keep their verbatim
spelling and need no change.)
