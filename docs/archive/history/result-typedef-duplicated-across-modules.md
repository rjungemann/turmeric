# Fix history: Result<A,B> typedef duplicated across module headers

Resolved report: [`docs/archive/result-typedef-duplicated-across-modules.md`](../result-typedef-duplicated-across-modules.md).

## Symptom

Under *separate compilation*, each module's generated `.h` emits a verbatim
`typedef struct Result__cstr__cstr { ... } Result__cstr__cstr;`. When one TU
includes two such headers (e.g. `app__main.c` `#include`s both
`app__produce.h` and `app__consume.h`, both returning `(Result cstr cstr)`),
cc rejects the build:

```
error: redefinition of 'struct Result__cstr__cstr'
```

C treats each `typedef struct { ... } Name;` as a fresh anonymous tag, so even
textually identical typedefs collide.

## Fix

Direction (1) from the report -- the smallest patch that preserves the
per-module-header layout. Every monomorphic-instantiation typedef emitted by
`src/compiler/types.c` is now wrapped:

```c
#ifndef TUR_TY_Result__cstr__cstr
#define TUR_TY_Result__cstr__cstr
typedef struct Result__cstr__cstr { ... } Result__cstr__cstr;
#endif
```

Emitters: `emit_registered_struct_app_rec` and `emit_registered_adt_app_rec`.
Repeated identical emissions across headers landing in one TU are now
idempotent.

## Why the bug only shows under separate compilation

A plain `tur build <dir>` on a *single-`main`* project reroutes to a
whole-program single-file build (`cmd_build_project` -> `cmd_build`,
`src/main.c`) that inlines every transitively-imported module into one TU and
de-dups the typedef within that TU -- so it never produces two headers to
collide. The original repro, `tur build spices/tourist`, is a *shared library*
(no `main`) built via separate compilation (`cmd_build_multi_files`), which
emits one `.h` per module. That is the path the regression test drives.

## Validation

`tests/run-result-typedef-multi-module.sh` (ctest
`tur_result_typedef_multi_module`):

1. `tur build --shared <fixture>` -- separate compilation; asserts a clean
   build (no `redefinition of 'Result__cstr__cstr'`), the `.so` is produced,
   and the `TUR_TY_Result__cstr__cstr` guard appears in >= 2 module headers.
2. `tur build <fixture>` -- whole-program executable; runtime smoke that the
   Result values round-trip (program returns 42).

Load-bearing check (manual): stripping the guard lines from the generated
`app__produce.h` / `app__consume.h` and recompiling `app__main.c` reproduces
`error: redefinition of 'struct Result__cstr__cstr'` -- confirming the guard,
not luck, is what makes the build pass.
