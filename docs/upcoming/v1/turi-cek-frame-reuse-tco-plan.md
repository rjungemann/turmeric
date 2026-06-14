---
title: Turi full CEK frame-reuse TCO -- Plan
category: Planning
description: Finish the eval-trampoline's CEK refactor by handling tail calls *in the explicit-stack driver* via frame reuse, instead of routing them through the legacy eval_apply / eval_body_tco / TcoFrame machinery. Unifies the interpreter on a single evaluator, makes tail chains fully heap-bounded, and removes ~200 lines of duplicated tail-dispatch logic. Optional follow-up to the (completed) trampoline plan.
---

# Turi full CEK frame-reuse TCO -- Plan

## Status and scope

This is the **optional** follow-up to
[turi-eval-trampoline-plan.md](turi-eval-trampoline-plan.md), which is **complete
for its stated goal**: deep non-tail recursion is heap-bounded (folded onto the
driver work-stack), TCO is preserved, and `escape-deep-capture` runs under
`--interpret`.

That plan landed a **hybrid** (T3.2b): the explicit-stack driver `eval_drive`
folds **non-tail** turi-body calls (`DK_CALL_ARG` -> `DK_CALL_RET`, body in the
loop), but **tail** calls and **leaf** (native / inline-C) calls still go through
the legacy `eval_apply` -> `eval_apply_inner` (the `TcoFrame` trampoline loop) ->
`eval_body_tco` (the tail dispatcher). The hybrid is correct and O(1) for tail
recursion, but it leaves the interpreter with **two evaluators** and a chunk of
duplicated machinery.

This plan converts tail calls to **frame reuse inside the driver**, so *all*
function application flows through one evaluator. It is a sizeable, invasive
refactor with no new user-visible capability beyond what the hybrid already
delivers -- hence optional. The payoff is code unification, a cleaner foundation
for first-class delimited continuations, and fully driver-based tail chains.

**Explicit non-goal:** this does **not** remove native-HOF re-entry C recursion.
A native HOF is inherently a C frame, and `turi_call` re-enters evaluation
through it, so the `eval_depth` guard stays load-bearing for that path (see the
trampoline plan's T3.4). Full frame reuse does **not** enable retiring the guard.

## What the hybrid leaves on the table

1. **Two evaluators with duplicated logic.** `eval_drive` and `eval_body_tco`
   both contain tail-position handling for `EX_IF` / `EX_DO` / `EX_LET` /
   `EX_MATCH` / `EX_CALL`. The driver's was written in T2/T3; `eval_body_tco`
   (`src/turi/eval.c:3666`) is the older `goto restart` version. They must be
   kept in sync.
2. **The `TcoFrame` bounce machinery** -- `TcoFrame` (`:100`), `tco_bounce`
   (`:106`), and `eval_apply_inner`'s loop (`:3885`) -- exists solely to give
   tail calls O(1) C-stack. The driver could do the same with frame reuse.
3. **Tail chains whose bodies contain non-tail calls still C-nest.** A tail call
   `g` is run by `eval_apply`; if `g`'s body has a non-tail call `h`, `h` folds
   in the driver, but it is entered from *inside* `eval_apply`'s `eval_body_tco`
   -> `eval_expr` (one C frame per such tail-call-with-non-tail-body). Pure tail
   or pure non-tail recursion is already flat; only this alternation nests.

## Current state (grounded 2026-06-14, `src/turi/eval.c`)

- `eval_drive` (`:4197`): the driver. A `bool tail` loop variable (`:4092` field
  on `DriveCont`) is threaded through the tail-transparent forms so each
  `EX_CALL` is classified tail vs non-tail.
- `DK_CALL_ARG` completion (`:4494`): once args are accumulated,
  `if (top->tail || !foldable) cur = eval_apply(env, cl, acc, n);` -- **tail and
  leaf calls go to `eval_apply`**; otherwise the slot is reused as `DK_CALL_RET`
  (`:4545`) and the body is descended (`tail = true`).
- `DK_CALL_RET` epilogue (`:4557`): restores `in_no_unwind`, fires the call's
  defers (`fire_defers_to_mark_reversed`), consumes an early `return` at the
  boundary, restores the module.
- `eval_apply` (`:4044`) -> `eval_apply_inner` (`:3885`): the TCO loop.
  `eval_body_tco` (`:3666`) is the tail dispatcher it calls.
- `DK_LET_BODY` / `DK_MATCH_BODY` currently **always** push (freeing their frame
  on return) -- including in tail position. That is what makes the hybrid safe
  (the tail call inside runs via `eval_apply`, returns, and the cleanup cont
  fires normally). Frame reuse changes this (see the crux below).

## Design: tail-call frame reuse in the driver

The tail flag already tells us when a call is in tail position. Replace
"tail call -> `eval_apply`" with "tail call -> **reuse** the enclosing
`DK_CALL_RET`," mirroring exactly what `eval_apply_inner` does on a `TcoFrame`
bounce.

At `DK_CALL_ARG` completion for a **tail, foldable (turi-body)** closure:

1. The enclosing activation's `DK_CALL_RET` must be the top of the work-stack
   after this `DK_CALL_ARG` pops (see *the crux*).
2. Run the **current** activation's per-iteration cleanup -- the same steps
   `eval_apply_inner` runs *before* a bounce: `env->in_no_unwind =
   <current>.was_no_unwind` is **not** restored yet (the chain continues), but
   `fire_defers_to_mark_reversed(<current>.defer_mark)` **is** fired, then the
   defer mark is reset for the new iteration. (Match `eval_apply_inner`
   `:3999-4020` precisely -- defers fire per iteration; `was_returning` /
   `saved_module` are captured once for the whole chain.)
3. Re-enter the new callee: `env->current_module = new_cl->module`; build a fresh
   `call_frame`; bind the new args; set `<top>.defer_mark = env->defer_stack`;
   set `env->in_no_unwind` for the new fn; `env->returning = false`.
4. Descend `new_fn->body` with `tail = true`, **without pushing a new
   `DK_CALL_RET`** -- the existing slot is reused. `saved_module` and
   `was_returning` on the slot are left as captured at the original non-tail
   entry that started the chain.

A **tail leaf call** (native / inline-C) produces a value directly; that value is
the activation's result, so dispatch the leaf and let the existing `DK_CALL_RET`
epilogue run (do not reuse-as-callee).

### The crux: tail-transparent frame lifetime

For step 1 to hold, the tail-transparent forms must not leave a cleanup
continuation between a tail call and the `DK_CALL_RET`:

- **`DK_IF_BRANCH`** already pops before descending the chosen branch. ✓
- **`DK_DO_SEQ`** must **pop before descending the final (tail) item**, so a tail
  call in a `do`'s tail position sees `DK_CALL_RET` on top. (Trailing-defer `do`s
  keep the cont -- their last value is non-tail anyway.)
- **`DK_LET_BODY` / `DK_MATCH_BODY` in tail position must not be pushed at all.**
  Instead descend the body `tail = true` and **leak the let/match frame**, which
  is exactly what `eval_body_tco` does today (`frame = new_frame; goto restart`,
  no free). So the tail flag must gate the push: non-tail let/match push the
  cleanup cont (free on return); tail let/match descend directly (leak). The
  per-iteration frame leak for a let/match-tail loop is process-lifetime and
  matches current behavior.

This frame-leak-vs-free-by-tail-position split is the single subtle piece the
hybrid deliberately avoided; frame reuse requires it.

### Retiring the legacy machinery (the payoff)

Once tail calls run in the driver:

- `DK_CALL_ARG` completion becomes: tail turi-body -> reuse; non-tail turi-body
  -> fold (new `DK_CALL_RET`); leaf -> direct native/inline-C dispatch.
- Provide an `eval_apply_driven(env, cl, args, n)` for the C entry points that
  need to apply a closure to ready args (`turi_call`, thunks, the fiber thunk):
  do the prologue (build frame, bind args, set module/no_unwind/defer_mark) then
  run `eval_drive` on `fn->body` with `tail = true`, wrapped in the epilogue.
- **Retire** `eval_body_tco`, `TcoFrame`, `tco_bounce`, and `eval_apply_inner`'s
  loop -- the driver now is the single evaluator (~200 fewer lines, no
  dual-maintenance).
- `eval_apply` collapses to the thin module-save wrapper around
  `eval_apply_driven` (or is inlined).

## Implementation phases

1. **F1 -- tail-gate the let/match cleanup conts.** When descending a let/match
   body in tail position, descend directly (leak the frame) instead of pushing
   `DK_LET_BODY` / `DK_MATCH_BODY`. Non-tail keeps the cont. Behaviour-preserving
   (matches `eval_body_tco`'s tail leak); validate let/match correctness +
   defers + the harness. No frame-reuse yet.
2. **F2 -- pop `DK_DO_SEQ` before the tail item** (so a tail call in `do` tail
   position exposes the `DK_CALL_RET`). Behaviour-preserving.
3. **F3 -- frame-reuse tail calls.** Replace the `eval_apply` call for tail
   turi-body closures with the reuse path (steps 1-4 above). Keep `eval_apply`
   for leaf calls and `turi_call`. This is where `eval_body_tco`/`TcoFrame` stop
   being on the tail-recursion hot path. Gate hard on TCO probes.
4. **F4 -- unify the C entry points.** Add `eval_apply_driven`; route
   `turi_call`, the thunk evals, and the fiber thunk through it; retire
   `eval_body_tco`, `TcoFrame`, `tco_bounce`, and `eval_apply_inner`'s loop.
5. **F5 -- cleanup + re-measure the guard.** With the synchronous path fully
   driver-based, re-measure `TURI_EVAL_FRAME_BYTES` against the residual
   HOF-re-entry path (still the binding one) and tidy the dead code. The guard
   stays (HOF re-entry), but the rationale comment should reflect the unified
   evaluator.

Each phase is independently landable and regression-green; the hybrid is a safe
resting point if F3/F4 stall.

## Risks and trade-offs

- **Defer order on tail calls.** F3 must fire each activation's defers before
  reuse, exactly as `eval_apply_inner` does (`:3999-4020`). Do **not** "fix" the
  pre-existing single-scope function-exit FIFO-vs-LIFO divergence here
  ([docs/reported/turi-tail-scope-defers-fire-fifo-not-lifo.md](../../reported/turi-tail-scope-defers-fire-fifo-not-lifo.md));
  reproduce current behaviour and track that separately.
- **Frame-leak-vs-free by tail position (F1).** The subtle correctness piece:
  freeing a let/match frame that a tail chain still uses is a use-after-free;
  failing to free a non-tail let/match frame is a (benign, process-lifetime)
  leak. Get the tail gate exactly right.
- **`turi_call` re-entry.** `eval_apply_driven` spins a fresh driver per call;
  native-HOF nesting therefore still C-recurses (bounded by the `eval_depth`
  guard, unchanged from the hybrid).
- **Scope.** Largest single change to the hottest function after T3.2b. Mitigate
  with the F1-F5 slicing behind the harness; revert to the hybrid if needed.

## Validation

- **TCO stays O(1)** (no OOM / no C-stack growth) at 1e6 iterations for every
  tail shape: `sum-acc` (if-tail), `loop-let` (let-tail), `loop-do` (do-tail),
  and a `match`-tail loop.
- **Non-tail stays heap-bounded:** `sum-to 500000`, `escape-deep-capture` pass
  under `--interpret`.
- **Defers / early-return / throw** through folded *and* tail calls match
  current output (and the compiled path where they already agree).
- **Modules:** a multi-module tail chain restores the caller's module exactly.
- `bash tests/run-turi.sh` green every phase; `tools/check_turi_parity.py`
  0-gaps; `ctest -R "eval|sandbox"` green.
- **After F4:** `grep` confirms `eval_body_tco` / `TcoFrame` / `tco_bounce` are
  gone and no behaviour shifted.

## See also

- [turi-eval-trampoline-plan.md](turi-eval-trampoline-plan.md) -- parent
  (T1-T3.4, completed); this finishes its CEK direction.
- [turi-interpreter-delimited-control-plan.md](turi-interpreter-delimited-control-plan.md)
  -- the unified driver work-stack is the substrate for reifying a continuation
  (the slice between a `reset` and a `shift`); frame reuse is a prerequisite for
  doing that cleanly in the interpreter.
- `src/turi/eval.c`: `eval_drive` (`:4197`), `DK_CALL_ARG`/`DK_CALL_RET`
  (`:4494`/`:4557`), `eval_apply_inner` (`:3885`), `eval_body_tco` (`:3666`),
  `TcoFrame`/`tco_bounce` (`:100`/`:106`).
