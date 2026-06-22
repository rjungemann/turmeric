---
title: Routing carrier <-> concrete crossings through shared recovery chokepoints -- retiring the per-site patch pile
category: Planning -- ABI / Codegen (carrier<->concrete unification, structural)
description: The crossing audit (docs/archive/carrier-concrete-abi-crossing-audit-plan.md -- archived 2026-06-22, all gaps closed)
  converted an open-ended bug stream into a closeable list, but each gap is
  still being closed the way the prior 45 were -- a narrowly-gated branch added
  next to the last one. This plan is the structural counterpart: make the two
  shared recovery routines MANDATORY chokepoints every crossing passes through,
  so a new emit site is correct by construction instead of being the next fish.
  It exists so the unification work is not lost between one-off gap closures.
status: OPEN -- proposed. Sequenced after the audit's P1 (stress-matrix
  fixtures) and interleaved with P2 (gap closures G2-G7).
---

# Routing carrier <-> concrete crossings through shared recovery chokepoints

## Status (verified 2026-06-22)

**IN PROGRESS.** Filed (commit `fbd0a94d`) as the structural follow-up to
the now-archived `carrier-concrete-abi-crossing-audit-plan.md`. All audit
gaps (G2-G10) closed via the old per-site patch approach *before* this plan
landed. Two structural slices now landed: the value-side recovery idiom is
consolidated behind a single shared routine (R0/R2 below), and the two
chokepoints are now self-validating via a Debug-only "forgot to route" ICE
(R3 below).

- **R0 -- Inventory of un-routed sites.** DONE for the value side (see the
  "R0 inventory" table below). The dominant un-routed pattern was the
  hand-rolled `for pi: if fd->params[pi] == b -> arg_types[pi]` recovery --
  the value-side chokepoint logic copied inline at 6 sites. Dispatch-side
  and fn-value-side inventory still open.
- **R1 -- Make the chokepoints total.** STARTED. The dispatch-tyvar
  *identification* walk (ascribed receiver / bare receiver / result type)
  was duplicated verbatim in the emit-time chokepoint
  (`emit_reresolve_disp_type`) and the scan-time predicate
  (`emit_call_dispatches_on_spec_tyvar`); it is now one shared routine,
  `emit_dispatch_tyvar` (`emit_core.c`, declared in `emit_internal.h`), that
  both consult, so they can no longer disagree about which position carries
  the dispatch variable. The chokepoint's constraint-var resolution *tail*
  and `emit_ground_constraint_var` remain separate **by design**: the latter
  handles a `param_idx < 0` direct-`type_arg` case and gates on result
  concreteness that the chokepoint deliberately omits, so merging them would
  change instance selection (a behavior change, not a refactor).
  `emit_var_spec_arg_type` (`emit_expr.c`) and
  `emit_reresolve_disp_type` (`emit_core.c`) have otherwise been *extended*,
  but every G2-G10 fix landed as a sibling branch beside them: G2 via
  `emit_abi_try_nested_instance_dispatch_redirect` (`emit_module.c:1960`,
  PRs #493/#495); G3 via call-site bridge in
  `expr_emits_byvalue_carrier_abi` (`emit_expr.c:382`, PR #497); G4 via
  per-element phantom clone (PR #486); G5/G7/G9 via local site fixes
  (PRs #482, #498, #494); G6 via the new fn-value axis (PRs #487-#502).
- **R2 -- Migrate ad-hoc sites onto chokepoints.** STARTED (value side).
  The value-side `params[pi] -> arg_types[pi]` recovery now lives in one
  exported routine, `emit_spec_arg_type_for_binding` (`emit_expr.c`,
  declared in `emit_internal.h`). `emit_var_spec_arg_type` delegates to it,
  and the four ad-hoc inline copies were deleted and routed through it:
  `field_read_emits_byvalue_aggregate` (`emit_expr.c`),
  `emit_reresolve_disp_type` (`emit_core.c`), the M4c Path A.2 by-value
  field-access override (`emit_expr.c`), and the instance-method twin-arg
  resolver (`emit_module.c`). Behavior-preserving: full suite 1765 passed,
  0 failed, zero fixture churn. Dispatch-side branch deletions not yet done.

### R0 inventory -- value-side recovery (`params[pi] -> arg_types[pi]`)

| Site | Status | Note |
|---|---|---|
| `emit_expr.c` `emit_var_spec_arg_type` | ROUTED (is the chokepoint; now delegates to `emit_spec_arg_type_for_binding`) | -- |
| `emit_expr.c` `field_read_emits_byvalue_aggregate` (G9) | MIGRATED | inline copy deleted |
| `emit_core.c` `emit_reresolve_disp_type` field-receiver recovery | MIGRATED | inline copy deleted |
| `emit_expr.c` M4c Path A.2 by-value field-access override | MIGRATED | inline copy deleted |
| `emit_module.c` instance-method twin-arg resolver (Gap 1) | MIGRATED | inline copy deleted |
| `emit_fns.c:910` carrier-payload param index | EXCLUDED | wants the param *index*, not its type -- different routine |
| `emit_effects.c:211` `ctx->fn_params[pi]` | EXCLUDED | different array (effect-handler params), not spec `arg_types[]` |

### R0 inventory -- dispatch-side recovery

| Site | Status | Note |
|---|---|---|
| `emit_core.c` `emit_reresolve_disp_type` dispatch-tyvar identification | ROUTED (delegates to `emit_dispatch_tyvar`) | -- |
| `emit_module.c` `emit_call_dispatches_on_spec_tyvar` tyvar identification | MIGRATED | inline copy deleted; now calls `emit_dispatch_tyvar` |
| `emit_core.c` constraint-var resolution tail | NOT MERGED (by design) | differs from `emit_ground_constraint_var` (param_idx<0 + concreteness gating); merge would change instance selection |
| `emit_module.c:2164` scan-time dispatch recovery | ALREADY ROUTED | calls `emit_reresolve_disp_type`; redirect helpers consume its result |

### R0 inventory -- fn-value (closure-thunk) axis

| Site | Status | Note |
|---|---|---|
| `emit_module.c` `emit_abi_scan_fn_values` signature derivation | ROUTED (delegates to `emit_abi_fn_value_signature`) | the named third-axis chokepoint |
| `emit_module.c` poly-closure inner-clone derivation (Stage B+C) | NOT ROUTED (by design) | env-aware (param 0 = env ptr); specialize decision already made upstream, computes no `abi_changes` |
| `emit_module.c` `emit_abi_register_call` arg/result `abi_changes` block | NOT ROUTED (by design) | layered M4c Path A.1 + G4 phantom + borrow-path cases; sharing would change behavior |
- **R3 -- Debug-build "forgot to route" ICE.** DONE (chokepoint
  postcondition form). `emit_abi_assert_routed_concrete` (`emit_core.c`,
  declared in `emit_internal.h`) is a Debug-only gate the two value/dispatch
  chokepoints now call on their success paths: a recovery routine that
  returns a non-concrete type (a leftover parametric param where a concrete
  representation is required) is a hard `tur` ICE in Debug, compiled out
  under `NDEBUG`/Release, with `TUR_ABI_NO_ROUTE_ICE=1` as an escape hatch.
  Strictness is per side (calibrated against all 1765 fixtures): the value
  side fires only on a *bare* `TY_TYVAR` (a `(Option A)`-style container whose
  element legitimately rides the carrier is fine); the dispatch side requires
  the whole resolved spine be tyvar-free (any tyvar would mis-select the
  `__inst_*`). Suite stays 1765 passed, 0 failed; the abort path is verified
  (forcing the strict check on the value side aborts `emit-c` on the
  `option_map`/`err_val` carrier specs).
- **R4 -- Audit-table-as-regression-guard.** DONE (static enforcement).
  The audit registry `docs/crossing-routing-audit.txt` lists every
  chokepoint call site as `<routine> <file.c> <count>`, and
  `tools/check_crossing_routing.py` (wired into `tests/run.sh` next to the
  turi-parity ratchets, opt-out `TUR_SKIP_PARITY_CHECK=1`) fails the suite
  when a new chokepoint call site appears without a registry row, a count
  drifts, or a registry entry goes stale. Regenerate with
  `python3 tools/check_crossing_routing.py --update` and add the matching
  row to the R0 inventory tables here. This is the documentation/CI half of
  the gate; R3's ICE is its runtime half.
- **Fn-value third axis (G6 motivated).** STARTED. The axis now has its
  named chokepoint: `emit_abi_fn_value_signature` (`emit_module.c`) derives a
  passed/captured function value's specialized arg/result types (instantiated
  through the outer spec bindings) and whether the specialization changes the
  C ABI -- the fn-value analogue of the value/dispatch chokepoints, with the
  thunk signature following the carrier `B` and never the int64 default.
  `emit_abi_scan_fn_values` routes through it. The closure-thunk inner-clone
  derivation (the poly-closure-result-specialization Stage B+C block) stays
  separate **by design**: it is env-aware (param 0 is the env pointer) and the
  specialize decision is already made upstream (`inner_float`/`inner_passed`/
  `inner_dispatch`), so it computes no `abi_changes` and would not fit the
  chokepoint's signature without adding special-casing.

**Net:** the plan is no longer unstarted -- the value, dispatch, and fn-value
axes each now have a single named recovery chokepoint, the R3 "forgot to
route" ICE gate enforces their postconditions in Debug, and the ad-hoc
value/dispatch recovery copies have been migrated onto the shared routines.
What remains is the deeper R2 branch-collapse (where it can be done without
changing behavior) and R4's audit-table-as-PR-gate.

## Why this plan exists (read first)

`docs/archive/carrier-concrete-abi-crossing-audit-plan.md` (archived 2026-06-22, every gap closed) did the hard diagnostic
work: it named the **one defect** (a parametric payload riding the int64
carrier where a concrete representation was required), enumerated the crossing
sites, and named the **two shared recovery routines** they should all consult.

What it did *not* do is make those routines a **gate**. They are invoked **ad
hoc, one emit site at a time**. So every gap (G1..G7) is still closed the way
PRs #437-#481 were -- by adding another narrowly-gated branch next to the last
one. `emit_module.c`'s `emit_abi_register_call` and `emit_core.c`'s
`emit_reresolve_disp_type` are already stacks of such branches, each commented
with the single bug it closed (`Gap #4`, `M5 residual-straddle`, `GHE2`,
`WKC3`, `Option C`, ...). A new contributor (human or agent) pattern-matches
that style and ships branch N+1. The architecture reproduces point-patches
because it *is* a pile of point-patches.

This plan is the antidote: turn the two recovery routines into **mandatory
chokepoints**, migrate the existing ad-hoc sites onto them, and delete the
redundant branches. After that, a crossing that forgets to route is a **visible
hole** (a missing audit row / a failing chokepoint assertion), not a silent
miscompile waiting for a spice to find it.

## The two chokepoints

Every carrier<->concrete crossing is on one of two sides. Each side gets one
routine that is the *only* sanctioned way to recover the concrete type:

1. **Value side -- `emit_var_spec_arg_type` (`emit_expr.c`).** Given an
   expression whose declared type is a spec parameter (receiver `x : (Vec A)`)
   but whose active specialization is concrete (`(Vec int)`), return the
   concrete monomorphized type from `current_abi_specialization->arg_types[]`.
   Every emit context that types a value crossing the boundary
   (`EX_GET_FIELD`, `EX_ASCRIBE`, `EX_CALL` arg typing, ...) must obtain the
   element type from here, never from a bare `emit_resolve_type` that can leave
   a parametric param unsubstituted.

2. **Dispatch side -- `emit_reresolve_disp_type` (`emit_core.c`).** Given a
   typeclass-method call inside an active spec whose receiver/result element was
   erased to the carrier, recover the concrete dispatch type and select the
   correct concrete/per-instantiation `__inst_*`. Every method-dispatch emit
   site (call-name rewrite, scan-time liveness, the by-value spec mint G2 needs)
   must consult this -- and it must be **recursive** (nested parametric element)
   and **ascription-aware** (a field/result ascription naming the instance type
   wins over the enclosing spec's result type).

The gaps map cleanly onto the two sides:

| Side | Routine | Gaps |
|---|---|---|
| Value | `emit_var_spec_arg_type` / call-site materialization (`expr_emits_byvalue_carrier_abi` + `emit_carrier_bridge`) | G5 (Option field read / type ordering), G4 (consumer-side cons walk), **G3 DONE** (by-value field-read receiver bridged to the carrier at the call site) |
| Dispatch | `emit_reresolve_disp_type` | **G2 DONE** (nested parametric element), G7 (sum-typed field instance selection), G9 (witness-side mirror of G2) |
| Fn-value | (new, parallel axis) | G6 (closure-thunk per carrier + cata result) |

(G3 landed on the value/materialization side, not the dispatch re-resolver: the
instance method kept its uniform carrier ABI and the by-value field-read
*receiver* was bridged to the carrier at the call site -- the same path a
by-value local already used. G10, the applied-struct instance-selection
conflation, is a keying/mangling defect outside both chokepoints.)

G6 reveals a **third axis** the audit's value-centric framing missed: function
pointer / closure-thunk ABI. It needs its own chokepoint (the
`emit_abi_scan_fn_values` / `poly-closure-result-specialization` path), held to
the same discipline: the thunk signature follows the carrier `B`, never the
int64 default.

## Phases

### R0 -- Inventory (cheap, do first)

Grep every site that recovers a concrete element type *without* going through a
chokepoint -- a bare `emit_resolve_type` on a receiver/field, a hand-rolled
`arg_types[pi]` lookup, a re-mint of an `__inst_*` name. Add each as a row to
the audit table (file:line, side, routed? Y/N). This is the worklist; it is
also the P3 regression guard seeded.

### R1 -- Make the chokepoints total

Audit `emit_var_spec_arg_type` and `emit_reresolve_disp_type` for the cases the
gaps prove they miss, and fold those *into the routine* rather than at the call
site:

- `emit_reresolve_disp_type`: recurse when the recovered receiver type is itself
  a parametric container (G2); honor a field/result ascription that names the
  instance type over the enclosing spec result (G7); handle a by-value
  struct-field receiver (G3).
- `emit_var_spec_arg_type`: cover the Option field read as an embedded aggregate
  (G5 Site 1), and the `:heap` cons whose head is a by-value aggregate (G4).

The test for "is this in the routine yet": a *new* emit site that calls the
routine gets the right answer with **zero site-local special-casing**.

### R2 -- Migrate existing ad-hoc sites onto the chokepoint

Replace each gated branch found in R0 with a single chokepoint call. Expect this
to **delete** code: the stacked special cases in `emit_abi_register_call` /
`emit_reresolve_disp_type` that re-derive the same concrete type collapse into
one call each. Do it in small, snapshot-stable steps (one site per commit,
regenerate fixtures in the same commit per CLAUDE.md).

### R3 -- Make routing mandatory (the gate)

Add a debug-build assertion / invariant: a carrier-ABI value or method-dispatch
that reaches code emission with an unresolved parametric param type, *without*
having passed a chokepoint, is a hard `tur` ICE in Debug (not a silent
miscompile). This is what flips "forgot to route" from a downstream spice crash
into a local, immediate failure. Keep it Debug-only so release builds are
unaffected.

### R4 -- Audit-as-regression-guard (adopt the audit's P3)

The audit table is the single source of truth for "which crossings are routed."
Any PR adding a recovery call site adds a row + a stress-matrix cell. A crossing
without a row fails review. This plan's R3 assertion is the runtime enforcement
of R4's documentation.

**Landed.** The single source of truth is now machine-readable and enforced:

- `docs/crossing-routing-audit.txt` -- the registry, one
  `<routine> <file.c> <call-site-count>` line per routed crossing.
- `tools/check_crossing_routing.py` -- enumerates the chokepoint call sites in
  `src/compiler/*.c` (definitions and prototypes excluded) and diffs them
  against the registry; `--update` regenerates it.
- `tests/run.sh` runs the check (with `--quiet`) alongside the turi-parity
  ratchets, so a crossing that forgot its row fails the suite, not just review.
  Opt out with `TUR_SKIP_PARITY_CHECK=1`.

The sanctioned chokepoints it tracks: `emit_spec_arg_type_for_binding` /
`emit_var_spec_arg_type` (value), `emit_reresolve_disp_type` /
`emit_dispatch_tyvar` (dispatch), `emit_abi_fn_value_signature` (fn-value), and
`emit_abi_assert_routed_concrete` (the R3 gate the call sites lean on).

## Risks / why this wasn't done already

- **Snapshot churn.** Collapsing branches changes emitted C; many
  `tests/fixtures/*/expected.c` regenerate. Mitigate with R2's one-site-per-
  commit cadence and same-commit regen (CLAUDE.md fixture policy).
- **Legacy carrier suite.** Per CLAUDE.md the default by-value suite is the
  gate; the `TUR_M7_HKT=0` carrier path may degrade as sites migrate -- that is
  expected, not a regression to chase.
- **Each migration is locally riskier than a patch.** That asymmetry (cheap
  patch vs. risky refactor, under a single failing test) is *exactly* why 45
  sessions chose the patch. R2's small steps + R3's assertion are what make the
  refactor's risk bounded and its payoff durable.

## Validation

- `bash tests/run.sh` (10-minute timeout) green throughout; intermediate
  snapshot churn reconciled within each migration commit.
- Each gap (G2-G7) closes by *adding to a chokepoint* (R1) rather than adding a
  site-local branch; its stress-matrix cell promotes to a fixture and its report
  moves to `docs/archive/`.
- End state: `emit_reresolve_disp_type` and `emit_var_spec_arg_type` are each a
  single routine every crossing calls, the per-bug gated branches are gone, and
  the audit table has no `[GAP]` rows.
