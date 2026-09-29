# clang (and gcc on Windows) reuse a thread-local's address after a fiber changes threads

**RESOLVED 2026-09-29**, in the PR that added
`tests/fixtures/r7rs-threads-fiber-migration`. Under clang, and under gcc on
Windows, the thread-locals a fiber carries from thread to thread, and the
collector's per-thread record where a fiber's code reads it, are now read
through an accessor that cannot be merged. gcc builds elsewhere are
unchanged.

## Symptom

`tur_scheduler_mt` keeps one shared queue, so a fiber that yields is resumed
by whichever worker is free next. The fixture's nine fibers change threads
on more than half of their yields. On macOS CI it failed every run, and a
plain Turmeric version of it with no Scheme and no collector (nine fibers
counting to 40, one `tur_scheduler_mt_yield` per step) failed too:

| build | plain, 3 workers | plain, 1 worker | fixture, 3 workers |
| --- | --- | --- | --- |
| macOS, Apple clang | 0 of 6 and 3 of 6 right | 6 of 6 right | 0 of 6 right |
| Linux, gcc | 20 of 20 | | 20 of 20 |
| Linux, clang 18 | 0 of 20 | | 0 of 20 |

The plain version failed with `fiber-yield: not in fiber`, or crashed. The
fixture failed with the same message, or with `continuation invoked after
its call/cc prompt returned`. The macOS fixture's 1-worker runs crashed as
well; that is a separate bug, `hoisted-include-shrinks-ucontext-on-macos`.

## What was happening

clang takes the address of a thread-local to be fixed for the whole of a
function call. `llvm.threadlocal.address` is `readnone`, so the address is
computed once and kept in a register across every call that follows,
including the call that switches fibers. When the fiber resumes on another
worker, the function goes on using the first worker's slot:

- `tur_fiber_block_yield`, inlined into a counting loop, read
  `tur_current_fiber` through the first worker's address. That worker had
  since cleared it, hence "not in fiber".
- A CPS entry wrapper raises `__dk_entry_depth`, saves `g_dk_driver`, runs
  its body and restores both. When the body yielded and moved, the restores
  went into the first worker's DK state, under whatever that worker was
  running by then.
- `call/cc` pushed its prompt onto the fiber's live-escape set, and the
  escape after the move looked for it in the old thread's set.

gcc on x86-64 Linux addresses every thread-local as `%fs:offset`, which
reads the thread pointer afresh at each access, and passed every run. gcc on
Windows (MSYS2's MinGW) does not. Its thread-locals are emulated, and
`__emutls_get_address` is a `const` builtin, so gcc hoists the call out of a
loop around a switch just as clang does. `fiber-scheduler-mt-migration`
passed on one Windows CI run and failed the next, with no output. The
fixture's counting loop, cross-compiled, computed `tur_current_fiber`'s
address once, before its first `__tur_uctx_swap`.

## Fix

- `TUR_TLS_FRESH(T, x, x__at)` (preamble head, clang, and gcc on `_WIN32`)
  defines `x__at()`, which returns `&x`. It is `noinline`, and an empty
  `asm volatile` with a memory clobber keeps clang from deducing that it
  reads no memory and merging two calls to it. The r7rs prelude split
  (on by default on Linux and macOS) reads the emitted text token by token, before
  preprocessing, and shaped the macro twice. The accessor's name is an
  argument rather than pasted from `x`, since the split's renaming did not
  see a pasted name (the first version failed to build under clang there).
  Each use ends in `;`, which the `extern` declaration the expansion ends
  with takes up. An unterminated use ran on into the next function in the
  split's eyes, the program unit lost that function's body, and every
  split build fell back to one unit.
- The state `tur_fiber_block_resume` swaps per fiber is `#define`d to
  `(*x__at())`: `tur_current_fiber`, the DK registry, entry depth, driver
  and resume state (`emit_dk_runtime.c`), and the live-escape set. Each block
  is guarded by `!defined(x)`, so a front end or split half that already
  reaches `x` through a host accessor (`src/runtime/tur_tls.c`, c2mir and
  the split runtime) keeps that.
- The collector's `tur_gc_self`, the calling thread's record, goes through
  the accessor (`TUR_GC_SELF_FRESH()`, `src/runtime/r7gc.c`) wherever the
  program's own code reads it: the allocator and `free`, a collection
  started from an allocation, the park and unpark around a blocking call
  and their `EINTR` checks, and the thread and key wrappers. Read stale, an
  allocation after a move popped the first worker's cache while that worker
  popped it too, and one slot went out twice. On macOS under
  `TUR_GC_TORTURE=31` the fixture crashed in 3 of 4 runs without this and
  passed 4 of 4 with it. The crash reports showed `tur_gc_mark_roots`
  faulting, and a fiber resumed from a context main was still building. A
  stale park or unpark, in turn, left a fiber's new worker parked for good.
  The stop handler, the collector's own frames, and a thread's start and
  end keep the plain read. They run on the thread they read and are never
  inlined into a fiber's code.
- Along the way, every change to park or unpark lost a parked thread's
  roots on macOS (`r7rs-threads-roots` under `TUR_GC_TORTURE=1`, every
  run). That was a separate bug the change only exposed:
  `parked-snapshot-unaligned`.

Thread-locals that belong to the thread rather than the fiber are not
changed. The trampoline's `tur_tb_*` and `tur_handler_chain` are not swapped
per fiber, so a fiber that yields inside them is already out of bounds.
`tur_panicking` is read after nearly every call in CPS code. It stays
direct: routing it through the accessor too cost 24% on the benchmark
below, against 4% without it.

## Cost

Only clang builds and Windows gcc builds change. The fiber's own state
changes only in the whole-preamble path, since split-runtime builds already
reached it through the host accessors. The collector's record changes in
both, since the split pastes the collector into each unit. The benchmark was a `#lang r7rs`
program of `fib 27`, 300,000 `call/cc` escapes, and 50 lists of 20,000
built and summed, compiled with clang 18 `-O2` on Linux. Times are the
minimum of 30 runs, over three rounds:

| build | main | this fix |
| --- | --- | --- |
| whole preamble (`TUR_PRELUDE_SPLIT=0`) | 172 ms | 191-192 ms |
| prelude split (the default) | 176-180 ms | 193-196 ms |

An earlier measurement took the whole-preamble cost in stages: 158-173 ms
direct, 175-177 ms with the fiber's own state through the accessor, and
182-186 ms with the allocator's read of the record too. Every preamble
thread-local through an accessor cost 205-216 ms.

## Verified

Linux, clang 18: the fixture 20 of 20, 10 of 10 under
`TUR_GC_TORTURE=31` with and without the prelude split, and the plain version
20 of 20. Windows: the plain version cross-compiled with MinGW gcc 13 reads
`tur_current_fiber` through the accessor after every swap. macOS and Windows
CI: see the PR.
