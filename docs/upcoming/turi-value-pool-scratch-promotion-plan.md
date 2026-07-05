# Plan: scratch/permanent value-pool regions for a long-lived turi env

**Status:** Phase A LANDED (opt-in, conservative promotion); full carrier-pointer
/ control-flow relocation remains future work. **Area:** `src/turi/`
(tree-walking interpreter).

## Phase A -- landed

The foundation and a conservative, correctness-first slice of the promotion walk
shipped, gated strictly opt-in and OFF by default so the entire existing
per-unit-env path is byte-for-byte unchanged:

- **`arena_reset` + `arena_owns` primitives** (`runtime/arena.{c,h}`).
  `arena_reset` rewinds every slab to empty in O(slabs) and, in a Debug build,
  poisons the reclaimed bytes (0xDE) first -- the plan's "poison-on-reset debug
  mode", so any straggler pointer into the rewound region crashes loudly under
  ASan. `arena_owns` is the scratch/not-scratch provenance check the walk needs.
- **Two-region split** on `TuriEnv`: the single `value_arena` became
  `value_scratch` (default target of every `turi_val_*` allocation) +
  `value_perm` (receives promoted escapees). With promotion off, `value_scratch`
  is never rewound and is a transparent rename of the old pool.
  `turi_val_perm_{alloc,calloc,strdup}` allocate into perm.
- **Escape promotion** (`turi_promote_escaping` in `eval.c`), run at each
  `turi_eval` top-level boundary when enabled. Two passes over the root set (the
  result + every global): a read-only promotability check, then a Cheney-style
  deep copy into perm using an out-of-band forwarding map (so cycles/sharing --
  cyclic `letrec` closures via captured frames -- copy once and a mid-walk bail
  never stamps scratch objects). Then `arena_reset(value_scratch)`.
- **Opt-in API**: `turi_env_set_scratch_promotion(env, bool)` (off by default).
- **Conservative by construction.** The walk relocates only the shapes it can
  prove safe: scalars, strings, closures (+ captured frames/bindings/tyvars),
  structs (+ fields). When an escaping value reaches something it cannot safely
  relocate -- a **carrier-encoded pointer boxed as a bare int** (cons/set/vec/ADT
  carriers), a live continuation/generator/handler/future/throw, a scratch-
  resident mutable ref or opaque native `ud`, or any pending async/control state
  on the env -- that eval declines to rewind (keeps scratch intact) rather than
  risk a dangling pointer. A missed shape therefore means "this eval does not
  shrink", never "use-after-reset". This is the safe base the plan's
  "incrementally, one payload shape at a time" recommendation asks for.
- **Tests**: `tests/turi/env-longlived.c` (`tur_env_longlived` ctest) asserts
  (1) steady state -- scratch is rewound each cycle (`total_bytes == 0`) and perm
  reaches a fixed point, versus a promotion-off control env whose scratch grows
  without bound; (2) cross-reset correctness -- structs, ADT values, plain and
  mutually-recursive closures, and nested structs stay valid across 100+ scratch
  rewinds (poison-on-reset would crash a mis-copy); (3) the conservative bail --
  a live generator left as a global blocks the rewind and stays intact.
  `tests/run.sh` 1943/0; the `tur_env_teardown` leak gate stays clean;
  `tests/run-turi.sh` matches baseline.

### Remaining (future increments)

The genuinely hard tail the plan flagged: relocating **carrier-encoded pointer
values** (the by-value HKT representation stores cons/set/vec/ADT payload
pointers as bare `int64`, indistinguishable from integers without per-carrier
type info) and the internal C-state of live continuations/generators/fibers/
tvars. Until those are walkable, a long-lived host that keeps such values live in
globals gets correct behavior but no memory bound for those cycles. Extending the
walk one such shape at a time -- against the same fixtures and poison mode -- is
the path forward.

---

**Original proposal (retained for context) follows.**

**Status:** proposal (not started). **Area:** `src/turi/` (tree-walking interpreter).
**Goal:** bound peak memory **within a single long-lived `TuriEnv`** (a
notebook-kernel-style host that shares one env across thousands of top-level
evals), without changing interpreter semantics or the host-visible value
contract.

This is the optional follow-up to the (landed) env-owned value-arena pool --
see `docs/archive/turi-env-owned-value-arena-pool-plan.md`. It was split out of
that plan's "Phase 2" because it is materially harder and only matters for the
immortal-env host pattern; the per-unit-env pattern is already served.

## Background -- what Phase 1 already delivers

The value-arena pool landed: `TuriEnv` owns an `Arena value_arena`, every
escaping value payload (closures, structs, captured frames/bindings, ADT/set/
cons payloads, control-flow structs, error strings via a global fallback) is
bump-allocated from it, and `turi_env_free` reclaims the lot in one shot. The
`tur_env_teardown` leak gate (`ASAN_OPTIONS=detect_leaks=1`) proves a full
create/eval/free cycle leaks nothing.

What Phase 1 deliberately did **not** do (its stated non-goal): it does not
*shrink* a never-destroyed env. Every value an immortal env ever allocates
stays live in `value_arena` until the (never-occurring) `turi_env_free`. A host
that runs one env across thousands of cells grows without bound -- the per-call
growth regression the Phase-1 doc called out. That is the problem this plan
addresses.

## Goals

1. A long-lived `TuriEnv` shared across N top-level evals holds steady-state
   memory proportional to *live* state (globals + reachable closures), not to
   the cumulative history of all N evals.
2. No change to interpreter semantics, evaluation results, or the host-visible
   value contract ("values valid until `turi_env_free`"). A value the host
   reads out of eval K must stay valid for the host's use even after eval K+1
   resets the scratch region.
3. Keep the Phase-1 per-unit-env teardown path leak-clean and unchanged.

## Non-goals

- Reference counting or a tracing GC -- a larger ownership-model change, still
  out of scope (see Alternatives in the archived Phase-1 plan).
- The two residual Phase-1 tails (inline-C emulator buffers; coroutine
  mmap/malloc stacks) tracked in
  `docs/reported/turi-value-pool-residual-sites.md` -- orthogonal; fix
  independently.

## Design -- two regions + escape promotion

Split `value_arena` into two pools on `TuriEnv`:

- a **permanent** pool for payloads reachable from `globals` / live host
  handles / surviving closures;
- a **scratch** pool, `arena_reset` (rewound, not freed) at each top-level eval
  boundary.

Values are scratch-allocated by default. A value that **escapes** a top-level
eval -- assigned into a global, returned to the host, or captured by a closure
that survives the eval -- is **promoted**: deep-copied into the permanent pool
*before* the scratch region is rewound. After promotion, the scratch pool is
reset and reused by the next eval.

```c
/* env.h, inside TuriEnv (replacing the single value_arena) */
Arena value_perm;     /* survives across top-level evals; freed at env_free */
Arena value_scratch;  /* arena_reset at each top-level eval boundary */
```

`arena_reset` (rewind all slabs to empty, keep the allocation) is a new
`arena.c` primitive alongside `arena_free`; it is what makes scratch reuse O(1).

### Promotion walk

Promotion needs an escape walk over the eval's result value plus any
newly-bound or rebound globals, deep-copying each reachable scratch-allocated
payload into the permanent pool and rewriting the pointers:

- **Roots:** the returned `TuriValue`; every `globals` binding touched this eval
  (track a dirty set during the eval, or diff against a pre-eval snapshot).
- **Reachable payloads:** for each tagged value, recurse into its payload --
  `TuriStruct` fields, closure captured `EvalFrame`/`EvalBinding` chains and
  their `tyvars`, cons/set backing, control-flow structs, etc. (the same payload
  kinds Phase 1 enumerated).
- **Cycle handling is mandatory:** closures are cyclic via `letrec` (a frame
  binding points at a closure that captures that frame). Use a
  forwarding-pointer scheme (Cheney-style): stamp each copied object with its
  new permanent address and follow the forward on revisit.
- **"Is this pointer scratch?"** the copy must distinguish scratch-pool pointers
  (copy + forward) from permanent-pool / `eval_arena` / global-string pointers
  (leave as-is). An address-range check per arena slab, or a one-bit tag, decides.

Only after the walk completes is `value_scratch` reset.

### Why this is hard (and deferred)

- The escape walk must cover **every** payload shape the interpreter can
  produce, including the control-flow machinery (continuations, work-stack
  conts, handler values, generators, fibers, tvars). Missing a shape silently
  corrupts memory after the reset.
- Cyclic closures + shared substructure require correct forwarding, not naive
  deep copy (which would loop or duplicate).
- A value the **host** still holds across an eval boundary must be promoted even
  if it is not reachable from globals -- so the host-handle set is also a root.
  The current API hands raw `TuriValue`s out; there is no handle registry to
  enumerate. Either add one, or conservatively promote every eval result.

## API / behavior changes

- New `arena_reset(Arena*)` primitive in `runtime/arena.c`/`.h`.
- `TuriEnv.value_arena` becomes `value_perm` + `value_scratch`; the `turi_val_*`
  allocators default to scratch, with a `turi_val_perm_*` variant (or an
  internal "promote here" path) for the walk.
- `turi_env_free` frees both regions (no signature change).
- A new internal `turi_promote_escaping(env, result)` invoked at each
  `turi_eval` top-level boundary.
- Unchanged: `TuriValue`, `turi_env_new`, `turi_eval_*` signatures, evaluation
  results, and the value-lifetime contract.

## Testing

- Extend the teardown harness (or add `tests/turi/env-longlived.c`): one env,
  loop M (large) top-level evals that each build large transient structures but
  leave only a small global behind; assert steady-state RSS / arena
  `total_bytes` does not grow with M (within a tolerance), and that values read
  out of eval K remain correct after eval K+1.
- A promotion-correctness fixture set: cyclic letrec closures, deeply nested
  structs, ADTs, cons/set values, and a generator/continuation captured into a
  global -- each survives a scratch reset and still evaluates correctly.
- Keep the Phase-1 `tur_env_teardown` gate green (per-unit-env path unaffected).
- Full suite (`bash tests/run.sh`, 10-min timeout) + `tests/run-turi.sh`.

## Risks

- **Incomplete escape walk -> use-after-reset corruption.** The single sharpest
  edge: any payload shape the walk forgets becomes a dangling pointer into the
  rewound scratch region. Mitigate with an exhaustive per-tag walk derived from
  the Phase-1 payload inventory and a debug mode that poisons the scratch region
  on reset so stragglers crash loudly.
- **Cycle/sharing bugs** in the copy (infinite loop or structure duplication)
  without correct forwarding.
- **Host-held values.** Without a handle registry, the safe default is to
  promote every eval result conservatively; a registry is more precise but a
  wider API change.
- **Promotion cost.** The walk runs every top-level eval; for large results it
  adds copy overhead. Acceptable for the kernel pattern (bounded steady state
  beats unbounded growth), but measure.

## Alternatives considered

- **Periodic full rebuild** -- snapshot globals, `turi_env_free`, `turi_env_new`,
  replay defs. Crude, loses non-global live state, and re-elaborates everything.
- **Generational arenas without promotion** -- reset scratch unconditionally and
  forbid cross-eval escapes. Breaks the value-lifetime contract; rejected.
- **Refcounting / tracing GC** -- the precise general answer, but a far larger
  change than this targeted two-region scheme; see the archived Phase-1 plan.

## Recommendation

Build this **only if** a single-immortal-env host (notebook kernel, long-lived
REPL service) becomes a real requirement. The landed Phase 1 already delivers
leak-clean teardown and the per-unit-env embedding pattern; for hosts that can
create an env per unit of work, that is sufficient and this plan is unnecessary.
If undertaken, gate the riskiest part -- the escape walk -- behind a scratch
poison-on-reset debug mode and land the `arena_reset` primitive + promotion walk
incrementally, one payload shape at a time, against the promotion-correctness
fixtures.
