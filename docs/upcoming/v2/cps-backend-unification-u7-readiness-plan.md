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
| `emit_cps_reset` | `emit_effects_reset` (emit_effects.c:1218) | `CT_RESET`/`CT_SHIFT` native for the `delim_ok` subset, now incl. **nested + sibling-nested reset** | reset/shift-specific gap is **closed** (escape/branch native since U1; nested reset + sibling nested resets admitted -- see resetshift-gap note). The only nestings still evicting do so via the *generic* `needs_heap_join` boundary (a non-tail cps->cps **call** on the heap chain), shared with the whole C1 subset -- not reset/shift-specific |
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

1. **Relocate the runtime (safe, available now). [DONE]** Move the prelude
   emitters out of `emit_cps.c` into a neutral module (`emit_dk_runtime.{c,h}`)
   that both the direct emitter and the CT-IR backend include. Mechanical and
   behavior-preserving (the emitted C is byte-identical; only the emitter's C file
   location changes). This decouples the runtime's lifetime from the lowering's,
   converting "delete a 2.1k-line file with load-bearing runtime in it" into
   "delete four functions with no callers." **This is the one U7-enabling slice
   that does not wait on any native gap.**

   *Refinement discovered during the cut:* the only code the surviving runtime
   needs is the **four prelude emitters** (`emit_cps_runtime_prelude`,
   `emit_cps_callcc_prelude`, `emit_cps_cloneable_bridge_prelude`,
   `emit_cps_serial_runtime_prelude`) -- pure string emitters with zero
   dependency on `emit_cps.c`'s analysis state. The framing above listed the
   shared analysis helpers (`collect_ctx`, `cl_can_lower`, `frame_c_expr`,
   `sk_tag_for_frame`, `ctx_if_branch`, ...) as also needing relocation on the
   belief that the native CT-IR emit depends on them; in fact `emit_cps_ir.c`
   carries its **own byte-for-byte copies** (see its `sk_tag_for_frame` /
   `frame_c_expr` mirrors) and depends only on the *emitted* runtime
   (`dk_copy_range`, `dk_invoke`, `__sk_frame_for_tag`, ...), not on the C-level
   helpers. So the analysis helpers are private to the lowering functions and
   die with them in step 3; they were correctly left in `emit_cps.c`. The
   `emit_cps_program_uses_*` gates that decide whether to emit each prelude also
   stay in `emit_cps.h` for now (step 5 retires them in favor of the CT-IR
   classification). `emit_cps.c` is now the four lowering functions + their
   private analysis helpers + the uses-gates.
2. **Close the native gaps**, each its own slice, each removing one lowering fn's
   last caller:
   - callcc: native `CT_CALLCC` emit (the largest single gap).
   - cloneable/serial closure receivers + serial 2-arg/Shape 1 (accepting the
     duplication, or a shared closure-lowering helper the runtime relocation could
     also host).
   - base reset/shift shapes outside `delim_ok` -- **the reset/shift-specific gap
     is now closed**: escape/branch shapes went native in U1; nested reset
     (`delim_ok` CT_RESET case) and sibling nested resets (`collect_caps_rec`
     CT_RESET/CT_SHIFT arms) are admitted (see
     [cps-backend-unification-u7-resetshift-gap.md](cps-backend-unification-u7-resetshift-gap.md)).
     The only residual is the generic `needs_heap_join` boundary (a non-tail
     cps->cps *call* on the heap chain), shared with the whole C1 subset, not
     reset-specific.
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
- **Step 1 (runtime relocation) is landed.** The four DK prelude emitters now live
  in `emit_dk_runtime.{c,h}`; `emit_module.c` includes the new header and the
  emitted C is byte-identical (verified against the `continuation-*` snapshots;
  full suite green). The runtime no longer shares a translation unit with the
  lowering functions U7 deletes, so steps 2-5 delete lowering without touching the
  runtime.

## Recommendation

The **runtime relocation (step 1) is now landed** -- `emit_cps.c` no longer holds
the load-bearing runtime, so the eventual delete is "remove four lowering
functions with no callers," not "carve runtime out of a 2.1k-line file."

The remaining U7 work is step 2's **native gaps**, and those are real projects
sized like the U3/U4 native ports, not one-turn slices: native `CT_CALLCC` emit
(the largest, 100% delegated today), the cloneable/serial closure receivers +
serial 2-arg/Shape 1 tail, and base reset/shift shapes outside `delim_ok`. They
should be scheduled as their own slices. Until each gap's last caller is gone the
delegation + eviction fallbacks are correct and the tree ships; the deletes
(steps 3-5) follow mechanically once coverage is total.
