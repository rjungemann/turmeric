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

### Progress so far (committed)

The contiguous preamble emission has been **extracted** out of `emit_program`
into a standalone `emit_runtime_preamble(Buf *out, const Expr *program)` (commit
on branch: "extract emit_program runtime preamble"). This was a **pure move** --
single-file `emit-c` output is byte-identical and the full suite stays at
**1532 passed, 0 failed**. This is the reusable foundation for emitting the same
runtime from the project-mode path. The shared-linkage conversion below is
**not** yet done.

### The hard constraint: shared global state is pervasive (~35 globals)

The runtime keeps **process-global mutable state** as file-scope `static`
globals. These must resolve to **exactly one** instance program-wide --
duplicating them per module TU gives each TU its own copy, so e.g. an `rc`
allocated in module A and dropped in module B touches a different GC registry
-> leaks / use-after-free (a **silent miscompile**, not a link error). The same
hazard applies to cross-module panic/catch (separate `jmpbuf`), effect handlers
(separate handler chain), fibers/scheduler, STM, and `*args*`.

An exhaustive scan of the extracted preamble found **~35** such globals, far
more than an initial RC-only survey suggests. Grouped by subsystem:

- **RC / GC (10):** `rc_free_queue`, `rc_free_queue_count`, `gc_all_blocks`,
  `gc_all_blocks_count`, `gc_suspect_roots`, `gc_suspect_count`,
  `gc_grey_queue`, `gc_grey_count`, `gc_mode`, `gc_enabled`.
- **Panic / unwind (5):** `tur_panic_in_progress`, `global_panic_frame`,
  `global_panic_jmpbuf`, `global_panic_jmpbuf_valid`, `global_panic_payload`.
- **Effects / fibers / scheduler (10, mostly `__thread`):**
  `global_effect_handler_chain`, `tur_current_fiber`,
  `tur_fiber_cancelled_flag`, `tur_current_thread_state`,
  `tur_current_scheduler_mt`, `tur_current_reset_ctx`, `tur_cancel_jmpbuf`,
  `tur_cancel_jmpbuf_valid`, `tur_scheduler`, `tur_io_waiters`,
  `tur_global_timers`.
- **STM (1, `__thread`):** `__stm_current_tx`.
- **Test registry (3):** `tur_test_registry_names`, `tur_test_registry_fns`,
  `tur_test_registry_count`.
- **Misc (4):** `g_tur_args` (the `*args*` list, set in `main`, read
  cross-module), `g_panic_trace` (compile-time constant -- safe to duplicate),
  `__tur_xr_state` (xorshift RNG -- per-TU duplication only costs shared entropy,
  not correctness), `__tur_fatshim_keep` (an `__attribute__((unused))`
  retention array -- safe to duplicate).

So ~30 of the ~35 are genuinely shared-state-sensitive.

The non-`static` (external-linkage) runtime **functions** that would collide if
emitted into multiple TUs number **22**, not 13: the 14 RC ones
(`rc_cb_alloc{,_kinded,_struct}`, `rc_strong/weak_{increment,decrement,count}`,
`rc_is_alive`, `rc_upgrade`, `rc_get_value`, `tur_rc_from_ref`,
`tur_ref_from_rc`) **plus** 8 STM/test ones (`tur_stm_new_transaction`,
`tur_tvar_{new,read,write}`, `tur_stm_commit`, `tur_atomically`,
`tur_test_register`, `tur_test_run_all`). Because the STM/test functions are
emitted unconditionally, *any* project-mode program -- not just RC ones -- hits
these once the full preamble is emitted into more than one TU.

### Why "replicate-static + externalize globals" is the wrong design

A tempting shape is: emit the full preamble into a header included by every TU,
demote the 22 functions to `static` (per-TU copies), and externalize the ~30
globals behind a `TUR_RT_OWNER` macro. This *works* but is **fragile**: it
requires exhaustively cataloguing every stateful global, and any future runtime
global silently breaks project-mode correctness the day it is added, with no
compiler or linker error. A prototype of this shape was built and reverted for
exactly this reason -- the catalogue is a moving target and the failure mode is
a silent miscompile.

### Recommended design: single owner TU + declarations-only header

Put the **entire** runtime -- all functions *and* all globals, exactly as
single-file emits it (everything `static`) -- into **one** generated
`tur_runtime.c`, compiled once. Module TUs include a generated
`tur_runtime.h` that carries only:

- the **typedefs** (so exported signatures referencing `RcControlBlock *`,
  `tur_frame`, etc. type-check),
- the **`static inline` pure helpers** generated module code calls
  (`tur_frame_init`, `tur_frame_push_defer`, ...),
- **`extern` prototypes** for the public function surface the modules call
  (`rc_cb_alloc{,_struct,_kinded}`, `rc_strong/weak_decrement`,
  `rc_free_queue_drain`, `tur_frame_fire_lifo/chain`, ...),
- **`extern` declarations for only the few globals generated module code
  references directly** (notably `g_tur_args`; audit the emitters in
  `emit_expr.c`/`emit_stmt.c` for any direct global reads).

The advantage: every stateful global stays `static` inside the single owner TU
with **zero per-global classification** -- they are only ever touched by the
runtime functions, which also live solely in the owner TU. The module TUs hold
no runtime state at all. This is robust to future runtime growth.

The cost is generating the declarations-only `tur_runtime.h` (splitting
typedefs + static-inline helpers + prototypes out of the monolithic preamble).
That extraction is the real remaining work and should be done as its own
carefully-tested step.

Build-driver wiring (`cmd_build_multi_files` in `src/main.c`, around the cc
invocation at ~3525-3552): generate `tur_runtime.c` + `tur_runtime.h`, add
`tur_runtime.c` to the cc inputs alongside `_main.c` and the module `.c` files,
and have every module `.h` `#include "tur_runtime.h"` as its **first** line
(before the `<stdint.h>` block) so the preamble's `#define _DEFAULT_SOURCE 1`
precedes all system includes.

Independently of the runtime, **`emit_implementation` must emit
`drop_glue_<Name>`/`walk_glue_<Name>`** for any struct with `needs_drop_glue`
before its `if (e->as.def_.struct_def) continue;` early-out
(emit_module.c:5936-5941; mirror the single-file Pass 0 at
emit_module.c:1824-1866). These are `static` and called only from local code,
so they belong in each module `.c`.

### Alternative: whole-program for executables

For the `tur build <dir>` **executable** case, concatenating the project's
module sources and running the already-validated single-file `emit_program`
once side-steps the runtime-linkage problem entirely (one TU -> one copy of
every global, no classification, no declarations header). It does change the
executable build from separate compilation to whole-program, so it must be
validated against the cross-spice-dep and `--shared` scenarios the
`run-build-project.sh` smoke tests cover (the `--shared` `.so` path still needs
the owner-TU design). This is the lowest-risk route to *correct* RC-in-project
executables and may be worth doing first.

## Validation

1. Repro A and Repro B above compile, link, and run under `tur build`.
2. **Single source of truth check:** *(done)* the preamble extraction kept
   single-file `emit-c` byte-identical -- no fixture snapshot changed and
   `bash tests/run.sh` stayed at **1532 passed, 0 failed**. Any future change to
   the shared path must preserve this.
3. **Cross-TU GC-state check (the whole point):** a project with module A
   allocating an `rc`/`rc<T>` struct and module B dropping it must run clean
   under `ASAN_OPTIONS=detect_leaks=1` -- proving both TUs share one
   `gc_all_blocks`/`rc_free_queue`. Add this to `tests/run-build-project.sh`.
4. A `nm` assertion that `gc_all_blocks` / `rc_cb_alloc` resolve to a single
   definition in the linked binary.
5. Existing single-file RC fixtures (`rc-struct-auto-drop`,
   `rc-struct-nested-rc-fields`, `arc-basic`, `weak-upgrade-option`, ...) must
   not regress.
