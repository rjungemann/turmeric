---
status: resolved 2026-08-05 (converted to LH_RESUME_CONT + dk_frame_resume_borrow, same day as filed)
severity: low (no reachable misbehavior; becomes real only if await continuations ever re-enter)
discovered: 2026-08-05
area: compiler (emit_cps_ir.c emit_await, F3 gap-2 bounded-continuation path)
---

# emit_await's bounded-continuation frame bakes its enclosing k

## Summary

`emit_await`'s F3 gap-2 path (a bounded full-CPS await continuation --
`await_cont_reset_ok` shapes: a branch, or a further sequential await) lifts
the continuation as `LH_RESET_CONT` and installs

```c
return dk_run(dk_shift(DK_ROOT_TAG, __tur_await_body, <fut>,
                       dk_frame(<aname>, <env carrying __k>, dk_done())), 0);
```

-- the pre-unification layout: the enclosing continuation is a pointer baked
into the frame's env, and the node's `next` is `dk_done()` (not even enclosing
handler markers). This is the same family as
[cps-reset-frame-pre-unification-layout](cps-reset-frame-pre-unification-layout.md)
and the fixed handle case
([cps-multishot-nontail-resume-inner-handle-drops-clause-rest](../archive/cps-multishot-nontail-resume-inner-handle-drops-clause-rest.md)).

## Why it is benign today

The baked env only misbehaves when a COPY of the frame is re-entered (the
copy jumps to the original chain) or when a chain walk needs to cross the
frame's `next` (it dead-ends). Neither is reachable for an await
continuation:

- it is resumed **exactly once**, by the future's completion delivering
  through the reactor -- there is no user-visible `k` to resume again, and no
  multi-shot surface reaches it;
- the `dk_done()` next means an effect performed in the await continuation
  would fail to find an enclosing handler through this frame -- but the
  admission for this path (`await_cont_reset_ok`, and the eviction of
  cps->cps tails noted in the emit comment) does not admit a `perform` there
  against an outer handler, so the dead end is not observed. The
  non-bounded await path (LH_PERFORM_CONT, `dk_frame(..., cur_k)`) threads
  the real chain already.

## The trap

Two futures directions would make this live: (a) admitting `perform` of an
outer-handled effect inside a bounded await continuation (the dead-end `next`
then yields a spurious "unhandled effect" or worse), and (b) any feature that
re-enters an await continuation -- speculative re-poll, cancellation-with-
retry, or exposing the continuation to user code. Either one first requires
the same conversion the handle got.

## Execution (2026-08-05)

Converted as prescribed, as a rider on the emit_reset conversion
([cps-reset-frame-pre-unification-layout](cps-reset-frame-pre-unification-layout.md)):
the bounded await continuation is lifted as `LH_RESUME_CONT` and installed as
`dk_frame_resume_borrow(<aname>, <caps-only env>, cur_k)` under the
`__tur_await_body` shift.

One correction to this report's own "mechanical and small" claim, worth
recording: the conversion is NOT purely mechanical, because this frame rides
below a **shift** node, so replacing `dk_done()` with the real chain changes
the shift's CAPTURE EXTENT -- the captured/parked continuation now includes a
copy of the real chain up to the root prompt, and resuming it replays that
copy instead of jumping to the baked originals (which become reap-collected
garbage).  That is the correct self-contained-continuation semantics, and
`__tur_await_body` is already copy-based on the park path
(`dk_copy_range(subk, NULL)` + `dk_free` after resume), so it absorbs the
larger capture unchanged -- but it is a semantic change, not a relabeling,
and it was verified against the delicate fixtures (async-await-cps,
-pending, -repark, -two) rather than assumed.  Both traps the report named
are closed: a perform in a resumed await continuation now finds the (copied)
enclosing handlers instead of dead-ending at `done`, and there is no baked
original left for a hypothetical re-entrant resume to double-run.

With both conversions landed, `LH_RESET_CONT` was deleted outright (no users
left), along with the `__k` env-slot machinery and the `borrowed_kont` copy
discipline.  Suites green: run.sh 2572/0, run-turi.sh 1759/0/705.
