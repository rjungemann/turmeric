---
title: "U7 readiness -- the emit_cps.c dependency surface and the cut sequence"
status: proposed
parent: cps-backend-unification-plan.md
description: A call-graph-grounded assessment of what still depends on emit_cps.c, correcting the framing of U7. "Retire emit_cps.c" is not one delete -- the file holds TWO kinds of code: the direct-style LOWERING functions the CT-IR backend is replacing (deletable once native coverage is complete), and the RUNTIME PRELUDE emitters + shared helpers that the NATIVE CT-IR emit itself depends on (must be RELOCATED, not deleted). This note maps both, sequences the cut, and identifies the one U7-enabling slice that is safe today (relocating the runtime) vs the native gaps that are the real blockers (callcc, closure receivers).
---

# U7 readiness -- cutting the emit_cps.c dependency

## The framing correction

The parent plan's U7 says "Delete the file." That is not accurate as stated:
`emit_cps.c` (~2.1k lines) holds **two separable bodies of code**, and only one
is deletable.

1. **Direct-style LOWERING functions** -- `emit_cps_reset`,
   `emit_cps_cloneable_reset`, `emit_cps_serial_reset`, `emit_cps_callcc`. These
   are the direct emitter's lowering of delimited control, which the CT-IR backend
   is replacing. **These are what U7 deletes** -- but only once the CT-IR backend
   covers every shape they handle (no eviction, no `CT_LETRAW` fallback into them).

2. **RUNTIME PRELUDE emitters + shared helpers** -- `emit_cps_runtime_prelude`
   (the DK multi-prompt machine: `dk_prompt`/`dk_shift`/`dk_run`/`dk_copy_range`
   /...), `emit_cps_serial_runtime_prelude` (`__sk_frame_for_tag`, the marshaler),
   `emit_cps_cloneable_bridge_prelude`, `emit_cps_callcc_prelude`, and the shared
   analysis helpers (`collect_ctx`, `cl_can_lower`, `frame_c_expr`,
   `sk_tag_for_frame`, `ctx_if_branch`, ...). **The native CT-IR emit DEPENDS on
   these**: the C that `emit_cloneable` / the serial branch generate calls
   `dk_shift`, `dk_copy_range`, `__sk_frame_for_tag`, etc., which are *defined by
   these preludes*. So this body of code cannot be deleted -- it must be
   **relocated** to a neutral home (`emit_module.c` or a new `emit_dk_runtime.c`)
   that both backends share.

So U7 is really: **delete the lowering functions, relocate the runtime.** Naming
that split is the main output of this note.

## The lowering-function dependency surface (what blocks the deletes)

Each lowering function is reached from the direct emitter, both by plain direct
emission and by the CT-IR `CT_LETRAW` delegation (which routes through
`emit_value` -> the direct emitter). To delete each, its native CT-IR coverage
must be total:

| Lowering fn | Reached from | Native CT-IR status | Gap to delete |
|---|---|---|---|
| `emit_cps_reset` | `emit_effects_reset` (emit_effects.c:1218) | `CT_RESET`/`CT_SHIFT` native for the `delim_ok` subset | shapes outside `delim_ok` (setjmp/longjmp escape path, non-admitted join shapes) still evict to direct -> this fn |
| `emit_cps_cloneable_reset` | `emit_effects_cloneable_reset` (:1239) + delegation | value-typed subset native (arith/call/let/if/do) | **closure/colored receivers** delegate here (porting them duplicates this fn's closure machinery -- see U3 steps 6-7 note) |
| `emit_cps_serial_reset` | `emit_effects_serial_reset` (:1703) + delegation | value-typed subset native (arith/call/let/if/do) | **2-arg call frames** (serialized env codec), **Shape 1 identity**, closure receivers delegate here |
| `emit_cps_callcc` | `EX_CALLCC` dispatch (emit_expr.c:2826) + delegation | **none -- 100% delegated** (U2 CT_LETRAW) | all of call/cc + escape has no native CT-IR emit |

The two biggest gaps are **callcc** (no native emit at all) and **cloneable/serial
closure receivers** (whose native port duplicates the very machinery being
retired). Base reset/shift is the smallest gap (a bounded set of non-`delim_ok`
shapes). These gaps are the real content of "finish U3/U4 + do callcc natively,"
and some are the "would duplicate emit_cps.c" cases the U3 steps-6-7 note already
flagged as deferred.

## The cut sequence

1. **Relocate the runtime (safe, available now).** Move the prelude emitters +
   shared DK/marshaling helpers out of `emit_cps.c` into a neutral module that
   both the direct emitter and the CT-IR backend include. Mechanical and
   behavior-preserving (the emitted C is byte-identical; only the emitter's C file
   location changes). This shrinks `emit_cps.c` to just the four lowering functions
   and their private helpers, making the remaining delete surface explicit and
   decoupling the runtime's lifetime from the lowering's. **This is the one
   U7-enabling slice that does not wait on any native gap.**
2. **Close the native gaps**, each its own slice, each removing one lowering fn's
   last caller:
   - callcc: native `CT_CALLCC` emit (the largest single gap).
   - cloneable/serial closure receivers + serial 2-arg/Shape 1 (accepting the
     duplication, or a shared closure-lowering helper the runtime relocation could
     also host).
   - base reset/shift shapes outside `delim_ok`.
3. **Delete the lowering functions** once each has zero callers: remove
   `emit_cps_reset` / `_cloneable_reset` / `_serial_reset` / `emit_cps_callcc`, the
   `emit_effects_*` dispatch wrappers, and the `EX_CALLCC` direct dispatch; drop
   the `CT_LETRAW` delegation for the now-native families.
4. **Delete the N6.5 delimited-control carve-out** in the coloring/routing (the
   fallback that keeps unsupported shapes on the direct path) -- valid only once no
   shape needs it.
5. **`emit_cps.c` is now empty** (runtime relocated, lowering deleted) -> remove
   the file and `emit_cps.h`; retire the `emit_cps_program_uses_*` gates in favor
   of the CT-IR classification (this is also U6's deeper move).

## Progress already banked toward this

- `emit_cps_program_uses_serial_dk` was dead (defined, never called) and is
  removed -- one fewer public symbol on the surface.
- U6 first slice computes the prelude gates once (`cps_uses_*`), which is exactly
  the call-site shape step 5's classification wants to feed.
- U3/U4 native ports have already cut the *common* cloneable/serial shapes over to
  native, so the delegation into `emit_cps_cloneable_reset` / `_serial_reset` now
  fires only for the residual hard shapes named above -- the deletes are closer,
  just gated on the hard tail.

## Recommendation

Do the **runtime relocation (step 1) next** -- it is the only safe, bounded,
high-leverage U7 step available before the native gaps close, and it converts
"delete a 2.1k-line file with load-bearing runtime in it" into "delete four
functions with no callers." The native gaps (callcc especially) are real projects
sized like the U3/U4 native ports, not one-turn slices, and should be scheduled as
such. Until then the delegation + eviction fallbacks are correct and the tree
ships.
