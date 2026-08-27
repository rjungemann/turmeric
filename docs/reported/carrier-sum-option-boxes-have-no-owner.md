# Carrier-riding sum Option/Result boxes have no automatic owner

**Severity: medium (memory growth in long-lived carrier-path programs).**
Filed 2026-08-27 during SR2b.

**Narrowed 2026-08-27 (SR3 slice A):** `(none)` no longer allocates -- the
carrier None is the null pointer (`adt_ctor_is_null_none`, types.c).  The
report now covers `(some x)` / `(ok x)` / `(err e)` boxes only.

## Summary

SR2b made stdlib Option/Result real sums.  On the default path a `(some x)` /
`(ok x)` / `(none)` construction mallocs a tagged monomorph box and hands back
the pointer as the int64 carrier -- and nothing frees it unless the caller
calls `option-free` / `result-free` by hand.  Before SR2b `(Option A)` /
`(Result A B)` over word-sized elements were BY-VALUE record products: no
allocation, so nothing needed an owner, and almost no caller frees today.

## Repro

`tests/fixtures/arc-weak-upgrade` (links ASan-built libturi, so LeakSanitizer
runs on the fixture binary): before the fixture added explicit
`(option-free (:: u :int))` calls, each `arc-upgrade` leaked its 16-byte
Option box.  Any fixture that links `-lturi` and constructs Options in a loop
shows the same growth.

## Root cause

`ctor_Some__*` / `tur_box_some` / `none()` malloc the tagged layout
(emit_module.c preamble, types.c monomorph ctor emission); the elaborator's
release machinery does not track sum ctor boxes the way it tracked the
(non-allocating) record path, so the box escapes with no release point.

## Fix directions

Either teach the release pass to treat a carrier sum box like other
compiler-owned temporaries (free at the end of the binding scope unless it
escapes), or move the default path to by-value flow (SR2's
`--enable=parametric-sum-byvalue` graduation) so the box never exists.  The
second is where the track is already heading; this report exists so the
interim leak is a known cost, not a surprise.

## Workaround

Callers that care (long-lived processes, leak-checked binaries) free the box
explicitly: `(option-free (:: o :int))` / `(result-free (:: r :int))` after
the payload has been read out.
