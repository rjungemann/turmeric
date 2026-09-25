# A threaded program under `tur jit` can hang under CPU load

**Severity:** medium. A threaded program run under `tur jit` sometimes never
finishes when the machine is busy. The compiled build of the same program
does not. On a loaded CI runner this can read as a flaky JIT leg. Found while
fixing docs/archive/jit-fork-child-hangs-with-threads.md; a different bug.

## Repro

```sh
# six busy loops to load a four-core box, then:
for i in $(seq 1 25); do
  timeout 15 ./build-jit/tur jit tests/fixtures/r7rs-threads-pause/input.tur
done
```

| Build | Under load | Result |
|---|---|---|
| `tur jit` | 6 busy loops | 2 of 25 runs timed out (exit 124) |
| `tur jit` | none | 0 of 40 runs hung |
| compiled | 6 busy loops | 0 of 40 runs hung |

It also showed in `tests/run-jit.sh` batches of the `r7rs-threads-*`
fixtures. Two of eleven batches had one timeout, on `r7rs-threads-pause` or
`r7rs-threads-roots`, reported as a stdout mismatch with empty stdout.

A hung run, under gdb:

- The main program thread is in `pthread_join` on the worker.
- The worker is running in JIT-generated code. It is not blocked in any
  system call, so it is spinning.

## Root cause

Unconfirmed. The prime suspect is thread-local storage. c2mir warns "Thread
local is not implemented" for every `_Thread_local` in the emitted runtime,
so under the JIT each `TUR_THREAD_LOCAL` variable is one global that every
thread shares: the handler chain, the shift/reset state, the escape note,
the backtracking root and the others. Two threads running Scheme code then
read and write each other's runtime state, and a lost or crossed value can
leave a loop that never ends. Load makes the interleavings that trigger it
likelier.

## Fix directions

- Confirm by symbolizing the spinning worker's frame. The JIT can map an
  address back to its function.
- Emulate thread-locals under `TUR_JIT_ENGINE`: route each one through a
  per-thread block the host allocates (`pthread_getspecific`, or an
  accessor the host exports), the way `^thread-local` globals already work.
- Until then, document that threaded programs belong in the compiled build.
  Or have `tur jit` warn at the first thread start, as the collector's
  TUR-W0072 once did.
