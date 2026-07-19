# cps-async: heap `await` is not a superset of the fiber path for two shapes

**Severity:** medium (blocked the `cps-async` graduation).

**STATUS: RESOLVED (2026-07-19).** Both shapes below were fixed so the heap path
is a strict superset of the fiber path, and `cps-async` graduated the same day
(`docs/archive/cps-async-graduation-plan.md`). The fixes:

1. `taskgroup-async` -- the heap await runtime `__tur_await_body` now drains the
   scheduler run-queue on a pending future (`while (!done && run_queue_len > 0)
   tur_scheduler_run_one(...)`) and resumes inline once it resolves, mirroring
   `tur_await_future`'s non-fiber branch. Bounded by `run_queue_len` so the F3.2
   deferred/reactor park still applies when the scheduler cannot progress. See
   `src/compiler/emit_module.c` (`__tur_await_body`).
2. `async-effect-spawn` -- an `await` inside a handler-case body now delegates to
   the fiber path (`build_letraw` -> `CT_LETRAW`, admitted by `handle_case_ok`)
   instead of building a `CT_AWAIT` the handler-case admission cannot host,
   tracked by the new `CpsB.in_handler_case` depth counter (set in
   `build_handle_core`, consulted at the `EX_AWAIT` sites in `src/passes/cps_ir.c`).

Retained as the investigation record. Archived from `docs/reported/`.

## Summary

Graduating `cps-async` makes the heap `dk_shift` lowering the *sole* path for
`await` (removing the U5 fiber-delegation fallback: `is_delegatable` returns
false for `EX_AWAIT`, and `EX_AWAIT` always lowers to `build_await`). Two in-tree
fixtures that pass today on the fiber path then break, so the heap path is not a
functional strict superset of the fiber path for async:

1. **`tests/fixtures/taskgroup-async` -- wrong result (empty output).**
2. **`tests/fixtures/async-effect-spawn` -- build-time internal error.**

Both pass at baseline (flag off) and fail with the flag forced on as the only
lowering.

## Repro

```sh
# baseline (flag off): both pass
./build/tur run tests/fixtures/taskgroup-async/input.tur     # -> 10 / 20 / 30 / done
./build/tur run tests/fixtures/async-effect-spawn/input.tur  # -> 42

# with async forced heap-only (graduation): both fail
./build/tur run tests/fixtures/taskgroup-async/input.tur     # -> (empty)
./build/tur run tests/fixtures/async-effect-spawn/input.tur  # -> internal error:
#   effect form (EX kind 57) reached the direct/fiber emitter
#   (fiber effect runtime deleted)
```

## Root cause

The U5 delegation path (`is_delegatable` -> true for `EX_AWAIT` when the flag is
off; `src/passes/cps_ir.c`) is what keeps a fiber-scheduler-dependent await, or an
await nested in a colored effect body, running on the proven fiber runtime.
Graduation deletes that path, exposing two things the heap `dk_shift` await does
not do:

- **It does not drive the ucontext scheduler.** `taskgroup-async` manually spawns
  fibers via inline-C (`tur_scheduler_spawn`) and depends on `(await fut)` running
  the scheduler until each fiber completes. Heap `await` parks its continuation on
  the reactor instead, so the spawned fibers never run and the futures never
  fulfill.
- **It does not compose with a colored effect body.** In `async-effect-spawn` the
  `await` is inside a handler `resume` body. Coloring the `await` forces the
  handler-bearing function to evict to the direct emitter for the inadmissible
  combined shape -- and after cps-tramp-resume's graduation the direct emitter can
  no longer emit effects, so eviction is an internal error rather than a fallback.

## Fix directions

Either (or both) of:

1. Make the heap `await` path drive the ucontext scheduler when a future is
   pending and runnable fibers exist, so manual-spawn + await keeps working; or
   retire the fiber-scheduler-dependent await pattern from fixtures/stdlib and
   confirm no supported surface relies on it.
2. Make `async`/`await` compose inside a colored effect body without evicting to
   the (now effect-incapable) direct emitter, so `async-effect-spawn` colors
   cleanly on the heap/DK path.

Then re-run the superset check (all async fixtures green with the flag forced on
as the only path) and resume the graduation (delete the flag, move `cps-async`
to `GRADUATED[]`).
