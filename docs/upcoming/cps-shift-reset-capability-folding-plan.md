---
title: "shift/reset -- fold cloneable-*/serial- into the unified surface via a capability annotation"
category: Planning
status: open -- broken out of the now-archived cps-backend-n6-resuming-shift-plan.md, whose other items all landed. This is the remaining surface-consolidation tail.
description: The resuming-shift plan landed its core (k-reset/k-shift on the DK substrate, (k v) sugar, single-shot ^linear, typed Cont, the surface unification routing abortive/resumable/single-shot/typed/cross-function by receiver convention, and -- as of the native-handle-in-reset work -- cross-function RESUME natively on DK). Two consolidation items remain and are captured here so they are not lost when the parent plan is archived: (1) folding cloneable-*/serial- keywords into shift/reset via a capability annotation, and (2) an optional lighter single-shot runtime.
---

# shift/reset capability-folding (surface consolidation tail)

## Where this came from

Broken out of `../archive/cps-backend-n6-resuming-shift-plan.md` (now archived). That plan's
core all landed:

- `k-reset` / `k-shift` on the cloneable/DK substrate; `(k v)` sugar via
  `k : cont`; single-shot via `^linear k : cont` (TUR-E0101); typed
  `Cont<BodyT,ResetT>` via `(cont BodyT ResetT)` with resume-value checking.
- **Surface unification**: ONE `shift`/`reset` pair now serves abortive +
  resumable + single-shot + typed + cross-function-abort, dispatched by the
  receiver's convention (cont-typed -> continuation-passing; plain -> abortive),
  with `reset` auto-promoting to the reified delimiter only when a resuming shift
  binds. The interim `k-shift`/`k-reset` scaffolding is retired.
- **Cross-function RESUME** -- the last open item -- now emits **natively on DK**
  (see the archived `../archive/cps-native-handle-in-reset-plan.md`, Reductions A+B, and
  `shift-crossfn-resume-works` = 11/105/23/306). The `abortive-shift-retirement`
  and `cross-function-resume-design` findings that gated it are resolved.

What is left is purely surface consolidation -- no runtime semantics gap.

## Remaining item 1 -- fold `cloneable-*` / `serial-*` into `shift`/`reset`

Today `cloneable-shift`/`cloneable-reset` and `serial-shift`/`serial-reset` are
distinct keywords. They are **capability specializations** of the same delimited
primitive:

- `cloneable` = multi-shot clone capability (snapshot the continuation).
- `serial`    = marshalable capability (a continuation of tagged, serializable
  frames).

The capture machinery is already shared (the DK substrate); only the capability
differs. The end state is a single `shift`/`reset` **surface** with a capability
knob, e.g. an annotation on the continuation param, so `shift` with a
`serial-cont` receiver yields a serial continuation and one with a
`cloneable-cont` receiver yields a cloneable one -- collapsing the `cloneable-*`
and `serial-*` keywords into thin capability spellings over the unified path
rather than separate pipelines.

Blocking nuance recorded by the parent plan: `shift` currently yields a cloneable
continuation regardless of the receiver's declared flavor, so folding `serial`
needs `shift` to **preserve the continuation's flavor** from the receiver's
`cont` capability annotation. That is the substantive part of this item.

Exit: `cloneable-*`/`serial-*` keywords removed (or reduced to sugar) with
`shift`/`reset` routing by the receiver's capability annotation; existing
cloneable/serial fixtures pass unchanged.

## Remaining item 2 -- optional lighter single-shot runtime

A single-shot continuation does not need the clone/snapshot machinery. A lighter
single-shot runtime is an **optimization, not a semantics blocker** -- the
`^linear` single-shot surface already works on the shared substrate. Pursue only
if profiling shows the snapshot path is a cost on single-shot-heavy code.

## Relationship to the runtime finish plan

This is orthogonal to `cps-runtime-finish-plan.md` (the N6.5 fallback-deletion
endgame). Capability-folding is a source-surface cleanup; it does not gate, and
is not gated by, emptying the BODY-* eviction surface. Sequence it whenever
convenient; it touches elaboration + the keyword set, not the CT-IR admission
predicates.
