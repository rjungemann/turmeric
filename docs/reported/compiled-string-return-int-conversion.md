# Compiled `String`-returning functions fail to build (`-Wint-conversion`)

**Severity:** High -- blocks *all* AOT-compiled use of the owned `String` type
under a modern clang/GCC where `-Wint-conversion` is a default-error. The entire
`String` adoption effort (stdlib + spices) targets compiled code; this makes the
compiled path unbuildable.

## Summary

Any program that `(load "stdlib/string.tur")` and is taken through the **compiled**
path (`tur build`, or `tur run`/`tur test` on a file that contains inline-C and
therefore cannot interpret) fails to compile: the emitted C for the `String`
wrappers returns a raw `void *` from a function declared `int64_t`, which clang
16+/GCC 14+ reject as `-Wint-conversion` (a hard error, not a warning).

The value is width-correct at runtime -- `void *` and `int64_t` are both 8 bytes on
LP64 -- so with the warning downgraded the program runs correctly. It is purely a
codegen type-hygiene defect.

## Minimal repro

```turmeric
(load "stdlib/string.tur")
(defn clen [s : cstr] : int
  ```c
  #include <string.h>
  return (int64_t)strlen((const char *)s);
  ```)
(defn main [] : int
  (let [s (string/from-cstr "hello")
        c (string/to-cstr s)
        n (clen c)]
    (do (string/release s) n)))       ; exits 5
```

`tur run repro.tur` (the inline-C forces compilation) emits, e.g.:

```
error: incompatible pointer to integer conversion returning 'void *'
       from a function with result type 'int64_t' [-Wint-conversion]
```

`TUR_CC_FLAGS="-Wno-error=int-conversion -Wno-error=incompatible-pointer-types" tur run repro.tur`
builds and exits 5 -- confirming the straddle is width-safe.

## Root cause

`String` is `(defopaque String :ptr<void>)` and the runtime declares the builders
as returning `void *`:

- `src/runtime/tur_string.h:34` `void *tur_string_from_cstr(const char *s);`
- `src/runtime/tur_string.h:46` `void *tur_string_from_int(int64_t v);`
- ... (`from_bytes`, `adopt_cstr`, `retain`, `concat`, `substring`, `to_upper`, ...)

But a tur `defn` that returns `String` is emitted with the ABI handle return type
`int64_t` (every handle in this ABI is an `int64_t`, bridged with
`(int64_t)(intptr_t)`). The String-wrapper emit path skips that bridge. Two emitted
shapes, both wrong:

1. Direct return of a `String`-typed call casts to the *value* type, not the slot
   type:
   ```c
   static int64_t __inst_Show_show_int32(int32_t x) {
       ...
       return (void*)tur_string_from_cstr(buf);   // void* returned from int64_t fn
   }
   ```
2. Panic-check temp is `__auto_type` (deduces `void *`), then returned:
   ```c
   static int64_t int_hy_gtstring(int64_t v) {          // int->string
       __auto_type __ps_192 = (tur_string_from_int(v)); // void*
       if (tur_panicking) return (int64_t){0};
       return __ps_192;                                 // void* from int64_t fn
   }
   ```

The pointer-arg mirror (`tur_string_concat((void*)(intptr_t)(a), ...)`) is fine on
the arg side but the same functions trip `-Wincompatible-pointer-types` in other
positions.

## Why it was never caught

No `String` fixture exercises the compiled path -- every `tests/fixtures/*string*`
runs in the tree-walking interpreter (no inline-C, no `requires.compiled`), which
never invokes `cc`. So the `-Wint-conversion` GCC14 "resolution" recorded in
`src/main.c` (and `docs/archive/codegen-gcc14-permerrors.md`) never covered the
String-return front.

## Fix directions

Real fix (remove the downgrade afterward): in the `String`/`ptr<void>`-returning
emit path, bridge the value into the `int64_t` C return slot with
`(int64_t)(intptr_t)` instead of `(void*)`/`__auto_type`. Two emit sites at least:
- the direct-return coercion (the `(void*)`-cast case), and
- the panic-check temp typing in `src/compiler/emit_expr.c` / `emit_fns.c` (the
  `__auto_type __ps_N` case) -- give the temp the slot type or cast on return.

A compiled-path `String` fixture (inline-C + `requires.compiled`) should land with
the fix so the front stays covered.

## Secondary blocker: `(load "stdlib/string.tur")` in an *imported* module

Independent of the codegen straddle, stdlib `String` cannot be pulled into a
**shared** spice module's signatures via `(load "stdlib/string.tur")`:

- Single file (`(load ...)` + `defmodule` + `main`): compiles and runs. OK.
- `(load ...)` in a module that is then **imported** by another module: fails
  at elaboration with `typeclass-show.tur:174: unknown function or operator
  'vec-show-loop'` -- the reentrant `string.tur -> typeclass.tur ->
  typeclass-show.tur -> (load string.tur)` chain leaves typeclass-show
  partially loaded (its own forward reference to `vec-show-loop` at :174,
  defined at :163, comes back unresolved) when the loading module is imported.
- `(load ...)` only in the entrypoint (importer) works, because the
  entrypoint's top-level loads run before the imported deps elaborate -- but
  that forces every downstream consumer of the spice to `(load
  "stdlib/string.tur")` before importing it, which is not an acceptable public
  API contract and is itself fragile.

Root cause is in the load/import elaboration ordering + the string<->typeclass
reentrant load guard, not in the spice. Until it is fixed, a spice cannot put
the stdlib `String` type in the signatures of modules that other modules
import.

**Workaround used by tourist-session:** a self-contained `session/ownstr`
module declares an ABI-identical `(defopaque String :ptr<void>)` plus thin
`extern` wrappers over the `tur_string_*` runtime (adopt-cstr / to-cstr /
release / len) and carries the `tur_string.c` `__tur_autolink__` marker. No
`(load)` anywhere -> no reentrancy; links cleanly. The bytes are the real
refcounted runtime String payload; only the nominal tur type is spice-local.
A proper fix to the load/import ordering would let spices use stdlib `String`
directly and retire `session/ownstr`.

## Current mitigation (landed with this report)

`src/main.c` re-adds `-Wno-error=int-conversion` and
`-Wno-error=incompatible-pointer-types` (both `build` and `emit-c`+build sites),
documented as the still-open String front, mirroring the retained
`-Wno-error=implicit-function-declaration`. This unblocks compiled `String` (and
the spice `String` adoption) at the cost of leaving these two warnings
non-fatal until the codegen bridge lands.
