---
title: mono_specs.h header comment claims VBM2b is deferred and codegen is
  carrier-box-only -- both false since 2026-07-05
severity: LOW (documentation / comment accuracy). No runtime, codegen, or
  type-checking defect. Filed because the comment actively misleads readers
  about which code path is live, and has already caused at least two incorrect
  survey conclusions.
status: RESOLVED 2026-08-13. All four fix directions landed; see "Resolution"
  at the bottom. Filed 2026-07-28.
---

# `mono_specs.h` header comment describes a superseded state

## Summary

The block comment at `src/compiler/mono_specs.h:8-34` says the by-value
monomorphization registry is **registry-only**, that **codegen keeps the Path A
carrier-box path**, and that the per-spec by-value body emit (VBM2b) is a
separate deferred item. All three claims were true when written and are false
now. VBM2b landed 2026-07-04, `vl-wide-mono` graduated to unconditional
2026-07-05, and composed-lens by-value propagation closed the same day.

The comment is self-contradicting: a later paragraph (`:32-34`) correctly notes
the 2026-07-05 graduation, but the earlier paragraph it contradicts was never
updated. A reader who stops at the first paragraph -- the natural reading
order -- gets the wrong answer.

## The offending text

`src/compiler/mono_specs.h:25-28`:

```
 * ... at most one emitted body per
 * concrete key.  Still registry-only: codegen keeps the Path A carrier-box path;
 * the per-spec by-value body emit (VBM2b) is tracked separately (see the plan and
 * docs/reported/vbm2-byvalue-lens-body-emit.md -- it depends on closing the
 * documented "M7-by-value gap" the MB2.5 carve-out at emit_module.c leaves open).
```

Three separate inaccuracies:

1. **"Still registry-only: codegen keeps the Path A carrier-box path."** False.
2. **"the per-spec by-value body emit (VBM2b) is tracked separately."** VBM2b is
   done, not tracked.
3. **Both referenced paths are dead.** `docs/reported/vbm2-byvalue-lens-body-emit.md`
   is now `docs/archive/history/vbm2-byvalue-lens-body-emit.md`, and
   `docs/upcoming/van-laarhoven-monomorphization-plan.md` (line 8) is now
   `docs/archive/history/van-laarhoven-monomorphization-plan.md`.

The graduation note at `src/runtime/experiments.c:77-83` also points at
`docs/upcoming/v2/van-laarhoven-composed-byvalue-plan.md`; there is no
`docs/upcoming/v2/` directory. That plan is at
`docs/archive/history/van-laarhoven-composed-byvalue-plan.md`.

## Actual current state

| Slice | Status | Evidence |
|---|---|---|
| VBM2a (cross-procedural spec resolution) | LANDED | `mono_specs_resolve_program`, `mono_specs.c` |
| VBM2b (by-value body emit) | **DONE 2026-07-04** | `docs/archive/history/vbm2-byvalue-lens-body-emit.md` |
| VBM3 (dispatch redirect) | DONE 2026-07-04 | same |
| VBM4 / `vl-wide-functor` | **GRADUATED 2026-07-04** | `src/runtime/experiments.c:71-76` |
| CM4 / `vl-wide-mono` | **GRADUATED 2026-07-05** | `src/runtime/experiments.c:77-83` |
| CB1-CB5 (composed-lens by-value) | **RESOLVED 2026-07-05** | `docs/archive/history/van-laarhoven-composed-byvalue-plan.md` |

What the code actually does today:

- **Simple lenses** (direct `fmap` tail) redirect unconditionally to a by-value
  `<lens>__mono_<hash>` body -- no carrier box, no dict dispatch. The VBM2b
  report records `dict_Functor_Identity_singleton` reaching **zero uses**, and
  `set-px` / `over-px` emitting `run_id__spec(point_x__mono(g, s))` with no
  `emit_agg_box` / `emit_agg_unbox` around the `(f S)` crossing.
- **Composed lenses** also thread `(f a)` by value end to end (CB1-CB5); the
  `has_composed_lens` poison survives only as the CB5 backstop.
- **Path A (carrier box)** is now a **narrow backstop**, not the default. It
  handles only the shapes CB2/CB3 cannot lower: runtime-selected nested lens,
  non-lens tail, missing adapter, and depth past the cap.
- `g_opt_vl_wide_mono` is **retired**; registration is unconditional.

So the accurate one-line summary is the near-inverse of the comment: *the
by-value path is the default for both simple and composed lenses; the carrier
box survives only as a backstop for a handful of unlowerable shapes.*

## Impact

No product defect -- this is purely a comment. But it is load-bearing for
anyone reasoning about ABI direction, because `mono_specs.h` is the natural
first stop for "how far along is monomorphization?" During the investigation
that produced this report, the stale paragraph caused a survey to conclude
"monomorphization registry is complete but by-value emit deferred, so codegen
still takes the carrier-box path for the wide case" -- a statement that was
then repeated downstream before being caught. That is two incorrect
conclusions from one comment.

This is the second instance of the same failure mode found in a single session;
the first was an archived row-types doc still being read as a live gap list.
See `docs/upcoming/row-types-followups-plan.md`.

## Fix directions

Small and self-contained:

1. Rewrite `src/compiler/mono_specs.h:25-28` to state the post-graduation
   reality: by-value emit is live for simple and composed lenses; Path A is the
   CB5 backstop for unlowerable shapes. Fold the accurate `:32-34` paragraph in
   rather than leaving two paragraphs that disagree.
2. Repoint the two dead doc paths at `docs/archive/history/`.
3. Repoint `src/runtime/experiments.c:77-83`'s
   `docs/upcoming/v2/van-laarhoven-composed-byvalue-plan.md` at its archive
   location.
4. While there, sweep for other `docs/upcoming/...` and `docs/reported/...`
   paths in source comments that have since moved to `docs/archive/` -- the
   archiving rule means every resolved report moves, so source comments citing
   them rot by design. A grep over `src/` for `docs/reported/` and
   `docs/upcoming/` would find the rest in one pass.

Item 4 is the general fix and worth doing once: comments that cite a
`docs/reported/` path are guaranteed to break when the report is archived, so
either they should cite the archive path from the start or cite nothing.

## References

- `src/compiler/mono_specs.h:8-34` -- the comment
- `src/runtime/experiments.c:71-83` -- the two graduation notes (accurate)
- `docs/archive/history/vbm2-byvalue-lens-body-emit.md` -- VBM2b resolution
- `docs/archive/history/van-laarhoven-composed-byvalue-plan.md` -- CB1-CB5
- `docs/archive/history/van-laarhoven-consumer-mono-plan.md` -- CM1-CM4
- `docs/guides/monomorphization-abi-guide.md`

## Resolution (2026-08-13)

All four fix directions landed together.

**1. The header comment.** `src/compiler/mono_specs.h` no longer carries two
paragraphs that disagree. The "still registry-only / codegen keeps the Path A
carrier-box path / VBM2b tracked separately" text is gone, replaced by a
`CURRENT STATE` block stating the post-graduation reality directly: by-value
body emit is live for SIMPLE lenses (VBM2b/VBM3, 2026-07-04) and for COMPOSED
lenses (CB1-CB5, 2026-07-05), and Path A is a narrow backstop for the shapes
CB2/CB3 cannot lower. The accurate `:32-34` paragraph is folded in rather than
left as a trailing correction.

**2/3. The dead doc paths.** Fixed as part of the general sweep below, which
subsumed both named cases (`vbm2-byvalue-lens-body-emit.md`,
`van-laarhoven-monomorphization-plan.md`, and `experiments.c`'s
`docs/upcoming/v2/van-laarhoven-composed-byvalue-plan.md`).

**4. The general sweep.** Every `docs/{reported,upcoming,archive}/` path cited
from `src/` was checked against the tree and repointed at its real location --
**255 citations across 88 files**, resolved by basename against `docs/`:

- 213 substitutions: `docs/reported/...` and `docs/upcoming/...` citations of
  reports and plans that had since been archived.
- 42 substitutions: citations that already said `docs/archive/` but named the
  wrong half of the split -- the file had moved on to `docs/archive/history/`.
  This class was invisible until the first pass ran, and is the one most likely
  to recur.
- 16 line-wrapped citations, where the path straddles a `*` comment
  continuation and so is invisible to a single-line grep. These need a
  multi-line match; a naive sweep silently skips them.

Two ambiguous basenames (present in both `docs/archive/` and
`docs/archive/history/`) were resolved to `docs/archive/` on the rule that a
source comment citing a *report* means the report, not its paper trail.

**Three citations named docs that were never filed at all** -- and each turned
out to be describing a state that is no longer true, so the fix was to correct
the claim rather than to file the missing report:

| Citation | Claim | Reality |
|---|---|---|
| `match-int-scrutinee-guard-null-adt.md` | guard needed | the guard is present -- the `!_scrut_is_adt` disjunct at `emit_expr.c`. Comment now points at the guard instead of a phantom report. |
| `stm-tvar-modify-codegen-stub.md` | "compiled path emits a no-op stub -- latent bug" | **false.** `elab_tvar_modify` (`elab_concurrent.c:743`) lowers `(tvar/modify tv f)` to `(let [g tv] (tvar/swap g (f (tvar/read g))))`, so the `EX_TVAR_MODIFY` arm in `emit_expr.c:8342` is a dead defensive arm, exactly as its own comment says. `eval.c` was warning about a bug that cannot fire. |
| `taskgroup-block-cancel-reason-layout-overflow.md` | "compiled `task-group-new` omits `cancel_reason` -- latent OOB write" | **false.** `stdlib/taskgroup.tur:85` declares `int64_t cancel_reason` and line 87 allocates `sizeof(TaskGroupBlock)`. Both layouts agree; the comment is stale. |

That last row is the interesting one. This report's thesis was that a stale
comment misleads readers -- and these two were worse than stale, they were
*inventing* defects. A reader auditing STM or task-group cancellation would
have found a comment asserting a live memory-safety bug, with a report path to
go read, and no report at either end of it.

**Also corrected while in the file** (same stale-comment failure mode, found by
following the citations):

- `mono_specs.c`'s `has_composed_lens` field comment still described the CM4
  gate ("a composed lens ... which the by-value mono body cannot yet emit").
  CB1-CB5 superseded that; the flag now marks only the CB5 backstop residue.
  The `resolve_walk` comment 600 lines below already said so, so this was the
  same two-paragraphs-disagree shape as the header itself.
- `experiments.c`'s vl-wide-mono graduation note said composed lenses "fall
  back to the always-on Path A carrier bridge" with the fix "tracked in" the
  composed-byvalue plan. True on 2026-07-05, closed the same day.

One citation was deliberately left alone: `docs/upcoming/v1/fancy-rows-plan.md`
in `experiments.c`'s header is inside a block explicitly marked "do not
uncomment -- illustrative only", showing the shape of an `EXPERIMENTS[]` row.
It is a fictional example, not a citation, and `fancy-rows` is not a real row.

### The general lesson, restated

The report's item 4 called this out and it holds: **a source comment citing a
`docs/reported/` path is guaranteed to rot**, because the archiving rule moves
every resolved report by design. 255 rotted citations is what one repo
accumulates. Two follow-ons worth knowing for the next sweep:

- Citing `docs/archive/` up front is not sufficient -- 42 of these already did,
  and rotted anyway when the file moved to `docs/archive/history/`.
- A grep that does not handle `*`-continuation line wrapping misses ~6% of the
  citations, and they are silently missed.

`git grep -n 'docs/\(reported\|upcoming\|archive\)/'` piped through an
existence check reproduces the audit in one pass; it now comes back clean.
