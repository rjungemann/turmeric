---
title: General stackless catch-unwind lowering (D3-G) -- Plan
category: Planning
description: The general segment-splitting lowering that retires the pattern-matched stackless-catch-unwind scaffold (slices 1-5). It splits an arbitrary catch-crossing function body into heap-continuation segments driven by a trampoline, so any nesting of catch-unwind runs with a flat C stack -- not just the fixed self-recursive grammar. Also resolves the result-box ownership the scaffold leaks, cross-function/mutual recursion, non-scalar params, and the fiber/effect/cancel integration that panic-return-signal defers.
---

# General stackless catch-unwind lowering (D3-G) -- Plan

## Why this exists

[compiled-catch-unwind-stackless-plan.md](./compiled-catch-unwind-stackless-plan.md)
landed a working stackless lowering of `catch-unwind` behind
`--enable=stackless-catch-unwind`, in five incremental slices: single param ->
N params -> non-int scalars -> float -> result-using `let` form. It reaches the
D4 target (`cu-rec` at 10,000,000; 1,000,000 under a 64 KiB stack), is gated,
byte-identical with the flag off, and differential-checked against native.

But every slice widened one axis of a **pattern-matched grammar**
(`stackless_catch_eligible` in `emit_fns.c`): the body must be exactly
`(if COND BASE (do|let ... (catch-unwind (fn [] (f RECUR))) AFTER))`, the
recursion must be a direct self-call, sub-expressions must be "simple", and the
caught result may only be predicated, not consumed. That scaffold has reached
its clean ceiling. The remaining follow-ons -- extracting the caught value,
the `err` branch, carrier/opaque params, mutual recursion, arbitrary body shapes
-- do not widen an axis; they all require **splitting an arbitrary
catch-crossing body into continuation segments** and giving the caught result a
real **lifetime**. This plan scopes that general lowering, which subsumes and
then retires the grammar-matched scaffold.

## What the scaffold already established (reuse)

- **Transport**: `panic-return-signal` (D1a) -- `panic` sets `tur_panicking` and
  returns; every panic-capable call site is A-normalized with an
  `if (tur_panicking) return` propagation check. The general lowering rides the
  same signal; a caught panic is a return-path signal the driver consumes.
- **Boundary discovery**: the D1 `tur_handler_node` chain. A segment that owns a
  `catch-unwind` pushes a node on DESCEND and pops it in its resume segment.
- **Continuation shape**: the `tur_cont` heap node + the two-action
  (`DESCEND`/`RETURN`) `for(;;)` driver `emit_stackless_catch_body` emits, and
  the validated hand-prototype
  [prototypes/d3-stackless-catch-unwind.c](./prototypes/d3-stackless-catch-unwind.c).
  The general lowering generalizes `tur_cont` from "one grammar's saved params"
  to "any segment's saved live locals + a resume tag".
- **Existing whole-program analysis**: `cps_color_program` (`src/passes/cps.c`,
  CPS1) already computes a `cps_colored` / `may_capture` set -- "consumed by the
  future selective-CPS lowering" per its own docstring. That coloring is the
  seed for G1.
- **Existing heap-continuation substrate**: the DK machine + `tur_cloneable_cont`
  (`src/compiler/emit_cps.c`, CPS9) already reifies delimited continuations as
  heap closures with a prompt stack for `shift`/`reset`/`call/cc`. G4/G7 decide
  whether to reuse it wholesale (catch-unwind as a prompt) or keep a lighter
  single-shot machine.

## The shape of the general lowering

A function `f` that (transitively) reaches a `catch-unwind` in non-tail position
is split at each `catch-unwind` (and each call to another trampolined function)
into **segments**: straight-line pieces that end in either

- a **DESCEND** -- push the caught boundary (if this site is a `catch-unwind`),
  reify the "what to do with the returned value" as a heap continuation node
  capturing the live locals + a resume tag, and hand control to the driver to
  run the callee/thunk body; or
- a **RETURN** -- deliver a value to the current continuation.

The driver is a `for(;;)` loop stepping DESCEND/RETURN actions, dispatching a
RETURN to the right resume segment by the node's tag. Because the "after the
catch" work lives in a heap node rather than a live C frame, arbitrary nesting is
bounded by heap, not the native stack -- exactly as the scaffold does for its one
grammar, now for any body.

## Phases

### G1 -- Analysis: which functions trampoline, and their live sets

- Reuse `cps_color_program`'s reachability to mark functions that can reach a
  `catch-unwind` in non-tail position (extend the seed from control operators to
  `EX_CATCH_UNWIND`). A colored function and its non-tail callers up to the
  nearest boundary form the **trampolined set**; everything else keeps the native
  ABI.
- Per colored function, compute **live variables at each split point** (the
  locals that outlive the DESCEND and are read by a resume segment). This is the
  set each continuation node must save. A standard backward liveness pass over
  the (already elaborated) body; the scaffold's "save every param" is the
  degenerate case.
- Output: a per-function segment graph (entry segment + one resume segment per
  split) and, per segment, its live-in set and successor action.

### G2 -- Continuation representation + driver generalization

- Generalize `tur_cont` to `{ int resume_tag; tur_handler_node *boundary;
  tur_cont *next; <saved-live-locals> }`. Options for the saved locals:
  (a) a per-function-typed cont struct (precise, more types emitted), or
  (b) the current `int64_t saved[]` bit-slots plus the scalar cast helpers,
  extended with a small typed side-area for non-scalars (see G6). Start with
  (a) for correctness; it also drops the `TUR_SC_MAXP` cap.
- Generalize the driver from per-function `for(;;)` to a dispatch on
  `(function id, resume_tag)`. Two realizations, decided by measurement:
  - a shared driver loop with a big `switch` on the tag, or
  - each resume segment emitted as its own `static` function and the node
    carrying a function pointer (mirrors `tur_cloneable_cont.cont_fn`) -- lets
    the general path converge with the DK machine (G7).

### G3 -- Segment-splitting emit for a single self-recursive function

- Replace the grammar match with a general split of one function's body at its
  `catch-unwind` sites, self-recursion allowed anywhere (tail or non-tail),
  multiple catch sites, nested `if`/`let`/`do`. This retires
  `stackless_catch_eligible`'s body-shape and "simple sub-expression"
  restrictions for self-recursive functions.
- Validation gate: every scaffold fixture (`stackless-catch-unwind-*`) plus new
  fixtures for multi-catch and nested-control bodies, all matching native at
  small depth and running 1,000,000 deep flat.

### G4 -- Cross-function + mutual recursion

- Extend the trampolined set across function boundaries: a trampolined `f`
  calling a trampolined `g` DESCENDs into `g` via the driver rather than
  C-calling. Mutual recursion falls out.
- A **CPS/direct shim** at the boundary: entering the trampolined region from
  native code reifies the C continuation as a heap node and enters the driver;
  returning to native code runs the driver to completion and hands back the
  value. This is the fiddly part -- the shim must preserve the native ABI for the
  boundary call while the interior is trampolined.

### G5 -- Result-box ownership (value extraction, err branch, no leak)

- Give the caught result box a lifetime tied to the continuation node that owns
  it: free (RC-drop) the box when its resume segment finishes, or arena it per
  call-tree. This removes the per-level box leak the scaffold documents.
- With ownership settled, lift the scaffold's restrictions: `AFTER` may extract
  the caught value (`ok-val`/`err-val`), and the **`err` branch** becomes live
  (a function that actually panics under its own catch). Non-int result boxes
  follow once the box repr per result type is handled (ties into G6).
- Depends on the language-level `ok-val`-on-`Panic` inference gap being closed
  first (see "Adjacent" below).

### G6 -- Non-scalar (carrier / opaque / aggregate) params

- Once G2 has a typed cont layout and G5 has ownership, params that are carrier
  handles / opaque newtypes / by-value aggregates can be saved/restored with the
  correct RC discipline (retain on save if the node co-owns, drop on node free).
  This is the ownership decision the scaffold deferred, not just casting.

### G7 -- Effects / fibers / cancel unification

- `panic-return-signal` (and thus the scaffold) punts the fiber auto-cancel,
  effect/`handle` suspension, and `with-cancel-guard` unwinds (they still
  `longjmp`). The general lowering must either (a) route those boundaries through
  the same driver (converging with the DK machine -- catch-unwind, prompts, and
  fibers all become nodes on one continuation stack), or (b) keep them separate
  with a well-defined precedence at the `tur_panic_with` dispatch. Prereq for
  graduating both experiments to always-on.

## Adjacent (not part of the lowering, but blocks G5)

- **`ok-val` / `err-val` on a `Panic` result fails to infer** (observed while
  building slice 5: no fixture extracts the caught value; the accessor reports
  `expected Result, got int`). This is a language/elaboration gap independent of
  codegen. File it under `docs/reported/` and fix it before G5's value
  extraction, or G5 has nothing to test against.

## Validation / graduation

- Per phase: default (flag-off) byte-identical; `bash tests/run.sh` green; the
  full `stackless-catch-unwind-*` fixture set plus the phase's new fixtures
  matching native at small depth and running 1,000,000 deep under a reduced
  stack `ulimit` (the categorical flat-stack proof).
- D4 sign-off (from the parent plan): `cu-rec`, `cu-catch-deep`, `atom-rec`,
  `fiber-rec` at 1,000,000 in the compiled backend with no SIGSEGV, matching the
  interpreter's `eval-tco` probes.
- Graduation (feature deleted, always-on): G1-G7 complete AND measured
  neutral-or-better on the non-catch hot path across the suite. Until then both
  `stackless-catch-unwind` and `panic-return-signal` stay prototypes behind their
  gates; `expires_at` 0.31.0.

## Staging and risk

- Land G1-G3 first (single-function general split): it retires the scaffold's
  body-shape/simple-expression limits with no ABI change beyond what the scaffold
  already made, and is fully differential-checkable. Biggest correctness win per
  unit risk.
- G4 (cross-function shim) is the ABI-heavy step; stage it behind the same flag
  and keep the single-function path as the fast case.
- G5/G6 (ownership) are where subtle RC bugs live; gate each new capability
  (value extraction, err branch, carrier params) as it lands and differential-
  check against native, since native is the oracle for both value and drop
  behavior.
- Keep the scaffold path in place until the general path passes every
  `stackless-catch-unwind-*` fixture, then delete `stackless_catch_eligible` /
  `emit_stackless_catch_body` and their grammar in one cleanup commit.

## Out of scope

- Source-level semantics of `catch-unwind` (unchanged throughout).
- A full effect-system rewrite -- G7 unifies boundaries onto the existing
  continuation substrate, it does not redesign effects.
- Retrofitting onto older releases (v-boundary ABI change; land once).
