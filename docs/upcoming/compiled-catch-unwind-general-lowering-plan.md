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

> **Status: DONE (single-function).** The grammar-matched scaffold
> (`stackless_catch_eligible` / `emit_stackless_catch_body` / `su_simple_expr`)
> is **deleted**; a general segment splitter (`emit_stackless_general_body` in
> `src/compiler/emit_fns.c`) replaces it. It lowers an arbitrary catch-crossing
> self-recursive body into heap-continuation segments driven by a
> `for(;;)`/`switch(__pc)` trampoline:
>
> - **Multiple catch sites** and **catch-unwind anywhere** (statement or value
>   position), not a single fixed site.
> - **Non-tail self-recursion anywhere**, including nested inside a pure builtin
>   (`(+ 1 (f ...))`) via emit_value "holes" (`ctx->sub_holes`): a suspended
>   sub-expression is hoisted into a saved temp and the enclosing expression is
>   re-emitted in the resume segment with the suspension replaced by that temp.
> - **A catch whose thunk actually panics** -- the resume segment consumes the
>   caught signal and builds an `err` box, so the **`err` branch is live** (the
>   scaffold assumed panic-free thunks, always `ok`). Result boxes may be
>   consumed by the pure `ok?`/`err?`/`some?`/`none?` predicates.
> - **Nested if/let/do**; let-bound locals live across a suspension are hoisted
>   to function scope and saved on the node.
> - **Tail self-calls** loop back to ENTRY with no node (O(1) space).
> - A pending panic unwinds by popping boundary-less nodes until a catch node (or
>   DONE -> uncaught abort), matching the D1a signal contract.
>
> The general path is tried first and subsumes the entire former grammar; it
> falls back to normal (native) emission only when a hard limit is hit
> (`> TUR_SC_MAXN=32` live scalars or `> 256` segments). All eight
> `stackless-catch-unwind-*` fixtures now lower through it and match native;
> `stackless-catch-unwind-multi-catch` and `stackless-catch-unwind-panic-caught`
> cover the new capabilities (multi-catch + real caught panic + err branch +
> non-tail recursion), each differential-checked against native and running
> 500,000 / 200,000 deep under a 128 KiB stack where native SIGSEGVs.
>
> Remaining for later phases: G4 (cross-function / mutual recursion via the
> boundary shim), G5/G6 (result-box ownership -- value extraction with `ok-val`,
> non-scalar/carrier params, RC discipline), G7 (effect/fiber/cancel unification).
> Precise per-split liveness (G1) is still the over-approximation "save every live
> scalar"; a real backward liveness pass is a later optimization.

- Replace the grammar match with a general split of one function's body at its
  `catch-unwind` sites, self-recursion allowed anywhere (tail or non-tail),
  multiple catch sites, nested `if`/`let`/`do`. This retires
  `stackless_catch_eligible`'s body-shape and "simple sub-expression"
  restrictions for self-recursive functions.
- Validation gate: every scaffold fixture (`stackless-catch-unwind-*`) plus new
  fixtures for multi-catch and nested-control bodies, all matching native at
  small depth and running 1,000,000 deep flat.

### G4 -- Cross-function + mutual recursion

> **Status: DONE.** A trampolined `f` calling a trampolined `g` DESCENDs into
> `g`'s ENTRY through one **shared group driver** instead of C-calling, so mutual
> recursion (and longer cycles) run with a flat C stack.
>
> - **Group detection** (`gs_find_group`): from a candidate function, BFS the
>   call graph over basic-eligible top-level defns, then take the greatest
>   fixpoint that keeps only members whose user-calls all target other members
>   (or the pure accessors) and whose group as a whole reaches a `catch-unwind`.
>   The catch requirement is per-GROUP, so a member with no catch of its own
>   (e.g. `b`/`c` in an `a -> b -> c -> a` cycle) still co-trampolines. Lifted
>   closures / catch-unwind thunk functions are excluded (`fd->closure`), and a
>   member must be the canonical defn of its binding.
> - **Shared driver + shims**: the group emits one `static int64_t
>   __cu_group_N(int __pc, int64_t __a0..)` whose locals are every member's
>   params (seeded per entry from the `__a` bit-slots) + hoisted let-vars +
>   suspension temps.  A call to member `m` sets `m`'s param locals and jumps to
>   its ENTRY tag; the driver returns raw int64 bits.  Each member's ordinary C
>   function becomes a **shim** -- `return <restore>(__cu_group_N(m_entry,
>   <args as bits>, 0..));` -- preserving the native ABI at the boundary while the
>   interior is trampolined.  The single-function (G3) path stays the fast case
>   for non-cross-calling functions and is byte-identical.
> - Covered by `stackless-catch-unwind-mutual` (ping/pong) and
>   `stackless-catch-unwind-mutual-cycle` (a/b/c), each matching native at small
>   depth and running 300k-1,000,000 deep under a 256 KiB stack where native
>   SIGSEGVs.  Full suite: 1965 passed, 0 failed.
>
> Not yet covered: a group whose live-scalar/segment count exceeds the caps, or
> `> GS_MAXMEM (8)` members, falls back to normal (native) emission.  Precise
> per-split liveness (G1) is still the "save every live scalar" approximation.

### G5 -- Result-box ownership (value extraction, err branch, no leak)

> **Status: value extraction + err branch DONE; no-leak deferred (matches
> native).** The catch resume now consumes a caught panic exactly as native
> `tur_catch_unwind_box` does -- it boxes the panic payload pointer into the err
> result (`tur_box_err((int64_t)(intptr_t)payload)`) instead of freeing it. This
> fixed a real SIGSEGV: the earlier consume `free`d the payload, which crashed on
> a `panic-with` int payload and lost the value.  Consequences:
> - `AFTER` may **extract** the caught value: `ok-val` on an ok box returns the
>   caught thunk value (covered by `stackless-catch-unwind-okval`, f(n)=7+n,
>   1,000,000-deep flat where native SIGSEGVs); `err-val` returns the Panic
>   handle with native-identical semantics; the pure `panic-payload-type`/
>   `-value` accessors are now allowed in the trampolined subset.
> - The **`err` branch is genuinely live** for a thunk that raises a typed panic
>   (`stackless-catch-unwind-panic-with`, f(n)=n via the err path), matching
>   native.
> - The `ok-val`-on-`Panic` inference gap ("Adjacent" below) was already closed
>   upstream (#620), so nothing blocked this.
>
> **Deferred:** the box (and its payload) still leaks -- but so does native's
> `tur_catch_unwind_box`, and native is the correctness oracle for drop
> behavior, so matching-native (leak) is preferred over a per-level free that
> would be use-after-free the moment the box escapes (is returned as the
> function's `Result`). A real RC-drop needs escape analysis; left for later.
> Non-int result boxes ride the int64 carrier and tie into G6.

### G6 -- Non-scalar (carrier / opaque / aggregate) params

> **Status: int64-carrier / opaque DONE; by-value aggregate deferred.** The
> `sc_scalar_kind` gate on params and the return type is generalized to
> `gs_slot_type`: any value whose C representation is a plain `int64_t` --
> carrier handles, `defopaque` newtypes over an int/handle, `:fn` function
> pointers, result/option boxes -- now rides the trampoline's int64 `saved[]`
> slot, with the save/restore kind forced to `TY_INT` so the intptr path (not
> the float bit-path) is used. Both params and the return type are covered, in
> the single-function and group (G4) paths.
>
> No retain/drop is inserted: this matches native, which keeps such a param live
> across the recursive call *by value* with no reference-count traffic -- the
> node merely relocates it from the C stack to the heap, not into a different
> ownership regime. Differential-checked against native (value AND flat-stack).
> Covered by `stackless-catch-unwind-opaque` (a `defopaque Counter :int` param,
> step(c,n)=c+n, 200k-deep flat where native SIGSEGVs) and an opaque param across
> mutual recursion (G4+G6). Full suite: 1968 passed, 0 failed.
>
> **Deferred:** a genuine by-value AGGREGATE param (a C `struct` / by-ptr ADT
> like `Option<int>` = `tur_adt_Option__int`, or `Result` passed by const-ref)
> is not `int64_t` and still bails to normal emission. Riding it would need a
> typed side-area in the `tur_cont` node (the G2 option-(a) typed layout) plus
> the RC decision if the aggregate co-owns heap -- a larger step left for later.

### G7 -- Effects / fibers / cancel unification

> **Status: DONE via option (b) -- separation + precedence.** Rather than merging
> onto the DK machine (option (a)), the split enforces a clean separation and a
> native-matching precedence at the panic boundary:
>
> - **Separation is structural.** A trampolined function's body may contain only
>   the handled constructs (leaves, `if`/`let`/`do`, pure builtins, self/cross
>   member calls, `catch-unwind`, `panic`). Every other control construct --
>   effect `perform`/`handle`, `with-cancel-guard`, `defer`, `async`/`await`/
>   `spawn` -- hits `gs_value_ok`'s `default: return false` and makes the
>   function ineligible, so it falls back to native. The driver's flat C frame
>   therefore never *hosts* a longjmp-based effect/cancel unwind; the only
>   cross-boundary control flow through it is a panic. (Verified: a
>   `catch-unwind` + `defer` function is not trampolined and runs correctly on
>   the native path.)
> - **Panic precedence matches native.** Two real bugs were fixed here:
>   (1) a `panic` buried in a non-suspending `do`/`if`/`let` was fast-pathed
>   through `emit_value`, which emits a bare `return` that abandons the driver
>   frame while a boundary is live (so an active `catch-unwind` never saw it);
>   `cps_emit` now forces structural emission when `gs_has_panic(e)`, routing the
>   panic to the `EX_PANIC` arm (which `break`s into the driver's unwind loop).
>   (2) a panic that escapes every boundary the trampoline owns reached the
>   driver's DONE and `abort()`ed unconditionally; it now mirrors `tur_panic`'s
>   precedence -- an **outer handler chain** (native catch / other trampoline) ->
>   propagate by returning with the signal set; else a **fiber** `panic_jmpbuf`
>   -> `longjmp` into it; else genuinely uncaught -> abort.
> - Fibers interoperate: a trampolined deep `catch-unwind` runs correctly inside
>   an `async` fiber, and an uncaught panic inside one aborts identically to
>   native.
> - Covered by `stackless-catch-unwind-outer-catch` (escape to an outer native
>   catch) and `stackless-catch-unwind-thunk-panic-compound` (a thunk panicking
>   via a `do`, caught by the trampoline's own boundary, 200k-deep flat). Full
>   suite: 1970 passed, 0 failed; all catch-unwind fixtures match native.
>
> This satisfies the graduation prerequisite that both experiments have a
> well-defined, native-matching interaction with effects/fibers/cancel. Full
> unification onto one continuation substrate (option (a), converging with the DK
> machine) remains possible later but is not required for correctness.

## Adjacent (not part of the lowering, but blocks G5)

- **`ok-val` / `err-val` on a `Panic` result fails to infer** -- **RESOLVED
  upstream** (#620 "Surface catch-unwind result as (Result A B) so ok-val/err-val
  infer"). Confirmed while building G5: `ok-val`/`err-val` on a caught result
  now infer and compile in both the native and stackless paths, so nothing
  blocked G5's value extraction.

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
