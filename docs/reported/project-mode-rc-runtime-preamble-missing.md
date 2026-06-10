# Project-mode codegen: RC/frame runtime preamble (and struct drop/walk glue) missing

**Severity:** hard error -- any project-mode (`tur build <dir>` / separate
compilation) module that uses reference counting (`rc/of`, `rc<T>` fields,
auto-drop) fails to compile in cc. Single-file `emit-c` is fine. This is a
broader sibling of `project-mode-defstruct-typedef-missing.md` (now resolved):
that report fixed the *struct typedef* emission, but RC support in project mode
is still entirely absent.

## Repro A -- plain `rc/of`, no struct

```
/tmp/rctest2/
├── build.tur                 ; (defpackage tur-rctest2 :name "tur-rctest2" :version "0.1.0")
└── src/main.tur
```

`src/main.tur`:
```turmeric
(defmodule main
  (defn main [] : int
    (let [inner (rc/of 10)]
      (rc/strong-count inner))))
```

`tur build /tmp/rctest2` fails:

```
main.c:20:13: error: unknown type name 'RcControlBlock'
   20 |     RcControlBlock *__t2 = rc_cb_alloc(0, 3, NULL);
main.c:20:36: warning: implicit declaration of function 'rc_cb_alloc'
main.c:21:17: error: request for member 'value' in something not a structure or union
main.c:24:13: error: unknown type name 'tur_frame'
main.c:25:13: warning: implicit declaration of function 'tur_frame_init'
```

The generated `main.c` references `RcControlBlock`, `rc_cb_alloc`,
`tur_frame`, `tur_frame_init`, etc. -- none of which are declared or
`#include`d in the project-mode output. The single-file `emit_program` path
emits these as part of its runtime preamble; the per-module
`emit_header`/`emit_implementation` path does not.

## Repro B -- `defstruct` with an `rc<T>` field (drop/walk glue)

```turmeric
(defmodule foo/box
  (export Wrapper)
  (defstruct Wrapper :move [val : rc<int>])
  (defn main [] : int
    (let [inner (rc/of 10)]
      (let [w (rc/of (make-struct Wrapper inner))]
        0))))
```

`tur build` fails with the same `RcControlBlock` errors **plus**:

```
foo__box.c:29: error: 'drop_glue_Wrapper' undeclared
foo__box.c:29: error: 'walk_glue_Wrapper' undeclared
```

`rc_cb_alloc_struct(..., drop_glue_Wrapper, walk_glue_Wrapper)` references the
per-struct drop/walk glue, but those `static` functions are never emitted in
project mode.

## Root cause

Two distinct gaps in `src/compiler/emit_module.c`:

1. **RC/frame runtime preamble.** `emit_program` (single-file) emits the RC
   control-block typedefs + frame typedefs + `rc_cb_alloc`/`tur_frame_init`
   prototypes as part of its file preamble. `emit_header`/`emit_implementation`
   (separate compilation) never emit these, nor `#include` a runtime header
   that declares them. Any `rc/of` in a project-mode module therefore emits
   references to undeclared types.

2. **Struct drop/walk glue.** The single-file Pass 0 (emit_module.c:1824-1866)
   emits `static void drop_glue_<Name>(...)` and `static void
   walk_glue_<Name>(...)` whenever `def->needs_drop_glue`. The project-mode
   `emit_implementation` arm (emit_module.c:5936-5941) early-outs on every
   struct-def `EX_DEF` (`if (e->as.def_.struct_def) continue;`), so the glue is
   never emitted in the `.c`. Note the comment block in `emit_header`
   (emit_module.c:5378-5379) asserts the glue functions "stay `static` in the
   implementation file" -- but nothing actually emits them there; that claim
   is currently aspirational.

The `project-mode-defstruct-typedef-missing` fix deliberately scoped itself to
the typedef (which unblocks float/int-only structs like `ADSRParams` in the
signal spice). RC structs need both gaps closed.

## Expected behavior

`tur build <dir>` of a module that uses `rc/of` or an `rc<T>` struct field
should compile and link, mirroring single-file `emit-c`.

## Proposed fix directions

1. Factor the RC/frame runtime preamble that `emit_program` emits into a shared
   helper and emit it (or `#include` a generated runtime header that declares
   it) from `emit_header`/`emit_implementation` when the module uses RC. A
   `#include "tur_runtime.h"` approach keeps the typedefs single-sourced across
   the many TUs of a project build.
2. In `emit_implementation`, before the `if (e->as.def_.struct_def) continue;`
   early-out, emit the `drop_glue_<Name>`/`walk_glue_<Name>` functions for any
   struct with `needs_drop_glue` (mirroring emit_module.c:1824-1866). They are
   `static` and called only from local code, so they belong in the `.c`, not
   the `.h`.

## Validation

1. Repro A and Repro B above compile and link with `tur build` and run.
2. A new entry in `tests/run-build-project.sh` exercising an `rc/of` module and
   an `rc<T>`-field struct module end-to-end.
3. Existing single-file RC fixtures (`rc-struct-auto-drop`,
   `rc-struct-nested-rc-fields`, `arc-basic`, ...) must not regress.
