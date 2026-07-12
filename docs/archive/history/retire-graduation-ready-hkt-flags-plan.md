---
title: Retire the four graduation-ready HKT/forall experiment flags -- Plan
category: Planning
description: Four of the five remaining `--enable=<name>` experiment flags (forall-kinds, forall-constraints, hkt-hrt, hrt-curried-result) have no visible feature deficit -- their gated code paths pass fixtures, their plan-doc TODO sections are drained, and they are only held back by inertia. This plan graduates them to always-on in a single change, deletes the `g_opt_*` bits and the disable-side branches, and archives the plan docs. The fifth flag (forall-dict-pass) had a codegen deficit (now fixed, archived at docs/archive/history/forall-dict-pass-codegen-and-scope.md) plus a remaining scope deficit (multi-constraint / HKT-receiver dicts) tracked in docs/upcoming/v1/forall-dict-pass-multi-constraint-hkt-plan.md; it stays experimental behind a bumped expires_at.
---

# Retire the four graduation-ready HKT/forall experiment flags -- Plan

> **Follow-up (2026-07-06):** the fifth flag, `forall-dict-pass`, has **also
> graduated** to always-on. Its remaining scope deficit (multi-constraint +
> HKT-receiver dicts) landed via
> [../archive/forall-dict-pass-multi-constraint-hkt-plan.md](forall-dict-pass-multi-constraint-hkt-plan.md).
> The `EXPERIMENTS[]` registry is now empty; the statements below that
> `forall-dict-pass` "stays experimental" describe the state at the time of
> *this* plan only.

## Why this exists

`src/runtime/experiments.c` carries five active `--enable=<name>` rows, all
introduced in 0.25.6 and all with `expires_at "0.27.0"`. The current version
*is* 0.27.0, so the next minor bump lands them past the contract date. An
audit of the flags found:

| Flag | Feature deficit? |
| --- | --- |
| `forall-kinds` | None. Parse gate for `(f :: * -> *)` kind annotations; fixtures pass. |
| `forall-constraints` | None. Mode-A static enforcement of `(forall [a] [(C a)] ...)`; fixtures pass. |
| `hkt-hrt` | None. Rank-2 forall over a higher-kinded var; skolemiser + unifier already handle `KIND_ARROW{n}`. |
| `hrt-curried-result` | None. `(forall [a] (-> a (-> a a)))` -- result-type instantiation + boxed-result closure dispatch already work. |
| `forall-dict-pass` | **Yes** -- dict-clone codegen return-type threading is now fixed ([../archive/forall-dict-pass-codegen-and-scope.md](../archive/forall-dict-pass-codegen-and-scope.md)), but the elaborator still refuses multi-constraint / HKT-receiver dicts. That remaining scope work is planned in [v1/forall-dict-pass-multi-constraint-hkt-plan.md](v1/forall-dict-pass-multi-constraint-hkt-plan.md). |

This plan retires the top four in one change. `forall-dict-pass` stays
`--enable`-gated with a bumped `expires_at` (0.28.0) and a pointer at the
multi-constraint/HKT plan.

## Non-goals

- **No new features.** Graduation makes the four gated code paths
  unconditional; it does not extend them.
- **No fix for `forall-dict-pass`.** Its Deficit 1 (codegen) is already fixed;
  the remaining Deficit 2 (multi-constraint / HKT-receiver dicts) has its own
  plan at [v1/forall-dict-pass-multi-constraint-hkt-plan.md](v1/forall-dict-pass-multi-constraint-hkt-plan.md).
- **No changes to the descriptor format.** `ExperimentDescriptor` and the
  `EXPERIMENTS[]` conventions stay as they are.

## What "retire" concretely means for one flag

Using `forall-kinds` as the worked example -- the other three follow the same
recipe:

1. **Delete the row** in `src/runtime/experiments.c` and replace it with a
   one-line breadcrumb comment matching the existing graduated entries
   (`src/runtime/experiments.c:33-41`):

   ```c
   /* forall-kinds GRADUATED 2026-07-XX -- explicit kind annotations on
    * forall/exists bound vars (e.g. (f :: * -> *)) are always accepted;
    * the gate at elab_types.c:1115 is removed.  See
    * docs/archive/history/constrained-hkt-forall-plan.md. */
   ```

2. **Delete the global** in `src/runtime/globals.{c,h}` (`g_opt_forall_kinds`)
   and every extern reference.

3. **Delete the disable-side branch** at the gate site. For `forall-kinds`:
   `src/compiler/elab_types.c:1115-1125` currently reads
   `if (!g_opt_forall_kinds) { <error>; } else { <parse kinds>; }`. The
   graduated form drops the guard and keeps only the parse-kinds path.

4. **Move the plan doc** from `docs/upcoming/v1/` to `docs/archive/`. Do not
   leave it in `upcoming/` after the code has moved -- an archived plan is
   the paper trail for a shipped feature, not an in-flight one.

5. **Regenerate fixture snapshots** if the graduation happens to change
   diagnostic surfaces (it should not for any of these four -- the gates are
   pre-elaboration parse rejections, not codegen changes). If a snapshot
   moves, regen in the same commit per CLAUDE.md's fixture-churn policy.

## Per-flag work

Each row below lists the descriptor row to delete, the global to delete, and
the gate site whose disable branch collapses.

- **`forall-kinds`**
  - Row: `src/runtime/experiments.c:42-48`
  - Global: `g_opt_forall_kinds` in `globals.c:179`, `globals.h:154`
  - Gate: `src/compiler/elab_types.c:1115` -- drop the `if (!g_opt_forall_kinds)`
    error branch; the parse-kinds path becomes unconditional.
  - Plan: `docs/upcoming/v1/constrained-hkt-forall-plan.md` -> archive.
    (Shared with `forall-constraints` and `hkt-hrt` -- move the doc once,
    on the last graduation.)

- **`forall-constraints`**
  - Row: `src/runtime/experiments.c:49-55`
  - Global: `g_opt_forall_constraints` in `globals.c:184`, `globals.h:162`
  - Gate: `src/compiler/elab_types.c:1041` -- drop the guard; constraint
    vectors on `forall` become always-parsed and always-enforced at
    instantiation sites.
  - Plan: shared archive with `forall-kinds`.

- **`hkt-hrt`**
  - Row: `src/runtime/experiments.c:56-62`
  - Global: `g_opt_hkt_hrt` in `globals.c:189`, `globals.h:170`
  - Gate: `src/compiler/elab_call.c:41` -- drop the guard; rank-2 forall
    over `f :: * -> *` becomes always-on at call sites.
  - Plan: shared archive with `forall-kinds`.

- **`hrt-curried-result`**
  - Row: `src/runtime/experiments.c:70-76`
  - Global: `g_opt_hrt_curried_result` in `globals.c:198`, `globals.h:187`
  - Gates: `src/compiler/elab_call.c:1039` (marks curried result boxed for
    fat dispatch) and `elab_call.c:6144` (instantiates the inner `TY_FN`
    result). Drop both guards; both paths become unconditional.
  - Plan: the mode-B plan is archived at
    `docs/archive/history/constrained-hkt-forall-mode-b-plan.md`; the live tracking for
    the surviving `forall-dict-pass` row is
    `docs/upcoming/v1/forall-dict-pass-multi-constraint-hkt-plan.md`.

## What stays

- **`forall-dict-pass`** stays in `EXPERIMENTS[]`. Its `expires_at` was bumped
  from `"0.27.0"` to `"0.28.0"` (Deficit 1 fixed; Deficit 2 keeps it
  experimental) and its `plan_path` now points at
  `docs/upcoming/v1/forall-dict-pass-multi-constraint-hkt-plan.md`, with a
  descriptor comment cross-linking the archived report so the fixed deficit is
  discoverable.
- **The mode-B plan** (`constrained-hkt-forall-mode-b-plan.md`) is archived; the
  `forall-dict-pass` descriptor now references the Deficit-2 plan instead.
- **All fixtures** currently passing under `--enable=<flag>` stay green,
  now without the enable line (fixture-level `--enable` lines can drop in
  the same commit).

## Fix the stale "release-cut enforced" annotation

The descriptor comments claim `expires_at` is "release-cut enforced"
(`src/runtime/experiments.c:19,46,53,60,67,74`; `src/runtime/experiments.h:9,25,31`).
The gate was removed in commit `8855f6fd8` and the release skills no longer
consult `expires_at`. The comment lies, and while it lies it will keep
inviting the gate to be reinvented (see the [no hallucinated release gates
feedback memory][mem]). Retire the phrase in the same PR -- reword to
"soft deadline; review the row and either graduate, shelve, or bump on that
version's cut."

[mem]: (context-local memory; not a repo file)

## Order of operations

One PR:

1. Fix stale descriptor comments (drop "release-cut enforced").
2. For each of the four flags, in any order: delete row, delete global,
   delete gate. Land as separate commits within the PR so the graduations
   read cleanly in `git log`, or one commit if the diff is small.
3. Bump `forall-dict-pass`'s `expires_at` (done: 0.28.0) and point its
   `plan_path` at the Deficit-2 plan, cross-linking the archived report in the
   descriptor comment.
4. Move `constrained-hkt-forall-plan.md` from `docs/upcoming/v1/` to
   `docs/archive/` (the mode-B plan is already archived).
5. Run `bash tests/run.sh` with the 10-minute timeout; regenerate any
   fixtures whose diagnostic surface moved (expected: none).
6. Archive this plan on merge to `docs/archive/`.

## Success criteria

- `tur experiments` lists exactly one row (`forall-dict-pass`) after the PR
  lands.
- No `.tur` file in the tree still needs `--enable=forall-kinds`,
  `--enable=forall-constraints`, `--enable=hkt-hrt`, or
  `--enable=hrt-curried-result` to compile.
- `bash tests/run.sh` is at the same pass count as before, minus any
  fixtures whose only purpose was to exercise the disable-side error
  message (which can be deleted alongside the guard).
- A minor release can be cut without any experiment-expiry concern for the
  four retired flags.
