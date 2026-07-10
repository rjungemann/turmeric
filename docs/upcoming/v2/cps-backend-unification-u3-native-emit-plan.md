---
title: "U3 core -- native CT-IR emission of cloneable (multi-shot) continuations"
status: proposed
parent: cps-backend-unification-plan.md
description: The remaining core of U3 -- teaching the CT-IR backend to EMIT the cloneable multi-shot machinery itself (rather than delegating the whole cloneable-reset region to the direct emitter). Maps the two emit shapes, where the dk_copy_range multi-shot risk lives, and a staged path that keeps each step suite-green behind the delegation fallback.
---

# U3 core -- native cloneable emission

The U3 first slice delegates a whole `(cloneable-reset ...)` region to the
direct emitter via `CT_LETRAW` (see the parent plan). That keeps colored
functions on the CT-IR path and reuses the proven multi-shot runtime, but the
cloneable *emit* still lives in `emit_cps.c`, so U7 cannot delete that file until
the emit is native. This note maps the native port.

## The two emit shapes (from the direct emitter)

Studied via `emit_cps_cloneable_reset` / `emit_cloneable_ctx` (`emit_cps.c`) and
the emitted C.

### Shape 1 -- trivial continuation (Case 1)

`(cloneable-reset (cloneable-shift receiver val))` where the shift is the whole
reset body. The captured continuation is the identity, so **no DK deep-clone is
needed**. The direct emit is just:

```c
static int64_t __cont_fn_N(void *env, int64_t v) { (void)env; return v; }
...
tur_cloneable_cont *c = tur_cloneable_cont_alloc(__cont_fn_N, NULL, NULL, NULL);
result = receiver((int64_t)(intptr_t)c);   /* receiver gets the cont handle */
/* result is the reset's value */
```

Resuming `c` (any number of times) runs the identity and returns the resumed
value -- multi-shot-correct trivially (the continuation is stateless).

**Native emission is low-risk here** and is the right first native step.

### Shape 2 -- non-trivial continuation (the risk)

`(cloneable-reset (+ 10 (cloneable-shift receiver val)))`, `cloneable-context-if`,
`cloneable-context-let`, etc. The continuation is "the rest of the reset body"
(e.g. `+ 10`), which must be reified as a **DK chain** and deep-cloned so each
resume is independent:

```c
static intptr_t __cc_body_N(intptr_t env, DK *subk) {
    DK *__cap = dk_copy_range(subk, NULL);              /* deep-clone the sub-cont */
    tur_cloneable_cont *__k = tur_cloneable_cont_alloc(
        __dk_cont_fn, __cap, __dk_env_clone, __dk_env_drop);
    return (intptr_t)receiver((int64_t)(intptr_t)__k);
}
/* plus __cc_ctx_N_i frame fns reconstructing the context (+ 10, if-arms, lets) */
```

This is the plan's flagged **"highest-risk (capture correctness)"**: the frame
fns, the `dk_copy_range` deep-clone, and -- for owning captures -- the
`__dk_env_clone` / `__dk_env_drop` glue (each resume clones the captured owning
value; drop runs once per resume, balanced). Getting the clone/drop wrong
miscompiles silently (wrong resumed state / double free), not at compile time.

## Staged native port (each step suite-green behind delegation)

**Steps 1-4 (Shape 1) LANDED.** A single `CT_CLONEABLE` node (`cps_ir.h`) models
the identity-continuation shape; `build_cloneable` (`cps_ir.c`) translates
`(cloneable-reset (cloneable-shift receiver val))` with a named uncolored
receiver and falls through to the `CT_LETRAW` delegation otherwise;
`emit_cloneable` (`emit_cps_ir.c`) emits the identity cont fn +
`tur_cloneable_cont_alloc(id, NULL, NULL, NULL)` + receiver call; admitted in
`term_core_ok` and threaded through the scan walkers. Oracle:
`cps-oracle-cloneable-native-shape1` (native multi-shot resume, 10/20).

**Step 5 (Shape 2, single frame) LANDED.** `CT_CLONEABLE` extended with an
optional single arithmetic context frame (`ctx_op` / `ctx_operand` /
`ctx_hole_left`): `(cloneable-reset (<op> <int-lit> (cloneable-shift receiver
val)))` for `op` in `+ - * /`, either hole side. `emit_cloneable` emits the DK
chain natively -- an arithmetic frame fn (`cloneable_frame_expr`, mirroring
`frame_c_expr`), a shift-body helper that `dk_copy_range`s the captured
sub-continuation into a `tur_cloneable_cont`, and
`dk_prompt`/`dk_frame`/`dk_shift`/`dk_run`/`dk_free` -- reusing the DK runtime
byte-for-byte. Oracle: `cps-oracle-cloneable-native-shape2` (15/110); verified
across all four operators and both hole sides.

**Step 5 (Shape 2) extended -- multi-frame, captured operands, `let`- and
`if`-bearing contexts LANDED.** `CT_CLONEABLE` now carries a `CloneFrame[]`
array (0 frames = Shape 1, N = an N-deep arithmetic context, outermost-first),
an optional `CloneLet[]` prelude, and an optional single `if` branch point
(`if_cond` / `if_pure` / `if_when`). `build_cloneable` (`cps_ir.c`) walks the
context spine in one loop, mirroring the direct emitter's `collect_ctx` /
`reaches_shift_kind` / `ctx_if_branch` logic *inline* (no `clone_spine` needed:
descending the shift-bearing arm past a recorded `if` naturally produces one flat
frame chain). Coverage:

- **Multi-frame nested contexts** -- `(* 2 (+ 10 (cloneable-shift ...)))` etc.,
  all four operators, both hole sides. Oracle `-nested`.
- **Captured (var) operands** -- a frame operand naming a param/local rides the
  frame env. Oracle `-var`.
- **`let`-bearing** -- pure scalar `let` bindings in the spine are direct-emitted
  as C locals at the reset site ahead of the frame operands (which may reference
  them); the init may itself capture (`(* base 2)`). Oracle `-let`.
- **`if`-bearing** -- one pure-conditioned `if` branch point; the shift-bearing
  arm rides the frame chain, the pure arm is direct-emitted on the other branch
  (`if (cond) { <chain> } else { <pure> }`); the condition may capture. Oracle
  `-if`. `let` and `if` are kept mutually exclusive in one native lowering (a
  `let` above an `if` would be referenced by the pure arm but declared only in the
  shift branch); the mix falls through to the still-correct delegation.

The capture walkers (`collect_caps_rec` / `has_capture_rec`) surface the free
vars of the direct-emitted sub-exprs (each `let` init, the `if` cond/pure arm)
via `collect_free_vars` -- the node's own `let` bindings excluded -- so a
cloneable node inside a lifted continuation captures correctly (the same complete
analysis `CT_LETRAW` uses).

Remaining in step 5: closure/colored receivers, and non-arithmetic `let` shapes
(the `do`-prelude and call-frame variants the direct `collect_ctx` also handles).
Steps 6-7 (multi-shot classification axis + retire delegation) follow.

1. **CT nodes.** Add `CT_CLONEABLE_SHIFT` (receiver + captured-cont, distinct
   from abortive `CT_SHIFT` -- the receiver takes a *handle*, not the value) and
   either reuse `CT_RESET` with a `cloneable` flag or add `CT_CLONEABLE_RESET`.
2. **Translation (`cps_ir.c`).** `EX_CLONEABLE_RESET`/`EX_CLONEABLE_SHIFT` ->
   the new nodes, reusing `collect_free_vars` for the receiver/body captures
   (now EX_CALLCC-complete; add the cloneable forms too). Gate: translate only
   Shape 1 natively at first; fall through to the `CT_LETRAW` delegation (still
   in place) for Shape 2.
3. **Emit (`emit_cps_ir.c`).** `emit_cloneable_shift` for Shape 1: emit the
   identity `__cont_fn`, `tur_cloneable_cont_alloc(..., NULL, NULL, NULL)`, the
   receiver call, deliver the result. Reuse the existing helper/`pending_handler_fns`
   flush (already fixed in the first slice).
4. **Admission + oracle.** Admit Shape 1 in `term_core_ok`; the U0 oracles
   (`cps-oracle-cloneable-basic`, `-multi-resume`) plus `-cloneable-mixed`
   assert direct == cps. Verify multi-resume specifically.
5. **Shape 2 (the risk), incrementally.** Port `emit_cloneable_ctx`'s frame-fn +
   `dk_copy_range` emission to CT-IR, one context form at a time
   (`+`/operator context, then `if`, then `let`), each behind its own oracle and
   with the delegation fallback catching anything not yet native. Port the
   clone/drop glue **byte-for-byte first** (same C), changing only *who emits the
   calls* -- the mitigation the parent plan calls for.
6. **Multi-shot classification axis.** Once native, extend `ensure_S` so a
   multi-shot (cloneable) continuation is placed on DK-with-clone and its
   receiver/handler stays co-located, distinct from the single-shot abortive
   `shift` axis (parent plan section 5).
7. **Retire delegation.** When every cloneable shape emits natively, remove the
   `EX_CLONEABLE_RESET` `CT_LETRAW` delegation; `emit_cps_cloneable_reset` +
   `emit_cloneable_ctx` become dead and move out of `emit_cps.c` (U6/U7).

## Why staged this way

The delegation fallback stays the safety net throughout: a shape not yet ported
natively keeps lowering through the proven direct emit, so the tree is always
shippable and `direct == cps` holds at every step. Shape 1 is a safe foothold
(no deep-clone); Shape 2 is where the care goes, gated per context form.
