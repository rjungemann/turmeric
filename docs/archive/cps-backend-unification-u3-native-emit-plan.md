---
title: "U3 core -- native CT-IR emission of cloneable (multi-shot) continuations"
status: landed
parent: cps-backend-unification-plan.md
description: The remaining core of U3 -- teaching the CT-IR backend to EMIT the cloneable multi-shot machinery itself (rather than delegating the whole cloneable-reset region to the direct emitter). Maps the two emit shapes, where the dk_copy_range multi-shot risk lives, and a staged path that keeps each step suite-green behind the delegation fallback.
---

# U3 core -- native cloneable emission

## Status (2026-07-12, v0.28.2): LANDED, and later superseded past its boundary

U3's native port landed at the boundary described below (native owns the
value-typed bare-fn-receiver subset; closure/colored receivers delegate). **Two
of this note's forward-looking conclusions were later overtaken and are no longer
accurate:**

- The "Steps 6-7" conclusion that *"the `CT_LETRAW` cloneable delegation stays"*
  and closure receivers are its principled permanent home was superseded by the
  post-graduation removal work: **native closure receivers landed** (Shape 1 + Shape 2,
  `receiver_expr` on `CT_CLONEABLE`; see the
  [u7-readiness note](cps-backend-unification-u7-readiness-plan.md)), and the
  cloneable/serial `CT_LETRAW` carve-out was **removed in phase D4** (see
  [direct-lowering-removal](cps-backend-direct-lowering-removal-plan.md)). A
  cloneable shape outside the native subset now evicts the whole function or emits
  `TUR-E0710`, rather than routing a sub-region to `emit_cps.c` -- which no longer
  exists.

The staged-port narrative below is retained as U3's historical record.

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

**Receivers -- investigated; nothing further to natively port.** The receiver of
a `(cloneable-shift receiver val)` is called through the indirect fn-pointer form
`((int64_t(*)(int64_t))(intptr_t)recv)(cont)`. Empirically, across both backends:

- **Named top-level fn receiver** (`k-id`) -- native (the original slice).
- **Capture-free lambda receiver** (`(fn [k] k)`) -- **already native**: the
  lambda captures nothing, so it is lifted to a top-level fn and the shift calls
  it through the same indirect form. Oracle `cps-oracle-cloneable-native-lambda-recv`.
- **Fat-closure receiver** (a lambda capturing a scalar). Two sub-cases:
  * *Uses the continuation* (`(fn [k] (+ k bump))`, Shape 1 or Shape 2) --
    **crashes the direct emitter too** (`Aborted` / type-error). Out of native
    scope; the CT-IR gate rejects it (`closure_fn_binding`) and it stays on the
    delegation path, which fails identically -- `direct == cps` preserved.
  * *Ignores the continuation* (`(fn [k] (+ n 5))`, oracle
    `cps-oracle-cloneable-nested-op`) -- direct handles it correctly via its
    closure lowering (env struct + thunk); native delegates. Porting it would
    duplicate emit_cps.c's closure machinery for a corner case (see the Steps 6-7
    assessment below), so it stays on the delegation by design.
- **Colored receiver** (a receiver fn that itself shifts) -- **unsupported by the
  direct emitter itself** (it calls the receiver without threading a continuation
  -> segfault). Out of native scope for the same reason; the gate rejects it
  (`callee_colored`).
- **Local-var / indirect receiver** (`(let [r k-id] ... (cloneable-shift r 0))`)
  -- blocked *upstream* of the receiver gate: the fn-valued `let` binding makes
  the CT-IR translation revert the whole function to direct emission (the function
  never reaches the colored CT-IR path), so relaxing the receiver gate alone has
  no effect. A separate future item (fn-valued `let` bindings in CT-IR), not a
  cloneable-receiver concern.

Net: the receiver shapes the direct backend supports already emit natively; the
shapes it does not are correctly delegated. No receiver change is warranted.

**Step 5 (Shape 2) extended -- 1-arg call frames LANDED.** `CloneFrame` gains a
`call_fn` alternative (mutually exclusive with the arithmetic `op`): a context
frame `(f [])` where `f` is a top-level uncolored `int -> int` fn and the hole is
its sole argument (no captured env).  `build_cloneable` admits it in the same
spine walk; `emit_cloneable` emits the frame fn as `return (intptr_t)f((int64_t)
value)` and pushes it with a 0 env.  Call frames nest with arithmetic frames
(`(+ 1 (dbl []))`, `(dbl (dbl []))`) in one chain.  Oracle
`cps-oracle-cloneable-native-shape2-callframe`.

Deliberately *not* native: **2-arg call frames** (`(f env [])`).  The direct
emitter drops a 2-arg call context onto the legacy identity path (it does not
reify the frame -- the resumed value is returned unchanged), so making it native
and correct would make `direct != cps`.  It stays on the delegation path to match
the direct backend's behavior byte-for-byte.

**Step 5 (Shape 2) extended -- do-prelude contexts LANDED.** `(cloneable-reset
(do PRELUDE... (cloneable-shift receiver v) TAIL...))`:

- **Prelude items** [0, m) are direct-emitted for side effect at the reset site
  (recorded as binding-less `CloneLet`s, emitted as `(void)(<expr>);`) -- they run
  once at capture time.
- Each **tail item** (m, N) is a 0-arg call to a top-level uncolored int fn,
  reified as an **ignore-value frame** (`CloneFrame.ignore_value`): on resume it
  runs `f()` regardless of the resumed value.  Tail items are recorded in reverse
  so the first runs innermost (first on resume) and the last outermost (its value
  is the reset's).

Restricted to the whole reset body (no outer frames) and no `if`.  Oracle
`cps-oracle-cloneable-native-shape2-doprelude` (prelude runs once; `tick`/`tock`
tails yield 2/2 across a clone + resume).

Deliberately *not* native (kept on delegation, matching the direct backend):
**1-arg ignore-value tails** (`(f env)`) -- the direct emitter crashes on them
(illegal instruction); and any shift with **live captures at its site**
(`n_live_captures != 0`), which the direct emitter's `cl_can_lower` also rejects.
The latter is a *shared-prelude* constraint: native Shape 2 emits references to the
DK runtime helpers (`__dk_cont_fn` / `__dk_env_clone` / `__dk_env_drop`,
`dk_copy_range`) whose definitions are gated by `cl_can_lower`; `build_cloneable`
now checks `n_live_captures == 0` so it never lowers a shape whose prelude the
gate would omit (which would leave undeclared C names).

**Step 5 native port is complete for the value-typed subset.** Native CT-IR now
owns every cloneable shape whose receiver is a *bare fn pointer* (named top-level
fn or capture-free lambda) and whose context is built from arithmetic frames (any
depth), 1-arg call frames, `let` preludes, one `if` branch point, or a do-prelude
with 0-arg ignore-value tails -- all with `n_live_captures == 0`. Everything else
stays on the `CT_LETRAW` delegation.

## Steps 6-7 -- assessment (why the delegation stays, and what "retire" means)

Steps 6-7 as originally written -- "add the multi-shot classification axis, then
delete `emit_cps_cloneable_reset`" -- were premised on *every* cloneable shape
eventually emitting natively. That premise does not hold, and the investigation
below re-scopes both steps.

### What the delegation still covers (and why it should)

Auditing every direct-supported cloneable shape against native emission (via the
`cps-oracle-cloneable-*` twins) shows the native path already owns all of them
**except closure/complex receivers**, which fall into two buckets:

- **Closure receivers that USE the continuation** (`(fn [k] (+ k bump))`, in
  Shape 1 *or* Shape 2) -- these **crash the direct emitter too** (`Aborted` /
  type-error): a genuinely unsupported shape on both backends. The delegation is
  *bug-compatible* here (native and direct fail identically), preserving
  `direct == cps`.
- **Closure receivers that IGNORE the continuation** (`(fn [k] (+ n 5))`, oracle
  `cps-oracle-cloneable-nested-op`) -- direct handles these correctly *via its
  closure lowering* (env struct + statically-known thunk), and native delegates.
  Porting this natively means the CT-IR backend allocating the receiver's closure
  env and calling its thunk -- i.e. **re-implementing emit_cps.c's closure
  machinery inside emit_cps_ir.c**. That duplicates the code we want to retire
  rather than eliminating it, and buys only a corner case (a receiver that
  discards its own continuation).

So the delegation is not merely a transitional scaffold -- for closure-receiver
cloneable it is the *principled* home, reusing the proven direct closure lowering.
Full deletion of `emit_cps_cloneable_reset` is therefore **not** the U3 end-state.

### Step 6 -- multi-shot classification axis: satisfied by construction

No `ensure_S` change is needed for cloneable, for three reasons:

- **No eviction to invert.** Unlike serial/async (which the classifier evicts from
  CT-IR ownership, the case the parent plan's "invert eviction into placement"
  targets), a cloneable-reset is **never evicted**: its enclosing function stays
  colored and the cloneable *region* delegates via `CT_LETRAW`. There is no
  cloneable placement rule to add.
- **Multi-shot capture is safe by restriction.** Native cloneable admits only
  scalar (int) frame operands / captures, which are Copy and thus multi-shot-safe
  with no clone/drop; the sub-continuation itself is deep-cloned by `dk_copy_range`
  in the emitted body. The single-shot-vs-multi-shot axis (`g_cap_single_shot`,
  which gates owning-value move-capture) already exists and is never tripped by
  the native cloneable subset.
- **The prelude gate is the one real coupling, and it is pinned.** Native Shape 2
  references the shared DK helpers whose definitions `cl_can_lower` gates; the
  `n_live_captures == 0` check in `build_cloneable` keeps the native gate a subset
  of `cl_can_lower`, so the prelude is always present. This is the invariant to
  preserve as new shapes are considered.

A fuller multi-shot axis (owning-value captures on DK-with-clone) is real work,
but it belongs to **U4/serial + U5/async**, where eviction *does* happen and the
clone/drop glue is actually exercised -- not to cloneable, whose subset is scalar.

### Step 7 -- retire delegation: re-scoped, and gated on U6/U7

The `CT_LETRAW` cloneable delegation stays. It is the correct fallback for
closure-receiver shapes and the bug-compatible fallback for shapes broken on both
backends. `emit_cps_cloneable_reset` / `emit_cloneable_ctx` can only be deleted
as part of the coordinated `emit_cps.c` retirement (U7), and only once either (a)
closure lowering is unified so the CT-IR backend can allocate a receiver closure
env itself, or (b) the genuinely-unsupported shapes are turned into a clean
compile-time diagnostic instead of bug-compatible codegen. Both are larger,
cross-cutting changes -- not bounded slices -- and are tracked under U6/U7, not
U3. U3's native port is done at its stable boundary: **native owns the value-typed
subset; the delegation owns the closure/complex subset.**

## Why staged this way

The delegation fallback stays the safety net throughout: a shape not owned by the
native path keeps lowering through the proven direct emit, so the tree is always
shippable and `direct == cps` holds. Shape 1 was the safe foothold (no
deep-clone); Shape 2 was gated per context form; and the boundary between native
and delegation now falls exactly where the direct backend's own support ends.
