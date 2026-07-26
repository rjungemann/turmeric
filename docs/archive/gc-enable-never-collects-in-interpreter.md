# `(gc-enable!)` left the collector inert in the interpreter/libturi path

**Severity:** high (silent -- the cycle collector never ran; no diagnostic)
**Status:** RESOLVED 2026-07-25
**Found by:** `tools/gc-copy-diff.py`, while costing out DEDUP step 4

## Summary

`gc_collect()` gates on **both** `gc_enabled` and `gc_mode`:

```c
void gc_collect(void) {
    if (!gc_enabled || gc_mode == GC_DISABLED) return;
    ...
}
```

`gc_mode` is initialised to `GC_DISABLED`. The **runtime** copy's `gc_enable()`
set only the flag:

```c
void gc_enable(void) {
    gc_enabled = true;      /* gc_mode stays GC_DISABLED */
}
```

so every subsequent collection returned immediately. Nothing in the tree ever
called `gc_set_mode()`, so there was no path that repaired it.

The **emitted** copy (`src/compiler/emit_module.c`) has always defaulted the
mode:

```c
static void gc_enable(void) {
    gc_enabled = true;
    if (gc_mode == GC_DISABLED) gc_mode = GC_MANUAL;
}
```

which is why compiled programs collected and the interpreter did not.

## Impact

`(gc-enable!)` lowers to inline-C `gc_enable();`, dispatched in
`src/turi/eval.c:8222` to the runtime function. `(gc!)` lowers to `gc_force();`
-> `gc_collect()`. So in `tur repl`, `tur run <file>` (interpreted), libturi
embedders, Trowel, Try Turmeric and the Godot integration, the sequence

```turmeric
(gc-enable!)
;; ... build a cycle ...
(gc!)
```

collected **nothing**, silently. The CG0--CG4 cycle collector was reachable
only from compiled programs.

## Minimal repro

```c
#include "rc.h"
#include "gc.h"
extern uint64_t gc_collections;

int main(void) {
    gc_enable();
    printf("gc_enabled=%d gc_mode=%d\n", (int)gc_enabled, (int)gc_mode);
    gc_collect();
    printf("gc_collections=%llu\n", (unsigned long long)gc_collections);
    gc_set_mode(GC_MANUAL);
    gc_collect();
    printf("gc_collections=%llu\n", (unsigned long long)gc_collections);
}
```

Before the fix:

```
after gc_enable():  gc_enabled=1 gc_mode=0
after gc_collect(): gc_collections=0        <-- no collection happened
after set_mode+collect: gc_collections=1
```

## Root cause

`src/runtime/gc.c` -- `gc_enable()` / `gc_disable()`, which were never
reconciled against the hand-written copy in
`src/compiler/emit_module.c`. This is the **fourth** bug produced by that
duplication (after CG1's double suspect-removal, CG3's `:heap` mis-cast and
CG4's weak force-free), and the first that was invisible from both sides:
compiled fixtures run the emitted copy, and nothing exercised the runtime
copy's mode contract.

## Fix

`src/runtime/gc.c`: `gc_enable()` now defaults `gc_mode` to `GC_MANUAL` when it
is `GC_DISABLED` (preserving an explicit `gc_set_mode` choice), and
`gc_disable()` clears the mode -- matching the emitted copy exactly.

Regression: `tests/turi/gc-runtime-copy-parity.c`, registered as ctest
`tur_gc_runtime_copy_parity`. It asserts the mode contract on the runtime copy
directly, against the same entry points the interpreter dispatches to.
Validated by reverting the fix and confirming
`gc_enable-defaults-to-manual-mode` and
`gc_collect-runs-after-bare-gc_enable` both fail.

## Follow-up

The underlying duplication is tracked in
[docs/archive/gc-cycle-collection-plan.md](gc-cycle-collection-plan.md)
(the DEDUP series). `tools/gc-copy-diff.py` now reports the remaining
divergence -- 23 functions at the time of this fix, 8 of them behavioral.
