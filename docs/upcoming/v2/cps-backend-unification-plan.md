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

  *Deliberately still evicted (blocker for the full flip):* a reset delim with a
  `letcont` join whose jbody delivers to the prompt -- i.e. a shift under a
  *non-empty* delimited continuation, `(reset (+ 10 (if c (shift ...) 5)))`.
  There the CT-IR path is correctly abortive but the default/`emit_cps.c` path
  *degrades* an out-of-subset reset to plain body-eval, yielding a different
  value; admitting it would break the `direct == cps` invariant. Tracked in
  [docs/reported/direct-reset-shift-degrades-out-of-subset.md](../../reported/direct-reset-shift-degrades-out-of-subset.md);
  closing it (fix the direct degradation, or U7 retires `emit_cps.c`) unblocks
  admitting the join-bearing shapes.
- **U2 -- call/cc + escape.** Port the `emit_cps_callcc_prelude` machinery.
- **U3 -- Cloneable (multi-shot) capture.** Port `dk_copy_range` deep-clone +
  capture clone/drop glue driven from CT-IR emit; add the multi-shot
  classification axis. This is the highest-risk phase (capture correctness).
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
