# Multishot effect fixtures abort on Windows (Win32-Fiber ucontext shim)

**Severity:** medium -- 2 fixtures of ~2180. Windows only. Blocks nothing else;
plain (one-shot) effect handlers and generators work.

**Status:** open. Root cause narrowed but NOT identified.

## Symptom

`fh-multishot-value` and `multishot-effect-cont-kv-sugar` build cleanly, then die
immediately with no output:

```
$ ./build-win/tur.exe build tests/fixtures/fh-multishot-value/input.tur -o /tmp/m.exe
$ /tmp/m.exe
$ echo $?
127                       # bash's rendering
# true Windows exit code: 0xC0000409
```

`0xC0000409` is STATUS_STACK_BUFFER_OVERRUN, which is ALSO what UCRT's `abort()`
produces (it routes through `__fastfail`). It is an abort, not stack corruption.

## Where it aborts

With `-O1 -g -fno-inline` (see "Diagnosis notes" -- `-O2` misattributes the
frame, and `-O0` does not link):

```
#0  ucrtbase!abort ()
#1  tur_fiber_shim (...) at <generated>.c:1299
#2  tur_win_fiber_trampoline (param=0x691080) at <generated>.c:60
#3  ntdll!RtlUserFiberStart ()
```

Line 1299 is the "unreachable" guard the fiber shim places after its final
context switch:

```c
    f->done = 1;
    if (task_group) tur_task_group_notify_done(task_group);
    swapcontext(&f->ctx, &f->caller_ctx);   /* 1298 -- must never return */
    abort();                                /* 1299 */
```

So the fiber ran past a `swapcontext` that, under POSIX ucontext, never returns.

## What has been RULED OUT

Each of these was a plausible theory that the evidence killed. Recording them so
the next person does not re-walk them:

1. **"Win32 Fibers cannot be copied, so multishot cannot rewind a stack."**
   Wrong. The generated code never snapshots `f->stack` -- multishot is handled
   by `tur_cloneable_cont`, which clones a CPS *closure env*, not a stack.
2. **"The fiber is resumed after it completed."** Wrong. A breakpoint on
   `tur_fiber_block_resume` shows three resumes, all with `f->done == 0`.
3. **"`swapcontext` took its `to->fiber == NULL` early-out and returned -1."**
   Wrong. That path now aborts with a message; it never fires.
4. **"Self-switch -- `SwitchToFiber` returns immediately when the target is the
   current fiber."** Wrong. Tracing every swap shows the handles alternate
   cleanly between main (`0x761850`) and the fiber (`0x761db0`); `from == to`
   is never true.

## The actual puzzle

Tracing every `tur_win_swapcontext` call shows exactly **five** switches:

```
nil       -> 0x761db0     (main -> fiber, first entry)
0x761db0  -> 0x761850     (fiber -> main)
0x761850  -> 0x761db0     (main -> fiber)
0x761db0  -> 0x761850     (fiber -> main)
0x761850  -> 0x761db0     (main -> fiber)
<abort>
```

There is **no sixth switch** -- yet reaching `abort()` at 1299 requires the
`swapcontext` at 1298 to have executed and returned. Something re-enters the
fiber, or resumes it past 1298, by a path that is not `tur_win_swapcontext`.

That is the thread to pull. Candidates not yet checked:

- The `setjmp(f->panic_jmpbuf)` / `longjmp` pair inside `tur_fiber_shim`.
  setjmp/longjmp across a Win32 fiber boundary is not obviously safe: the jmp_buf
  records a stack pointer belonging to one fiber's stack, and Windows' unwinder
  is SEH-based. A longjmp landing on the wrong fiber's stack could resume
  execution at 1299 without any SwitchToFiber.
- Whether the effect-handler dispatch re-enters via a second FiberBlock whose
  handle happens not to be traced.

## Diagnosis notes (for whoever picks this up)

- `-O2` inlines `tur_win_swapcontext`, so a breakpoint on it *undercounts*
  switches, and frame attribution is misleading. Use `-fno-inline`.
- `-O0` does **not** link: undefined references to `tur_get_contract_handler` /
  `tur_set_contract_handler`. Those are declared but never defined in the emitted
  C; at `-O1`+ the calls are eliminated so the link succeeds. That is a separate
  latent codegen bug worth its own report.
- Build with debug info via:
  `TUR_CC_FLAGS="-O1 -g -fno-inline -std=c99 -fno-strict-aliasing" tur build ...`

## Relevant code

- `src/compiler/emit_module.c` -- `emit_win_ucontext_shim()` emits the
  Win32-Fiber ucontext implementation into the generated C.
- `src/platform_ucontext_win.h` -- the same implementation for the interpreter
  (which only ever calls `makecontext` with argc == 0; the generated code uses
  argc == 2).

A correct fix may require the real Windows x64 context switch that WIN3 defers
(`fiber_ctx_x64_win.asm`) rather than Win32 Fibers, since a hand-rolled context
switch stores a register snapshot -- the same thing ucontext does -- whereas a
Win32 fiber is an opaque OS-owned execution unit.

## Related: cross-thread fiber migration (`scheduler-multithread`)

Same root cause, different symptom. `scheduler-multithread` runs two fibers on a
2-thread scheduler and expects each to print a distinct thread id. On Windows it
is **nondeterministic**: it often prints the same id twice (`1 1` / `2 2`), and
under the parallel suite's CPU load it occasionally **hangs** (exit 124). It
"passes" only when the scheduler happens to place the fibers so their ids differ.

The cause is that **Win32 Fibers are thread-affine**: a fiber created (via
`CreateFiber`) on thread A cannot be resumed with `SwitchToFiber` on thread B --
that is undefined and can deadlock. A multithread scheduler migrates fibers
across worker threads, which the ucontext model (a portable register snapshot)
supports and Win32 Fibers do not.

This is NOT a regression from the WIN1 fixture work -- the fixture's emitted C is
byte-identical before and after it -- it is the same Win32-Fiber limitation as
the multishot abort above, and it lands in the same place: WIN3's real x64
context switch, which is thread-agnostic and re-entrant, is the actual fix. Until
then, turmeric-side multithread scheduling is unsupported on Windows (consistent
with the `io_iocp.c` async stub).
