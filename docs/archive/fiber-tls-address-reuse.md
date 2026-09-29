# clang reuses a thread-local's address after a fiber changes threads

**RESOLVED 2026-09-29**, in the PR that added
`tests/fixtures/r7rs-threads-fiber-migration`. Under clang, the thread-locals
a fiber carries from thread to thread, and the collector's per-thread record
where the allocator reads it, are now read through an accessor that cannot
be merged. gcc builds are unchanged.

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

gcc on x86-64 addresses every thread-local as `%fs:offset`, which reads the
thread pointer afresh at each access, and passed every run.

## Fix

- `TUR_TLS_FRESH(T, x, x__at)` (preamble head, clang only) defines
  `x__at()`, which returns `&x`. It is `noinline`, and an empty
  `asm volatile` with a memory clobber keeps clang from deducing that it
  reads no memory and merging two calls to it. The r7rs prelude split
  (on by default on Linux) reads the emitted text token by token, before
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
  the accessor where the allocator and `free` read it
  (`TUR_GC_SELF_FRESH()`, `src/runtime/r7gc.c`). Read stale, an allocation
  after a move popped the first worker's cache while that worker popped it
  too, and one slot went out twice. On macOS under `TUR_GC_TORTURE=31` the
  fixture crashed in 3 of 4 runs without this and passed 4 of 4 with it; the
  crash reports showed `tur_gc_mark_roots` faulting, and a fiber resumed from
  a context main was still building.
- Everywhere else in the collector the read is as it was. Two broader
  versions each lost the roots of a parked thread on macOS
  (`r7rs-threads-roots` under `TUR_GC_TORTURE=1`, 0 of 4, Release and Debug):
  every read through the accessor, and the collector's entry points made
  `noinline`. The second also broke `r7rs-threads-pause`. `tur_gc_park`
  takes a parked thread's registers from inside its own frame, and it and
  the blocking-call wrappers around it are left exactly as they were.

Thread-locals that belong to the thread rather than the fiber are not
changed. The trampoline's `tur_tb_*` and `tur_handler_chain` are not swapped
per fiber, so a fiber that yields inside them is already out of bounds.
`tur_panicking` is read after nearly every call in CPS code. It stays
direct: routing it through the accessor too cost 24% on the benchmark
below, against 4% without it.

## Cost

Only clang builds of the whole-preamble path change. Split-runtime builds
already reached every one of these through the host accessors. The
benchmark was a `#lang r7rs` program of `fib 27`, 300,000 `call/cc`
escapes, and 50 lists of 20,000 built and summed, compiled with clang 18
`-O2` on Linux, minimum of 30 runs:

| thread-locals | time |
| --- | --- |
| direct (before) | 158-173 ms |
| the fiber's own state through the accessor | 175-177 ms |
| this fix: that, and the allocator's read of the collector's record | 182-186 ms |
| every preamble thread-local through an accessor | 205-216 ms |

## Verified

Linux, clang 18: the fixture 20 of 20, 10 of 10 under
`TUR_GC_TORTURE=31` with and without the prelude split, and the plain version
20 of 20. macOS: see the PR.
