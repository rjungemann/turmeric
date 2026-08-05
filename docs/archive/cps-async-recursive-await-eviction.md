# cps-async: recursive `await` evicts to the direct emitter -- and that is correct

**STATUS: RESOLVED / BY-DESIGN (archived 2026-07-18).** This was never a defect
-- the eviction is a deliberate, correct engineering choice. Recorded as a
permanent design note; folded into the endgame plan's carve-out reasoning.
Archived from `docs/reported/`.

**Severity:** low (not a defect -- a deliberate, better-for-the-common-case
eviction; filed to record the investigation so it is not "re-fixed" into a
regression).

## Summary

Under `--enable=cps-async`, a function whose `await` continuation is
**non-straight-line** -- a branch, a tail call, or a recursive-`await`
continuation -- does **not** color onto the heap-continuation (`dk_shift` /
`__tur_await_body`) lowering. It evicts to the direct emitter, which lowers
`await` via `tur_await_future`. This was originally framed (plan F3.5) as a gap:
"deep async on the heap representation is not yet expressible." Investigation
shows the eviction is the **correct** default, not a bug.

## Why forcing the heap path is a regression

A prototype that admitted the full continuation (validate `await`'s continuation
with `term_core_ok`, lift it as an `LH_RESET_CONT` body threading the enclosing
`k`, `next = dk_done()`) made recursive `await` color and produced **correct**
results. But a **ready-future** inline resume then recurses through `dk_invoke`,
which is not a tail call (it `dk_free`s after `dk_run_impl`), so each recursion
level retains C frames:

```
go__cps(N) -> dk_run_impl [shift] -> __tur_await_body -> dk_invoke
           -> dk_run_impl [frame] -> go_ak0 -> go__cps(N-1) -> ...
```

Measured, `go(n,acc) = let v = await(async (fn [] 1)) in if n==0 then acc else go(n-1, acc+v)`:

| depth | heap path (prototype), `ulimit -s 256` | direct path (shipping) |
| ---: | --- | --- |
| 1,000 | 1000 (ok) | 1000 (ok) |
| 100,000 | **SIGSEGV** (O(N) stack) | 100000 (ok) |
| 1,000,000 | **SIGSEGV** | 1000000 (ok) |

The direct emitter checks readiness inline and tail-loops in **O(1)** stack
(`__tur_tailcall:` + `goto`), so a ready-future recursion is strictly better
there. `(async fn)` is synchronous today, so recursive `await` is *always* a
ready future on the compiled path -- exactly the case the heap path handles
worst.

## Where the heap representation actually wins

The DK heap continuation's stackless property is for a **genuinely suspending**
await (F3.2): the park unwinds the C stack back to the driver, and the resume
runs flat from `tur_future_fulfill -> on_complete`. That is real and landed for
the single-await case. It does **not** apply to synchronous recursion, where
there is no suspension to unwind.

Neither path dominates for recursion:

| | ready future | pending future |
| --- | --- | --- |
| direct (`tur_await_future`) | O(1) stack (TCO) | busy-loop / hang if not in a fiber |
| heap (`dk_shift`) | O(N) stack (dk_invoke recurses) | O(1) with the F3.2 driver |

Since `await` readiness is not known at compile time and the synchronous case
dominates, evicting recursive `await` to the direct TCO path is the right
default.

## Fix direction (only if a stackless *suspending* recursive await is needed)

Make the DK inline resume trampolined so `dk_invoke` does not recurse: a ready
`__tur_await_body` would return a "resume with this (subk, value)" signal to a
top-level driver loop that iterates instead of calling back into `dk_run_impl`.
That is a core-substrate change shared with effects (`dk_perform` / `resume`
have the same shape), so it must not regress the graduated effect runtime -- a
separate, larger effort. Alternatively, an `await`-specific inline lowering that
checks `tur_future_done` and continues *inline* (letting the C compiler TCO the
recursive tail call) would match the direct path's O(1) behavior while keeping a
heap park only on the pending branch -- essentially re-deriving `tur_await_future`
inside the colored body.

## Status

Resolved as **works-as-intended**. The `term_core_ok` CT_AWAIT arm carries a
comment pointing here so the eviction is not mistaken for a missing case and
"fixed" into the O(N) regression above. The async-rec sign-off probe
(`tests/probes/stackless-signoff/async-rec.tur`) runs the direct path at
1,000,000 depth under `ulimit -s 256`.
