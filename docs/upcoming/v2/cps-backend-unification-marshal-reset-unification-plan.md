---
title: "Unify build_cloneable / build_serial into one marshal-reset spine walk"
status: proposed
parent: cps-backend-unification-plan.md
description: The U3 (cloneable, multi-shot) and U4 (serial, marshalable) native CT-IR ports grew two near-identical context-spine walks (build_cloneable / build_serial) and share a single emit path (emit_cloneable, branched on a `serial` flag). This note plans folding the two build walks into one parameterized `build_marshal_reset`, so the remaining serial shapes (do-prelude, 2-arg call frames, Shape 1) and any future context shape are added once for both families.
---

# Unify the cloneable / serial native spine walk

## Status (2026-07-12, v0.28.2): STILL OPEN -- the last CPS-unification follow-up

Verified against the tree: `build_cloneable` (`src/passes/cps_ir.c:401`) and
`build_serial` (`:753`) remain two separate spine walks; **`build_marshal_reset`
does not exist**. This is a pure code-health refactor and the one remaining open
item from the broader CPS-backend unification (which is otherwise landed --
`emit_cps.c` is deleted; see the
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
