---
title: M4d -- Typed/Conditional Dict Emit for Eq[Vec] (root-2 crossings) -- Execution Plan
category: Planning -- ABI / Codegen, end-to-end monomorphization
description: The concrete plan for the dominant residual M3 audit bucket -- the 80 `Vec int` carrier crossings (root 2). Grounded in the emitted C of vec-of-tvec-eq / data-literal-typed-empty as of the post-Vec-producer-slice tree (commit 600e859). Establishes that these crossings live in a DEAD-but-emitted Eq[Vec] carrier base whose only referent is an unread dict singleton, and sequences the conditional-dict-emit work that removes them.
---

# M4d -- Typed/Conditional Dict Emit for Eq[Vec] -- Execution Plan

## Status (verified 2026-06-22) -- effectively superseded

**Audit-reducing intent of M4d is complete.** The "80 `Vec int` carrier
crossings" framing was retired in-plan on 2026-06-17 (post #400). What
the plan called for as Phase 1 shipped via a single-pass
`emit_instance_is_live` liveness gate rather than the two-pass post-emit
DCE the doc concluded was the only robust route.

- **#400 (TCO'd by-value `Eq [Vec]`)** -- DONE. Commit `6f381cc4`. The
  `Eq[Vec]` carrier base no longer carries any `Vec int` crossing.
  Floor recorded at 34/10 by `7b03c65c` ("Track A: record post-#400
  bridge-audit floor (60 -> 34, zero deref-copies)"). Floor
  documented in `web/dist/client/docs/monomorphization-audit.md:22-50`.
- **Phase 1 (conditional dict + carrier-base emission):** DONE via
  different mechanism than sketched. `emit_instance_is_live` at
  `src/compiler/emit_module.c:4149` is the liveness oracle; dict-emit
  gate at `src/compiler/emit_stmt.c:464-466` (covers both HKT (gated on
  `g_m7_hkt_enabled`) and ground/kind-* instances unconditionally);
  carrier-base body-skip at `emit_module.c:4221` / `:4237` inside
  `emit_abi_fn_skip_generic`. The doc's two-pass post-emit DCE was NOT
  taken; instead liveness is the union of `emit_abi_has_carrier_call`
  plus EX_DICT/witness liveness.
- **Phase 2a (typed comparator parameter):** NOT TAKEN. Plan's own
  2026-06-17 update proved 2a only relocates the cast.
- **Phase 2b (accept comparator cast as permanent type-erased
  boundary):** DONE (as a decision). 22 `Vec int` casts documented as
  permanent in `monomorphization-audit.md:30-37`.
- **Phase 3 (re-audit + bridge down-scope):** DONE for the non-HKT
  collection-Eq cascade. Audit floor 34/10, all permanent by-design
  boundaries. `emit_carrier_bridge` still alive (~15 call sites in
  `emit_expr.c`) only on documented type-erased boundaries.

**Open remainders (low-priority):** the two-pass post-emit DCE (lines
223-274) was never implemented; current single-pass coverage handles
audit-reducing cases but the deeper code-size cleanup (e.g. dropping
`defn-basic`'s 93 dead `__inst_*` references) is still on the table as
an optional coordinated-regen window.

**Recommendation:** archive this plan -- working line moved to the parent
monomorphization audit doc and is tracked there.

---

> **STATUS UPDATE 2026-06-17 (post-#400 -- read this first).** #400 (the
> pure-Turmeric TCO'd `Eq [Vec]` rewrite) **moved the by-value loop into the
> `__spec`** and left the int64 **carrier base** delegating to the carrier
> helpers (`vec_hylen` / `vec_hyeq_hyloop`). The carrier base therefore **no
> longer carries any `Vec int` crossing** -- the "80 `Vec int` carrier crossings
> in the dead carrier base" this plan was written against are GONE. The current
> `TUR_M3_AUDIT=1` floor is **34 crossings / 10 fixtures**, of which the **22
> `Vec int`** are entirely the **live element-comparator thunks** (Phase 2),
> never the carrier base. Consequences for this plan:
>
> - **Phase 1 (dead-dict/base DCE) no longer reduces the audit.** The dead
>   carrier base it removes has zero crossings post-#400, so Phase 1 is now a
>   pure **code-size / compile-time / dead-`emit_carrier_bridge`-reachability**
>   cleanup, not a crossing win. It is still worth doing (e.g. `defn-basic`'s
>   snapshot carries **93 dead `__inst_{Eq,Clone,Hash,Ord,Show}` references**),
>   but it is a **large coordinated snapshot regen** (every program that links
>   stdlib emits these dead instances) and must be scheduled as a regen window,
>   not rushed. The two-pass post-emit DCE (below) remains the only robust route.
> - **Phase 2a (typed comparator parameter) does NOT eliminate the cast --
>   verified.** The byval `vec-eq-loop` spec sources each element with
>   `vec_hyget(x, i)`, which returns **int64** (the matrix keeps Vec's `data[]`
>   buffer int64 for *every* element type -- interpreter parity + float-element
>   reads). So typing the comparator param to `Vec__int *` only **relocates** the
>   `(Vec__int *)(intptr_t)` cast from the thunk body to the call site
>   (`cmp((Vec__int *)vec_hyget(...), ...)`); it cannot remove it. Full
>   elimination needs **element-buffer monomorphization** (`vec-get` returning
>   `Vec__int *` per element type), which the parametric-type ABI matrix
>   deliberately rules out. **Therefore the disposition is Phase 2b: the 22
>   comparator casts are the permanent type-erased boundary** the bridge is
>   down-scoped *to*, not deleted from (matrix roadblock 4). They are cheap
>   reinterpret casts (Vec is `:heap`, so the int64 *is* the pointer), correct
>   and free at `-O2`.
>
> Net: the audit-reducing work this plan targeted is **complete** (via #400, a
> different route than the dict typing here). What remains is (a) optional
> Phase-1 dead-code DCE as a coordinated-regen cleanup, and (b) accepting the
> Phase-2 comparator casts as permanent. The original root-2 framing below is
> kept for history.

---

This is **root 2** in the bucket-A breakdown of
[m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md](../../reported/m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md):
the **80 `Vec int` carrier crossings** that dominate the 93-crossing residual
after the Vec producer slice (commit `600e859`). It is the implementation arm of
the parent plan's **M4** ("non-HKT typeclass instances switch to per-method
ABI") for the specific case the audit still flags.

## The finding that grounds everything (verified in emitted C)

Concrete `.eq?` dispatch on a `(Vec int)` already calls the **typed
per-instantiation spec** `__inst_Eq_eq_qu_Vec__spec__..(Vec__int *, Vec__int *)`
directly -- M4c (Path A) landed that. So the typed dispatch path is clean.

The crossings live in the **int64 carrier base** that M4c keeps for the dict:

```c
static bool __inst_Eq_eq_qu_Vec(int64_t x, int64_t y) {           // carrier base
    int64_t lx = vec_len_byval__spec__..((Vec__int *)(intptr_t)(x));   // crossing
    int64_t ly = vec_len_byval__spec__..((Vec__int *)(intptr_t)(y));   // crossing
    ... vec_eq_loop_byval__spec__..((Vec__int *)(intptr_t)(x), (Vec__int *)(intptr_t)(y), ...); // crossing
}
typedef struct dict_Eq_Vec { bool (*eq_qu)(int64_t, int64_t); } dict_Eq_Vec;
static dict_Eq_Vec dict_Eq_Vec_singleton = { .eq_qu = __inst_Eq_eq_qu_Vec };
```

Two facts, both confirmed by grep over `vec-of-tvec-eq` and
`data-literal-typed-empty`:

1. **The carrier base is never called.** `__inst_Eq_eq_qu_Vec(` appears only as
   its forward decl, its definition, and the dict-singleton initializer -- there
   is no call site. Concrete dispatch goes to the `__spec`.
2. **The dict singleton is never read.** `dict_Eq_Vec_singleton` appears only at
   its own definition -- no abstract `(defn f [A] [(Eq A)] ...)` consumes it in
   these fixtures.

So the carrier base + dict singleton are **dead code** in every fixture that
only does concrete Vec dispatch. clang DCEs them at `-O2` (no binary cost), but:

- the **emit-c audit** counts the 3 dead casts per fixture (~30 of the 80), and
- emitting them keeps **`emit_carrier_bridge` reachable**, which blocks the M3
  goal of down-scoping/deleting the bridge.

The remaining ~50 `Vec int` crossings are the **live** cases: the synthesized
element-comparator thunks `__fn_N(int64_t, int64_t)` for recursive
`Vec[Vec[int]]` eq (the closure/fat-comparator ABI is uniform int64, so the
thunk casts to `Vec__int *` to call the typed spec), plus the `vec-eq-loop-byval`
carrier base reached through them. Those are a separate, harder sub-problem
(typed closure carriers) -- see "Phase 2".

## Why the dead carrier base survives today

`emit_abi_fn_skip_generic` (`emit_module.c:2195`) already suppresses a generic
function's carrier body once it has been specialized and no carrier call is
observed (`!emit_abi_has_carrier_call`). It does NOT fire for the Eq[Vec]
carrier base because:

- the **dict singleton initializer** `.eq_qu = __inst_Eq_eq_qu_Vec` is a
  genuine C-level reference to the symbol, so even if the body were skipped the
  initializer would dangle (link error). The dict keeps the base alive.

Therefore the dead carrier base cannot be removed in isolation -- the **dict
singleton must be conditionally emitted too**. That is the crux of M4d.

## Phase 1 -- conditional dict + carrier-base emission (the ~30 dead crossings)

Emit the non-HKT instance's int64 carrier base **and** its `dict_X_<args>`
struct + singleton **only when the dict is actually consumed** somewhere in the
program. "Consumed" = at least one of:

- an `EX_DICT` method dispatch whose `instance` is this instance and whose
  receiver type is still abstract at the call (the genuine carrier-dispatch
  path), or
- a **bare-dict-value** use -- the singleton address handed to a constrained
  polymorphic function (`(defn f [A] [^&: Eq A] ...)`), a witness table slot
  (`witnesses[i] = &dict_X_singleton`, emit_expr.c:4656), or any
  `(int64_t)(intptr_t)(&dict_X_singleton)` materialization (emit_expr.c:1427).

When NO such consumer exists, the dict + carrier base are dead; skip both.

### Implementation sketch

1. **Dict-consumption scan** (new, in the emit_abi scan pass, `emit_module.c`):
   walk every item; for each `EX_DICT` dispatch with an abstract receiver and
   each bare-dict-value materialization, mark `inst->dict_consumed = true`
   (new `bool` on `TypeClassInstance`, default false). The two emit sites that
   take the singleton address (1427, 4656) and the abstract-dispatch path are
   the complete set -- the same three forms the M3 audit's "bare dict value"
   note enumerates.
2. **Gate the dict emit** (`emit_stmt.c:399` `EX_INSTANCE_DEF`): when
   `tc->is_hkt == false` and `!inst->dict_consumed`, emit neither the dict
   struct, the singleton, nor force-keep the carrier base. (HKT instances always
   emit -- their dispatch is inherently dict-driven, M6/M7.)
3. **Let `emit_abi_fn_skip_generic` drop the now-unreferenced carrier base.**
   With the singleton gone, the carrier base has no referent and the existing
   skip path (or a small extension recognizing instance-method carrier bases
   whose only caller was the dropped dict) elides it. Verify the forward decl is
   dropped in lockstep (emit_module.c:2390 region).

### Risk + validation for Phase 1

- **Link errors** if the consumption scan misses a consumer -> the dict is
  skipped but something references it. Mitigate by making the scan
  conservative: when in doubt (any abstract-receiver dispatch, any address-of),
  mark consumed. A missed *non*-consumer only leaves a dead dict (status quo);
  a missed consumer is a link error, so bias toward keeping.
- **Separate compilation**: an exported instance's dict may be consumed in
  another TU. Gate Phase 1 to `!ctx->separate_compilation` OR keep the dict for
  exported instances. Confirm with the spice build (ecs/json link across TUs).
- Validation: `bash tests/run.sh` green; `TUR_M3_AUDIT=1` sweep shows the
  ~30 dead-carrier-base `Vec int` crossings gone (and the same for any other
  non-HKT instance whose dict is unconsumed -- this generalizes beyond Vec, so
  expect a broader drop). Snapshot regen for any fixture whose emitted C loses a
  dead dict (could be large -- coordinate one regen window). Spice roundtrip.

Phase 1 is the **safe, high-value increment** and removes the dead crossings for
*every* non-HKT instance, not just Eq[Vec]. It does not need typed dict slots at
all -- it just stops emitting provably-dead carrier dispatch.

### Phase 1 attempt 2026-06-16 -- 84 -> 8, then reverted; the consumption model is subtler than "dict singleton scan"

A full Phase 1 was implemented (dict-consumed scan + dict-emit gate +
`emit_abi_fn_skip_generic` extension to drop the carrier-base method) and got the
**audit from 93 -> 57 crossings** (every dead non-HKT carrier base gone;
`defn-basic`'s snapshot dropped **600 lines** of unused Eq/Clone/Hash/Ord/Show
instance bodies). But the full suite went to **84 failed**, then **8 failed**
after the consumption signal was refined to the recorded specialized-call set
(`ctx->specialized_call_exprs`, exact call-expr match, checked AFTER
`emit_abi_register_call`). The change was **reverted** -- the residual 8 expose
that an instance carrier-base method is reached by MORE paths than the dict
singleton, and crucially by paths that do not exist at consumption-scan time:

1. **Primitive direct dispatch.** `Eq[cstr]` / `Hash[bool]` concrete dispatch is
   NOT monomorphized to a `__spec` (only struct/app receivers are, M4c Path A);
   it calls `__inst_Eq_eq_qu_cstr` **directly**. So the method is live even
   though its dict singleton is dead. (`cgi-constrained-generic-dispatch`.)
2. **Specialized-generic instance resolution at EMIT time.** A constrained
   generic `(defn geq [^Eq K] (eq? a b))` specialized to `K=cstr` resolves the
   inner `(eq? a b)` to `__inst_Eq_eq_qu_cstr` **during spec-body emission** --
   *after* the abi-scan that does the consumption marking has run. The
   pre-emit scan sees only the generic body's abstract `EX_DICT`, never the
   per-K resolved call, so it cannot mark the cstr/bool/Map method live.
   (`serial-return-dispatch-tyvar`, the `Map` cases.)
3. **Constrained-generic dict passing.** `(f m)` with `[(Eq A)]`, `A=Map`, passes
   `&dict_Eq_Map_singleton`; the bare-dict mark should catch it but the dispatch
   structure (dict as the call's dict_arg vs a value arg) made the scan miss it.
   (`gde-generic-dict-eq-map`, `gde4-generic-size-map`.)

**Conclusion: the dict-emit gate (dict struct + singleton) IS safe to skip on a
pure bare-dict/witness scan, but skipping the carrier-base METHOD body is not,
because method liveness is only fully known after emit** (specialized generics
resolve `__inst_X` symbols during spec emission). And the dict-only skip does
NOT reduce the audit -- the crossings live in the method body, which must stay.

So the **robust Phase 1 is a two-pass emit / emitted-symbol-reference sweep**:
emit the program once, collect the set of `__inst_*` and `dict_*_singleton`
symbols actually *referenced* (excluding their own definitions), then re-emit
(or post-process) dropping unreferenced static definitions. That is a
dead-static-definition DCE over the emitted C -- definitively correct (it
matches exactly what clang would DCE at -O2) and immune to all three paths
above, because it observes the *final* references rather than predicting them.
It is a larger change than the single-pass scan this plan first sketched;
estimate it as its own increment. The single-pass receiver-concreteness /
specialized-call heuristics are a dead end (they cannot see emit-time
specialized-generic resolution).

Alternative narrower scope if the two-pass emit is too invasive: restrict the
method skip to instances whose type arg is a **parametric struct/app that has a
minted per-instantiation `__spec`** (proving concrete dispatch bypasses the
carrier base) AND whose dict is bare-dict/witness-dead AND that no constrained
generic specializes over (path 2). The `Map`-via-generic-dict case shows even
struct/app instances can be live, so this still needs path-2 coverage -- i.e.
it does not avoid the core difficulty. The two-pass emit is the clean answer.

### Phase 1 attempt #2 2026-06-16 -- pre-emit spec-constraint liveness ALSO insufficient; two-pass emit confirmed as the only robust route

A second pre-emit attempt added a **spec-constraint liveness** check on top of
the bare-dict/witness + specialized-dispatch marking: keep an instance when some
interned ABI spec is a constrained generic (`(defn f [^Class K] ...)`) whose fn
constrains the instance's class AND some resolved type in the spec matches the
instance's head. This was meant to cover path 2 (the emit-time specialized-
generic resolution) by reading it off the interned spec table rather than
predicting it.

It got **further** -- `cgi-constrained-generic-dispatch` and
`serial-return-dispatch-tyvar` passed (the `geq [^Eq K]`->cstr / `ghash`->bool
cases ARE visible as specs binding cstr/bool under an Eq/Hash constraint). But it
**still failed** on `gde-generic-dict-eq-map` (`__inst_Eq_eq_qu_Map`),
`gde4-generic-size-map`, `cloneable-drop-rc` (`__inst_Clone_clone_int`),
`union-types-typeclass-dispatch`. Root cause, confirmed by probe: `eq2 [^Eq A]`
called on a `(Map cstr int)` specializes to a **carrier spec** -- `A` binds to
the int64 carrier, NOT to a concrete `Map` head -- because `Map` is a `:heap`/
carrier type. So the spec's resolved types do NOT contain a `Map`-headed type the
liveness check can match, yet the spec body still emits `__inst_Eq_eq_qu_Map`
during emission. The instance is resolved at emit time with no concrete trace in
the spec table.

**Definitive conclusion (after two pre-emit attempts): no pre-emit scan can be
both safe and useful here.** Any constrained generic specialized to a carrier
spec resolves the instance method at emit time with no concrete binding visible
before emission. Over-approximating to cover it (keep every instance whose class
is constrained by ANY carrier spec) collapses to "keep almost everything" and
loses the reduction. The **two-pass / post-emit emitted-symbol-reference DCE is
the only robust route** and should be implemented as follows:

1. Route each instance-method carrier-base **definition + forward decl** and each
   `dict_X` **struct + singleton** into a side buffer keyed by the symbol name
   (instead of straight into the file buffer). Everything else (main, ordinary
   fns, ALL spec bodies, the dict-consuming code) emits to the file buffer as
   today.
2. After all emission, compute the live set by a fixpoint reference scan:
   - seed: every `__inst_*` / `dict_*_singleton` token that appears in the file
     buffer (the non-instance/non-dict output) -> live;
   - propagate: a live `dict_X_singleton`'s initializer references its methods ->
     those `__inst_*` become live; a live instance-method body may reference
     other instances -> live; iterate to fixpoint.
3. Append only the live side-buffer definitions (decl + def) to the file, in the
   original order, dropping the dead ones.

This observes the FINAL references, so it is immune to all of path 1/2/3 and to
the carrier-spec case above -- it matches exactly what clang DCEs at -O2, but at
the source level so the emit-c audit and compile time benefit too. The plumbing
(intercepting instance-method + dict emission into keyed side buffers, then the
fixpoint splice) is the real work; budget it as a focused increment. Both
single-pass attempts are reverted; the tree is at the green Vec-producer-slice
baseline.

## Phase 2 -- typed element-comparator thunks (the ~50 live crossings)

The recursive `Vec[Vec[int]]` eq synthesizes an element comparator
`__fn_N(int64_t a, int64_t b)` that casts `a`/`b` to `Vec__int *` to call the
inner `Eq[Vec]` spec. The cast is forced because the comparator is passed
through the **uniform int64 fat-closure / `^fat val-cmp` ABI** that
`vec-eq-loop-byval` / `vec_hyeq_qu` consume.

Options (a design sub-pass, analogous to the parent plan's M6):

- **2a. Typed comparator parameter.** Give the byval eq helpers a typed
  comparator param (`bool (*)(Vec__int *, Vec__int *)`) per element
  monomorphization, so the synthesized thunk is typed and the cast disappears.
  Rides the existing per-spec machinery; the helper's comparator slot becomes
  part of its spec signature.
- **2b. Accept the thunk cast as the type-erased boundary.** The comparator IS a
  first-class function value crossing a uniform-ABI boundary; per the matrix's
  roadblock 4 the carrier survives at genuine type-erasure. If 2a's per-element
  comparator-typed helper specialization proves too broad, classify these as the
  permanent carrier boundary and down-scope (not delete) the bridge around them.

Default: try 2a for the `Eq`-family helpers (the audit's whole live residual is
Eq-comparator thunks); fall back to 2b if the comparator-typed spec explosion is
worse than the win.

## Phase 3 -- re-audit + bridge down-scope

After Phases 1-2, re-run the `TUR_M3_AUDIT=1` sweep. Target: `Vec int` crossings
-> near 0 (modulo any 2b-classified permanent comparator boundary). At that
point the only `emit_carrier_bridge` callers are the matrix's documented
type-erased boundaries (existential / `@Any` / `tur_poly_fn_t` / blessed
inline-C `tur_ok`), and the bridge can be **down-scoped** (not deleted -- matrix
roadblock 4) to exactly those, per the M3 sequencing.

## Sequencing note

Phase 1 is independent and shippable on its own (it is dead-code elimination, no
ABI change, no typed dict slots). Do it first -- it is the larger crossing win
(generalizes to all non-HKT instances) at the lower risk. Phase 2 is the genuine
ABI change and should follow once Phase 1 quantifies how many live crossings
actually remain.

## Validation harness (all phases)

1. `bash tests/run.sh`: zero new `FAIL`; one coordinated snapshot regen per
   phase that changes shared codegen (Phase 1 may drop dead dicts from many
   snapshots).
2. `bash tests/run-turi.sh`: interpreter parity baseline (1206/2).
3. `TUR_M3_AUDIT=1` per-fixture sweep tracking the `Vec int` count down.
4. Spice roundtrip `../turmeric-spices/spices/{ecs,json}` (ecs is the heavy Vec
   + dispatch user; the cross-TU dict-consumption case lives here).

## Related

- [docs/reported/m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md](../../reported/m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md)
  -- the audit + bucket breakdown this plan's root-2 owns.
- [docs/upcoming/m4-typeclass-per-method-abi-plan.md](../m4-typeclass-per-method-abi-plan.md)
  / [docs/upcoming/m4c-execution-plan.md](../m4c-execution-plan.md) -- M4a-c,
  the per-instantiation typed spec work this builds on.
- [docs/upcoming/v1/vec-typed-pointer-vertical-slice-plan.md](vec-typed-pointer-vertical-slice-plan.md)
  -- the Vec producer slice (root 1) this is the sequel to.
