---
title: mono_specs.h header comment claims VBM2b is deferred and codegen is
  carrier-box-only -- both false since 2026-07-05
severity: LOW (documentation / comment accuracy). No runtime, codegen, or
  type-checking defect. Filed because the comment actively misleads readers
  about which code path is live, and has already caused at least two incorrect
  survey conclusions.
status: OPEN. Fix is a comment edit in src/compiler/mono_specs.h:25-28 plus two
  stale doc paths. Filed 2026-07-28.
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
