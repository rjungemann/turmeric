# The DK runtime's reap list and entry depth are shared by every thread

**Severity:** medium. A compiled program that runs CPS code on more than one
thread at once (a Scheme program whose worker threads call back into it, a
Turmeric program using effects from a worker) races on unsynchronized
globals. The possible results are a lost registration (a leak), a
registration freed by hand while its own thread still uses it, or a write
into an array another thread just reallocated. The last two are memory
corruption, and can look like a hang.

Filed 2026-09-28 from PR rjungemann/turmeric#956's CI.

## Repro

No deterministic repro. What was seen: on #956, "Auxiliary suites
(macos-latest)", `run-r7rs-gc.sh` timed out on `r7rs-threads-lifecycle`
(`FAIL r7rs-threads-lifecycle -- timed out (>300s) under
TUR_GC_TORTURE=31`). The same emitted program had passed that job on the
commit before, and 56 runs under torture on a 4-core Linux box, 48 of them
at twice the core count, never hung. The fixture loop prints no stack dump at
its deadline (only the dedicated `threads-lifecycle` case does), so the
cause of that hang is not known. It may be this report, or another
macOS-only collector stall like the two already found in this fixture
([r7rs-gc-threads-lifecycle-macos-timeout](../archive/r7rs-gc-threads-lifecycle-macos-timeout.md),
[r7rs-gc-threads-lifecycle-rare-hang](../archive/r7rs-gc-threads-lifecycle-rare-hang.md)).
The race below is certain by inspection.

The fixture's shape is the one that races. Its keeper, detached and child
workers are each a CPS entry (`life__life_hykeeper` and the rest, in the
emitted C), and they run while the main thread is inside its own top-level
form, which is a CPS entry too (`r7rs-run-toplevel-thunk`).

## Root cause

`src/compiler/emit_dk_runtime.c:753-756` emits the reap list as plain
process globals:

```c
static void **__dk_reap_v = NULL;
static unsigned char *__dk_reap_kind = NULL;
static size_t __dk_reap_n = 0, __dk_reap_cap = 0;
static int __dk_entry_depth = 0;
```

Every CPS entry wrapper (`emit_cps_ir.c:10169`, `:10264`) does
`__dk_entry_depth++` on entry and, on exit,
`if (--__dk_entry_depth == 0) __dk_reap_run(); else __dk_reap_drop_to(mark);`.
With two threads in CPS entries at once:

- `__dk_entry_depth++`/`--` are not atomic. A lost update can let one
  thread's exit see 0 while another thread is still inside an entry.
- Then `__dk_reap_run` (`emit_dk_runtime.c:777`) frees **every** thread's
  registrations by hand, including chains another thread's `dk_run` is
  still walking. Even with an exact count, the depth is global. So
  "outermost" means "the last thread to leave", not "this thread's
  outermost entry", and a registration can outlive its own thread's use or
  be freed under it.
- `__dk_reap_push` reallocs the array with no lock. Two pushes can realloc at
  once, or one thread can store into the block another just freed. Under
  the r7rs collector, realloc and free are `tur_gc_realloc`/`tur_gc_free`, so
  the stale block is heap memory that may already hold another object.

`__dk_reap_drop_to` (from #956) truncates the list to the exiting entry's
mark. That is only meaningful for one thread, so #956 disables it once the
program starts a thread (`tur_gc_threaded` in `src/runtime/r7gc.c`, set in
`tur_gc_pthread_create`). A threaded program is back to exactly what main
did. The races above predate it.

## Fix directions

- **Per-thread bookkeeping (the real fix).** Make the four globals
  `TUR_THREAD_LOCAL`. Each thread then has its own depth and its own list,
  "outermost" means this thread's outermost entry, and the drop can stay
  on in threaded programs. Two things come with it:
  - under the r7rs collector the list's array must stay a root, so the
    variables join the TLS roots (`emit_rt_tls` / `r7gc_note_tls_root` in
    `emit_module.c` do both);
  - under the JIT, where MIR has no TLS, they need host accessors in
    `src/runtime/tur_tls.c`, like the other runtime thread-locals.

  The regenerated preamble touches every snapshot.
- **Stopgap.** Atomics for the depth and a lock around the list would stop
  the corruption. They would not fix the global notion of "outermost", so
  a worker's exit could still reap a chain the main thread is using.
- **Harness.** Have the `run-r7rs-gc.sh` fixture loop dump stacks at its
  deadline, as `run_deadline` already does for the dedicated cases, so the
  next macOS sighting says what hung.
