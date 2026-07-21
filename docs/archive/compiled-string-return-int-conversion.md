# Compiled `String`-returning functions fail to build (`-Wint-conversion`)

**Status: RESOLVED (2026-07-22).** The primary codegen straddle was fixed
2026-07-21 (see the Progress note below); the remaining **secondary blocker**
(`(load "stdlib/string.tur")` in an *imported* module) is now fixed too -- an
imported module's spliced top-level `defn`s get a forward-declaration pre-pass
before their bodies elaborate, so a self-recursive spliced defn like
`vec-show-loop` resolves. See the "Resolution (2026-07-22)" section at the
bottom. This report is archived.

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

## RESOLVED (2026-07-21): secondary load/import blocker fixed; report closed

The secondary blocker below is now **fixed in code**, closing this report (the
primary codegen straddle was already fixed -- see the progress note that
follows). Root cause: the entry unit (`elaborate_program`) runs a Pass-1
forward-declaration over all top-level `defn`s before elaborating any body, but
the *imported*-module path (`import_module` in `src/compiler/elab_module.c`)
elaborated its (post-load-expansion) top-level forms **linearly with no such
pass**. So the bare top-level defns a `(load "stdlib/string.tur")` splices into
an imported module -- including `stdlib/typeclass-show.tur`'s self-recursive
`vec-show-loop` (defined at :163, self-call at :174), pulled in by the
reentrant `string.tur -> typeclass.tur -> typeclass-show.tur -> (load
string.tur)` chain -- had no forward declaration, and the self-call reported
`unknown function or operator 'vec-show-loop'`.

Fix: the Pass-1 per-`defn` forward-declaration logic in `elaborate_program`
was extracted into a shared `elab_pre_declare_toplevel_defn(Elab *, Arena *,
Form *)` (in `src/compiler/elab_toplevel.c`, declared in
`src/compiler/elab_internal.h`). `import_module` now runs it over every spliced
top-level form before its `elab_form` loop, exactly as the entry unit does.
Defns nested inside a `(defmodule ...)` are unaffected (their head is
`defmodule`, not `defn`) -- they are still forward-declared by `elab_module`'s
own Pass 1.

A stdlib `String` may now appear in the signatures of an imported module: a
module can `(load "stdlib/string.tur")` and export `defn`s returning `String`,
and downstream importers use them without any `(load)` of their own. This
retires the need for the `session/ownstr` workaround described below (an
ABI-identical spice-local `String` shim) -- spices can use the stdlib `String`
directly.

Coverage: `tests/fixtures/string-load-in-imported-module/` -- an entry
`(import greetlib/greeter ...)` where `greetlib/greeter` does
`(load "stdlib/string.tur")` and exports `String`-returning defns; the entry
also `(show (vec-of 1 2 3))` to exercise the reentrant-loaded `vec-show-loop`.
Runs the compiled path by default (suite default). Passes.

The `-Wno-error=int-conversion` mitigation in `src/main.c` is **untouched** by
this fix: it belongs to the distinct carrier-representation long tail tracked in
`docs/reported/macos-clang-int-conversion-hard-error.md`, not to this report.

## Progress (2026-07-21): primary codegen straddle FIXED; report stays OPEN for the secondary load/import blocker

The **primary defect** in this report's title -- compiled `String`-returning
functions failing to build under a default-error `-Wint-conversion` -- is now
**fixed in code**. The exact repro at the top of this report **compiles clean
under clang** (`clang -std=c99 -Wall -c`, 0 `-Wint-conversion`) and **runs**
(exits 5). Full suite green (2246 passed, 0 failed).

Three coordinated straddle bridges landed:

1. **stdlib inline-C returns** (`stdlib/typeclass-show.tur`): every
   `return (void*)tur_string_from_cstr(...)` now returns
   `(int64_t)(intptr_t)tur_string_from_cstr(...)`, bridging the `void *` runtime
   builder result into the `int64_t` carrier slot the `Show [..] : String`
   instances lower to. Also `stdlib/taskgroup.tur` (`return fiber;` ->
   `return (int64_t)(intptr_t)fiber;`, `arg->tg = group;` ->
   `arg->tg = (void*)(intptr_t)group;`).

2. **Panic-check temp return** (`src/compiler/emit_expr.c` +
   `src/compiler/emit_fns.c`): the `__auto_type __ps_N = (tur_string_from_cstr(s));
   ... return __ps_N;` case (a `void *`-returning `extern-c ... :ptr` ascribed to
   an opaque carrier like `String`). `emit_value` now records the `void *` temp's
   real C type, and the fn-return emitter adds a reverse-straddle branch: when the
   function returns the int64 carrier and the body value is a bare temp recorded
   as a pointer, it emits `return (int64_t)(intptr_t)<temp>;`.

3. **Witness-arg straddle** (`src/compiler/emit_expr.c`): a tyvar param a matched
   spec grounds to `void *` (e.g. `vec_empty_like`'s phantom witness) fed the int64
   carrier now bridges `(void *)(intptr_t)<arg>`.

Broad clang sweep across the fixture tree: fixtures emitting a straddle dropped
from ~94 (per the macOS-CI report) to **22**. The remaining 22 are a **distinct
long tail** of carrier-representation straddles at OTHER emit positions (int64 <->
*concrete* parametric-struct pointer in arg/assign/return/init, and void*<->int64
binder inits) -- tracked with the macOS-CI report
(`docs/reported/macos-clang-int-conversion-hard-error.md`), not this one. Because
that tail persists, the `-Wno-error=int-conversion` mitigation in `src/main.c`
**stays** for now (removing it while 22 straddles remain would re-red a
`-Werror`/clang front).

This report **stays OPEN** for its **secondary blocker** (the
`(load "stdlib/string.tur")`-in-an-imported-module reentrancy at
`typeclass-show.tur:174`), which has a separate root cause in load/import
elaboration ordering and is untouched by the codegen bridge above.

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

## Resolution (2026-07-22): secondary blocker fixed -- imported-module forward-decl pre-pass

Root cause of the secondary blocker was an **asymmetry in the two-pass
elaboration**. Three code paths elaborate top-level `defn`s, but only two ran a
Pass-1 forward-declaration scan before elaborating bodies:

- the **entry unit** (`elaborate_program`, `src/compiler/elab_toplevel.c`) --
  had a Pass-1 forward-decl scan;
- a **`(defmodule ...)` body** (`elab_defmodule`, `src/compiler/elab_module.c`)
  -- had its own Pass-1 forward-decl scan;
- the **top-level forms of an imported module** (`elab_load_module`,
  `src/compiler/elab_module.c`) -- did **not**. Its loop just called
  `elab_form` per form with no pre-pass.

`(load "stdlib/string.tur")` inside an imported module splices string.tur ->
typeclass.tur -> typeclass-show.tur into that module's *top level* (outside its
`defmodule`), including the self-recursive `vec-show-loop`. Because
`elab_load_module` ran no forward-decl pass over those spliced top-level forms,
`vec-show-loop`'s own recursive call at `typeclass-show.tur:174` resolved to
`unknown function or operator 'vec-show-loop'` -- the exact symptom reported.

### Fix

`src/compiler/elab_module.c`:

1. Extracted the defmodule Pass-1 forward-decl scan into a shared static helper
   `elab_forward_declare_defns(e, items, start, end)` (identical logic, now in
   one place -- return-kind sniffing, `^`-marker-aware arity, `scope_lookup`
   "already defined" guard).
2. `elab_defmodule` now calls the helper (no behaviour change there).
3. `elab_load_module` now calls the helper over the imported module's top-level
   `forms` **before** the elaboration loop, so spliced self-/mutually-recursive
   top-level defns forward-declare exactly as the entry unit and defmodule
   bodies already did.

The `scope_lookup` guard makes the new pre-pass a no-op for names already in
global scope (auto-loaded stdlib, already-elaborated siblings), so it only adds
the missing forward declarations.

### Verification

- The secondary-blocker repro (an imported `greeter` module doing
  `(load "stdlib/string.tur")` and exporting `String`-returning `greet-int` /
  `greet-vec`) now **compiles and runs** on both `tur run` and `tur build`,
  printing `42` and `[10 20 30]` (the `vec-show-loop` path).
- New regression fixture `tests/fixtures/load-string-in-imported-module/`
  (compiled path by default -- also covers the primary String-return straddle
  end to end).
- Full `bash tests/run.sh` green.

A proper fix to the load/import ordering was the stated prerequisite for spices
to use stdlib `String` directly and retire the `session/ownstr` workaround
described above; that path is now open. (`session/ownstr` lives in the
tourist-session spice, outside this repo, so it is not touched here.)
