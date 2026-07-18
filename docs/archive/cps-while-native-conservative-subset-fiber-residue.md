# The native `while`-loop lowering covers only a conservative subset -- richer loop shapes still evict to the fiber

**STATUS: RESOLVED (verified 2026-07-18).** The while-loop effect fixtures now
DK-lower under `--enable=cps-tramp-resume` with zero `tur_effect_perform("` call
sites: `cps-tramp-resume-while-handle`, `cps-tramp-resume-while-handle-escape`,
`cps-tramp-resume-while-readset`, `cps-tramp-resume-loop-in-handle-continuation`,
and `effect-handler-capture-loop`. The conservative-subset residue this report
tracked has closed. Archived from `docs/reported/`.

**Severity:** medium (correctness is fine -- every out-of-subset shape evicts to
the fiber and runs correctly). It is an **endgame blocker**: the
`cps-dk-sole-effect-lowering-plan` finish line is DELETING the fiber effect
runtime, which requires ZERO `eff=1` functions. A `while` with an interior/escaping
control op that falls outside the `CT_LOOP` subset stays `eff=1`, so the fiber
runtime cannot be deleted while any such program is in scope.

**One-line:** the `CT_LOOP`/`CT_CONTINUE` native loop lowering
(`docs/archive/cps-while-loop-with-interior-handle-no-native-lowering.md`) admits
only a conservative subset -- entry-version-only reads, one unconditional `set!`
per carried var, a single live-after var, no nested loops, interior effect
discharged within the loop. Every richer `while`-with-effect shape still evicts
`eff=1` under `--enable=cps-tramp-resume`.

## The uncovered shapes (each a residual `eff=1`)

Measured on branch `claude/effect-reopen-report-w2n5zh` (2026-07-17):

| Shape | Fixture | Status under the flag |
| --- | --- | --- |
| A `set!` reads a var an EARLIER `set!` wrote this iteration | `tests/fixtures/cps-tramp-resume-while-readset` | still `BODY-UNSUPPORTED eff=1` (open) |
| Interior `perform` ESCAPES to an OUTER handler | `tests/fixtures/cps-tramp-resume-while-handle-escape` | **RESOLVED 2026-07-17** -- DK-lowers (`run__cps`, eff=0), prints 50 |

Also uncovered (no dedicated fixture yet, but the guards reject them): a `set!`
inside an `if`/`match` arm (conditional set), more than one live-after loop var,
and nested loops.

**Escaping-effect landed** (fix direction 3): the loop helper already threads its
`__kont` (the enclosing handle's prompt chain), so the interior `perform` reaches
the outer handler.  It only needed the perform continuation -- which carries the
loop back-edge (`CT_CONTINUE`) -- to be admitted into `perform_cont_reset_ok`
(mirroring its `CT_TAILCALL` case) so `emit_perform` lifts it as an
`LH_RESUME_CONT` resume-frame (carrying `__kont`).  Verified against
`cps-tramp-resume-while-handle-escape` (50) plus a non-zero-init `i*perform`
variant (63) and a two-escaping-effect data-dependent recurrence (`total*2+1` ->
31).  The remaining open shapes are the mutation-width ones below.

## Root cause (by design -- the guards trade coverage for soundness)

The native lowering sidesteps a general SSA pass by resolving in-body reads to the
loop-ENTRY (param) version (by naming) and writing `set!`s to pre-created `$next`
CVars. That is sound ONLY inside a narrow subset, enforced by:

- `loop_guard` read-after-set reject -- `src/passes/cps_ir.c:2593`
  (`if (idx >= 0 && (*mask & (1u << idx))) return false;`). A read of an
  already-set carried var would resolve to the entry version -> miscompile, so it
  evicts. This is exactly the `readset` fixture.
- `loop_guard` conditional-set reject -- `src/passes/cps_ir.c:2603`
  (`if (in_branch) return false;`). A `set!` under an `if`/`match`/handler-case
  arm would leave the single-back-edge `$next` binder unbound on the other path.
- `build_loop` "every carried var set exactly once" -- `src/passes/cps_ir.c:2710`.
- `build_loop` single-live-after continuation -- `src/passes/cps_ir.c:2685`
  (the continuation must be a trivial `CT_APPCONT` delivering ONE loop-carried
  var; multiple live-after vars evict).
- `build_loop` nested-loop reject -- `src/passes/cps_ir.c:2725`
  (`if (saved_n_loop != 0) return NULL;`).
- The ESCAPING-effect shape never reaches `build_loop` at all: the enclosing
  `handle` diverts `run` to `BODY-STRUCT-OR-TAINT` before the `EX_WHILE` case
  fires. Structurally the loop helper's `__kont` DOES chain to the outer handler
  (installed in `run__cps` around the entry call), so an interior `perform` could
  thread out to it -- it is simply not admitted yet.

## Self-caveat -- the remaining guard-test companion is still fiber-dependent

`cps-tramp-resume-while-handle-escape` now DK-lowers (fix direction 3 landed), so it
is endgame-neutral.  `cps-tramp-resume-while-readset` is still an eviction test: it
prints the correct value via the fiber and therefore DEPENDS on the fiber runtime
existing.  At endgame it must be revisited -- either the mutation-width widening
(fix direction 1) covers it (then it becomes `run__cps`, zero `eff=1`), or it
converts to expected-hard-error.  It is a correct test of TODAY's conservative
behavior but is NOT endgame-neutral -- do not read its green status as "no fiber
needed."

## Fix directions

1. **Mutation width (readset + conditional/multi-set).** Replace the
   entry-version-only guard with a real SSA-renaming PRE-PASS over the loop body:
   a forward walk assigns each `^mut` read the version in effect at that point, so
   a read after a `set!` resolves to the updated `$next` rather than the entry
   param. This removes `loop_guard`'s read-after-set (`:2593`) and conditional-set
   (`:2603`) rejects. Verify: a fizzbuzz-style loop whose later `set!` reads an
   earlier one DK-lowers and prints correctly.
2. **Multiple live-after vars.** `build_loop`'s single-live-after restriction
   (`:2685`) needs a multi-value exit: either a multi-arg exit continuation or a
   synthesized tuple/struct delivered to the caller's continuation, which then
   destructures. (The report's obstacle-2 "multi-arg join" concern, now on the
   exit side rather than the back-edge.)
3. **Escaping interior effect.** LANDED (2026-07-17).  `build_loop` already fired
   for this shape; the block was that the interior `perform`'s continuation (which
   carries the `CT_CONTINUE` back-edge) was not admitted.  Added a `CT_CONTINUE`
   case to `perform_cont_reset_ok` (mirroring `CT_TAILCALL`), so `emit_perform`
   lifts the continuation as an `LH_RESUME_CONT` resume-frame (carrying `__kont`)
   and the back-edge threads out to the enclosing handler.
   `cps-tramp-resume-while-handle-escape` now DK-lowers to `run__cps` (eff=0),
   output `50`.  Paper trail:
   `docs/archive/history/cps-while-loop-with-interior-handle-no-native-lowering.md`.
4. **Nested loops.** Lift the `n_loop != 0` reject (`:2725`) once the loop state in
   `CpsB` is a stack rather than a single frame.

## Context

Follow-up to the landed native loop lowering
(`docs/archive/history/cps-while-loop-with-interior-handle-no-native-lowering.md`).
Tracked under `docs/upcoming/v2/cps-dk-sole-effect-lowering-plan.md` (the
"native CPS loop-lowering" entry, now marked LANDED for the conservative subset).
This report is the residue that stands between that subset and the fiber-runtime
deletion endgame.
