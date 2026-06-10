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

## Implementation spec (turn-key)

The single-file inline runtime is emitted **only** by `emit_program`
(`src/compiler/emit_module.c`), as one contiguous assembly block to `out`
spanning roughly **lines 2303-5100** (interleaved with feature-flag sub-blocks:
effects/handlers, sets, frame/defer, panic, CPS, test registry, RC/GC,
`-Xsessions` channels). `emit_header`/`emit_implementation` (the
separate-compilation path used by `tur build <dir>`) emit none of it.

### The hard constraint: shared global state

The runtime keeps **process-global mutable state** as file-scope `static`
globals. These must live in **exactly one** TU -- duplicating them per module
TU gives each TU its own GC registry / free queue, so an `rc` allocated in
module A and dropped in module B touches different state -> leaks and
use-after-free (a **silent miscompile**, not a link error). The 16 globals
(name @ emitting line in `emit_program`):

| Global | Line |
| --- | --- |
| `tur_panic_in_progress` | 2953 |
| `global_panic_frame` | 2954 |
| `tur_test_registry_names` | 2882 |
| `tur_test_registry_fns` | 2883 |
| `tur_test_registry_count` | 2884 |
| `__tur_fatshim_keep` | 2646 |
| `rc_free_queue` | 4511 |
| `rc_free_queue_count` | 4512 |
| `gc_all_blocks` | 4527 |
| `gc_all_blocks_count` | 4528 |
| `gc_suspect_roots` | 4535 |
| `gc_suspect_count` | 4536 |
| `gc_grey_queue` | 4537 |
| `gc_grey_count` | 4538 |
| `gc_mode` | 4539 |
| `gc_enabled` | 4540 |

The 13 **non-`static` (extern) functions** would also collide if emitted in
multiple TUs: `rc_cb_alloc_kinded` (4655), `rc_cb_alloc` (4673),
`rc_cb_alloc_struct` (4680), `rc_strong_decrement` (4686),
`rc_strong_increment` (4692), `rc_weak_increment` (4700),
`rc_weak_decrement` (4704), `rc_strong_count` (4728), `rc_weak_count` (4732),
`rc_is_alive` (4736), `rc_upgrade` (4740), `rc_get_value` (4748),
`tur_rc_from_ref` (4756).

**Everything else is already `static` / `static inline`** (all typedefs, the
`gc_*` helpers, `tur_frame_*`, `rc_free_queue_push/drain`, set ops, panic
helpers). `static` storage means each TU safely gets its own copy *of the
code*, and those copies all reference the single `extern` globals -- so
replicating them per-TU is correct.

The public surface generated user code calls directly (must be visible in every
module TU): `rc_cb_alloc`, `rc_cb_alloc_struct`, `rc_cb_alloc_kinded`,
`rc_strong_decrement`, `rc_weak_decrement`, `rc_free_queue_drain`,
`tur_frame_init`, `tur_frame_push_defer`, `tur_frame_fire_lifo`,
`tur_frame_fire_chain`, plus per-struct `drop_glue_<Name>`/`walk_glue_<Name>`.

### Recommended design: shared header + single owner TU

1. **Extract** the contiguous preamble emission out of `emit_program` into
   `void emit_runtime_preamble(Buf *out, bool shared)` (pure move first; in the
   default `shared == false` path the single-file output must stay
   **byte-identical** -- verify with the fixture snapshots and the full suite
   before layering any behavior change). `emit_program` then calls it with
   `shared = false`.
2. In the `shared == true` path, wrap **only** the 16 global definitions and
   the 13 extern-function bodies in `#ifdef TUR_RT_OWNER ... #else <extern decl
   / prototype> #endif`. All other (`static`) code emits unchanged.
3. **Build driver** (`cmd_build_multi` / `cmd_emit_c_to_dir` in `src/main.c`):
   - Generate `tur_runtime.h` = `emit_runtime_preamble(buf, /*shared=*/true)`
     wrapped in an include guard.
   - Generate `tur_runtime.c` = `#define TUR_RT_OWNER\n#include "tur_runtime.h"`
     -- the **single** TU that defines the globals + 13 extern functions.
   - Add `tur_runtime.c` to the cc input set; have every module `.h`
     `#include "tur_runtime.h"` (so typedefs are visible in exported
     signatures), and `_main.c` already includes the module headers.
4. In `emit_implementation`, before the `if (e->as.def_.struct_def) continue;`
   early-out (emit_module.c:5936-5941), emit `drop_glue_<Name>` /
   `walk_glue_<Name>` for any struct with `needs_drop_glue` (mirror
   emit_module.c:1824-1866). They are `static`, called only from local code, so
   they belong in the module `.c`, not the shared header.

### Alternative considered (rejected)

Emitting the full inline runtime into every module TU with
`__attribute__((weak))` on the 29 collide-prone symbols lets the linker fold
duplicates, but relies on platform-specific weak-folding semantics (ELF vs
Mach-O differ) -- "works because the linker folds it" is precisely the
luck-based correctness this repo forbids. Use the owner-macro design.

A whole-program route for executables (concatenate the project's module sources
and run the already-validated `emit_program` once) avoids touching the runtime
emission entirely, but changes `tur build <dir>` from separate compilation to
whole-program, which risks the cross-spice-dep and `--shared` scenarios the
`run-build-project.sh` smoke tests cover. Viable but a larger behavioral
change; the shared-header design preserves the existing build model.

## Validation

1. Repro A and Repro B above compile, link, and run under `tur build`.
2. **Single source of truth check:** the `shared == false` extraction must keep
   single-file `emit-c` output byte-identical -- regenerate fixture snapshots
   (none should change) and confirm `bash tests/run.sh` stays at 0 FAIL before
   adding the shared path.
3. **Cross-TU GC-state check (the whole point):** a project with module A
   allocating an `rc`/`rc<T>` struct and module B dropping it must run clean
   under `ASAN_OPTIONS=detect_leaks=1` -- proving both TUs share one
   `gc_all_blocks`/`rc_free_queue`. Add this to `tests/run-build-project.sh`.
4. A `nm` assertion that `gc_all_blocks` / `rc_cb_alloc` resolve to a single
   definition in the linked binary.
5. Existing single-file RC fixtures (`rc-struct-auto-drop`,
   `rc-struct-nested-rc-fields`, `arc-basic`, `weak-upgrade-option`, ...) must
   not regress.
