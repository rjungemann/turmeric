---
title: "Removing the direct lowering -- retiring emit_cps.c after cps-backend graduation"
status: proposed
parent: cps-backend-unification-plan.md
description: The cps-backend experiment graduated (2026-07-11) -- the CT-IR CPS backend is now the default lowering for every emittable colored function, with the direct emitter (emit_cps.c) kept only as the eviction fallback. This plan sequences the SECOND, larger milestone: deleting the direct-style delimited-control LOWERING entirely. It is not one delete. Two distinct caller populations still reach the four lowering functions -- colored functions that evict residual shapes via CT_LETRAW, and uncolored/main/exported functions that dispatch straight to them because they are never CPS-emitted at all. This note names both, sequences closing them, and lists the exact deletes that follow.
---

# Removing the direct lowering (emit_cps.c)

## Where we are: graduation landed, deletion did not

The `cps-backend` experiment **graduated** on 2026-07-11
([graduation-readiness note](cps-backend-unification-graduation-readiness.md)):

- The row is gone from `EXPERIMENTS[]` (`src/runtime/experiments.c`); the
  `g_opt_cps_backend` gate is retired (removed from `globals.{c,h}`);
  `emit_cps_ir_try_fn` / `emit_cps_ir_program_has_emittable`
  (`src/compiler/emit_cps_ir.c`) run **unconditionally**.
- Every emittable colored function now lowers through the CT-IR CPS backend by
  default; the direct emitter stays as the **eviction fallback** only.
- `--enable=cps-backend` is an accept-and-warn no-op (TUR-W0063; the
  `GRADUATED[]` table in `experiments.c`), so downstream `build.tur` /
  `experiments.tur` that opted in keep compiling.

Graduation was **milestone 1** ("become the default"). It was verified faithful:
the flag-off-by-default output is byte-identical to the old
`--enable=cps-backend` output across the corpus (139 `expected.c` snapshots
regenerated; zero behavior failures -- this matches the readiness probe's
"278 codegen-mismatch, 0 behavior" result).

This plan is **milestone 2** ("retire the direct lowering"). Graduation did
**not** by itself delete anything: making the CPS backend the default for
*colored* functions did not remove either population of callers into the
direct-style delimited-control lowering. Those callers are the subject of this
note.

## The delete target: what "direct lowering" is

`emit_cps.c` (~2.1k lines) already had its **runtime preludes relocated** out to
`emit_dk_runtime.{c,h}` (U7 step 1, landed -- see the
[U7 readiness note](cps-backend-unification-u7-readiness-plan.md)). What remains
in `emit_cps.c` is exactly the direct-style **lowering** this plan deletes, plus
its private analysis helpers and the syntactic uses-gates:

| Symbol | `emit_cps.c` anchor | Role |
|---|---|---|
| `emit_cps_reset` | :446 (`emit_cps_reset_escape` :406) | base reset/shift lowering (DK + setjmp escape) |
| `emit_cps_callcc` | :488 | call/cc + escape lowering (local setjmp landing) |
| `emit_cps_cloneable_reset` | :1309 | multi-shot cloneable lowering (`dk_copy_range` + clone/drop glue) |
| `emit_cps_serial_reset` | :1630 | serial marshaling lowering (`__sk_frame_for_tag`, `SkReg`) |
| `frame_c_expr`, `collect_ctx`, `ctx_if_branch`, `cl_can_lower`, `sk_tag_for_frame` | :728, :752, :988, :1116, :1480 | private analysis helpers (die with the lowering fns) |
| `emit_cps_program_uses_delimited` / `_callcc` / `_cloneable_dk` | :85, :201, :1414 | syntactic form-presence prelude gates |

Reached through:

- **Dispatch wrappers** in `emit_effects.c`: `emit_effects_reset` (:1209),
  `emit_effects_cloneable_reset` (:1223), `emit_effects_serial_reset` (:1696) --
  each calls its `emit_cps_*` fn, falling back to the legacy direct lowering when
  it returns NULL.
- **`EX_CALLCC` direct dispatch** in `emit_expr.c` (:2826):
  `case EX_CALLCC: return emit_cps_callcc(...)`.
- **`CT_LETRAW` delegation** from colored functions in `src/passes/cps_ir.c`
  (`safe_to_delegate` :888-948; the `cps_bind`/`cps_tail` cases :1259-1341,
  :1460-1499): a colored function evicts a residual delimited *sub-shape* to the
  direct emitter, which routes through `emit_value` -> the `emit_effects_*`
  wrappers -> the `emit_cps_*` lowering.
- **Prelude gating** in `emit_module.c` (:6647-6651, :6727-6766) still reads the
  syntactic `emit_cps_program_uses_*` gates.

Deleting the lowering means driving **both** the dispatch-wrapper population and
the CT_LETRAW population to zero, then removing the functions, the wrappers, the
`EX_CALLCC` dispatch, the delegation arms, the gates, and finally the files.

## Why graduation did not delete it: two caller populations

### Population 1 -- colored-function eviction residue (CT_LETRAW)

A colored function is CPS-emitted, but a delimited *sub-shape* it contains may
still fall outside the native CT-IR subset and delegate to the direct lowering
via `CT_LETRAW`. The U7 readiness table established that every **named** native
gap is closed; the residual delegations are narrower, un-named tails:

- **Colored (fat-closure) receivers** for cloneable / serial / callcc. The
  native path handles bare-fn and capture-free-closure receivers; a *colored*
  receiver still delegates (the direct emitter miscompiles it too, so this is
  bug-compatible delegation, not a correctness gap).
- **Serial 2-arg call frames with `cstr` / `Serializable` envs.** The int-env
  2-arg frame marshals inline natively; the non-int env still delegates (real new
  marshaler machinery).
- **Shapes outside the supported context grammar** -- contexts `collect_ctx` /
  the `build_marshal_reset` walk do not yet reify.
- **The generic `needs_heap_join` boundary** -- a non-tail cps->cps *call* on the
  heap chain. This is **not** delimited-control-specific: it is the whole C1
  emittable-subset boundary, shared with the five emittable-subset gap plans
  ([non-scalar-values](../v1/cps-backend-non-scalar-values-plan.md),
  [owning-pointers](../v1/cps-backend-owning-pointers-plan.md),
  [tier-c-crossing](../v1/cps-backend-tier-c-crossing-plan.md),
  [effectful-callbacks](../v1/cps-backend-effectful-callbacks-plan.md),
  [generic-monomorph-classification](../v1/cps-backend-generic-monomorph-classification-plan.md)).

### Population 2 -- uncolored / `main` / exported direct dispatch

This is the population graduation left entirely untouched, and the true blocker.
A function that is **not** effect-colored -- including `main` and any exported
function -- is **never CPS-emitted**: the classifier does not route it to
`emit_cps_ir_try_fn`, and `main` / exported entry points carry a fixed C ABI
(`int main(void)`, `f(args)`), not the `f__cps(args, DK *k)` + direct-entry
wrapper shape the CPS backend produces. So when such a function uses
`reset`/`shift`/`shift0`, `call/cc`/`escape`, or `cloneable`/`serial` reset, it
dispatches **straight** to the direct lowering through the `emit_effects_*`
wrappers and the `EX_CALLCC` case -- with no CT-IR involvement to evict *from*.

Graduation changed nothing here: it flipped the default *for colored functions*.
An uncolored `(defn main [] (reset (shift k (k 1))))` takes the direct lowering
before and after graduation, identically. **This population is why the four
lowering functions still have live callers even at 100% colored-function native
coverage.**

## The sequenced work

The two populations close independently; neither blocks the other. Each closes
by *extending native coverage*, verified per-shape (`direct == cps`), then the
now-dead caller is removed.

### Phase D1 -- close the colored-function eviction residue (Population 1)

Extend native CT-IR emit until no colored function delegates a delimited shape:

- **D1a -- colored receivers.** Teach `emit_cloneable` / the callcc landing to
  emit a colored (fat-closure) receiver natively (the receiver runs once at
  capture; only the continuation is cloned/marshaled -- the multi-shot / serial
  semantics already established for closure receivers extend to colored ones).
  This also fixes the direct emitter's miscompile, so it is a net correctness
  gain, not just a port.
- **D1b -- serial `cstr`/`Serializable` 2-arg envs.** Extend the inline env
  marshaler (`SK_ENV_INT` today) to the non-int env codec so a 2-arg call frame
  with a `cstr` / `Serializable` env round-trips through
  `save-cont!`/`resume-cont!` natively.
- **D1c -- context-grammar generalization.** Widen `collect_ctx` /
  `build_marshal_reset` to reify the remaining context shapes (the
  [marshal-reset unification](cps-backend-unification-marshal-reset-unification-plan.md)
  is the natural home -- one `build_marshal_reset(..., serial)` covering both
  families).
- **D1d -- the `needs_heap_join` boundary.** This is the shared C1-subset item;
  it advances through the five emittable-subset gap plans, not as
  delimited-specific work. D1 is *complete for delimited control* when D1a-D1c
  land; D1d raises the native fraction generally and is tracked there.

**Exit:** with D1a-D1c landed, remove the `CT_LETRAW` delegation arms for
`EX_RESET`/`EX_SHIFT`/`EX_SHIFT0`, `EX_CLONEABLE_RESET`, `EX_SERIAL_RESET`, and
`EX_CALLCC` from `safe_to_delegate` and the `cps_bind`/`cps_tail` cases in
`cps_ir.c`. (`EX_ASYNC`/`EX_AWAIT` stay delegated -- they ride a *separate*
stackful fiber runtime in `emit_module.c`, untouched by the `emit_cps.c` delete;
see the [U5 async-substrate note](cps-backend-unification-u5-async-substrate-plan.md).
Their delegation targets the fiber runtime, not the four lowering functions, so
it is out of scope for this plan.)

### Phase D2 -- CPS-emit uncolored/`main`/exported delimited functions (Population 2)

Make the `f__cps(args, DK *k)` + direct-entry-wrapper shape available to a
function that uses delimited control **regardless of effect-coloring**:

- **D2a -- syntactic delimited-control CPS trigger.** Today the CPS-emit trigger
  is effect-coloring (`fd->cps_colored` / `se->in_s`). Add a second admission
  path: a function whose body syntactically contains `reset`/`shift`/`shift0`,
  `call/cc`/`escape`, or `cloneable`/`serial` reset is CPS-emitted even if it is
  not effect-colored. (The syntactic gates `emit_cps_program_uses_*` already
  compute exactly this predicate per-program; reuse the per-function form of it.)
  An uncolored function with *no* delimited control stays on the direct emitter
  as before -- CPS-emitting a genuinely pure function is pure overhead and must
  not be triggered.
- **D2b -- fixed-ABI entry wrappers.** The CPS backend already emits a
  direct-entry wrapper that preserves a colored function's callable symbol; make
  that wrapper cover the fixed entry-point ABIs -- `int main(void)` calling
  `main__cps(dk_done())`, and each exported `f(args)` calling `f__cps(args,
  dk_done())`. This is the mechanism the graduation-readiness note flagged as
  missing ("cannot take the `f__cps(args, DK*)` + wrapper shape").
- **D2c -- verify against the direct output.** Each uncolored/`main`/exported
  delimited shape gets a `direct == cps` oracle (the direct output is the current
  baseline; the CPS output must match it) before its dispatch is removed.

**Exit:** with D2a-D2b landed and the oracles green, remove the `emit_cps_*`
calls from the `emit_effects_*` wrappers (`emit_effects.c` :1209/:1223/:1696) and
the `EX_CALLCC` dispatch (`emit_expr.c` :2826). The wrappers either collapse to
their now-sole remaining behavior or are deleted with their callers.

### Phase D3 -- delete the lowering functions

With both populations at zero callers, delete from `emit_cps.c`:
`emit_cps_reset` (+ `emit_cps_reset_escape`), `emit_cps_callcc`,
`emit_cps_cloneable_reset`, `emit_cps_serial_reset`, and their private analysis
helpers (`frame_c_expr`, `collect_ctx`, `ctx_if_branch`, `cl_can_lower`,
`sk_tag_for_frame`). Delete the `emit_effects_*` dispatch wrappers and the
`EX_CALLCC` dispatch arm that D2's exit made dead.

### Phase D4 -- delete the N6.5 delimited-control carve-out

The N6 fallback-removal plan retained a **named carve-out** (N6.5) that keeps the
delimited-control family routed to `emit_cps.c` because it was the sole emitter
for those shapes (see
[N6 plan](../v1/cps-backend-n6-fallback-removal-plan.md) N6.5). Once D1-D3 remove
that sole-emitter status, delete the carve-out: the routing that special-cases
the delimited family in the coloring/eviction path is no longer needed, and "the
CT-IR backend is the sole lowering for every colored function" becomes true
without exception.

### Phase D5 -- retire the gates and the files

- Replace the syntactic `emit_cps_program_uses_*` prelude gates
  (`emit_module.c` :6647-6651) with the CT-IR taint classification (`ensure_S`),
  the deeper U6 move: prelude emission is driven by which families the
  *classification* proves are used, not by a second form-presence scan. (The
  preludes themselves stay in `emit_dk_runtime.{c,h}` -- they are the shared
  runtime both the fixed-ABI wrappers and the colored bodies call.)
- Delete `emit_cps_program_uses_delimited` / `_callcc` / `_cloneable_dk` and
  their declarations in `emit_cps.h`.
- `emit_cps.c` is now empty (runtime relocated in step 1, lowering deleted in D3,
  gates deleted here) -> remove `emit_cps.c` and `emit_cps.h`, drop the
  `#include "emit_cps.h"` sites and the CMake source entry.

## Verification strategy

- **The direct path loses its differential net at graduation.** The
  `cps-oracle-*` twins were built to pin `direct == cps` by toggling the flag;
  post-graduation both twins run CPS (there is no toggle), so they no longer
  compare backends -- they remain valid *output-correctness* fixtures but stop
  exercising the direct lowering. Until D3 deletes it, keep the direct lowering
  under test via **the eviction fixtures** (`cps-backend-*` fixtures that
  intentionally hit non-emittable shapes -- non-scalar values, owning pointers,
  Tier C crossings -- and therefore fall back to the direct emitter), plus, if a
  finer net is wanted, a **debug-only `--force-direct-lowering` knob** (a codegen
  knob, not an experiment) that forces the direct path for a named function so a
  `direct == cps` oracle can be reconstructed per shape during D1/D2. The knob is
  deleted in D3 with the lowering it tests.
- **Caller-count is the progress metric.** D1 and D2 each drive a *countable*
  caller population to zero; instrument with a one-line `--dump-*` counter (or a
  grep over emitted C for the direct lowering's signature symbols) so "residual
  delegations = 0" and "direct-dispatch callers = 0" are measured, not asserted.
  **Landed:** `--dump-direct-lowering-callers` (a codegen knob in `globals.{c,h}`,
  wired in `main.c`; the emit hook is `emit_cps_note_direct_caller` in
  `emit_cps_ir.c`, called from the three `emit_effects_*` wrappers and the
  `EX_CALLCC` dispatch). It prints one line per reach into the direct lowering:

  ```
  direct-lowering-caller: <eviction|direct-dispatch> <family>-<emit|fallback>
  ```

  where `eviction` = Population 1 (colored function evicting a sub-shape via
  CT_LETRAW -- attributed via the `g_cps_delegating` bracket in `emit_letraw`),
  `direct-dispatch` = Population 2 (function not in `S`, emitted wholly by the
  direct emitter), and `emit` = the lowering actually emitted (a genuine
  emit_cps.c caller; the delete target) vs `fallback` = returned NULL and the
  inline path in `emit_effects.c` ran (NOT an emit_cps.c caller). Run it across
  the corpus with `emit-c --dump-direct-lowering-callers <file> 2>&1 >/dev/null`.

  ### Measured baseline (corpus scan, 2026-07-11)

  Scanning `emit-c` over every fixture input, counting only genuine `-emit`
  reaches (the live callers of the four lowering functions):

  | Population | Family | `-emit` reaches | Distinct fixtures |
  |---|---|---:|---:|
  | **eviction** (P1) | callcc | 2 | `cps-oracle-escape-capture-in-shift-body`(+`-cps` twin) |
  | **direct-dispatch** (P2) | serial | 42 | 17 |
  | **direct-dispatch** (P2) | cloneable | 34 | 10 |
  | **direct-dispatch** (P2) | callcc | 29 | 17 |
  | **direct-dispatch** (P2) | reset | 2 | 2 |

  This materially updates the plan's premise. **Population 1 (Phase D1) is
  essentially closed**: the U7 work
  ([U7 readiness](cps-backend-unification-u7-readiness-plan.md)) drove the
  cloneable / serial / reset colored-eviction residue to zero; the *only*
  remaining P1 reach is `call/cc` captured inside a `shift` body (one logical
  fixture + its `-cps` twin). D1a-D1c as written are therefore already landed
  for everything except this narrow callcc-in-shift-body tail.

  **Population 2 (Phase D2) is the dominant blocker** -- 41 distinct fixtures.
  `in_s` (`emit_cps_ir.c` ~:1841) is false, and the function wholly direct-emits,
  for four reasons; P2 splits along them:
  - `main` / `c_export_name` -- excluded by construction; the fix is the D2b
    fixed-ABI entry wrapper (`main__cps(dk_done())` / `f__cps(args, dk_done())`).
  - `fn_sig_ok` / `term_core_ok` reject -- the emittable-subset boundary (the
    five v1 gap plans + D1c context-grammar widening + D1d `needs_heap_join`).

  D3-D5 (deleting the lowering, the carve-out, the files) stay gated on **both**
  populations reaching zero `-emit`, i.e. on closing the callcc-in-shift-body P1
  tail *and* the full D2 uncolored/main/subset-reject population.
- **Snapshot churn** regenerates in the same PR as each phase that moves codegen
  (D2 in particular re-emits every uncolored delimited function), per the
  CLAUDE.md fixture-regen recipe.

## Scope and non-goals

- **Async/await are out of scope.** They lower to a separate stackful `ucontext`
  fiber runtime (`FiberBlock` / `TurScheduler`, `emit_module.c`), not the DK
  machine, and are untouched by the `emit_cps.c` delete. The stackful->stackless
  migration is a decoupled effort (U5 note), not a blocker here.
- **No runtime redesign.** The shared DK runtime (`emit_dk_runtime.c`) and the
  fiber runtime stay as-is; this plan only removes the *direct-style emit* that
  drives the DK runtime, unifying on the CT-IR emit.
- **No new control semantics.** Multi-shot resume of a single-shot continuation
  stays `TUR-E0201` on the unified path exactly as today.

## Dependency summary

```
graduation (DONE) ──► D1 (colored residue) ──┐
                                              ├─► D3 (delete lowering) ─► D4 (delete carve-out) ─► D5 (delete files)
                      D2 (uncolored/main) ────┘
```

D1 and D2 are independent and can land in either order (or interleaved); D3 waits
on both reaching zero callers. D4 waits on D3. D5 waits on D4 and folds in the
deeper U6 classification-driven prelude gating.
