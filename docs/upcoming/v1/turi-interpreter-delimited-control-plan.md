---
title: Turi interpreter delimited-control completion -- Plan
category: Planning
description: Make the tree-walking interpreter's effect-handler/continuation machinery multi-shot, escape-safe, and nested-handler-aware, so multishot resume, resuming a continuation after its handle block returns, and resuming through nested handlers all work under `tur --interpret` as they do compiled. Builds on the explicit-stack (trampoline) evaluator.
---

# Turi interpreter delimited-control completion -- Plan

> **LANDED 2026-06-14.** Implemented on the driver work-stack (not a fiber-stack
> deep-copy): a *capturable* handle installs a `DK_PROMPT` and runs its body on
> `eval_drive_ex`'s work-stack; `perform` captures the slice between itself and
> the matching prompt as a heap-owned `TuriWsCont` (`src/turi/eval.c`). The
> ucontext-fiber path is retained as the fallback for performs that black-box
> away from the work-stack (`while`/`try`/`async`/native-HOF bodies); a static,
> conservative `ws_capturable` predicate picks the path per handle, guaranteeing
> a work-stack handle never nests a fiber handle that must escape to it. All five
> target fixtures pass under `--interpret` with their `requires.tur-only` markers
> removed; `tests/run-turi.sh` shows no regressions and `check_turi_parity.py` is
> 0-gap.
>
> Step 2 (multishot) -- true multishot on both paths: the interpreter clones the
> captured slice per resume (`clone_ws_slice`), and the compiled path was fixed
> to snapshot/restore the suspended fiber stack per resume. `multishot-handler`/
> `fh-multishot-value` now yield `30`. The originally-degenerate compiled
> behaviour and its fix are recorded in
> [docs/archive/history/turi-effect-multishot-degenerate-resume.md](../../archive/history/turi-effect-multishot-degenerate-resume.md)
> (now RESOLVED).

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
[docs/archive/history/turi-interpreter-delimited-control-gaps.md](../../archive/history/turi-interpreter-delimited-control-gaps.md);
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

**Sequencing: the trampoline evaluator is now landed**
([turi-eval-trampoline-plan.md](turi-eval-trampoline-plan.md) T1-T3.2b, plus the
frame-reuse follow-up [turi-cek-frame-reuse-tco-plan.md](turi-cek-frame-reuse-tco-plan.md)
F1-F5), so this plan's hard dependency is satisfied and **it can land now** --
build delimited control on the existing `DriveCont` work-stack. Attempting
clonable continuations on the current ucontext-fiber representation is possible
(deep-copy the fiber stack on capture) but fragile -- it duplicates raw C stack
memory and interacts badly with ASan -- so it is a fallback, not the primary
path. See the `tur_continuation_snapshot` overlap noted in
[turi-multishot-continuation-snapshot-miscompile.md](../../archive/turi-multishot-continuation-snapshot-miscompile.md):
the same clone primitive backs that fix.

### Scope boundary: capture through a native HOF (deferred to SR)

This plan captures a continuation as the work-stack slice from the capture point
up to the matching prompt. That is complete for control that flows through
turi-code forms (`perform`/`handle`/`shift`/`reset`) -- which is **all five
target fixtures**. It is *not* complete when the capture point sits inside the
callback of a native / inline-C higher-order function (e.g. a `shift` inside the
`fn` passed to a recursive `option-map`): today that callback runs on a live C
frame the work-stack slice cannot see, so the captured continuation would omit
the native's pending work. Carve that case out cleanly (a clear error, matching
the existing capturable-grammar limits in
[turi-capturing-shift-unimplemented.md](../../archive/history/turi-capturing-shift-unimplemented.md))
and **defer it to**
[turi-cek-stackless-reentry-plan.md](turi-cek-stackless-reentry-plan.md) (SR),
which puts native callbacks on the work-stack (`DK_NATIVE_RESUME`) so this plan's
capture/clone path extends across them with no rework -- it adds a frame kind the
capture logic already knows how to copy. **Do this plan first** (higher-severity
crash fixes, dependency already met); SR follows and lifts this restriction.

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
- A continuation captured *inside a native HOF callback* produces a clean error
  (not a crash or silent miscompile), deferred to SR; the five target fixtures do
  not exercise that case.
- `bash tests/run.sh` unchanged (all five already pass compiled);
  `tools/check_turi_parity.py` 0-gaps; `bash tests/run-turi.sh` green with the
  five newly added.

## See also

- [docs/archive/history/turi-interpreter-delimited-control-gaps.md](../../archive/history/turi-interpreter-delimited-control-gaps.md)
  -- root-cause report with minimal repros (this plan's source).
- [docs/upcoming/v1/turi-eval-trampoline-plan.md](turi-eval-trampoline-plan.md)
  -- the explicit-stack evaluator this plan builds on (now landed, with the
  [frame-reuse follow-up](turi-cek-frame-reuse-tco-plan.md)).
- [docs/upcoming/v1/turi-cek-stackless-reentry-plan.md](turi-cek-stackless-reentry-plan.md)
  -- the follow-up (SR) that lands *after* this plan and lifts its
  "capture through a native HOF" restriction by putting native callbacks on the
  work-stack. Shares the resume protocol.
- [docs/archive/turi-multishot-continuation-snapshot-miscompile.md](../../archive/turi-multishot-continuation-snapshot-miscompile.md)
  -- shares the clone primitive (step 5).
- [docs/archive/history/turi-interpret-flip-residual-plan.md](../../archive/history/turi-interpret-flip-residual-plan.md)
  -- W5 flip (landed); bucket R4 tracks these as the remaining continuation gap.
