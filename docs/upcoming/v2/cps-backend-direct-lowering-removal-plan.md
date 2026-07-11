---
title: "Removing the direct lowering -- retiring emit_cps.c after cps-backend graduation"
status: proposed
parent: cps-backend-unification-plan.md
description: The cps-backend experiment graduated (2026-07-11) -- the CT-IR CPS backend is now the default lowering for every emittable colored function, with the direct emitter (emit_cps.c) kept only as the eviction fallback. This plan sequences the SECOND, larger milestone: deleting the direct-style delimited-control LOWERING entirely. It is not one delete. Two distinct caller populations still reach the four lowering functions -- colored functions that evict residual shapes via CT_LETRAW, and uncolored/main/exported functions that dispatch straight to them because they are never CPS-emitted at all. This note names both, sequences closing them, and lists the exact deletes that follow.
---

# Removing the direct lowering (emit_cps.c)

## Where we are: graduation landed, deletion did not

The `cps-backend` experiment **graduated** on 2026-07-11
([graduation-readiness note](cps-backend-unification-graduation-readiness.md)):

- The row is gone from `EXPERIMENTS[]` (`src/runtime/experiments.c`); the
  `g_opt_cps_backend` gate is retired (removed from `globals.{c,h}`);
  `emit_cps_ir_try_fn` / `emit_cps_ir_program_has_emittable`
  (`src/compiler/emit_cps_ir.c`) run **unconditionally**.
- Every emittable colored function now lowers through the CT-IR CPS backend by
  default; the direct emitter stays as the **eviction fallback** only.
- `--enable=cps-backend` is an accept-and-warn no-op (TUR-W0063; the
  `GRADUATED[]` table in `experiments.c`), so downstream `build.tur` /
  `experiments.tur` that opted in keep compiling.

Graduation was **milestone 1** ("become the default"). It was verified faithful:
the flag-off-by-default output is byte-identical to the old
`--enable=cps-backend` output across the corpus (139 `expected.c` snapshots
regenerated; zero behavior failures -- this matches the readiness probe's
"278 codegen-mismatch, 0 behavior" result).

This plan is **milestone 2** ("retire the direct lowering"). Graduation did
**not** by itself delete anything: making the CPS backend the default for
*colored* functions did not remove either population of callers into the
direct-style delimited-control lowering. Those callers are the subject of this
note.

## The delete target: what "direct lowering" is

`emit_cps.c` (~2.1k lines) already had its **runtime preludes relocated** out to
`emit_dk_runtime.{c,h}` (U7 step 1, landed -- see the
[U7 readiness note](cps-backend-unification-u7-readiness-plan.md)). What remains
in `emit_cps.c` is exactly the direct-style **lowering** this plan deletes, plus
its private analysis helpers and the syntactic uses-gates:

| Symbol | `emit_cps.c` anchor | Role |
|---|---|---|
| `emit_cps_reset` | :446 (`emit_cps_reset_escape` :406) | base reset/shift lowering (DK + setjmp escape) |
| `emit_cps_callcc` | :488 | call/cc + escape lowering (local setjmp landing) |
| `emit_cps_cloneable_reset` | :1309 | multi-shot cloneable lowering (`dk_copy_range` + clone/drop glue) |
| `emit_cps_serial_reset` | :1630 | serial marshaling lowering (`__sk_frame_for_tag`, `SkReg`) |
| `frame_c_expr`, `collect_ctx`, `ctx_if_branch`, `cl_can_lower`, `sk_tag_for_frame` | :728, :752, :988, :1116, :1480 | private analysis helpers (die with the lowering fns) |
| `emit_cps_program_uses_delimited` / `_callcc` / `_cloneable_dk` | :85, :201, :1414 | syntactic form-presence prelude gates |

Reached through:

- **Dispatch wrappers** in `emit_effects.c`: `emit_effects_reset` (:1209),
  `emit_effects_cloneable_reset` (:1223), `emit_effects_serial_reset` (:1696) --
  each calls its `emit_cps_*` fn, falling back to the legacy direct lowering when
  it returns NULL.
- **`EX_CALLCC` direct dispatch** in `emit_expr.c` (:2826):
  `case EX_CALLCC: return emit_cps_callcc(...)`.
- **`CT_LETRAW` delegation** from colored functions in `src/passes/cps_ir.c`
  (`safe_to_delegate` :888-948; the `cps_bind`/`cps_tail` cases :1259-1341,
  :1460-1499): a colored function evicts a residual delimited *sub-shape* to the
  direct emitter, which routes through `emit_value` -> the `emit_effects_*`
  wrappers -> the `emit_cps_*` lowering.
- **Prelude gating** in `emit_module.c` (:6647-6651, :6727-6766) still reads the
  syntactic `emit_cps_program_uses_*` gates.

Deleting the lowering means driving **both** the dispatch-wrapper population and
the CT_LETRAW population to zero, then removing the functions, the wrappers, the
`EX_CALLCC` dispatch, the delegation arms, the gates, and finally the files.

## Why graduation did not delete it: two caller populations

### Population 1 -- colored-function eviction residue (CT_LETRAW)

A colored function is CPS-emitted, but a delimited *sub-shape* it contains may
still fall outside the native CT-IR subset and delegate to the direct lowering
via `CT_LETRAW`. The U7 readiness table established that every **named** native
gap is closed; the residual delegations are narrower, un-named tails:

- **Colored (fat-closure) receivers** for cloneable / serial / callcc. The
  native path handles bare-fn and capture-free-closure receivers; a *colored*
  receiver still delegates (the direct emitter miscompiles it too, so this is
  bug-compatible delegation, not a correctness gap).
- **Serial 2-arg call frames with `cstr` / `Serializable` envs.** The int-env
  2-arg frame marshals inline natively; the non-int env still delegates (real new
  marshaler machinery).
- **Shapes outside the supported context grammar** -- contexts `collect_ctx` /
  the `build_marshal_reset` walk do not yet reify.
- **The generic `needs_heap_join` boundary** -- a non-tail cps->cps *call* on the
  heap chain. This is **not** delimited-control-specific: it is the whole C1
  emittable-subset boundary, shared with the five emittable-subset gap plans
  ([non-scalar-values](../v1/cps-backend-non-scalar-values-plan.md),
  [owning-pointers](../v1/cps-backend-owning-pointers-plan.md),
  [tier-c-crossing](../v1/cps-backend-tier-c-crossing-plan.md),
  [effectful-callbacks](../v1/cps-backend-effectful-callbacks-plan.md),
  [generic-monomorph-classification](../v1/cps-backend-generic-monomorph-classification-plan.md)).

### Population 2 -- uncolored / `main` / exported direct dispatch

This is the population graduation left entirely untouched, and the true blocker.
A function that is **not** effect-colored -- including `main` and any exported
function -- is **never CPS-emitted**: the classifier does not route it to
`emit_cps_ir_try_fn`, and `main` / exported entry points carry a fixed C ABI
(`int main(void)`, `f(args)`), not the `f__cps(args, DK *k)` + direct-entry
wrapper shape the CPS backend produces. So when such a function uses
`reset`/`shift`/`shift0`, `call/cc`/`escape`, or `cloneable`/`serial` reset, it
dispatches **straight** to the direct lowering through the `emit_effects_*`
wrappers and the `EX_CALLCC` case -- with no CT-IR involvement to evict *from*.

Graduation changed nothing here: it flipped the default *for colored functions*.
An uncolored `(defn main [] (reset (shift k (k 1))))` takes the direct lowering
before and after graduation, identically. **This population is why the four
lowering functions still have live callers even at 100% colored-function native
coverage.**

## The sequenced work

The two populations close independently; neither blocks the other. Each closes
by *extending native coverage*, verified per-shape (`direct == cps`), then the
now-dead caller is removed.

### Phase D1 -- close the colored-function eviction residue (Population 1) -- DONE

Extend native CT-IR emit until no colored function delegates a delimited shape.
**Complete: the Population-1 eviction residue is zero** -- every cloneable /
serial / callcc delimited shape a colored function contains now emits through the
native CT-IR path; no colored function delegates a delimited shape via CT_LETRAW.

- **D1a -- colored / lifted receivers.** Teach `emit_cloneable` / the callcc
  landing to emit a colored (fat-closure) receiver natively (the receiver runs
  once at capture; only the continuation is cloned/marshaled -- the multi-shot /
  serial semantics already established for closure receivers extend to colored
  ones).

  **Landed** for the one shape the corpus exercised: a capturing `escape` nested
  in a `shift` body's abort value (`cps-oracle-escape-capture-in-shift-body`).
  `safe_to_delegate` treated a call/cc / escape whose receiver `build_callcc` can
  emit (`callcc_native_recv` -- a capturing closure included) as delegatable, so
  the whole `(+ 1 (escape ...))` rode a CT_LETRAW delegation into `emit_cps.c`'s
  `setjmp`/`tur_escape_cont` lowering. Reporting such a call/cc *non*-delegatable
  forces the enclosing form to decompose, so the callcc lands at a bind position
  and lowers to a native `CT_CALLCC`; only a receiver `build_callcc` cannot emit
  still delegates. Closes the last eviction.
- **D1b -- serial `cstr`/`Serializable` envs.** Extend the inline env marshaler
  (`SK_ENV_INT` today) to the non-int env codec so a captured call-frame env
  round-trips through `save-cont!`/`resume-cont!` natively.

  **Landed.** The runtime marshaler already encoded all three env kinds
  (`SK_ENV_INT`/`_CSTR`/`_SER` in `emit_dk_runtime.c`, built for the direct
  emitter); the native CT-IR serial path now *produces* the matching `SkReg`
  entries. Covered shapes:
  - *do-tail captured-config frame* `(loop cfg)` -- a 1-arg tail whose argument
    is a captured value applied on resume (the resume value ignored). `int`
    (inline), `cstr` (length-prefixed), and `Serializable` (nominal, via its
    instance's serialize/deserialize) envs. Closes `serial-context-do-cfg`,
    `serial-context-do-struct`.
  - *2-arg hole-call frame* `(f other [])` with a `Serializable` env (in
    addition to `int`), including a non-atomic env (`(mk-rec ...)`) emit_value'd
    at the reset site (`CloneFrame.env_expr`). Only the hole slot must be `int`.
    Closes `serial-struct-env`.
  - The env-kind decision keys off the captured operand's type; the
    Serializable instance is gated at build time by a pure `cps_serializable_exists`
    scan (in the pass) and its serialize/deserialize C names resolved at emit
    time by `serial_env_ser_names` (in `emit_cps_ir.c`, mirroring
    `sk_find_serializable`) -- no coupling to `emit_cps.c`.
  - The `cloneable` side needed no marshaling for these: a captured env rides the
    `dk_frame` slot and is deep-cloned with the sub-continuation (2-arg cloneable
    call frames landed under D1c above).

  **CC4 value typing landed alongside.** A value-typed `cont<T>` whose result
  (and resumed value, and a call-frame env/operand) is a non-int scalar (`cstr`)
  now emits natively: `cstr` rides the same intptr_t carrier as int, so the
  builders admit any scalar kind (`cps_scalar_kind_ok`) for the call-frame
  result / hole-param / env, and the emitter casts to the real kind at each call
  boundary (`cc_cast_for_kind`) and casts the reset result to the binder's C
  type (`binder_ctype_full`) instead of `int64_t` -- both no-ops for int, so int
  fixtures stay byte-identical. This also unblocked the `cstr` 2-arg env that
  paired with the value typing. Closes `cont-value-typed`.
- **D1c -- context-grammar generalization.** Widen `collect_ctx` /
  `build_marshal_reset` to reify the remaining context shapes (the
  [marshal-reset unification](cps-backend-unification-marshal-reset-unification-plan.md)
  is the natural home -- one `build_marshal_reset(..., serial)` covering both
  families).

  **Partially landed** (in `build_cloneable`/`build_serial` + `emit_cloneable`):
  - *`let`+`if` mix.* Lifted the `nl > 0`-at-`if` rejection: `emit_cloneable`
    lays every `let` prelude local at the reset site ahead of the branch, so a
    `let` above the `if` is in scope for the outer frame operands and the pure
    arm. Also lifted the `saw_if`-at-`let` rejection: a `let` nested in the
    shift-bearing arm is hoisted to the reset site (sound -- its init is pure and
    scalar; a binding the shift body needs is a live capture the shift admission
    already rejects). Closes the eviction in `cloneable-context-if` /
    `-outer-frames`.
  - *2-arg call frames in cloneable contexts.* `build_cloneable`/`emit_cloneable`
    now reify a `(f other [])` / `(f [] other)` frame (uncolored (int,int)->int,
    int env) natively, matching `build_serial`. The env rides the `dk_frame`
    slot, deep-cloned with the sub-continuation on resume, so multi-shot is
    correct with no marshaling. Closes all four evictions in `context-call-frame`.

  The `serial` do-tail / 2-arg env-marshaling shapes are now handled here (see
  D1b); what remains delegating on the context-grammar axis is only the
  value-typed `cont<T>` shape, which is CC4 value-typing, not context grammar.
- **D1d -- the `needs_heap_join` boundary.** This is the shared C1-subset item;
  it advances through the five emittable-subset gap plans, not as
  delimited-specific work. D1 is *complete for delimited control* when D1a-D1c
  land; D1d raises the native fraction generally and is tracked there.

**Progress (eviction `-emit` reaches, corpus scan): Population 1 is at ZERO.**
D2b introduced 16 Population-1 evictions (cloneable/serial residue in CPS-emitted
mains); the D1c (let+if, let-under-if, 2-arg cloneable call frames), D1b (serial
cstr/Serializable do-tail + 2-arg env marshaling), CC4 (value-typed
`cont<cstr>`), and D1a (native callcc/escape with a `build_callcc`-emittable
receiver) work above drove the residue **18 -> 0**. No colored function delegates
a delimited shape any longer. The remaining direct-lowering `-emit` reaches in
the corpus (**41**) are entirely Population 2 (`direct-dispatch`): uncolored /
`main` / exported / subset-reject functions that wholly direct-emit -- chiefly
callcc-in-`main` (call/cc is not a coloring seed, so those mains never reach the
CPS classifier) and delimited helpers outside the emittable subset `S`.

**Exit:** with D1a-D1c landed **and Population 1 at zero**, the `CT_LETRAW`
delegation arms for `EX_RESET`/`EX_SHIFT`/`EX_SHIFT0`, `EX_CLONEABLE_RESET`,
`EX_SERIAL_RESET`, and `EX_CALLCC` in `safe_to_delegate` and the
`cps_bind`/`cps_tail` cases in `cps_ir.c` are now **dead for colored functions**
and can be removed (a mechanical cleanup, gated on re-confirming the zero with
`--dump-direct-lowering-callers`). (`EX_ASYNC`/`EX_AWAIT` stay delegated -- they
ride a *separate*
stackful fiber runtime in `emit_module.c`, untouched by the `emit_cps.c` delete;
see the [U5 async-substrate note](cps-backend-unification-u5-async-substrate-plan.md).
Their delegation targets the fiber runtime, not the four lowering functions, so
it is out of scope for this plan.)

### Phase D2 -- CPS-emit uncolored/`main`/exported delimited functions (Population 2)

Make the `f__cps(args, DK *k)` + direct-entry-wrapper shape available to a
function that uses delimited control **regardless of effect-coloring**:

- **D2a -- syntactic delimited-control CPS trigger.** Today the CPS-emit trigger
  is effect-coloring (`fd->cps_colored` / `se->in_s`). Add a second admission
  path: a function whose body syntactically contains `reset`/`shift`/`shift0`,
  `call/cc`/`escape`, or `cloneable`/`serial` reset is CPS-emitted even if it is
  not effect-colored. (The syntactic gates `emit_cps_program_uses_*` already
  compute exactly this predicate per-program; reuse the per-function form of it.)
  An uncolored function with *no* delimited control stays on the direct emitter
  as before -- CPS-emitting a genuinely pure function is pure overhead and must
  not be triggered.

  **Landed for `call/cc` / `escape` via coloring.** `reset`/`shift`/`shift0` and
  `cloneable`/`serial` reset were already coloring seeds (`cps_directly_uses_
  control` in `cps.c`), so a function using them was already classified. `call/cc`
  / `escape` was NOT a seed -- a function whose only control op was a call/cc /
  escape stayed uncolored and wholly direct-emitted through `emit_cps_callcc`.
  Adding `EX_CALLCC` to the seeds colors those functions; `ensure_S` classifies
  them and (native receiver + emittable body) CPS-emits with the call/cc lowered
  natively (`emit_callcc`), the callable symbol preserved by the direct-entry
  wrapper so uncolored callers (a `main` calling them) are unchanged. This closed
  the bulk of Population 2: **callcc direct-dispatch reaches 28 -> 2**.
- **D2b -- fixed-ABI entry wrappers.** The CPS backend already emits a
  direct-entry wrapper that preserves a colored function's callable symbol; make
  that wrapper cover the fixed entry-point ABIs -- `int main(void)` calling
  `main__cps(dk_done())`, and each exported `f(args)` calling `f__cps(args,
  dk_done())`. This is the mechanism the graduation-readiness note flagged as
  missing ("cannot take the `f__cps(args, DK*)` + wrapper shape").

  **Landed (main only; exported deferred).** `emit_cps_ir_try_fn` now admits a
  zero-arg `main` whose body directly uses delimited control and passes the same
  `fn_sig_ok`/`term_core_ok` subset gates (`fn_is_d2b_main` in `emit_cps_ir.c`;
  the `in_s` classifier lifts the `main` exclusion for it). For such a `main` the
  wrapper is a fixed-ABI `int main(int argc, char **argv)` that reproduces the
  direct emitter's `main` prologue (panic-trace flag + the `*args*` cons build),
  seeds the root prompt, trampolines into `main__cps`, and returns the delivered
  value as the exit code. Effect-only mains (no delimited op) stay excluded --
  CPS-emitting them is pure overhead and removes no direct-lowering caller.
  Exported (`c_export_name`) functions stay excluded: none appear in the corpus
  and the extern-linkage ABI is left as a follow-up.

  *Effect on the caller count (corpus scan): total genuine `-emit` reaches
  **109 -> 51**; distinct fixtures with any reach **43 -> 28**; **15** former
  Population-2 fixtures fully cleared.* Base `reset`/`shift` in a CPS-emitted
  `main` emit **natively** (caller removed). `cloneable`/`serial`/`callcc` in a
  CPS-emitted `main` mostly emit natively too, but a residual delegates via
  `CT_LETRAW` -- this **reclassifies** those reaches from Population 2 to
  Population 1 (the after-scan shows 7 `eviction cloneable` + 9 `eviction serial`
  where before there were none). D2b does not by itself zero those; closing them
  is D1's native-emission + delegation-removal work. `callcc`-only mains are not
  admitted (call/cc is not a coloring seed, so such a `main` never reaches the
  classifier loop) and stay on the direct emitter unchanged.

  **Native context-grammar fix landed alongside.** CPS-emitting `main` first
  exposed a latent miscompile in the **native** cloneable/serial if-split
  emitter (`emit_cloneable`): the pure arm of `(cloneable-reset OUTER[if cond
  THEN[shift] ELSE])` dropped the OUTER context frames (emitting `ELSE` instead
  of `OUTER[ELSE]`). The `CloneFrame` chain conflated outer frames with then-arm
  frames. Fixed by recording `n_outer_frames` (frames collected before the `if`
  during the outside-in walk, in `build_cloneable`/`build_serial`) and
  re-applying exactly those to the pure arm (`emit_cloneable_pure_arm`). This is a
  general correctness fix (a colored helper with the same shape would have
  miscompiled too), verified by `cloneable-context-if-outer-frames` /
  `serial-context-if-outer-frames`.

- **D2c -- verify against the direct output.** Each uncolored/`main`/exported
  delimited shape gets a `direct == cps` oracle (the direct output is the current
  baseline; the CPS output must match it) before its dispatch is removed.

  *Status:* the D2b mains are covered by their existing `expected.stdout`
  fixtures (all green); the two if-split-outer-frames fixtures pin the
  context-grammar fix. A dedicated `--force-direct-lowering` oracle knob is not
  yet added.

**Exit:** with D2a-D2b landed and the oracles green, remove the `emit_cps_*`
calls from the `emit_effects_*` wrappers (`emit_effects.c` :1209/:1223/:1696) and
the `EX_CALLCC` dispatch (`emit_expr.c` :2826). The wrappers either collapse to
their now-sole remaining behavior or are deleted with their callers.

*Progress toward the exit.* Population 1 is at zero (see Phase D1). Population 2
has fallen from its post-D2b peak to **zero** genuine `-emit` reaches. The closures:
- the `EX_CALLCC` coloring seed closed callcc `28 -> 2`;
- accepting `CT_CALLCC` in `handle_case_ok` (mirroring the `shift_body_ok` fix)
  closed the last 2 callcc via native `emit_callcc` (`cps-oracle-escape-capture-
  in-handler-case` + twin);
- admitting call-frame envs in `term_core_ok` (a non-atomic / Serializable
  captured env rides `env_expr` / the marshaler, skipping the operand-slot
  `atom_ok` gate) closed the serial-in-`main` fixtures (`serial-struct-env`,
  `serial-context-do-struct`);
- admitting `BS_FUNC_CALL` builtins in the emittable subset (`shape_supported` +
  `prim_expr` emit `c_op(args)`) closed `callcc-star-context` -- a `main` that
  captures continuations and resumes them via `tur_cloneable_cont_resume`/`_clone`
  outside the reset (`3` reaches).

**The last Population-2 shape -- the capturing-receiver base shift -- is now
closed.** The shape was `(reset (let [a b c] (shift (fn [v] (+ a (+ b (+ c v)))) 10)))`,
in `continuation-advanced` (`test-deeply-nested-shift`) and `continuation-substrate`
(`t-deep`), `2` reset reaches. The `shift` receiver is a closure that captures
enclosing locals; after closure conversion it is an `EX_CLOSURE` with an env
param, so `cps_shift_body_kf`'s synthesized `(closure arg)` was an indirect call
`indirect_callee_ok` rejects, and the whole function stayed direct.

The fix beta-reduces the receiver instead of synthesizing a call: abortive-shift
value is `receiver(arg)` = `body[v := arg]`, so `cps_shift_body_kf` now
recognizes an `EX_CLOSURE` receiver and inlines it as `(let [v arg] body)`
delivered to the prompt. The closure body still references its captured source
bindings by their original names (closure conversion prepends the env param but
leaves the body's capture references intact, resolving them only when the thunk
is emitted), so inlining resolves them to the visible reset-context locals, which
the lifted shift body captures via `collect_caps`. A non-capturing lambda is
already lifted to a named global (an `EX_VAR` here), so this fires only for the
capturing case that otherwise evicts. This ports the U7 receiver mechanism to
`CT_SHIFT` without introducing an indirect call.

**With this landed, a corpus scan reports zero genuine `-emit` reaches** into
`emit_cps.c` (the four lowering functions) across every fixture -- both
Population 1 (eviction) and Population 2 (direct-dispatch) are at zero. The
residual `-fallback` lines that remain (`reset`/`cloneable`/`serial`) are the
inline legacy paths in `emit_effects.c`, **not** `emit_cps.c` callers, and do not
gate D3. **D3 is unblocked.**

### Phase D3 -- delete the lowering functions

With both populations at zero callers, delete from `emit_cps.c`:
`emit_cps_reset` (+ `emit_cps_reset_escape`), `emit_cps_callcc`,
`emit_cps_cloneable_reset`, `emit_cps_serial_reset`, and their private analysis
helpers (`frame_c_expr`, `collect_ctx`, `ctx_if_branch`, `cl_can_lower`,
`sk_tag_for_frame`). Delete the `emit_effects_*` dispatch wrappers and the
`EX_CALLCC` dispatch arm that D2's exit made dead.

### Phase D4 -- delete the N6.5 delimited-control carve-out

The N6 fallback-removal plan retained a **named carve-out** (N6.5) that keeps the
delimited-control family routed to `emit_cps.c` because it was the sole emitter
for those shapes (see
[N6 plan](../v1/cps-backend-n6-fallback-removal-plan.md) N6.5). Once D1-D3 remove
that sole-emitter status, delete the carve-out: the routing that special-cases
the delimited family in the coloring/eviction path is no longer needed, and "the
CT-IR backend is the sole lowering for every colored function" becomes true
without exception.

### Phase D5 -- retire the gates and the files

- Replace the syntactic `emit_cps_program_uses_*` prelude gates
  (`emit_module.c` :6647-6651) with the CT-IR taint classification (`ensure_S`),
  the deeper U6 move: prelude emission is driven by which families the
  *classification* proves are used, not by a second form-presence scan. (The
  preludes themselves stay in `emit_dk_runtime.{c,h}` -- they are the shared
  runtime both the fixed-ABI wrappers and the colored bodies call.)
- Delete `emit_cps_program_uses_delimited` / `_callcc` / `_cloneable_dk` and
  their declarations in `emit_cps.h`.
- `emit_cps.c` is now empty (runtime relocated in step 1, lowering deleted in D3,
  gates deleted here) -> remove `emit_cps.c` and `emit_cps.h`, drop the
  `#include "emit_cps.h"` sites and the CMake source entry.

## Verification strategy

- **The direct path loses its differential net at graduation.** The
  `cps-oracle-*` twins were built to pin `direct == cps` by toggling the flag;
  post-graduation both twins run CPS (there is no toggle), so they no longer
  compare backends -- they remain valid *output-correctness* fixtures but stop
  exercising the direct lowering. Until D3 deletes it, keep the direct lowering
  under test via **the eviction fixtures** (`cps-backend-*` fixtures that
  intentionally hit non-emittable shapes -- non-scalar values, owning pointers,
  Tier C crossings -- and therefore fall back to the direct emitter), plus, if a
  finer net is wanted, a **debug-only `--force-direct-lowering` knob** (a codegen
  knob, not an experiment) that forces the direct path for a named function so a
  `direct == cps` oracle can be reconstructed per shape during D1/D2. The knob is
  deleted in D3 with the lowering it tests.
- **Caller-count is the progress metric.** D1 and D2 each drive a *countable*
  caller population to zero; instrument with a one-line `--dump-*` counter (or a
  grep over emitted C for the direct lowering's signature symbols) so "residual
  delegations = 0" and "direct-dispatch callers = 0" are measured, not asserted.
  **Landed:** `--dump-direct-lowering-callers` (a codegen knob in `globals.{c,h}`,
  wired in `main.c`; the emit hook is `emit_cps_note_direct_caller` in
  `emit_cps_ir.c`, called from the three `emit_effects_*` wrappers and the
  `EX_CALLCC` dispatch). It prints one line per reach into the direct lowering:

  ```
  direct-lowering-caller: <eviction|direct-dispatch> <family>-<emit|fallback>
  ```

  where `eviction` = Population 1 (colored function evicting a sub-shape via
  CT_LETRAW -- attributed via the `g_cps_delegating` bracket in `emit_letraw`),
  `direct-dispatch` = Population 2 (function not in `S`, emitted wholly by the
  direct emitter), and `emit` = the lowering actually emitted (a genuine
  emit_cps.c caller; the delete target) vs `fallback` = returned NULL and the
  inline path in `emit_effects.c` ran (NOT an emit_cps.c caller). Run it across
  the corpus with `emit-c --dump-direct-lowering-callers <file> 2>&1 >/dev/null`.

  ### Measured baseline (corpus scan, 2026-07-11)

  Scanning `emit-c` over every fixture input, counting only genuine `-emit`
  reaches (the live callers of the four lowering functions):

  | Population | Family | `-emit` reaches | Distinct fixtures |
  |---|---|---:|---:|
  | **eviction** (P1) | callcc | 2 | `cps-oracle-escape-capture-in-shift-body`(+`-cps` twin) |
  | **direct-dispatch** (P2) | serial | 42 | 17 |
  | **direct-dispatch** (P2) | cloneable | 34 | 10 |
  | **direct-dispatch** (P2) | callcc | 29 | 17 |
  | **direct-dispatch** (P2) | reset | 2 | 2 |

  This materially updates the plan's premise. **Population 1 (Phase D1) is
  essentially closed**: the U7 work
  ([U7 readiness](cps-backend-unification-u7-readiness-plan.md)) drove the
  cloneable / serial / reset colored-eviction residue to zero; the *only*
  remaining P1 reach is `call/cc` captured inside a `shift` body (one logical
  fixture + its `-cps` twin). D1a-D1c as written are therefore already landed
  for everything except this narrow callcc-in-shift-body tail.

  **Population 2 (Phase D2) is the dominant blocker** -- 41 distinct fixtures.
  `in_s` (`emit_cps_ir.c` ~:1841) is false, and the function wholly direct-emits,
  for four reasons; P2 splits along them:
  - `main` / `c_export_name` -- excluded by construction; the fix is the D2b
    fixed-ABI entry wrapper (`main__cps(dk_done())` / `f__cps(args, dk_done())`).
  - `fn_sig_ok` / `term_core_ok` reject -- the emittable-subset boundary (the
    five v1 gap plans + D1c context-grammar widening + D1d `needs_heap_join`).

  D3-D5 (deleting the lowering, the carve-out, the files) stay gated on **both**
  populations reaching zero `-emit`, i.e. on closing the callcc-in-shift-body P1
  tail *and* the full D2 uncolored/main/subset-reject population.
- **Snapshot churn** regenerates in the same PR as each phase that moves codegen
  (D2 in particular re-emits every uncolored delimited function), per the
  CLAUDE.md fixture-regen recipe.

## Scope and non-goals

- **Async/await are out of scope.** They lower to a separate stackful `ucontext`
  fiber runtime (`FiberBlock` / `TurScheduler`, `emit_module.c`), not the DK
  machine, and are untouched by the `emit_cps.c` delete. The stackful->stackless
  migration is a decoupled effort (U5 note), not a blocker here.
- **No runtime redesign.** The shared DK runtime (`emit_dk_runtime.c`) and the
  fiber runtime stay as-is; this plan only removes the *direct-style emit* that
  drives the DK runtime, unifying on the CT-IR emit.
- **No new control semantics.** Multi-shot resume of a single-shot continuation
  stays `TUR-E0201` on the unified path exactly as today.

## Dependency summary

```
graduation (DONE) ──► D1 (colored residue) ──┐
                                              ├─► D3 (delete lowering) ─► D4 (delete carve-out) ─► D5 (delete files)
                      D2 (uncolored/main) ────┘
```

D1 and D2 are independent and can land in either order (or interleaved); D3 waits
on both reaching zero callers. D4 waits on D3. D5 waits on D4 and folds in the
deeper U6 classification-driven prelude gating.
