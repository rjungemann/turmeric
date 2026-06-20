## sleep-ms (and the rest of `stdlib/time.tur`) is not in the auto-load list

**Status:** RESOLVED (2026-06-17). `stdlib/time.tur` is now wrapped in a
`(defmodule time ...)` and is importable via
`(import time :refer [sleep-ms ...])` (Option 2 below). It is still **not**
auto-loaded, so no program's generated C grows and no codegen snapshot
churns.

**Severity:** Documentation / build-ergonomics defect. Every fixture
across `tur-httpd` and `tur-tourist` that used `(sleep-ms ...)` had been
failing CI since the test suites were introduced (May 2026); the per-fixture
stubs masked it but did not address the broader hole that any new spice
author writing `(sleep-ms ...)` hit the same wall.

## Summary

`stdlib/time.tur` exports `sleep-ms` / `get-time-ms` / `get-cpu-time` plus
the `Real-Time` / `HighRes-Time` / `Mock-Time` capability constructors. None
of those names were visible to user code by default, and the file carried no
`(defmodule ...)` so it could not be imported either. The compiler's stdlib
auto-load list at `src/main.c` includes `macros.tur`, `safe.tur`,
`contract.tur`, `hamt.tur`, the typeclass shims, and the typed-collection
modules -- but deliberately omits the Phase T19 concurrency files and
`time.tur` "to avoid polluting every program's generated C and invalidating
codegen snapshots."

## Resolution: `time` as an import module (Option 2)

`stdlib/time.tur` is now `(defmodule time (export ...) ...)`. The module name
is `time` so `(import time)` resolves to `stdlib/time.tur` via the import
stdlib-dir fallback in `elab_load_module` (`src/compiler/elab_module.c`).
Auto-loading is untouched -- a program that never imports `time` emits
exactly the C it did before, so all ~1668 fixture snapshots are unchanged.

```turmeric
(defmodule app
  (import time :refer [sleep-ms get-time-ms])
  (defn main [] : int
    (sleep-ms 50)
    0))
```

Why not Option 1 (auto-load): verified empirically that there is **no**
dead-code elimination on auto-loaded stdlib -- `load`-ing `time.tur` into a
hello-world emits all 12 time functions (~178 lines of C) whether used or
not. Auto-loading would inject that into every program and rebase every
snapshot, which is exactly the outcome the T19 omission exists to prevent.

## Latent bugs found and fixed in the same session

Wrapping `time.tur` so it actually compiles surfaced three real defects --
the file had **never** been through the C compiler before (not auto-loaded,
not importable, no fixture referenced it), so these had been dormant:

1. **Capability constructors emitted invalid C.** `Real-Time` /
   `HighRes-Time` / `Mock-Time` defined their `now`/`sleep` helpers as
   **nested `static` functions** inside the constructor body, then stored
   their addresses in a heap struct that outlives the call. C rejects
   `static` storage class on nested functions, and even as GCC nested
   functions the stored addresses would be dangling stack trampolines.
   *Fix:* hoisted the helpers (and the cap structs) into a single
   file-scope `` ```c `` block in the module body; the constructors now
   just `malloc` and point the slots at the file-scope functions.

2. **`Mock-Time` returned the wrong pointer.** It allocated a process-wide
   `MockTime` (now/sleep/current_time) but returned a separate two-pointer
   `Time` wrapper; `mock-time-set` / `mock-time-get` then cast that wrapper
   to `MockTime*` and read/wrote `current_time` past its end (OOB).
   *Fix:* the handle returned IS the `__tur_mock_cap`; the now/sleep
   closures share it via a file-scope singleton, so set/get and now/sleep
   agree.

3. **`get-time-ms` truncated to 32-bit.** `return (int)(now * 1000)` cast
   epoch-milliseconds (~1.78e12) to a 32-bit `int`, wrapping to a garbage
   negative value, despite the turmeric return type `:int` being 64-bit and
   the docstring promising `1747123456000`. *Fix:* `return (int64_t)now * 1000`.

Also removed five vestigial `extern-c` declarations (`time`/`clock`/
`nanosleep`/`malloc`/`free`) whose hand-written signatures were wrong and
clashed with the real libc prototypes from the `<time.h>`/`<unistd.h>`/
`<stdlib.h>` preamble includes; the inline-C bodies call libc directly.

## Typing note (No-`:int` rule)

The capability handle is now a real `(defopaque Time :ptr<void>)` newtype,
exported as `Time`, rather than the bare inferred `int` the untyped defns
produced. `sleep-ms`/`get-time-ms`/`get-cpu-time` are typed `[ms : int] :
void` / `[] : int`, where the ints are genuine millisecond counts.

## Validation

- New fixture `tests/fixtures/time-import-module/` imports `time` and
  exercises `sleep-ms` + the `Mock-Time` capability; passes.
- `bash tests/run.sh`: `1668 passed, 0 failed` -- no snapshot churn because
  `time` is import-only, not auto-loaded.

## Follow-up (separate repo)

The per-fixture `(defn sleep-ms ...)` stubs in `../turmeric-spices`
(`tur-tourist`, `tur-httpd`) can now be removed in favour of
`(import time :refer [sleep-ms])`. That is spice-repo work, tracked there.
