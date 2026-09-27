# `run-r7rs-gc.sh`: `threads-lifecycle` hung once under `TUR_GC_TORTURE=31`

**RESOLVED 2026-09-27.** Root-caused on its second sighting (PR #947's
macOS Auxiliary suites job) and fixed in src/runtime/r7gc.c. The rest of this
file is the original report, kept as the record of what did not reproduce it.

## Root cause

A self-deadlock on the collector's `world` lock, in code the fixture itself
runs. `life_extra_records` (tests/fixtures/r7rs-threads-lifecycle/life.tur)
counts the registry under `pthread_mutex_lock(&tur_gc_G->world)`. After r7gc.c
is pasted ahead of the unit, that spelling is the release-point wrapper
`tur_gc_mutex_lock`. When the lock was contended (a detached thread retiring
its own record in `tur_gc_thread_gone` held it at that instant), the wrapper
parked, took `world`, then unparked -- and `tur_gc_unpark` takes `world` too.
The thread waited on a lock it held, with every other thread gone. gdb on the
hung process: the only thread left, in `tur_gc_unpark` ->
`__lll_lock_wait`, called from the fixture's top-level form.

It was not the branch's doing: a program built by `main`'s compiler hangs the
same way. It is rare because the window is one retirement: about one run in
600 under 12-way contention on 4 cores, none in 3000 runs of `main`'s build.
Twenty rounds of the fixture's detach phase in one run hit it every time,
with `main`'s runtime and this branch's alike.

## Fix

`tur_gc_mutex_lock` takes the collector's own locks (`world`, `heap`,
`meta_lock`) directly, without the park: parking around `world` waited on
itself, and parking around `heap` or `meta_lock` would take `world` under
them, against the collector's lock order. Unparked, the wait is a stop point
like the collector's other lock waits: a collection signals the thread and it
answers.

The fixture's detach phase now runs twenty rounds of forty threads (13 s
under `TUR_GC_TORTURE=31`); it hangs on every run without the fix.


**Severity:** low, unconfirmed. Seen once and not reproduced since. Recorded
so a second sighting has somewhere to land, rather than being taken for a
fresh problem.

## What was seen

2026-09-26, Linux, Debug build at the head of branch
`claude/srfi-r7rs-support-yt5qoi`. One run of `bash tests/run-r7rs-gc.sh`
reported:

```
FAIL threads-lifecycle -- timed out (>300s) under TUR_GC_TORTURE=31 (a missing root can read as a hang: a freed list walked in a cycle)
```

Nothing else was running on the box. The run before it and the run after it
both passed that case, and the program normally finishes in about 3 s.

## What did not reproduce it

The same binary (`tur build tests/fixtures/r7rs-threads-lifecycle/input.tur`)
under `TUR_GC_TORTURE=31` completed correctly:

- 40 times with its output redirected to a file;
- 20 times captured with `$(...)`, as the harness does, in case a forked
  child held the pipe open.

It never took more than 3 s.

## Why it is filed separately

The branch where it appeared changed only the Scheme front end (the reader
and scheme_lower.c: leading-colon identifiers, brackets, the Turmeric-form
refusal, and library macro export). The fixture uses none of those: it has no
leading-colon tokens, no brackets, no Turmeric forms, and no Scheme library
imports, so its lowering and emitted program are unchanged by the branch.

The fixture's own claims are the likely suspects:

- thread-local key values are roots;
- a `fork` in the middle of an allocation is safe;
- detached threads leave the registry.

Each is a place where a narrow race under torture could stall a stop-the-world
collection or leave a freed list to be walked in a cycle.

## If it recurs

- Keep the hung process alive and attach `gdb -p <pid>`, then run
  `thread apply all bt`. A hang in the collector's stop handshake (the signal
  wait in src/runtime/r7gc.c) and a cycle walk in the mark phase look very
  different.
- Note what else was running. docs/archive/ci-cps-tramp-turi-timeouts-under-load.md
  has the trap of mistaking load for a hang.
- A loop of several hundred runs under `TUR_GC_TORTURE=31` with a 30 s
  timeout is the cheapest way to get a rate.
