---
title: Graduate cps-async -- retire the gate now that F4 is declined
category: Planning
status: open -- graduation DEFERRED (2026-07-19). Part 1 (framing corrections +
  F4 archival) landed. Part 2 (graduate: delete the flag, heap async/await goes
  always-on) was attempted and reverted: the superset confirmation check came
  back red -- forcing async heap-only regressed `taskgroup-async` (fiber-
  scheduler-dependent await -> empty output) and `async-effect-spawn` (await in an
  effect handler -> build-time internal error). Per this plan's Contingency
  section the flag STAYS gated (criterion repointed off F4 onto the new fiber-
  interop gap; expires_at bumped 0.30.0 -> 0.31.0; gap tracked in
  docs/reported/cps-async-heap-fiber-interop-gap.md). See the 2026-07-19 progress
  note below. Plan of record for eventual graduation is unchanged.
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

## Progress (2026-07-19) -- graduation DEFERRED; superset check came back red

Part 1 (framing corrections) landed as written: the stale F4-based graduation
comment is gone, the recursive-await eviction is documented as by-design in
`emit_cps_ir.c`, the report citation now points at `docs/archive/`, and the F4
plan (`compiled-stackless-recursive-await-plan.md`) is archived as the decision
record.

Part 2 (graduate: delete the flag, make the heap lowering unconditional) was
**attempted and reverted** because the pre-landing confirmation check -- "the
heap representation is a functional strict superset of the fiber path for async"
-- came back **red**. Forcing async heap-only as the sole lowering regressed two
in-tree fixtures that pass today on the fiber path:

- **`taskgroup-async`** (wrong result): the program manually spawns ucontext
  fibers via inline-C (`tur_scheduler_spawn`) and relies on `(await fut)` to
  *drive the fiber scheduler* until each spawned fiber completes. On the heap
  `dk_shift` path, `await` parks its continuation on the reactor and never drives
  the ucontext scheduler, so the spawned fibers never run and the futures never
  fulfill. Output is empty instead of `10 / 20 / 30 / done`.
- **`async-effect-spawn`** (build-time internal error): an `await` sits inside an
  effect handler's `resume` body. Graduation colors the `await`, which forces the
  whole handler-bearing function to evict to the direct emitter when the combined
  shape is inadmissible on the heap path -- and the direct emitter can no longer
  emit effects (`fiber effect runtime deleted`, from cps-tramp-resume's
  graduation). The result is `internal error: effect form (EX kind 57) reached
  the direct/fiber emitter`.

Root cause (both): graduation removes the U5 fiber-delegation path for `await`
(`is_delegatable` -> false, `EX_AWAIT` always lowers to `build_await`). That
delegation is exactly what let a fiber-scheduler-dependent await, or an await
nested in a colored effect body, keep working on the proven fiber runtime. The
heap `dk_shift` await is therefore **not** a strict superset for these shapes:
it neither drives the ucontext scheduler nor composes with a colored effect body.

Per this plan's own "Contingency only" section, graduation does **not** proceed
this cycle:

- The `cps-async` row stays in `EXPERIMENTS[]` (flag still gates the feature).
- Its comment is repointed off the (now-settled) F4 criterion onto the concrete
  fiber-interop gap above, and `plan_path` points back at this plan.
- `expires_at` is bumped `0.30.0 -> 0.31.0` so the 0.30.0 release cut is not
  blocked by an unmet contract.
- The gap is tracked in
  `docs/reported/cps-async-heap-fiber-interop-gap.md`.

### To graduate later

Close the gap, then re-run Part 2 as written:

1. Make the heap `await` path drive the ucontext scheduler when a future is
   pending and runnable fibers exist (so `taskgroup-async`-style manual-spawn +
   await keeps working), **or** retire the fiber-scheduler-dependent await
   pattern from the fixtures and stdlib and confirm no supported surface needs it.
2. Make `async`/`await` compose inside a colored effect body without evicting to
   the (effect-incapable) direct emitter, so `async-effect-spawn` colors cleanly.
3. Re-confirm the superset check (all async fixtures green with the flag forced
   on as the only path), then delete the flag and move `cps-async` to
   `GRADUATED[]`.

The rest of this document is the original graduation plan, retained as the
plan of record for that eventual graduation.

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
