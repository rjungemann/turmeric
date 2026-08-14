# Fn-value fat normalization -- one calling convention for non-carrier fn boundaries

**Status:** proposed. This is the plan the investigation in
`docs/reported/poly-result-hof-capturing-closure-sigbus.md` said the fix
wants ("it is a calling-convention change across every non-carrier fn-typed
parameter, so it wants its own plan and a full-suite regen, not a patch"),
plus the sibling findings that arrived since
(`docs/archive/history/fn-typed-value-return-ascribe-miscompiles.md`). No `--enable`
gate: this is a miscompile fix converging on existing checked behavior, not a
new user-facing feature -- the experiment machinery does not apply, the
regen-coordination rules do.

This is **increment 1** of
[representation-consolidation-meta-plan.md](representation-consolidation-meta-plan.md);
the landing checklist and probe discipline there apply on top of the stages
below.

## Problem

A fn-typed value today travels in one of (at least) six representations,
chosen independently per boundary: the `tur_poly_fn_t {env, fn}` carrier,
`^fat` handles, `:ptr<void>`-fat sinks (with an `is_fat` dispatch flag),
nominal bare `TY_FN` code pointers, struct-field-fat, and (visible in emitted
C) a by-value fat struct in parameter slots. Producer and consumer each pick
a form with no shared rule, so specific compositions pass `tur check` and
then emit invalid C or crash:

- capturing closure -> nominal thin `TY_FN` param (tyvar arg/result, by-value
  struct arg/result, effect row, `^linear`/`^borrow`): SIGSEGV/SIGBUS
  (poly-result report, mechanism confirmed -- the callee jumps into the env
  struct).
- fn-typed value RETURNED through a pass-through param: invalid C thin
  (`return (int64_t)(intptr_t)v;` on an aggregate) and SIGSEGV `^fat`.
- ascription around a let-wrapped closure value: SIGSEGV.
- `^fat` HOF over a nested fn type `(fn [] (fn [] T))`: SIGSEGV.

See `docs/guides/value-representations-guide.md` for the full inventory and
the missing-cells table.

## Direction

Do for non-carrier fn-typed *parameters, returns, and let/ascribe positions*
what was already done for `defstruct` fn-typed *fields*
(`tests/fixtures/capturing-closure-struct-field/`), because that fix is the
existence proof:

> Concrete `(fn ...)` fields now use the fat representation uniformly ...
> the make-struct store shims a bare/thin fn into a fat `{thunk, env}`
> handle, and every field-call dispatches via the fat protocol (TUR_APPLY).

Invariant after this plan: **a fn-typed VALUE crossing any boundary that is
not the carrier is a fat `{thunk, env}` handle, always.** Bare top-level fns
and non-capturing lambdas are shimmed into fat handles at the boundary (env =
NULL or a sentinel; the shim is the existing struct-field one). Every invoke
of a non-carrier fn value dispatches via the fat protocol. The carrier stays
exactly as-is for carrier-eligible signatures -- this plan does not widen
carrier eligibility (`fn_type_has_named_tyvar` exists because widening it
miscompiled by-value struct args; see `poly-hof-constrained-arg-baked-carrier`).

## Stage 0 -- landed 2026-07-30 (ABI ratified, matrix pinned, blast radius measured)

Per the meta-plan checklist, before any emitter change:

- **ABI ratification probe:** `tests/probes/fat-normalization-f0/fatparam.c`
  proves the stage-1 convention in hand-written C against the real emitted
  layouts (drop-glue header, `__fn` slot 0, `{shim, orig}` bare box,
  slot-0 dispatch): one compiled callee body serves a capturing closure
  AND a shimmed bare fn through a nominal fn-typed parameter, including a
  pass-through (`thru`) hop. All 5 properties PASS, ASan/UBSan clean.
- **Ok-rows pinned pre-fix:** `tests/fixtures/fn-value-matrix-ok-rows/`
  holds the working rows of the boundary matrix (direct / let / ascribe /
  gid invoke, thin consume, `^fat` consume, cstr payload) so the working
  boundary cannot regress while the broken rows flip.
- **D0 blast radius, measured with the increment-0 repr-trace** over a
  300-fixture sample (seed-42 shuffle; stdlib baseline of 12 fn-param
  lines/emit subtracted): the sample carries **~20 user nominal thin-fn
  params** vs ~21 `^fat`, ~8 carrier, 1 cfnptr -- extrapolating,
  **roughly 95-100 nominal thin fn params corpus-wide, plus 1 in the
  auto-loaded stdlib** (a tyvar-sig param, so present in every program).
  Stage 1's churn is therefore concentrated in ~7% of fixtures plus one
  stdlib signature -- moderate, not tree-wide.

**Implementation map for stage 1** (found while investigating; the two
sides must land together):

- *Elab side:* the `^fat` auto-shim block (`elab_call.c` ~5425, keyed on
  `FN_ARG_FLAG(..., FA_FAT)`) is the machinery to extend to nominal thin
  TY_FN param slots -- and it is a dense accretion of guards that ALL
  apply to the generalized case: the already-fat ascription strip, the
  captureless-under-ascription strip, the is_fat double-shim retype, the
  `EX_POLY_TO_FAT` conversion, and the pass-through set. Each guard is a
  pre-registered canary for stage 1, not incidental complexity.
- *Emit side:* the invoke branch (`emit_expr.c`, "ER2: Callback call
  through a local TY_FN parameter") currently fat-dispatches only
  `is_fat || boxed` and falls through to the thin
  `((R (*)(...))(intptr_t)p)(...)` call. The flip is scoped to
  `is_param` bindings (let-bound TY_FN locals keep today's behavior in
  stage 1) and must exclude `cfnptr`.
- *Hazard found:* a capturing closure at a nominal param is ACCEPTED by a
  different elab path than the `^fat` one (`A#1` gate at ~4780 is
  FA_FAT-gated), so the acceptance and the shim rules live in different
  blocks -- stage 1 must reconcile them or the shim will miss shapes the
  checker admits.

## Stage 1 -- LANDED 2026-07-30, with a NARROWED claim

Shipped: a nominal thin fn-typed parameter with a **concrete, effect-free**
signature is fat-normalized -- the shared decision is
`fn_param_type_is_fat_normalized` (`types.c`), consulted by BOTH the elab
call-site shim (`elab_call.c`, the generalized `^fat` auto-shim gate) and
the emit invoke dispatch (`emit_expr.c` ER2, scoped to `is_param`).  The
measurements, in meta-plan per-step form:

| tree | `run.sh` | behavioral |
| --- | --- | --- |
| baseline (stage 0) | 2437 / 0 | -- |
| Step A alone (emit flip, broad claim) | 2275 / 162 | 24 stdout |
| A+B (broad claim) | 2265 / 172 | 28 stdout |
| A+B (narrowed) + forwarding guard | **2437 / 0** | 0 |

The two narrowings, each measured (the CPS-graduation rule -- narrow the
claim, do not patch the misses):

- **effect rows stay thin**: 17 effect/cps fixtures regress behaviorally
  under normalization -- a colored (CPS-lowered) callback's thin convention
  is LOAD-BEARING (twin/trampoline dispatch).  Normalizing effectful fn
  params needs CPS-aware treatment; deferred.
- **tyvar signatures stay thin**: 10 hkt-cata / van-laarhoven fixtures
  regress -- a tyvar-sig param's arguments arrive through the
  generic/carrier machinery as thin pointers the call-site shim cannot
  see.  Deferred to the carrier-side increments (meta-plan 2+).
  **Lifted 2026-08-01 by increment 2 below.**

One post-flip defect found and fixed by the s1c probe: a normalized param
FORWARDED as an argument into another normalized slot was double-shimmed
(the fat handle re-boxed, then invoked as code).  The existing `is_fat`
double-shim guard now also covers normalized params.  Zero snapshot churn:
no fixture exercised the normalized set -- which is exactly why its crash
rows survived so long.

What this closes (pinned in `tests/fixtures/fn-value-fat-normalized-params/`):
the poly-result crash table's by-value struct ARG and RESULT rows, heap
container results, and the forwarding hop -- each with capturing closures.
The `^linear`/`^borrow` substructural rows are verified fixed as well
(probed with capturing closures, correct output): substructural params are
not `plain` so they were nominal, and their concrete signatures fall inside
the narrowed claim.  Still open from that table: the tyvar rows and the
effect-row row, both now explicitly out of the narrowed claim.  The
`--known-probes` by-value probe prints FIXED; the tyvar probe still fires,
as narrowed.

## Stage 2 -- LANDED 2026-07-30 (return / let / ascribe positions)

Four targeted bridges, all gated on the same shared predicate:

1. **Tail normalization** (`elab_normalize_fn_tail_leaves`, elab_fns.c): a
   defn whose declared result is a concrete effect-free fn type returns a
   fat handle ALWAYS -- thin tail leaves shimmed, carrier-param leaves
   boxed via EX_POLY_TO_FAT (previously `return (int64_t)(intptr_t)v;` on
   the tur_poly_fn_t aggregate, the invalid-C row), fat leaves untouched;
   the declared result is marked `boxed` so consumers ride the existing
   boxed-result plumbing.
2. **Binding-aware tail classification**: `fn_tail_fn_leaf_kinds` and the
   mixed-path boxer now recognize stage-1 normalized params and carrier
   params -- closing a latent stage-1 double-box hazard in mixed bodies.
3. **Ascription preserves fat identity** (elab_types.c): `(:: e T)` onto a
   fn type keeps `boxed` when the inner is already a fat handle -- the
   ascribe-around-let SIGSEGV row.
4. **Nested results in param annotations** are boxed recursively: stage-2
   producers return fat, so `(fn [int] (fn [int] int))` annotations must
   say so or `((f 1) 2)` thin-dispatches a fat handle -- found by the
   suite measurement (2 curried fixtures), not predicted.

Measurements: matrix probes all green (m1-m15); suite 2436/2 after the
first three bridges (the two curried fixtures), **2438/0** with bridge 4.
The full boundary matrix of `fn-typed-value-return-ascribe-miscompiles` --
broken rows included -- is pinned in
`tests/fixtures/fn-value-matrix-ok-rows/`.

## Increment 2 -- LANDED 2026-08-01 (tyvar signatures; the carrier-side feeds)

Stage 1 deferred tyvar-signature params on a diagnosis that turned out to
name exactly two missing shim sites, not a representation problem: "a
tyvar-sig param's arguments arrive through the generic/carrier machinery as
thin pointers the call-site shim cannot see."  Both feeds are now shimmed,
so the exclusion is gone from `fn_param_type_is_fat_normalized`.

**Re-measuring first.** Lifting the tyvar exclusion on the post-stage-2 tree
costs **2** behavioral fixtures, not the 10 stage 1 saw -- stage 2's
return/let/ascribe bridges had already absorbed most of that set.  Both
survivors were one mechanism each:

1. `hrt-curried-fn-result` -- `((l inc) 10)` where `l` is a rank-2 forall
   param.  The call goes through the carrier (`l.fn(l.env, arg)`), a path
   `elab_poly_call` owns and the A#1 call-site shim never reaches.  Fixed by
   shimming fn-typed slots of the forall body there, beside the existing
   `TY_FORALL`-slot `EX_POLY_WRAP` handling.  Both ends agree by TYPE: every
   function reachable through that forall declares the same slot.
2. `stdlib-lens-record-field` -- `(lens get put)` where `get`/`put` are now
   normalized params being stored into fat struct fields.  The make-struct
   store shim's already-fat test is `type.as.fn.boxed`, which a normalized
   param does not set (normalization is a property of the type, not a flag
   on it), so the handle was boxed a SECOND time and the field call ran it as
   code.  Fixed with the store-side twin of the A#1 forwarding guard.

**The param/result asymmetry is deliberate.** A tyvar-sig fn type is
normalized in PARAM position but NOT as a declared RESULT --
`fn_result_type_is_fat_normalized` (new) keeps the concrete-only claim and is
what stages 2's tail normalizer and the nested-annotation boxer consult.
Widening the result side too double-boxes against the hrt-curried-result
poly-call protocol, which boxes returned closures itself.  The two sides may
differ safely because the call-site shim normalizes whatever it is handed.
`repr_of` routes on `REPR_POS_PARAM` so the ABI trace reports the same split.

**Measurements**

| | `run.sh` | behavioral | `run-turi.sh` |
| --- | --- | --- | --- |
| baseline | 2503 / 0 | -- | 1701 / 0 |
| tyvar flip, no carrier-side work | 2361 / 142 | 2 | -- |
| + both shims | 2363 / 140 | **0** | -- |
| + snapshot regen + acceptance fixture | **2504 / 0** | 0 | **1702 / 0** |

The 140 snapshots are one function: the auto-loaded stdlib's `consume`
(a tyvar-sig fn param, so present in every program) moving from thin to fat
dispatch -- exactly the "1 in the auto-loaded stdlib" stage 0's blast-radius
probe predicted.  `--known-probes` now prints FIXED for both poly-result
rows, the `tyvar_run` `known_bug_slug` row is retired, and `--n 500` on two
fresh seeds (1337, 20260801) is green with those legs back in the pool.

**Perf: no measurable cost on real workloads, one synthetic cliff.**
`benchmarks/run-benchmarks.sh` on Release builds either side: 4 of 11
benchmarks run (the other 7 fail identically before and after -- pre-existing),
and those 4 are unchanged (86->86ms, 132->131ms, 3->2ms; parsec-json's
7->10ms harness reading is single-run noise -- best-of-5 on the built binary
is 9.8ms before, 8.2ms after).

The cliff is real but narrow: `EX_FN_TO_FAT` **mallocs a fresh `{shim, orig}`
box every time it executes**, so a tight loop that passes a BARE GLOBAL fn
into a tyvar-sig HOF now allocates per iteration.  A 20M-iteration
`(apply1 add3 acc)` loop goes 0.003s -> 0.533s -- though the 0.003s is the C
compiler deleting a loop it could inline through, so the multiplier is an
artifact of an empty body, not a 175x on real code.  This is stage 1's
mechanism, not increment 2's; increment 2 widens its population.

## Increment 2b -- LANDED 2026-08-01 (the static box; the leak, not the clock)

The allocation above is not merely slow.  Nothing frees a box handed to a
normalized param, so the loop LEAKS one per iteration: 5e6 iterations of
`(apply1 add3 acc)` peaked at **122 MiB**, 24 bytes a turn, growing linearly
with trip count.  That reclassifies the follow-up from perf to memory
correctness, so it was done rather than deferred.

A box whose `orig` is a file-scope function is a constant, so it is allocated
once at file scope and filled from `__tur_static_init` (S1b, which exists so
startup work survives any C11 front end -- a function pointer cast to
`int64_t` is not an address constant, so a static initializer would not be
portable).  Storage is a `union { void *; int64_t; char[...] }` rather than a
struct: it needs `max(void *, int64_t)` alignment WITHOUT a struct's padding,
which on a 32-bit target (wasm32) would move slot 0 off `+ sizeof(void *)` and
break the `h[-1]` header recovery.

The drop-glue blocker dissolved into two pieces:

- The header is `__tur_fatbox_keep`, a no-op, not the malloc'd box's NULL.
  NULL means "free the base allocation" to `tur_closure_drop`, which on a
  static address is heap corruption and, on a second drop, a double free.
  A no-op glue makes every drop path correctly do nothing -- which is also
  what makes SHARING one box between sites sound.  (The box is write-once:
  `EX_FN_TO_FAT` and `EX_POLY_TO_FAT` are the only writers of these slots in
  the tree, both at creation, and nothing compares fn values by identity.)
- Correct at runtime is not sufficient.  GCC cannot see the no-op through the
  inlined `tur_closure_drop` and reports `'free' called on unallocated
  object` (-Wfree-nonheap-object) at every drop site -- observed on
  `closure-drop-glue-fatshim` and two `catch-unwind-*` fixtures.  Initializing
  the header statically did not fold the branch away.

So the hoist is **opt-in per shim site** (`fn_to_fat_.static_ok`), and exactly
one site opts in: the A#1 shim at a normalized NOMINAL param.  Nothing drops a
box handed to one -- which is precisely why it leaked -- so no drop path can
reach a static box.  `^fat` sinks and owning struct fn-fields keep the heap
box: a `^fat` callee MAY drop its argument (`closure-drop-glue-fatshim` calls
`TUR_CLOSURE_DROP` on one) and the call site cannot tell.  Default-off means a
future EX_FN_TO_FAT site is heap-allocated until someone proves otherwise.

**Measurements.** The leak is gone and the cliff is mostly gone:

| | peak RSS (5e6 iters) | 2e7-iter loop |
| --- | --- | --- |
| before | 122 MiB | 0.533 s |
| after | **1 MiB** | **0.042 s** |

The residual 0.042 s over 2e7 calls is ~2 ns/call -- the fat indirection this
plan always accepted, not an allocation.  Suite 2505/0, turi 1703/0, ratchet
green (`fn_result_type_is_fat_normalized` joined the pinned predicate list).
Corpus footprint is small and honest: **one** fixture
(`van-laarhoven-lens-wide-compose`) actually takes the static box, because
most of the corpus's 4334 boxing sites are `^fat` sinks, struct stores and
typeclass shims rather than the normalized nominal slot.  Pinned by
`tests/fixtures/fn-value-static-fatbox/`, which also asserts the two forms
that must NOT change (owning fn-fields, capturing closures).

**Found while measuring, filed not fixed:** the same per-call box at a `^fat`
sink leaks too, and worse -- 5e6 iterations peaked at **1002 MiB**.  It is
pre-existing and needs an ownership contract on `^fat` before a caller can
choose a representation, so it is its own report:
[`fat-sink-shim-box-leaks-per-call`](../archive/fat-sink-shim-box-leaks-per-call.md).

## Stages

1. **Param normalization.** Non-carrier fn-typed parameters take the fat
   handle: call sites shim bare/thin values in, callee invokes fat. Sites:
   `src/compiler/emit_expr.c:4536` (nullary nominal-`TY_FN` invoke) and the
   n-ary sibling ~4550; the shim exists in the struct-field store path.
   Kills the whole poly-result crash table (tyvar arg/result, by-value
   struct arg/result, effect row, `^linear`/`^borrow` rows).
2. **Return/let/ascribe positions.** A fn-typed value returned from a defn,
   bound by `let`, or passed through `(:: e T)` keeps the fat handle -- no
   thin re-casts on the way out (`return (int64_t)(intptr_t)v` on an
   aggregate is the current failure). Kills the
   fn-typed-value-return-ascribe matrix rows, including nested
   `(fn [] (fn [] T))`.
3. **Unify the flag'd sinks.** `:ptr<void>`-fat's `is_fat` runtime flag and
   the struct-field-fat protocol become the same code path as stages 1-2
   where practical; at minimum, document any residual difference in the
   representations guide.
4. **Regen + acceptance.** Full fixture regen in the same PR (this will
   move every snapshot that touches closures). Acceptance tests:
   - the full matrix from `fn-typed-value-return-ascribe-miscompiles.md`,
     ok rows included;
   - the poly-result crash table as fixtures;
   - `python3 tests/type-fuzz-src.py --known-probes` shows the three
     fn-value probes `FIXED`;
   - retire the corresponding `known_bug_slug` rows in
     `tests/type-fuzz-src.py` (the thunk-crossing avoid rule and the
     thin-hof by-value rule), returning those shapes to the default fuzz
     pool -- then a `--n 500` session on two fresh seeds stays green;
   - verify `van-laarhoven-lens-*`, `hkt-cata-fn-arg-carrier`,
     `local-struct-fnfield-drop`, and `capturing-closure-struct-field` still
     pass -- the fixtures the abandoned compile-time diagnostic falsely
     fired on; they pin today's correct fat behavior and must not move.

## Costs / risks

- **ABI churn:** every non-carrier fn boundary changes emitted C; expect a
  large snapshot regen (coordinate timing per the fixture-churn rule -- one
  regen window, same PR).
- **Perf:** fat dispatch adds an indirection for previously-thin bare fns at
  non-carrier boundaries. These boundaries are exactly the ones that today
  crash on closures, so the population is small; benchmark `tur run bench`
  before/after and note the delta in the PR.
- **Interpreter parity:** turi already treats closures uniformly; verify
  with `tests/run-turi.sh` rather than assuming.

## Non-goals

- Widening carrier eligibility (explicitly rejected; see above).
- A compile-time diagnostic for unrepresentable combinations -- tried,
  abandoned, and documented in the poly-result report (six false positives
  on correct fixtures); normalization makes the combinations representable
  instead of rejected.
- `generic-closure-return-type-app` Defect A/B (checker-side type-app
  erasure and missing ctor emission) -- same neighborhood, different layer;
  its report keeps its own fix directions.

## Doc follow-up

Update `docs/guides/value-representations-guide.md` in the landing PR: the
closure zoo collapses to carrier + fat, the missing-cells table loses its
closure rows, and the two closure reports move to `docs/archive/` with their
links corrected. (Each report's "Guide upkeep" section says the same from
the other side.)
