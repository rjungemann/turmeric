---
title: Turi interpreter delimited-control completion -- Plan
category: Planning
description: Make the tree-walking interpreter's effect-handler/continuation machinery multi-shot, escape-safe, and nested-handler-aware, so multishot resume, resuming a continuation after its handle block returns, and resuming through nested handlers all work under `tur --interpret` as they do compiled. Builds on the explicit-stack (trampoline) evaluator.
---

# Turi interpreter delimited-control completion -- Plan

## Status and scope

Under `tur --interpret`, the effect-handler/continuation machinery
(`src/turi/eval.c` fiber path, around `:691+`) is **one-shot and single-frame**.
Three capabilities the compiled path implements correctly fail in the
interpreter, and all five affected fixtures are carved `requires.tur-only`
(reason `interp-continuation`):

| Capability | Fixtures | Symptom |
| --- | --- | --- |
| Multishot resume (`^multishot k` resumed >1x) | `fh-multishot-value`, `multishot-copy-capture`, `multishot-handler` | SIGABRT (rc=134), empty stdout |
| Resume after the `handle` block returned (escaping `k`) | `effect-capture-k` | ASan heap-use-after-free |
| Resume *through* nested handlers | `effect-handler-capture-nested` | wrong error: `unhandled effect: B` |

The full root-cause analysis with minimal repros lives in
[docs/reported/turi-interpreter-delimited-control-gaps.md](../../reported/turi-interpreter-delimited-control-gaps.md);
this plan turns that report into a sequenced implementation.

Severity: **high among the carve-outs**. Two of the three are hard crashes
(SIGABRT / heap-use-after-free), not silent miscompiles -- but they are the
single largest coherent interpreter-feature hole remaining after the W5 flip.

## Why these are one feature, not three bugs

Each failure is a different consequence of the same representational choice: a
continuation is backed by a **single ucontext fiber** whose lifetime is tied to
the enclosing `handle` frame and which is *consumed* on first resume.

- **Multishot** needs the captured continuation to be **clonable** -- each
  `resume` must run a fresh copy of the suspended stack + saved state, not
  re-enter a finished/freed fiber.
- **Escaping** needs the continuation state to be **heap-owned** with the
  lifetime of the continuation *value*, so it survives after its `handle` frame
  returns.
- **Nested** needs resume to **re-establish the enclosing handler frames** (the
  prompt stack up to the matching handler), so a `perform` inside a resumed
  continuation still finds the outer handlers.

A continuation representation that is heap-owned, clonable, and carries its
prompt/handler stack satisfies all three.

## Dependency: the explicit-stack evaluator

The clean way to get a heap-owned, copyable continuation is to stop relying on
the native C stack (and ucontext fibers) for suspension. That is exactly the
goal of
[docs/upcoming/v1/turi-eval-trampoline-plan.md](turi-eval-trampoline-plan.md):
once `eval_expr` is reified onto an explicit heap work-stack, a continuation
becomes a heap value (a snapshot of the work-stack + environment chain up to the
prompt) that can be copied and re-entered -- precisely what multishot, escaping,
and nested resume all require.

**Recommended sequencing: land the trampoline evaluator first**, then build
delimited control on top of its reified stack. Attempting clonable continuations
on the current ucontext-fiber representation is possible (deep-copy the fiber
stack on capture) but fragile -- it duplicates raw C stack memory and interacts
badly with ASan -- so it is a fallback, not the primary path. See the
`tur_continuation_snapshot` overlap noted in
[turi-multishot-continuation-snapshot-miscompile.md](../../reported/turi-multishot-continuation-snapshot-miscompile.md):
the same clone primitive backs that fix.

## Proposed implementation (on the trampoline)

1. **Reify continuations as heap values.** Define a `TURI_CONT` value that owns
   (a) a copy of the explicit work-stack frames from the capture point up to the
   matching prompt, (b) the environment chain those frames close over, and (c)
   the handler/prompt stack in force at capture. Capture happens at the
   `perform` site when the handler binds `k` (`^once` / `^multishot` /
   `^linear`).

2. **Clone on resume for multishot.** `resume k v` runs a **copy** of the
   continuation's frames with the resumed value substituted at the hole, leaving
   the original intact for a subsequent `resume`. `^once`/`^linear` keep the
   current move-once semantics (consume on first resume; the linearity checker
   already enforces this and shares the elaborator, so no new diagnostics).

3. **Heap lifetime decouples from `handle`.** Because the continuation owns its
   frames, storing `k` and resuming after the `handle` returns (`effect-capture-k`)
   is just resuming a still-live heap value -- no use-after-free.

4. **Re-establish enclosing handlers on resume.** The captured handler/prompt
   stack (1c) is reinstalled when a continuation runs, so a `perform (B)` inside
   a resumed inner continuation finds the outer B/A handlers
   (`effect-handler-capture-nested` -> `321`).

5. **Wire `tur_continuation_snapshot`.** Map the snapshot/clone primitive to the
   new clone path so the related snapshot miscompile is fixed by the same
   machinery.

## Validation

- Per fixture, under `ASAN_OPTIONS=detect_leaks=0 ./build/tur --interpret`:
  - `multishot-handler` -> `20`; `fh-multishot-value`, `multishot-copy-capture`
    -> their expected outputs, rc=0, no SIGABRT.
  - `effect-capture-k` -> `0` then `10`, rc=0, no ASan heap-use-after-free.
  - `effect-handler-capture-nested` -> `321`.
- Remove each `requires.tur-only` marker as it passes; the post-W5 denylist
  harness then runs it automatically. Update the reason inventory in the
  delimited-control report.
- Run the full effect/continuation interpreter surface to guard against
  regressions in the already-green one-shot cases (`effect-*`, `shift*`,
  `callcc*`, `cloneable-context-*`, `serial-context-*`).
- `bash tests/run.sh` unchanged (all five already pass compiled);
  `tools/check_turi_parity.py` 0-gaps; `bash tests/run-turi.sh` green with the
  five newly added.

## See also

- [docs/reported/turi-interpreter-delimited-control-gaps.md](../../reported/turi-interpreter-delimited-control-gaps.md)
  -- root-cause report with minimal repros (this plan's source).
- [docs/upcoming/v1/turi-eval-trampoline-plan.md](turi-eval-trampoline-plan.md)
  -- the explicit-stack evaluator this plan builds on.
- [docs/reported/turi-multishot-continuation-snapshot-miscompile.md](../../reported/turi-multishot-continuation-snapshot-miscompile.md)
  -- shares the clone primitive (step 5).
- [docs/archive/history/turi-interpret-flip-residual-plan.md](../../archive/history/turi-interpret-flip-residual-plan.md)
  -- W5 flip (landed); bucket R4 tracks these as the remaining continuation gap.
