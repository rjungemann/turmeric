# Plan: relocate carrier-encoded and control-flow values during scratch promotion

**Status:** proposal (not started). **Area:** `src/turi/` (tree-walking interpreter).
**Goal:** extend the landed scratch/permanent value-pool promotion so it can also
relocate the value shapes it currently **bails on**, so a long-lived `TuriEnv`
bounds its steady-state memory even when the live global set includes
carrier-encoded collections or suspended control-flow values.

This is the follow-up tail split out of
`docs/archive/turi-value-pool-scratch-promotion-plan.md` (Phase A -- landed). That
work delivered the two-region split (`value_scratch` + `value_perm`), the
`arena_reset`/`arena_owns` primitives, poison-on-reset, the opt-in
`turi_env_set_scratch_promotion` API, and a conservative promotion walk that
relocates scalars, strings, closures (+ captured frames/bindings/tyvars), and
structs (+ fields). Everything here builds on that; nothing here changes the
default (promotion-off) path or the value-lifetime contract.

## Background -- what Phase A bails on

Phase A's promotion is conservative *by construction*: at each top-level eval it
proves the whole root set (result + every global) is relocatable, and if any
escaping value reaches a shape it cannot safely copy, it **declines to rewind**
that cycle (keeps scratch intact) rather than risk a dangling pointer. A missed
shape means "this eval does not shrink", never "use-after-reset".

The shapes that trigger the bail -- and that this plan makes relocatable -- are:

1. **Carrier-encoded pointer values.** The by-value HKT representation stores
   cons/set/vec/ADT payload pointers as a bare `int64` (`TURI_INT` whose value is
   a scratch address; see the `int64_t *` cons cells / set backing / ADT field
   memory allocated via `turi_val_alloc` in `eval.c`). A `TuriValue` walk cannot
   tell such an int from a plain integer, so Phase A refuses to rewind whenever a
   root reaches a `TURI_INT` that addresses the scratch region.
2. **Live control-flow C-state.** `TuriCont` / `TuriWsCont` / `TuriEffectCont` /
   `TuriGen` / `TuriFiber` / `TuriTVar` / `TuriHandlerVal` carry
   interpreter-internal graphs (saved `EvalFrame`s, `TsFrame` / `DriveCont`
   arrays, handler cases) rather than plain `TuriValue` payloads, plus -- for
   generators/fibers -- an mmap/malloc coroutine stack tracked separately
   (`TuriCoroStack`). Phase A bails whenever a root reaches one of these resident
   in scratch, and the env-quiescence guard already bails on any *pending* async
   or handler state.

## Goals

1. A long-lived `TuriEnv` that keeps carrier-encoded collections (cons lists,
   sets, vecs, ADT values) live in globals reaches steady-state memory
   proportional to live state, not cumulative eval history -- the same bound
   Phase A gives for scalar/struct/closure state.
2. A suspended, still-reachable control-flow value (e.g. a generator paused and
   stored in a global) survives a scratch rewind and resumes correctly.
3. No change to the default path, the opt-in gate, or the value-lifetime
   contract. Every increment keeps `tests/run.sh`, the `tur_env_teardown` leak
   gate, and `tests/run-turi.sh` at baseline.

## Non-goals

- Reference counting or a tracing GC (still the larger ownership-model change;
  see the archived Phase-1 plan's Alternatives).
- The residual coroutine-stack / inline-C-buffer tails in
  `docs/reported/turi-value-pool-residual-sites.md` -- orthogonal.
- Relocating the coroutine *stacks* themselves: they back a `ucontext_t` and must
  keep stable addresses. Only the interpreter structs move; the stacks stay put
  (they are already tracked on the env and reclaimed at `turi_env_free`).

## Design

### Part 1 -- type-directed carrier relocation

The blocker is provenance: which `int64` roots are pointers, and what layout do
they point to. The elaborator already knows -- the static type of the eval result
is available at the top-level boundary (`extract_type_tag` reads
`last_expr->type`), and each global binding has an elaborated type. Thread that
type into promotion so the walk becomes **type-directed** for carrier values:

- At the boundary, pair each root `TuriValue` with its elaborated `Type` (result
  type for `last`; the binding's declared/inferred type for each global).
- Add a `promo_copy_carrier(env, int64 bits, Type carrier_ty, fwd)` that, guided
  by `carrier_ty`, walks the concrete layout: `Cons A` -> head (as `A`) + tail
  (as `Cons A`); `Set A` / `Vec A` -> element backing; a record/ADT carrier ->
  its `int64` field array reinterpreted per the constructor's field types.
  Recurse through `promo_copy` for `TuriValue`-typed sub-fields and back into
  `promo_copy_carrier` for nested carriers, sharing the same forwarding map so
  cycles and sharing copy once.
- Only a `TURI_INT` whose type is a known carrier and whose value addresses
  scratch is relocated; a genuinely integer `int64` (type `int`) is left as-is.
  This removes the Phase A "bare-int addresses scratch -> bail" rule for the
  carriers we have a layout for; unknown/opaque carrier types keep bailing.

The main cost is threading `Type` to the promotion roots and enumerating the
carrier layouts. Land one carrier family at a time (cons, then set/vec, then ADT
records), each behind the existing opt-in + poison, with a matching
`env-longlived` fixture.

### Part 2 -- relocate suspended control-flow structs

For each control-flow struct, add a copy routine that mirrors its allocation site
and deep-copies its reachable graph into `value_perm` via the shared forwarding
map:

- `TuriGen`: copy the struct + its captured `EvalFrame` graph and pending state;
  leave the tracked coroutine stack in place (stable address), rebinding the
  struct's pointer to it.
- `TuriCont` / `TuriWsCont` / `TuriEffectCont`: copy the `TsFrame` / `DriveCont`
  arrays and the saved `EvalFrame`s they reference (reusing `promo_copy_frame`).
- `TuriHandlerVal`: copy the `HandleCase` array and captured frame.
- `TuriTVar`: a plain `{value, version}` box -- copy directly.

Scope this to values that are **reachable from a root and suspended** (the
env-quiescence guard still bails on *in-flight* control state, so we never move a
struct the C stack is mid-way through). Land per struct kind, each with a fixture
that stores the value in a global, churns scratch, then resumes/reads it.

### "Is this pointer scratch?" stays the provenance primitive

`arena_owns(&env->value_scratch, p)` remains the copy/leave decision for every
pointer the type-directed walk dereferences, exactly as in Phase A.

## API / behavior changes

- Internal: promotion roots gain an associated `Type`; new
  `promo_copy_carrier` and per-control-flow-struct copy routines in `eval.c`.
  Possibly a small `Type`-carrying variant of the root iteration.
- No public API change: `turi_env_set_scratch_promotion` still gates it;
  `turi_env_*` / `turi_eval_*` signatures and the value contract are unchanged.
- Each landed increment narrows the set of shapes that trigger the conservative
  bail; a still-unhandled shape continues to bail (no rewind), never corrupts.

## Testing

- Extend `tests/turi/env-longlived.c` (`tur_env_longlived`) per increment:
  - Part 1: a global cons list / set / vec / ADT value built from large
    transients, churned across many evals -- assert scratch is now rewound
    (`value_scratch.total_bytes == 0`), perm reaches a fixed point, and the
    collection still reads back correctly (length, membership, field access)
    after 100+ rewinds under the poison.
  - Part 2: a generator (then a saved continuation) stored in a global, drained
    across evals that each churn scratch -- assert correct resumption values.
- Keep the Phase A conservative-bail fixtures for the shapes not yet handled
  (they must still bail, not corrupt).
- `tests/run.sh` (10-min timeout), `tur_env_teardown` leak gate, `run-turi.sh`.

## Risks

- **Carrier layout drift.** `promo_copy_carrier` encodes the concrete carrier
  layouts; a change to the by-value HKT representation must update it in lockstep.
  Mitigate by deriving the layout from the same constructor/type metadata the
  interpreter builds the carrier from, not a hand-copied constant.
- **Incomplete control-flow copy -> use-after-reset.** Same sharpest edge as
  Phase A; the poison-on-reset debug mode is the safety net, and per-shape
  incremental landing keeps each step small and fixture-covered.
- **Type availability.** A root whose elaborated type is unavailable or erased
  (e.g. `ptr<void>` carriers) cannot be type-directed -- those keep bailing.
- **Promotion cost.** Type-directed carrier copying adds walk overhead
  proportional to live collection size each eval; acceptable for the kernel
  pattern (bounded steady state beats unbounded growth), but measure.

## Recommendation

Land **only if** a single-immortal-env host needs the bound for carrier-heavy or
control-flow-heavy live state -- Phase A already bounds scalar/struct/closure
state and safely (conservatively) tolerates the rest. If undertaken, land Part 1
(carriers) before Part 2 (control flow), one shape family at a time, behind the
existing opt-in + poison, against per-shape `env-longlived` fixtures.
