---
title: Heap-bound the C-scoped boundary forms (retire eval_depth) -- Plan
category: Planning
description: The last source of unbounded C recursion in the tree-walking interpreter after turi-cek-stackless-reentry landed is a small set of genuinely C-scoped boundary forms -- catch-unwind (setjmp panic boundary), atomically (STM transaction + retry), and async/await/handle (fibers). Each nests one C frame / setjmp / ucontext per level, so deep nesting C-recurses (the compiled path SIGSEGVs on the same patterns). Model each on the driver work-stack so the eval_depth guard, its increment in eval_apply/eval_expr, and TURI_EVAL_FRAME_BYTES can finally retire -- making interpreter recursion provably heap-bounded for every form, not just the folded/control paths.
---

# Heap-bound the C-scoped boundary forms (retire eval_depth) -- Plan

## Why this is split out

`turi-cek-stackless-reentry-plan.md` (archived) heap-bounded every recursion
that flows through a **driven position**: ordinary tail/non-tail calls, the full
delimited-control surface (`reset`/`shift`, `call/cc`, serial/cloneable
reset+resume, `resume-cont!`), and -- in the driver-coverage follow-up --
`try`/`catch`. Measured after that work, tail recursion, non-tail recursion,
`reset`/`shift`, and `try`/`catch`-wrapped recursion all run **1,000,000 deep**
with no `eval_depth` guard fire.

What remains are three **genuinely C-scoped boundary forms** whose own nesting
still C-recurses, one C frame / setjmp / ucontext per level:

- **`catch-unwind`** (`EX_CATCH_UNWIND`) -- a setjmp panic landing pad plus a
  `turi_call` of the body thunk. Panics propagate via `env->catch_jmp` +
  `longjmp`, the panic analog of the old `reset` setjmp boundary.
- **`atomically`** (`EX_ATOMICALLY` / `EX_STM`) -- runs the transaction body and
  re-runs it on `retry`; a transaction boundary + body evaluation per level.
- **`async` / `await` / `handle`** (`EX_ASYNC`, `EX_AWAIT`, `EX_HANDLE`,
  `EX_WITH_HANDLER`) -- fiber-based (`swapcontext`/`ucontext`); each suspension
  point is a C-stack/ucontext boundary.

These are C-stack-scoped **by design in both backends**: the *compiled* path also
C-recurses on them (e.g. `(rec (catch-unwind (fn [] (+ 1 (rec (- n 1))))))`
SIGSEGVs the compiled binary at ~200000). So the `eval_depth` guard is currently
the *right* tool for them -- it turns a would-be SIGSEGV into a graceful
`recursion limit exceeded`, matching the compiled crash boundary.

**Decision (2026-06-24):** keep the guard for now; break this remaining work out
here. Deleting `eval_depth` is only correct once all three forms are
heap-bounded -- otherwise deletion just trades the graceful error for a SIGSEGV
with no heap-bounding gain. Doing this work also makes the interpreter strictly
*exceed* the compiled backend's capabilities (it would succeed where compiled
crashes); that is a deliberate, documented semantic choice this plan must own.

## The shared mechanism (reuse SR's abort signal)

The reset/shift conversion (SR N4 Slice 1) is the template: replace a
setjmp/longjmp boundary with a **work-stack signal** that propagates through the
existing `returning || throwing || aborting` short-circuit guards and is consumed
by a driver `DK_*` frame. The same shape applies here:

### Phase C1 -- `catch-unwind` on a work-stack panic signal

- Add `env->panicking_unwind` (+ payload: msg/type/value/file/line, already on
  `env` as `catch_panic_*`) as a propagating signal, exactly parallel to
  `env->aborting`. A `panic` raises it instead of `longjmp(catch_jmp)` when a
  driver `DK_CATCH_UNWIND` boundary is in scope.
- Add `DK_CATCH_UNWIND`: a driver case for `EX_CATCH_UNWIND` applies the body
  thunk on the work-stack (have_apply / `DK_CALL_RET`) beneath the boundary
  frame; on return it consumes a matching panic signal (delivers the caught
  payload, restores saved handler/defer/module/no_unwind) or propagates.
- Extend the one `replace_all` site set: add `|| env->panicking_unwind` to the
  propagation guards (or fold it into a single `env->signaled()` helper to avoid
  a fourth flag proliferating through 94 sites).
- Keep `catch_jmp`/`longjmp` for the **non-driver** path (panic raised while no
  `DK_CATCH_UNWIND` is on the work-stack, e.g. inside a still-black-boxed form),
  exactly as SR kept synchronous fallbacks.
- Note the existing archived bug `catch-unwind-drops-captures-segv.md` when
  touching capture handling here.

### Phase C2 -- `atomically` / STM transaction on the work-stack

- Model the transaction boundary as a `DK_ATOMICALLY` frame: drive the body
  beneath it; on `retry` (an STM signal) re-drive the body in the same slot
  (a loop-native style re-request, like `DK_CONT_FOLD`), rather than recursing.
- Reconcile with `g_stm_tx` / the STM log lifetime across work-stack suspension.

### Phase C3 -- fibers (`async` / `await` / `handle`)

- Largest piece: the effect/async runtime is `ucontext`-based. Either (a) keep
  fibers but ensure a fiber's *own* recursion is heap-bounded (it already runs on
  the driver inside the fiber), and bound only the per-suspension cost, or
  (b) a larger CEK-style reification of the handler/async continuation. Scope
  this phase only after C1/C2 land and measure what actually still trips.

## Retiring the guard (Phase C4)

Once C1-C3 land and the audit probe set (extend `tests/turi/eval-tco`) shows no
form trips the guard at 1,000,000 deep:

- Remove the `eval_depth++` / `>= max_eval_depth` checks in `eval_apply` and
  `eval_expr` (`src/turi/eval.c`), the `max_eval_depth` field plumbing, and the
  `TURI_EVAL_FRAME_BYTES` byte-estimate.
- Keep a cheap **static AST-nesting** guard if desired (parse-time depth) as a
  pathological-input backstop -- it does not track runtime C depth.
- Update `TURI_DEFAULT_SANDBOX_DEPTH` / `turi_default_max_eval_depth` consumers.

## Validation

`bash tests/run.sh` (1783/0 baseline) + the `tur_eval_tco` audit suite, each with
the 10-minute timeout. Add a regression per phase mirroring the SR slices
(`cu-rec`, `atom-rec`, `fiber-rec` at 200000). `run-turi` baseline 23.

## Out of scope / open question

Whether the interpreter *should* heap-bound forms the compiled backend crashes on
is a real semantic-parity question (a program that nests `catch-unwind` 1M deep
runs under the interpreter but SIGSEGVs compiled). Resolve this before C4: either
accept the divergence (interpreter is a superset) or gate these forms' depth to a
compiled-comparable static limit so both backends fail the same way.
