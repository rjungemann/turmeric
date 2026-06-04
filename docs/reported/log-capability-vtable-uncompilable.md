# `stdlib/log.tur` and `stdlib/test/capability.tur` capability vtables are uncompilable (same nested-`static`-function pattern as io)

> **Resolved (this session).** Both modules now compile + run through the C
> backend, reusing the file-scope c-block hook landed with the io fix.
> - **Defect A** (`log.tur`): the shared `Logger` struct and every logger's
>   methods (timestamped, simple, null) were hoisted into a top-level
>   ` ```c ... ``` ` block; `Real-/Simple-/Null-Logger` now just allocate the
>   vtable and wire file-scope function pointers.
> - **Defect A** (`capability.tur`): all four mock capabilities
>   (`Test-FileSystem`/`Logger`/`Random`/`Time`) plus their singleton state were
>   hoisted to a single file-scope c-block, so the standalone accessor defns
>   (`testfs-clear`, `testlog-count`, ...) and the vtable methods share one copy
>   of the structs/globals (previously the globals were trapped inside the
>   constructor bodies, leaving the accessors referencing undefined symbols).
> - **Defect B** (`log.tur`): dropped the `extern-c stderr/stdout/time/ctime`
>   (and `fprintf`) decls; `<stdio.h>` comes from the preamble and the
>   timestamped logger `#include <time.h>` in its c-block.
> - Drive-by fixes while rewriting: annotated the untyped `:cstr` params on the
>   `log-*-direct` writers (silenced real `%s`/`int64_t` format mismatches),
>   cast handles cleanly in the `*-free` bodies, and replaced the
>   `Test-Random` LCG's signed-overflow UB (`int * 1103515245`) with unsigned
>   arithmetic (UBSan-clean).
>
> Validated by two new happy-path fixtures,
> `tests/fixtures/log-stdlib-roundtrip` and
> `tests/fixtures/capability-stdlib-roundtrip` (both also verified
> leak-clean + UBSan-clean under an `-fsanitize=address,undefined` build).

**Summary.** `stdlib/log.tur` (`Real-Logger`, `Simple-Logger`, `Null-Logger`)
and `stdlib/test/capability.tur` (`Test-FileSystem`) build their capability
structs the same way the now-fixed `Real-FileSystem` did: an inline-C body that
defines `static` helper functions *inside* the generated C function and stores
their addresses in a returned struct. Loading either module into a program that
reaches the C backend fails to compile. `log.tur` additionally hand-declares
libc globals/functions via `extern-c` in a way that conflicts with the real
headers.

**Severity.** Hard compile error for the whole module (same class as
`io-real-filesystem-and-list-dir-uncompilable`). Latent for the same reason: no
fixture compiles a program that loads these modules.

## Repro

```turmeric
(load "stdlib/log.tur")
(defn main [] : int 0)
```

```sh
./build/tur build repro.tur -o /tmp/repro   # cc invocation fails
```

Observed (abridged):

```
error: invalid storage class for function 'get_timestamp'
error: invalid storage class for function 'log_with_level'
error: 'stderr' redeclared as different kind of symbol
error: 'stdout' redeclared as different kind of symbol
error: conflicting types for 'time';  have 'void *(void *)'
error: conflicting types for 'ctime'; have 'const char *(void *)'
```

## Defect A -- nested `static` functions in the capability bodies

`Real-Logger` / `Simple-Logger` / `Null-Logger` (and `Test-FileSystem`) define
their `static` helper functions inside the enclosing C function and store their
addresses in the returned vtable struct -- identical to the old `Real-FileSystem`
(see `io-real-filesystem-and-list-dir-uncompilable.md`, Defect 1). `cc` rejects
the nested `static` definitions; even dropping `static` would dangle at runtime.

## Defect B -- `log.tur` `extern-c` collides with libc headers

```turmeric
(extern-c stderr [^] :ptr)   ; stderr is a FILE* object-like macro, not a function
(extern-c stdout [^] :ptr)
(extern-c time   [^ptr] :ptr)   ; lowers to void* time(void*) -- wrong prototype
(extern-c ctime  [^ptr] :cstr)  ; lowers to const char* ctime(void*) -- wrong prototype
```

`stderr`/`stdout` are not functions; declaring them as `extern int64_t
stderr(...)` redeclares the stdio macros/objects. `time`/`ctime` get the wrong
C signatures versus `<time.h>`.

## Proposed fix (mechanism now available)

The `file-scope-c-block` hook added while fixing the io report is the tool for
Defect A: hoist each logger's struct + helper functions into a top-level
` ```c ... ``` ` block (emitted at file scope), exactly as was done for
`Real-FileSystem`. For Defect B: drop the `extern-c stderr/stdout` declarations
and call them from inline-C that already has `<stdio.h>` in scope; either drop
the `extern-c time/ctime` lines and `#include <time.h>` in the inline-C that
uses them, or give them correct prototypes. `Test-FileSystem` in
`stdlib/test/capability.tur` needs the same Defect-A hoist.

## How to validate a fix

1. `(load "stdlib/log.tur")` + a logger round-trip must `tur build` and run.
2. Add a leak-checked happy-path fixture (parallel to
   `tests/fixtures/io-stdlib-roundtrip`).
3. `bash tests/run.sh` stays green.

## Relationship to the io fix

This shares root cause with `io-real-filesystem-and-list-dir-uncompilable.md`
(now resolved). That fix intentionally stayed scoped to `stdlib/io.tur`; these
sibling modules are recorded here so the identical pattern is not forgotten. The
reusable half of the fix -- file-scope top-level c-blocks -- already landed with
the io work, so closing this out is mostly mechanical hoisting plus untangling
`log.tur`'s `extern-c` declarations.
