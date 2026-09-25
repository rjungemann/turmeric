# Threads under the r7rs-gc collector: a collector lock first, a Boehm-style collector after

Status: **proposed** (2026-09-25). Follows
[docs/archive/r7rs-gc-plan.md](../archive/r7rs-gc-plan.md), whose collector
graduated the same day with one limit left standing: a compiled `#lang r7rs`
program that starts a thread stops at the start site (exit 70) and is told to
build with `TUR_R7RS_GC=0`. This plan removes that limit in stages, each of
which ships on its own, and ends at a collector that runs threads in
parallel the way Boehm's does.

## 0. The shape of the plan

| stage | what a Scheme program gets | what it costs | the piece it leaves behind |
|---|---|---|---|
| A. collector lock | threads, through every stdlib primitive, with memory reclaimed | one thread of the unit's code runs at a time (Python's GIL) | the thread registry, per-thread root blocks, the release points |
| B. stop the world | the lock is held only for collections; threads run in parallel between them | a pause mechanism per platform; an allocator lock on the slow path | the pause, the per-thread allocation caches |
| C. parallel-safe heap | no allocator lock on the fast path; large objects, `free`, `realloc` and the chunk map safe under contention | a second look at every data structure in r7gc.c | -- |
| D. graduation | the refusal and the `TUR_R7RS_GC=0` advice for threads go; the four gates run threaded programs | -- | -- |

Stage A is the intermediate design and is worth shipping alone. Stages B and
C are the Boehm-style collector; B is the hard one. Each stage keeps the
previous stage's gates green and adds its own.

Out of scope throughout: the interpreter (`tur --interpret` keeps its
values for the life of the process by design), `tur jit` (the collector is
compiled to plain libc under `TUR_JIT_ENGINE`), `--shared` and project
builds (each unit would have its own heap), and Windows (no roots there
either).

## 1. What a second thread breaks today

The refusal exists because three separate things go wrong, and a fix has to
cover all three. A mutex "at the Scheme/Turmeric boundary" covers none of
them, because the boundary that matters is the allocator, not the language
seam: under the collector the whole translation unit's `malloc` is
redirected (`emit_r7rs_gc_macros`, emit_module.c), and so is the runtime
archive's (`src/runtime/rt_alloc.h`). A Turmeric thread that never touches a
Scheme value allocates from the collected heap on its first call.

1. **The heap is unlocked.** `tur_gc_alloc_small` pops a per-class free
   list, `tur_gc_maybe_collect` reads and writes the allocation counter, and
   a collection rebuilds every free list (`tur_gc_collect_now`). Two threads
   allocating at once corrupt the lists.
2. **Only one stack is a root.** `tur_gc_mark_roots` spills the calling
   thread's registers into a `jmp_buf` on its own frame and scans from there
   to `G->stack_base`, the base recorded at init for the thread that
   initialized the collector. A pointer that lives only on another thread's
   stack, or in its registers, is invisible: its object is freed underneath
   it.
3. **Per-thread runtime state is shared.** The collector cannot scan
   thread-local storage, so under it the emitter defines `TUR_THREAD_LOCAL`
   as nothing (emit_module.c, `r7rs_gc_active`), and the eleven per-thread
   variables of the emitted runtime become plain statics in the data segment
   the collector does scan: `tur_handler_chain`, `__stm_current_tx`,
   `tur_current_fiber`, `tur_current_thread_state`,
   `tur_current_scheduler_mt`, `tur_cur_shift_reset`, `tur_cancel_jmpbuf`
   and its valid flag, `tur_fiber_cancelled_flag`, `tur_panicking`, and the
   backtracking root (`TUR_TB_TLS`). A second thread reads and writes the
   first thread's handler chain and transaction.

The prelude's own `_Thread_local` variables (`r7k_base_tls`, `r7k_form_base`
in stdlib/r7rs/prelude.tur) are real thread-locals even under the collector,
because they hold stack addresses, not heap pointers, and need no scanning.

## 2. Stage A: the collector lock

**One lock that a thread must hold to run any code of the unit.** While it
is held, the collector's world is what it is today: one running thread, one
heap owner. Every other thread is parked at a known point, which is what
makes the other two problems tractable.

### 2.1 The lock

A `pthread_mutex_t` in `tur_gc_state`, `G->world`. A thread holds it from the
moment it enters the unit's code until it blocks or exits. The program's
initial thread takes it in `tur_gc_ctor` (constructor 101, before any other
constructor in the unit) and never releases it except at a release point.

Not a spin lock and not a fair lock: parked threads wait on the mutex, and
the order they wake in is the OS's. Fairness is stage B's problem, when the
lock stops being the thing threads spend their time on.

### 2.2 The thread registry

`tur_gc_pthread_create` stops refusing and becomes a wrapper. It allocates a
`tur_gc_thread` record from `tur_gc_meta` (mmap'd metadata, never scanned
as data, never collected) and starts the OS thread on a trampoline:

```c
typedef struct tur_gc_thread {
    struct tur_gc_thread *next;     /* the registry, under G->world */
    pthread_t             tid;
    unsigned char        *stack_base;   /* pthread_getattr_np / pthread_get_stackaddr_np, taken ON the thread */
    unsigned char        *stack_sp;     /* the thread's stack pointer at its last release point */
    jmp_buf               regs;         /* its callee-saved registers, spilled at that point */
    void                **tls_roots;    /* the addresses of its TUR_THREAD_LOCAL variables (2.4) */
    size_t                n_tls_roots;
    bool                  parked;       /* at a release point (its stack_sp and regs are current) */
    void *(*fn)(void *); void *arg;     /* what the program asked for */
} tur_gc_thread;
```

The trampoline records the stack base (the same two calls `r7k_stack_base`
makes in the prelude, for glibc and macOS), links the record into
`G->threads`, takes `G->world`, runs `fn(arg)`, then unlinks under the lock,
releases it and returns `fn`'s result. `pthread_join` needs no wrapper: the
joiner is at a release point (2.3) while it waits.

The record is the thread's identity for the collector, kept in a real
`_Thread_local` pointer (`tur_gc_self`) so a release point can find it
without a search.

### 2.3 Release points

A thread that is about to block releases `G->world` first, and takes it back
before it touches the unit again. The pair is one function each way:

```c
static void tur_gc_park(void)   { spill regs and sp into tur_gc_self; parked = true;  unlock(G->world); }
static void tur_gc_unpark(void) { lock(G->world); parked = false; }
```

The spill is `setjmp(self->regs)` plus the address of a local, exactly the
two lines `tur_gc_mark_roots` runs for the collecting thread today, so a
parked thread's roots are its `regs` plus `[stack_sp, stack_base)`.

The release points are every place the unit blocks. They are a fixed, known
set, all in code this repository owns:

- stdlib/thread.tur: `thread-join`.
- stdlib/mutex.tur: `mutex-lock` (the try variant does not block).
- stdlib/chan.tur: `chan-send`, `chan-recv` (the try and async variants do
  not block, or block through the scheduler below).
- stdlib/future.tur: `future-join` and the condition waits and `nanosleep`s
  behind it (12 waits, 4 sleeps).
- stdlib/threadpool.tur: the worker's condition wait and the pool's joins.
- stdlib/taskgroup.tur: `task-group-wait`, `task-group-join`,
  `join-timeout-thread`, the timeout thread's sleep.
- stdlib/session.tur: the join.
- stdlib/httpd.tur: `accept`, `recv`, `read`, the worker's condition wait
  and join.
- stdlib/reactor.tur: the `kqueue`/`epoll` wait.
- The emitted multi-threaded scheduler (emit_module.c, the `tur_scheduler_mt`
  worker loop, 18 blocking calls between condition waits, joins and sleeps)
  and `spawn-conveying`'s trampoline.
- Blocking file and socket reads elsewhere in the stdlib (`read-line` on a
  pipe, `sleep`): audited in the same pass. A read that returns at once from
  a file is not worth a release; one that can wait on a peer is.

The rule that makes the set complete is mechanical: **any `pthread_join`,
`pthread_cond_wait`, `nanosleep`, `poll`, `accept`, `recv` or blocking
`read` in the unit is wrapped as `tur_gc_park(); call; tur_gc_unpark();`**,
and a grep for those names is the check (a CI lint, the way
`no-dev-server-port.sh` checks a pattern). Under `TUR_R7RS_GC=0` and on a
Turmeric entry file, `tur_gc_park`/`unpark` are `((void)0)`, so the stdlib
text compiles unchanged on both arms, the same way `TUR_REGION_NOTE` does.

A thread that blocks while holding the lock deadlocks every other thread.
That is a bug in the release-point set, and the gate's threaded fixtures
(2.7) are how it shows. A held lock across a long compute is not a bug; it
is the design's cost.

Inline C that a thread runs while parked -- a callback from a library
thread into the unit -- has to `tur_gc_unpark()` first. That is Python's
C-API rule, and the one place a user's inline C has an obligation. The
reactor's callbacks run on the reactor's own thread, which is registered
and holds the lock while it dispatches.

### 2.4 Roots: every thread's stack, and thread-local storage back

`tur_gc_mark_roots` walks `G->threads`: the collecting thread scans itself
as today; each other thread is parked (it must be, since the collector holds
the lock), so its `regs` and `[stack_sp, stack_base)` are scanned. The
registry is metadata memory, not scanned as data, and the collector never
frees a record while its thread lives.

With every thread parked at a known point, `TUR_THREAD_LOCAL` can go back to
`_Thread_local`. The collector still cannot scan thread-local storage, so
each thread registers the addresses of its eleven variables in its
`tls_roots` when it starts. The emitter already knows the set (`emit_rt_tls`
writes each one); it gains one function, `tur_rt_register_tls_roots(void
**out)`, that the trampoline calls on the new thread and `tur_gc_ctor` calls
on the first. A collection scans each thread's `tls_roots` as words. The
`TUR_TB_TLS` backtracking root is in the set; the trail's arrays stay on
libc as today (its cells are reached from the root).

`G->stack_base` becomes the initial thread's registry record.

### 2.5 What does not change

- The heap, the size classes, the chunk map, marking, sweeping and the
  threshold: single-owner as today, because the lock guarantees the owner.
- The runtime archive's hook: its allocations run under the lock, since the
  caller holds it.
- Continuation images (T5): a `call/cc` image is a heap object holding a copy
  of one thread's stack; nothing about it is per-thread. A continuation
  invoked on a different thread than captured it is already an error in the
  prelude's model (the image is copied back over the invoking thread's
  stack) and stays one; a fixture pins the refusal.
- Fibers: a fiber's stack is a heap allocation, scanned when its block is
  reachable, and `swapcontext` never crosses a release point.
- `tur_dk_pinned` and the DK reap: global, under the lock, as today.

### 2.6 The escape hatch stays

`TUR_R7RS_GC=0` / `--no-r7rs-gc` keep meaning "libc, no collector, real
parallelism": a threaded program that needs cores rather than reclamation
builds that way. Stage A changes the advice in the refusal's place to a
warning at the first thread start (TUR-W0071, once): "this program's threads
run one at a time under the collector; build with TUR_R7RS_GC=0 for
parallelism". The warning is the honest statement of the cost and goes away
in stage B.

### 2.7 Gate

`tests/run-r7rs-gc.sh` section 3 flips from "a thread start is refused" to:

- **threads-run**: the existing threaded fixture (a C thread started through
  `(turmeric spawner)`) starts, joins and prints, under the collector, with
  `TUR_GC_TORTURE=1`.
- **threads-share**: a Scheme program builds a list on the main thread,
  hands it to a Turmeric thread through a channel, which walks it and sends
  a sum back; a collection on every allocation, and both directions.
- **threads-roots**: a thread holds the only pointer to a large list on its
  own stack across a `chan-recv` while the main thread churns garbage
  through a hundred collections; the list is intact after.
- **threads-tls**: two threads each install an effect handler and raise
  through it; each sees its own chain (the thing 1.3 breaks today).
- **threads-deadlock-lint**: the grep over the blocking calls finds every
  one wrapped.
- Every `#lang r7rs` fixture still passes under torture (section 1), and the
  reclamation check (section 4) still fits.

Plus `tests/fixtures/r7rs-threads-*` for the ordinary suite, so the shape
runs on every push, not only the gate.

### 2.8 Sizing

r7gc.c grows by the registry, park/unpark and the roots walk (about 150
lines); the emitter by the TLS-root registration and the `TUR_THREAD_LOCAL`
switch (about 40); the stdlib by the wrapped blocking calls (about 60
sites, one line each, behind a macro); the gate by four cases. The
release-point audit is the real work. One to two days.

## 3. Stage B: stop the world without the lock

The goal is to hold `G->world` only during a collection. Between
collections threads run in parallel, and a thread that wants to collect has
to pause the others itself. Two pieces:

### 3.1 The pause

Boehm's answer, and the recommended one here, is **signals**: the collecting
thread sends each registered thread a real-time signal (`SIGPWR` /
`SIGXCPU` on glibc; Boehm's choice), whose handler spills registers and the
stack pointer into the thread's record (the same `tur_gc_park` spill), posts
a semaphore, and blocks on a second semaphore until the collection ends. On
macOS signals into a Mach thread are less reliable and Boehm uses
`thread_suspend` + `thread_get_state` instead; the record's `regs` becomes
the state the kernel hands back. Either way a thread can be stopped anywhere,
which is what a conservative collector wants: no polls in loops, no emitter
change.

The alternative, **safepoints**, would have the emitter insert a flag check
on every backward branch and every call. It is what precise collectors do
and it is not needed here: conservative scanning does not care where a
thread stopped. Stage A's release points stay, but as "already parked, skip
the signal" fast paths rather than the only pause.

A thread parked at a release point when the signal arrives is fine: the
handler runs on its stack, spills again, and the collector scans from the
lower of the two stack pointers (the handler's frame is below the parked
frame).

### 3.2 The allocator under contention

With the lock gone from the fast path, `tur_gc_alloc_small` needs either a
lock or a per-thread cache. Per-thread caches (Boehm's thread-local free
lists) are the right answer: each thread record owns a small free list per
size class, refilled from the global lists under `G->heap`, a second mutex
distinct from `G->world`. Allocation touches no shared state until a refill.
The allocation counter that drives `tur_gc_maybe_collect` becomes per-thread
and is summed at refill time.

A thread that wants to collect takes `G->world` (excluding a second
collector), pauses the others (3.1), waits for every pause to be
acknowledged, marks, sweeps, releases the paused threads, drops the lock.
Marking is single-threaded in this stage.

A sweep that rebuilds the per-class free lists must also invalidate every
thread's cache: the cached slots are either marked (the thread will hand
them out again) or not (they go back to the global list). The simplest rule
is to empty every cache at the start of a collection and let the threads
refill.

### 3.3 Gate

The stage A gate, plus:

- **threads-parallel**: two threads compute independently for a second;
  wall time is under 1.5x one thread's (the lock is gone from the run).
- **threads-pause**: a thread in a tight allocation loop is paused by a
  collection the other thread triggers, a thousand times, under
  `TUR_GC_TORTURE`.
- **threads-signal-in-syscall**: a thread blocked in `accept` is paused and
  resumed; the accept completes after (EINTR handling in the release
  points).
- The ASan and UBSan runs of the threaded fixtures (`run-r7rs-sanitize.sh`),
  because a stop-the-world pause is exactly where a sanitizer finds a
  handler that touched something it should not have.

### 3.4 Sizing

The pause is per-platform C in r7gc.c (about 200 lines for Linux, 150 for
macOS, the way the roots already split), the caches about 150, the gate
three cases. The EINTR audit of the release points is the tedious part.
Three to four days, most of it making the pause reliable.

## 4. Stage C: a parallel-safe heap

Stage B leaves the slow paths under `G->heap`. This stage reviews every
structure in r7gc.c for what a second thread can do to it while the lock is
not held:

- **The chunk map** (`tur_gc_map_put`/`get`): readers during marking are
  the collector only (paused world). Writers are `tur_gc_new_small` and
  `tur_gc_alloc_large` under `G->heap`. A reader on the free path
  (`tur_gc_free`, `tur_gc_realloc`) races a `tur_gc_map_grow`: either take
  `G->heap` on those paths (they are rare in a Scheme program, which never
  frees) or make the map grow copy-on-write so a stale reader sees the old
  table. Take the lock.
- **`tur_gc_free` and `tur_gc_realloc`**: under `G->heap`. The prelude
  frees scratch and the archive frees HAMT nodes on `map-dissoc`; neither
  is a fast path.
- **Large objects**: `tur_gc_alloc_large` mmaps under `G->heap`;
  `tur_gc_release_large` runs in the sweep, world stopped.
- **The mark stack**: single collector, unchanged. Parallel marking (Boehm's
  work-stealing markers) is a later option, not part of this plan: a Scheme
  program's live set is small, and re-marking it is the 1.2x-1.8x the
  original plan measured, not a bottleneck.
- **`G->since` and `G->threshold`**: per-thread counters summed at refill
  (3.2); the threshold is read under `G->heap`.
- **The region generations** (`tur_region_each_used`): a region bracket is
  per-thread today (regions are thread-local generations); the collector
  walks every thread's generations, which the region runtime exposes with a
  per-thread walker. Check that the walker is safe against a thread that is
  paused mid-`with-region`.

### 4.1 Gate

The stage B gate plus a stress fixture: eight threads allocating,
`map-assoc`ing and `map-dissoc`ing a shared Turmeric persistent map through
a mutex while a ninth churns garbage, under `TUR_GC_TORTURE=31`, ten
seconds, under ASan. The number is chosen to be more threads than cores on
the CI runners, so preemption inside every slow path is exercised.

### 4.2 Sizing

One to two days, mostly review and the stress fixture.

## 5. Stage D: graduation

- The refusal is gone since stage A; the TUR-W0071 warning goes in stage B.
- `docs/guides/r7rs-guide.md` Memory section: the Threads bullet becomes
  "threads are supported; one runs at a time" (A) and then just "threads
  are supported" (B). `TUR_R7RS_GC=0` stays documented as the no-collector
  build.
- `tests/run-r7rs-gc.sh` keeps every case; the CMake `tur_r7rs_gc` target
  keeps its 720 s timeout, which the threaded cases fit within by
  construction (each is bounded by a torture count, not a wall clock).
- CHANGELOG entries per stage.

## 6. Risks and open questions

- **A release point missed** (stage A) is a deadlock, found only by a
  program that blocks there. The lint (2.3) and the threaded fixtures make
  it rare; a `TUR_GC_DEBUG_LOCK=1` build that aborts when the lock is held
  across any of the listed calls would make it loud. Worth building in
  stage A.
- **Signals and libc** (stage B): a thread paused inside `malloc` of *libc*
  (the trail, `FILE` buffers) holds libc's lock; the collector never calls
  libc's allocator during a collection (all its metadata is mmap'd), so
  there is no deadlock, but the audit has to confirm no `printf` on the
  collection path (`tur_gc_report` runs at exit, not during).
- **macOS pause**: `thread_suspend` on a thread inside a Mach syscall is
  documented and used by Boehm; the state read has to be tried on CI's
  `macos-latest` early, the way the roots were, since there is no macOS box
  here.
- **Stack-image continuations across threads**: unsupported before and
  after; the refusal at invocation time needs a fixture (2.5).
- **Fibers migrating between scheduler threads** (`tur_scheduler_mt`): a
  fiber's stack is a heap object and its registers are spilled by
  `swapcontext` into the fiber block, so a fiber parked on one thread and
  resumed on another has its roots reachable either way. Confirm with a
  fixture under torture rather than by argument.
- **Should stage A ship at all, or go straight to B?** Stage A is a day or
  two and gives every threaded stdlib library to Scheme programs with memory
  reclaimed; B is the better part of a week and its pause is the risky
  piece. A stays worth shipping first: its registry, root blocks and release
  points are exactly what B needs, and a program that blocks on I/O keeps
  its concurrency under A.

## 7. Order of work

1. Stage A: registry and trampoline; park/unpark and the release-point
   audit with the lint; TLS roots and `TUR_THREAD_LOCAL` back to real TLS;
   the four gate cases; the warning. Ship.
2. Stage B on Linux: per-thread caches, the signal pause, EINTR audit,
   the three gate cases. Then macOS via CI. Ship.
3. Stage C: the slow-path review and the stress fixture. Ship.
4. Stage D: docs and the warning's removal, in C's PR.
