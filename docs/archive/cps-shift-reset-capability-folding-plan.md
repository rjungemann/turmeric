---
title: "shift/reset -- fold cloneable-*/serial- into the unified surface via a capability annotation"
category: Planning
status: item 1 LANDED (2026-07-21) -- `shift`/`reset` now route by the receiver's `cont` capability annotation: a `serial-cont` receiver makes plain `shift`/`reset` produce a serial (marshalable) continuation (EX_SERIAL_SHIFT / EX_SERIAL_RESET), a plain `cont`/`cloneable-cont` receiver keeps the multi-shot cloneable lowering, and an ignore-k receiver keeps the abortive route. The `cloneable-*`/`serial-*` keywords remain as sugar producing the same unified nodes. Item 2 (lighter single-shot runtime) is an optional optimization the plan itself gates behind profiling; left unpursued (not a semantics gap). This was a pure source-surface cleanup and did not gate v1.
description: The resuming-shift plan landed its core (k-reset/k-shift on the DK substrate, (k v) sugar, single-shot ^linear, typed Cont, the surface unification routing abortive/resumable/single-shot/typed/cross-function by receiver convention, and -- as of the native-handle-in-reset work -- cross-function RESUME natively on DK). Two consolidation items remained: (1) folding cloneable-*/serial- keywords into shift/reset via a capability annotation -- LANDED 2026-07-21; and (2) an optional lighter single-shot runtime -- left unpursued (optimization, not a gate).
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

### Progress (2026-07-21) -- item 1 LANDED

- Item 1 (fold `cloneable-*`/`serial-*` into `shift`/`reset` via a capability
  annotation): **DONE.** `elab_shift`/`elab_reset` now preserve the
  continuation's flavor from the receiver's `cont` capability annotation:
  - `elab_cont_shift_core` (`src/compiler/elab_effects.c`) reads the receiver's
    `cont` param flavor via `receiver_cont_param_type`. A `serial-cont`
    (`CONT_SERIAL`) receiver emits `EX_SERIAL_SHIFT` and marks the enclosing
    depth's `reified_serial_at_depth[d]`; any other cont flavor keeps the
    cloneable `EX_CLONEABLE_SHIFT` path unchanged.
  - `elab_reset` promotes a plain `reset` to `EX_SERIAL_RESET` (running the
    Serializable-capture check) when the shift that bound at depth d was serial,
    to `EX_CLONEABLE_RESET` when it was a cloneable resuming shift, and stays
    `EX_RESET` otherwise -- so ONE `reset` keyword now spans abortive, cloneable,
    and serial delimiters.
  - The four `cloneable-*`/`serial-*` keywords are unchanged in behaviour and
    now read as sugar: each produces the very same unified AST node the
    capability-routed `shift`/`reset` surface produces. The downstream passes
    (`cps.c`, `effect_lower.c`, `cps_ir.c`) needed no change -- they already
    lower the `EX_SERIAL_*` / `EX_CLONEABLE_*` nodes the folded surface emits.
  - Fixture `tests/fixtures/shift-reset-capability-folding` pins the new routing
    (cloneable vs serial by receiver annotation, incl. the `if`-context serial
    shape); the full suite passes on both the compiled and interpreter paths,
    and all pre-existing cloneable/serial fixtures are unchanged.
  - Mismatch diagnostic: the folded surface makes the reset-side flavor matter,
    so a `serial-cont` plain-`shift` bound to the literal `cloneable-reset`
    keyword (a migration slip) is now rejected at elaboration with a tailored
    `TUR-E0019` pointing at the actual fix ("use a plain `reset` or
    `serial-reset`"), instead of the misleading downstream `TUR-E0706` "context
    not capturable".  Detection is a per-depth `pinned_cloneable_at_depth[d]`
    flag set by `elab_cloneable_reset` and cleared by the plain `elab_reset`, so
    nearest-delimiter binding is honoured and sibling delimiters don't leak the
    pin.  Error fixture `errors/serial-cont-shift-under-cloneable-reset` pins it.
    (The rarer reverse -- a cloneable receiver under `serial-reset` -- still
    reaches the pre-existing `TUR-E0016`; left as-is, less reachable and costlier
    to disentangle from the cross-function path.  Nested serial-reset inside
    cloneable-reset keeps its pre-existing `TUR-E0706`, identical to the
    all-keyword form, so the sugar-equivalence holds.)
- Item 2 (lighter single-shot runtime): unpursued; still an optional
  optimization, not a semantics gap. The plan gates it behind profiling.

Note: the *build-side* duplication this item's sibling plan
(`cps-backend-unification-marshal-reset-unification-plan.md`) targeted IS
resolved -- `build_cloneable`/`build_serial` now share `build_marshal_reset`.
That is the internal spine-walk merge, not the source-surface keyword folding
tracked here; the two are independent and only this surface-folding tail remains.

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

Exit (MET 2026-07-21): `cloneable-*`/`serial-*` keywords reduced to sugar over a
`shift`/`reset` surface that routes by the receiver's capability annotation;
existing cloneable/serial fixtures pass unchanged. The substantive "shift
preserves the receiver's `cont` flavor" nuance below is implemented: a
`serial-cont` receiver yields a serial continuation from plain `shift`/`reset`.

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
