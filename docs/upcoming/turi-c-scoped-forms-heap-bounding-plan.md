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

> **Status (landed 2026-07-06).** Implemented in `src/turi/eval.c`:
> `DK_CATCH_UNWIND` (a driver descend + consume case modeled on `DK_RESET`),
> `panicking` promoted to a propagating signal folded into a single
> `env_signaled()` helper (replacing the raw `returning || throwing || aborting`
> disjunction at ~94 sites), a `g_catch_stack` of `TuriCatchBoundary`s that lets
> a `panic` pick the innermost boundary and unwind via the signal (driver) or
> `longjmp` (setjmp fallback, kept for `catch-panic-of` and non-driver callers),
> and defer-during-unwind firing that clears `panicking` per defer body while a
> new `g_firing_panic_defer` flag preserves double-panic detection. Regressions
> `cu-rec` / `cu-catch-deep` at 200000 added to `tests/turi/eval-tco.{tur,sh}`.
> Note found in flight: the interpreter's *pre-C1* nested `catch-unwind` was
> already effectively heap-bounded (the SR driver folds the recursion; it reaches
> 5M deep on a 2MB stack without tripping the guard), so C1 is a work-stack
> *modeling* change (moving `catch-unwind` off the `eval_apply`/`eval_depth`
> re-entry path -- a C4 prerequisite) rather than a crash fix. The **compiled**
> backend still SIGSEGVs on the same shape at ~200000 (verified), which is the
> documented interpreter-superset divergence.

- Promote the existing `env->panicking` bool (`src/turi/eval.c:7462,7488`, set
  on panic start and cleared when the setjmp landing pad catches it) into a
  first-class *propagating* signal, parallel to `env->aborting` -- i.e. reuse
  the name, don't add a second `panicking_unwind` field alongside it. Payload
  is already on `env` as `catch_panic_msg`/`catch_panic_type`/`catch_panic_value`
  /`catch_panic_file`/`catch_panic_line`. A `panic` raises the signal (and
  fills the payload) instead of `longjmp(catch_jmp)` when a driver
  `DK_CATCH_UNWIND` boundary is in scope.
- Add `DK_CATCH_UNWIND`: modeled on `DK_RESET` (the closest SR template -- see
  the existing kinds `DK_RESET`/`DK_ESCAPE`/`DK_CONT_FOLD` around
  `src/turi/eval.c:4370-4415`). A driver case for `EX_CATCH_UNWIND` applies the
  body thunk on the work-stack (have_apply / `DK_CALL_RET`) beneath the
  boundary frame; on return it consumes a matching panic signal (delivers the
  caught payload, restores saved handler/defer/module/no_unwind) or
  propagates.
- Extend the one `replace_all` site set: add `|| env->panicking` to the
  propagation guards (94 sites in `src/turi/eval.c` today match
  `env->returning ... env->throwing ... env->aborting`). Strongly prefer
  folding into a single `env->signaled()` helper -- a fourth raw flag through
  94 sites is exactly the churn SR paid down.
- Keep `catch_jmp`/`longjmp` for the **non-driver** path (panic raised while no
  `DK_CATCH_UNWIND` is on the work-stack, e.g. inside a still-black-boxed form),
  exactly as SR kept synchronous fallbacks.
- Note the existing archived bug `catch-unwind-drops-captures-segv.md` when
  touching capture handling here.

### Phase C2 -- `atomically` / STM transaction on the work-stack

> **Status (landed 2026-07-06).** Implemented in `src/turi/eval.c`: a
> `DK_ATOMICALLY` driver descend + consume case for `EX_ATOMICALLY` (heap
> `TuriStmTx` linked on `g_stm_tx`, committed / retry-errored on the consume
> side, mirroring `eval_atomically`), and a `DK_STM_SEQ` driver case for
> `EX_STM` that drives the body items on the work-stack with the retry/abort
> short-circuit (so recursion inside an stm item folds onto the heap). The
> synchronous `eval_atomically` / `eval_expr_impl` `EX_STM` loop is kept for
> non-driver callers (e.g. an `or-else` arm). Unlike C1, nested `atomically`
> *did* C-recurse pre-C2 and tripped the guard ("recursion limit exceeded") at
> 200000; it now folds. Retry semantics are unchanged: the single-threaded
> interpreter still errors on a requested retry (no way to make progress), so
> C2 does **not** re-drive the body -- the plan's "re-drive on retry" is a no-op
> here because a serial re-run can never make progress (see the TI4 note at
> `eval.c` `TuriStmTx`). Regression `at-rec` at 200000 added to
> `tests/turi/eval-tco.{tur,sh}`.

- Model the transaction boundary as a `DK_ATOMICALLY` frame: drive the body
  beneath it; on `retry` (an STM signal) re-drive the body in the same slot
  (a loop-native style re-request, like the existing `DK_CONT_FOLD` at
  `src/turi/eval.c:4370-4415`), rather than recursing. The `EX_STM` /
  `EX_ATOMICALLY` cases (`src/turi/eval.c:7980,7993`) currently loop in C
  through `eval_atomically()`; they are the black boxes this phase replaces.
- Reconcile with `g_stm_tx` / the STM log lifetime across work-stack suspension.

### Phase C3 -- fibers (`async` / `await` / `handle`)

- Largest piece: the effect/async runtime is `ucontext`-based. Either (a) keep
  fibers but ensure a fiber's *own* recursion is heap-bounded (it already runs on
  the driver inside the fiber), and bound only the per-suspension cost, or
  (b) a larger CEK-style reification of the handler/async continuation. Scope
  this phase only after C1/C2 land and measure what actually still trips.

## Retiring the guard (Phase C4)

Once C1-C3 land and the audit probe set (extend `tests/turi/eval-tco.tur`,
driven by `tests/turi/eval-tco.sh`) shows no form trips the guard at
1,000,000 deep:

- Remove the `eval_depth++` / `>= max_eval_depth` checks in `eval_apply` and
  `eval_expr` (`src/turi/eval.c`), the `max_eval_depth` field plumbing, and the
  `TURI_EVAL_FRAME_BYTES` byte-estimate.
- Keep a cheap **static AST-nesting** guard if desired (parse-time depth) as a
  pathological-input backstop -- it does not track runtime C depth.
- Update `TURI_DEFAULT_SANDBOX_DEPTH` / `turi_default_max_eval_depth` consumers.

## Validation

`bash tests/run.sh` (regenerate the pass/fail baseline on the day the phase
lands -- the fixture set moves; CLAUDE.md rounds to ~1442, the on-disk count
is ~1580, both are older than this line will be) + the `tur_eval_tco` audit
target, each with the 10-minute timeout. Add a regression per phase mirroring
the SR slices (`cu-rec`, `atom-rec`, `fiber-rec` at 200000).

## Out of scope / semantic-parity stance

The interpreter will heap-bound these forms even though the compiled backend
still C-recurses on them (a program that nests `catch-unwind` 1M deep runs
under the interpreter and SIGSEGVs compiled). **We accept this divergence as
temporary, not as a language decision.** The interpreter runs on a work-stack
driver; the compiled backend runs directly on the C stack; the two paths were
always going to converge on heap-bounding at different times, and gating the
interpreter down to match a limit the compiler will eventually lift is
back-pressure in the wrong direction.

The compiled path has a coherent design that closes the gap -- a heap-allocated
handler chain replacing `setjmp`/`longjmp` for `catch-unwind`, a heap-anchored
tx-log for `atomically`, and a CPS/first-class-continuation direction for
`async`/`handle`. That work is scoped in the sibling
[compiled-c-crossing-tco-plan.md](./compiled-c-crossing-tco-plan.md); it is
ABI-level and not on the near-term roadmap, but it exists and it is the reason
the divergence is a temporal gap, not a permanent semantic split.

Concretely for this plan: land C1-C4 without gating on compiled parity, and
document the interpreter-superset stance in the user-facing docs so a program
that relies on 1M-deep `catch-unwind` under the interpreter cannot be surprised
when the compiler crashes on the same input.
