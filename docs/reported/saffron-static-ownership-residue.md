# Saffron without its collector: the `any` boxes no static owner reaches

**Severity: low, by design.** Only a `#lang saffron` program built with the
collector OFF (`TUR_SAFFRON_GC=0` / `--no-saffron-gc`, or a `--shared` build)
leaks these; a default build reclaims them (the collector is the allocator of
a compiled single-unit Saffron program since 2026-09-28). This report is the
residue three earlier ones left behind when they were closed by that change,
and it is what `tests/run-leak-check.sh` still measures -- that gate builds
Saffron fixtures with the collector off, because the collector's heap is
invisible to LeakSanitizer.

## The shapes

All three are "a box whose every holder is reached through an opaque dynamic
call", which no AST walk can settle:

1. **Argument- and return-position `any` widens around a recursive walker**
   (`tests/fixtures/saffron-higher-order`, `known-leak`: 880 bytes in 22
   allocations). A by-value payload widened into an `any` argument is a box
   the callee reads but may pass to `(f h)`; a result widened on the way out
   carries values `(f h)` produced. Attribution and the static fixes that
   landed first are in
   [any-widen-stored-in-an-adt-field-has-no-owner](../archive/any-widen-stored-in-an-adt-field-has-no-owner.md).
2. **A capturing closure returned as `any`** -- its env
   (`tests/fixtures/tailcall-dyn-leak`, `known-leak`: 32 bytes). See
   [dynamic-returned-closure-env-is-never-freed](../archive/dynamic-returned-closure-env-is-never-freed.md).
3. **A capturing closure built in a CPS-lowered function and passed to a
   dynamic call** -- its env, one per call. See
   [cps-capturing-closure-env-leaks-through-dyn-call](../archive/cps-capturing-closure-env-leaks-through-dyn-call.md).

## Why it stays open rather than being fixed

The two static answers each report ended on -- a move discipline for `any`
arguments, or a reference count on every `any` box -- are either not
decidable across an opaque call or cost a count on every widen and copy. The
collector settles all three without either. Close this only if a no-collector
Saffron build becomes a supported target in its own right; until then the
`known-leak` markers are what keep the static drops in those fixtures honest
(a double free or use-after-free there still fails the gate).
