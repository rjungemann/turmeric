---
title: Graduate cps-async -- retire the gate now that F4 is declined
category: Planning
status: DONE -- cps-async GRADUATED (2026-07-19). Part 1 (framing corrections +
  F4 archival) and Part 2 (graduate: flag deleted, heap async/await unconditional)
  both landed. The superset check first came back red on two fiber-interop shapes
  (`taskgroup-async`, `async-effect-spawn`); both gaps were closed (heap `await`
  now drives the scheduler run-queue; a handler-case `await` delegates to the
  fiber path via CpsB.in_handler_case) so the heap path is a genuine strict
  superset, then the flag was removed and `cps-async` moved to GRADUATED[]. Full
  suite green. See the 2026-07-19 progress note below.
description: F3 shipped async/await on the DK heap-continuation representation
  behind `--enable=cps-async`. Its only remaining graduation blocker was F4
  (stackless recursive await), which has been re-investigated and DECLINED --
  recursive await evicting to the direct emitter is the correct, shipping default.
  With the last admissibility gap closed and the one open item settled as by-design,
  cps-async is done: this plan GRADUATES it (removes the flag, makes the heap
  lowering unconditional, adds `cps-async` to GRADUATED[]), fixes the by-design
  comments, and archives the F4 plan as the decision record.
---

# Graduate cps-async

## Progress (2026-07-19) -- gaps fixed; cps-async GRADUATED

Part 1 (framing corrections) landed as written, and Part 2 (graduate) landed
after first closing the two gaps the initial superset check surfaced.

**Initial superset check (red), then fixed.** Forcing async heap-only regressed
two in-tree fixtures that passed on the fiber path. The root cause of both was
that graduation removes the U5 fiber-delegation path for `await`. Rather than
keep the flag, both gaps were closed so the heap path is a genuine strict
superset:

- **`taskgroup-async`** (was: empty output). The program manually spawns ucontext
  fibers via inline-C (`tur_scheduler_spawn`) and relies on `(await fut)` to
  *drive the fiber scheduler* until each spawned fiber completes. Fix: the heap
  await runtime `__tur_await_body` now, on a pending future, drains the scheduler
  run-queue (`while (!done && run_queue_len > 0) tur_scheduler_run_one(...)`) and
  resumes the captured continuation inline once the future resolves -- mirroring
  `tur_await_future`'s non-fiber branch. Bounded by `run_queue_len`, so a future
  that can only complete asynchronously still falls through to the deferred park
  (the F3.2 reactor path). See `src/compiler/emit_module.c` (`__tur_await_body`).
- **`async-effect-spawn`** (was: build-time internal error). An `await` sits in a
  handler-case `resume` body; graduation built it as a `CT_AWAIT` that
  `handle_case_ok` cannot host, so the whole colored function evicted to the
  (effect-incapable) direct emitter. Fix: an `await` lexically inside a
  handler-case body now delegates to the fiber path (`build_letraw` -> `CT_LETRAW`,
  which `handle_case_ok` admits) instead of heap-lowering. This is tracked by a
  new `CpsB.in_handler_case` depth counter, bumped around the case-body build in
  `build_handle_core` and consulted at the two `EX_AWAIT` lowering sites in
  `cps_ir.c`. The effect handler still DK-lowers; only the self-contained async
  region delegates -- exactly the pre-graduation shape, which was known-good.

With both closed, the heap path is a functional strict superset of the fiber
path (the fiber runtime stays as the async scheduler and as the delegation target
for handler-case awaits -- both explicitly in scope to remain). Graduation then
landed as written below:

- The `cps-async` row is deleted from `EXPERIMENTS[]`; `"cps-async"` is added to
  `GRADUATED[]` (a lingering `--enable=cps-async` is a TUR-W0063 no-op).
- `g_opt_cps_async` is removed; every read site is unconditional.
- The F4 plan stays archived; the fiber-interop gap report is resolved and
  archived (`docs/archive/cps-async-heap-fiber-interop-gap.md`).

Verification: full suite green (12-minute timeout); F3 fixtures
(`async-await-cps` / `-pending` / `-two` / `-repark`) and the two formerly-red
fixtures all pass on the graduated (flag-gone) default path; `--enable=cps-async`
compiles as a TUR-W0063 no-op.

The rest of this document is the original graduation plan.

## Why this document exists

Archiving the completed v1/v2 CPS plans (2026-07-19) surfaced a live contradiction:

- **F4 is declined.** `compiled-stackless-recursive-await-plan.md` was re-scoped to
  `status: superseded / declined`: a recursive `await` evicting to the direct
  emitter is **works-as-intended**, not a bug. Because `(async fn)` is synchronous
  on the compiled path, a recursive await is always a ready future -- the exact
  case the heap `dk_shift` path handles worst (O(N) C stack) and the direct
  emitter's inline readiness check + `goto __tur_tailcall` loop handles in O(1).
  The root-cause report was archived 2026-07-18 marked RESOLVED / BY-DESIGN.

- **But `cps-async` is still gated on F4.** In `src/runtime/experiments.c` the
  `cps-async` descriptor (still `XF_LIFECYCLE_PROTOTYPE`, `introduced 0.28.2`,
  `expires_at 0.30.0`) carries a comment that says the flag "**Stays gated on the
  ONE remaining residual** -- a recursive await must run stackless ON the heap path
  rather than evict -- tracked by the successor plan below. **Graduate when it
  lands.**" The successor plan is F4. That residual will not land, so the stated
  graduation criterion is unreachable.

Net: cps-async has a stale, unsatisfiable graduation bar and a hard expiry one
minor release away (tree is at `0.29.0`; expiry is `0.30.0`). At the 0.30.0 cut the
release-cut skill will refuse to bump until cps-async is graduated. And there is
nothing left to wait for -- the feature is admissibility-complete and its one open
item is settled as by-design. The resolution is not "decide what to do with the
flag"; it is "the flag has done its job -- graduate it." This plan does that.

## The plan: clean up the framing, then graduate

Two parts, landing in one change. Part 1 corrects the stale comments and citations;
Part 2 graduates the experiment. Both point the same direction -- cps-async is done.

### Part 1 -- correct the by-design framing

These changes stand on their own and precede the graduation edits in the same commit.

1. **Rewrite the stale graduation comment.** In `experiments.c` (the ~10-line
   comment block above the `cps-async` row, currently lines ~98-108) replace
   "Stays gated on the ONE remaining residual ... Graduate when it lands" and the
   "successor plan below" reference with the settled reality: F3 closed every
   admissibility gap; recursive await evicting to the direct emitter is the
   shipping, by-design behavior; graduation is **not** blocked on any F4 work.

2. **Make the recursive-await rejection permanent-by-design in the code comments.**
   The `CT_AWAIT` admission guard rejects a cps->cps tail-call (recursive) await
   continuation on purpose:
   - `src/compiler/emit_cps_ir.c:~2138-2147` (`await_cont_reset_ok` rejection +
     the `docs/reported/cps-async-recursive-await-eviction.md` citation).
   - `src/compiler/emit_cps_ir.c:1736` (`await_cont_reset_ok` itself).
   Update the comments from "not admitted (see reported bug)" to "not admitted
   **by design** (see the archived decision)". Do **not** change the behavior --
   the rejection stays; only the framing (open-gap -> settled-decision) changes.

3. **Repoint the stale report citation.** `emit_cps_ir.c:~2145` cites
   `docs/reported/cps-async-recursive-await-eviction.md`, which was archived
   2026-07-18. Repoint it to its `docs/archive/` (or
   `docs/archive/history/`) location. Verify the archived path first.

4. **Archive the F4 plan as the decision record.** Move
   `docs/upcoming/compiled-stackless-recursive-await-plan.md` to `docs/archive/`.
   It is kept for the record of *why* recursive await is not colored onto the heap
   path. This can only happen once Part 2 removes the live `plan_path` pointer at
   it (see below) -- so archive F4 in the same change that resolves Part 2, never
   before (a live experiment `plan_path` must resolve to an existing file).

### Part 2 -- graduate cps-async (the plan of record)

**cps-async graduates.** This is not a menu -- it is the decision. F3 landed the
feature and closed every *admissibility* gap. The one item ever left open was a
*performance* residual (F4), and F4 is now settled as by-design: recursive await
evicting to the direct emitter is the *correct* default, not a defect. A feature
that is admissibility-complete with no open defect and a flag whose only graduation
criterion is "a thing we deliberately chose not to build" does not stay gated -- it
graduates. Keeping the flag past this point is pure carrying cost, and the 0.30.0
expiry would force the issue anyway. Graduate it on its own merits, now.

Graduating makes the heap async/await lowering the unconditional CPS-path lowering
and deletes the flag. The recursive-await eviction stays exactly as it ships today
(the `await_cont_reset_ok` rejection is now permanent, not gated).

The one thing to *confirm* (not decide) before landing: the heap representation is a
functional **strict superset** of the fiber path for async -- correct results on
every shape, given that recursive await intentionally evicts. F3's own gap-closure
is the evidence; the concrete check is that the F3 fixtures pass with the flag
forced on as the *only* path (see Verification). This is a green-light confirmation,
not a fork in the road: the expectation is that it holds, and the fallback below
exists only for the contingency that it does not.

Work:
- Delete the `cps-async` row from `EXPERIMENTS[]`; add `"cps-async"` to
  `GRADUATED[]` (a lingering `--enable=cps-async` becomes a TUR-W0063 accept-and-warn
  no-op, mirroring `cps-tramp-resume`).
- Remove `g_opt_cps_async` (`src/runtime/globals.{h,c}`) and make every read of it
  unconditional (the flag is now always-on). Known read sites to flip and simplify:
  - `src/passes/cps.c:118`, `:327` (coloring: `await` is a control op).
  - `src/passes/cps_ir.c:1506` (returns `!g_opt_cps_async`), `:3525`, `:3837`
    (tail `await` lowers to `build_await`).
  - `src/compiler/emit_cps_ir.c:7160` (`experimental_surface = g_opt_cps_async ||
    g_opt_cps_tramp_resume` -- simplify now that the first disjunct is always true).
  - Sweep `grep -rn g_opt_cps_async src/` for the complete set before editing; the
    list above is from the 2026-07-19 audit, not a guarantee of totality.
- The `plan_path` pointer disappears with the deleted row, unblocking Part 1.4
  (archive F4). Update any remaining `docs/upcoming/compiled-stackless-recursive-await-plan.md`
  source citations to the `docs/archive/` path.

### Contingency only -- if the superset check fails

Included for completeness, **not** an expected outcome. The only thing that would
stop graduation is the confirmation check above coming back red -- i.e. some real
async shape produces a *wrong result* (not merely evicts) on the heap path as the
sole lowering. If and only if that happens:

- Do **not** graduate this cycle. Keep the row, but replace the stale F4-based
  criterion with the concrete shape that failed, bump `expires_at` past `0.30.0`
  with a one-line rationale, and repoint `plan_path` off F4 onto a new tracking
  plan for that specific gap. Then fix the gap and come back to graduate.
- This is a bug-found-late fallback, not a strategic alternative. There is no
  "keep async on fibers forever" branch in this plan: the effect endgame already
  made CPS/DK the sole effect lowering (`cps-tramp-resume` graduated), and async is
  the last holdout -- graduation moves it the same direction.

## Verification (graduation)

- `bash tests/run.sh` green (12-minute timeout). Specifically the F3 fixtures must
  pass with the flag GONE / forced-on as the only path: `async-await-cps` (F3.1),
  `async-await-cps-pending` / `-two` / `-repark` (F3.2).
- Default codegen for non-async programs byte-identical (the graduation only affects
  colored async bodies; nothing else should move).
- Regenerate fixture snapshots in the same change and reconcile any codegen drift.
- Confirm `--enable=cps-async` still compiles as a TUR-W0063 no-op (GRADUATED[]).

## Out of scope

- Retiring the `ucontext` fiber runtime -- it stays the async scheduler; this plan
  only settles the *experiment gate*, not the runtime's disposition. (Whether the
  heap path should eventually replace fibers for async is a separate, larger arc,
  aligned with the effect endgame's CPS/DK direction.)
- Re-opening F4. Recursive await evicting to the direct emitter is the accepted
  design; nothing here reverses that.
- The parked-`__root` leak (F3.2/gap-2) -- tracked separately, non-blocking.

## Touch-point index (from the 2026-07-19 audit)

- `src/runtime/experiments.c` -- the `cps-async` descriptor row + its comment block;
  `GRADUATED[]`.
- `src/runtime/globals.{h,c}` -- `g_opt_cps_async` declaration/definition.
- `src/passes/cps.c` -- coloring reads (`:118`, `:327`).
- `src/passes/cps_ir.c` -- lowering reads (`:1506`, `:3525`, `:3837`).
- `src/compiler/emit_cps_ir.c` -- `experimental_surface` (`:7160`); the by-design
  `await_cont_reset_ok` rejection + report citation (`:1736`, `:~2138-2147`).
- `docs/upcoming/compiled-stackless-recursive-await-plan.md` -- F4, to be archived.
