# A panic inside a plain (async ...) body has no per-task boundary

**Severity: medium** -- the panic unwinds the caller's stack / aborts the
process instead of rejecting the task's future. error-handling-guide.md
documents the intended semantics as "Planned". Found in the 2026-08-20 docs
audit.

## Repro

```turmeric
(let [f (async (fn [] : int (panic "boom")))]
  (await f))
;; aborts the process; the future is never FUTURE_REJECTED
```

## Root cause

`tur_async_fiber` / `tur_async_fiber_closure`
(src/runtime/generated/tur_rt_split.c:2700-2738) run the body with no setjmp
panic handler; only task-group fibers get one (`tur_fiber_shim`, same file
~line 1991), and that one auto-cancels the group rather than rejecting a
future.

## Fix direction

Wrap the async body the way `tur_fiber_shim` does and route a caught panic to
`tur_future_reject` with the panic payload; surface it at `await` as the
existing rejection value (TURI_REJECTION parity on the interpreter).

## Guides to update when fixed

- docs/guides/error-handling-guide.md ("Planned (task-boundary panics)" block)
- docs/guides/async-await-guide.md

## Resolution (2026-08-21)

Fixed, with one correction to the filing's root cause.

**The mechanism was not a missing setjmp.** `tur_async_fiber` /
`tur_async_fiber_closure` run the body **inline on the caller's stack** -- there
is no fiber, so there is no `panic_jmpbuf` to arm, and `tur_fiber_shim`'s shape
does not transfer. The panic protocol these bodies actually use is the
`tur_handler_chain` one: with a handler node installed, `tur_panic` stages the
payload, sets `tur_panicking`, and *returns*, and the emitted body's
per-call-site `if (tur_panicking) return ...;` checks unwind back to the
boundary. So the fix is a handler node around the body call, exactly like
`tur_catch_unwind_box`, plus `tur_async_reject_if_panicking`, which converts a
staged panic into `tur_future_reject`.

**Await re-raises rather than aborting.** `tur_await_future` (and the CPS
`__tur_await_body`) previously printed `await: future rejected` and `abort()`ed
on a rejected future -- which would have made this fix a pure deferral of the
same process death, since nothing else at the Turmeric surface can observe a
rejection. Awaiting a rejected task now calls `tur_panic` with the task's own
message: catchable by a `catch-unwind` at the await, and with no handler in
scope it prints that message and aborts exactly as an uncaught panic does
anywhere. The rejection message takes ownership of the payload's strdup'd
string (`TurFuture::error` is an unowned `const char *`), so the payload struct
is freed with `owns_value` cleared.

Three properties, all pinned by `tests/fixtures/async-panic-rejects-future/`:
the spawn no longer unwinds its caller; a task whose panic nobody awaits does
not kill the program; and the panic surfaces at the await with its own message,
recoverable there.

Note the boundary is the **spawn-side frame**, so a panic raised after the task
re-parks on a pending `await` (the CPS/park path) is still outside it -- the
node is gone by the time the parked continuation resumes. That is recorded in
the guide's "Still planned" list along with the three items the report did not
cover (cancel-vs-panic precedence, async-main exit codes, the WASM lowering).

The change is in the runtime preamble (`src/compiler/emit_module.c`), so
`src/runtime/generated/tur_rt_split*` were regenerated with
`tools/gen-runtime-split.py` and all 142 `expected.c` snapshots with them.
`bash tests/run.sh`: 2677 passed, 0 failed.

## Guides updated

- `docs/guides/error-handling-guide.md` -- the "Today" block described the old
  no-boundary behaviour and the "Planned (task-boundary panics)" list is now
  three items shorter, with a worked recovery example.
- `docs/guides/async-await-guide.md` -- new "Task panics" subsection under Core
  Concepts.
