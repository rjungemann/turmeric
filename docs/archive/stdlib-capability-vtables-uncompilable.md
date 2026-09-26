# `stdlib/capability.tur` does not compile: its four capability structs are block-scoped inside their own `-type` defns

**RESOLVED 2026-09-26.** All four fix directions, and the drive-by.

- The four vtable structs (`__tur_cap_fs`, `__tur_cap_logger`,
  `__tur_cap_random`, `__tur_cap_time`) live in one file-scope c-block at the
  top of `stdlib/capability.tur`; the `*-type` defns, whose only purpose was
  the typedef block scope defeated, are gone (Defect 1).
- Every defn declares its types. The handles are nominal: `FileSystem`,
  `Logger` and `Random` are `defopaque ... :ptr<void>` declared here, and
  `Time` is the `time` module's own (the module is loaded), so a
  `Real-Time` / `Mock-Time` handle is the same capability `time-now` takes.
  The make-* constructors take `c-fn` callbacks spelled with the vtable's C
  signatures (`i32`, `ptr<u8>`, `ptr<i32>`) and return their handle; the
  *-free and helper defns return `nil` or `int` (Defects 2 and 3).
- `stdlib/io.tur` now loads `stdlib/capability.tur` for `FileSystem` rather
  than declaring its own, so a `Real-FileSystem` is exactly what `fs-read` /
  `fs-write` / `fs-delete` / `fs-list` call through (the two layouts were
  already identical).
- `with-capability` takes a `let`-shaped binding vector, like every other
  `with-*` form, so its docstring example is now the working spelling.

Pinned by `tests/fixtures/capability-module-roundtrip`, which loads the
module and drives every capability end to end: Turmeric defns passed as
`c-fn` callbacks, the Logger through `with-capability`, a `Mock-Time` read by
`time-now`, and a `Real-FileSystem` read by `fs-delete`.

**Severity: medium.** Nothing computes a wrong answer -- the module cannot be
loaded into a compiled program at all. `(load "stdlib/capability.tur")` plus a
bare `main` is 36 C errors and a failed `cc` invocation. It is latent because
**no in-tree program loads it**: the one fixture that looks like it does,
`tests/fixtures/capability-stdlib-roundtrip`, loads `stdlib/test/capability.tur`
-- a different file, which was fixed and which does compile.

**Status:** OPEN. Filed 2026-09-21, found while testing the `with-capability`
macro during the single-body-control-forms fix (PR for
`single-body-control-forms-require-an-explicit-do`). Verified against
`./build/tur` v0.50.0 built from `main` @ `361024bad`.

**This is a sibling that a previous fix missed.** It is the same class as
[log-capability-vtable-uncompilable](../archive/history/log-capability-vtable-uncompilable.md),
which resolved `stdlib/log.tur` and `stdlib/test/capability.tur` by hoisting
their structs and helpers into a file-scope c-block. `stdlib/capability.tur` --
the non-test module, and the one the docs and the `with-capability` docstring
point users at -- was not in that report's scope and still has the defect.

## Repro

```turmeric
(load "stdlib/capability.tur")
(defn main [] : int 0)
```

```sh
$ ./build/tur run repro.tur
error: invalid application of 'sizeof' to an incomplete type 'FileSystem' ...
... 36 errors ...
tur: cc invocation failed (status 256)
```

Full distinct error set (`emit-c` then `cc -ferror-limit=0`):

```
   8 incomplete definition of type 'Logger' (aka 'struct Logger')
   8 incomplete definition of type 'FileSystem' (aka 'struct FileSystem')
   4 incomplete definition of type 'Time' (aka 'struct Time')
   4 incomplete definition of type 'Random' (aka 'struct Random')
   4 incompatible integer to pointer conversion ... 'int64_t' to 'void *' [-Wint-conversion]
   8 void function '<name>' should not return a value [-Wreturn-mismatch]
   4 invalid application of 'sizeof' to an incomplete type
```

## Defect 1 -- the struct is defined in block scope, so no other body can see it

This is the primary defect and it is **compiler-independent**, not a strictness
setting. `FileSystem-type` (`stdlib/capability.tur:28`) defines the struct
*inside its own function body*:

```turmeric
(defn FileSystem-type []
  ```c
  typedef struct FileSystem FileSystem;
  struct FileSystem {
      int (*read_file)(const char* path, unsigned char* buf, int buf_len, int* out_len);
      ...
  };
  return NULL;
  ```)
```

A struct tag declared inside a block has **block scope** (C11 6.2.1), so it is
invisible to every other function. `make-FileSystem` (`:56`) then writes its own
`typedef struct FileSystem FileSystem;`, which declares a *fresh, incomplete*
tag at that block's scope -- and `sizeof(FileSystem)` and `fs->read_file` are
constraint violations against it:

```turmeric
(defn make-FileSystem [read_fn write_fn delete_fn list_fn]
  ```c
  typedef struct FileSystem FileSystem;
  FileSystem* fs = (FileSystem*)malloc(sizeof(FileSystem));   ; incomplete type
  fs->read_file = ...;                                        ; incomplete type
  ```)
```

Reduced to plain C, with no turmeric involved:

```c
void *a(void) { typedef struct FS FS; struct FS { int (*r)(const char*); }; return NULL; }
void *b(void) { typedef struct FS FS; return malloc(sizeof(FS)); }
```

```
error: invalid application of 'sizeof' to an incomplete type 'FS' (aka 'struct FS')
note: forward declaration of 'struct FS'
```

All four capabilities repeat the pattern: `FileSystem` (`:28`/`:56`/`:286`...),
`Logger`, `Random`, `Time`. That accounts for 28 of the 36 errors.

**This would fail on Linux too.** A reader who files this under "AppleClang 21
is stricter" will conclude the module works on CI, and it does not -- there is
no conforming C compiler that accepts it.

## Defect 2 -- `void` functions that `return` a value (strictness-gated)

The four `*-type` and four `make-*` defns declare no return type, so the emitter
gives them `void`, while their bodies `return NULL;` / `return (void*)fs;`. That
is `-Wreturn-mismatch`: **an error on AppleClang 21, a warning on older
toolchains**. 8 errors. Same class as the `-Wint-conversion` /
`-Wunterminated-string-initialization` / implicit-declaration breakage the
macOS CI leg turns up: Linux gcc only warns, so this pair is macOS-first.

## Defect 3 -- an erased handle passed straight to `free()` (strictness-gated)

`FileSystem-free` (`:74`) and its three siblings take an untyped `fs`, which is
carried as `int64_t`, and hand it to `free()`:

```turmeric
(defn FileSystem-free [fs]
  ```c
  free(fs);
  ```)
```

`-Wint-conversion`, also an error on AppleClang 21. 4 errors. Note this is also
a CLAUDE.md "no lazy `:int` stand-ins" site -- a capability handle typed as a
bare untyped parameter. The fix for Defect 1 should give these a real type
(`:ptr<void>` at minimum, a `defopaque` newtype properly) rather than a cast.

## Fix direction

The pattern is already in the tree, one directory down. `stdlib/test/capability.tur`
puts every struct, every singleton, and every helper in **one file-scope
` ```c ... ``` ` block** (see its `;; file-scope-c-block:` comment at `:14`), and
the constructor defns then only allocate a vtable and wire file-scope function
pointers. Do the same here:

1. Hoist the four struct definitions into a single file-scope c-block. That
   alone clears Defects 1 (28 errors).
2. Declare the return types on the eight `*-type` / `make-*` defns (Defect 2),
   and delete the `*-type` defns entirely if the file-scope block subsumes them
   -- their only purpose was to introduce the typedef, which is exactly what
   block scope prevented them from doing.
3. Give the `*-free` params a real type instead of the untyped/`int64_t` carrier
   (Defect 3).
4. Add a happy-path fixture that loads `stdlib/capability.tur` and exercises one
   capability end to end -- the mirror of `tests/fixtures/capability-stdlib-roundtrip`.
   Without one this regresses silently again, which is how it survived the
   `log-capability-vtable-uncompilable` pass.

## Drive-by: the `with-capability` docstring does not match the macro

Unrelated to the compile failure, same file. `with-capability`
(`stdlib/capability.tur:254`) is `[binding cap_expr & body]` -- three positional
arguments -- but its docstring example at `:249` shows a binding *vector*:

```turmeric
;;;   (with-capability [fs (make-FileSystem ...)] (fs-read fs "data.txt"))
```

Written that way it binds `binding` to the vector `[fs (make-FileSystem ...)]`
and `cap_expr` to `(fs-read fs "data.txt")`, and fails with
`vector destructuring elements must be symbols`. The working spelling is
`(with-capability fs (make-FileSystem ...) (fs-read fs "data.txt"))`. Either fix
the example or change the macro to take a `let`-shaped binding vector, which is
what every other `with-*` form in the stdlib does and what the docstring's author
evidently expected.
