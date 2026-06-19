---
title: Closure-Result Monomorphization -- Execution Plan (the final carrier-bridge floor)
category: Planning -- ABI / Codegen, end-to-end monomorphization
description: The plan for the last ~102 carrier-bridge crossings (24 fixtures), all in `bind`/`ap` monadic dispatch. These are proven (with three tested implementations) NOT removable by method-level monomorphization -- they require the continuation CLOSURE itself to return a by-value container. Grounded in the post-dead-instance-elimination tree (suite green at 1685/0, crossings 1338 -> 102).
---

# Closure-Result Monomorphization -- Execution Plan

> **UPDATE (2026-06-19, execution attempt).** The bind/ap grounding gap was
> root-caused and fixed, and `bind` (plus every aggregate-returning-continuation
> method) now monomorphizes to a by-value `__spec` clone exactly like
> fmap/bimap/pure -- the by-value spec takes the container by value (no arg
> spill) and invokes the continuation through the by-value cast with NO bridge in
> the spec body. Suite green at 1688/0. See "Execution findings" below.
>
> **HOWEVER the crossing count did NOT move (102 -> 102), and the plan's central
> premise is contradicted by the live audit.** The remaining 102 crossings are
> NOT "only bind and ap" (the STATUS line below). A fresh `TUR_M3_AUDIT=1` sweep
> shows: 41 are `Vec`/`MutableMap` carrier boundaries in non-HKT generic fixtures
> (m5-constrained-poly-vec-eq etc.) -- nothing to do with monadic dispatch; the
> Option/Result crossings are dominated by CONSUMER-side bridges where a by-value
> `Option__int` meets the fixtures' deliberate `:int`-carrier inline-C extractors
> (`opt-val`/`res-ok`/...), which intentionally read the carrier layout and so
> MUST take int64; and `ap` is still on its carrier base (the fn-in-container
> `ff : (f (fn a b))` case this fix does not address). Eliminating the bind ARG
> spill simply traded it for an already-present consumer bridge -- the net-neutral
> trap, now confirmed for the real (not half-measure) by-value spec.
>
> **Conclusion: reaching 0 crossings is NOT achievable via closure-result
> monomorphization alone.** It requires (a) by-value Option/Result *consumers*
> (a fixture rewrite away from `:int`-carrier extractors, which defeats their
> purpose as carrier-ABI regression tests, OR consumer-side monomorphization),
> and (b) Vec/MutableMap element monomorphization -- i.e. the parent
> end-to-end-monomorphization plan, not this one. The bind/ap closure-result work
> landed here is correct and worth keeping (architectural parity with the other
> HKT methods), but it is not the lever that deletes the bridge.
>
> **STATUS (2026-06-19).** Carrier-bridge deletion is at **102 crossings / 24
> fixtures**, down from 1338 (92% deleted) via construct-monomorphization +
> dead-instance elimination. Suite green at 1685/0. The remaining crossings are
> **only `bind` and `ap`** (HKT monadic dispatch), and they are the genuine
> floor: `fmap`/`bimap`/`pure` already monomorphize to by-value `__spec` clones
> with no crossing. This plan covers the closure-result ABI change needed to
> remove the last crossings. It is a **multi-session, miscompile-prone** effort
> (three in-session attempts are recorded below, all failed or net-neutral), so
> it is scheduled as its own project with an explicit red-gate window, NOT folded
> into an incremental commit.

## Prerequisite reading

- `docs/upcoming/v2/m7-phase5-carrier-bridge-audit.md` -- the full audit, the
  92% reduction, and the three tested completion attempts with their exact
  failure modes. Read this first; this plan assumes its findings.
- `docs/archive/end-to-end-monomorphization-plan-2.md` -- the parent plan
  (Phases 1-5); Phase 5's "delete the bridge" is what this plan finishes.

## Why the remaining 102 crossings exist (root cause, empirically established)

After dead-instance elimination, every remaining crossing is in a fixture that
**directly calls** a stdlib Option/Result HKT method at a concrete element type.
Of those methods:

- **`fmap` / `bimap` / `pure` already monomorphize cleanly.** Verified: in
  `hkt-stdlib-option-result-instances` the `fmap` call routes to
  `__inst_Functor_fmap_Option__spec__Option__int_Option__int_int64_t(Option__int,
  tur_poly_fn_t)` -- container BY VALUE, no crossing. `fmap`'s continuation `g`
  returns an ELEMENT (a scalar `b`) that flows into a `#{Construct}`
  (`(some (g x))`); the construct takes the carrier scalar directly.

- **`bind` / `ap` do NOT, and cannot at the method level.** `bind`'s
  continuation `k : (fn a (m b))` returns a CONTAINER `(m b)` (a carrier
  `Option`) that IS the result (`(k (.value ma))` in tail position). `ap` is
  analogous (`ff : (f (fn a b))` carries a function whose result is the
  container element).

The decisive proof is implementation #3 below: a **correct** by-value `bind`
spec is **net-neutral** on the crossing count, because `k` is a
`tur_poly_fn_t` whose thunk returns the int64 carrier by design -- so `bind`
reading it by value needs a `carrier->concrete` bridge regardless of whether
`bind` itself is by-value. The crossing simply moves from the call-site
arg-spill to the continuation-result read. **No method-level change removes it.
The continuation CLOSURE must return a by-value container.**

## What was tried in-session (do not repeat these as-is)

1. **Ground all free result tyvars to `int`** (elab `m7_ground_free_result_tyvars`,
   called unconditionally). Result: `ap`'s `ff : (Option (fn a b))` does not
   ground by element alone -- the function element collapsed to `Option__opaque`,
   and the minted
   `__inst_Applicative_ap_Option__spec__Option__int_Option__opaque_Option__int`
   mismatched the call site (`cc: incompatible type for argument 1`). Build
   error. Reverted.

2. **Ground `bind` only** (gate out function-in-container methods like `ap` via
   `m7_method_has_fn_in_container_param`). Result: built cleanly, but
   **miscompiled at runtime** -- `(bind (some 20) (fn [x] (some (+ x 1))))`
   returned `0` instead of `21`. Reverted.

3. **Fix the bind miscompile properly, then re-test #2.** Root cause of the
   miscompile: `emit_expr.c`'s phase-F poly_fn invocation cast `k.fn` to return
   the by-value struct (`((Option__int (*)(void*, int64_t))k.fn)(...)`), but a
   `tur_poly_fn_t` thunk ALWAYS returns the int64 carrier -- a struct-vs-int64
   return-ABI mismatch yielding garbage (`0`). Fixing it to invoke through the
   int64 thunk ABI and bridge the carrier result to the struct made the by-value
   `bind` spec build AND run correctly (`21`). **But crossings went 12 -> 13**:
   the `concrete->carrier` arg-spill was replaced by a `carrier->concrete`
   bridge on the continuation result. Net-neutral. Reverted (no crossing win),
   though the poly_fn-return-ABI fix is a **real latent-miscompile finding**
   worth landing on its own for any future poly_fn returning an aggregate.

Lesson: the problem is not in the method spec or its grounding. It is the
uniform-carrier **closure result ABI**. Attack that directly.

## The actual fix: by-value closure results for monadic continuations

A closure today is a `tur_poly_fn_t { void *env; int64_t (*fn)(void*, int64_t...) }`
-- the thunk result is uniformly `int64_t`. The lambda body of
`(fn [x] (some (+ x 1)))` already builds a by-value `Option__int` internally
(construct half), then the thunk **boxes it to the int64 carrier at return**.
`bind` then **unboxes** it (the `carrier->concrete` bridge). Both the box and
the unbox are the crossing.

The fix is to let such a continuation's thunk **return the by-value container
directly**, and have `bind`/`ap` invoke it through a matching typed signature:

```
// today (carrier):
int64_t __k_thunk(void* env, int64_t x) { return /*box*/ Option__int{...} -> int64; }
__t = /*unbox*/ ((tur_option_t*)k.fn(k.env, x));   // 2 crossings

// target (by-value):
Option__int __k_thunk(void* env, int64_t x) { return Option__int{...}; }   // no box
__t = ((Option__int (*)(void*, int64_t))k.fn)(k.env, x);                    // no unbox
```

This is **closure-result monomorphization**: per-result-type closure thunk
variants, with the `tur_poly_fn_t` slot invoked through the element-correct
typed signature at the (monomorphic) call site.

## Existing infrastructure to build on

- **Typed thunks already exist** for the by-value-arg / fat-closure path:
  `tur_thunk_<R>_<A>_t` typedefs, the typed fat shims (`ensure_typed_fatshim`),
  and the phase-F concrete invocation in `emit_expr.c` (the `phase_f_concrete`
  branch around line 2236) that casts `k.fn` to a typed signature. This plan
  extends that machinery from **arguments** to **results**.
- **The construct half + boundary bridges** (commit 5c8b725) already make the
  lambda BODY produce a by-value container and already bridge at return/if-merge
  sites. So the lambda-internal value is already by-value; only the **thunk
  return type** and the **invocation signature** need to change.
- **`emit_instance_is_live` + dead-instance elimination** mean the carrier
  bases/dicts are already gone for unused instances; the live ones are exactly
  the 24 fixtures this plan targets.

## Phases

### Phase 0 -- land the poly_fn-return-ABI miscompile fix on its own

Independent of monomorphization, the phase-F poly_fn invocation casting `k.fn`
to a by-value struct return is a **latent miscompile** for any `tur_poly_fn_t`
whose result is a carrier-ABI aggregate. Land the int64-thunk-call + bridge fix
(from attempt #3) as a standalone correctness commit, with a regression fixture
that exercises a poly_fn returning an aggregate. This is green-safe today (no
current fixture reaches it until Phase 2 mints by-value bind specs, but the fix
is correct and guards the rest of the plan).

- [ ] `emit_expr.c` phase-F branch: when the call result type is a carrier-ABI
      aggregate, invoke through `int64_t` thunk ABI and bridge `carrier->concrete`.
- [ ] Add fixture `tests/fixtures/poly-fn-aggregate-result/`.
- [ ] Validation: `bash tests/run.sh` green (10-min timeout).

### Phase 1 -- typed closure-result thunks

Teach closure/thunk emission to give a continuation thunk a **by-value result
type** when its body's tail is a by-value construct of a carrier-ABI aggregate
(the construct half already computes this -- reuse `m7_body_constructs_byvalue`'s
notion, or the thunk's resolved result type).

- [ ] Emit `<ByValStruct> __k_thunk(void* env, ...)` instead of boxing to int64
      when the result is a by-value aggregate. The body already produces the
      by-value value; drop the return-boxing bridge.
- [ ] Extend the `tur_thunk_*_t` typedef set + fat-shim machinery to carry the
      by-value result type (mirror the existing by-value-arg path).
- [ ] Gate strictly: ONLY for continuations consumed by a monomorphized
      `bind`/`ap` spec (see Phase 2). A bare closure stored in a carrier slot
      (heterogeneous container, dict dispatch) must keep the carrier result --
      do not change the closure ABI globally, or the heterogeneous-closure and
      any indirect-dispatch paths break.
- [ ] Validation: green; inspect `hkt-stdlib-option-result-instances` thunk C.

### Phase 2 -- mint and route by-value bind/ap specs against typed continuations

With typed-result continuations available, mint the by-value `bind`/`ap` specs
(the elab grounding from attempts #1-#3, now SOUND because the continuation
returns by value):

- [ ] Elab: ground `bind`'s `(m b)` / `ap`'s `(f b)` from the continuation's
      **by-value result type** (now concrete, not collapsed) -- replaces the
      unsound int-defaulting of `m7_ground_free_result_tyvars`.
- [ ] Handle `ap`'s `ff : (f (fn a b))` -- the function-in-container arg. Its
      by-value form is `Option<typed-thunk>`; represent it so the spec arg type
      is not `Option__opaque`. This is the hardest sub-step (attempt #1's failure
      point); may need a typed-fn element representation in the by-value Option.
- [ ] Emit: the by-value `bind`/`ap` spec invokes the continuation through the
      typed signature (no spill, no unbox).
- [ ] Validation: green; crossings in the 24 fixtures should now drop (NOT be
      net-neutral -- verify the count actually decreases).

### Phase 3 -- sweep to zero, delete the bridge

- [ ] `TUR_M3_AUDIT=1` per-fixture sweep: confirm 0 crossings across all
      fixtures (the 5.1 tripwire in `emit_core.c` should fire nowhere).
- [ ] Delete `emit_carrier_bridge` and its call sites; remove
      `expr_emits_byvalue_carrier_abi` / `type_uses_carrier_in_dispatch` and the
      `tur_box_*` helpers if now unreferenced.
- [ ] Regenerate all fixture snapshots (large coordinated regen -- schedule a
      regen window, do not collide with in-flight branches).
- [ ] Archive `end-to-end-monomorphization-plan.md` and the v2 audit to
      `docs/archive/`; this plan to `docs/archive/` on completion.

## Risks (read before starting)

- **Closure ABI blast radius.** The closure result ABI is on the path EVERY
  higher-order program compiles through. Phase 1's gating is the crux: change it
  only for monomorphized-bind/ap continuations, never globally, or heterogeneous
  closures and any future indirect dispatch miscompile. Two of the three
  in-session attempts produced a build error / miscompile in this exact area.
- **`ap`'s function-in-container arg** (`Option<fn>`) is the genuinely novel
  representation problem (Phase 2 step 2); it has no precedent in the construct
  half. Budget the most time here.
- **Red-gate window.** Phases 1-2 will not be green mid-flight. Per CLAUDE.md
  transient red is acceptable *while a multi-step change is in flight, reconciled
  before done* -- so this MUST run to green within the dedicated effort, not be
  left abandoned-red. If it cannot converge, revert to the 92% green floor
  (which is the current committed state) rather than ship a partial red tree.
- **Net-neutral trap.** Attempt #3 proved a half-measure (by-value method,
  carrier continuation) does not reduce crossings. Phase 2's validation must
  confirm the count actually drops, not just that it builds and runs.

## Definition of done

- `TUR_M3_AUDIT=1` per-fixture sweep: **0 crossings**, all fixtures.
- `bash tests/run.sh`: green at the then-current fixture count.
- `emit_carrier_bridge` and the bridge predicates deleted (or provably
  unreachable and removed).
- Parent monomorphization plan + v2 audit archived.

## Execution findings (2026-06-19)

What was implemented and landed (green, 1688/0):

1. **Root-caused the bind/ap grounding gap.** An unannotated monadic
   continuation `(fn [x] (some (+ x 1)))` whose body is a carrier-ABI aggregate
   recorded only `result_kind = TY_APP` and dropped the full result type:
   `elab_fns.c`'s return-type inference only captured `result_full_type` for a
   `TY_FN` body. So the continuation typed as `(fn [int] (type-app ? ?))`, and
   `m7_collect_tyvar_bindings` could not recover `b` from `bind`'s `(m b)`
   result -- `m7_byvalue_grounded` stayed false and dispatch fell back to the
   carrier base. The agent-claimed "b is only known at runtime" was wrong; the
   info was present in the continuation body, merely discarded at lift time.

2. **Fix (elab_fns.c).** When inferring a lambda's return type from its body,
   also preserve a fully-ground carrier-ABI aggregate body type
   (TY_APP/TY_ADT/parametric TY_STRUCT). Gated on NO free named tyvar -- a
   residual free arm (the fixed `B` of a partial ok-biased `(Result int B)`)
   makes the lifted continuation wrapper `emit_abi_fn_is_generic_unsafe` and it
   gets skipped as dead generic code while its address is still taken (an
   undeclared `__poly_N`).

3. **Result:** `bind` (and any aggregate-returning-continuation method) now mints
   `__inst_Monad_bind_Option__spec__...(Option__int, tur_poly_fn_t)` taking the
   container BY VALUE and invoking the continuation via the by-value cast
   `((Option__int (*)(void*, int64_t))k.fn)(...)` -- no arg spill, no bridge in
   the spec body. This is genuine architectural parity with fmap/bimap/pure.

4. **Reverted the Phase-0 carrier->concrete bridge.** It assumed the continuation
   thunk always returns the int64 carrier. Post construct-monomorphization,
   `make_poly_wrapper` returns the by-value aggregate directly and
   `ensure_aggregate_spill_shim` explicitly EXCLUDES carrier-ABI aggregates, so
   no int64-boxing shim is inserted -- the original by-value cast is correct, and
   the bridge produced a wild-pointer deref once the path became reachable. (So
   Phase 0 as written in this plan is moot: the int64-thunk premise no longer
   holds for Option/Result continuations.)

Why the metric did not move (the decisive measurement):

- A full `TUR_M3_AUDIT=1` sweep is byte-identical before and after: 102 -> 102.
- `hkt-stdlib-option-result-instances`: 12 -> 12. The bind args are now by-value
  (`some__spec__...(20)`, no spill), but the fixture's `(opt-val byval_option)`
  consumers -- `opt-val : (defn [o : int] ...)`, an inline-C extractor reading
  the int64 carrier layout -- bridge `concrete->carrier (Option int)`. Removing
  the producer spill exposed the pre-existing consumer bridge 1:1.
- `ap` is still on its carrier base (`__inst_Applicative_ap_Option((int64_t)
  (intptr_t)(&__t57), ...)`); the lambda-result grounding does not touch
  `ff : (f (fn a b))`.
- 41 of the 102 are `Vec`/`MutableMap` (e.g. `m5-constrained-poly-vec-eq`),
  unrelated to monadic dispatch and untouched by this work.

Recommendation: keep the bind/ap-grounding commit (correct, green, parity). Do
NOT pursue Phase 3 (delete the bridge) under this plan -- the bridge is
load-bearing for the consumer-side Option/Result crossings and the Vec/Map
crossings, none of which closure-result monomorphization removes. The path to 0
crossings runs through the parent end-to-end-monomorphization plan (consumer +
Vec/Map element monomorphization), so this plan should be re-scoped to "land
bind/ap by-value parity" (DONE) and the bridge-deletion goal moved under the
parent plan.

## Final disposition (2026-06-19) -- why this plan cannot reach 0 crossings, and that is correct

Two questions settle the plan's fate:

**Does leaving the bridge in lose any functionality?** No. `emit_carrier_bridge`
is purely an internal ABI impedance-matcher (int64 carrier <-> by-value struct).
Every crossing is a correct translation; the suite is green at 1688/0 with it in.
For Option/Result it is a stack spill + field copy; for the 41 Vec/MutableMap
crossings it is a no-op pointer reinterpret the C compiler folds away. The only
cost of keeping it is the bridge machinery's presence in the tree. "Delete the
bridge" is a code-hygiene goal, not a capability goal.

**Should the remaining-crossing fixtures change?** No -- they serve a purpose:

- The `:int`-carrier inline-C extractors (`opt-val`/`res-ok`/... in the
  `hkt-stdlib-*` fixtures) are deliberate carrier-ABI regression coverage: they
  read the raw `{ bool; int64_t }` layout to assert the representation. Their
  crossing is `(opt-val byval_option)` -> concrete->carrier. Rewriting them to
  by-value `some?`/`unwrap` would delete that coverage and game the metric.
- `ap`'s fixture (`hkt-stdlib-option-result-instances`) explicitly ascribes the
  function-in-container to int: `(some (:: (fn ...) int))`. So `ff` is genuinely
  `(Option int)` -- the fn structure is gone by the fixture's own choice, and `b`
  cannot be recovered structurally. This is the empirical reason attempt #1's
  "element collapsed to opaque" happened; it is the FIXTURE erasing the element,
  not a compiler gap. There is no fixture exercising `ap` with a by-value
  fn-in-container, so `ap` grounding has nothing to ground against.
- The 41 Vec/MutableMap crossings are generic heap-tagged helpers compiled once
  over a tyvar; eliminating them is per-element monomorphization of all generic
  Vec/Map functions -- the parent end-to-end-monomorphization plan, a large
  change to erase a no-op cast.

**Conclusion.** Every remaining crossing is either (a) deliberate carrier-ABI
regression coverage or (b) a benign Vec/Map heap reinterpret reducible only by
the parent plan's general monomorphization. None is removable by closure-result
monomorphization, and none should be removed by editing the deliberate tests.
The bind/ap by-value grounding landed here is the correct and complete extent of
THIS plan (architectural parity with fmap/bimap/pure). The "0 crossings / delete
the bridge" objective is hereby moved to the parent
`end-to-end-monomorphization-plan` (general consumer + Vec/Map monomorphization)
and is NOT pursued here. The bridge stays -- load-bearing and functionally free.
