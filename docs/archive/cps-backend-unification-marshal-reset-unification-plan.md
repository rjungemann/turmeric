---
title: "Unify build_cloneable / build_serial into one marshal-reset spine walk"
status: proposed
parent: cps-backend-unification-plan.md
description: The U3 (cloneable, multi-shot) and U4 (serial, marshalable) native CT-IR ports grew two near-identical context-spine walks (build_cloneable / build_serial) and share a single emit path (emit_cloneable, branched on a `serial` flag). This note plans folding the two build walks into one parameterized `build_marshal_reset`, so the remaining serial shapes (do-prelude, 2-arg call frames, Shape 1) and any future context shape are added once for both families.
---

# Unify the cloneable / serial native spine walk

## Status (2026-07-19): COMPLETE -- verified in the tree; ready to archive

Re-verified against the source as of 2026-07-19: `build_marshal_reset(b, e, x,
rest, serial)` is the single spine walk in `src/passes/cps_ir.c`, with
`build_cloneable` / `build_serial` as one-line `serial=false` / `serial=true`
wrappers pointing at it; `ctx_reaches_shift(e, shift_kind)` and
`marshal_named_receiver(b, shift, serial)` are the unified reach test / receiver
finder. `emit_cps.c` is deleted. All four serial oracle fixtures exist
(`cps-oracle-serial-native-doprelude`, `-callframe-2arg`, `-shape1`,
`-closure-recv`). No tracked item remains open -- the build unification, the
emit-side shape additions, and the closure-receiver parity slice all landed.

## Status (2026-07-12, v0.28.2): LANDED -- build walk unified

The build-side unification (staging steps 1-4 below) is done:
`build_marshal_reset(b, e, x, rest, serial)` (`src/passes/cps_ir.c`) is now the
single spine walk; `build_cloneable` / `build_serial` are one-line wrappers that
call it with `serial=false` / `serial=true`. The two reach tests collapsed into
`ctx_reaches_shift(e, shift_kind)` and the two receiver finders into
`marshal_named_receiver(b, shift, serial)`. The cloneable-only `n_live_captures`
Shape-2 gate is guarded with `!serial`; the per-family carve-outs that genuinely
differ (the serial 2-arg call frame's Serializable-ADT env + hole-param-only
gate, the serial do-tail's 1-arg captured-config frame) branch on `serial`
inline. The node is unchanged (`CT_CLONEABLE` with the `serial` flag), so the
emit path, binder decls, capture walkers, and the ok/join predicates were
untouched.

Verified: every `*cloneable*` / `*serial*` fixture's `emit-c` output is
byte-identical before/after, and the full suite is green (2111 passed, 0
failed). Each new context shape now gets written once for both families.

The *emit*-side shape additions in "Interaction with the remaining U4 shapes"
below (serial do-prelude, 2-arg call frames, Shape 1 identity) are **already
landed and native** in the tree, each with a passing oracle fixture
(`cps-oracle-serial-native-doprelude`, `cps-oracle-serial-callframe-2arg`,
`cps-oracle-serial-shape1`) -- confirmed no delegation markers, only the native
`_skreg` / `_skcall` / `dk_shift` machinery. The unification made them
single-write on the build side; their emit work was done independently.

### Follow-up slice (2026-07-12): serial closure-receiver parity + warning-clean emit

Probing the unification's promise ("serial gains any context shape cloneable
already had") surfaced that the U7 **closure receiver** path had no serial oracle
coverage and that `emit_cl_shift_bodyfn` (`src/compiler/emit_cps_ir.c`) -- the
shared Shape-2 closure-receiver body emitter for both families -- cast the baked-in
closure thunk's arguments with a blanket `(int64_t)`, ignoring the thunk's real
param types. That produced `-Wint-conversion` warnings (harmless only because
int64_t and pointers are same-width): the closure env param is always `void *`,
and the continuation param `k` is `void *` for a serial `ptr<void>` receiver vs
the `int64_t` carrier for a cloneable `:cont` receiver (both share kind
`TY_PTR_VOID`, so the family flag -- not the param kind -- is the reliable signal).
The body emit now casts env to `void *` and `k` to `int64_t` / `void *` per the
`serial` flag, so the generated closure-receiver call is warning-clean for both
families (cloneable-closure-shape2 went 7 warnings -> 0). Added
`cps-oracle-serial-closure-recv` (Shape 1 identity + Shape 2 arithmetic/call
frames, capturing + non-capturing receivers, round-tripped through
`save-cont!`/`resume-cont!`). Full suite green (2112 passed, 0 failed).

This was the last open item from the broader CPS-backend unification (otherwise
landed -- `emit_cps.c` is deleted; see the
[umbrella plan](../../archive/cps-backend-unification-plan.md)).

Two references below are now stale in their surrounding context (the refactor
itself is unaffected):

- The "Interaction with the remaining U4 shapes" and "Non-goals" sections speak of
  the `CT_LETRAW` cloneable/serial delegation as the fallback. That carve-out was
  **removed in phase D4** ([direct-lowering-removal](../../archive/cps-backend-direct-lowering-removal-plan.md)):
  a cloneable/serial shape outside the native subset now evicts the whole function
  or emits `TUR-E0710`/`TUR-E0706`, and closure receivers went native. The
  duplication this note targets is still real; only the "what the delegation
  covers" framing changed.

## Why

The native CT-IR emission of `cloneable-reset` (U3, multi-shot) and `serial-reset`
(U4, marshalable) are structurally the same delimited region:

```
(<family>-reset  <context>  (<family>-shift receiver v))
```

where `<context>` is a chain of arithmetic / call frames, optional `let` prelude,
and an optional `if` branch point, bottoming out in a named receiver that gets the
captured sub-continuation. The two lowerings differ only in **how the frames are
reified and how the receiver is handed the continuation**:

| | cloneable (multi-shot) | serial (marshalable) |
|---|---|---|
| arithmetic frame | per-site `_ccctx` fn (`cloneable_frame_expr`) | shared `__sk_frame_for_tag(tag)` |
| call frame | per-site `_ccctx` wrapper | per-site `_skcall` wrapper + `SkReg` registration |
| shift body | `cloneable_cont_alloc(__dk_cont_fn, __cap, clone, drop)` | hands the receiver the copied DK chain `__cap` directly |
| capture safety | `dk_copy_range` deep-clone | `dk_copy_range` + name-keyed marshal round-trip |

Everything *else* -- the context recognition, the frame/let/if bookkeeping, the
capture analysis, the binder threading -- is identical. Today that identity is
expressed as **duplication**:

- `build_cloneable` and `build_serial` (`src/passes/cps_ir.c`) are two spine
  walks with the same arithmetic / call / `let` / `if` branches; the only
  differences are the shift kind (`EX_CLONEABLE_SHIFT` vs `EX_SERIAL_SHIFT`), the
  reaches-shift helper (`cloneable_ctx_reaches_shift` vs `serial_reaches_shift`),
  the receiver finder (`cloneable_named_receiver` vs `serial_named_receiver`), and
  a cloneable-only `n_live_captures == 0` gate.
- The emit side is *already* unified: `emit_cloneable` (`src/compiler/
  emit_cps_ir.c`) branches on `t->as.cloneable.serial`, and the `let`-prelude loop,
  `if`-branch wrapper, binder decls, and capture walkers already operate on the
  shared `CT_CLONEABLE` node fields regardless of the flag.

So the build side is the last place the two families diverge for no semantic
reason. Every new context shape (do-prelude has already been added to cloneable;
2-arg call frames, Shape 1, and future forms are pending) currently has to be
written twice. Unifying the walk means writing each shape once.

## What to unify

Fold `build_cloneable` and `build_serial` into a single:

```c
static CTerm *build_marshal_reset(CpsB *b, Expr *e, CVar x, CTerm *rest,
                                  bool serial);
```

parameterized by `serial`, which selects:

1. **Reset-body accessor** -- `serial ? e->as.serial_reset_.body
   : e->as.cloneable_reset_.body`.
2. **Shift kind** -- `serial ? EX_SERIAL_SHIFT : EX_CLONEABLE_SHIFT`. Thread this
   into the reach test (see 4).
3. **Receiver finder** -- both `*_named_receiver` bodies are identical except the
   `k_fn` accessor (`cloneable_shift_.k_fn` vs `serial_shift_.k_fn`); collapse into
   one that switches on the shift kind.
4. **Reach test** -- generalize `cloneable_ctx_reaches_shift` /
   `serial_reaches_shift` into one `ctx_reaches_shift(e, shift_kind)` (they are
   byte-identical apart from the leaf kind). This is the one helper whose signature
   change touches multiple call sites inside the walk.
5. **The `n_live_captures == 0` gate** -- cloneable-only (it pins the native subset
   under `cl_can_lower`'s shared-DK-prelude gate; serial's prelude is presence-gated
   so it needs no such check). Guard it with `if (!serial && ...)`.

Then:

```c
static CTerm *build_cloneable(CpsB *b, Expr *e, CVar x, CTerm *rest)
    { return build_marshal_reset(b, e, x, rest, /*serial=*/false); }
static CTerm *build_serial(CpsB *b, Expr *e, CVar x, CTerm *rest)
    { return build_marshal_reset(b, e, x, rest, /*serial=*/true); }
```

and delete the duplicated second walk. The `EX_CLONEABLE_RESET` /
`EX_SERIAL_RESET` cases in `cps_tail` / `cps_bind` are unchanged (they already call
the thin wrappers).

The node stays `CT_CLONEABLE` with the `serial` flag (no new node), so the emit
path, binder decls, capture walkers, `term_core_ok`, `needs_heap_join`, and
`joins_closed_rec` are all untouched -- they already handle both families.

## Staging (keep the tree green at each step)

The risk is entirely on the cloneable path: it is the more-covered of the two
(more oracles, the `n_live_captures` and do-prelude subtleties), and a bad
extraction regresses it silently. Stage so the cloneable behavior is provably
unchanged before serial gains anything new:

1. **Generalize the reach test** to `ctx_reaches_shift(e, shift_kind)` and update
   both families' call sites. No behavior change; full suite stays green
   (the `cps-oracle-cloneable-*` and `serial-*` twins are the net).
2. **Collapse the receiver finders** into one shift-kind-switched helper. No
   behavior change.
3. **Extract `build_marshal_reset`** from the current `build_cloneable` body,
   adding the `serial` parameter and the two guards (reset-body accessor, the
   `!serial` around the `n_live_captures` gate). Point `build_cloneable` at it as
   a `serial=false` wrapper. **This step must leave every cloneable oracle
   byte-identical** -- diff the emitted C for a representative cloneable fixture
   before/after to confirm.
4. **Repoint `build_serial`** at `build_marshal_reset(..., serial=true)` and delete
   the old serial walk. Serial immediately gains any context shape cloneable
   already had (e.g. do-prelude, once its emit lands -- see below) with no
   further build work.

Each step is independently revertable and suite-gated.

## Interaction with the remaining U4 shapes

Some serial shapes need an **emit** addition regardless of the build unification;
the unification only removes the *build* duplication:

- **do-prelude serial** -- the build walk already produces do-prelude nodes once
  unified, but `emit_cloneable`'s serial branch must emit the ignore-value tail as
  a `_skcall` wrapper `return f()` registered under the `$0` side (not the `$L`
  hole side). Small, localized emit change.
- **2-arg call frames** -- need a serialized env operand: the `SkReg` `env_kind`
  code + `Serializable` ser/deser fn pointers (the `env_ser` / `env_deser` the
  direct emitter threads). This is the one shape with real new machinery on both
  build (collect the env operand + its Serializable instance) and emit (emit the
  `SkReg` with the codec). Worth its own slice.
- **Shape 1 (identity) serial** -- `(serial-reset (serial-shift rt v))`. The emit
  needs a serial identity path (hand the receiver an empty/identity DK chain);
  distinct from the cloneable Shape 1 (`cloneable_cont_alloc(id, NULL, NULL,
  NULL)`), so it does not come for free from the unification.

Sequence: land the unification (steps 1-4 above) first so the do-prelude and
2-arg shapes are single-write, then add each shape's emit once.

## Non-goals

- Not merging cloneable and serial into one *node* or one *emit function beyond
  the existing flag* -- the emit genuinely differs (tagged marshaler + registry vs
  per-site clone fns) and the `serial` branch in `emit_cloneable` already isolates
  that cleanly.
- Not touching the `CT_LETRAW` delegation, which remains the fallback for every
  shape neither native path owns (closure/colored receivers, 2-arg-until-ported,
  the broken-on-both cases).
- Not a prerequisite for U5 (async) -- async rides the cloneable/serial runtime,
  which is already native for the value-typed subset; the unification is a code-
  health/velocity change, not a capability one.
