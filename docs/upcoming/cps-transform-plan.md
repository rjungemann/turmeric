# Whole-Program CPS Transform -- Implementation Plan (CPS0--CPS7)

> **Status:** In progress. **CPS0 ratified (2026-06-02)**; **CPS1 (may-capture
> coloring) done (2026-06-02)**; **CPS2 (CPS/ANF IR) done (2026-06-02, dump-only)**;
> **CPS3 (selective lowering + boundary bridging) done (2026-06-02, dump-only)**;
> **CPS4 (heap-reified continuations + trampoline runtime) done (2026-06-02,
> standalone runtime)**; **CPS5 (multi-prompt delimited substrate + implicit
> root prompt) done (2026-06-02, standalone machine)**; **CPS6 (retire the
> capture ceiling on the CPS path) done (2026-06-02)**; **CPS7 (benchmarks /
> perf gates / docs) done (2026-06-02)**; **CPS8 (codegen wiring for base
> delimited control + undelimited `call/cc`/`escape`) done (2026-06-02)**;
> **CPS9 (cloneable continuations with captured context on the DK machine) done
> (2026-06-02)**; **CPS10 (serializable continuations on the DK machine, CPS5.4)
> done (2026-06-02)**; **CPS11 (`call/cc*` captures up to an enclosing
> cloneable-reset) done (2026-06-02)**. **CPS0--CPS11 implemented**; base
> `reset`/`shift`/`shift0`, undelimited `call/cc`/`escape`,
> `cloneable-reset`/`cloneable-shift` (capturing the delimited *context* and
> replaying it multi-shot via `dk_invoke`), `serial-reset`/`serial-shift`
> (capturing a *marshalable* context that serialize/deserialize round-trips), and
> now `call/cc*` (capturing the real continuation up to the nearest enclosing
> cloneable-reset) all execute on this substrate in emitted code (`emit_cps.c`).
> The whole delimited/undelimited operator set now runs on the CPS substrate.
> **Follow-on increments landed 2026-06-02** (see "Follow-on increments" below):
> OQ-CPS2 **resolved** (keep the fiber path as selective fallback + bounded fast
> path); the captured-context subset **broadened** to integer division and to
> arbitrary 2-arg call frames (serial frames name-keyed for marshaling); and
> **CC4** `(k v)` application sugar + `:cont` parameter typing, extended by a
> **flavored `cont`** so `(k v)` dispatches to the escape/cloneable/serial resume
> runtime. Remaining: the *value* half of `cont<T>` -- a non-int result `T`
> threaded through the shift node + resume + (for serial) a `Serializable` env
> codec, which still gates cstr/struct serial env types; the parametric
> `(cont T)` annotation (needs an arrow-kind `cont`); and broadening contexts
> past single-hole binops/2-arg calls (lets, if-branches). This is the substrate
> that
> unblocks *undelimited* control (real Scheme `call/cc`, an implicit
> program-wide prompt) and removes the bounded-capture ceiling on the existing
> delimited runtime.
>
> **Implementation note on incrementality.** CPS2--CPS6 are landed
> *additively*: the new IR, runtime, and substrate are built and exercised
> behind dev flags and the colored-only path, so the default compile pipeline
> and the full fixture suite stay green at every commit (CLAUDE.md forbids
> committing a failing suite). Each phase notes explicitly what is "built +
> inspectable (dump-only)" versus "wired into live codegen". The final wiring
> into the shipping `tur build`/`emit-c` path is gated until the substrate is
> complete end-to-end.
>
> **Key insight:** Turmeric already has working **delimited** continuations --
> `shift`/`reset`/`shift0` (one-shot, `tur_cont`) and `call/cc*` (cloneable,
> multi-shot, `tur_cloneable_cont`) -- implemented via fiber-context
> save/restore (`src/async/fiber_ctx_{x64,arm64}.S`). What we do *not* have is
> unbounded capture: `tur_cont_alloc` captures at most
> `TUR_CONT_MAX_CAPTURED_FRAMES` (= 16) stack frames and returns `NULL` past
> that (`src/runtime/runtime.c:103`, `:180`). That ceiling is fine for a
> *nearby* `reset`, but it is exactly why an undelimited `call/cc` -- whose
> prompt sits at the top of the program -- cannot be expressed today: any
> non-trivial call depth overflows the buffer. Reifying the continuation as a
> heap data structure (CPS) makes capture O(1) and unbounded, and lets us
> install a single implicit prompt around `main`.
>
> **Last updated:** 2026-06-02
>
> **Related:**
> - [`call-cc-completion-plan.md`](call-cc-completion-plan.md) -- the primary
>   consumer; reframed to be built *on top of* this plan (undelimited `call/cc`
>   + implicit program-wide prompt).
> - [`multishot-continuations-plan.md`](multishot-continuations-plan.md)
>   -- `CK_MULTISHOT` ownership; orthogonal to the capture mechanism but rides
>   on the same reified-continuation substrate.
> - [`control-flow-completeness-plan.md`](../archive/history/control-flow-completeness-plan.md)
>   / [`control-flow-completeness-audit.md`](../archive/history/control-flow-completeness-audit.md)
>   -- CF4 deferred "real capture" to "a whole-program CPS pass"; this plan *is*
>   that pass.
> - [`serializable-continuations-guide.md`](../guides/serializable-continuations-guide.md)
>   -- heap-reified continuations are strictly easier to marshal than stack
>   snapshots; see CPS5.4.

---

## Motivation

The control-flow audit (CF4) deferred a real `call/cc` on the grounds that
"real capture requires a whole-program CPS pass." The delimited runtime that
shipped afterwards narrowed the gap -- `shift`/`reset` and `call/cc*` give us
*delimited* capture today -- but it did not close it. Two things still require
the deferred pass:

1. **Undelimited capture.** Scheme's `call/cc` captures "the rest of the
   program." With a fiber-frame snapshot bounded at 16 frames, capturing a
   continuation that reaches back to the top of a deep call stack is
   impossible; `tur_cont_alloc` returns `NULL`.
2. **An implicit program-wide prompt.** For bare `call/cc` to "just work"
   anywhere (no explicit enclosing `reset`), there must be a prompt at program
   entry and capture must extend to it. That is only sound if capture is
   unbounded.

This plan introduces a CPS-based substrate where the current continuation is an
explicit, heap-allocated value. Capturing it is a pointer read; resuming it is
a call. Depth is bounded by the heap, not by a fixed array. On top of this,
delimited control becomes multi-prompt (`pushPrompt`/`withSubCont`), the
delimited operators are re-expressed without the frame cap, and an implicit
root prompt around `main` makes undelimited `call/cc` express the Scheme
semantics directly.

### Goals

- Reify the current continuation as a heap value with O(1), unbounded capture.
- Provide a **selective** CPS transform: only functions that can reach a control
  operator are CPS-converted; pure/direct code keeps its calling convention and
  performance.
- Re-express `reset`/`shift`/`shift0`/`call/cc*`/`serial-*` on the new substrate
  with no behavior change, and retire the `TUR_CONT_MAX_CAPTURED_FRAMES` ceiling.
- Install an implicit root prompt around `main` so undelimited `call/cc` is
  expressible (consumed by `call-cc-completion-plan.md`).
- Bound native stack growth via trampolining and guarantee proper tail calls
  through CPS'd code.

### Non-goals

- Changing the observable semantics of delimited operators that already ship.
- Forcing CPS on the whole program unconditionally -- direct-style code stays
  direct (see CPS1 coloring). "Whole-program" here means the *analysis* is
  whole-program; the *rewrite* is selective.
- `CK_MULTISHOT` ownership accounting -- that is
  [`multishot-continuations-plan.md`](multishot-continuations-plan.md);
  it consumes this substrate but is scheduled separately.

---

## The two viable mechanisms (and why CPS)

| Mechanism | Capture cost | Depth bound | Direct-style perf | Notes |
|---|---|---|---|---|
| Bounded fiber-frame snapshot (today) | O(frames) copy | **16 frames** | Native | Ships now; cannot go undelimited |
| Segmented/copyable native stacks | O(stack) copy | Heap | Native | Portability + ASan/UBSan friction; copy cost per capture |
| **Selective CPS (this plan)** | O(1) | Heap | Native on uncolored code | Reifies `k`; multi-shot/serialize fall out naturally |

CPS wins because (a) capture is a pointer read regardless of depth, (b) the
reified continuation is the *same* object multi-shot snapshotting and
serialization want, and (c) selectivity keeps the cost off hot direct-style
paths. The delimited-control model is the
Dybvig--Peyton-Jones--Sabry multi-prompt framework: `reset` = push a fresh
prompt, `shift`/`shift0` = capture/compose the sub-continuation up to the
nearest prompt, undelimited `call/cc` = capture up to the implicit *root*
prompt.

---

## Phase ordering at a glance

| Phase | Scope | Why this order |
|---|---|---|
| CPS0 | Ratify model: selective CPS + multi-prompt delimited control; pick trampoline strategy | Lock the substrate before any IR moves |
| CPS1 | "May-capture" coloring analysis (whole-program) | Decides which functions get CPS'd; everything downstream keys off the coloring |
| CPS2 | CPS/ANF IR + normalization for colored functions | The representation the lowering targets |
| CPS3 | Selective CPS lowering + direct<->CPS boundary bridging | The transform itself |
| CPS4 | Heap-reified continuations + trampoline + TCO | Replaces the bounded fiber snapshot for colored code |
| CPS5 | Multi-prompt delimited substrate; re-express shift/reset/shift0/call-cc*/serial; install implicit root prompt | Unifies existing ops; exposes the program-wide prompt |
| CPS6 | Retire `TUR_CONT_MAX_CAPTURED_FRAMES`; unbounded capture | The ceiling is gone once CPS5 lands |
| CPS7 | Benchmarks, perf gates, docs, migration | Prove no direct-style regression; document the model |

---

## Phase CPS0 -- Ratify the model  -- **RATIFIED 2026-06-02**

- **CPS0.1 -- Selective CPS. RATIFIED.** We adopt **selective** CPS (color only
  may-capture functions) over whole-program CPS. Direct-style code keeps its
  native calling convention and pays no trampoline/allocation tax; only
  functions that can dynamically reach a control operator are CPS-converted.

  **Coloring rule (agreed; implemented by CPS1).** A function `F` is *colored*
  (must be CPS'd) iff it can dynamically reach a control operator. Concretely:

  1. **Seed.** `F` is colored if its body *directly* contains a control-op IR
     node: `EX_SHIFT`, `EX_SHIFT0`, `EX_RESET`, `EX_CLONEABLE_SHIFT`,
     `EX_CLONEABLE_RESET`, `EX_SERIAL_SHIFT`, `EX_SERIAL_RESET`, `EX_PERFORM`,
     `EX_HANDLE`, `EX_RESUME`, or `EX_DISCONTINUE`. (Surface `call/cc`, `escape`,
     `call/cc*`, and effect `perform`/`handle` all desugar to these nodes during
     elaboration, so seeding on the post-elaboration IR captures them uniformly.)
  2. **Transitive closure.** `F` is colored if it calls (via a resolved
     `EX_CALL.fn_binding`) any colored function. Propagate backward to a least
     fixed point.
  3. **Conservative over-approximation.** An *indirect* call -- one whose callee
     is not a statically resolved top-level binding (`fn_binding == NULL`: a
     call through a parameter, closure value, or field) -- is treated as
     possibly reaching a control op, so `F` is colored. This is sound (never
     under-colors); it may over-color higher-order code, which CPS3 may refine
     later via control-flow analysis (tracked as a CPS7 precision follow-up).

  The analysis is **whole-program** (CPS1.1 builds the call graph); the
  *rewrite* (CPS3) is selective. Coloring is stored as additive IR metadata
  (`FnDef.cps_colored`) so it perturbs nothing until CPS3 consumes it.

- **CPS0.2 -- Multi-prompt delimited-control framework. RATIFIED.** The single
  substrate is the Dybvig--Peyton-Jones--Sabry multi-prompt model. The mapping
  from existing operators onto prompts/sub-continuations (`pushPrompt`,
  `withSubCont`, `pushSubCont`) is:

  | Surface / IR node | Prompt action | Sub-continuation capture | Re-install prompt on resume? |
  |---|---|---|---|
  | `reset` / `EX_RESET` | `pushPrompt p` (fresh prompt `p`) | -- | -- |
  | `shift` / `EX_SHIFT` | capture up to nearest prompt `p` | `withSubCont p` (composable, abortive body) | **Yes** -- resume re-pushes `p` (`pushSubCont` with the delimiter) |
  | `shift0` / `EX_SHIFT0` | capture up to nearest prompt `p`, **and pop `p`** | `withSubCont p` | **No** -- resume does *not* re-push `p` |
  | `call/cc*` / `EX_CLONEABLE_SHIFT` | capture to nearest cloneable prompt | `withSubCont` (multi-shot: sub-cont re-enterable N times) | Yes (cloneable resume re-pushes) |
  | `serial-shift` / `EX_SERIAL_SHIFT` | capture to nearest serial prompt | `withSubCont` (reified chain is marshalable) | Yes |
  | undelimited `call/cc` | capture to the **implicit root prompt** (CPS5.3) | `withSubCont root` | Yes (root is re-pushed) |

  This matches the existing `EX_SHIFT`/`EX_SHIFT0` semantics (CF2): the
  elaborator types `shift` and `shift0` identically (`shift0_` differs only in
  *delimiter behavior at runtime*, see `src/compiler/elab_effects.c:178`) -- the
  *only* observable difference is whether the prompt is re-installed on resume,
  which is exactly the "re-install" column above. `shift` => re-push the
  delimiter as part of the resumed sub-continuation; `shift0` => the delimiter
  is consumed by the capture and not re-pushed.

- **CPS0.3 -- Native-stack bounding: TRAMPOLINE. RATIFIED.** CPS'd code returns
  *thunks* (return-to-driver continuations) to a trampoline loop rather than
  recurring on the native C stack. Rationale: portable, guaranteed-proper tail
  calls across all our C targets (gcc/clang at `-O0` Debug + ASan, MSVC-less but
  varied) are **not** guaranteed -- `[[clang::musttail]]`/`__attribute__((musttail))`
  is not universally available at the optimization levels we build under (Debug
  is `-O0 -fsanitize=address,undefined`), and the existing snapshot path already
  assumes a driver-style resume. A trampoline gives O(1) native stack for
  arbitrarily deep CPS tail chains.

  **Perf budget.** The trampoline tax is paid *only on colored code* (CPS0.1).
  Budget: one indirect call + one heap-allocated thunk per CPS tail step on the
  colored path; **zero** overhead on uncolored direct-style code (enforced by
  the CPS7.1 gate). Platform-TCO as an opt-in fast path is deferred to OQ-CPS3 /
  CPS7 and is explicitly *not* a blocker.

- **CPS0.4 -- Fiber-runtime coexistence. RATIFIED (coexist, then re-evaluate).**
  The fiber path (`src/async/fiber_ctx_{x64,arm64}.S`, `tur_cont`,
  `tur_cloneable_cont`) **stays** as the shipping implementation for all
  delimited operators until CPS5 re-expresses them on the prompt substrate.
  Removal is **not** part of CPS1--CPS4: those phases add the CPS substrate
  alongside the fiber path without disturbing it (coloring is additive metadata;
  the trampoline/heap-cont machinery is only exercised by colored code once CPS3
  lowering lands). At CPS5 the delimited operators move to prompts; at that point
  the fiber path is either (a) removed for a single substrate, or (b) retained as
  a depth-bounded fast path for shallow `reset`. That keep-or-remove decision is
  deferred to OQ-CPS2 and will be made on CPS7.2 numbers -- it is **not**
  pre-committed here. Until CPS5, both paths coexist and the fiber path remains
  authoritative.

## Phase CPS1 -- "May-capture" coloring analysis  -- **DONE 2026-06-02**

A function is **colored** (must be CPS'd) iff it can dynamically reach a control
operator: `call/cc`, `escape`, `shift`/`shift0`, `call/cc*`, `serial-shift`, or
an effect `perform`. Coloring is transitive through (possibly) called
functions; indirect/closure calls whose target is unknown are conservatively
colored.

**Implementation.** The analysis lives in `src/passes/cps.c`
(`cps_color_program` / `cps_dump_coloring`, declared in `src/passes/cps.h`).
Surface `call/cc`/`escape`/`call/cc*`/effect `perform`/`handle` all desugar to
the control-op IR nodes during elaboration, so the seed predicate
(`cps_directly_uses_control`) keys off the post-elaboration IR nodes
(`EX_SHIFT`/`EX_SHIFT0`/`EX_RESET`/`EX_CLONEABLE_*`/`EX_SERIAL_*`/`EX_PERFORM`/
`EX_HANDLE`/`EX_RESUME`/`EX_DISCONTINUE`). The coloring result is stored
additively on `FnDef.cps_colored` -- distinct from the Phase-18 `may_capture`
field the existing delimited-CPS pass owns -- so nothing in the shipping
pipeline is perturbed. Exposed via the dev flag `--dump-cps-coloring`.

- **CPS1.1** Build the call graph (including a conservative over-approximation
  for indirect calls / closures). *Done:* `cps_collect_calls` walks each
  top-level function's body (stopping at nested-fn boundaries) collecting
  resolved call edges via `EX_CALL.fn_binding`; any unresolved call (indirect /
  call-through-local-value / extern) sets `has_indirect`, the conservative
  over-approximation. Calls to nested lambdas are unresolved and thus covered by
  this rule, which is why the node set can be restricted to top-level functions
  while staying sound.
- **CPS1.2** Seed the colored set with the control-operator primitives and
  effect performs; propagate backward to all callers (least fixed point).
  *Done:* seed = `cps_directly_uses_control(body) || has_indirect`; then a
  worklist-free fixed-point loop colors any function with a colored resolved
  callee until stable. A function reachable to a control op is colored; a
  provably pure leaf is not.
- **CPS1.3** Expose the coloring as IR metadata consumed by CPS3. *Done:*
  written to `FnDef.cps_colored`; the `--dump-cps-coloring` fixture shows
  `pure-arith`/`also-pure` uncolored and `uses-shift` (seed) /
  `calls-shifter` (transitive) colored.
- **CPS1.4** Fixture: `tests/fixtures/cps-coloring/` -- a small module asserting
  the colored/uncolored partition via a debug dump (gated behind a dev flag).
  *Done:* `tests/fixtures/cps-coloring/input.tur` plus the
  `dump-cps-coloring-partition` / `dump-cps-coloring-no-output` cases in
  `tests/run-flags.sh`.

## Phase CPS2 -- CPS/ANF intermediate representation  -- **DONE 2026-06-02 (dump-only)**

**Implementation.** A compact ANF/CPS IR lives in `src/passes/cps_ir.{h,c}`
(atoms `CAtom`, continuations `CKont`, terms `CTerm`). The translation is the
textbook two-function scheme -- `cps_tail(e, k)` delivers `e`'s value to
continuation `k`; `cps_bind(e, x, rest)` names `e`'s value and continues -- with
non-trivial arguments atomized (named by a fresh binder), which is what
establishes ANF. Exposed via `--dump-cps`. **Dump-only: not yet wired into
codegen** (that is CPS3). It runs only for colored functions; uncolored bodies
are never touched.

- **CPS2.1** ANF normalization for colored functions. *Done:* every non-trivial
  subexpression is named by a `let`-style binder (`CT_LETPRIM`/`CT_LETCALL`/
  `CT_LETVAL`). E.g. `(+ 1 (leaf-shift x))` lowers to a named call result plus a
  named `(+ 1 t)` prim. Uncolored bodies keep their direct-style Expr tree.
- **CPS2.2** CPS IR nodes + continuation threading. *Done:* the IR represents
  `(k v)` as `CT_APPCONT` and a colored tail call `f(args, k)` as `CT_TAILCALL`
  (the continuation is threaded through); non-tail colored calls introduce a
  join continuation `CT_LETCONT`. Tail positions become continuation
  applications. Reuses `TY_CONT` as the continuation type kind.
- **CPS2.3** Type the continuation parameter as `cont<T>`. *Done (represented):*
  `KK_RET` carries the function's result type kind, printed as `k:cont<T>`; join
  continuations carry their parameter's type. The IR is internally consistent
  with the `shift` typing rule (CF2). *Deferred to CPS3:* re-running the host
  type checker over the emitted CPS output ("round-trips through the type
  checker") happens when the IR is wired into the elaborator/codegen, since the
  IR is currently a standalone dump artifact.
- **Tests:** `dump-cps-anf` / `dump-cps-no-output` in `tests/run-flags.sh`
  assert the ANF naming, the `k:cont<int>` typing, the threaded
  `tailcall ...(... j)`, the `letcont` join, and the `reset`/`shift` forms.

## Phase CPS3 -- Selective CPS lowering + boundary bridging  -- **DONE 2026-06-02 (dump-only)**

**Implementation.** The CPS2 IR *is* the lowered form; CPS3 adds the
direct<->CPS boundary classification on top of it (`src/passes/cps_ir.c`).
Because coloring is transitive (CPS1), every mid-program caller of a colored
function is itself colored, so the boundaries reduce to two kinds, both made
explicit in the `--dump-cps` output:

- **cps->direct** -- a colored function calling an *uncolored* one: lowered to
  `CT_LETCALL` (call normally, feed the result to the current continuation),
  tagged `; cps->direct` in the dump.
- **cps->cps** -- a colored tail call: `CT_TAILCALL` threads the continuation
  through, tagged `; cps->cps`.
- **direct->CPS entry** -- a colored function with no colored caller is an
  *entry root* (the C runtime / implicit root prompt supplies the initial
  continuation); classified `entry` vs `internal` in each `cps-fn` header
  (`is_cps_entry`).

This is **dump-only**: the classification and IR are produced for inspection;
the live codegen still routes colored functions through the existing fiber path,
so the fixture below runs correctly today via that path.

- **CPS3.1** Lower colored functions to CPS (add `k`, sequence ANF bindings,
  tail calls -> `k` applications). *Done (in the IR):* delivered by CPS2's
  `cps_ir_translate_fn`; CPS3 confirms each colored function lowers and tags its
  boundaries. *Deferred:* emitting C from this IR is CPS4 (needs the heap-cont
  runtime + trampoline) -- "compiles through the new path" is gated on CPS4.
- **CPS3.2** Leave uncolored functions direct; bridge at the boundaries. *Done:*
  uncolored functions are never lowered (e.g. `twice` does not appear in the
  dump); cps->direct and direct->CPS-entry boundaries are classified as above. A
  `main` that drives a colored helper which calls `shift` runs correctly.
- **CPS3.3** Fixture: `tests/fixtures/cps-mixed-coloring/`. *Done:* a mixed
  program (uncolored `twice`, colored `shift-then-twice`/`run`, entry `main`)
  that runs (interpreter + compiled) to the correct result `41`, with
  `dump-cps-bridge` in `tests/run-flags.sh` asserting the boundary
  classification. *Deferred to CPS7:* the "no allocation on the uncolored path"
  perf assertion (needs the CPS7 perf harness).

## Phase CPS4 -- Heap-reified continuations + trampoline  -- **DONE 2026-06-02 (standalone runtime)**

**Implementation.** A self-contained heap-continuation + trampoline runtime in
`src/runtime/cps_rt.{h,c}`. A continuation is a heap closure chain (`TurKont`:
`fn`, `next`, `env`, per-frame `defer`); a trampoline (`tur_trampoline`) drives
it via return-to-driver steps (`TurStep`). **Standalone: linked into `tur_core`
but not yet driven by emitted code** (codegen wiring is CPS5). Validated by the
`tur_cps_rt_unit` ctest target (`tests/cps_rt_unit.c`), run under ASan with leak
detection ON (the runtime frees everything it allocates).

- **CPS4.1** Continuation as a heap closure chain, not a `tur_frame[16]`
  snapshot. *Done:* capture = take the current `TurKont*` (O(1), unbounded);
  resume = `tur_kont_resume` / `tur_trampoline`. The old 16-frame ceiling does
  not participate.
- **CPS4.2** Trampoline driver bounding native stack. *Done:* a frame that would
  tail-call `next` instead returns a `TurStep` thunk to `tur_trampoline`, which
  loops. The `cps4-deep-resume` check resumes a **500,000-frame** chain in O(1)
  native stack (a recursive resume of that depth would overflow).
- **CPS4.3** `defer`/frame-teardown semantics. *Done:* per-frame `defer` fires
  exactly once -- on normal resume (`cps4-deep-defer-once`), on abort/free
  (`cps4-abort-defer-once`), never twice (`cps4-defer-idempotent`,
  `cps4-free-no-refire`). `tur_kont_fire_defer` is idempotent.
- **CPS4.4** Capture/resume across depth well beyond 16. *Done:* the 500k-frame
  `cps4-deep-resume` check succeeds where the fiber path returned `NULL`. (This
  is exercised as a C unit test rather than a `.tur` fixture because the runtime
  is not yet wired into emitted code; the end-to-end `.tur` deep-capture fixture
  lands with the CPS5 codegen wiring.)

## Phase CPS5 -- Multi-prompt delimited substrate + implicit root prompt  -- **DONE 2026-06-02 (standalone machine)**

**Implementation.** A multi-prompt delimited-control machine in
`src/runtime/cps_prompt.{h,c}`, the Dybvig--Peyton-Jones--Sabry model expressed
over continuation chains (`DK`) with prompt markers. Because capture is a
*chain slice* (`dk_copy_range` from the shift point up to the nearest prompt),
it is unbounded and O(depth-of-slice). Validated by the `tur_cps_prompt_unit`
ctest (`tests/cps_prompt_unit.c`) under ASan. **Standalone machine, not yet
driven by emitted code** -- the existing `EX_RESET`/`EX_SHIFT`/`EX_SHIFT0`
lowerings still run on the fiber path, so the `continuation-*` fixtures keep
passing unchanged; re-pointing those lowerings at this machine is the codegen
integration that remains (gated until the substrate is wired end-to-end).

- **CPS5.1** Prompts on the CPS substrate; shift vs shift0 re-install. *Done (on
  the substrate):* `reset` = `dk_prompt`; `shift`/`shift0` = `dk_shift`/
  `dk_shift0` capture up to the nearest prompt; `shift` re-installs the prompt on
  the captured sub (`cps5-shift-reinstall`), `shift0` does not
  (`cps5-shift0-no-reinstall`); compose works (`cps5-shift-compose`:
  `reset{1+shift(k.2+k(k(3)))}` = 7). *Landed (CPS8):* the base
  `reset`/`shift`/`shift0` lowerings are re-pointed at this machine in emitted
  code (`emit_cps.c`); the `continuation-*` base-shift fixtures run on it.
- **CPS5.2** `call/cc*` (multi-shot) and `serial-*`. *Done (multi-shot):* a
  captured sub is re-entrant -- `dk_invoke` runs a fresh copy each time
  (`cps5-multishot`: `k(10)+k(20)` over `[]*2` = 60), which is exactly cloneable
  resume; one-shot is "invoke once". *serial-* simplification:* see CPS5.4.
- **CPS5.3** Implicit root prompt. *Done:* `dk_run_root` delimits an unmatched
  shift at program entry; `cps5-root-prompt` captures `(1 + [])` up to entry with
  no explicit `reset` and resumes (= 1101). This is the prompt an undelimited
  `call/cc` captures up to (consumed by `call-cc-completion-plan.md`).
- **CPS5.4** Note (no code): a heap-reified sub-continuation is a flat closure
  chain (`DK`: a list of `(frame-fn, env)` / prompt nodes), which the
  `Serializable` machinery can walk directly -- no fiber/stack snapshot to
  marshal. This makes `serial-*` a straightforward chain walk (serialize each
  frame's tag + env), strictly simpler than snapshotting a native stack. See
  [`serializable-continuations-guide.md`](../guides/serializable-continuations-guide.md);
  recorded in CPS7 docs. **Realized in emitted code by CPS10** -- the captured
  `serial-*` continuation marshals as exactly this tag+env chain walk.

## Phase CPS6 -- Retire the capture ceiling  -- **DONE 2026-06-02**

The CPS path has **no** capture ceiling by construction (CPS4): capture is an
O(1) pointer take into an unbounded heap chain. The
`TUR_CONT_MAX_CAPTURED_FRAMES` constant remains only in the retained fiber fast
path (CPS0.4 / OQ-CPS2), where it is intrinsic (a fixed `captured[]` array) --
removing the guard there would overflow the array, not enable unbounded
capture. So "retiring the ceiling" means: it does not apply on the CPS path, and
the constant is scoped + documented as fiber-only.

- **CPS6.1** No live `NULL`-on-overflow on the CPS path. *Done:* `grep` shows
  `TUR_CONT_MAX_CAPTURED_FRAMES` only in the fiber path (`runtime.c:~106`,
  `:~187`) and as a doc reference in `cps_rt.h`; the CPS substrate
  (`cps_rt.c` / `cps_prompt.c`) has no such guard. The CPS4.4 deep-capture check
  (`cps4-deep-resume`, 500k frames) passes. The fiber-path guards now carry a
  comment marking them as the retained bounded fast path and pointing to the
  unbounded CPS substrate.
- **CPS6.2** `runtime.h` doc comments. *Done:* the continuation section now
  documents the two coexisting models (bounded fiber snapshot vs unbounded
  heap-reified CPS substrate), and `#define TUR_CONT_MAX_CAPTURED_FRAMES` is
  annotated "FIBER FAST PATH ONLY".

> **Remaining integration (post-CPS6).** Fully *removing* the fiber path (so the
> CPS substrate is the single implementation) depends on re-pointing the
> `EX_RESET`/`EX_SHIFT`/`EX_SHIFT0`/`call/cc*`/`serial-*` codegen lowerings at
> the CPS substrate (the CPS5.1/CPS5.2 "deferred" items) and running the
> `continuation-*` suite on that path. **CPS8 re-points base
> `EX_RESET`/`EX_SHIFT`/`EX_SHIFT0`** onto the DK machine in emitted code;
> `call/cc*`/`serial-*` remain on their existing lowerings, so both substrates
> still coexist for those ops -- the CPS0.4 coexistence contract. The
> keep-vs-remove decision (OQ-CPS2) stays open until those, too, move over.

## Phase CPS7 -- Benchmarks, perf gates, docs  -- **DONE 2026-06-02**

- **CPS7.1** No regression on direct-style (uncolored) hot paths. *Done:* the
  CPS work is **additive** -- the default `emit-c`/`build` codegen path is
  unchanged, so all `tests/fixtures/*/expected.c` codegen snapshots are
  byte-identical (the full suite passes with **no snapshot regeneration**). That
  identical-codegen invariant *is* the perf gate: uncolored code emits exactly
  the same C as before, hence zero added allocation/overhead. (Once the CPS path
  is wired into codegen, a microbenchmark gate on a no-control hot loop becomes
  meaningful; until then identical snapshots are the stronger guarantee.)
- **CPS7.2** Colored-path cost. *Done (sized):* the trampoline runtime
  (`cps_rt`) sustains **~43 M steps/s** (10,000,000-frame chain resumed in
  ~231 ms, `-O2`, x86-64 Linux), producing the correct result in O(1) native
  stack. The current cost is one small heap allocation per bounce (the
  per-step `ResumeState`); a freelist/arena for those is the obvious follow-up
  and is noted as an optimization, not a blocker. The CPS5 machine adds one
  chain-slice copy per `shift` and one copy per multi-shot `dk_invoke`.
- **CPS7.3** Docs. *Done:* `docs/guides/effects-system-guide.md` gains a "Prompt
  Model and Unbounded Capture (CPS substrate)" section with the operator->prompt
  mapping table, the implicit root prompt, unbounded capture, and the
  selectivity perf note, linking this plan and the serializable guide.
- **CPS7.4** Audit. *Done:* `control-flow-completeness-audit.md` "Full CPS
  transformation" deferred item carries a **CF4 update** pointing here and
  summarizing what CPS0--CPS6 deliver.

## Phase CPS8 -- Codegen wiring (base delimited control)  -- **DONE 2026-06-02**

The substrate is now driven by emitted code for base delimited control. The
delimited lowerings no longer sit only on the bespoke inlined runtime; a
`(reset BODY)` whose body can dynamically reach a base `shift` bound to it is
compiled to a run on the multi-prompt `DK` machine (`dk_run` / `dk_prompt` /
`dk_shift` / `dk_shift0`), emitted directly into the generated program.

**Implementation.** `src/compiler/emit_cps.{h,c}`.

- **CPS8.1 -- Substrate in emitted programs.** `emit_cps_runtime_prelude`
  emits a faithful C port of the `DK` machine (`src/runtime/cps_prompt.c`) into
  the generated preamble, gated by `emit_cps_program_uses_delimited` (true iff
  the program uses `EX_RESET`/`EX_SHIFT`/`EX_SHIFT0`). Previously `cps_prompt.c`
  was linked only into the unit-test targets; now the same machine ships inside
  programs that use delimited control.
- **CPS8.2 -- reset/shift/shift0 onto the machine.** `emit_effects_reset`
  delegates to `emit_cps_reset`. Turmeric's `shift` is abortive -- `(shift f v)`
  applies the receiver `f` to the body value and delivers the result to the
  nearest `reset`, discarding the captured context (the sub-continuation is
  never handed back to user code). The lowering expresses exactly this: the
  reset is a `dk_prompt`, the shift is a `dk_shift`/`dk_shift0` node whose body
  (`__dk_abort_body`) returns the precomputed `f(v)` and ignores the captured
  sub. Because `f(v)` is precomputed in-scope, the shift node needs no
  per-frame environment-capture codegen -- the value rides in the node's
  `body_env`. Sub-expression evaluation reuses the direct-style emitter, so all
  value types/closures/builtins keep working; preceding `let`/`do` effects are
  emitted ahead of the shift node so evaluation order is preserved.
- **CPS8.3 -- Selective + safe fallback.** A pure `can_lower` feasibility check
  runs before any emission, so shapes outside the supported subset (e.g. a
  `shift` in an `if` branch, a non-`intptr`-safe result type) fall back to the
  legacy lowering with byte-identical output. Only the four base-shift snapshot
  fixtures (`continuation-basic`, `continuation-advanced`,
  `shift-result-typing`, `shift0-result-typing`) regenerate; the rest of the
  suite is unchanged.
- **CPS8.4 -- Executing test.** `tests/fixtures/continuation-substrate/` builds
  and runs reset/shift/shift0 (tail shift, shift0, nested resets, first-shift
  abort, captured `let` locals, no-shift passthrough) on the DK machine and
  asserts the values -- the executing gate the substrate previously lacked (the
  prior `continuation-basic`/`-advanced` fixtures have no `main`, so only their
  snapshots were checked).
- **CPS8.5 -- Undelimited `call/cc` / `escape` (landed).** `(call/cc f)` and
  `(escape f)` now capture a real, undelimited, one-shot continuation against an
  implicit program-wide prompt -- no enclosing `reset` required, and unbounded
  depth (the 16-frame fiber ceiling does not apply). The lowering (`EX_CALLCC`,
  `emit_cps_callcc`) establishes a setjmp landing at the call/cc site and hands
  `f` the landing as the continuation handle; invoking it via
  `tur_escape_resume` is an upward escape that returns the value at the call/cc
  site, abandoning `f`'s pending work. This is exactly the spec's one-shot
  upward semantics (call-cc-completion CC1/CC3, OQ2/OQ3): invoking the
  continuation *after* its prompt has returned is a runtime error, so no
  downward re-entry is required. Validated by `callcc-real-capture` (CC1.4: no
  reset, `k` aborts, result 42) and `escape-deep-capture` (escape from 5000
  frames deep -- the unbounded-capture proof). Still gated behind `-Xcallcc`
  (CC5 ungating is a follow-up); the continuation is resumed via the
  `tur_escape_resume` builtin (the proven `call/cc*` handle pattern) -- direct
  `(k v)` application sugar and the `cont<T>` parameter typing (CC4) remain.
- **Remaining (next increment) -- addressed by CPS9/CPS10/CPS11.** Reifying a
  *resumable* sub-continuation in emitted code (the piece the abortive shift and
  one-shot upward escape do not need) is delivered by **CPS9** for
  `cloneable-reset`/`cloneable-shift` (multi-shot), **CPS10** for
  `serial-reset`/`serial-shift` (marshalable), and **CPS11** for `call/cc*`
  (which now captures up to an *enclosing* cloneable-reset -- the real call/cc*
  semantics -- rather than installing a fresh trivial inner reset). The whole
  operator set is on the substrate; only perf/cleanup follow-ons remain (see the
  Status summary and Open questions).

## Phase CPS9 -- Cloneable continuations with captured context on the DK machine  -- **DONE 2026-06-02**

The headline "next increment" from CPS8: re-point cloneable continuations onto
the multi-prompt `DK` machine so a captured continuation actually reifies the
*delimited context* -- the frames between the `cloneable-shift` and its
`cloneable-reset` -- and replays it, **multi-shot**, via `dk_invoke`.

**Why this was needed.** Before CPS9 the emitted cloneable continuation was
trivial: the captured "rest of the computation" was a no-op `__cont_fn`
(`return __value`). So `(cloneable-reset (+ 10 (cloneable-shift k ...)))` lost
the `+ 10` on resume, and with the shift nested inside an operand the legacy
path did not even compile (it referenced an undeclared `tur_current_reset_ctx`,
because `cps_expr_contains_cloneable_shift` does not look inside builtins). The
interpreter already captured context correctly; CPS9 brings the **compiled**
path up to the same semantics on the unbounded DK substrate.

**Implementation.** `src/compiler/emit_cps.c` (`emit_cps_cloneable_reset` and
the `emit_cps_cloneable_bridge_prelude`), hooked from
`emit_effects_cloneable_reset` and gated in `emit_module.c`. The key
realization is that the *existing* emitted `tur_cloneable_cont` machinery is
reused wholesale: the reified sub-continuation chain rides in the cont's `env`,
with a DK-invoking `cont_fn` (`__dk_cont_fn` -> `dk_invoke`), a clone that
copies the chain (`__dk_env_clone` -> `dk_copy_range`), and a drop that frees it
(`__dk_env_drop` -> `dk_free`). `resume`/`clone`/`drop` are unchanged;
**multi-shot falls out of `dk_invoke`'s internal copy** (resuming the same
handle repeatedly is safe).

- **CPS9.1 -- Context reification.** The delimited context is lambda-lifted into
  `DK` frames: each enclosing single-hole integer binary op (`+`, `-`, `*`)
  becomes a `DKFrame` (`static intptr_t __cc_ctx_N(intptr_t env, intptr_t v)`),
  with the non-hole operand captured into the frame env. Operands are pure int
  expressions evaluated **once** at capture time and reused on every resume --
  the standard delimited-control semantics. *Done:* `cl_collect_context` walks
  the reset body to the shift; frames are wrapped outermost-first so the
  innermost runs first on resume (inside-out).
- **CPS9.2 -- The captured sub as a cloneable cont.** At the reset the body runs
  on a `dk_shift(1, __cc_body_N, fenv, <frames> -> dk_prompt(1, dk_done()))`
  chain via `dk_run`. The shift body (`__cc_body_N`) copies the captured sub
  (`dk_copy_range`), wraps it in a `tur_cloneable_cont` (DK env + the bridge fn
  pointers), and hands it to the receiver `f`; `f`'s return value becomes the
  reset's value. *Done.*
- **CPS9.3 -- Selective + safe fallback.** A pure `cl_can_lower` feasibility
  check gates the path: it fires only for a **non-empty** supported context
  around exactly one `cloneable-shift` whose receiver has no live captures and
  whose result is intptr-safe (int). Everything else (including every existing
  empty-context fixture, e.g. `call-cc-star`, `cloneable-basic`,
  `cloneable-multi-resume`, and the RC/clone/defer fixtures) returns NULL and
  falls back to the legacy lowering, **byte-identical** -- no snapshot
  regenerated, full suite green. *Done.*
- **CPS9.4 -- Executing test.** `tests/fixtures/cloneable-context-multishot/`
  builds and runs five captured-context cases (`+`, `*`, nested `* (+ ...)`, `-`
  with the hole on the right, and a single handle resumed three times) and
  asserts the values (`23 6 46 197 3111`) -- exactly the context-replaying
  multi-shot semantics the compiled path previously lacked.

## Phase CPS10 -- Serializable continuations on the DK machine (CPS5.4)  -- **DONE 2026-06-02**

Re-express `serial-reset`/`serial-shift` on the DK machine so the captured
continuation is **marshalable**, realizing the CPS5.4 insight: a heap-reified
sub-continuation is a flat list of `(frame, env)` nodes, so it serializes by
writing each frame's stable *tag* plus its env and rebuilds by mapping the tag
back to a frame function -- no fiber/native-stack snapshot to walk, and portable
(tags are stable integers, not code addresses). In the compiled path
`serial-shift` was previously a **stub returning 0**; CPS10 gives it real
capture + marshaling on the same substrate as CPS8/CPS9.

**Implementation.** `src/compiler/emit_cps.c` (`emit_cps_serial_reset` +
`emit_cps_serial_runtime_prelude`), hooked from `emit_effects_serial_reset`,
gated in `emit_module.c`, with three runtime builtins registered in
`builtins.c`. The context walk is shared with CPS9 (`collect_ctx` parameterized
by shift kind). To keep tags stable across a serialize/deserialize boundary the
context frames are a **fixed** set emitted once into the preamble (`__sk_add` /
`__sk_mul` / `__sk_subr` / `__sk_subl`, keyed by `SK_TAG_*`), rather than the
per-site lambda-lifted frames of CPS9.

- **CPS10.1 -- Reify + capture.** A `(serial-reset CTX[serial-shift f v])` runs
  on a `dk_shift(1, __sk_body_N, fenv, <tagged frames> -> dk_prompt(1,
  dk_done()))` chain via `dk_run`; `__sk_body_N` copies the captured sub and
  hands it to `f` as the serial-continuation handle (a `DK*`). *Done.*
- **CPS10.2 -- Marshal.** `tur_serial_cont_serialize` walks the captured chain's
  frames (up to the prompt) and writes `[len][n][(tag, env) x n]` into a
  length-prefixed buffer (the stdlib `bytes` layout);
  `tur_serial_cont_deserialize` rebuilds a runnable `[frames..., prompt, done]`
  chain from the tags. `tur_serial_cont_resume` runs a (possibly deserialized)
  chain via `dk_invoke`. The env is an int, marshaled inline; richer env types
  via the `Serializable` typeclass (per-frame `.serialize`/`.deserialize`) are
  the documented follow-on. *Done.*
- **CPS10.3 -- Selective + safe fallback.** `sk_can_lower` gates the path
  (supported context -- including the empty-context whole-body case -- around one
  `serial-shift`, intptr-safe result). `serial-reset` with no shift returns NULL
  and falls back to the legacy "just evaluate the body" lowering, so
  `serial-reset-basic` is unchanged; no snapshot regenerated. *Done.*
- **CPS10.4 -- Executing test.** `tests/fixtures/serial-context-marshal/`
  round-trips captured continuations through serialize/deserialize before
  resuming (`+`, nested `* (+ ...)` resumed directly, empty context, and `-`
  with the hole on the right), asserting `15 10 7 95` -- the marshaling proof the
  compiled path previously lacked.
- **Note on the interpreter runtime.** The tree-walking interpreter has its own
  serial-continuation runtime (`src/runtime/serial.{h,c}`: a `SerialFrame` chain
  keyed by interned symbols). CPS10 is the **compiled-path** substrate
  (DK chain, integer tags); the two marshal formats are independent (the
  fixture is compiled-only, like the cloneable fixtures). Unifying them onto a
  single substrate is a future cleanup, tracked with the OQ-CPS2 keep-or-remove
  decision.

## Phase CPS11 -- `call/cc*` captures up to an enclosing cloneable-reset  -- **DONE 2026-06-02**

The last operator on the legacy path. `(call/cc* f)` used to desugar
unconditionally to `(cloneable-reset (cloneable-shift f 0))` -- a freshly
installed inner reset, so the captured continuation was always empty (the
identity), discarding any surrounding context. CPS11 gives `call/cc*` the real
delimited semantics: it captures the continuation up to the nearest *enclosing*
cloneable-reset.

**Implementation.** `src/compiler/elab_effects.c` (`elab_call_cc_star`). The
elaborator already tracks `cloneable_reset_depth`. The desugaring is now
context-sensitive:

- **Inside a cloneable-reset (`cloneable_reset_depth > 0`):** desugar to a *bare*
  `(cloneable-shift f 0)` that binds to the enclosing reset. The reset body is
  then the real delimited context around the shift, which the **CPS9** lowering
  already reifies onto the DK machine and replays multi-shot. E.g.
  `(cloneable-reset (+ 10 (call/cc* f)))` hands `f` a continuation computing
  `(+ 10 [])`; resuming it with 5 yields 15, and a clone + original resume
  ((+ 10 1) + (+ 10 2)) yields 23.
- **No enclosing cloneable-reset:** keep the original fresh-delimiter sugar
  `(cloneable-reset (cloneable-shift f 0))`, so `call/cc*` still works on its own
  with the identity continuation -- the existing `call-cc-star` /
  `cloneable-multi-resume` semantics are preserved byte-identically (those
  fixtures have no enclosing reset).

This is purely additive: no existing fixture uses `call/cc*` inside a
cloneable-reset, so every shipped snapshot and both the compiled and interpreter
suites stay green with no regeneration.

- **CPS11.1 -- Context-sensitive desugar.** *Done:* bare shift under an enclosing
  reset; fresh-reset sugar otherwise.
- **CPS11.2 -- Executing test.** `tests/fixtures/callcc-star-context/` proves the
  enclosing-reset capture: single resume through `(+ 10 [])` (= 15), multi-shot
  ((10+1)+(10+2) = 23), nested `(* 2 (+ 1 []))` (= 10), plus the no-reset
  identity case (100, 200) to lock in the preserved sugar.

## Follow-on increments -- **DONE 2026-06-02**

Post-CPS11 polish, each landed additively (selective lowering + legacy fallback;
no shipped snapshot regenerated):

- **OQ-CPS2 resolved -- keep the fiber path.** Recorded in Open questions: the
  fiber/inlined runtime stays as the selective fallback (for shapes outside the
  CPS subset) and the bounded fast path; removal is gated on the subset becoming
  total plus a CPS7.2 head-to-head. Doc-only.
- **Broaden the context subset: integer division.** `/` joins `+`/`-`/`*` in the
  shared context grammar (`collect_ctx`), aborting on a zero divisor to match
  `BS_DIV_CHECK`. A `frame_c_expr` helper keeps the per-site cloneable frames and
  the fixed tagged serial frames (`SK_TAG_DIVR`/`SK_TAG_DIVL`) in lock-step.
  Fixture: `context-division`.
- **Broaden the context subset: 2-arg call frames + name-keyed serial
  marshaling.** Contexts may now wrap the hole in a call to a resolved 2-arg
  top-level function (not just arithmetic). Cloneable frames are lambda-lifted
  per-site; serial frames marshal by the target's emitted C name via a
  self-registering (constructor-populated) `name <-> DKFrame` table -- the
  name-keyed scheme `serial.c` uses -- with a self-describing record format
  (`[tag][env]` for arithmetic, `[SK_TAG_CALL][name][env]` for calls, memcpy'd,
  variable length). Env values are int for now (`env_kind_ok`); cstr/struct envs
  await the value-typed half of `cont<T>` (see below). Fixture: `context-call-frame`.
- **CC4: `(k v)` application sugar + `:cont` parameter typing.** A continuation
  parameter typed `:cont` is invoked directly as `(k v)`, desugaring (in
  `elab_call`) to a continuation resume; `TY_CONT` lowers to an `int64_t` handle
  (`type_c_name`). Initially scoped to cloneable continuations; the flavored-cont
  increment below extends it to escape and serial. Fixture: `callcc-kv-sugar`.
- **Flavored `cont` -- `(k v)` for escape/cloneable/serial.** The cont type gains
  a `flavor` (`ContFlavor`: cloneable / escape / serial), set by the parameter
  annotation `:cont` / `:escape-cont` / `:serial-cont`. The `(k v)` desugar
  selects the resume runtime by flavor (`tur_cloneable_cont_resume` /
  `tur_escape_resume` / `tur_serial_cont_resume`), so escape and serial
  continuations get the same `(k v)` ergonomics cloneable already had. This is
  the keystone for unifying `(k v)` across flavors. Fixture: `cont-flavors`
  (cloneable multi-shot 23, serial 15, escape one-shot upward abort 42).

  *Remaining (value-typed `cont<T>`):* the resume builtins and the shift node are
  still int-typed, so a non-int continuation value (cstr/struct) can't be
  expressed at the surface yet -- which is what still blocks richer **serial**
  env types. The flavor half of "flavored `cont<T>`" is done; the *value* half
  (a non-int result `T` threaded through the shift node, the resume result, and
  -- for serial -- a `Serializable`-typeclass env codec) layers on top. The
  parametric `(cont T)` annotation additionally needs `cont` registered as an
  arrow-kind (`* -> *`) constructor. Broadening contexts past single-hole binops
  / 2-arg calls (lets, if-branches) is a separate grammar extension.

---

## Status summary (2026-06-02)

CPS0--CPS7 are **implemented and tested**. The substrate is built and exercised
end-to-end as standalone, inspectable machinery, with the default compile
pipeline and the full fixture suite green at every step:

| Phase | Deliverable | Form | Tests |
|---|---|---|---|
| CPS0 | Ratified model (selective CPS, multi-prompt, trampoline, fiber coexistence) | doc | -- |
| CPS1 | May-capture coloring | live analysis (additive `FnDef.cps_colored`) | `--dump-cps-coloring`; run-flags |
| CPS2 | ANF/CPS IR | dump-only (`cps_ir`) | `--dump-cps`; run-flags |
| CPS3 | Selective lowering + direct<->CPS bridging | dump-only | `dump-cps-bridge`; `cps-mixed-coloring` fixture |
| CPS4 | Heap-reified continuations + trampoline | standalone runtime (`cps_rt`) | `tur_cps_rt_unit` (incl. 500k-frame) |
| CPS5 | Multi-prompt machine + implicit root prompt | standalone runtime (`cps_prompt`) | `tur_cps_prompt_unit` |
| CPS6 | Retire ceiling on the CPS path | runtime docs/scoping | grep + CPS4.4 |
| CPS7 | Perf gate (identical snapshots) + bench + docs | doc/bench | suite + 43 Mstep/s bench |
| CPS8 | Codegen wiring: base reset/shift/shift0 (+ undelimited call/cc/escape) on the DK machine | live codegen (`emit_cps`) | `continuation-substrate` fixture + regen'd base-shift snapshots |
| CPS9 | Cloneable continuations with captured context, multi-shot via `dk_invoke` | live codegen (`emit_cps`) | `cloneable-context-multishot` fixture (legacy fallback keeps existing snapshots) |
| CPS10 | Serializable continuations with captured context, marshalable (serialize/deserialize) | live codegen (`emit_cps`) | `serial-context-marshal` fixture (legacy fallback keeps `serial-reset-basic`) |
| CPS11 | `call/cc*` captures up to an enclosing cloneable-reset (real semantics) | context-sensitive desugar (`elab_call_cc_star`) -> CPS9 lowering | `callcc-star-context` fixture (no-reset sugar preserved: `call-cc-star`) |

**Codegen wiring -- CPS8/CPS9/CPS10/CPS11, landed.** The remaining piece called
out under CPS3.1/CPS5.1/5.2/CPS6 -- re-pointing the delimited lowerings off the
legacy runtime and onto this substrate -- is implemented for **base**
`reset`/`shift`/`shift0` (CPS8), **undelimited `call/cc`/`escape`** (CPS8.5),
**cloneable continuations with a captured delimited context** (CPS9:
`cloneable-reset`/`cloneable-shift`, multi-shot via `dk_invoke`), **serializable
continuations** (CPS10: `serial-reset`/`serial-shift`, marshalable via
serialize/deserialize), and **`call/cc*`** (CPS11: captures up to an enclosing
cloneable-reset). The full delimited/undelimited operator set now executes on
the DK machine; the corresponding exit criteria are met. Only perf/cleanup
follow-ons remain: OQ-CPS2 (**resolved**: keep the fiber path as the selective
fallback + bounded fast path); richer serial env types via the `Serializable`
typeclass; CC4 `(k v)` sugar / `cont<T>` typing; broadening the context subset.

---

## Exit criteria

- Base `reset`/`shift`/`shift0` execute on the CPS substrate with no semantic
  change (CPS8: `continuation-basic`/`-advanced`/`shift-result-typing`/
  `shift0-result-typing` snapshots regenerated and green; `continuation-substrate`
  asserts the runtime values). **Done.** Cloneable continuations with a captured
  context (CPS9: `cloneable-context-multishot`), serializable continuations
  (CPS10: `serial-context-marshal`, serialize/deserialize round-trip), and
  `call/cc*` capturing up to an enclosing cloneable-reset (CPS11:
  `callcc-star-context`) also run on the substrate. Every delimited/undelimited
  operator now has a CPS-substrate lowering for its supported shape subset; the
  legacy lowering survives only as the selective fallback for shapes outside that
  subset (the keep-or-remove decision for it is OQ-CPS2).
- A continuation captured across a call depth far greater than 16 captures and
  resumes correctly (the fiber path's `NULL`-on-overflow no longer applies).
  **Done in emitted code (CPS8.5/CPS9):** `escape-deep-capture` captures and
  resumes from 5000 frames deep, and CPS9 reifies a *resumable* delimited
  sub-continuation in emitted code (a `DK` chain), replayed multi-shot via
  `dk_invoke` -- both unbounded by construction (`dk_*` has no frame ceiling).
- An implicit root prompt exists around `main`; an undelimited `call/cc` with no
  explicit `reset` captures to it and resumes. **Done (CPS8.5):** `(call/cc f)`/
  `(escape f)` capture a real one-shot continuation against an implicit
  program-wide prompt with no enclosing `reset`; `callcc-real-capture` resumes
  it (result 42) and `escape-deep-capture` escapes from 5000 frames deep
  (unbounded). Multi-shot/downward re-entry stays with `call/cc*`.
- Uncolored direct-style benchmarks show no regression beyond the CPS7.1
  threshold.
- `control-flow-completeness-audit.md` CF4 is marked resolved.

---

## Open questions

- **OQ-CPS1 -- Selective vs whole-program.** Ratified selective in CPS0.1; the
  fallback (whole-program CPS) is simpler to implement but pays the trampoline
  tax everywhere. Revisit only if the coloring analysis proves too conservative
  to be useful (e.g. pervasive indirect calls force most code colored).
- **OQ-CPS2 -- Keep the fiber fast path? RESOLVED 2026-06-02: KEEP (as the
  selective fallback + bounded fast path).** Two independent reasons make "keep"
  the correct call today, and neither is a close perf judgement:

  1. **Correctness, not just speed.** The CPS-substrate lowerings (CPS8--CPS11)
     are *selective*: each fires only for its supported shape subset and returns
     NULL otherwise, falling back to the legacy fiber/inlined runtime. Shapes
     still outside the subset -- e.g. a `shift` in an `if`/`match` *branch*
     (CPS8.3), a non-arithmetic delimited context (calls, `let`-bodies that
     aren't a single-hole `+`/`-`/`*` chain), float-typed results, or a
     `cloneable-shift` with live `Clone` captures -- are handled *only* by the
     legacy path. Removing it would regress those to "not compilable," so removal
     is **blocked on the CPS subset becoming total**, not on a benchmark.
  2. **It is genuinely a fast path for the common small case.** For a shallow
     `reset` with a nearby `shift` the fiber snapshot copies a handful of frames
     with no per-step heap thunk; the trampoline pays one indirect call + one
     small allocation per bounce (CPS7.2). The fiber path therefore stays as the
     depth-bounded fast path the original OQ-CPS2 anticipated; the CPS path is the
     unbounded/serializable/multi-shot substrate for everything the fiber path
     cannot express.

  **Removal criteria (revisit when all hold).** (a) The CPS context grammar
  covers the shapes above (tracked by the "broaden the context subset" follow-on
  and CC4); (b) a CPS7.2 head-to-head shows the trampoline within an acceptable
  factor of the fiber snapshot on the small-`reset` microbenchmark (or platform
  TCO removes the trampoline tax, OQ-CPS3); (c) the `TUR_CONT_MAX_CAPTURED_FRAMES`
  fiber capture is no longer reachable from any non-fallback lowering. Until then
  the two substrates coexist exactly as ratified in CPS0.4, and
  `TUR_CONT_MAX_CAPTURED_FRAMES` stays scoped + documented as fiber-only (CPS6).
- **OQ-CPS3 -- Trampoline vs platform TCO.** If all supported C compilers/targets
  give reliable tail calls under our codegen, the trampoline could be dropped on
  those targets. Treat as a CPS7 perf follow-up, not a blocker.
