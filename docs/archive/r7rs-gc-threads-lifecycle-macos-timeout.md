# `run-r7rs-gc.sh`: `threads-lifecycle` timed out on macOS after the world-lock fix

**RESOLVED 2026-09-27.** A deadlock with libpthread's own thread-list lock,
root-caused from the stack dump the harness began printing with this report.
It recurred on PR #949's macOS Auxiliary suites job, and the dump showed it.
The rest of this file below "Root cause" and "Fix" is the report as filed.

## Root cause

macOS's `pthread_create` holds libpthread's global thread-list lock (an
`os_unfair_lock`) while it links the new thread in, and `pthread_kill` takes
the same lock to validate its target. The collector stops a thread by
signalling it (`pthread_kill` in `tur_gc_stop_world`). The dump at the
deadline:

- thread 1, the program's main thread, was in the stop handler's
  `sigsuspend`, interrupted inside `_pthread_create` (called from
  `tur_gc_pthread_create`, spawning the fixture's detached threads). It was
  stopped, and still held the list lock;
- thread 2, the collecting thread (inside `tur_gc_malloc`), was blocked in
  `pthread_kill` on `_os_unfair_lock_lock_slow`, trying to signal the next
  thread;
- the rest were stopped, or were new threads waiting on `world` in
  `tur_gc_thread_main`.

The collector waited on a lock held by a thread it had already stopped.
glibc's `pthread_create` takes no lock that `pthread_kill` needs, which is
why Linux never showed it. The window is the few instructions
`pthread_create` spends under the lock, so the stall was rare. It showed
only under `TUR_GC_TORTURE`, with 800 thread creations racing a collection
every 31 allocations.

## Fix

`tur_gc_pthread_create` and `tur_gc_pthread_detach` (macOS's
`pthread_detach` takes the same lock) now park the calling thread around
the libc call. Nothing is scanned there, and a parked thread is never
signalled. `pthread_join` already parked. Nothing in the stdlib or the
emitter spells another call that takes the list lock (`pthread_cancel`,
`pthread_kill`, `pthread_getname_np`, ...). r7gc.c's header comment states
the rule: a thread must never be stopped holding a lock the stop itself
takes.


**Severity:** low-medium, unconfirmed as a hang. Seen once, on macOS CI.
The Linux self-deadlock in
[docs/archive/r7rs-gc-threads-lifecycle-rare-hang.md](../archive/r7rs-gc-threads-lifecycle-rare-hang.md)
is fixed, and this sighting is on a runtime that has that fix. Either a
second, macOS-only stall is in the fixture's path, or the case is simply
slow enough on a macOS runner to reach its 300 s deadline now and then.

Filed 2026-09-27 from PR #948's CI.

## What was seen

PR #948 (branch `claude/srfi-r7rs-stages`, head `16c80fbc4`), "Auxiliary
suites (macos-latest)":

```
FAIL threads-lifecycle -- timed out (>300s) under TUR_GC_TORTURE=31 (a missing root can read as a hang: a freed list walked in a cycle)
r7rs-gc: 76 passed, 1 failed (torture every 31 allocations)
```

The PR does not touch src/runtime/r7gc.c, the fixture or the harness. The
same job passed on PR #947's last commit, which carried the same runtime and
the same fixture.

## Why it may be a hang, and why it may not

- **The timings.** ctest's `tur_r7rs_gc` took 260.7 s on the passing #947
  run and 472.5 s on the failing #948 run. The cases other than
  `threads-lifecycle` took about 172 s in both: the failing run's 472.5 s is
  the 300 s deadline plus 172 s. So on the passing run
  `threads-lifecycle` took about 89 s. That is against 13 s on a 4-core
  Linux box: twenty rounds of forty detached threads, with a collection
  every 31 allocations, each stopping every live thread by signal.
- **So the failing run was at least 3.4 times the passing one**, while
  every other case in the same job ran at its usual speed. That points at
  this case, not the machine. But macOS thread creation and signal delivery
  are slow and vary. Nothing yet rules out a slow tail.

## What was checked, and ruled out

- **A thread exiting while signals are off.** macOS's `pthread_exit`
  disables signal delivery before it runs the key destructors, where
  `tur_gc_thread_gone` retires a detached thread. A thread signalled then
  would never answer the stop. It is not signalled, though: `tur_gc_stop_world`
  skips a record marked `done`, and `tur_gc_thread_end` marks it, under
  `world`, before the start routine's frame is left.
- **The fork phase.** A child that hangs is killed by its own `alarm(5)` and
  counted as a failure. The parent waits in `waitpid`, a release point.
  Neither can stall for 300 s.

## Diagnostics added with this report

`tests/run-r7rs-gc.sh` now dumps every thread's stack when a threaded case
outlives its deadline, before killing it. It uses gdb, lldb or macOS's
`sample`, whichever the host has. The next sighting's CI log will show
whether the threads are parked in the collector's stop handshake (a
deadlock) or still working (a slow run).

## Fix directions

- **If the stacks show a deadlock**, it is a macOS-specific ordering in the
  stop protocol or the thread-exit path. Start from the thread that holds
  `world` and what it waits on.
- **If they show a slow run**, cut the macOS cost of the detach phase
  rather than raising the deadline. For example, fewer rounds under
  `TUR_GC_TORTURE` on Darwin. The twenty rounds exist to make the Linux
  self-deadlock's one-retirement window likely. They do not need to be
  twenty on every host once that fix is in.
