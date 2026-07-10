---
title: Unifying the two CPS backends (emit_cps.c + emit_cps_ir.c)
status: proposed
description: The compiler carries two independent CPS lowerings onto the shared DK multi-prompt runtime -- emit_cps.c (older whole-program transform owning the delimited-control family) and emit_cps_ir.c (newer per-function CT-IR backend owning colored effect functions). This plan folds emit_cps.c's delimited-control family into the CT-IR backend so there is one lowering, one classification, and one place the DK runtime is driven -- which also closes the N6.5 delimited-control fallback carve-out.
---

# Unifying the two CPS backends

## 1. Where we are: two lowerings, one runtime

The compiler currently CPS-lowers colored (may-capture / effectful) functions
through **two independent code paths** that both target the same DK
multi-prompt runtime (`dk_run`, `dk_frame`, `dk_handler`, `dk_perform`,
`dk_shift`/`dk_shift0`, `dk_invoke`, `dk_prompt`, `dk_done`,
`dk_copy`/`dk_copy_range`):

### `emit_cps.c` -- the older whole-program transform (~1990 lines)

Owns the **delimited-control family** and drives it as a whole-program CPS
transform, gated by *syntactic form presence* in the program:

| Feature | Gate (form-presence probe) |
| --- | --- |
| reset / shift / shift0 | `emit_cps_program_uses_delimited` (line 85) |
| call/cc, escape | `emit_cps_program_uses_callcc` (line 140) |
| cloneable-reset / cloneable-shift (multi-shot) | `emit_cps_program_uses_cloneable_dk` (line 1308) |
| serial-reset / serial-shift | `emit_cps_program_uses_serial_dk` (1662), `emit_cps_program_contains_serial` (1672) |
| async / await | (rides the cloneable/serial machinery) |

It carries its own DK deep-clone (`dk_copy_range`) plus capture clone/drop glue
(`live_captures` / `capture_clone_fns` / `capture_drop_fns`), and emits the
runtime preludes: `emit_cps_runtime_prelude` (1883),
`emit_cps_cloneable_bridge_prelude` (1316), `emit_cps_serial_runtime_prelude`
(1684), `emit_cps_callcc_prelude` (426).

### `emit_cps_ir.c` -- the newer per-function CT-IR backend (~2769 lines)

Owns **colored effect functions** (perform / handle / reset / shift / shift0),
gated by the `--enable=cps-backend` experiment. Built on:

- a per-function ANF/CPS IR (`CTerm` / `CAtom` / `CKont`) produced by
  `cps_ir.c` (~1041 lines);
- a **whole-program effect-taint classification** (`ensure_S`) with a fixpoint
  that keeps performers and their handlers co-located on the same machine (DK
  vs. fiber), so the two never split;
- delegation to the direct emitter via `CT_LETRAW` for anything it does not own;
- the generic-monomorph levers (G1-G3b), effectful callbacks, and multi-arg
  effects landed under N6.

It does **not** emit the cloneable / serial / async family: those shapes fall
back to `emit_cps.c`, and the N6 classification is written to route them there
correctly.

### The coloring pass

Upstream of both, `cps_color_program` (`cps.c`, ~1453 lines) sets
`fd->cps_colored`. Both backends consume that coloring; only the CT-IR backend
adds the finer taint classification on top.

## 2. Why two backends is a problem worth closing

- **Two classifications that must agree.** `emit_cps.c` decides what it owns by
  *syntactic form presence*; `emit_cps_ir.c` decides by *whole-program effect
  taint*. Today they are kept consistent by hand -- the CT-IR taint is authored
  to evict exactly the shapes `emit_cps.c` claims. Every new control form risks
  a gap where both claim a function or neither does.
- **Two DK-driver implementations.** Capture clone/drop glue, `dk_copy_range`
  deep-clone, and the prompt/handler wiring exist in both files with subtly
  different invariants. A runtime fix (e.g. a capture-drop ordering bug) has to
  be made twice or risks divergence.
- **The N6.5 carve-out cannot close.** The N6 fallback-removal plan can delete
  the general whole-function `CT_UNSUPPORTED` fallback *except* for the
  delimited-control family, which must stay routed to `emit_cps.c`. As long as
  `emit_cps.c` exists, N6.5 ships with a permanent carve-out rather than a
  clean "CT-IR is the sole lowering for colored functions" statement.
- **Onboarding + audit cost.** ~2000 lines of the older transform must be held
  in the reader's head alongside the newer backend to reason about any colored
  function.

## 3. Goal and direction

**Make `emit_cps_ir.c` the single CPS lowering for every colored function,
including the delimited-control family, and retire `emit_cps.c`.**

Direction rationale -- why fold *into* the CT-IR backend rather than the
reverse:

- The CT-IR backend is the more principled design: an explicit ANF/CPS IR, a
  whole-program taint classification with a proven fixpoint, and a clean
  delegation boundary (`CT_LETRAW`). `emit_cps.c` is a direct syntax-directed
  transform with form-presence gates -- the weaker foundation to build the
  union on.
- The taint classification already reasons about the delimited-control shapes
  (it evicts them); it is a *superset-in-waiting*, not a competitor. Porting
  the emit is additive to a classifier that already understands the shapes.
- The DK runtime is shared, so the port is "teach the CT-IR emitter to produce
  the cloneable/serial/callcc DK sequences," not "re-invent the runtime."

## 4. What must be ported

Each item below moves from `emit_cps.c` into the CT-IR pipeline (`cps_ir.c` for
IR forms, `emit_cps_ir.c` for emit, shared runtime for preludes):

1. **New CT-IR forms** for the delimited-control family that the current
   `CTerm` set does not model:
   - `reset` / `shift` / `shift0` as first-class CT nodes (today the CT-IR
     backend handles *effect-based* reset/shift via handle/perform lowering;
     the raw delimited forms are what `emit_cps.c` owns). Decide whether raw
     `reset`/`shift` lower to the same CT node as `handle`/`perform` or a
     distinct one.
   - `cloneable-reset` / `cloneable-shift` (multi-shot capture).
   - `serial-reset` / `serial-shift`.
   - `call/cc` / `escape`.
   - `async` / `await`.
2. **`cps_ir.c` translation** from surface AST to the new CT nodes, reusing the
   existing free-variable / capture analysis (`collect_free_vars`,
   `collect_caps_rec`).
3. **The DK deep-clone + capture glue** (`dk_copy_range`, `live_captures`,
   `capture_clone_fns`, `capture_drop_fns`) driven from CT-IR emit rather than
   the `emit_cps.c` transform. This is the load-bearing piece for multi-shot
   (cloneable/serial) continuations.
4. **The async scheduler** wiring that today rides the cloneable/serial
   machinery.
5. **The runtime preludes** (`emit_cps_runtime_prelude`,
   `emit_cps_cloneable_bridge_prelude`, `emit_cps_serial_runtime_prelude`,
   `emit_cps_callcc_prelude`) -- emitted from the unified path, ideally
   refactored into one prelude emitter keyed by which families the program
   actually uses (preserving the current "only emit what's used" property).

## 5. Unified classification

Replace `emit_cps.c`'s syntactic form-presence gating
(`emit_cps_program_uses_*`) with the CT-IR backend's whole-program
effect-taint classification (`ensure_S`), extended so that:

- The delimited-control families become **taint inputs** the same way effects
  are: a function that uses cloneable/serial/async multi-shot capture is
  classified into the machine that supports it, and its handler/prompt is kept
  co-located by the same fixpoint that already co-locates performer and handler.
- The current eviction rules (which *remove* cloneable/serial shapes from CT-IR
  ownership so they fall back) are inverted into *placement* rules (which
  machine emits them), since after the port there is no fallback target.
- **Multi-shot remains a first-class classification axis.** Single-shot
  (abortive v1 `shift`) and multi-shot (cloneable/serial) continuations need
  different capture strategies; the classifier must distinguish them (it
  already must, to route today) and select clone-vs-move capture accordingly.

The win: one classifier decides machine placement for *all* colored functions,
and there is no second gate to keep in sync.

## 6. Phased plan

The port is large; stage it so each phase lands suite-green behind the existing
`--enable=cps-backend` experiment, with `emit_cps.c` still present as the
fallback until the final phase removes it.

- **U0 -- Inventory + oracle fixtures.** Enumerate every program shape
  `emit_cps.c` currently owns (reset/shift/shift0, cloneable, serial, callcc,
  escape, async/await, and their interactions with handle/perform). Turn each
  into a round-trip fixture asserting direct-vs-CPS value equality, run under
  *both* backends today (CT-IR falls back to `emit_cps.c`, so both are the same
  emit now -- these fixtures become the regression net for the port). No code
  change. **Landed** -- see
  [cps-backend-unification-u0-inventory.md](cps-backend-unification-u0-inventory.md)
  (12 shapes, 24 `cps-oracle-*` twin fixtures).
- **U1 -- Raw reset/shift/shift0 into CT-IR.** Add the CT nodes + `cps_ir.c`
  translation + emit for single-shot delimited control. Extend `ensure_S` to
  place these on DK instead of evicting them. Flip the fallback for exactly
  these shapes. Keep cloneable/serial/callcc/async still routed to
  `emit_cps.c`. **Partially landed.** The CT nodes (`CT_RESET`/`CT_SHIFT`,
  `shift0` flag), `cps_ir.c` translation, and DK emit (`emit_reset`/
  `emit_shift` -> `dk_shift`/`dk_shift0`/`dk_run`) already existed and covered a
  restricted zero/scalar-capture subset; the remaining eviction was the
  *admission predicate*. This slice adds `delim_ok` (`src/compiler/
  emit_cps_ir.c`), a reset-delim admission that permits KK_PROMPT delivery
  through straight-line + branch (`if`) structure, so the canonical
  branch-and-escape patterns -- `(reset (if c (shift ...) v))`, nested-if
  escapes, `do`-sequenced shifts -- now lower on DK instead of evicting. Oracle
  pairs: `cps-oracle-reset-if-escape`, `-reset-if-straightline-else`,
  `-reset-nested-if-escape` (direct == cps).

  **Join-bearing shapes now also landed.** A reset delim with a `letcont` join
  whose jbody delivers to the prompt -- a shift under a *non-empty* delimited
  continuation, `(reset (+ 10 (if c (shift ...) 5)))` -- was initially left
  evicted because the default/`emit_cps.c` path *degraded* it to plain body-eval
  (a different value), which would have broken `direct == cps`. That direct-path
  degradation is now fixed: `emit_cps_reset` lowers a branch-bearing base reset
  via a `setjmp`/`longjmp` escape path (`emit_cps_reset_escape`), giving correct
  abortive semantics from inside a branch. With direct corrected, `delim_ok`
  re-admitted the join-bearing shapes (the `CT_LETCONT` case), so they lower on
  DK under the experiment with `direct == cps`. Oracles:
  `cps-oracle-reset-join-escape`, `cps-oracle-reset-both-branch-shift`. Resolved
  report: [docs/archive/direct-reset-shift-degrades-out-of-subset.md](../../archive/direct-reset-shift-degrades-out-of-subset.md).
- **U2 -- call/cc + escape.** Port the `emit_cps_callcc_prelude` machinery.
  **Landed.** `(call/cc f)` / `(escape f)` (both `EX_CALLCC`) is an *undelimited*
  escape: its continuation is captured at a **local setjmp landing** that
  `emit_cps_callcc` establishes inline, so -- unlike shift/perform -- it does not
  thread the DK continuation. A *colored* function that also contains a
  call/cc/escape used to evict wholesale (the `EX_CALLCC` hit `safe_to_delegate`'s
  conservative `default: false`). `EX_CALLCC` is now added to `safe_to_delegate`
  (`src/passes/cps_ir.c`), so the escape is delegated to the direct emitter via
  `CT_LETRAW` and the enclosing colored function stays CPS-emitted.

  - **Capture-free receivers** delegate via the normal is-delegatable-value
    check (a plain fn, fat-boxed fn, or zero-capture closure). Oracles:
    `cps-oracle-colored-escape`, `cps-oracle-colored-callcc`.
  - **Capturing receivers** also delegate: `collect_caps` (`collect_caps_rec`
    `CT_LETRAW`) walks the callcc receiver's free vars into the lifted
    continuation env, so an enclosing capture (a scalar) rides the env; a
    non-Copy capture bails to fallback. Oracle: `cps-oracle-colored-escape-capture`.
  - **Lifted-helper positions (fully landed):** a capturing escape inside a
    DK-lifted helper used to be evicted because the helper env did not carry the
    receiver's captures. Two independent capture walkers each lacked an
    `EX_CALLCC` case: `collect_free_vars` (`elab_core.c`, for the **shift body**
    and **perform continuation** helpers) and `collect_handle_captures`
    (`emit_core.c`, for the fiber **handler case** helper). Both now descend into
    the `(call/cc/escape)` receiver (folding an `EX_CLOSURE`'s captures, or
    collecting a raw `EX_FN` body's free vars minus its params), so all three
    carve-out guards were removed -- a capturing escape in any lifted position
    lowers on DK with `direct == cps`. Oracles:
    `cps-oracle-escape-capture-in-shift-body`, `cps-oracle-escape-capture-after-handle`,
    `cps-oracle-escape-capture-in-handler-case`. (An owning-value capture still
    bails to the direct emitter -- not a Copy capture.) Resolved report:
    [docs/archive/direct-capturing-escape-in-lifted-helper.md](../../archive/direct-capturing-escape-in-lifted-helper.md).
  - **Prelude gate hardened:** `uses_callcc` (the escape-continuation prelude
    gate) was missing many control/value forms (`shift`, `handle`, `perform`,
    `resume`, `match`, `async`, casts, ...), so an escape nested in one lost its
    `tur_escape_cont` prelude and failed to build on *both* backends. Now
    complete (additive -- it only ever emits the prelude when a callcc is
    actually present).

  The change only affects CT-IR emission plus the (additive) prelude gate, so
  default-backend codegen is byte-identical for callcc-free programs.

  *Follow-on (U6/U7):* physically relocating `emit_cps_callcc` +
  `emit_cps_callcc_prelude` out of `emit_cps.c` (prelude consolidation / file
  deletion). No call/cc/escape shapes remain evicted on capture grounds.
- **U3 -- Cloneable (multi-shot) capture.** Port `dk_copy_range` deep-clone +
  capture clone/drop glue driven from CT-IR emit; add the multi-shot
  classification axis. This is the highest-risk phase (capture correctness).
  **First slice landed (delegation).** A `(cloneable-reset body)` is a
  self-contained multi-shot delimited region owned end-to-end by the direct
  emitter (`emit_effects_cloneable_reset` -> `emit_cps_cloneable_reset`, which
  drives `dk_copy_range` + the capture clone/drop glue). `EX_CLONEABLE_RESET` is
  now delegatable via `CT_LETRAW` (`safe_to_delegate`, `src/passes/cps_ir.c`) --
  the bare `cloneable-shift` stays non-delegatable (it captures the rest of its
  reset body) -- so a colored function containing a cloneable-reset stays
  CPS-emitted (the region direct-emitted as a unit) instead of wholly evicting,
  reusing the proven multi-shot runtime. Multi-shot resume is exercised by
  `cps-oracle-cloneable-multi-resume`; the mixed case (base reset/shift + a
  cloneable-reset in one function) by `cps-oracle-cloneable-mixed`.

  This slice also fixed a CPS-backend infrastructure bug: a `CT_LETRAW`
  delegation emits its file-scope helper fns into `ctx->pending_handler_fns`,
  which the direct-function path flushes ahead of the using function but the
  `__cps` function path did not -- so a delegated cloneable-reset's `__cont_fn`
  was defined after its use (`'__cont_fn_N' undeclared`). The CPS emitter now
  flushes `pending_handler_fns` before each `__cps` body (a general fix for any
  helper-emitting delegation).

  Also fixed the cloneable prelude-gate gap: `cps_expr_contains_cloneable_shift`
  (`src/passes/cps.c`) now descends into `EX_BUILTIN` and the other missing
  control/value forms, so a `cloneable-shift` nested under an operator
  (`(+ 1 (cloneable-reset ...))`) emits its `tur_cloneable_cont` prelude on both
  backends. Oracle: `cps-oracle-cloneable-nested-op`. Resolved report:
  [docs/archive/cloneable-prelude-gate-misses-nested-shift.md](../../archive/cloneable-prelude-gate-misses-nested-shift.md).

  **Native emit -- Shape 1 landed.** The staged native port (see
  [cps-backend-unification-u3-native-emit-plan.md](cps-backend-unification-u3-native-emit-plan.md))
  begins with the trivial (identity) continuation: `(cloneable-reset
  (cloneable-shift receiver val))` where the shift is the whole reset body, so
  there is no `dk_copy_range`. A new `CT_CLONEABLE` node (`cps_ir.h`) is
  translated for that shape with a named uncolored receiver (`build_cloneable`,
  `cps_ir.c`; other shapes fall through to the delegation) and emitted natively
  (`emit_cloneable`, `emit_cps_ir.c`): an identity continuation fn +
  `tur_cloneable_cont_alloc(id, NULL, NULL, NULL)` + the receiver call -- no
  `emit_cps.c` involvement. Multi-shot resume is trivially correct (the identity
  continuation is stateless), verified natively by
  `cps-oracle-cloneable-native-shape1` (clone + two resumes -> 10/20). All
  cloneable-basic-style fixtures now emit Shape 1 natively; everything else keeps
  delegating.

  **Native emit -- Shape 2 (single frame) landed.** The first non-trivial
  continuation now emits natively: `(cloneable-reset (<op> <int-lit>
  (cloneable-shift receiver val)))` for `op` in `+ - * /` (either hole side).
  `build_cloneable` reifies the `(<op> operand [])` context and `emit_cloneable`
  emits the DK chain directly -- an arithmetic frame fn, a shift-body helper that
  `dk_copy_range`s the captured sub-continuation into a `tur_cloneable_cont`, and
  `dk_prompt`/`dk_frame`/`dk_shift`/`dk_run`/`dk_free` -- reusing the shared DK
  runtime (`dk_copy_range`, `__dk_cont_fn`/`__dk_env_clone`/`__dk_env_drop`)
  byte-for-byte, no `emit_cps.c`. Multi-shot verified natively across all four
  operators and both hole sides; oracle `cps-oracle-cloneable-native-shape2`
  (clone + two resumes -> 15/110).

  Shape 2 now admits a **captured (var) frame operand** (`(+ n (cloneable-shift
  k 0))` with `n` a parameter; oracle `cps-oracle-cloneable-native-shape2-var`)
  and **multi-frame nested arithmetic contexts** -- `(cloneable-reset (* a (+ b
  (cloneable-shift k 0))))` -- reified as a chain of DK frames pushed
  outermost-first. `CT_CLONEABLE` now carries a `CloneFrame[]` (0 frames = Shape
  1, N frames = an N-deep arithmetic context); `build_cloneable` walks the nested
  binop chain, `emit_cloneable` emits one frame fn per level. Oracle:
  `cps-oracle-cloneable-native-shape2-nested` (30/26 across two frames).

  Shape 2 also admits **`let`-bearing** and **`if`-bearing** contexts. A pure
  scalar `let` binding in the spine (`(cloneable-reset (let [a (* base 2)] (+ a
  (cloneable-shift k 0))))`) is direct-emitted as a C local at the reset site
  ahead of the frame operands that reference it; a single pure-conditioned `if`
  branch point (`(cloneable-reset (if (> n 0) (+ 100 (cloneable-shift k 0)) 42))`)
  runs the DK chain on the shift-bearing arm and yields the direct-emitted pure
  arm on the other branch. `build_cloneable` walks the context spine in one loop,
  mirroring `collect_ctx` / `ctx_if_branch` *inline* (descending the shift arm past
  a recorded `if` yields one flat frame chain -- no `clone_spine`); the capture
  walkers surface the free vars of the direct-emitted sub-exprs via
  `collect_free_vars` (node `let` bindings excluded), so a cloneable inside a
  lifted continuation captures correctly. Oracles
  `cps-oracle-cloneable-native-shape2-let` / `-if`. `let` and `if` are kept
  mutually exclusive per lowering (the mix falls through to the still-correct
  delegation).

  *Remaining (the risky core, narrowing):* closure/colored receivers and the
  non-arithmetic `let` shapes (`do`-prelude / call frames) the direct
  `collect_ctx` also handles, each behind its own oracle (the delegation fallback
  still catches everything not yet native), plus the multi-shot classification
  axis. Mapped in the native-emit note.
- **U4 -- Serial.** Port `emit_cps_serial_runtime_prelude` + serial placement.
- **U5 -- Async / await.** Port the scheduler wiring on top of the now-unified
  cloneable/serial base.
- **U6 -- Prelude consolidation.** Fold the four `emit_cps_*_prelude` emitters
  into one "emit the preludes the program uses" pass driven by the unified
  classification.
- **U7 -- Retire `emit_cps.c`.** Delete the file, its gates
  (`emit_cps_program_uses_*`), and the routing in `emit_module.c`
  (~6712-6720). Delete the N6.5 delimited-control carve-out. Graduate the
  experiment if the backend is otherwise ready to be always-on (separate
  decision -- see N6 plan).

Each of U1-U5 flips its family's fallback only after its oracle fixtures pass
under the CT-IR emit; a family whose port is not ready simply keeps falling back
to `emit_cps.c`, so the tree is always shippable mid-port.

## 7. Risks, non-goals, alternatives

### Risks

- **Multi-shot capture correctness (U3) is the crux.** The deep-clone +
  capture clone/drop glue is the subtlest runtime code in `emit_cps.c`; porting
  it wrong miscompiles silently (wrong resumed state) rather than failing to
  compile. Mitigation: the U0 oracle fixtures plus targeted multi-resume
  round-trips, and porting the *runtime* glue byte-for-byte first (same C), only
  changing *who emits the calls*.
- **Classification regressions.** Folding form-presence gating into the taint
  fixpoint could mis-place a function that today lands correctly by the
  hand-tuned agreement between the two gates. Mitigation: U0 fixtures run under
  both emits and must stay equal through each flip.
- **Prelude "emit only what's used" property.** Consolidating four preludes
  into one must not start emitting unused runtime (code-size / leak-check
  regressions). Mitigation: keep per-family usage bits; U6 is a refactor with
  byte-diff review of emitted preludes.
- **Scope creep vs. v1.** This is v2 work; it must not block the N6 v1
  finish-line (which ships with the carve-out intact).

### Non-goals

- **No new control semantics.** This is a *lowering* unification, not a
  language change. Multi-shot resume of a single-shot continuation stays a hard
  error (`TUR-E0201`) on the unified path exactly as on both paths today.
- **No DK runtime redesign.** The shared DK runtime is the target of both
  backends and stays as-is; only the *emit* that drives it is unified.
- **Not a rewrite of the direct emitter.** `CT_LETRAW` delegation to the direct
  emitter stays the boundary for everything colored functions don't own.

### Alternatives considered

- **Fold CT-IR into `emit_cps.c` (the reverse).** Rejected: it would rebuild
  the taint classification and ANF IR on top of the weaker syntax-directed
  transform -- more work, worse foundation.
- **Leave two backends, formalize the boundary.** Rejected as the end state:
  it keeps the duplicated DK glue and the permanent N6.5 carve-out, and every
  new control form pays the two-gate-agreement tax. (It is, however, exactly the
  *interim* state each phase preserves -- so this "alternative" is really the
  migration's safety property, not a destination.)

## 8. Relationship to N6

N6 (v1) removes the general whole-function `CT_UNSUPPORTED` fallback but must
retain a **delimited-control carve-out** (N6.5): cloneable/serial/async/raw
reset/shift keep routing to `emit_cps.c`. That carve-out exists *only* because
`emit_cps.c` is still the sole emitter for those shapes.

This plan is what lets N6.5 close: once U1-U5 land, the delimited-control
family is emitted by the CT-IR backend, and U7 deletes both `emit_cps.c` and
the carve-out. After that, the statement "the CT-IR backend is the sole lowering
for every colored function" is true without exception -- the actual finish line
that N6 approaches asymptotically while the second backend exists.

## Depends on / reuses

- The whole-program effect-taint (`ensure_S`) -- extended, not replaced.
- The shared DK multi-prompt runtime -- unchanged.
- `cps_ir.c` ANF/CPS IR + `collect_free_vars` / `collect_caps_rec` capture
  analysis -- extended with the new control forms.
- Parent (v1): [cps-backend-n6-fallback-removal-plan.md](../v1/cps-backend-n6-fallback-removal-plan.md).
