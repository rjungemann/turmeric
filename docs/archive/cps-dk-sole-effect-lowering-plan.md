---
title: "v2 -- CPS/DK as the sole effect lowering; delete the fiber effect runtime"
status: COMPLETE (2026-07-19) -- the fiber effect runtime is deleted; CPS/DK is the sole effect lowering.
severity: existential. (Resolved GO -- the deletion was achieved, not proven impossible.)
resolved: 2026-07-19
---

# v2 -- Make CPS/DK the sole lowering for effectful colored code, then delete the fiber effect runtime

> **RESOLVED -- GOAL ACHIEVED (2026-07-19).** The singular, non-negotiable goal
> below -- *no effect ever performed or handled on the fiber runtime; the fiber
> effect machinery DELETED* -- is DONE. The `cps-tramp-resume` experiment (E7's
> trampolined tail-resume, the load-bearing enabler surfaced by the Sec 9
> kill-probe) GRADUATED to always-on (`src/runtime/experiments.c` GRADUATED[]).
> The measured remaining surface (the 24-fixture B1-B8 work-list in the tactical
> companion `cps-dk-endgame-remaining-plan.md`) was driven to zero, and Stage G
> physically deleted `tur_effect_perform`, `EffectHandlerFrame`/`Case`,
> `global_effect_handler_chain`, `tur_handler_dispatch` (+ msdyn),
> `tur_effect_cont_*`, and `TurEffectCaptureCtx` from `emit_module.c` (commits
> 7b40fc4e / d46f74bef). A full-corpus sweep of emitted C confirms zero call
> sites of any fiber effect symbol. `FiberBlock`/scheduler/reactor (concurrency),
> `tur_handler_table_t`, and `tur_cloneable_cont_*` (the DK handler-value +
> `__Shift` bridge) STAY, as designed. The two-runtime split is closed; the CPS/DK
> delimited-control substrate is the sole effect lowering. This plan is fully
> discharged and ready to archive. Residual owning-value teardown leaks are
> pre-existing and tracked separately in `docs/reported/`; they are not this
> plan's blockers.

> **UPDATE 2026-07-18 -- measured remaining surface.** The concrete, current
> work-list lives in **`cps-dk-endgame-remaining-plan.md`** (tactical companion).
> Measured precisely (count of `tur_effect_perform("` call sites under
> `--enable=cps-tramp-resume`, NOT the over/under-counting `eff=1` column): the 22
> opted-in `cps-tramp-resume-*` fixtures are 100% fiber-clean, and **24
> non-opted-in fixtures** remain fiber-bound, bucketed B1-B8 (20 fixable, 4
> permanent-candidate carve-outs). The `SIG-*` inventory in Sec 1/2 below is the
> original strategic framing; the companion doc supersedes it for day-to-day
> execution and states the measurement methodology explicitly.

## 0. Why this document exists

The v1 finish plan (`../cps-runtime-finish-plan.md`) drove the CPS/DK backend to
the point where **`TUR_TRACE_EVICT` shows only `SIG-*`** across the corpus
(`BODY-UNSUPPORTED = 0`, `BODY-STRUCT-OR-TAINT = 0`, suite 2179/0) and that gate
is now **build-time enforced** (Slice PZ: a colored non-`SIG-*` fallback is a hard
error). But v1 stopped at an honest wall: the direct/**fiber** effect runtime is
still emitted and still runs every `SIG-*` colored function, because those
functions are kept on the direct emitter *by design*.

This plan removes that wall. The goal is singular and non-negotiable:

> **No effect is ever performed or handled on the fiber runtime. The emitted fiber
> effect machinery (`tur_effect_perform`, `EffectHandlerFrame`,
> `global_effect_handler_chain`, `tur_handler_dispatch`, the `emit_effects.c`
> direct perform/handle/resume emitters) is DELETED. All effectful colored code
> lowers through the CPS/DK backend.**

This is existential: if it cannot be done, the two-runtime split is permanent and
the project's core premise (a single stackless delimited-control substrate) fails.
So this document also states, honestly, the **kill criteria** (Sec 9) -- the specific
points at which we would conclude it is impossible.

Everything below is grounded in a full inventory of the two runtimes and the
calling-convention gap between them (the file:line evidence in Sec 2 and Sec 3).

---

## 1. Current state (the starting line)

- CPS/DK backend is the sole lowering for every colored function whose body is in
  the admissible subset. That subset now covers the whole fixture corpus:
  `BODY-* = 0`.
- A colored function still evicts to the direct/fiber emitter only for a
  **permanent signature reason** -- the `SIG-*` set:

  | Category | What it is | Distinct fns (corpus) | Touches the fiber EFFECT runtime? |
  |---|---|---|---|
  | `SIG-REJECT` | non-scalar `__cps` signature (ADT/struct/fn-value params or return) -- collides with the carrier/dict ABI | ~414 (mostly `__fn_*` lambdas + `option-eq?`/`*-eq-driver` shapes) | **Only if it performs/handles an effect.** Most are pure fn-value appliers (comparators/mappers). |
  | `SIG-EXPORT` | `c_export_name` pinned (typeclass `__inst_*` methods reached via a dict slot) | 26 | **Only the effectful instances.** Most are pure (`fmap`, `bind`, `eq?`). |
  | `SIG-MAIN` | program entry `int main(...)` ABI (non-`d2b` mains) | 1 (every fixture's `main`) | **Yes** -- effectful mains install handlers on `global_effect_handler_chain`. |
  | `SIG-TAINT` | shares an effect with a permanent `sig_perm` fn (derivative) | 67 | **Yes, by definition** -- it is the cascade that keeps effectful peers together on fiber. |
  | `SIG-INLINE-C` | opaque inline-C body the CPS backend can't thread a DK through | 1 (the session-effect mains) | Body is opaque C; performs no *Turmeric* effect. |

- The DK runtime and the fiber runtime are **cleanly separated at the C level**:
  no emitted DK C references `global_effect_handler_chain` / `tur_current_fiber` /
  `tur_effect_perform` (the only hit is a comment, `emit_cps_ir.c:2697`). This is
  the single most important fact enabling the deletion: **the DK effect path does
  not depend on the fiber effect path.**

### The reframing that makes this tractable

The deletion target is the fiber **EFFECT** runtime, not the fiber runtime wholesale.
`FiberBlock` and the scheduler/reactor/futures (concurrency: `spawn`/`async`) are a
**separate axis** that also uses `FiberBlock*` and is out of scope here. So a
colored function only blocks the deletion if **an effect is performed or handled
on the fiber runtime somewhere in its dynamic extent.** A *pure* colored function
(colored only because it applies a fn-value -- `option-eq?`, `fmap`, most `__fn_*`
lambdas) never touches the fiber effect runtime and can keep direct-emitting
harmlessly. This narrows the job:

> **We do NOT need every colored function to CPS-emit. We need every EFFECT to be
> performed/handled on the DK, never on a fiber.**

That distinction (proved in Sec 4) is what turns an intractable "rewrite the entire
value/closure ABI" problem into a bounded, ordered migration.

---

## 2. The deletion target -- what dies, what stays (grounded inventory)

The emitted fiber effect runtime lives entirely inside
`emit_runtime_preamble` in **`src/compiler/emit_module.c:6191`** (emitted
**unconditionally** -- there is no `cps_uses_*` gate; it dies by becoming
unreachable, not by flipping a flag).

### 2a. DELETE (once no effect runs on a fiber)

Emitted C (all in `emit_module.c` unless noted):

- `struct TurEffectCaptureCtx` (`:7276`), `TurContK` (`:7273`),
  `struct EffectHandlerCase` (`:7289`), `struct EffectHandlerFrame` (`:7295`).
- `__thread EffectHandlerFrame *global_effect_handler_chain` (`:7467`).
- `tur_effect_perform` (`:8500-8534`) -- the core fiber `perform`.
- `tur_handler_dispatch` + `__tur_msdyn_cont`/`__tur_msdyn_clone` +
  `struct __tur_msdyn_env` (`:8549-8607+`) -- first-class handler dispatch loop.
- `tur_effect_cont_resume` (`:7512`), `tur_effect_cont_valid` (`:7518`).
- `tur_handler_table_t` / `tur_handler_entry_t` + `_new/_concat/_free` (`:6414-6440`).
- The fiber's effect-specific FIELDS only: `FiberBlock.effect_handler_chain`
  (`:7320`) and `FiberBlock.eff_ctx` (`:7330`), plus the copy at
  `tur_fiber_block_new:7478`.

The direct/fiber effect **emitters** (`src/compiler/emit_effects.c`):

- `emit_effects_perform` (`:91-152`), `emit_effects_handle` (`:158-852`),
  `emit_effects_resume` (`:1213-1304`), `emit_effects_discontinue` (`:1306-1324`),
  `emit_effects_handler_lit` (`:858`), `emit_effects_with_handler` (`:1015`),
  `emit_effects_compose_handlers` (`:1001`).
- Their dispatch from `emit_value` (`emit_expr.c:6746-6752`).

### 2b. STAYS (do NOT touch)

- The whole DK runtime: `emit_dk_runtime.c` (`dk_run`, `dk_shift`, `dk_perform`,
  `dk_handler`, prompts, the reaper). This is the surviving substrate.
- `struct FiberBlock` (minus the two effect fields) + `tur_fiber_block_new/
  resume/yield/free` + the scheduler/reactor/futures (`emit_module.c:7613-8225`) --
  **concurrency**, a separate deletion axis.
- `tur_cloneable_cont` + `tur_cloneable_cont_alloc/resume/clone/drop` +
  `tur_continuation_snapshot` (`emit_module.c:7156-7212`, gated on
  `cps_uses_cloneable_rt`). Used by BOTH the fiber multishot path (dying) AND the
  DK `__Shift` bridge (`__dk_cont_fn`/`__dk_env_clone`/`__dk_env_drop`,
  `emit_dk_runtime.c:48-62`). It **stays** -- the DK bridge still needs it.
- The delimited-control emitters in `emit_effects.c` (`emit_effects_reset`
  `:1330`, shift/serial/cloneable) -- already DK-lowered or near-empty
  passthroughs post-D3; not part of the fiber effect runtime.
- `ctx->pending_handler_fns` file-scope ordering buffer (`emit_fns.c:2756-2782`,
  `:3978`) -- shared by the DK cloneable-reset emitter; stays.

### 2c. The decisive gate

`emit_cps_ir_try_fn` (`emit_fns.c:2624`, def `emit_cps_ir.c:5057`): returns true =
DK lowering; false = fall through to the direct/fiber emitter. **The fiber effect
runtime is dead exactly when no effectful colored function makes this return
false** -- *except the session/thread subsystem carve-out* (W5 correction): the
`session-effects` / `session-mp-effects` mains are permanent fiber clients whose
inline-C pthread/session-channel body cannot thread a DK, so they legitimately
return false while handling `SessionLog` / `MpLog`. The gate's target is "no
effect on a fiber **via the DK-lowerable path**"; the thread/session-channel
runtime is a separate subsystem the deletion does not target. The deletion is
safe when that holds for every effectful colored function that is not a member of
that carve-out.

---

## 3. The unifying blocker -- the carrier/DK ABI gap

Both runtimes are cleanly separated, but the *bridge* between DK-native code and
the fn-value / dict / indirect-call world is missing a channel for the
continuation. Every fn-value invocation speaks a uniform
`{env-or-void*, int64 carrier}` ABI with **no `DK *` parameter**:

- **Fat closure box** (`emit_fns.c:2829`): `struct <env> { int64_t __fn; captures... }`;
  the value is the env pointer; slot 0 is the thunk `int64_t(void*, int64...)`.
- **`tur_poly_fn_t`** (`emit_module.c:6386`): `{ void *env; int64_t (*fn)(void*, int64_t); }`.
- **Dict slot** (`emit_stmt.c:581`, dispatch `emit_core.c:1962`): each method is a
  carrier-ABI fn-ptr; the MB1 fallback casts **every** param to `int64_t`
  (`emit_core.c:1984`).
- **Indirect call emission** (`emit_expr.c:3058-3448`): casts the callee value to
  a bare `R(*)(argsig)` or `fn.fn(fn.env, args...)` -- **no trailing `DK*`**.

Consequences (each a `SIG-*` root cause, with the exact mechanism):

1. **`sig_slot_ok` collision** (`emit_cps_ir.c:195-215`). A colored fn emits
   `f__cps(<concrete>, DK*)` + wrapper `f(<concrete>)`. If the *same symbol* is
   also reached via the carrier/dict ABI (`_Bool(int64_t,int64_t)`), the concrete
   re-emission is a `conflicting types` C error. So `fn_sig_ok` admits **scalar
   signatures only** -> non-scalar-signature colored fns SIG-REJECT.
2. **`__inst_*` pinned to the carrier ABI** via `c_export_name`
   (`elab_typeclasses.c:3789`) -> SIG-EXPORT; the mono path enforces the same
   (`mono_sig_ok` `is_inst` scalar gate, `emit_cps_ir.c:5003`).
3. **Indirect calls can't thread the DK** (`emit_cps_ir.c:2407-2425`,
   `indirect_callee_ok:740`). The callee's C signature has no `DK*` slot, and a
   capturing-closure callee would segfault the direct emitter's bare-pointer cast,
   so the whole subterm delegates to the fiber emitter via `CT_LETRAW`. An effect
   performed *through* such a call therefore runs on the fiber runtime.

The single unifying fix is to **give the continuation a channel through the
carrier ABI**: a carrier-shaped `__cps` entry (`int64_t f__cps(int64_t..., DK*)`)
for functions, and a `DK*`-carrying thunk variant for effectful fn-values/dicts.

---

## 4. Which effects actually run on a fiber (the real work-list)

An effect runs on a fiber iff, in some dynamic extent, one of these direct/fiber
emitters fires: `emit_effects_perform` / `emit_effects_handle` / `_resume`. That
happens only inside a **direct-emitted** function body. A colored function is
direct-emitted only when it is `SIG-*`. So the effectful work-list is:

- **(W1) Direct-effectful `SIG-REJECT` / `SIG-EXPORT`.** A colored fn with a
  non-scalar signature (or a dict-pinned `__inst_*`) that itself performs/handles
  an effect. Cleared by **E1** (carrier-ABI `__cps` emission removes the SIG-REJECT
  and the SIG-EXPORT collision, so it CPS-emits and its perform/handle DK-lowers).
- **(W2) `SIG-MAIN`.** Every effectful `main` installs a handler on the fiber
  chain. Cleared by **E3** (all mains get the fixed-ABI DK entry wrapper).
- **(W3) Effect reached through a fn-value / indirect call.** A `perform` inside a
  lambda/closure/dict-method that is invoked indirectly runs on the fiber runtime
  because the indirect call can't thread the DK. This is the effectful-fn-value
  cluster (v1 Slices PL-PQ pinned it permanently fiber = SIG-TAINT). Cleared by
  **E2** (a `DK*`-threading thunk variant for effectful fn-values).
- **(W4) `SIG-TAINT` cascade.** Derivative: a fn pulled to fiber only because it
  shares an effect with W1/W2/W3. Dissolves automatically once W1-W3 clear
  (nothing seeds `g_perm` -> the taint fixpoint empties).
- **(W5) `SIG-INLINE-C`.** An inline-C body performs no *Turmeric* effect (opaque
  C, no `perform`), so it never fires the fiber emitters. It is already a leaf --
  reclassify it as one (**E5**); no runtime effect migration needed.

  **Correction (session-effects / session-mp-effects are NOT covered by E3).**
  The two SIG-INLINE-C corpus instances (`session-effects`, `session-mp-effects`
  mains) are the exception this row originally glossed as "also mains -> covered
  by E3." They are NOT: each `main` HANDLES a real algebraic effect
  (`SessionLog` / `MpLog`) that IS performed on the fiber, and its inline-C is
  **load-bearing** -- raw `pthread` `spawn`/`join` + inline-C session/channel
  primitives -- not an opaque no-effect leaf. E3's fixed-ABI DK entry wrapper
  cannot help: the CPS backend can never thread a DK continuation through that
  inline-C, so `main` stays SIG-INLINE-C and permanently taints the effect
  (the session/role param SIG-REJECTs, and would only reclassify to SIG-TAINT if
  admitted -- a net-zero widening). These two are a **separate concurrency
  subsystem** (OS threads + inline-C session channels), NOT the
  delimited-continuation DK effect machine this plan deletes, so they are
  **permanent fiber clients, explicitly out of scope for the deletion**: "delete
  the fiber effect runtime" means "delete the DK effect machine's fiber path,"
  which the thread/session-channel runtime is not part of. Analysis + decision:
  [docs/archive/cps-session-effects-permanently-fiber-bound.md](../../archive/cps-session-effects-permanently-fiber-bound.md).

**Pure indirect-call-colored functions are NOT on this list.** `option-eq?`,
`fmap`, `bind`, a comparator `__fn_*` -- colored only because they apply a
fn-value, performing no effect -- never fire the fiber emitters. They can stay
`SIG-REJECT`/`SIG-EXPORT` direct-emitted forever without keeping one byte of the
fiber effect runtime alive. **They are explicitly out of scope for the deletion.**

This is the crux insight: **the deletion needs E1+E2+E3+E5 (+ automatic W4), NOT a
wholesale fn-value ABI rewrite.**

---

## 5. The enablers

### E1 -- Carrier-ABI `__cps` emission for non-scalar-signature colored functions

**Problem:** `f__cps(<concrete params>, DK*)` collides with the same symbol's
carrier/dict specialization. **Fix:** for a colored function that must interoperate
with the carrier/dict ABI (non-scalar signature, or dict-reached), emit its CPS
variant under the **uniform carrier ABI**:

```
int64_t f__cps(int64_t p0, int64_t p1, ..., DK *__kont);   // carrier params
```

with unbox-at-entry / box-at-exit glue (the direct emitter already owns the
carrier<->concrete bridge -- reuse `slot_load`/`slot_store` + the existing
by-value/heap-handle carriers). The direct-entry wrapper `f(...)` likewise emits
under the carrier ABI so it matches how dict/fn-value callers invoke `f`. No
concrete signature is ever emitted for these symbols, so there is no collision.

- Widen `fn_sig_ok`/`sig_slot_ok` to admit a non-scalar signature **when the CPS
  emission uses the carrier ABI** (a new emission mode, not the concrete mode).
- Relax `mono_sig_ok`'s `is_inst` gate correspondingly: a typeclass instance
  method's `__cps` clone is emitted under the carrier ABI, matching the dict slot.
- Clears W1. Combined with E4 (below) also clears the SIG-EXPORT collision.

**Risk:** medium-high. Touches the signature gate, the wrapper emitter, and every
carrier<->concrete crossing at the boundary. Bounded by: the carrier bridge already
exists (the direct emitter uses it), and the change is additive (a second emission
mode). Verifiable incrementally per fixture.

### E2 -- `DK*`-threading thunk for EFFECTFUL fn-values (the hard one)

**Problem:** an effect performed inside an indirectly-called fn-value runs on the
fiber runtime because the fn-value ABI has no `DK*` slot. **Fix:** give an
**effectful** fn-value (non-empty declared effect row) a second thunk entry that
threads the continuation:

```
int64_t (*fn_cps)(void *env, int64_t a0, ..., DK *__kont);   // added slot
```

- Fat-closure box (`emit_fns.c:2829`): emit BOTH `__fn` (carrier, for pure/legacy
  callers) and `__fn_cps` (carrier + DK) when the closure's effect row is
  non-empty. The DK backend, at an indirect call to an effectful fn-value, reads
  `__fn_cps` and threads `__kont`; the perform inside reaches the caller's DK
  prompt.
- `tur_poly_fn_t`: add an `fn_cps` slot (or a parallel `tur_poly_fn_cps_t`).
- Dict slots for effectful methods: add a `__cps` method pointer.
- Indirect-call emission in the CT-IR (`emit_cps_ir.c:2407`): admit an effectful
  fn-value callee as a native `CT_TAILCALL`/`CT_LETCALL` that threads `__kont`
  through `__fn_cps`, instead of `CT_LETRAW`-delegating.
- **Scope reducer:** only fn-values whose effect row is non-empty need the cps
  thunk. Pure fn-values keep the single-slot carrier ABI unchanged. Effect-row
  info is already on `Closure`/`CtorField.effect_row` and the fn type
  (`expr_fn_effect_row`, v1 Slice PL).

Clears W3, and is what finally lets the effectful-fn-value cluster (v1's dominant
SIG-TAINT wall) go DK-native.

**Risk:** HIGH. This is a foundational ABI addition to every effectful fn-value
channel (closure box, poly box, dict slot, indirect-call site, and `EX_FN_TO_FAT`/
`__tur_poly_to_fat` shims must forward the cps slot). It is the single most likely
place for this plan to fail; Sec 9 makes it the primary kill-criterion probe.

### E3 -- Every `main` gets the fixed-ABI DK entry wrapper

**Problem:** a non-`d2b` (effect-only, no delimited control) `main` is excluded
from CPS emission "because CPS-emitting it is pure overhead" (`fn_is_d2b_main`,
`emit_cps_ir.c:2226`). For the deletion it MUST CPS-emit so its handlers install on
the DK, not the fiber chain. **Fix:** drop the `cps_expr_contains_shift` condition
in `fn_is_d2b_main` -- every zero-arg `main` gets the `int main(...)` trampoline
into `main__cps` (`emit_cps_ir.c:5240`, already implemented for the delimited
case). An effect-only main's `handle` then DK-lowers like any other.

**Risk:** low-medium. The wrapper already exists; the change is widening its
applicability. Watch for effect-only mains whose bodies were only ever fiber-tested.

### E4 -- Exported colored symbols emit under the carrier ABI

With E1, an exported colored fn's `f__cps` + wrapper already use the carrier ABI,
which is exactly the `c_export_name` export contract. Remove the blanket SIG-EXPORT
exclusion (`emit_cps_ir.c:5113`) for a colored export **once its signature is
carrier-ABI-emittable**; the pinned C name becomes the carrier-ABI wrapper.
Effect-free exports (most `__inst_*`) may stay direct (out of scope, Sec 4); the
target is effectful exported/dict-reached methods.

**Risk:** medium. Coupled to E1; verify no dict-slot signature mismatch.

### E5 -- inline-C colored bodies are leaves, not fiber code

An inline-C body performs no Turmeric effect. Reclassify a colored fn whose body's
only residual is inline-C as a **leaf** the DK backend calls via `cps->direct`
(no `__kont` needed) rather than a SIG-* eviction that keeps the fiber runtime
nominally reachable. In practice the corpus instances are mains (covered by E3);
this is mostly a classification tidy-up so no SIG-INLINE-C remains after E3.

**Risk:** low.

### E6 -- SIG-TAINT dissolves (verification, not work)

`SIG-TAINT` seeds from `g_perm`, which is fed by the sig_perm sources (export /
main / reject / inline-c / whole-body-delegation base taint). Once E1-E5 remove
every effectful sig_perm source, the permanent-taint fixpoint has no seed and
`SIG-TAINT` empties. This needs no code -- it is the measurable signal that W1-W3
are done. If SIG-TAINT does not empty after E1-E5, an effectful source was missed:
`TUR_TRACE_EVICT` + the `g_perm` seed dump name it.

### E7 -- Trampolined tail-resume (NEW; surfaced by the Sec 9 kill-probe)

**The kill-probe (Sec 9) revealed a wrong assumption in this plan's framing.** The
DK model is *not* already stackless for resumptive recursion. The current
`dk_perform` runs a resume by calling `H->handler(...)` which calls `dk_run(sub, v)`
**inline** -- so a resumed continuation that performs again nests a fresh
`dk_perform` -> `handler` -> `dk_run` chain on the C stack. Measured cost:
**~160 bytes of C stack per resumed perform** (`docs/upcoming/v2/probes/e2-killprobe.out`),
i.e. ~153 MB at 1e6 -- a guaranteed SIGSEGV. This is *exactly why* the compiler's
`perform_cont_reset_ok` (`emit_cps_ir.c:1398`) **rejects** a perform-continuation
that ends in a tail call and evicts it to the fiber emitter ("would recurse
unboundedly through dk_invoke -- an O(N) resume stack"). The fiber runtime is
today the *only* thing keeping deep effectful tail-recursion flat.

**Consequence:** deleting the fiber effect runtime without first making the DK
resume flat would REGRESS every deep effectful tail-recursive program from
"works" to "stack overflow." So a trampolined tail-resume is a **hard
prerequisite** for the deletion, not an optional nicety.

**Fix (proven feasible by the kill-probe, `e2-killprobe.c` version B):** for a
**tail-resume** handler case (body ends in `(resume k <expr>)`), `dk_perform`
must NOT call `dk_run` inline. Instead it unwinds to a driver loop (the outermost
direct->cps entry, `dk_run_root`) carrying `(resumed_chain, resume_value)`; the
driver re-enters `dk_run` on the resumed chain from the top. C-stack depth then
stays bounded by a single inter-perform slice (**measured 263 bytes, constant, at
N=1e6**), not by the recursion length. This composes with the E2 fn-value `DK*`
threading with **no per-call prompt** and does not disturb the handler search.

- Tail-resume (`(Eff [x] k) ... (resume k e)`): trampolinable, flat. This is the
  deep-recursion case and the one that matters.
- Abortive (no `resume`): trivial -- return the handler value, never yields.
- Non-tail resume (work after `resume` returns): the post-resume work is a bounded
  DK frame that rides on the resumed chain, so it stays flat too; needs care but
  is structurally the same unwind-to-driver.
- Multishot (`resume` called >1x): out of the flatness target; keeps the existing
  `dk_invoke` copy-and-run (bounded by the multishot fan-out, not the recursion).

**Sequencing:** E7 lands as **Stage 0**, before Stage E's deep-recursion migration
and before the Sec 8 stackless-sign-off fixture. Stages A-D (mains, inline-C,
carrier-ABI, exports) do not depend on it as long as the bodies they move are not
1e6-deep resumptive; E7 is what lets the tail-recursive effectful cluster leave
fiber without regressing, and what the Stage-G deletion rests on.

**Risk:** HIGH -- it changes the core resume mechanism in `dk_perform`. Bounded by:
the driver boundary already exists (`dk_run_root` / the reap boundary), the tail-
resume shape is already recognized by the classifier (it just evicts today), and
the kill-probe demonstrates the exact unwind-and-re-enter structure end to end.

#### E7 validated algorithm (full-fidelity probe, `probes/e7-fidelity-probe.c`)

A second probe reproduced the ACTUAL `emit_handle` chain layout
(`HANDLER -> FRAME(kname handle-continuation) -> enclosing handlers`), a deep
tail-resume loop through the fn-value `__fn_cps` slot, AND an enclosing handle for
a different effect. It converged on the correct algorithm only after two wrong
turns that a naive implementation would also hit -- both now documented so the
runtime port avoids them:

1. **A naive trampoline must NOT flatten by appending `H->next` onto the resumed
   chain.** That loses delivery ORDER: with an enclosing handle, the outermost
   handle-continuation and the nested deliveries have to run in LIFO (nesting)
   order. Flattening ran the enclosing continuation zero times (value right by
   luck, delivery wrong).
2. **The fix is a heap meta-stack of pending deliveries.** On a tail-resume,
   `dk_perform` pushes the handler's `H->next` (what the inline
   `dk_run_impl(H->next, r)` would run) onto a LIFO heap stack and yields the
   resumed chain to the driver. The driver runs the chain to `DONE`, then pops and
   runs the top delivery on the result, repeating -- preserving nesting order with
   the C stack flat. A delivery that is only `HANDLER`/`DONE` nodes is a **no-op**
   (it runs after a `DONE`, can perform nothing) and is **elided**, which keeps the
   meta-stack O(nesting) instead of O(N).
3. **The perform-continuation of a tail-recursive effectful loop must be a
   `DKK_RESUME_FRAME`, not a plain `DKK_FRAME`.** A `RESUME_FRAME` receives its
   run-time downstream chain `rest` (the reinstalled-handler tail) and threads THAT
   into the recursion, so subsequent performs find the REINSTALLED handler (whose
   `H->next` is a no-op) rather than the original handle chain. Without this, every
   iteration re-finds the original handler and re-runs the real handle-continuation
   (measured: `kframe` ran N+1 times). This node kind and threading already exist
   in the runtime and in `emit_perform`'s Track A (`LH_RESUME_CONT`); E7 extends
   their use to the tail-recursive-loop shape the classifier currently evicts.

Full-fidelity result at N=1e6: **max C stack 152 bytes (flat), handle-continuation
runs exactly once, enclosing handle catches the propagated effect, side effects
correct, meta-stack high-water = 1** (`probes/e7-fidelity-probe.out`). The runtime
port is: (a) a meta-stack + trampoline in `dk_perform`/`dk_run_root`
(`emit_module.c` / `emit_dk_runtime.c`), guarded so non-tail/multishot/abort keep
today's inline path byte-identical; (b) relax `perform_cont_reset_ok`
(`emit_cps_ir.c:1398`) to ADMIT the tail-call-in-perform-continuation shape,
emitting it as a `RESUME_FRAME`, now that the runtime keeps it flat.

---

## 6. Staged execution (ordered; each stage independently verifiable, suite-green)

Preconditions carried from v1: `BODY-* = 0` and the hard-error gate (PZ) are live,
so every stage is protected -- a regression that pushes a function off the DK path
in the shipping config fails the build.

- **Stage 0 -- E7 (trampolined tail-resume).** The prerequisite the kill-probe
  surfaced. Make `dk_perform` unwind a tail-resume to the `dk_run_root` driver
  instead of resuming inline, so deep effectful tail-recursion is flat *before* any
  effectful body leaves the fiber runtime. Gate: a new 1e6-deep effectful-loop
  fixture runs flat (no SIGSEGV) through the DK path; suite green; ASan clean. This
  is the load-bearing stage -- if it cannot be made suite-green, revisit E7's risk
  note before proceeding to A-G.
- **Stage A -- E3 (all mains).** Smallest, self-contained, high signal (clears W2,
  shrinks SIG-MAIN to 0). Gate: SIG-MAIN = 0 in the corpus scan; suite green.
- **Stage B -- E5 (inline-C leaves).** Classification tidy-up; SIG-INLINE-C = 0.
- **Stage C -- E1 (carrier-ABI `__cps`), incrementally.** Land the carrier-ABI
  emission mode behind the existing per-fn admission, one signature shape at a
  time (by-value flat product -> heap handle -> fn-value param -> combinations),
  regenerating snapshots per shape. Gate after each: effectful SIG-REJECT count
  strictly decreases; suite green; ASan clean. This is where most fixtures'
  codegen moves -- expect large snapshot regens (coordinate per the CLAUDE.md
  fixture-churn rule).
- **Stage D -- E4 (exports).** Ride on E1; remove the SIG-EXPORT exclusion for
  effectful carrier-ABI-emittable exports.
- **Stage E -- E2 (effectful fn-value DK thunk).** THE hard stage. Land in
  sub-slices: (1) the ABI addition (extra `__fn_cps` slot, poly slot, dict slot,
  shims forward it) with pure code still using the old slot -- no behavior change,
  verify byte-identical; (2) the CT-IR indirect-call admission threading `__kont`
  for an effectful fn-value; (3) migrate the effectful-fn-value cluster fixtures
  (map-list-with-effectful-callback, parser combinators over an effect, the
  handler-installer HOFs from PL-PQ). Gate: SIG-TAINT strictly decreases toward 0;
  suite green; ASan clean; **stackless probe green** (a deep effectful-HOF
  recursion must not grow the C stack -- the whole point of DK).
- **Stage F -- E6 verification.** Corpus scan: `SIG-TAINT = 0`, `SIG-MAIN = 0`,
  `SIG-INLINE-C = 0`, and every remaining `SIG-REJECT`/`SIG-EXPORT` is provably
  **effect-free** (a new `TUR_TRACE_EVICT` column: `eff=0`). If any effectful
  function still evicts, it is a missed root -- fix before deletion.

### The deletion (Stage G -- only after F is clean)

1. Add a build-time assertion in the classifier: **no colored function with a
   non-empty net effect row may return false from `emit_cps_ir_try_fn`** (extend
   the PZ hard error to fire for an effectful SIG-* fallback too, in the shipping
   config). This makes "no effect on a fiber" a compiler invariant. **Carve-out:**
   the assertion must whitelist the session/thread subsystem (W5 correction) --
   an effectful SIG-INLINE-C fn whose inline-C is load-bearing pthread/session
   runtime (`session-effects` / `session-mp-effects`) is a permanent fiber client,
   not a missed DK root. Key the exemption on the SIG-INLINE-C-with-effect shape
   (or an explicit fixture allowlist), NOT on the effect name, so a genuine
   regression still fires.
2. Delete the `emit_effects.c` direct emitters (Sec 2a) and their `emit_value`
   dispatch. Replace with a hard error ("effect reached the direct emitter -- v2
   invariant violated") so any resurrection is caught.
3. Delete the fiber effect runtime C from `emit_module.c` (Sec 2a): `tur_effect_perform`,
   `EffectHandlerFrame`/`Case`, `global_effect_handler_chain`, `tur_handler_dispatch`
   + msdyn, `tur_effect_cont_*`, `tur_handler_table_t`, and the two `FiberBlock`
   effect fields. Keep `FiberBlock` (concurrency) and `tur_cloneable_cont` (DK
   bridge). **Blocked (W5 correction):** `session-effects` / `session-mp-effects`
   still emit and use `tur_effect_perform` / `global_effect_handler_chain` /
   `EffectHandlerFrame` / `tur_handler_dispatch` (their mains handle
   `SessionLog` / `MpLog` on the fiber). This step cannot proceed until those two
   permanent fiber clients are rewritten or bucketed out -- see Sec 7 and
   [docs/archive/cps-session-effects-permanently-fiber-bound.md](../../archive/cps-session-effects-permanently-fiber-bound.md).
4. Verify (Sec 8).

---

## 7. What "done" looks like

- `grep -c 'tur_effect_perform\|global_effect_handler_chain\|EffectHandlerFrame'`
  in any emitted `.c` = **0**.
- `emit_effects.c` contains only the delimited-control passthroughs; the direct
  perform/handle/resume emitters are gone.
- `emit_cps_ir_try_fn` never returns false for an effectful colored function
  **outside the session/thread carve-out**; the invariant is asserted at build
  time (with that carve-out whitelisted -- see Sec 6 step 1).
- Full suite green (2179/0), ASan clean, stackless probe green.
- The CPS/DK backend is the **sole effect lowering** for the delimited-
  continuation effect machine. (Pure indirect-call-colored functions may still
  direct-emit -- correct, touches no effect runtime.)

  **Blocker to deleting the fiber effect runtime C to ZERO (W5 correction,
  measured):** `session-effects` / `session-mp-effects` emit and *use* the fiber
  effect runtime symbols Sec 6 step 3 wants to delete -- their emitted C contains
  `tur_effect_perform`, `global_effect_handler_chain`, `EffectHandlerFrame`, and
  `tur_handler_dispatch` (their `main` handles `SessionLog` / `MpLog` on the fiber
  chain). So the Sec 7 "grep = 0" and the Sec 6 step 3 deletion **cannot be
  reached while these two fixtures stand**. This is the report's honest framing:
  the fiber effect runtime cannot be deleted to zero without first addressing the
  thread-based session model. Resolve before Stage G by one of: (a) rewrite the
  two fixtures so the log effect is handled in a non-inline-C helper (E3'-style),
  leaving only threaded session I/O on the fiber; (b) move them to a `requires.*`
  bucket and scope the "grep = 0" target to non-session fixtures; or (c) retain a
  minimal fiber-style effect path for the session/thread subsystem and redefine
  "done" as "no effect on the DK-lowerable path." See
  [docs/archive/cps-session-effects-permanently-fiber-bound.md](../../archive/cps-session-effects-permanently-fiber-bound.md).

---

## 8. Verification harness (run before/after every stage)

```sh
# 1. BODY-* stays empty AND no effectful SIG-* remains
TUR=./build/tur
for d in tests/fixtures/*/; do i="$d/input.tur"; [ -f "$i" ] || i="$d/$(basename $d).tur";
  [ -f "$i" ] && TUR_TRACE_EVICT=1 "$TUR" emit-c "$i" >/dev/null 2>>/tmp/ev.txt; done
grep '^\[EVICT\]' /tmp/ev.txt | awk '{print $2}' | sort | uniq -c        # category totals
# (add an eff=N column to the trace; assert every SIG-* row has eff=0 after Stage F)

# 2. no fiber effect runtime symbol survives in emitted C (post-deletion)
for d in tests/fixtures/*/; do "$TUR" emit-c "$d/input.tur" 2>/dev/null; done \
  | grep -c 'tur_effect_perform\|global_effect_handler_chain\|EffectHandlerFrame'   # want 0

# 3. suite (12-min timeout, STRICT RULE) + ASan
bash tests/run.sh        # expect 2179 passed, 0 failed

# 4. stackless sign-off: a deep effectful HOF recursion must not SIGSEGV
#    (author a fixture: 1e6-deep recursion through an effectful fn-value under a handler)
```

---

## 9. Honest risk assessment + KILL CRITERIA

This plan is feasible **only if E2 is**. E1/E3/E4/E5 are bounded engineering on
existing machinery (the carrier bridge, the d2b wrapper, the classifier). E2 -- a
`DK*`-threading channel through the fn-value/dict ABI -- is the load-bearing
assumption. Probe it FIRST, in isolation, before committing to Stages A-D:

- **Kill-probe (do this before anything else):** hand-write the C for one effectful
  fn-value threaded through a DK continuation -- a `map` over a list applying an
  effectful callback under a DK handler, where the callback's `perform` reaches the
  handler's prompt and a deep recursion stays flat. If the fat-closure box cannot
  carry a `DK*`-threading thunk that composes with `dk_perform`/`dk_shift` without
  reintroducing a per-call prompt (which would break the stackless property or the
  handler search), **E2 is impossible as framed** and the project must either (a)
  accept a permanent fiber effect runtime for effectful-HOF code (project premise
  fails), or (b) redesign the fn-value ABI wholesale (a far larger effort than this
  plan scopes). Either outcome is the honest "this may be dead" moment -- surface
  it immediately, do not grind.
- **Secondary risk -- snapshot churn (E1).** Carrier-ABI emission moves a large
  fraction of the corpus's codegen. Mitigate by landing per-signature-shape with
  per-shape snapshot regens; never one monster regen.
- **Secondary risk -- the cloneable-cont coupling.** `tur_cloneable_cont` is shared
  by the dying fiber multishot path and the surviving DK bridge. When deleting the
  fiber multishot emitter, confirm the DK bridge still constructs its cloneable
  conts (it does today via `__dk_cont_fn`); do not delete the shared runtime.
- **Non-goal creep.** Do NOT migrate pure indirect-call-colored functions or the
  concurrency `FiberBlock` axis. They do not block the deletion; folding them in
  turns a bounded effect-runtime deletion into an unbounded ABI rewrite.

## 10. Progress log

### Session 7 -- CLOSED: flag graduated, Stage G deletion done (2026-07-19)

The plan is complete. After Sessions 1-6 drove Stages 0-E, the tactical companion
`cps-dk-endgame-remaining-plan.md` closed the measured 24-fixture B1-B8 surface
(buckets B1-B7 fixed; B8 -- the mislabeled "permanent" session/fiber bucket --
DK-lowered after all). Then:

- **Flag graduation.** `cps-tramp-resume` moved from `EXPERIMENTS[]` to
  `GRADUATED[]` (`src/runtime/experiments.c`); `g_opt_cps_tramp_resume` defaults
  true. The 24 `--enable=cps-tramp-resume` fixture flag files were removed. A
  lingering `--enable` is now a TUR-W0063 no-op. (commit 786946a1)
- **Whole-suite flag-on correctness** was reached first (endgame Sec 3a.1): the
  generic-template tyvar-carrier sig-reject fix cleared 14 build failures, the
  `g_dk_driver` save/restore across `tur_fiber_block_resume` fixed the 2
  DK-trampoline crashes (`fiber-effect`, `p19-8`), the synthesized-main fold stopped
  swallowing top-level `?`/`return`/unhandled-perform diagnostics, and the 104-byte
  tail-resume DK-node leak was reaped at the entry boundary.
- **Stage G -- physical deletion (DONE).** `emit_module.c` no longer emits
  `tur_effect_perform`, `EffectHandlerFrame`/`Case`, `global_effect_handler_chain`,
  `tur_handler_dispatch` (+ `__tur_msdyn_*`), `tur_effect_cont_*`, or
  `TurEffectCaptureCtx`, plus the two `FiberBlock` effect fields. A full-corpus
  sweep returns zero call sites of every fiber effect symbol. The dead
  `emit_effects_perform` / `emit_effects_with_handler` fiber emit branches were
  removed as a follow-up tidy. (commits 7b40fc4e, cf3f44e9, d46f74bef)

`FiberBlock`/scheduler/reactor (concurrency), `tur_handler_table_t`, and
`tur_cloneable_cont_*` STAY -- they back the surviving concurrency axis and the DK
handler-value + `__Shift` bridge, exactly as the Sec 2b "STAYS" list specified.
Suite 2203/0. The two-runtime split is closed; **CPS/DK is the sole effect
lowering.** Remaining owning-value teardown leaks are pre-existing and tracked in
`docs/reported/`, out of scope for this plan.

### Session 6 -- E2 row-poly fn-value cluster onto the DK (Stage E, real fixture movement)

The effect-poly/-row/-subtype SIG-TAINT cluster was blocked NOT on the
fat-closure `fn_cps` channel (sub-slice 1) but on the ROW-VARIABLE effect
fn-value param gate.  A `(fn [] #fx{e} int)` param tiered PT_E1, forcing the
callback lambda to the fiber; those callbacks are bare int64 fn-ptrs threadable
via the E2a registry (the DK thread is effect-agnostic).  Two gated changes land
the cluster:

1. `param_thread_class` (emit_cps_ir.c): admit a row-variable TY_FN param under
   `--enable=cps-tramp-resume` (drop the `ERK_CONCRETE` requirement).
2. `colored_call_wbd_delegatable` (cps_ir.c): under the flag, skip the cross-HOF
   leaf-fiber delegation (`apply-logged = (apply callback x)`).  Its premise --
   the callback stays fiber -- no longer holds once (1) threads the callback;
   delegating the HOF while the callback threads splits performer/handler across
   runtimes (effect-poly-infer aborted).  Skipping it threads the HOF's `__cps`
   through the whole chain (`main -> apply-logged__cps -> apply__cps -> lambda`).

Moved onto the DK: `effect-poly-typeclass` (2), `-bracket` (release),
`effect-row-ho` (42), `-compose` (42), and the multi-hop `effect-poly-infer`
(calling).  Real fiber-live fixtures **27 -> ~18**.  Flag-off byte-identical
(default suite 2203/0); the whole effect family flag-on == baseline; full flag-on
soundness sweep clean (the mandatory gate for this cluster -- the earlier E2
attempt was reverted here for unsoundness).  Paper trail:
docs/archive/cps-e2-rowpoly-fnvalue-threading-boundary.md.

**Remaining fiber-live roots** (post-cluster): effect-subtype-* / -type-alias /
-struct-field-row / capability-effect-poly / fh-discharge-row (partial -- residual
eff=1 but correct output), effect-poly-map (non-tail leaf-fiber recursion),
effect-ref / effect-capture-k (owning-across-control / by-ref mut capture),
effect-nested (nested-handle), the while-native mutation-width residue, and the
permanent session/export carve-outs.

### Session 5 -- readset (delicate) landed; E2 STARTED (Stage E sub-slice 1, the ABI slot)

- **while-native read-after-set (LANDED).**  `set! total (+ total prev)` after
  `set! prev X` reads `prev` after its set.  The loop body lowers BACKWARD
  (EX_DO `for i downto 0`), so a during-lowering mask can't see the later set --
  the fix is a FORWARD pre-pass (`loop_rs_scan`, mirrors `loop_guard`'s walk) that
  records, by Expr-node identity, each straight-line carried-var read following its
  `set!`; `atomize` resolves such a read to the var's `$next` CVar.  Reads inside a
  branch / handle case still evict (their lifted frame can't reach `$next`).
  `cps-tramp-resume-while-readset` -> `run__cps`, prints 10; a chained
  read-after-set probe (`a=i+1; b=a+a; i+=b`) prints 6 flag-on and flag-off
  identically.  (A bug found + fixed mid-slice: `CpsB.rs_n` was uninitialised, an
  ASan stack overflow in `atomize` -- diagnosed from the trace, not reverted.)

- **E2 Stage E sub-slice 1 -- `tur_poly_fn_t` ABI slot (LANDED).**  The effectful
  fn-value channel starts with the cleanest, unambiguous representation:
  `tur_poly_fn_t { void *env; int64_t (*fn)(void*,int64_t); }` gains a third slot
  `int64_t (*fn_cps)(void*,int64_t, struct DK *)`.  Purely additive: every existing
  `(tur_poly_fn_t){env,fn}` positional literal zero-inits `fn_cps` to NULL and no
  code reads it, so behaviour is identical (`struct DK *` is an incomplete-pointer
  type -- the struct precedes DK in the preamble -- completed later, compatibly).
  Verified: 140 preamble snapshots regenerate with exactly ONE changed line each
  (the typedef); a full flag-off compile sweep is clean (the only failures are the
  standing `httpd-*` `-lturi` / `hamt.h` / dedicated-runner false-positives, all
  flag- and change-independent).

  **Sub-slice 2 (the atomic hard core -- NEXT).**  Interconnected, must land
  together for one fixture end-to-end:
  1. **Extend threadability.**  `fn_value_threadable` / `expr_collect_effects_acc`
     (thr_ok) must count a CALL of an `is_poly_fn` effectful param, in a HOF that
     will thread it through `fn_cps`, as a threadable use -- so the effectful lambda
     flowing in is marked `threadable_has`.
  2. **Reverse the fiber classification** (`emit_cps_ir.c:3663`): the
     `is_lifted_lambda || addr_taken -> sig_perm` forcing already exempts
     `threadable_has(binding)`; step 1 makes an `fn_cps`-threaded effectful lambda
     threadable, so it becomes a CPS candidate with a real `__cps` entry.
  3. **Thread the call.**  At the effectful poly-fn call site (`cps_ir.c` EX_CALL,
     the "E2 pending" eviction ~2931/3312), emit a CT_TAILCALL variant (new
     `via_poly_fn_cps` flag) that lowers to `f.fn_cps(f.env, args, __kont)` instead
     of `CT_UNSUPPORTED`.  Start with the TAIL case (a non-tail call needs the
     heap-join frame, like E2c).
  4. **Populate `fn_cps` at construction** (`emit_expr.c:7213/7218/7265`,
     `EX_POLY_WRAP` / `__tur_poly_to_fat`): set `fn_cps = &<lambda>__cps` when the
     wrapped fn is effectful and CPS-emitted.
  Verify: the target fixture prints the correct value (effect reaches the caller's
  handler), flag-off byte-identical, flag-on compile sweep clean, effect-poly family
  outputs unchanged, deep-recursion stackless probe green.  Then sub-slice 3
  migrates the cluster (effect-poly-*/-row-*/-subtype-*/-ho, run-with).

### Session 4 -- top-level-handle fold + loop-in-continuation (gated); prerequisite roots driven down

Focus: Stage A/E3 completion for the idiomatic top-level handler, plus two of the
4 non-permanent CPS roots.  All gated on `--enable=cps-tramp-resume`; flag-off
byte-identical (default suite 2202/0 throughout); flag-on build sweep clean (0
regressions) after each landing.

- **Top-level-handle fold (THE SIG-TAINT lever).** Root: top-level non-`defn`
  forms are bare exprs in `EX_PROGRAM.items`, never a `FnDef`, so the idiomatic
  `(println (handle (compute) ...))` was direct/fiber-emitted into a synthesized
  `int main()` that never reached the CPS classifier -- base_tainting its effect
  and cascading SIG-TAINT to every performer (~33 fixtures).  `elaborate_program`
  now folds trailing top-level statements into a synthesized
  `(defn main [] : int (do <stmts> 0))` that flows through `fn_is_d2b_main` +
  `emit_cps_ir` like a user main.  Conservative + macro-safe (`fold_stmt_is_risky`
  skips nested-handle / escaping-mut-`set!` shapes the DK can't lower yet).
  Result: real fiber-live fixtures 39 -> 29 (10 effect fixtures DK-lower;
  `effect-handler` emits `compute__cps`, output 104).  Report:
  docs/reported/cps-toplevel-synthesized-main-bypasses-dk.md.
- **Loop-in-handle-continuation (2 CPS roots).**  A `while` loop in a handle
  continuation evicted BODY-STRUCT-OR-TAINT.  Fixed: (1) `has_capture_rec` /
  `collect_caps_rec` gained `CT_LOOP` cases (a loop in a lifted continuation was
  bailing the capture collection); (2) `emit_loop` now threads the loop body's
  loop-INVARIANT free vars (a fn param / handle result the loop reads) as extra
  helper params, passed unchanged at the entry call and every back-edge (new
  `CE.cur_loop_inv`).  Moves `cps-backend-composite-in-continuation` +
  `cps-oracle-shift-under-handle` onto the DK (output 40).  Archived:
  docs/archive/cps-loop-in-handle-continuation-invariant-threading.md.
- **Owning-field borrow in a handler case.**  `expr_is_pure_borrow_of` now peels a
  field-read chain in its rc/weak arms, so `(rc/strong-count (.r o))` is a pure
  borrow -> the owning by-value aggregate capture is admitted.

**Remaining prerequisites (all the harder / multi-session stages):**
- **E2 (Stage E, THE hard stage) -- effectful fn-value `fn_cps` channel.** Blocks
  the whole effect-poly/-row/-subtype/-ho family (~11 SIG-TAINT fixtures whose
  effectful lambda passed to a HOF taints the effect) + `run-with` (handle-body
  fn-value).  Multi-slice per the Stage E plan; not startable as a quick win.
- **While-native mutation-width residue (`cps-tramp-resume-while-readset`).**
  Read-after-set (`set! total (+ total prev)` after `set! prev ...`).  Requires a
  forward SSA-renaming PRE-PASS: the loop body lowers BACKWARD (continuation-
  first), so a read of `prev` after its `set!` is lowered before the `set!` is
  processed -- a during-lowering version mask cannot see it.  The pre-pass must
  compute per-read versions and feed them to the backward lowering.  Delicate;
  1 fixture.
- **By-reference mutable capture (`effect-capture-k`).**  A `^mut` written in a
  lifted handler case + resumed after the handle needs a heap-cell capture
  (`collect_caps` walks only a `set!`'s value, captures copy in by value).  New DK
  feature.
- **Nested-handle `__kont` threading (`effect-nested`).**  Inner handle's
  continuation needs `__kont` from the sub-continuation.
- **Permanent carve-outs:** `session-effects` / `session-mp-effects` (pthread +
  inline-C session runtime) and `typeclass-effect-row-caller` (exported instance)
  -- scoped OUT of the deletion (Sec 7/Stage G carve-out), not DK targets.

### Session 1 -- gate cleared, E7 runtime landed (gated)

- **Kill-probe: GO.** `probes/e2-killprobe.c` -- an effectful callback reached
  through a fat-closure `fn_cps(void*,int64_t,DK*)` slot performs an effect that
  reaches the caller's `dk_handler` with no per-call prompt. E2 is feasible; the
  project is not dead at the gate. It also disproved the "DK is already stackless"
  premise (inline resume is 160 B/elt -> O(N)); recorded as enabler **E7**.
- **E7 algorithm validated at full fidelity.** `probes/e7-fidelity-probe.c`
  reproduces the real `emit_handle` chain layout + a deep tail-resume loop through
  the fn-value slot + an enclosing handle. Correct algorithm = heap meta-stack of
  pending deliveries (no-op-elided, O(nesting)) + `DKK_RESUME_FRAME`-threaded
  perform-continuation. Result at N=1e6: 152 B C stack (flat), handle-continuation
  once, enclosing propagation correct, meta-stack hwm=1.
- **E7 runtime machinery LANDED, gated (`--enable=cps-tramp-resume`).** The
  `struct DK.tail_resume` flag, `dk_handler_tail`, the meta-stack, `dk_tail_resume`,
  `__dk_drive_after`, and `dk_perform`'s yield branch are emitted only under the
  flag (`emit_cps_runtime_prelude_ex`); flag-off output is byte-identical (verified
  0 tramp tokens across 60 fixtures; flag-on C compiles). Experiment registered.
- **E3 (all mains CPS-emit) LANDED, gated.** `fn_is_d2b_main` returns true for any
  zero-arg main under the flag, dissolving the SIG-MAIN taint seed. Verified: an
  effectful main compiles and runs correctly under the flag (d2b `main` calling a
  still-fiber effect fn is fine).
- **Suite green: 2179/0** (flag off = default = unchanged).

**Key finding blocking an end-to-end `.tur` E7 test.** Even a *shallow*
perform+handle keeps its performer/handler SIG-TAINT after E3, because the
handler-installer `run` is a **whole-body delegation** that seeds its effect into
the base taint (`emit_cps_ir.c:2814`, the comment at :2809 names exactly this).
So a `handle` only leaves fiber once it DK-lowers.

### Session 2 -- Stage 0 COMPLETE end-to-end (gated)

Landed the full E7 emitter path + a scalar-signature slice of E1, all gated on
`--enable=cps-tramp-resume`, so a Turmeric effectful program now runs flat on the
DK path from source:

- **Scalar-signature handle DK-lowering (E1-lite).** Under the flag, a deep
  `handle` is no longer whole-body-delegatable (`cps_ir.c:1358` returns false), so
  it routes `build_handle -> CT_HANDLE` and DK-lowers. This dissolves the
  handler-installer's base taint -- `run`/`once`/`main` now CPS-emit; a shallow
  perform+handle runs entirely on the DK (`dk_perform`), verified `=> 42`. (Full
  non-scalar E1 -- carrier-ABI `__cps` for non-scalar signatures -- is still Stage C.)
- **E7 tail-recursive-perform admission.** `perform_cont_reset_ok` now admits a
  `CT_TAILCALL` perform-continuation under the flag (lifts as a `RESUME_FRAME`).
- **E7 emission wiring.** `case_body_tail_resumes` marks a tail-resume case;
  `emit_handle` installs it with `dk_handler_tail`; `emit_resume` emits
  `dk_tail_resume` (yield) for a tail resume in a handler case; the d2b-main entry
  wrapper installs the `setjmp` driver (`__dk_drive_after`).
- **End-to-end result.** A 1e6-deep tail-recursive effectful loop under a resuming
  handler runs **flat** (10e6 also flat; inline resume SIGSEGVs by ~50k); the
  accumulator form threads the resumed value correctly (`3*1e6 = 3000000`);
  **valgrind: 0 errors, 0 leaks**. Fixture `tests/fixtures/cps-tramp-resume-deep`.
- **Suite: 2180/0** (2179 unchanged + the new fixture; flag-off byte-identical).

**This is the stackless sign-off (Sec 8 item 4) passing from source, gated.** The
Stage-0 prerequisite the kill-probe surfaced is done and demonstrated end-to-end.

### Session 2 (cont.) -- Stage F `eff` column + the REMAINING surface is measured

Added the Stage-F `eff=N` column to `TUR_TRACE_EVICT` (`eff=1` marks a fn that
performs/handles an effect -- the ONLY evictions that keep the fiber effect
runtime alive; pure `eff=0` SIG-REJECT/SIG-EXPORT are out of scope per Sec 4). Also
exempted `--enable=cps-tramp-resume` from the N6.5 hard-error gate (like
`cps-async`): the flag EXPANDS the colored surface and legitimately carries
in-flight BODY residuals until it graduates.

**Corpus scan under the flag (275 effect-bearing fixtures) -- the actual remaining
work for the deletion is BOUNDED and small:**

| Category | all | **eff=1 (blocks deletion)** | stage |
|---|---|---|---|
| SIG-REJECT | 1171 | **7** | E1 (non-scalar carrier-ABI) |
| SIG-EXPORT | 828 | **1** | E1/E4 |
| SIG-INLINE-C | 2 | **2** | E5 |
| SIG-TAINT | 22 | **22** | dissolves when above clear |
| BODY-STRUCT-OR-TAINT | 44 | **38** | native DK admission (v1-style) |
| BODY-UNSUPPORTED | 2 | **1** | native DK admission |

The ~1990 pure `eff=0` SIG-* evictions are out of scope. The real remaining surface
is **~48 effectful functions**: ~39 BODY roots (the same "drive effectful BODY
shapes to DK-admission" work v1 did for the shipping config, now under the flag),
8 non-scalar signatures (E1), 2 inline-C (E5); the 22 SIG-TAINT then dissolve.
This is what Stages C-F now cover concretely.

### Session 2 (cont.) -- the 39 BODY roots characterized into concrete slices

Minimal probes under the flag pinpoint what actually admits vs what the ~39 BODY
roots need (they are NOT 39 independent shapes -- most cascade from two roots):

**Already admits (verified, so these are taint-driven, not body-shape blockers):**
- Non-tail perform as an operand -- `(+ (perform (C)) n)` -> works (=> 142). So
  `effect-reopen:counted-sum` evicts only via TAINT from `main`, not its own body.
- Sequential nil-effect performs -- `(do (perform (W "a")) (perform (W "b")))` -> works.

**The two genuine roots the BODY bucket cascades from:**
1. **Join (`CT_LETCONT`) inside a perform continuation.** `(let [n (perform (R))]
   (perform (W (if (> n 0) "pos" "neg"))))` evicts: the second perform's arg is a
   value produced by a branch, which lowers to a `CT_LETCONT` join, and
   `perform_cont_reset_ok` (`emit_cps_ir.c:1398`) has no `CT_LETCONT` case.
   **Slice C1: admit a join in a perform continuation** (lift the join as its own
   frame, like the reset/handle join path). Clears the effect-row / re-opening
   fixtures (`effect-do-union`, `effect-reopen`, `effect-row-*`).
2. **Effectful fn-value param + effectful fn-value (E1+E2).** `run-with-write
   (fn [] (println "pure"))` etc.: the fn-value-param callee is SIG-REJECT ->
   fiber, so the effect it performs runs on fiber and the enclosing DK handle
   cannot catch it -> the whole main evicts. This is the dominant remaining bucket
   (`effect-subtype-*`, `effect-poly-*`, `handle-effectful-fn-param-*`, every
   `__fn_*` root) and needs **E1** (admit the fn-value-param signature, carrier-ABI)
   + **E2** (the `__fn_cps` slot so the callback's perform threads the DK). This is
   the large, coupled ABI change (plan Stage C/E); the kill-probe already proved it
   feasible end to end.

**Ordered remaining slices:** C1 (join-in-perform-cont, bounded) -> E1 fn-value-param
signature admission -> E2 fn-value `__fn_cps` ABI (the big one) -> E5 (2 inline-C)
-> SIG-TAINT empties -> promote off the `--enable` gate (default BODY=0 re-verified)
-> Stage G deletion. **Next concrete step: Slice C1 (admit `CT_LETCONT` in the
perform continuation), then the E1/E2 fn-value ABI.**

### Session 3 -- C1 landed; E2 attempted, REVERTED (unsound); the flag is provably
### incomplete in intermediate states

- **C1 LANDED (gated):** `perform_cont_reset_ok` admits a `CT_LETCONT` join and a
  `KK_VAR` join-jump under the flag, so a branch feeding a subsequent perform lifts
  into the RESUME_FRAME. `effect-do-union` clears; parity holds on the effect-row
  fixtures. Fixture `cps-tramp-resume-join`. (This slice is sound and kept.)
- **E2 ATTEMPTED then REVERTED.** Built a `__tur_cps_lookup(direct-ptr -> cps-ptr)`
  registry + tail-position threading (0-arg, then multi-arg scalar). It works for a
  BARE function pointer (named fn / non-capturing lambda) -- verified `5`, `35`,
  `1112`. **But it is unsound for a CAPTURING closure**: the fn-value there is an
  ENV pointer, not a thunk pointer, so the registry lookup misses and the fallback
  calls the env as a function -> wrong value / crash. There is NO way to tell a
  bare-fn-ptr from a capturing-closure at the call site without the fat-closure box
  `__fn_cps` slot, so the registry approach is a dead end. Reverted (commit
  e92609f). **E2 genuinely requires the fat-closure box `__fn_cps` slot (plan E2 as
  originally written), not a side registry.**
- **CRITICAL FINDING -- partial migration is unsound behind the flag.** A soundness
  sweep of the effect fixtures UNDER THE FLAG (the main suite only exercises them
  flag-OFF, so it never caught this) shows that even WITHOUT E2, the E1-lite/C1
  handle-DK-lowering leaves several shapes ESCAPING (`unhandled effect`):
  `cps-backend-capture-fnvalue`, `cps-backend-indirect-call`, `effect-poly-infer`,
  `fiber-effect`. Root: a handle DK-lowers, but its effect is still performed
  through an unthreadable path (a capturing fn-value, an indirect call, a fiber)
  that stays fiber, and the taint model does not co-classify that performer with
  the DK handler. **The flag cannot be sound until (a) every effect path can thread
  the DK (full E1+E2) AND (b) the taint model conservatively evicts any handle
  whose effect could be performed through a not-yet-threadable path.** This is
  inherent to a partial migration; the DEFAULT config stays sound (suite 2187/0),
  and the flag is a genuine in-flight experiment that MUST NOT be promoted until
  both hold.
- **Revised guidance for the next session:** do NOT extend gated admission
  piecemeal -- each increment that moves a handle to DK without a *complete* taint
  guard adds an escape. The sound path is: (1) implement the fat-closure `__fn_cps`
  ABI (E2) and the carrier-ABI `__cps` for non-scalar signatures (E1) so effect
  paths are actually threadable; (2) add a taint-completeness guard that keeps a
  handle on fiber unless ALL performers of its effect are DK-threadable; (3) only
  then does the gated corpus become escape-free and the gate promotable. A
  per-stage `--enable` soundness sweep (build+run the effect fixtures UNDER the
  flag, diff vs baseline) must be part of every future stage's gate -- the main
  suite does not cover flag-on behavior.

### Session 3 (cont.) -- drove the flag-on soundness sweep 7 mismatches -> 3

Built a repeatable **flag-on soundness sweep** (build+run the 274 effect fixtures
under `--enable=cps-tramp-resume`, diff vs baseline, distinguishing build-fails
from runtime divergence -- an earlier ad-hoc sweep was unreliable because it reused
stale binaries). Starting point after the E2 revert: 261 sound / 3 build-fail / 7
runtime mismatch. Landed four soundness fixes, all gated:

1. **Reverted aggressive E3.** Making EVERY zero-arg main d2b broke a main that
   transitively reaches fiber effect code (the DK entry does not activate the fiber
   effect runtime): SIGSEGV / skipped-println / unhandled-effect, and it triggered
   the pre-existing `tur_poly_fn_t/void*` build error. Restored the historical
   `contains shift` gate on both configs.
2. **E7 driver in the direct->cps wrapper** (not only the d2b main), so a colored
   body reached from a plain main (`(println (run))`) still trampolines flat.
3. **Shallow-handler tail-resume fix.** `emit_resume` must yield (`dk_tail_resume`)
   only for a case installed with `dk_handler_tail` -- a DEEP tail-resume. A shallow
   case was yielding while `dk_perform` queued no delivery, dropping the handle's
   post-continuation (`(* 10 (handle-shallow ...))` -> 42 not 420). Threaded via a
   new `CE.case_tail_resume` flag.
4. **Evict + taint effectful fn-value CALLS.** An effectful fn-value call can't
   thread the DK (needs E2); it now evicts `CT_UNSUPPORTED`, is `sig_perm`, and the
   fn-value's concrete effect row is credited as a perform so the effect taints and
   the DK handler co-classifies to fiber.

Then a 5th fix took it to **269 / 274 sound**:

5. **An effectful lifted lambda evicts to fiber.** A captureless lifted lambda
   (`Binding.is_lifted_lambda`) is ALWAYS used as a fn-value; its DK direct-entry,
   invoked indirectly, installs a fresh root, so a CPS-emitted effectful lambda's
   `perform` escapes (`effect-poly-infer`: `(apply (fn [v] (do (perform (Log ..))
   v)) x)` -> unhandled effect). Now, in `ensure_S`, an effectful lifted lambda is
   `candidate=false + sig_perm` -- a fiber source, so its effect taints and the DK
   handler-installer co-classifies to fiber (the lambda's fiber perform is then
   caught by dynamic lookup). Verified: `effect-poly-infer` matches baseline;
   `effect-poly-map` + E7 fixtures unchanged.

Then a 6th fix took it to a **CLEAN sweep**:

6. **Address-taken effectful fns evict to fiber.** Generalized fix #5 to ANY fn
   used as a fn-value. A program-wide pre-pass (`g_addr_collecting`) records every
   global fn referenced in value position (reusing the `EX_VAR` fn-value detection
   in `expr_collect_effects_acc`; a direct-call callee never reaches it --
   `fn_binding` with `fn_expr=NULL`). `ensure_S` forces an effectful address-taken
   fn (or lifted lambda) to `sig_perm`. Fixed both fiber cases -- `fiber-effect`
   (10|99), `p19-8-fiber-effect-chain` (20|30|99) -- where an effectful body is
   passed as a fn-value to a fiber-creating inline-C fn.

**Result: the flag-on soundness sweep is CLEAN -- 271 / 274 sound, 0 build-fails,
0 runtime mismatches** (the 3 uncounted are fixtures baseline itself does not
build). Default suite 2185/0; E7 + poly fixtures unchanged.

### Where the gate stands now

`--enable=cps-tramp-resume` is **escape-free across the effect corpus**: every
effect either DK-lowers correctly (deep tail-resume flat) or evicts to fiber and is
handled there, with the taint model co-classifying performers and handlers. What it
does NOT yet do is move the effectful fn-value / non-scalar-signature cases OFF the
fiber -- they EVICT (soundly) rather than thread the DK. So the gate is a sound
in-flight state, not yet promotable: promotion still needs

- **E2** -- the fat-closure `__fn_cps` slot so an effectful fn-value threads the DK
  (instead of evicting), which is what actually shrinks the fiber effect runtime's
  live set toward empty; and
- **E1** -- carrier-ABI `__cps` for non-scalar signatures.

The taint-completeness guards built this session (fn-value-call eviction,
address-taken/lambda `sig_perm`, indirect-effect crediting) are exactly the
invariant Stage G's deletion assertion needs: no effect ever reaches a fiber that a
DK handler is responsible for. **Next: E2 (fat-closure `__fn_cps`), then re-run the
sweep -- each fn-value that threads the DK is one more removed from the fiber set;
the gate promotes when that set is empty.**

### E2 design spec (grounded, so the next session executes without the earlier misstep)

The current effectful eviction surface under the flag (all guards active): **62
SIG-TAINT** (the cascade), 8 BODY-STRUCT-OR-TAINT, 7 SIG-REJECT, 3 BODY-UNSUPPORTED
(2 "effectful fn-value call (E2 pending)" + 1 EX_WHILE), 2 SIG-INLINE-C, 1
SIG-EXPORT. The 62 SIG-TAINT dissolve once the effectful fn-value ROOTS thread the
DK -- that is E2.

**Why the earlier registry attempt was unsound (do NOT repeat it).** A fn-value is
carried in THREE distinct C representations, and a single `int64` carrier at the
call site cannot be reliably interpreted as one versus another:

1. **Bare `int64` fn-ptr** -- a monomorphic `(fn [int] int)` value that is a named
   fn or a captureless lambda; the carrier IS the direct-entry address.
2. **`tur_poly_fn_t { void *env; int64_t (*fn)(void*, int64_t); }`** -- a rank-2
   (`is_poly_fn`) param, or a value boxed via `EX_FN_TO_FAT`; the carrier is a
   pointer to this struct.
3. **Fat-closure box `struct <env> { int64_t __fn; captures... }`** -- a capturing
   lambda; the carrier is the env pointer, slot 0 is the thunk.

A `__tur_cps_lookup(carrier -> cps-ptr)` registry keys correctly only on
representation (1). For (2)/(3) the carrier is an env/box pointer, not a thunk
address, so the lookup misses and the fallback calls the pointer as a function ->
wrong value / crash. There is no call-site test to disambiguate.

**The sound design: give each representation its own `DK*`-threading channel, and
choose it STATICALLY from the callee's C type at the call site (which the emitter
already knows -- `is_poly_fn`, the closure env type, the mono carrier).**

- `tur_poly_fn_t` -> `{ void *env; int64_t (*fn)(void*, int64_t); int64_t
  (*fn_cps)(void*, int64_t, DK*); }`. Call an effectful poly fn-value as
  `f.fn_cps(f.env, args, __kont)`. `EX_FN_TO_FAT` / `__tur_poly_to_fat` populate
  `fn_cps` alongside `fn`. This is the cleanest representation and the place to
  START (contained; no ambiguity -- a `tur_poly_fn_t` is always a struct).
- Fat-closure box -> add a second slot `int64_t (*__fn_cps)(void*, ..., DK*)` after
  `__fn`. The indirect call on a box reads `box->__fn_cps`.
- Bare `int64` fn-ptr -> the ONLY case the registry handles soundly (verified this
  session: 0-arg/multi-arg tail threading gave `5`/`35`/`1112`). Keep the registry
  ONLY for the statically-known-bare-fn-ptr call site; do not use it where the
  callee could be (2)/(3).

For EACH, the `__fn_cps`/`fn_cps` thunk is the CPS variant of the fn body
(`<name>__cps(env..., args..., DK*)`); the indirect-call CT-IR admits an effectful
fn-value callee as a native `CT_LETCALL`/`CT_TAILCALL` threading `__kont` (replacing
this session's eviction guard), and the perform inside reaches the caller's prompt
(kill-probe-proven). Sequence: (E2a) `tur_poly_fn_t.fn_cps` end-to-end with a
flag-on sweep; (E2b) fat-closure box slot; (E2c) bare-ptr via the registry, only at
statically-bare sites; after each, the "effectful fn-value call (E2 pending)"
eviction and its SIG-TAINT cascade shrink. When the effectful eviction surface
reaches only genuinely-pure `eff=0` SIG-*, the gate promotes and Stage G deletes.

**Non-negotiable process:** run the flag-on soundness sweep (build+run the 274
effect fixtures under `--enable`, diff vs baseline) after every E2 sub-slice -- it
is the only check that covers flag-on behavior, and it is what caught the earlier
unsound attempt.

#### The invariant that makes E2 a COLORING problem, not a set of local sub-slices

Two facts, verified this session, together mean E2 cannot be landed as isolated
call-site rewrites -- it needs a whole-program fn-value-threading decision:

1. **Representation is statically knowable at the call site.** A mono `(fn [int]
   int)` param is a bare `int64` fn-ptr UNLESS a capturing closure can reach it, in
   which case `EX_FN_TO_FAT` boxes it and the param becomes `is_poly_fn`
   (`tur_poly_fn_t`). Confirmed: `apply1(tur_poly_fn_t f, ...)` when passed a
   capturing `(fn [v] (+ v base))`; `run_hywith(int64_t f)` when passed a
   captureless one. So the emitter can pick the right `fn_cps` channel from
   `is_poly_fn` -- there is no runtime ambiguity to resolve.

2. **A CPS-emitted (DK) fn-value escapes at ANY unthreaded call site.** Its direct
   entry installs a fresh root, so a `perform` inside finds no handler. Therefore a
   fn-value may thread the DK ONLY IF *every* site that invokes it threads `__kont`
   -- a bare-ptr tail call in a DK body, a `tur_poly_fn_t.fn_cps` call, a box
   `__fn_cps` call. If ANY use is unthreadable (a fiber HOF invokes it, a non-tail
   position that can't carry `__kont`, an inline-C `((fn_ptr)f)()` like the fiber
   fixtures), the fn-value MUST stay fiber (direct entry, dynamic handler lookup).

This is why the session's taint-completeness guards EVICT effectful fn-values
rather than thread them: eviction is the sound default, and it is correct until the
coordinated decision exists. **E2 is therefore a coloring fixpoint over fn-values:**
a fn-value is "DK-threadable" iff all its invocation sites are DK-threadable and it
does not flow into an un-threadable sink (inline-C fn-ptr cast, a fiber-block body,
a dict slot not yet given a `__cps` method); the fixpoint seeds from the
un-threadable sinks and propagates. A fn-value in the DK-threadable set gets a
`fn_cps`/`__fn_cps` channel and every call site threads it; one outside the set
keeps this session's eviction. Only when the fixpoint marks an effectful fn-value
DK-threadable do its `__cps` entry, the call-site threading, and the removal of the
"E2 pending" eviction all land together -- atomically per fn-value, verified by the
sweep. This is the real shape of E2, and the reason it is a focused multi-day
effort rather than a quick continuation: the ABI plumbing (the three `fn_cps`
channels) is the easy half; the fn-value-threading coloring that keeps it sound is
the load-bearing half.

#### E2 threadability analysis LANDED (codegen-neutral) -- the coloring, measured + tiered

The load-bearing half above is now implemented as a **codegen-neutral** coloring
pass (`emit_cps_ir.c`, gated on `cps-tramp-resume`; runs only under
`TUR_TRACE_EVICT`, so it changes no emission -- default suite 2185/0, flag-on sweep
0 build-fails / 0 real mismatches, both byte-identical). It answers, per effectful
fn-value that today evicts to the fiber: could it thread the DK, and how hard?

- `param_thread_class(fd, i)` -- classifies param `i` of colored HOF `fd`.
  `ptc_walk` counts, with tail-position tracking, the param's value-uses (escapes),
  its tail callee-calls, and its non-tail callee-calls; a call under the HOF's own
  `handle`/`reset` body, or an occurrence in an uncovered form, is absorbed into
  value-uses (conservative). A param that NEVER escapes (all uses are calls to it)
  is a threading candidate, tiered by difficulty: **PT_NOW** (tail-only, scalar
  carrier), **PT_NONTAIL** (a non-tail call needs a `__kont`-threading `CT_LETCALL`),
  **PT_E1** (an `is_poly_fn` / capturing param needs E1's carrier ABI first).
- `fn_value_threadable(fv)` -- over ONE exhaustive program walk, count fv's total
  value-uses and its in-surface threadable-arg uses (fv passed, wrappers peeled
  through `EX_POLY_WRAP`/`EX_FN_TO_FAT`/`EX_POLY_TO_FAT`/ascription, as arg `i` to a
  threading-tier param), and the hardest tier among them. Threadable iff **all**
  uses are threadable-args. Traced `[E2-COLOR] <fn> thr=Y/N tier=now/nontail/e1 ...`.

**Measured surface (runnable effect corpus): 9 threadable, 10 fiber sinks.** The
tier breakdown of the 9 corrects an earlier misread of this data:

| tier | count | shape | E2 work |
|---|---|---|---|
| `now` | 4 | tail-only callee, scalar-carrier param (`call-writer [f] (f ...)`, `effect-poly-infer`, `effect-row-compose`) | admit an effectful fn-value TAIL call as a `__kont`-threading `CT_TAILCALL` |
| `nontail` | 5 | callee-only but a call is non-tail (`use-writer [f] (do (f "hi") n)`, `run-twice [f] (+ (f) (f))`, `effect-poly-bracket`) | admit an effectful fn-value NON-TAIL call as a `__kont`-threading `CT_LETCALL` |
| `e1` | **0** | -- | -- |

The 10 `thr=N` are **genuine fiber sinks**, correctly staying fiber: a fiber
inline-C block casts the fn to a raw fn-ptr (`fiber-effect`, `p19-8` x2), the
fn-value is stored in a capability / struct field (`capability-effect-poly`,
`effect-struct-field-row`), it is called under the HOF's OWN `handle`
(`handle-effectful-fn-param-same-fn` -- threading past that handler would be wrong),
or it is passed as a recursive arg the conservative coloring does not yet chase
(`effect-poly-map`).

**Correction -- E2 is NOT gated by E1.** An earlier note here claimed the surface
was dominated by SIG-REJECT HOFs and that E1 must precede E2. The tiered data
refutes it: `fn_sig_ok` ADMITS a plain (non-`is_poly_fn`) effectful `TY_FN` param,
so HOFs like `apply-cb`/`use-writer`/`run-twice` already CPS-emit; the blocker is
NOT their signature but that the effectful fn-value CALL inside them is currently
lowered as a delegated (uncolored) indirect call rather than a `__kont`-threading
one. **Zero** of the threadable-surface fn-values need E1. So E2's real, contained
work is: admit the effectful fn-value call as a DK-threading call -- **tier `now`
first** (4 cases, `CT_TAILCALL` threading `__kont`), then **tier `nontail`** (5
cases, `CT_LETCALL` threading `__kont`) -- each gated per-fn-value by this coloring
(`thr=Y`), verified by the flag-on sweep. E1 (`is_poly_fn` carrier ABI) is a
separate, smaller concern that gates none of these 9. The `[E2-COLOR]` trace is the
instrument: re-run it after each E2 slice and watch the sinks hold while the
`now`/`nontail` set migrates off the fiber.

#### E2a REGISTRY PROBE -- tier-`now` bare-fn-ptr threading is sound (GO)

Before wiring the emitter, the tier-`now` mechanism was proven end-to-end in
`docs/upcoming/v2/probes/e2a-registry-probe.c` (output `e2a-registry-probe.out`;
`cc -O2 -o /tmp/e2a docs/upcoming/v2/probes/e2a-registry-probe.c && /tmp/e2a`). The
kill-probe proved a fat-closure `fn_cps` SLOT threads; this probe proves the OTHER
path the coloring needs -- recovering the `__cps` entry by LOOKING UP a bare
direct-entry fn-ptr, because (confirmed from the emitted C) a captureless effectful
lambda is carried as `(int64_t)__fn_NNNN` with no slot to read.

The probe models `effect-fn-type-annot` verbatim and **PASSES**: register
`(int64)__fn_1282 -> __fn_1282__cps` at startup; `call_writer__cps(f, __kont)`
recovers `cps = tur_cps_lookup(f)` and tail-calls `cps("...", __kont)`;
`__fn_1282__cps` does `dk_perform(WRITE_TAG, s, __kont)`; the perform's handler
search crosses the fn-value to main's DK handler, which prints and `dk_invoke`s the
resume -- string printed once, `result == 0`. So the registry indirection (the only
new element over the kill-probe) is sound for representation (1).

#### E2a implementation checklist (ordered, gated, revert-safe per step)

Grounded in the confirmed ABI (`__fn_NNNN` is a bare `int64` direct-entry;
`call_hywriter((int64)__fn_NNNN)` passes it bare) and the passing probe. Each step
is gated on `cps-tramp-resume`, per-fn-value gated by `thr=Y tier=now`, and verified
by the flag-on sweep; land only when the sweep is clean, else revert.

1. **Registry runtime** (`emit_dk_runtime.c`, gated on `tramp`). Emit
   `tur_cps_register(intptr_t direct, void *cps)` + `tur_cps_lookup(intptr_t)` and a
   small static table, exactly as the probe. Consumed by steps 3-5, so it is not
   dead code once they land (land it WITH step 2, not before).
2. **Lambda `__cps` emission.** For a `g_threadable_fn` captureless effectful
   lambda `__fn_N`, route its body through the CPS emitter to emit
   `__fn_N__cps(int64 args..., DK *__kont)` beside its existing direct entry. Its
   `perform` lowers to `dk_perform(TAG, arg, __kont)` (already how a colored fn
   lowers a perform). This requires **not** `sig_perm`-ing the threadable lambda
   (relax the fix-#5/#6 guard for `g_threadable_fn` members) so it enters S.
3. **Register at startup.** Emit `tur_cps_register((intptr_t)__fn_N, __fn_N__cps);`
   in the program's entry preamble for each registered lambda.
4. **Threaded call site.** Replace the `call_is_effectful_fnvalue` eviction
   (`cps_ir.c`, the "effectful fn-value call (E2 pending)" CT_UNSUPPORTED) for a
   tier-`now` call with a `CT_TAILCALL` that threads `__kont`; the emitter
   (`emit_cps_ir.c`) lowers it as `cps = tur_cps_lookup(f); return ((cps_t)cps)(args,
   __kont);` at the statically-bare call site.
5. **Let the taint model co-classify the handler.** With the lambda in S and its
   perform DK-lowered, its effect is no longer fiber-tainted, so the enclosing
   `handle` (e.g. `main`'s) co-classifies to DK automatically -- no separate change,
   but the whole chain (lambda perform -> HOF call -> handler) must flip together,
   which is exactly what the per-fn-value `thr=Y` gate guarantees.
6. **Verify + iterate.** Re-run `[E2-COLOR]` (the `now` fn-value should now emit a
   `__cps` + registration and its HOF a threaded call) and the flag-on sweep. When
   the 4 `now` cases are off the fiber and sound, extend to **tier `nontail`**
   (step 4 becomes a `CT_LETCALL` binding the result, threading `__kont`).

Only `is_poly_fn`/capturing fn-values (tier `e1`, currently 0 in the corpus) need
the `tur_poly_fn_t.fn_cps` / fat-box `__fn_cps` channels; those are E2b/E1 and gate
none of the 9.

#### Obligations discovered by ATTEMPTING slice 1 (empirical, reverted)

A first attempt at slice 1 (registry runtime landed + step 2's `sig_perm` relaxation
for tier-`now` `g_threadable_fn` members) was built and swept; it was **reverted**
because the flag-on sweep went red (`effect-poly-infer`: `unhandled effect (tag 2)`,
abort). The failure is instructive and sharpens the checklist:

- **Steps 2 and 4 are ATOMIC -- never relax the lambda's gate before its callers
  thread.** Relaxing `sig_perm` alone had two outcomes across the corpus: on
  `effect-fn-type-annot` it was INERT (the lambda stayed fiber anyway, because
  `call-writer`'s still-evicting `(f ...)` call taints `Write`, and the taint model
  co-classifies the lambda back to fiber); on `effect-poly-infer` it was UNSOUND (the
  lambda's taint path differed, so it entered S, CPS-emitted its `perform` on the DK,
  and that `perform` ESCAPED at the still-unthreaded call site -> unhandled effect).
  This is a concrete, reproduced proof of the plan's fact #2 ("a CPS-emitted fn-value
  escapes at ANY unthreaded call site"). So the lambda `__cps` (step 2), its
  registration (step 3), and the threaded call site (step 4) must land in ONE change,
  per fn-value, verified by the sweep before the gate is relaxed for that fn-value.

- **A NEW soundness obligation -- two-directional coloring.** `g_threadable_fn` is a
  value->param property (the lambda's every use is a threadable-arg). Threading a
  call `(f ...)` in a HOF also needs the CONVERSE param->value property: EVERY
  fn-value that can flow to param `f` must be registered. Otherwise a threaded call
  site could `__tur_cps_lookup` a fn-value that was never registered (a lambda that
  is `thr=N` because one of its OTHER uses is a sink, yet still flows to this
  threading param) -> lookup returns NULL -> abort. Add `param_is_thread_safe(program,
  fd, i)` = every call `(fd ... arg_i ...)` passes an `arg_i` that peels to a member
  of `g_threadable_fn`; thread `(f ...)` iff `f`'s param is BOTH `PT_NOW` AND
  thread-safe. (In the corpus each tier-`now` HOF receives exactly one registered
  lambda, so this holds trivially -- but the check is load-bearing in general.)

- **Cross-file bridge.** The eviction decision lives in `cps_ir.c` (the pass), but the
  threading decision needs the program + coloring, which live in `emit_cps_ir.c`.
  Bridge with a global set of param bindings (`g_e2_thread_params`) that
  `emit_cps_ir.c` populates (params that are `PT_NOW` and thread-safe) before
  `cps_ir_translate_fn` runs; `cps_ir.c`'s `call_is_effectful_fnvalue` eviction then
  checks membership and, for a member, emits a threaded `CT_TAILCALL` instead of
  `CT_UNSUPPORTED`.

- **Registry runtime is trivial + verified.** Step 1 (the `emit_dk_runtime.c`
  `__tur_cps_register`/`__tur_cps_lookup` table, gated on `tramp`, `__attribute__
  ((unused))`) built and swept clean on its own -- it is the one piece that can land
  ahead, but per the atomicity point it should land WITH steps 2-4 for a fn-value, not
  as lone dead code.

Net: slice 1 is a single atomic change spanning `emit_dk_runtime.c` (registry) +
`emit_cps_ir.c` (relax gate for thread-safe tier-`now`; populate `g_e2_thread_params`;
emit `__cps` + registration; lower the threaded call) + `cps_ir.c` (emit the threaded
`CT_TAILCALL` for a `g_e2_thread_params` callee), landed only when the flag-on sweep
is green. The mechanism is proven (probe); the obligations above are what a correct
landing must satisfy.

#### THE decisive blocker found by building slice 1 fully (reverted): the handler is in `main`

A second attempt built the ENTIRE slice-1 machinery -- registry runtime, the
`param_thread_class` concrete-effect gate (row-poly params -> `PT_E1`, which un-broke
`effect-poly-infer`), `param_is_thread_safe` (the param->value converse), the
`g_thread_params` cross-file bridge (`cps_ir.h` API + `cps_ir.c` set), the
`via_registry` `CT_TAILCALL` flag, and its registry-lookup emission. It compiled and
the concrete-gated baseline swept **185/0/0 clean**. But the target fixture STILL did
not thread, and the trace showed why:

```
[E2-COLOR] __fn_1282   thr=Y tier=now ...       <- lambda IS colored threadable
[EVICT]    SIG-TAINT   eff=1 call-writer         <- but its HOF still evicts, TAINTED
```

`call-writer` evicts **SIG-TAINT on `Write`**, and the taint source is **`main`**:
`main` is the program entry (`fn_is_main && !fn_is_d2b_main` -> `sig_perm`), it
*handles* `Write`, and `fn_is_d2b_main` only admits a `shift`-containing body, NOT a
`handle`-containing one. So `main`'s handler runs on the **fiber** (confirmed: the
emitted C carries `__eff_frame_*` / `tur_effect_perform`, no `main__cps`), which
permanently taints `Write`, which co-classifies `call-writer` AND the lambda back to
fiber. **All four tier-`now` corpus fixtures put their `handle` in `main`.**

**So E2a fn-value threading is necessary but NOT sufficient for this corpus.** The
gating blocker is orthogonal and larger: *the effect handler itself is on the fiber
because it lives in `main`, and `main` is not d2b for a `handle` body*. Threading the
performer to a fiber handler changes nothing. The real dependency graph is a
whole-program fixpoint:

> `main` d2b-for-handle  <=>  handler DK  <=>  performer (the lambda) DK  <=>
> fn-value threaded (E2a)  <=>  `main` d2b-for-handle

None of these can flip alone; they flip together or not at all. This is the same
"whole chain flips together" wall, now traced to its root: **the entry `main`'s
handler.** Two ways forward, both larger than E2a-in-isolation:

1. **Prerequisite E3' -- `main` d2b for a `handle` body.** Extend `fn_is_d2b_main`
   (or the entry-wrapper path) to CPS-emit a `main` whose body is a `handle` (install
   a DK prompt + the setjmp driver, exactly like the d2b-shift-main already does),
   GATED so it only fires when the handled effects are fully DK-threadable (else the
   aggressive-E3 regression returns: a `main` reaching fiber effect code SIGSEGVs).
   That gate is the same coloring fixpoint above. With `main`'s handler on DK, the
   Write taint clears, `call-writer` CPS-emits, and the E2a threaded call delivers the
   perform to `main`'s DK handler -- the whole chain flips.
2. **A handler-in-helper fixture.** A program whose effect HANDLER lives in a
   non-`main` helper (already DK-emittable) + a tier-`now` HOF + captureless lambda
   would let E2a land in ISOLATION (no `main`-d2b dependency). The current corpus has
   none; a synthetic fixture (`effect-fn-value-helper-handle`) would be the minimal
   first sound E2a landing, decoupled from E3'.

Recommendation: land **E3' (main d2b-for-handle, coloring-gated)** first -- it is the
true prerequisite the corpus needs -- OR add the handler-in-helper fixture to land
E2a decoupled. The slice-1 machinery above is correct and reusable; it was reverted
only because, without one of these two, it cannot move a corpus fixture and so cannot
be sweep-verified as doing anything.

#### DEEPEST trace: the handler-in-helper attempt (reverted) -- the full E2a landing chain

The handler-in-helper path was built end-to-end against a synthetic fixture
(`run` handles Write in a helper; `main` just calls `run`). This got the furthest
yet -- the threading MACHINERY works -- and it exposed the complete chain of
admission gaps E2a must close. Three were SOLVED in the attempt; a fourth remains.

Order of blockers hit, each fixed before the next surfaced:

1. **Whole-body delegation intercepts the HOF (SOLVED).** `call-writer`'s body
   `(f "...")` never reached `cps_tail` -- `cps_ir_translate_fn` whole-body-delegates
   any body whose leaf is a fn-value call (`safe_to_delegate` returns true for an
   effectful fn-value call with no enclosing handle). Fix: `safe_to_delegate`'s
   EX_CALL returns false for a `cps_ir_thread_param_has(fn)` callee, so a thread-param
   call takes the per-node path. After this, `[e2-call] cur=call-writer effv=1` fires.
2. **The threaded call emits (SOLVED).** With the call reaching `cps_tail`, the
   `via_registry` CT_TAILCALL + `((int64_t(*)(int64_t,DK*))__tur_cps_lookup((intptr_t)
   f))(arg, __kont)` emission + the per-lambda registration constructor all work.
3. **The `main` taint clears (SOLVED by the helper structure).** With the handler in
   `run` (a non-`sig_perm` helper), `main` no longer taints Write; the eviction
   category dropped from **SIG-TAINT** (permanent) to **BODY-STRUCT-OR-TAINT**
   (fixable), and `main` left the effect picture entirely. Confirmed the helper
   decoupling is real.
4. **REMAINING -- the handler fn fails `term_core_ok` on the "pass-the-lambda"
   admission.** `run` is classified `candidate=0` (traced: `unsupp=(none)`, so not a
   `CT_UNSUPPORTED` -- a structural `term_core_ok` reject). Pinpointed:
   `term_core_ok(run's CT_HANDLE)` -> `handle_delim_ok(delim)=0`. The delim is the
   cps->cps tail call `(call-writer __fn_1283)`, and `handle_delim_ok`'s CT_TAILCALL
   case -> `call_args_ok(call-writer, [__fn_1283], ...)` REJECTS the captureless
   fn-value ARGUMENT `__fn_1283` (a `TY_FN` atom). `run` then evicts, taints Write,
   and cascades the whole cluster back to fiber (BODY-STRUCT-OR-TAINT on `__fn_1283`,
   `call-writer`, `run`).

**So the true final gap is orthogonal to threading: passing a captureless effectful
fn-value as an ARGUMENT into a CPS-emitted call (here, into the handler's delim tail
call) is not admitted** (`atom_ok`/`slot_ty`/`call_args_ok` for a `TY_FN` arg). E2a
built the "CALL the fn-value through the DK" half; the missing half is "PASS the
fn-value into a DK-emitted callee." Both are needed for even one fixture:

- **E2a-call** (built, works): thread an effectful fn-value tail call via the
  registry. Gated by the `g_thread_params` coloring; `safe_to_delegate` +
  `cps_tail` + `via_registry` emission + registration.
- **E2a-pass** (the remaining gap): admit a captureless (`!is_poly_fn`) `TY_FN`
  atom as a scalar carrier arg through `atom_ok`/`call_args_ok`/`handle_delim_ok`,
  so the HOF-call that hands the lambda to the HOF is itself DK-emittable. This is a
  narrower, targeted admission change (a bare fn-ptr IS a scalar int64), but it
  touches the shared `atom_ok` path, so it must be gated and swept carefully -- it
  was NOT attempted, to avoid a broad unverified change.

Both halves plus the registry runtime are one atomic landing, verified by the sweep.
The machinery for E2a-call is proven; E2a-pass (fn-value-arg admission) is the last,
now-isolated piece. Reverted clean (default 2185/0); no code lands.

#### E2a LANDED -- the first effectful fn-value runs on the DK

Both halves landed together and the first effectful fn-value now threads the DK
instead of running on the fiber effect runtime.  Fixture
`tests/fixtures/cps-tramp-resume-e2a-fnvalue` (`call-writer` HOF + a captureless
`Write`-performing lambda + a helper `run` handler): under `--enable=cps-tramp-
resume` the emitted C carries `run__cps` (DK handler), the registration constructor
`__tur_e2reg___fn_NNNN`, and the threaded call `return ((int64_t(*)(int64_t,DK*))
__tur_cps_lookup((intptr_t)f))("threaded", __kont);` -- and ZERO fiber `Write`
performs (the only `tur_effect_perform` left is the unused preamble definition).
Output `threaded`, correct.

The landing is exactly the atomic change scoped above:
- **emit_dk_runtime.c** -- the `__tur_cps_register` / `__tur_cps_lookup` table (gated
  on `tramp`).
- **cps_ir.h / cps_ir.c** -- the `via_registry` CT_TAILCALL flag; the
  `g_thread_params` set + API; `cps_tail`'s eviction emits a `via_registry` tailcall
  for a thread-param callee; `safe_to_delegate` refuses to whole-body-delegate a
  thread-param call (so it takes the per-node path).
- **emit_cps_ir.c** -- `param_thread_class`'s concrete-effect gate (row-poly ->
  PT_E1); `param_is_thread_safe` (the param->value converse); the tier-`now`
  `g_threadable_fn` + `g_thread_params` population; `sig_perm` relaxed for a
  threadable lambda; the `via_registry` tailcall emission (null-terminated cast --
  `buf_write` does NOT NUL-terminate, which was the one codegen crash); the per-lambda
  registration constructor; and **E2a-pass** -- `atom_ok` admits a captureless
  (`!is_poly_fn`) `TY_FN` atom as a scalar carrier (gated on the flag).

Verified: flag-on sweep **185 / 0 build-fails / 0 mismatches**; default suite 2185/0
(flag-off byte-identical -- every change is gated on `g_opt_cps_tramp_resume`).

**Scope of this first slice:** tier-`now` only -- a captureless (bare int64 fn-ptr)
effectful lambda, passed to a HOF whose param is a concrete-effect, thread-safe,
tail-only threading param, whose effect handler is CPS-emittable (a helper, not
`main`).  Confirmed general within that scope: single-arg (`cps-tramp-resume-e2a-
fnvalue`) and multi-arg (`cps-tramp-resume-e2a-fnvalue-multiarg`) both thread and
run correctly.  The keystone is proven in the codegen now, not just a probe: **an
effectful fn-value can leave the fiber.**

#### Remaining E2 surface -- ordered, each reuses the E2a machinery

Each slice below sits on top of the landed tier-`now` machinery (registry, coloring,
`via_registry` tailcall, E2a-pass atom admission), gated by the same coloring +
sweep.  Ordered by tractability, with the specific new work each needs:

1. **tier `nontail`** -- a non-tail fn-value call (`{(f) * 2} + base`) -- **LANDED
   (handle-free HOF).**  The CPS pass produces a `CT_LETCONT { j, jbody=rest,
   body=via_registry CT_TAILCALL(kont=KK_VAR j) }` (the colored-callee non-tail shape
   at `cps_bind`), `letcont_is_heap_join` admits a `via_registry` body, and
   `emit_heap_join` threads the reified join frame to `__tur_cps_lookup(f)` instead of
   `<fn>__cps`.  Result: `cps-backend-effectful-callback` (`apply-cb [f base] {{(f) *
   2} + base}`, handler in helper `run`) now threads its non-tail call -- zero fiber
   performs, prints `27`.  Regression fixture `cps-tramp-resume-e2a-fnvalue-nontail`.
   Flag-on sweep 187/0/0.
   REMAINING refinement -- a non-tail fn-value call inside a HOF that ALSO installs a
   `handle` (`use-writer [f] (let [n (handle (g)...)] (do (f "hi") n))`) sits in the
   handle's LIFTED continuation frame, which does not capture the fn-value param, so
   the threaded `__tur_cps_lookup(f)` references an out-of-scope `f` (build error).
   GUARDED for now: `param_thread_class` tiers such a param `PT_E1` (via
   `expr_has_handle(fd->body)`), so it stays fiber.  The fix is to make the capture
   collector (`collect_caps`/`has_capture_rec`) carry a `via_registry` tailcall's
   callee as a captured var so the lifted frame's env holds `f`.
   NOT BOUNDED (assessed): the capture gate itself rejects this callee.  A captureless
   effectful fn-value param is NOT `is_poly_fn` (the whole E2a premise -- it rides as a
   bare `int64` direct-entry fn-ptr, not a `tur_poly_fn_t`), so in `cap_add` the
   `is_poly`/`borrowed`/single-shot escapes are all false and it falls to
   `cap_ty_ok(TY_FN, ...)` = `slot_ty(TY_FN) || slot_box_ty(type)` -- and `slot_ty(TY_FN)`
   is false, so `cap_add` sets `cs->ok = false` (capture fails).  Carrying `f` on the
   lifted frame's env therefore requires WIDENING the capture gate to admit a bare-int64
   fn-ptr scalar carrier for a captureless `TY_FN` param: `cap_ty_ok`/`cap_add` (accept
   it), `cap_ctype` (env-field type = `int64_t`), and `emit_heap_join` (read `env->fN`,
   not raw `f`, for the `__tur_cps_lookup` arg).  That touches the capture gate used by
   every lifted continuation -- not a ~1-fixture-sized change -- so the PT_E1 guard
   (sound: the HOF stays on the fiber) is retained deliberately.  Fold this into E2b,
   which widens the fn-value channel anyway.
2. **named address-taken fns** (not just lifted lambdas).  New work: a no-direct-
   escaping-call guard -- a lambda is never called directly so relaxing it is
   automatically safe, but a named fn CAN be `(cb x)`-called under a handle, whose
   direct entry now installs a fresh root and escapes.  Admit a named fn only when it
   has NO direct-call site (or its direct calls are DK-safe).  Low corpus value now
   (all corpus tier-`now` fn-values are lambdas).
3. **E3' -- `main` d2b for a `handle` body** (the real corpus lever) -- **LANDED.**
   Most corpus fixtures put the handler in `main`; `main` was `sig_perm` (SIG-MAIN,
   not d2b) so its handle ran on the fiber and tainted the effect, blocking the whole
   chain even though E2a can now thread the performers.  The circular fixpoint
   resolved more simply than feared: `fn_is_d2b_main` now returns true for a handle-
   body main (under the flag) as a d2b CANDIDATE, and the EXISTING architecture does
   the rest -- `emit_cps_ir_try_fn` only reaches the d2b `int main` wrapper when the
   main is `in_s`, and the taint fixpoint drops a handle-main whose subtree reaches
   fiber code (so `try_fn` returns false BEFORE the wrapper and the direct/fiber main
   is emitted -- its historical path, no forced-d2b regression).  The gate IS the
   fixpoint; the aggressive-E3 SIGSEGV is avoided because nothing is FORCED d2b.
   Change: an `expr_has_handle` helper + `... || (g_opt_cps_tramp_resume &&
   expr_has_handle(fd->body))` in `fn_is_d2b_main`.  Result: `effect-fn-type-annot`
   (a real corpus fixture) now emits `main__cps` + the E2a threaded fn-value and runs
   FULLY on the DK (correct output, zero real fiber `Write` performs); **32 handle-
   containing fixtures now emit a DK `main`** (handle-mains were never d2b before).
   Verified: flag-on sweep 187/0/0; default 2188/0 (flag-off byte-identical).
4. **E2b -- `is_poly_fn` / capturing fn-values** (tier `e1`) -- **CALL/CAPTURE-ONLY
   SLICE LANDED.**  The corpus fixtures (`cps-backend-capture-fnvalue`,
   `-indirect-call`) carry a fat-closure (`tur_poly_fn_t`) fn-value param that is only
   CALLED / captured-then-called -- NOT an effectful fn-value threaded through the DK, so
   they need neither the registry (E2a) nor the `fn_cps` channel: the emission machinery
   (emit_params `tur_poly_fn_t` spelling, `cap_add` is_poly_fn env field, CT_LETRAW
   delegated indirect call) was already in place; only `fn_sig_ok`'s blanket `is_poly_fn`
   reject blocked them.  Relaxed under the flag for a single-concrete-signature fn, guarded
   by `fatparam_only_called` (the fat param is used ONLY as a callee -- `ptc_walk` `val ==
   0`; a fn that THREADS the closure as an arg safely SIG-REJECTs, since a fat closure
   cannot cross a one-word slot / the caller/callee is_poly_fn ABI can diverge).  Moved both
   fixtures onto the DK (42 -> 40 real, SIG-REJECT 4 -> 2).  Regression fixtures
   `cps-tramp-resume-e2b-{capture-fnvalue,indirect-call}`.
   STILL FUTURE WORK -- an EFFECTFUL fat-closure fn-value THREADED through the DK (the
   `tur_poly_fn_t.fn_cps` / fat-box `__fn_cps` channel, kill-probe-proven): 0 in the current
   corpus, so not landed.  Also exposed + fixed a latent flag-on bug in the E1 by-value-
   aggregate-param slice: a defdata RECORD param is passed BY POINTER by the direct emitter
   (`type_struct_pass_by_ptr`), so `fn_byval_agg_param_ok` now excludes by-pointer aggregates.

#### Current landscape (measured after E2a + E3' + tier-nontail + E1-byval-param + E1-carrier-ret)

Measured with the flag on across the whole corpus via the `TUR_TRACE_EVICT` readiness
trace (`eff=1` = an eviction that actually keeps the fiber effect runtime alive):

```
$ for f in tests/fixtures/*/; do TUR_TRACE_EVICT=1 tur emit-c --enable=cps-tramp-resume \
    "$f/input.tur" 2>&1 >/dev/null | grep 'eff=1'; done | count-by-category
  57  SIG-TAINT            (downstream: de-taints for free when a root below goes DK)
  10  SIG-REJECT           (E1 -- remaining non-scalar signatures; the biggest ROOT)
   7  BODY-STRUCT-OR-TAINT (downstream of the row-poly / fn-value / while roots)
   3  BODY-UNSUPPORTED     (1 EX_WHILE + 2 guarded fn-value-in-handle)
   2  SIG-INLINE-C         (PERMANENT -- inline-C can't thread a DK cont; out of scope)
   2  SIG-EXPORT           (exported typeclass instances; sig_perm)
```

**49 fixtures still keep the fiber alive** (was 52 before the E1 slices below).  The
SIG-TAINT bucket (57) is entirely DOWNSTREAM -- each entry is evicted only because it
shares an effect with one of the permanent/root sources below, and the taint fixpoint
releases it automatically once that source goes DK.

> **CORRECTION -- the `eff=1` count OVER-reports fiber liveness (6 of the 49 are
> non-blockers).**  `eff=1` fires on any fn that performs or handles an effect *tag* --
> but `Unsafe` is a COMPILE-TIME marker, not a fiber effect: `(unsafe expr)` is
> discharged inline and emits ZERO fiber/perform/handler calls (verified -- the direct
> emit of `sized-buf-*` is a plain `raw(x); return;`, no `Unsafe` C symbol at all).  So
> the **6 `Unsafe`-marker-only fixtures are already fiber-free** and are NOT deletion
> blockers: `sized-buf-cross-param-accept`, `generic-inline-c-struct-through-unsafe`,
> `free-interpreter`, `free-lift-bind`, `typeclass-unsafe-passbyptr-struct-arg`,
> `unsafe-closure-capture`.  **The true count is 43 real fiber-live fixtures.**  Do NOT
> spend E1 effort chasing the `sized-buf`/`dense` shape onto the DK -- an opaque-carrier
> + `k`-param widening was built and REVERTED once this was found: it admitted their
> signatures but the fixtures stayed fiber (their inline-C `*-raw` helpers permanently
> taint `Unsafe`), and moving them changes nothing at runtime.  Filter `eff=1` by a real
> `defeffect`/`perform`/`handle`/`shift`/`reset` before treating an eviction as a blocker.

The real remaining ROOTS among the **43 real** fiber-live fixtures, largest-lever first
(SIG-REJECT 4: `exchange`/`role-a` session effects + `f`x2 fat-closure fn-values;
BODY-STRUCT-OR-TAINT 7; BODY-UNSUPPORTED 3; SIG-INLINE-C 2 + SIG-EXPORT 1 permanent):

- **E1 -- non-scalar signatures (10 SIG-REJECT remaining).**  The single biggest
  fixable root.  **Two sub-families LANDED**, both via the shared
  `fn_single_concrete_sig` guard (not an instance method / dict-clone / constrained --
  exactly the fns the sig_slot_ok name-collision hazard targets; a plain concrete defn
  has one C signature the direct emitter also emits):
    - owning-free by-value aggregate PARAMs (`fn_byval_agg_param_ok`) -- `fn_sig_ok`
      admits a `slot_box_ty` param, matching the direct emitter's by-value spelling.
      Moved `effect-handler-capture-struct` (`run [c : Cfg]`) +
      `cps-backend-capture-nonscalar`.  Fixture `cps-tramp-resume-e1-byval-struct-param`.
    - heap-ADT/struct HANDLE returns (`fn_carrier_ret_ok`) -- `fn_sig_ok`'s return gate
      admits a `carrier_handle_ok` return; the `__cps` entry returns int64_t and
      delivers the handle through the DK slot by a `(T *)__r` cast (machinery already
      in slot_ok_t).  Moved `cps-backend-heap-adt-return` (`mkvec [] : (Vec int)`).
      ASan-verified move-out (no double-free).  Fixture
      `cps-tramp-resume-e1-heap-adt-return`.
  REMAINING **real-effect** E1 shapes, each its own per-shape slice: fat-closure
  `is_poly_fn` fn-value params (`cps-backend-capture-fnvalue`, `-indirect-call` -> E2b);
  session-typed effects (`session-effects` `exchange`, `session-mp-effects` `role-a`).
  NOTE: the `sized-buf-*` / `dense-*` opaque-carrier shape is NOT a real target -- those
  fixtures are `Unsafe`-marker-only (already fiber-free; see the CORRECTION above).  An
  opaque-carrier param/return admission (`type_is_opaque_carrier` + a `k`-param widening)
  WAS built and reverted: sound, but moved 0 real fixtures (their inline-C `*-raw`
  helpers permanently taint `Unsafe`, keeping the wrappers SIG-TAINT regardless).
- **native CPS loop-lowering (1 EX_WHILE root: `effect-handler-capture-loop`).**  LANDED
  (2026-07-17) as `CT_LOOP`/`CT_CONTINUE`: an `EX_WHILE` with an interior control op lowers
  to a synthesized tail-recursive colored `__cps` helper (params = the `^mut` loop vars,
  back-edge = a `CT_TAILCALL`-style re-entry, exit = deliver the live-after var to KK_RET),
  gated on `--enable=cps-tramp-resume`.  `run` emits `run__cps` (zero `eff=1`), prints `100`;
  default suite `2200 passed, 0 failed` (flag-off byte-identical).  Reads resolve to the
  loop-entry version by naming and `set!` writes pre-created `$next` CVars (build-order
  safe); a conservative `loop_guard` evicts read-after-set / conditional-set / multi-live
  shapes.  Regression fixtures: `cps-tramp-resume-while-handle`,
  `-while-handle-escape`, `-while-readset`.  Paper trail:
  `docs/archive/history/cps-while-loop-with-interior-handle-no-native-lowering.md`.
  **Residue (endgame blocker):** the subset is conservative, so read-after-set,
  conditional/multi-var `set!`, multiple live-after vars, nested loops, and an
  ESCAPING interior effect still evict `eff=1` to the fiber -- they must DK-lower
  before the fiber runtime can be deleted.  Filed:
  `docs/reported/cps-while-native-conservative-subset-fiber-residue.md`.  Below
  is the pre-landing analysis (retained for context):

  Flag-
  on DE-TAINTED `run` (its self-contained handle no longer shares a tainted effect), so
  it is now a d2b candidate whose ONLY blocker is the raw `EX_WHILE` in its body -- the
  CPS transform has no loop lowering (task-15's "EX_WHILE" was whole-body *delegation*
  to the FIBER direct emitter, which does not move it onto the DK).  A true DK landing
  needs a loop join point with the `^mut` loop vars threaded as loop-carried
  continuation args.  Real slice -- and BIGGER than a lone `case EX_WHILE`: measured,
  the CPS IR has NO mutation representation (`atom_of` maps a var directly to its source
  `Binding`; no rebinding map in `CpsB`) and `CT_LETCONT` is SINGLE-param (no multi-arg
  loop join), and `emit_heap_join` has no self-recursive loop emission.  **SUPERSEDING
  (2026-07-17): the multi-arg-loop-join design is RETRACTED.**  Pinned against
  `emit_handle` (`emit_cps_ir.c:5438`): it lifts the handle continuation into a SEPARATE
  C function, and this loop's carried update (`total += <handle result>`) is produced
  inside that lifted continuation -- so a same-function `goto` back-edge is impossible.
  The loop is FORCED to be a synthesized recursive colored `__cps` function whose
  back-edge is an ordinary `CT_TAILCALL` (already lowered by `emit_term`).  That removes
  the new-emitter-construct risk but adds the real cost: SYNTHESIZE a `Binding`/`FnDef`
  for the loop fn and INJECT it into the classifier so `binding_in_s`/forward-decls/
  emission pick it up (`emit_cps_ir.c:3442-3670,5870,5980-6230`), plus the `EX_WHILE`
  transform with loop-scoped `set!`/`^mut` rebinding under the loop-entry-version-only
  guard.  Still no sound partial moves the fixture.  Also ruled out this session: a
  `CT_LETRAW` delegation of the self-contained region -- it runs on
  `global_effect_handler_chain` (non-DK), which the plan wants deleted too, so it does
  not put `run` on the DK.  Full scope + the superseding shape:
  `docs/reported/cps-while-loop-with-interior-handle-no-native-lowering.md`.
- **E2b / tier-nontail-in-handle (2 BODY-UNSUPPORTED fn-value).**  The guarded
  `cps-backend-fn-param-effectful` + `handle-effectful-fn-param-same-fn`; both need the
  capture-gate widening recorded under roadmap item 1 (fold into E2b).
- **BODY-STRUCT-OR-TAINT (7 real).**  These are COMPOUND -- each has more than one cause,
  so no single fix moves them yet.  One LAYER is now cleared: a PURE delegated call in a
  handled body's reified continuation (`(println (add-int 3 4))` after a colored call) used
  to evict because `handle_delim_ok` had no `CT_LETRAW` case and the delegated op fell to
  `term_core_ok`, whose `CT_APPCONT` gate rejects the following `KK_PROMPT` deliver (a
  `term_core_ok`/`first_unsupported` INCONSISTENCY -- see the minimal repro in
  docs/archive/cps-delegated-call-in-heap-join-cont-evicts.md).  `letraw_effect_free` now
  admits the pure case (effectful delegated calls still evict -- handle-effectful-fn-param-
  same-fn unchanged), always-on.  A SECOND layer is now cleared too: `effect-row-poly` -- a `#{e}` ROW-VARIABLE call that
  is runtime-PURE (`twice [x] #fx{e} = (+ x x)`) used to read as effectful in
  `callee_effect_free` (declared row only).  `callee_effect_free` now falls back to the
  INFERRED row (the sound runtime-effect summary), so a pure row-variable call is admitted;
  `effect-row-poly` moved FULLY onto the DK (43 -> 42 real, BODY-STRUCT-OR-TAINT 7 -> 5).
  Regression fixture `cps-rowvar-pure-call-in-handle`.  REMAINING per-fixture causes:
  `effect-subtype-capability` -- an EFFECTFUL fn-value stored in a struct field and called
  via `(.run act ...)` (E2-adjacent, harder than E2a's param case); `effect-reopen` --
  nested re-handled effects; the owning-struct-field-op-capture pair.  Each is its own slice.
- **SIG-EXPORT (2), SIG-INLINE-C (2).**  Exported typeclass instances stay sig_perm;
  inline-C bodies are permanent (can't thread a DK cont) -- out of scope for deletion.

No bounded corpus-moving slice remains: every root above is a substantial, per-shape
piece of work.  The two E1 easy wins (by-value-aggregate params, heap-ADT returns) are
LANDED; the remaining real blockers are E2b (fat-closure fn-value channel), session-typed
effects, native CPS loop-lowering, and the compound BODY-STRUCT-OR-TAINT causes above.
The invariant held throughout this session's landings: default `2190 passed, 0 failed`
(flag-off byte-identical), flag-on sweep clean.

#### `effect-reopen` LANDED -- the first compound BODY-STRUCT-OR-TAINT root moves to the DK

One of the "compound BODY-STRUCT-OR-TAINT causes above" -- `effect-reopen` (nested
handle + effect re-opening + a colored call in a perform continuation) -- now
DK-lowers fully under the flag (`start`/`done`/`142`, no `eff=1` eviction).  Its
admission had THREE stacked gaps, all fixed together (gated on
`--enable=cps-tramp-resume`; flag-off byte-identical -- `dk_hgroup`/`hgroup` absent
from default emit):

1. **`println` in a perform continuation** -- `perform_body_ok` /
   `perform_cont_reset_ok` rejected `is_println_shape` (stale conservatism;
   `handle_case_ok` already admits it through the same `emit_term` frame path).
   Admitted under the flag.
2. **Heap join whose jbody performs** (resolves
   `docs/archive/cps-perform-cont-heap-join-eviction.md`) -- `emit_heap_join`'s
   `needs_kont` was `jbody_has_cps_tailcall` only, so a join whose jbody PERFORMS
   lifted as a value-only `LH_PERFORM_CONT` frame with no `__kont` and the interior
   `dk_perform` referenced an undeclared `__kont`.  New `jbody_has_perform` promotes
   it to `LH_RESUME_CONT`.
3. **Re-opening across a nested handle with a multi-suspension continuation** -- a
   pre-existing defect in the re-opening runtime (`dk_case_enclosing`, commit
   `ffd878897`), dormant because `effect-reopen` had always evicted so the
   re-opening DK path NEVER RAN.  `dk_case_enclosing` skipped "consecutive
   `DKK_HANDLER`" to find enclosing handlers, but `dk_perform`'s re-install flattens
   the chain (drops inter-handle frames), so an enclosing handle's handler becomes
   adjacent and got wrongly skipped -> a re-opened outer effect in the resumed
   continuation escaped (`unhandled effect`).  Fix: a per-handle group id
   (`dk_hgroup` stamps a handle's sibling cases; `dk_case_enclosing` and the
   re-install skip only same-`hgroup` handlers).  Robust for any nesting depth.

Verified: `effect-reopen` output matches `expected.stdout`; bisection fixtures
(single-handle heap-join-perform; nested re-opening inline; two sequential
re-opening performs; single-effect repro `142`) all correct; regression fixture
`tests/fixtures/cps-tramp-resume-reopen`.  Known follow-up (does NOT block; compiled
fixtures are not leak-checked by the suite): the re-opening DK path leaks DK nodes
O(N) per re-opened perform (the trampoline yield branch never frees its `sub`) --
filed `docs/reported/cps-reopen-perform-onode-leak.md`.

This clears ONE of the compound BODY-STRUCT-OR-TAINT roots.  Remaining in that
bucket (each still its own slice): `effect-subtype-capability` (effectful fn-value
in a struct field, called via `.run`), the owning-struct-field-op-capture pair.
The larger roots (E2b fat-closure fn-value channel, session-typed effects, native
CPS loop-lowering) are unchanged.

---

**Bottom line:** the deletion is achievable iff effectful fn-values can thread the
DK (E2). Prove that with the kill-probe first. If it holds, Stages A-F are an
ordered, gated grind with the v1 hard-error gate protecting every step, ending in
the Stage-G deletion. If it does not hold, we have found the wall that decides the
project -- and we will know it in one focused probe rather than after months.

### 9a. KILL-PROBE RESULT -- GO (with a refinement)

Run: `docs/upcoming/v2/probes/e2-killprobe.c` (embeds the verbatim DK runtime from
`src/compiler/emit_dk_runtime.c`); output captured in `probes/e2-killprobe.out`.
Build/run: `cc -O2 -o /tmp/e2probe docs/upcoming/v2/probes/e2-killprobe.c && /tmp/e2probe`.

The probe builds the exact E2 scenario -- a tail-recursive effectful loop whose
callback is reached **indirectly through a fat-closure `fn_cps(void*, int64_t, DK*)`
slot** (the precise E2 ABI addition), under a `dk_handler` that tail-resumes -- two
ways: the current inline-resume model and a trampolined tail-resume.

| Question (Sec 9) | Result |
|---|---|
| (a) Does the callback's `perform`, reached through the fn-value `__fn_cps` slot, find the CALLER's handler? | **YES** -- `side_sum` matches `sum(i+1)` exactly; handler search crosses the fn-value. No per-call prompt. |
| (b) Does deep (1e6) tail-resume recursion stay flat? | **YES**, trampolined: `max_stack = 263 bytes`, constant in N. |
| (b') Is the *current* inline-resume model O(N) stack? | **YES** -- 160 B/element -> ~153 MB at 1e6 (guaranteed SIGSEGV). |

**Verdict: E2 is NOT impossible as framed** -- the fat-closure box can carry a
`DK*`-threading thunk that composes with `dk_perform` with no per-call prompt and
no broken handler search. **The project is not dead at this gate.**

**Refinement forced by the probe:** the plan's premise that "the DK model is
already stackless" was **wrong for resumptive recursion**. The current DK resume is
O(N) C stack; the fiber runtime is presently the only thing keeping deep effectful
tail-recursion flat. Deleting it therefore REQUIRES a **trampolined tail-resume**
first -- added as enabler **E7 (Stage 0)** above. E2's fn-value plumbing is the easy
half; E7 is the load-bearing runtime change, and it is proven feasible end-to-end
by version B of this same probe.
