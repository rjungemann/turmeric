---
title: Higher-kinded dispatch position in emit_dispatch_tyvar (constrained-poly spec re-resolution)
status: RESOLVED (2026-07-29) -- receiver-directed dispatch; lifted lambdas still open
area: compiler (src/compiler/emit_core.c, src/compiler/emit_module.c)
---

# A constrained-poly spec dispatched through the representative instance

## Symptom

A monomorphized spec of a constrained kind-polymorphic fn called its class
methods on whichever instance the environment yielded first, not on the spec's
own concrete type. With the autoloaded stdlib, a poly fn instantiated at `Option`
ran through `Monad [Result]`:

    (defn poly-bind [^m] [^Monad m x : (m int)] : (m int)
      (bind x (fn [v] (some (* v 2)))))

    $ ./build/tur emit-c ... | grep -o "__inst_Monad_bind_[A-Za-z0-9_]*" | sort -u
    __inst_Monad_bind_Result_tyvar        # and no ..._Option at all

Correct by prefix coincidence on the `some` path (`is_ok`/`is_some` and
`ok_val`/`value` align); an out-of-bounds load of `err_val` at offset 16 of a
2-word Option on the `none` path.

## Root cause

`emit_dispatch_tyvar` identifies the dispatch variable in three positions --
ascribed receiver, bare receiver, call result -- and each looked for a **bare
`TY_TYVAR`**. That is a kind-`*` assumption. A `defclass Monad [^m]` method
called on the abstract constructor has receiver `(m int)` and result `(m b)`:
TY_APP spines whose **head** is the dispatch variable. None of the three checks
saw it, so `emit_reresolve_disp_type` returned false and the spec kept the baked
representative.

## Fix

Two parts.

**1. `emit_core.c` -- teach `emit_dispatch_tyvar` the higher-kinded position.**
If the receiver (or the call result) is a TY_APP whose head is a tyvar, hand back
the whole spine. `emit_resolve_type` grounds it to the concrete `(Option int)`
for the active spec and instance selection matches head-wise, so the head is the
selector. Checked last, so every kind-`*` case keeps its previous answer; and
deliberately inert for the scan-time predicate, which re-checks `TY_TYVAR`.

**2. `emit_module.c` -- do not mint a by-value spec of a higher-kinded instance
method.** Part 1 alone makes the ABI machinery route HKT calls into
`emit_abi_try_nested_instance_dispatch_redirect`, which mints a by-value spec of
a **carrier-written** method: `__inst_Monad_bind_Option` takes `int64_t ma` and
derefs it, so cloning that body under a `tur_adt_Option__int` parameter feeds a
struct to `some_qu(int64_t)` -- ill-typed C. Higher-kinded classes keep the
uniform int64-carrier dispatch (Plan M6/M7, the same carve-out elab applies when
binding the class var), so the redirect is skipped for them.

### The subtlety that cost a first attempt

An earlier version guarded the redirect with
`redisp_is_hkt && !is_vl_wide_mono`, reasoning that the van Laarhoven wide-functor
Path B body must still get its by-value twin. That inverted the test at the
redirect: for a Path B body the guard evaluated false, so the redirect *was*
attempted and minted the bad twin -- breaking all 11 `van-laarhoven-lens-wide-*`
fixtures with the identical shape (`run_hyid(int64_t)` fed a
`tur_adt_Identity__int`).

The redirect must be skipped for **every** higher-kinded class, unconditionally.
Path B still gets its twin -- from the interning further down, via the existing
`is_vl_wide_mono` carve-out, which is a different code path. Instrumenting the
decision point was what settled it: `Monad [Option]` and the VL `Functor
[Identity]` are indistinguishable on every type-level flag (same param kind, same
`is_heap`, same `adt_is_byvalue_product`, same carrier-ABI answer) and differ
only in the enclosing specialization's `is_vl_wide_mono`.

## Verification

`tests/fixtures/hkt-constrained-spec-reresolves-instance` -- the spec body now
emits `__inst_Monad_bind_Option`, with no out-of-bounds read on the `none` path.
Note that stdout cannot distinguish the instance (Option and Result share a
prefix -- precisely why this survived), so the regression signal is the emitted
symbol, recorded in the fixture's header comment.

All 12 `van-laarhoven-lens-wide-*` fixtures stay green.

Suite: 2403 passed, 0 failed.

## Still open

`pure` inside a **lifted continuation** was resolved the same day by Route B
(dictionary passing) -- see
[../constrained-hkt-lifted-lambda-keeps-representative-instance.md](../constrained-hkt-lifted-lambda-keeps-representative-instance.md).
