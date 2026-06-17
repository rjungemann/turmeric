## Result<A,B> typedef duplicated across module headers -> C redefinition error

**Status:** RESOLVED. Fixed via direction (1) -- a per-instantiation include
guard (`#ifndef TUR_TY_<Name> / #define / #endif`) wrapped around every
monomorphic Result/Option/ADT-application typedef in `src/compiler/types.c`
(`emit_registered_struct_app_rec` + `emit_registered_adt_app_rec`). Pinned by
the dedicated test `tests/run-result-typedef-multi-module.sh` (ctest
`tur_result_typedef_multi_module`), which drives the *separate-compilation*
(shared-library) build -- the exact shape of the original repro,
`tur build spices/tourist` -- so two module headers (`app__produce.h`,
`app__consume.h`) both emit the guarded typedef and `app__main`'s TU includes
both without colliding. Verified load-bearing: stripping the guard from the
generated headers reproduces `redefinition of 'struct Result__cstr__cstr'`
exactly. See history note in `docs/archive/history/`.

**Severity:** Blocking for multi-module spices that return the same Result instantiation.

When two modules in a single project both export functions whose return type
is the same Result instantiation (e.g. `(Result cstr cstr)`), each module's
generated `.h` emits a verbatim `typedef struct Result__cstr__cstr { ... }
Result__cstr__cstr;` declaration at file scope. As soon as a third module
imports both of them transitively, the C compiler sees two identical typedefs
and rejects the build with:

```
error: redefinition of 'Result__cstr__cstr'
error: typedef redefinition with different types
       ('struct (unnamed struct at .../tourist__param.h:138:16)'
        vs 'struct Result__cstr__cstr')
```

The error fires even though the two typedefs are textually identical -- C
treats each `typedef struct { ... } Name;` as defining a *fresh* anonymous
struct tag, so the second one is not "redefinition with same type" but
"redefinition with a different (anonymous) type."

### Repro

Currently triggered while migrating the tourist/httpd request path off `:int`
stand-ins for `result<cstr>`:

- `spices/httpd/src/httpd/request.tur` -- `req-header : (Result cstr cstr)`
- `spices/tourist/src/tourist/param.tur` -- `param`, `capture : (Result cstr cstr)`
- `spices/tourist/src/tourist/router.tur` -- `captures-get : (Result cstr cstr)`

`tur build spices/tourist` (which has `tourist__routing.c` transitively
including `tourist__param.h` + `tourist__router.h` + `httpd__request.h`)
fails with the redefinition diagnostic. The functions check fine
individually (`tur check <file>` passes); the failure is purely at link-
time codegen when multiple headers land in one translation unit.

### Root cause

Each module's `.h` writer emits the typedef for any Result/Option
monomorphic instantiation used in the module's exported signatures.
There is no per-instantiation include guard and no shared "shapes" header,
so identical typedefs collide.

Search points: `emit_module.c` Result/Option typedef emission for export
headers (look for emitters that write `typedef struct Result__` or the
matching Option helpers).

### Proposed fixes

Three viable directions:

1. **Per-instantiation include guard.** Wrap each typedef in
   `#ifndef TUR_TY_Result__cstr__cstr / #define / #endif` so multiple
   identical emissions are idempotent. Cheapest; preserves current layout.
2. **Shared shapes header.** Hoist all Result/Option instantiations a
   project needs into a single generated `tur_shapes.h` that every module
   includes; per-module headers stop emitting the typedefs themselves.
3. **Use named struct tags + `typedef struct Tag Tag;`** declared once
   and *defined* once -- C lets forward typedef + later struct definition
   compose, so multiple forward typedefs are legal.

(1) is the smallest patch and unblocks the spices migration immediately.
(2) is cleaner long-term and aligns with the monomorphization north-star.

### Validation

After a fix, the four-function migration in `turmeric-spices` (tourist
`param`, `capture`, `captures-get`; httpd `req-header`, all returning
`(Result cstr cstr)`) should `tur build spices/tourist` cleanly without
needing to revert any signature back to `:int`.
