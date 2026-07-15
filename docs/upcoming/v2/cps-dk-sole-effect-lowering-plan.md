---
title: "v2 -- CPS/DK as the sole effect lowering; delete the fiber effect runtime"
status: open -- THE defining track. Nothing else ships until this lands or is proven impossible.
severity: existential. If the fiber effect runtime cannot be deleted, this project is dead.
---

# v2 -- Make CPS/DK the sole lowering for effectful colored code, then delete the fiber effect runtime

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
false.** The deletion is safe when that holds.

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
  reclassify it as one (**E5**); no runtime effect migration needed. (The two
  corpus instances are also mains -> also covered by E3.)

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
   config). This makes "no effect on a fiber" a compiler invariant.
2. Delete the `emit_effects.c` direct emitters (Sec 2a) and their `emit_value`
   dispatch. Replace with a hard error ("effect reached the direct emitter -- v2
   invariant violated") so any resurrection is caught.
3. Delete the fiber effect runtime C from `emit_module.c` (Sec 2a): `tur_effect_perform`,
   `EffectHandlerFrame`/`Case`, `global_effect_handler_chain`, `tur_handler_dispatch`
   + msdyn, `tur_effect_cont_*`, `tur_handler_table_t`, and the two `FiberBlock`
   effect fields. Keep `FiberBlock` (concurrency) and `tur_cloneable_cont` (DK
   bridge).
4. Verify (Sec 8).

---

## 7. What "done" looks like

- `grep -c 'tur_effect_perform\|global_effect_handler_chain\|EffectHandlerFrame'`
  in any emitted `.c` = **0**.
- `emit_effects.c` contains only the delimited-control passthroughs; the direct
  perform/handle/resume emitters are gone.
- `emit_cps_ir_try_fn` never returns false for an effectful colored function; the
  invariant is asserted at build time.
- Full suite green (2179/0), ASan clean, stackless probe green.
- The CPS/DK backend is the **sole effect lowering**. (Pure indirect-call-colored
  functions may still direct-emit -- that is correct and touches no effect
  runtime.)

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
This is what Stages C-F now cover concretely. **Next: drive the 39 effectful BODY
roots to DK-admission under the flag (largest bucket, same technique as v1), then
E1 for the 8 non-scalar signatures, then E5, then the taint empties and Stage G
(deletion) is unblocked.**

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
