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

> **Start here:** two tracks are specified below. The **whole-program track**
> (further down) is *proven* to fix RC for project-mode **executables** with no
> compiler changes and should land first. The **owner-TU track** (T3-T11, next)
> is the more involved change still required for `--shared` `.so` builds. T1
> (preamble extraction) is already committed; the struct drop/walk-glue task
> (T9) is needed by both tracks for `rc<T>` *struct fields*.


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

## Research findings (de-risking the owner-TU design)

These were gathered by auditing the codegen emitters
(`emit_core.c`, `emit_cps.c`, `emit_effects.c`, `emit_expr.c`, `emit_fns.c`,
`emit_stmt.c`) for the runtime symbols they emit into *module* C, and by
enumerating the preamble's typedefs/helpers. They sharply narrow the
declarations-only header's surface.

### Only 4 runtime globals are referenced by generated module code

Despite ~30 shared-state globals existing in the runtime, generated module code
emits a *direct* reference to only **four** of them (ref counts across the
emitters):

| Global | refs | linkage |
| --- | --- | --- |
| `tur_current_fiber` | 12 | `__thread` |
| `tur_current_reset_ctx` | 5 | `__thread` |
| `g_tur_args` | 4 | plain |
| `global_effect_handler_chain` | 3 | `__thread` |

Everything else (`gc_*`, `rc_free_queue*`, panic `jmpbuf`/`payload`, STM tx,
scheduler/timers/IO, cancel `jmpbuf`, ...) is touched **only by runtime
functions**. Under the owner-TU design those stay `static` inside the single
`tur_runtime.c`, with **zero** externalization. **Only these 4 globals need an
`extern` declaration in `tur_runtime.h`** (3 of them `extern __thread`), and
only these 4 lose their `static` in the owner TU.

### The `static inline` helpers are pure (safe in a per-TU header)

The three `static inline` helpers (`tur_frame_init`, `tur_frame_push_defer`,
`tur_cps_apply`) operate solely on their arguments -- verified no global
references -- so they can live in `tur_runtime.h` as `static inline` and be
replicated per-TU with no shared-state hazard.

### Public function surface module code calls (-> extern prototypes)

RC: `rc_cb_alloc`, `rc_cb_alloc_struct`, `rc_cb_alloc_kinded`,
`rc_strong_increment`, `rc_strong_decrement`, `rc_weak_increment`,
`rc_weak_decrement`, `rc_strong_count`, `rc_free_queue_drain`, `rc_upgrade`,
`rc_get_value`, `tur_rc_from_ref`, `tur_ref_from_rc`. Frame:
`tur_frame_fire_lifo`, `tur_frame_fire_chain` (pure -- could be header `static`
or extern). Panic: `tur_panic_with`, `tur_panic_set_frame`, `tur_panic_abort`,
`tur_catch_unwind_box`, `tur_catch_panic_of_box` (stateful). Cloneable:
`tur_cloneable_cont_resume`, `tur_cloneable_cont_clone`. STM:
`tur_stm_new_transaction`, `tur_tvar_new`, `tur_tvar_read`, `tur_tvar_write`,
`tur_stm_commit`, `tur_atomically`. Sets: `tur_set_from_items`,
`tur_set_member`. Option/Result/Cons (pure): `tur_some`, `tur_ok`, `tur_err`,
`tur_opt_value`, `tur_ok_value`, `tur_err_value`, `__tur_cons_of`. Handlers:
`tur_handler_table_new`.

**Classification rule for the header:** a module-called function whose body
references one of the ~30 owner-static globals must get an `extern` prototype
(body stays in the owner TU); a *pure* one (no shared-global ref, e.g. the frame
fire/init helpers and the option/result/cons accessors) may instead be emitted
as a `static`/`static inline` definition in the header. The simplest robust
rule is to give *every* module-called function an `extern` prototype + a single
body in the owner TU, and additionally inline only the 3 pure `static inline`
helpers in the header for the hot paths.

### Typedefs the header must carry (~45)

One-line typedefs: `EffectHandlerCase`, `EffectHandlerFrame`, `FiberBlock`,
`FiberLocalEntry`, `GcColor`, `GcMode`, `RcControlBlock`, `STM_Transaction`,
`TVar`, `TurAsyncTask`, `TurContK`, `TurEffectCaptureCtx`, `TurFuture`,
`TurFutureStatus`, `TurScheduler`, `TurSelectClause`, `TurSelectWaiter`,
`__tur_cons_cell`, `tur_cloneable_cont`, `tur_exists_t`, `tur_handler_entry_t`,
`tur_handler_t`, `tur_handler_table_t`, `tur_option_t`, `tur_panic_payload`,
`tur_poly_fn_t`, `tur_result`, `tur_result_box_t`, `tur_result_tag`,
`tur_set_t`, `tur_tagged_t`. Struct/enum/union typedefs (`} Name;`):
`TurChannel`, `TurIOWaiter`, `TurRole`, `TurRouter`, `TurSchedulerMT`,
`TurSyncCh`, `TurThreadHandle`, `TurThreadSpawnArg`, `TurThreadState`,
`TurTimerEntry`, `TurTimerWheel`, `tur_cloneable_reset_ctx`, `tur_cps_cont_t`,
`tur_existential_t`, `tur_frame`. (53 `typedef` lines total in the preamble.)

Because many of these are referenced by exported function signatures in module
headers, the cleanest approach is to emit **all** of the preamble's typedefs
into `tur_runtime.h` rather than try to subset them.

## Detailed implementation tasks

Ordered, each independently testable. Items T1-T2 are landed; start at T3.

- **T1 (done).** Extract the preamble into
  `emit_runtime_preamble(Buf *out, const Expr *program)`. Byte-identical;
  suite 1532/0. *(committed)*

- **T2 (done).** Verify single-file output unchanged and capture the shared-state
  catalog + emitter audit above. *(this doc)*

- **T3 -- `shared` parameter + CPS force-on.** Add a `bool shared` parameter to
  `emit_runtime_preamble`. When `shared` is true, force every `program`-gated
  CPS block on (6 `if (...)` sites: prepend `shared ||`) so the shared runtime
  is feature-complete regardless of which module drove generation; allow
  `program == NULL`. `emit_program` passes `shared = false` (byte-identical).
  *Test:* full suite stays 1532/0.

- **T4 -- owner/extern split for the 4 globals.** In `shared` mode emit the four
  module-referenced globals (`tur_current_fiber`, `tur_current_reset_ctx`,
  `g_tur_args`, `global_effect_handler_chain`) as
  `#ifdef TUR_RT_OWNER <def> #else extern <decl> #endif` (preserve `__thread`
  and existing initializers/comments); all other globals stay `static`
  untouched. *Test:* `shared=false` still byte-identical.

- **T5 -- public-function extern prototypes.** In `shared` mode, after the
  function bodies, emit `extern` prototypes for the public function surface
  listed above (or, equivalently, gate each body behind
  `#ifdef TUR_RT_OWNER` with a prototype in the `#else`). Keep the 3 pure
  `static inline` helpers inline in the header. *Test:* `shared=false`
  byte-identical; a hand-written 2-TU C smoke (`tur_runtime.h` + two `.c`
  files) links with exactly one definition of each public symbol (`nm`).

- **T6 -- generated `tur_runtime.{h,c}`.** Add an exported
  `emit_shared_runtime_header(Buf*)` (include-guarded wrapper around
  `emit_runtime_preamble(out, NULL, /*shared=*/true)`) and
  `emit_shared_runtime_owner(Buf*)` (emits `#define TUR_RT_OWNER` then
  `#include "tur_runtime.h"`). Declare both in `emit.h`.

- **T7 -- build-driver wiring** (`cmd_build_multi_files`, `src/main.c`, near the
  cc invocation at ~3525-3552). Write `tur_runtime.h` and `tur_runtime.c` into
  the build's working dir; add `tur_runtime.c` to the cc inputs (alongside
  `_main.c` and the module `.c` files; for `--shared` it must be in the `.so`
  too). Clean both up like `_main.c`.

- **T8 -- module headers include the runtime.** In `emit_header`
  (separate-compilation path), emit `#include "tur_runtime.h"` as the **first**
  line after the guard, *before* the `<stdint.h>` block, so the preamble's
  `#define _DEFAULT_SOURCE 1` precedes all system includes. Drop any now-duplicated
  std includes if they conflict.

- **T9 -- struct drop/walk glue in module `.c`.** In `emit_implementation`,
  before `if (e->as.def_.struct_def) continue;` (emit_module.c:5936-5941), emit
  `drop_glue_<Name>`/`walk_glue_<Name>` for any struct with `needs_drop_glue`
  (mirror the single-file Pass 0 at emit_module.c:1824-1866). `static`, local to
  the module `.c`.

- **T10 -- end-to-end + cross-TU ASan tests.** Add to
  `tests/run-build-project.sh`: (a) Repro A (`rc/of`, no struct) and Repro B
  (`rc<T>` struct field) build/link/run; (b) a 2-module project where module A
  allocates an `rc`/`rc<T>` and module B drops it, run under
  `ASAN_OPTIONS=detect_leaks=1` to prove a single shared GC registry; (c) `nm`
  assertion that `gc_all_blocks` and `rc_cb_alloc` are single-definition.

- **T11 -- regression sweep.** Full `bash tests/run.sh` at 0 FAIL; signal spice
  still builds; the `build-project-*` dedicated runner green.

## Whole-program track for executables (PROVEN -- recommended to do first)

This route needs **no compiler/runtime-emission changes at all** and is proven
to work for RC across modules today. It should land before the owner-TU work;
the owner-TU design (T3-T8) is then only required for `--shared` `.so` builds.

### Evidence

`tur build <file>` single-file mode routes through `compile_to_c` ->
`emit_program` (`src/main.c:847`), which inlines every transitively-imported
module's code into **one** TU with the full inline runtime. Verified end to end:

```
/tmp/wp/src/foo/alloc.tur:  (defmodule foo/alloc (export alloc-and-count)
                              (defn alloc-and-count [] : int
                                (let [r (rc/of 10)] (rc/strong-count r))))
/tmp/wp/src/foo/main.tur:   (defmodule foo/main
                              (import foo/alloc :refer [alloc-and-count])
                              (defn main [] : int (alloc-and-count)))

$ tur build /tmp/wp/src/foo/main.tur -I /tmp/wp/src -o /tmp/wp/bin   # rc=0
$ ASAN_OPTIONS=detect_leaks=1 /tmp/wp/bin ; echo $?                  # 1  (clean)
```

One TU, one copy of every runtime global, RC alloc/count correct across the
module boundary, ASan-clean. (`tur build <dir>` project mode fails the same
program at cc.) Note: a single source *file* may hold only one `defmodule`, so
the modules must live in separate files reached via `import` + `-I`, exactly as
above -- the build driver already resolves that include path.

### Tasks (whole-program executable route)

- **W1 -- locate the entry module.** In `cmd_build_project` /
  `cmd_build_multi_files`, find the module whose body defines `main`. Error
  clearly if zero or more than one across the project.

- **W2 -- route executables through single-file build.** When `!shared`, instead
  of emitting per-module `.h`/`.c` + `_main.c` and separately compiling, invoke
  the existing single-file build on the entry module's file with the project's
  full include path (own `src/` + every `:spices` dep's `src/`, exactly what
  project mode already computes for `-I`). This reuses the battle-tested
  `emit_program` path verbatim.

- **W3 -- keep `--shared` on the separate-compilation path** (it needs the
  owner-TU design; do not reroute it).

- **W4 -- validate.** `tests/run-build-project.sh`: Repro A/B build+run; the
  cross-module RC ASan case (module A allocs, module B drops) under
  `detect_leaks=1`; and confirm the existing `build-project-*` cases
  (cross-spice dep, prelude macros, sym cross-TU, cblock order, defstruct
  typedef) still pass through the rerouted executable path. Full
  `bash tests/run.sh` at 0 FAIL.

### Open questions for W2

- Cross-spice `:path` deps: project mode currently *compiles* each dep's modules
  into the link (see `build-project-links-cross-spice-dep`). Under whole-program
  the entry file's `import` of a dep module must pull that dep's source into the
  same TU -- confirm the single-file resolver follows `:spices` `src/` dirs the
  same way `tur run` does (it appears to; `tur run <file>` already auto-discovers
  spice includes).
- Build artifacts / incremental rebuild: separate compilation caches per-module
  ABI under `.tur-abi-cache/`; whole-program recompiles everything each time.
  Acceptable for correctness now; note as a perf follow-up.
- The defstruct-typedef and prelude-macro project cases must keep passing when
  routed whole-program (they should -- single-file mode is a superset).
