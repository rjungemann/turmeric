# `stdlib/io.tur` runtime path is uncompilable: `Real-FileSystem` nested `static` functions + `list-dir` missing `<dirent.h>` (and conflicting `extern-c` decls)

**Summary.** Loading `stdlib/io.tur` into any program that actually goes through
the C backend (`tur build` / `emit-c` then `cc`) fails to compile. The module's
type-checking is fine, but its generated C has three independent, latent
defects that have never been exercised because no fixture compiles a program
that `(load "stdlib/io.tur")`. These are *separate* from the
`io-file-open-untyped-params-default-to-int` finding (which has been fixed); they
are what remains blocking a full happy-path fixture for that report.

**Severity.** Hard compile error (`cc` exits non-zero) for the whole module.
Any program that loads `stdlib/io.tur` and reaches codegen cannot build, even if
it only uses `file-open`/`read-file`/etc. and never calls `Real-FileSystem` or
`list-dir` -- because the backend emits every defn in the loaded file. Latent:
the only fixtures that mention these (`errors/io-wrong-handle`,
`errors/filestream-wrong-handle`) are negative fixtures that assert a
type-checker diagnostic and stop at `emit-c`/`check`; they never C-compile the
output.

## Defect 1 -- `Real-FileSystem` defines `static` functions inside a function body

`stdlib/io.tur` (the `Real-FileSystem` defn) has an inline-C body that declares
four `static` helper functions *inside* the generated C function body and stores
their addresses in a returned struct:

```c
static int fs_read_file(const char* path, ...) { ... }   // nested in Real_FileSystem()
static int fs_write_file(...) { ... }
static int fs_delete(...) { ... }
static int fs_list(...) { ... }
...
fs->read_file = fs_read_file;   // address escapes the enclosing function
return (void*)fs;
```

`cc` rejects this with `error: invalid storage class for function 'fs_read_file'`
(a function definition with `static` storage class is not allowed inside another
function). Even dropping `static` would only compile under GCC's *nested
function* extension (clang rejects it outright), and would be **undefined
behavior at runtime** here: the trampolines are stored in a heap struct that
outlives `Real_FileSystem`, so calling `fs->read_file(...)` later dereferences
out-of-scope nested functions. "Compiles on GCC and happens not to crash" would
be a works-by-luck bug, not a fix.

Root cause: inline-C bodies are pasted verbatim into the enclosing C function,
so there is no way for an inline-C block to define *file-scope* helper functions
-- but a capability-vtable of function pointers fundamentally needs file-scope
functions to point at.

## Defect 2 -- `list-dir` uses `DIR`/`struct dirent` with no `<dirent.h>`

`list-dir` (and the `fs_list` helper above) reference `DIR`, `struct dirent`,
`opendir`, `readdir`, `closedir`, `rewinddir`, `strdup` in inline-C, but
`<dirent.h>` is never included by the emitted preamble:

```
error: unknown type name 'DIR'
error: invalid use of undefined type 'struct dirent'
warning: implicit declaration of function 'rewinddir'
```

## Defect 3 -- `extern-c` opendir/readdir/closedir conflict with the real headers

The top of `stdlib/io.tur` declares:

```turmeric
(extern-c readdir [^ptr] :ptr)
(extern-c opendir [^cstr] :ptr)
(extern-c closedir [^ptr] :int)
```

These lower to `int64_t`-typed prototypes. The naive fix for Defect 2 (adding
`#include <dirent.h>` to the inline-C block) then collides with the real libc
prototypes: `error: conflicting types for 'closedir'; have 'int(DIR *)'`. A real
fix has to reconcile these declarations with the system header (e.g. drop the
`extern-c` lines once the header is included, or stop hand-declaring POSIX
functions that the header already provides).

## Minimal repro

```turmeric
(load "stdlib/io.tur")
(defn main [] : int 0)
```

```sh
./build/tur build repro.tur -o /tmp/repro    # cc invocation fails (status 256)
```

Observed: `cc` errors as above. Expected: a program that loads `stdlib/io.tur`
and uses the file helpers compiles and runs.

## Proposed fix directions

- **Defect 1:** Give the codegen a way to emit *file-scope* C from a definition
  (a top-level raw-C / "C prelude" emission hook), and rewrite `Real-FileSystem`
  so `fs_read_file` & friends are emitted at file scope rather than nested.
  Alternatively, model the capability with ordinary top-level `defn`s and build
  the vtable from their (stable) mangled C names. Either is a real change to how
  inline-C interacts with the module emitter -- out of scope for the
  untyped-params fix.
- **Defect 2/3:** Hoist `<dirent.h>` into the preamble (gated like
  `g_needs_regex_h` so it does not churn snapshots for non-`io` programs) *and*
  remove the conflicting `extern-c` opendir/readdir/closedir declarations so the
  system prototypes win.

## How to validate a fix

1. The minimal repro above must `tur build` and run.
2. Add a happy-path fixture that `(load "stdlib/io.tur")` and round-trips a file
   through `file-open` / `file-read` / `file-close` *and* exercises `list-dir` /
   `Real-FileSystem`, then run it under the leak-checked suite.
3. `bash tests/run.sh` stays green.

## Status / relationship to the file-open fix

The `io-file-open-untyped-params-default-to-int` report has been fixed in this
session: `file-open`/`read-file`/`write-file`/`file-exists?`/`file-read` now
carry their real `:cstr`/`ptr<void>`/`int` parameter and return types, and the
backend was taught to emit a struct-returning C function for an inline-C defn
whose declared return is a by-value struct (previously demoted to `int64_t`,
which made `file-open`'s `return fh;` uncompilable). The documented
`(file-open "/path" "rb")` call now type-checks, and the same pattern builds and
runs in isolation -- see the `inline-c-struct-return-cstr-params` fixture.

A fixture that loads the *whole* `stdlib/io.tur` and builds is still blocked by
the three defects above, which is why the file-open regression test is a
self-contained mirror of the FileHandle pattern rather than a direct
`(load "stdlib/io.tur")` fixture.
