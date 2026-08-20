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
