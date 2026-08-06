# Windows: a `longjmp` on a fiber stack raises STATUS_BAD_STACK, so effects inside a fiber abort

**Summary:** On Windows x64, the DK runtime's tail-resume trampoline
(`dk_tail_resume` -> `longjmp`) is fatal when it executes on a fiber stack.
`longjmp` on win64 is a real SEH unwind, and the OS unwinder rejects a stack
it does not recognise. Any program that performs an effect inside a fiber
dies before producing output.

**Severity:** Medium. Two fixtures fail; the feature (effects x fibers) does
not work at all on Windows. Nothing silently miscompiles -- the process
aborts loudly -- and neither effects alone nor fibers alone are affected.

**Platform:** Windows only (win64 SEH). Linux and macOS are unaffected: their
`longjmp` is a register restore with no unwinder involvement.

## Repro

```sh
tur build tests/fixtures/fiber-effect/input.tur -o fe.exe
./fe.exe        # no output, exit 127
```

Also `tests/fixtures/p19-8-fiber-effect-chain`. Under `tests/run.sh` both
report `stdout mismatch` with an empty `actual.stdout`, which understates the
problem -- the program never ran, it was killed by the OS.

## Root cause

```
gdb: unknown target exception 0xc0000028
#0  ntdll!RtlRaiseStatus
#1  ntdll!RtlUnwindEx
#2  ntdll!RtlUnwind
#3  ucrtbase!.intrinsic_setjmpex
#4  dk_tail_resume.constprop
#5  fiber_hybody
#6  tur_fiber_shim
#7  __tur_uctx_run
#8  __tur_uctx_tramp
#9  0x0000000000000000 in ?? ()
```

`0xc0000028` is `STATUS_BAD_STACK` -- "an invalid or unaligned stack was
encountered during an unwind operation."

The mechanism is a platform difference in `longjmp`, not a bug in the fiber
code or in the DK runtime:

- On win64 there is no setjmp/longjmp register-pair. MinGW's `setjmp` maps to
  `_setjmpex`, and `longjmp` calls `RtlUnwindEx`, which walks the SEH
  unwind tables frame by frame from the longjmp site back to the setjmp
  frame, running termination handlers on the way.
- `RtlUnwindEx` validates each frame's RSP against the *current thread's*
  stack bounds, which it reads from the TEB (`StackBase` / `StackLimit`).
- A fiber stack is `malloc`'d by `tur_fiber_block_new` and entered by
  `__tur_uctx_tramp`, which does not (and cannot portably) update the TEB.
  So every frame on it is out of bounds as far as the unwinder is concerned,
  and frame #9's null return address is where the walk falls off the end.

`dk_tail_resume` ([src/compiler/emit_dk_runtime.c:691](../../src/compiler/emit_dk_runtime.c))
is the site that trips it:

```c
static intptr_t dk_tail_resume(DK *sub, intptr_t v) {
    if (!g_dk_driver) return dk_invoke(sub, v);   /* safe path */
    g_dk_resume_chain = sub; g_dk_resume_val = v;
    longjmp(*g_dk_driver, 1);                      /* fatal on a fiber stack */
    return 0;
}
```

Note the existing fallback: with no driver installed it resumes inline via
`dk_invoke` and never longjmps. That is why effects outside a fiber are fine,
and it is also a hint at the shape of a fix.

## Not caused by, and not fixable in, the Windows bring-up work

Verified across three builds, all Debug/UCRT64, same `build-pm` config so the
only variable is source:

| tree | result |
| --- | --- |
| `origin/main` (fb713d11a), no Windows work at all | exit 127, backtrace above |
| `windows-bringup` pre-merge (ef86b0d99) | exit 127 |
| the merge (bde3ff7a4) | exit 127 |

In particular this is **not** the COMDAT change to `emit_win_ucontext_shim`:
that alters section placement of the context-switch asm, and the fault is a
stack-bounds check inside the OS unwinder. Pure `origin/main` produces a
byte-identical backtrace.

## Fix directions

Roughly in increasing order of cost:

1. **Suppress the trampoline on a fiber stack.** `dk_tail_resume` already
   falls back to `dk_invoke` when there is no driver. If a fiber can be made
   to look driverless -- save/clear `g_dk_driver` across
   `tur_fiber_block_resume` and restore on yield -- the longjmp never
   happens and the inline path (documented as "byte-identical to the
   non-trampolined path") runs instead. Cheapest, and it degrades depth
   rather than correctness: deep recursion inside a fiber loses flattening.
2. **Tell the TEB about the fiber stack.** Set `StackBase`/`StackLimit` (and
   `DeallocationStack`) around the switch in `__tur_uctx_tramp` /
   `tur_fiber_shim`, so `RtlUnwindEx` accepts the frames. This is what
   Windows' own `CreateFiber` does. Correct in principle, but it means
   writing undocumented TEB offsets from generated code.
3. **Use real Win32 fibers** (`CreateFiber`/`SwitchToFiber`) on Windows
   instead of the hand-rolled ucontext shim. The OS then owns the stack and
   the TEB bookkeeping, and `longjmp` works. Largest change, and it retires
   `fiber_ctx_x64_win.S` plus the emitted asm shim -- worth weighing against
   how much Windows fiber use is actually planned.

Option 1 is the one to try first: it is a few lines, it is reversible, and it
tells us whether anything else on the fiber path also unwinds.

## Related

- [windows-posix-inline-c-gaps.md](windows-posix-inline-c-gaps.md)
- [windows-subprocess-and-shared-lib-gaps.md](windows-subprocess-and-shared-lib-gaps.md)
- [docs/upcoming/v1/windows-remaining-plan.md](../upcoming/v1/windows-remaining-plan.md)
