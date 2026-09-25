# A child forked from a multithreaded `tur jit` program can hang

**Severity:** low. Found while writing the r7rs-gc stage C fixtures.
`fork` from a threaded program run under `tur jit` is rare, and the
compiled build is unaffected.

## Repro

`tests/fixtures/r7rs-threads-lifecycle`, fork check (`life-forks` in
`life.tur`). One thread calls a Scheme procedure that allocates, in a loop,
while the main thread forks. Each child calls a Scheme procedure, then
`_exit`s. The fixture skips this check under `TUR_JIT_ENGINE`; delete that
`#if` branch to reproduce.

```
$ ./build-jit/tur jit tests/fixtures/r7rs-threads-lifecycle/input.tur
(fork-failures 1)      # the child: WIFSIGNALED, SIGALRM -- it hung until its alarm(5)
```

How the same fork check behaves elsewhere:

| Build | Result |
|---|---|
| `tur jit`, with the burner thread running | fails: a child hangs on its first run |
| `tur jit`, burner thread not started | passes |
| compiled, with the r7rs-gc collector | never fails (repeated runs) |
| compiled, `TUR_R7RS_GC=0` | never fails (repeated runs) |

## Root cause

Unconfirmed. The child inherits a lock that the other thread held at the
fork, and that lock is inside the JIT path. It is not the collector's: the
collector is compiled out under `TUR_JIT_ENGINE`, and its own locks are
fork-safe since stage C (`pthread_atfork`, src/runtime/r7gc.c). The prime
suspect is the engine's lazy code generation. The child is the first caller
of `child!` and `child-exit`, so it asks MIR to generate them, and MIR's
generator state may have been locked by the burner thread's own lazy
generation at the moment of the fork.

## Fix directions

- Confirm under gdb: attach to a hung child and read the stack of its one
  thread.
- If it is MIR's generator: register `pthread_atfork` handlers around the
  engine's generation lock, or generate eagerly before running a program
  that starts threads.
- Or document that a threaded program which forks and runs Turmeric code in
  the child should be compiled, not run under `tur jit`.
