# Whole-Program CPS Transform -- Implementation Plan (CPS0--CPS7)

> **Status:** Not started. This is the substrate that unblocks *undelimited*
> control (real Scheme `call/cc`, an implicit program-wide prompt) and removes
> the bounded-capture ceiling on the existing delimited runtime.
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
> **Last updated:** 2026-06-01
>
> **Related:**
> - [`call-cc-completion-plan.md`](call-cc-completion-plan.md) -- the primary
>   consumer; reframed to be built *on top of* this plan (undelimited `call/cc`
>   + implicit program-wide prompt).
> - [`multishot-continuations-plan.md`](../archive/multishot-continuations-plan.md)
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
  [`multishot-continuations-plan.md`](../archive/multishot-continuations-plan.md);
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

## Phase CPS0 -- Ratify the model

- **CPS0.1** Confirm **selective** CPS (color only may-capture functions) over
  whole-program CPS. *Done when:* this section is annotated "ratified" with a
  date and the coloring rule (CPS1) is agreed.
- **CPS0.2** Adopt the multi-prompt delimited-control framework as the single
  substrate: `reset` -> fresh prompt, `shift`/`shift0` -> sub-continuation
  capture to nearest prompt, `call/cc` -> capture to the implicit root prompt.
  Record how `shift` (re-install prompt) vs `shift0` (do not) map onto
  `pushSubCont` vs not. *Done when:* the mapping table is in this doc and
  matches the existing `EX_SHIFT`/`EX_SHIFT0` semantics (CF2).
- **CPS0.3** Choose the native-stack-bounding strategy: a **trampoline** driver
  for CPS'd code (return-to-driver thunks) vs. relying on the C compiler's tail
  calls. Default: trampoline, because portable TCO across our targets is not
  guaranteed. *Done when:* the decision and its perf budget are recorded.
- **CPS0.4** Decide coexistence with the fiber runtime during transition: the
  fiber path (`fiber_ctx_*.S`, `tur_cont`) stays for delimited code until CPS5
  re-expresses it, then is removed or retained only as a fast path. *Done
  when:* a deprecation/coexistence note is recorded.

## Phase CPS1 -- "May-capture" coloring analysis

A function is **colored** (must be CPS'd) iff it can dynamically reach a control
operator: `call/cc`, `escape`, `shift`/`shift0`, `call/cc*`, `serial-shift`, or
an effect `perform`. Coloring is transitive through (possibly) called
functions; indirect/closure calls whose target is unknown are conservatively
colored.

- **CPS1.1** Build the call graph (including a conservative over-approximation
  for indirect calls / closures). *Done when:* the graph is available to the
  elaborator/IR stage.
- **CPS1.2** Seed the colored set with the control-operator primitives and
  effect performs; propagate backward to all callers (least fixed point).
  *Done when:* a function reachable to a control op is colored; a provably pure
  leaf is not.
- **CPS1.3** Expose the coloring as IR metadata consumed by CPS3. *Done when:* a
  fixture dump shows `pure-arith` uncolored and `uses-shift` colored.
- **CPS1.4** Fixture: `tests/fixtures/cps-coloring/` -- a small module asserting
  the colored/uncolored partition via a debug dump (gated behind a dev flag).

## Phase CPS2 -- CPS/ANF intermediate representation

- **CPS2.1** Add an A-normal-form normalization pass for colored functions so
  every non-trivial subexpression is named -- the precondition for a clean CPS
  translation. *Done when:* colored function bodies are in ANF; uncolored
  bodies are untouched.
- **CPS2.2** Add the CPS IR node(s): an explicit continuation parameter, `k`,
  threaded through colored functions; tail positions become `k` applications.
  Reuse `TY_CONT` (`src/compiler/types.h:92`) as the continuation type. *Done
  when:* the IR can represent `(f x k)` and `(k v)`.
- **CPS2.3** Type the continuation parameter as `cont<T>` end-to-end so CPS3's
  output type-checks with the same rule `shift` already uses (CF2). *Done
  when:* a CPS'd identity function round-trips through the type checker.

## Phase CPS3 -- Selective CPS lowering + boundary bridging

- **CPS3.1** Lower colored functions to CPS: add the `k` parameter, sequence
  ANF bindings into nested continuations, translate tail calls to `k`
  applications. *Done when:* a colored function with a control op compiles
  through the new path.
- **CPS3.2** Leave uncolored functions in direct style. At a call from CPS'd
  code into a direct function, call normally and feed the result to `k`; at a
  call from direct code into a colored function, enter via a driver that
  supplies the initial `k` (the current direct continuation reified). *Done
  when:* a direct `main` calling a colored helper that calls `shift` works.
- **CPS3.3** Fixture: `tests/fixtures/cps-mixed-coloring/` -- direct hot loop
  calling a colored callee; assert correct result and (via CPS7 harness) no
  allocation on the uncolored path.

## Phase CPS4 -- Heap-reified continuations + trampoline

- **CPS4.1** Represent a captured continuation as a heap closure chain (not a
  `tur_frame[16]` snapshot). Capture = take the current `k`; resume = call it.
  *Done when:* a continuation captured below depth 16 resumes correctly
  (the old ceiling no longer participates on the CPS path).
- **CPS4.2** Implement the trampoline driver per CPS0.3 so CPS'd tail calls
  return thunks to the driver, bounding native stack. *Done when:* a
  deep (>>16) mutually-recursive colored loop runs in constant native stack.
- **CPS4.3** Preserve `defer`/frame-teardown semantics: the existing
  `tur_frame_fire_lifo*` defer firing on drop/resume must have an equivalent in
  the reified-continuation model (defers attached to continuation segments).
  *Done when:* a fixture proves `defer` fires exactly once on normal return,
  on abort, and is not double-fired on resume.
- **CPS4.4** Fixture: `tests/fixtures/cps-deep-capture/` -- capture/resume a
  continuation across a call depth well beyond 16; must succeed where the
  fiber path returned `NULL`.

## Phase CPS5 -- Multi-prompt delimited substrate + implicit root prompt

- **CPS5.1** Implement prompts on the CPS substrate: `reset` pushes a fresh
  prompt; `shift`/`shift0` capture the sub-continuation up to the nearest
  prompt (`shift` re-installs it on resume, `shift0` does not). Re-express the
  existing `EX_RESET`/`EX_SHIFT`/`EX_SHIFT0` lowerings to target prompts.
  *Done when:* the entire existing `continuation-*` fixture suite passes
  unchanged on the CPS path.
- **CPS5.2** Re-express `call/cc*` (cloneable/multi-shot) and `serial-*`
  on prompts. Cloneable resume = re-enter the captured sub-continuation
  multiple times; one-shot = consume after first resume. *Done when:* the
  `call-cc-star` and serial-continuation fixtures pass on the CPS path.
- **CPS5.3** Install an **implicit root prompt** around the program entry
  (`main`). This is the prompt an undelimited `call/cc` captures up to. *Done
  when:* a `call/cc` with no enclosing `reset` captures a continuation that
  reaches the top of the program and resumes correctly. (Consumed by
  `call-cc-completion-plan.md`, which flips OQ1.)
- **CPS5.4** Note (no code): a heap-reified sub-continuation is a closure chain,
  which the `Serializable` machinery can already walk -- record any
  simplification this enables for `serial-*` and cross-link the serializable
  guide. *Done when:* the cross-link and a short note land in CPS7 docs.

## Phase CPS6 -- Retire the capture ceiling

- **CPS6.1** Remove the `n_frames > TUR_CONT_MAX_CAPTURED_FRAMES -> NULL`
  early-outs (`src/runtime/runtime.c:103`, `:180`) for the CPS path; capture is
  unbounded. Keep the constant only if the fiber fast path is retained per
  CPS0.4. *Done when:* `grep` shows no live `NULL`-on-overflow on the CPS path
  and the deep-capture fixture (CPS4.4) passes.
- **CPS6.2** Update `runtime.h` doc comments that describe continuations as
  bounded 16-frame snapshots. *Done when:* the struct comments match the
  reified model.

## Phase CPS7 -- Benchmarks, perf gates, docs

- **CPS7.1** Benchmark direct-style hot paths (no control ops) before/after to
  prove selectivity keeps them allocation- and overhead-free. *Done when:* a
  perf gate asserts no regression beyond an agreed threshold on the uncolored
  benchmarks.
- **CPS7.2** Benchmark colored paths (shift/reset, call/cc, backtracking) to
  size trampoline + allocation overhead. *Done when:* numbers are recorded in
  the doc and deemed acceptable for the use cases.
- **CPS7.3** Docs: extend `docs/guides/effects-system-guide.md` with the
  prompt model; document the implicit root prompt and unbounded capture. *Done
  when:* the guide describes the substrate and links here.
- **CPS7.4** Update `control-flow-completeness-audit.md` CF4: mark "whole-program
  CPS pass" **resolved by CPS0--CPS6**. *Done when:* the audit points here.

---

## Exit criteria

- The full existing `continuation-*`, `call-cc-star`, and serial-continuation
  fixtures pass on the CPS substrate with no semantic change.
- A continuation captured across a call depth far greater than 16 captures and
  resumes correctly (the fiber path's `NULL`-on-overflow no longer applies).
- An implicit root prompt exists around `main`; an undelimited `call/cc` with no
  explicit `reset` captures to it and resumes.
- Uncolored direct-style benchmarks show no regression beyond the CPS7.1
  threshold.
- `control-flow-completeness-audit.md` CF4 is marked resolved.

---

## Open questions

- **OQ-CPS1 -- Selective vs whole-program.** Ratified selective in CPS0.1; the
  fallback (whole-program CPS) is simpler to implement but pays the trampoline
  tax everywhere. Revisit only if the coloring analysis proves too conservative
  to be useful (e.g. pervasive indirect calls force most code colored).
- **OQ-CPS2 -- Keep the fiber fast path?** Retaining `fiber_ctx_*.S` for shallow
  delimited capture could beat CPS on the common small-`reset` case. Decide in
  CPS0.4 / revisit with CPS7.2 numbers: keep as a depth-bounded fast path, or
  remove for a single substrate.
- **OQ-CPS3 -- Trampoline vs platform TCO.** If all supported C compilers/targets
  give reliable tail calls under our codegen, the trampoline could be dropped on
  those targets. Treat as a CPS7 perf follow-up, not a blocker.
