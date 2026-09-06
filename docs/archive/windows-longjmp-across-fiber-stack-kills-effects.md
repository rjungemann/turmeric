# Windows: a `longjmp` on a fiber stack raises STATUS_BAD_STACK, so effects inside a fiber abort

> **RESOLVED 2026-08-06.** The DK tail-resume landing now uses
> `__builtin_setjmp`/`__builtin_longjmp` on Windows. `fiber-effect` and
> `p19-8-fiber-effect-chain` pass. See "Resolution" at the end -- including a
> correction to fix direction 1 below, which does NOT work, and the two sibling
> sites this did not cover
> ([windows-longjmp-remaining-fiber-sites.md](windows-longjmp-remaining-fiber-sites.md)).

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

1. ~~**Suppress the trampoline on a fiber stack.**~~ **DOES NOT WORK -- do not
   try this.** The idea was that `dk_tail_resume` already falls back to
   `dk_invoke` when there is no driver, so clearing `g_dk_driver` across
   `tur_fiber_block_resume` would make a fiber look driverless and no longjmp
   would happen. Two reasons it fails:

   - `tur_fiber_block_resume` **already** saves and restores `g_dk_driver`
     (emit_module.c, guarded on `g_opt_cps_tramp_resume`) -- the scaffolding
     this proposed is there; it just never *clears*.
   - Clearing it would not matter anyway. A fiber body is a direct->cps entry,
     so it installs its OWN landing (`jmp_buf __dkjb`) **on the fiber stack**
     and points `g_dk_driver` at that. Confirmed in the emitted C for
     `fiber-effect`: `fiber_hybody()` carries the full
     `setjmp(__dkjb) ... __dk_drive_after()` wrapper. So the fatal longjmp is
     fiber-stack -> fiber-stack, entirely inside the fiber, and no amount of
     boundary bookkeeping removes it.

   The mechanism is simply that **any** `longjmp` executed on a fiber stack
   dies, self-paired or not. That is what made direction 4 the answer.
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

4. **Don't use the OS unwinder.** GCC's `__builtin_setjmp` /
   `__builtin_longjmp` are a plain SP/FP/PC save-restore -- no `RtlUnwindEx`,
   no TEB check, no termination handlers. The DK trampoline never wanted an
   unwind; it wants a jump. **This is what was done.**

## Resolution (2026-08-06)

The trampoline's landing pad went behind `tur_dk_jmp_buf` / `TUR_DK_SETJMP` /
`TUR_DK_LONGJMP`, which resolve to the GCC builtins on Windows and to plain
`setjmp`/`longjmp` everywhere else. Touched: the DK runtime prelude
(`emit_dk_runtime.c`), the direct->cps entry wrappers (`emit_cps_ir.c`), and
the fiber-boundary save/restore (`emit_module.c`).

Measured first, on a fiber stack, using the compiler's own emitted ucontext
shim (scratch probes, not committed):

| | on a fiber stack |
| --- | --- |
| plain `setjmp`/`longjmp` | dies, `STATUS_BAD_STACK` |
| `__builtin_setjmp`/`__builtin_longjmp` | survives |

then stressed against the shapes the runtime actually produces -- 200 re-armed
hops through a `for(;;)` landing, throws 30-40 frames deep, a nested scoped
landing (`__dk_drive_bounded`'s shape) restoring the outer one, and a throw
after a `swapcontext` round trip. All pass.

### The S2 split constraint this exposed

The choice **cannot be a preprocessor test**. Under the split-runtime path the
runtime half is compiled by the host toolchain (GCC) and the program half by
c2mir, so `#if defined(__GNUC__)` resolves *differently in the two halves of
one program*: c2mir has no `__builtin_setjmp` (verified -- no such identifier
anywhere in c2mir) and does not define `__GNUC__`. Since setjmp/longjmp must
PAIR, the mismatch is silent -- the program runs to **exit 0 having printed
nothing**. That was a real regression, caught by running an effect program
through the JIT and diffing against a stashed build.

So the decision is made at emission and baked in as a literal
(`rt_split_canonical_emission()`), which makes both halves agree by
construction. Under the split, both keep plain `setjmp` -- so fibers-plus-
effects on the JIT path stay broken exactly as they were. Lifting that means

> **Correction 2026-09-05.** "Exactly as they were" was wrong. The JIT path was
> broken by a *different* SEH failure: `STATUS_BAD_FUNCTION_TABLE` (0xC00000FF)
> raised by `RtlUnwindEx` because MIR emits no `.pdata`/`.xdata`, so it cannot
> unwind through a JIT-generated frame -- not the `STATUS_BAD_STACK`
> (0xC0000028) this report is about, and not dependent on a fiber at all. The
> tell was there to be seen: fiber+effect fixtures PASSED on the JIT path while
> fiber-free effect fixtures failed. Fixed without touching c2mir, by calling a
> hand-written save/restore pair through a symbol both halves can name --
> src/async/tur_sjlj_x64_win.S, and
> docs/reported/jit-windows-support-spike.md "Resolution: the JIT longjmp".
teaching c2mir the builtins in the MIR fork.

Regenerating `src/runtime/generated/tur_rt_split*` is **mandatory** with this
change: the preamble text moved, so a stale blob fails the hash guard and
silently disengages S2 (observed -- `hello.tur` fell off the native JIT path
onto the cc fallback via `__va_start`).

### Verified

- `fiber-effect` -> `10 99`, `p19-8-fiber-effect-chain` -> `20 30 99`, exit 0
- JIT: `hello.tur` still native under `TUR_JIT_GEN=eager`; an effect program
  still prints `42`

### Not covered

`call/cc` and panic-in-fiber have the identical defect and were left alone --
see [windows-longjmp-remaining-fiber-sites.md](windows-longjmp-remaining-fiber-sites.md).

## Related

- [windows-longjmp-remaining-fiber-sites.md](windows-longjmp-remaining-fiber-sites.md) -- the sibling sites
- [windows-posix-inline-c-gaps.md](../reported/windows-posix-inline-c-gaps.md)
- [windows-subprocess-and-shared-lib-gaps.md](../reported/windows-subprocess-and-shared-lib-gaps.md)
- [docs/upcoming/v1/windows-remaining-plan.md](../upcoming/v1/windows-remaining-plan.md)
