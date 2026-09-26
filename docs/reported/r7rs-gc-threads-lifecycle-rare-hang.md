# `run-r7rs-gc.sh`: `threads-lifecycle` hung once under `TUR_GC_TORTURE=31`

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
