# Carrier-riding sum Option/Result boxes have no automatic owner

**Severity: medium (memory growth in long-lived carrier-path programs).**
Filed 2026-08-27 during SR2b.

**Narrowed 2026-08-27 (SR3 slice A):** `(none)` no longer allocates -- the
carrier None is the null pointer (`adt_ctor_is_null_none`, types.c).  The
report now covers `(some x)` / `(ok x)` / `(err e)` boxes only.

**Narrowed again 2026-08-27 (SR2a graduation), and this is most of it.** A
CONCRETE `(Option T)` / `(Result T E)` monomorph now flows by value, so its
constructor is a struct literal and there is no box to own.  What remains is
the ERASED path only: a generic base (`some`, `ok`, an instance method's
carrier base) whose element is still a type variable mallocs the tagged layout
and hands back the pointer.  `arc-weak-upgrade`, the repro below, is by-value
now -- its explicit `option-free` calls had to be REMOVED, because by value
they free a stack slot.  The residue shrinks further with each site that
monomorphizes; end-to-end monomorphization is where it reaches zero.

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

The second of the two original directions -- move the default path to by-value
flow so the box never exists -- is DONE for concrete monomorphs (the SR2a
graduation, 2026-08-27).  For the erased residue the options are unchanged:
teach the release pass to treat a carrier sum box like other compiler-owned
temporaries (free at the end of the binding scope unless it escapes), or
monomorphize the site so it stops being erased.  The second is where the track
is already heading.

## Workaround

Callers that care (long-lived processes, leak-checked binaries) free the box
explicitly: `(option-free (:: o :int))` / `(result-free (:: r :int))` after
the payload has been read out.
