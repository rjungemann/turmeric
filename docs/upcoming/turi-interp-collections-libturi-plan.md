# Plan: make Vec / Set / Map usable on the libturi interpreter path

**Status:** Part 1 + Part 2 done (2026-07-08); Part 3 verified + filed as a
separate follow-up. **Area:** `src/main.c`, `src/turi/`
(tree-walking interpreter + embedding library).

**Done:** the collection natives (`native_vec_*` / `native_set_*` /
`native_map_*` / `native_tur_hamt_*` / `native_hamt_*` + the `set_*`/`vec_*`
helpers) were relocated verbatim from `src/main.c` into
`src/turi/collections_native.c` (in `tur_core`, so they land in `libturi.a` /
`libturi_wasm.a`), behind a single `turi_register_collection_natives(TuriEnv *)`
entry point that `turi_env_new` (env.c) now calls for every interpreter env --
so embedders, the WASM REPL, and the interpreter test harnesses resolve the same
overrides as the `tur` CLI. Parity test: `tests/turi/collections-embed.c`
(ctest `tur_collections_embed`). Part 3 (the `load`-path re-elaboration of
set.tur/map.tur) was confirmed genuinely broken under the interpreter's
elaborator -- a distinct carrier-bridge root cause, filed in
`docs/reported/interp-load-set-map-elaboration-gap.md`; the supported
auto-load/`import` path elaborates cleanly and is unchanged.
**Goal:** make the `Vec` / `Set` / `Map` (and backing `hamt`) collection
operations available to **every** consumer of the interpreter -- embedders using
the `turi_eval` C API, the web/WASM REPL, and the interpreter test harnesses --
not just the `tur` CLI binary.

## Background -- the actual current state

Contrary to "collections don't work in the interpreter," `Vec` (and `Map`) *do*
work under `tur --interpret`, via native overrides. What is missing is that
those natives live only in the CLI, so anything that links `libturi` instead of
the `tur` binary gets nothing.

Verified empirically against this tree (`build/tur`, Debug):

- **Works in the `tur` binary, interpret mode.** A correctly-written program
  round-trips through the interpreter and matches the compiled result:

  ```turmeric
  (defn main [] : int
    (let [v (:: (vec-new) (Vec int))]   ; ascription pins the element type A
      (vec-push! v 7)
      (vec-push! v 9)
      (vec-get v 1)))                    ; => 9
  ```

  `tur run --interpret` and `tur run` both exit `9`. `map-new` constructs and
  reads back likewise.

- **Element type comes from context; ascription is only the no-context
  fallback.** A zero-arg `vec-new : (Vec A)` has no value argument to fix `A`, so
  it takes `A` from whatever typed position it flows into. A typed `let` binding
  (`(let [v : (Vec int) (vec-new)] ...)`), a `defn` return type, a `def`
  annotation, or a typed parameter at the call site (`(fill (vec-new))` where
  `fill` takes `(Vec int)`) all pin `A` with no ascription -- verified in both
  interpret and compiled modes. `(:: (vec-new) (Vec T))` (see
  `tests/fixtures/constrained-generic-dispatch-float-element/`) is needed only
  when nothing else constrains `A`: a bare/standalone expression, or an
  unannotated binding. Bare `(vec-new)` fails elaboration identically under the
  compiler and the interpreter (`function 'vec-push!' arg 1: expected
  (type-app Vec tyvar 'A'), got nil`) -- this is ordinary bidirectional
  inference, *not* an interpreter bug.

- **Broken for `libturi` embedders.** The same program driven through the
  `turi_eval` C API (which links `libturi.a`, not `main.c`) fails at runtime:

  ```
  eval: inline-C not supported in interpreter mode
        (function uses a native C implementation; run it with
         `tur build`/`tur run` instead of `--interpret`)
  ```

  `vec-new` + `vec-len` happen to succeed (their inline-C constructor bodies are
  modelable by the interpreter's inline-C emulator), but `vec-push!`,
  `vec-get` on a pushed vec, and every `set-*` / `map-*` op that has a native
  override but no emulatable body fail.

## Root cause

The collection native overrides and their registration are defined in
`src/main.c`, and registration is invoked from the **CLI command handlers**, not
from `turi_env_new` / `turi_init`:

- Implementations: `native_vec_*` (`src/main.c:8825+`), `native_set_*`
  (`:6879+`), `native_map_*` (`:7130+`), `native_hamt_*` (`:6231+`,
  `:7343+`), plus the small tag helpers `vec_retag_cell` / `vec_tag_set`
  (main.c).
- Registration wrappers: `wk_register_stdlib_natives`,
  `wk_register_hamt_natives`, `wk_register_map_natives` (main.c), called from
  the interpret/eval command handlers (e.g. `cmd_eval_h` around
  `src/main.c:5648`, and again near `:11178`).

Because `main.c` is compiled only into the `tur` executable
(`add_executable(tur main.c ... $<TARGET_OBJECTS:tur_core>)`), these symbols are
absent from `libturi.a` and `libturi_wasm.a`:

```
$ nm build/src/libturi.a       | grep -c native_vec_new   # => 0
$ nm build/src/libturi_wasm.a  | grep -c native_vec_new   # => 0
```

So the interpreter's native-override-for-inline-C path (eval.c, the "keep native
override" branch of `EX_FN_DEF`) never finds a `vec-push!` / `set-add` /
`map-assoc` override for a `libturi` env, and the inline-C body -- which the
interpreter cannot execute -- raises the "inline-C not supported" error.

The core dependency the set/map natives need, the HAMT runtime
(`tur_hamt_new` / `tur_hamt_set` / ...), is already in `libturi`
(`src/runtime/hamt.c`, present in `libturi.a`), so the blocker is purely
*where the native overrides are defined and registered*, not a missing runtime.

Affected consumers today:

1. **Embedders** using `#include "turi/eval.h"` + `turi_eval` (the documented
   embedding API) -- no collections.
2. **Web / WASM REPL** (`libturi_wasm`, `web/wasm_glue.c`) -- no collections.
3. **Interpreter test harnesses** that link `libturi` (e.g.
   `tests/turi/env-longlived.c`, `env-teardown.c`, and any future value-pool /
   promotion fixture) -- cannot exercise collections, so a whole class of
   long-lived-collection behavior is untestable from the harness.

## Goals

1. `Vec` / `Set` / `Map` (and the `hamt` primitives they sit on) resolve to their
   native overrides for **any** interpreter env created through `libturi`, so the
   `tur` binary, embedders, the WASM REPL, and the test harnesses all behave
   identically.
2. No change to the compiled path, to the `tur` binary's observable behavior, or
   to the `(:: (vec-new) (Vec T))` ascription contract.
3. The interpreter and the compiler continue to agree element-for-element on
   collection programs (same result, same element retagging for float/cstr/bool
   carriers).

## Non-goals

- Removing the ascription idiom or adding element-type inference for zero-arg
  polymorphic constructors -- that is a shared elaborator change (it affects the
  compiler identically) and is out of scope here.
- Reworking the collection *representation* or fixing its memory management. The
  `Vec` header (`{data,len,cap}`) and the `Set`/`Map` HAMT box are
  raw-`malloc`-owned, not env-pool allocated, and never reclaimed in the
  tree-walker (see "Known limitation" below). That leak is real, but it is the
  *same* behavior the `tur` binary already ships; this availability change does
  not touch it and does not make it worse.
- New collection *operations*. This is a relocation/availability change, not an
  API expansion. If an op has an inline-C body but no native override even in the
  `tur` binary, it stays unsupported in the interpreter (call it out, do not
  paper over it).

## Known limitation -- collections are never reclaimed in the interpreter

In the tree-walking interpreter a `Vec` / `Set` / `Map` buffer is created with
raw `calloc`/`malloc` (`main.c` `native_vec_new` etc.), is **not** drawn from the
env value pool, is **not** tracked on the env, and is never reached by the
interpreter's rc-drop path -- `EX_RC_DROP` / `turi_rc_drop_value` act only on the
`__rc` `TURI_STRUCT` wrapper, never on the bare `TURI_INT` carrier a collection
handle is. `turi_env_free` reclaims arenas, the scheduler, and spice images, not
these buffers. There is no GC and no refcount for them. So **every collection a
program builds persists until the process exits**, even after it leaves scope,
unless the program explicitly calls `vec-free` / `set-free` / `map-free`.

Measured in the tree-walker (`tur interpret`; transient vecs each grown to 200
ints, then dropped from scope every iteration; peak `VmRSS`):

| N collections created | peak VmRSS |
| --- | --- |
| 5,000  | 508 MB |
| 20,000 | 1,039 MB |
| 60,000 | 2,004 MB |
| 60,000 (scalar control, no vec) | 133 MB |

Peak grows ~linearly with the number of collections created; the scalar control
stays flat. This is orthogonal to the value-pool scratch-promotion work
(`docs/upcoming/turi-value-pool-carrier-relocation-plan.md`): the buffers live
outside `value_scratch`, so promotion neither bounds them nor is endangered by
them -- but it means a long-lived interpreter host that uses collections has an
unbounded leak that scratch promotion does not address. Exposing collections
through `libturi` (this plan) inherits the limitation unchanged. A real fix
(env-tracked collection buffers freed at `turi_env_free`, or drop-glue for the
collection carriers) is separate work; it is filed in
`docs/reported/interp-collections-never-freed.md`.

## Design

### Part 1 -- move the natives into libturi

Relocate the collection native implementations + their registration into a
`tur_core` translation unit so they land in `libturi.a` / `libturi_wasm.a`:

- New file (e.g. `src/turi/collections_native.c` + a small `.h`), added to
  `TURI_EVAL_SOURCES` / `tur_core` in `src/CMakeLists.txt`. It carries:
  - `native_vec_*`, `native_set_*`, `native_map_*`, `native_hamt_*` and the
    `vec_retag_cell` / `vec_tag_set` helpers, moved verbatim from `main.c`;
  - a single public entry point, e.g.
    `void turi_register_collection_natives(TuriEnv *env);`, that performs the
    same `turi_env_register_native(env, "vec-new", ...)` sequence the
    `wk_register_*` wrappers do today.
- `main.c` keeps its CLI-only natives (json, schema, seq bridges, sym, etc.) and
  now calls `turi_register_collection_natives(env)` where it used to call the
  three `wk_register_*` collection wrappers, so the binary is unchanged.

Prefer moving whole functions verbatim (they are self-contained: `TuriValue`,
`malloc`/`free`, and the already-in-libturi `tur_hamt_*`). Audit each moved
native for a stray `main.c`-local dependency (a static helper, a global flag);
pull the few genuinely shared helpers along, and leave CLI-only helpers behind.

### Part 2 -- register for every interpreter env

Make the registration reachable without the CLI. Two viable placements; pick the
narrower one that fits the existing native-registration lifecycle:

1. **Auto-register in `turi_env_new` / `turi_init`** -- collections become
   always-available, matching how the compiler auto-loads the stdlib. Simplest
   for embedders; the natives are cheap to register (name -> fn-ptr inserts).
2. **Public opt-in** -- export `turi_register_collection_natives` and have each
   consumer call it (the `tur` binary, `wasm_glue.c`, and the test harnesses).
   Keeps `libturi`'s "pure core" boundary explicit at the cost of one call per
   embedder.

Recommendation: (1), gated the same way the other always-on interpreter natives
are, so `turi_eval` "just works" for collections the way it does for list /
option / result. Whichever is chosen, wire the WASM glue and the interpreter
test harnesses to the same path.

### Part 3 -- close the `load "stdlib/set.tur"` gap (verify, then fix if real)

Driving `(load "stdlib/set.tur")` through `turi_eval` currently reports
elaboration errors that do not occur on the compiler's auto-loaded stdlib path
(`set.tur:369` `if condition must be bool, got int`; `set.tur:385`
`set-eq-loop arg 1: expected ptr<void>, got int`). These are ahead of native
dispatch, so Part 1/2 alone may not make `Set` loadable via `load`. Determine
whether real `Set` usage in the `tur` binary reaches `set.tur` through the
normal auto-load (which elaborates cleanly) rather than `load`, and:

- if the auto-load path is the supported one, document that and point the test
  harness at it;
- if `load`-time elaboration of `set.tur` is genuinely broken under the
  interpreter's elaborator (a `ptr<void>` <-> int64 carrier equivalence or an
  int->bool coercion the compiler's `emit_carrier_bridge` supplies but the
  tree-walker's elaborator does not), file it and scope the elaborator fix
  separately -- it is a distinct root cause from the main.c/libturi split.

## API / behavior changes

- New `tur_core` source + public `turi_register_collection_natives(TuriEnv *)`
  (or auto-registration inside `turi_env_new`). No change to `TuriValue`,
  `turi_eval*`, or the value-lifetime contract.
- `libturi` / `libturi_wasm` gain the collection symbols; the `tur` binary's
  behavior is unchanged (same natives, registered through the new entry point).
- Interpreter collection semantics are unchanged -- this exposes existing
  natives to more callers; it does not alter them.

## Testing

- **Embedding parity unit test** (new `tests/turi/*.c`, linked against
  `libturi`): construct a `Vec` (`(:: (vec-new) (Vec int))` + `vec-push!` +
  `vec-get` + `vec-len`), a `Set` (`set-add` / membership / `set-count`), and a
  `Map` (`map-assoc` / `map-get` / `map-count`) purely through `turi_eval`, and
  assert the read-backs. This fails today (natives absent) and passes after
  Part 1/2.
- **Interpreter/compiler agreement**: a fixture (or a `run-turi.sh` case) that
  runs the same Vec/Set/Map program both ways and diffs the output, including a
  `:float` element to cover the carrier-retag path.
- Regression: full `tests/run.sh` (10-min timeout), `run-turi.sh`, and the
  `tur_env_teardown` leak gate stay at baseline. Watch for new leaks -- the
  collection buffers are `malloc`-owned; if the parity test creates and drops
  collections, run it with `detect_leaks=0` (matching the interpreter's
  process-lifetime-native policy) or free them explicitly.

## Risks

- **Hidden `main.c` coupling.** A moved native may lean on a `main.c`-local
  static (a CLI flag, a helper). Mitigate by compiling the new TU early and
  fixing each undefined-symbol / shared-helper as it surfaces; keep CLI-only
  helpers in `main.c`.
- **Boundary creep.** Auto-registering in `turi_env_new` widens what "the core
  library" ships. If that boundary matters, prefer the explicit
  `turi_register_collection_natives` opt-in (Part 2, option 2).
- **`load`-path elaboration (Part 3) is a separate rabbit hole.** If `set.tur`
  really does not elaborate under the interpreter's `load`, that is an
  elaborator/carrier-bridge gap, not a registration gap; scope it on its own and
  do not let it block the high-value main.c -> libturi relocation.
- **WASM build drift.** Adding the natives to `tur_core` pulls them into
  `libturi_wasm`; confirm the WASM link still succeeds and the web REPL picks
  them up.

## Recommendation

Do Part 1 + Part 2 (option 1) first -- it is a mechanical relocation with a
clear parity test and immediately unblocks embedders, the WASM REPL, and the
interpreter test harnesses (including the value-pool fixtures). Treat Part 3 as a
separate, verify-first follow-up: confirm whether `Set` is actually broken via
the supported auto-load path before spending elaborator effort on the `load`
re-elaboration edge.
