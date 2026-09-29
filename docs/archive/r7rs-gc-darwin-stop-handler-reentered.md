# macOS: the collector's stop signal re-enters its own handler

**RESOLVED 2026-09-28**, in the PR that made the DK runtime's state
per-thread (its new fixture `r7rs-threads-cps-entries` is what exposed this).
On macOS the r7rs-gc stop handler no longer waits in `sigsuspend` for a
resume signal. It polls its `stopped` flag and yields, the collector sends
no resume signal there, and a nested entry of the handler acknowledges and
returns to the entry it interrupted, so nesting never goes deeper than a
frame or two. Linux is unchanged.

## Symptom

`tests/run-r7rs-gc.sh` on macOS runs every r7rs fixture under
`TUR_GC_TORTURE=31`. `r7rs-threads-cps-entries` (four worker threads and
main, a loop of `call/cc` escapes) failed most runs there, and passed every
run on Linux:

| macOS build | 12 runs under torture |
| --- | --- |
| Release | 3 right, 7 SIGILL (exit 132), 2 hung |
| Debug (ASan/UBSan runtime linked) | 3 right, 4 SIGILL, 5 hung |

Without torture it passed 6 of 6 on both. Bisected on macOS: main alone
passed 10 of 10; the four workers alone failed 8 of 10. None of the three
`call/cc` forms alone crashed in 10 runs; two of them hung once each.

## What was happening

The macOS crash reports read `EXC_CRASH (SIGILL)` with termination
`SIGNAL 25`. Signal 25 is SIGXFSZ, the collector's resume signal on macOS.
The kernel raises SIGILL when it cannot write a signal's frame onto the
thread's stack. The faulting stack pointer was 112 bytes above a 16 KiB page
boundary in all three reports read.

The stacks above it were `tur_gc_stop_handler` → `_sigtramp` →
`tur_gc_stop_handler`, six deep, on every stopped worker. Every one of those
workers had been stopped waiting on the collector's heap mutex. The handler
installs with a full `sa_mask`, and the only signal its `sigsuspend` lets
through is the resume signal. Even so, the stop signal was being delivered
into a handler that was still running.

The diagnostic counted the handler's entries, and how many arrived while an
earlier entry on the same thread was still running (the program is the
fixture; `stress` is the `r7rs-threads-stress` control; 8 runs each):

| handler wait | workers | full fixture | stress |
| --- | --- | --- | --- |
| `sigsuspend` (as before) | 4 right, 4 hung; 32-47% of entries nested | 4 right, 4 hung; 26-28% nested | 8 right; none nested |
| poll `stopped`, `sched_yield` | **8 right**; 62-65% nested | **8 right**; 65-67% nested | 8 right; none nested |
| return from a nested entry at once | 8 hung | 8 hung | 8 right |

On Linux the same fixture enters the handler about 320,000 times a run, and
never nested.

Nesting is harmless in itself. A thread gets one stop signal per
collection, and each collection waits for every thread's acknowledgement
before sending its own. So a nested entry is always the next collection
stopping a thread that has not yet left the previous collection's handler.
It spills from a deeper frame, which the scan covers, and acknowledges for
its own collection. Returning from it without acknowledging loses that
collection's acknowledgement, which is why the third row hangs.

What breaks is waiting at every level. With `sigsuspend`, each nested
level waits for a resume signal of its own, the levels consumed one
another's resume signals, and threads hung. Polling fixed that (the second
row), but on its own it still let the nesting grow: under torture the next
collection's stop signal keeps landing on a thread just as it is about to
leave, and each landing is another frame. The first push polled only, and
CI's Debug build still died with SIGILL, the kernel out of stack for a
signal frame. A second diagnostic measured the depth (8 runs per cell, both
builds):

| nested entry | Debug | Release |
| --- | --- | --- |
| waits in its own loop (polling alone) | 1 SIGILL in 16; depth up to 554 | 3 SIGILL in 16; depth up to 334 |
| acknowledges and returns to the outer loop | **16 right**; depth 2-3 | **16 right**; depth 2-3 |

A nested entry does not need to wait. Since the outer entry spilled, the
thread has run nothing but the handler, so the outer spill is still its
state; the nested entry marks the thread stopped for its own collection,
acknowledges, and returns to the outer entry's loop, which waits for that
collection too.

## Fix

`src/runtime/r7gc.c`, on `__APPLE__` only:

- `tur_gc_stop_handler` waits with `while (stopped) sched_yield();` in place
  of the `sigsuspend` loop. `sched_yield` is a Mach trap (`swtch_pri`), safe
  in a handler.
- A thread record carries `in_stop`. An entry that finds it set is nested:
  it sets `stopped`, acknowledges, and returns without spilling or waiting.
  The outer entry clears `in_stop` when its wait ends and checks `stopped`
  once more, since a stop signal landing between the two is an outer entry
  of its own.
- `tur_gc_start_world` clears `stopped` and sends no resume signal.

Tested on Linux as well, with the handler installed with an empty mask and
`SA_NODEFER` so that it nests there too (about 20,000 nested entries a run):
the fixture 10 of 10 and `tests/run-r7rs-gc.sh` 215 of 215.

The handlers for both signals are still installed, so nothing else changes.

## Open question

Why a handler with a full `sa_mask` sees its own signal again on macOS is
not established. It happened only to threads that had been stopped while
blocked in libpthread's mutex wait (`__psynch_mutexwait`), as far as the
crash reports show. The stress fixture never nested. Boehm GC does not use
signals on Darwin at all: it stops threads with Mach `thread_suspend` and reads their
registers with `thread_get_state`. That is the fallback if this handler
ever proves insufficient.
