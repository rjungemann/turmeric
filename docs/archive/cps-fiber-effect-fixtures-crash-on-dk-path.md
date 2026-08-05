# fiber-effect / p19-8-fiber-effect-chain crash when their own body CPS-lowers

**STATUS: RESOLVED (2026-07-19).** `tur_fiber_block_resume` now saves and restores
`g_dk_driver` + the DK meta-stack depth across its `swapcontext` (emitted only
under the trampoline path), so a DK handle that yields mid-flight inside a
coroutine fiber can no longer leave the driver global pointing into the fiber's
(later freed) stack. fiber-effect -> 10/99, p19-8-fiber-effect-chain -> 20/30/99,
both flag-on and flag-off; suite 2203/0 flag-off byte-identical. The "Fix
direction" below (preserve the driver lifetime across the fiber boundary) is what
landed -- the runtime save/restore, not the emit-side entry wrapper.

**Severity:** high (blocks CPS/DK flag graduation -- the fixtures that fiber-runtime
deletion would force permanently onto the DK path crash on it). Experimental
`--enable=cps-tramp-resume` path only; flag-off both fixtures pass.

## Symptom

Forcing the CPS/DK path on globally (default `g_opt_cps_tramp_resume = true`, or
building these two fixtures with `--enable=cps-tramp-resume`) makes both fiber
effect fixtures crash at runtime instead of printing their expected output:

```sh
CC=cc ./build/tur --enable=cps-tramp-resume build tests/fixtures/fiber-effect/input.tur -o /tmp/fx
/tmp/fx                       # => Segmentation fault (exit 139); expected: 10 / 99

CC=cc ./build/tur --enable=cps-tramp-resume build tests/fixtures/p19-8-fiber-effect-chain/input.tur -o /tmp/fx
/tmp/fx
# => *** longjmp causes uninitialized stack frame ***: terminated  (exit 134)
#    expected: 20 / 30 / 99
```

These are the *fiber-effect* fixtures themselves. Under the fiber-live sweep they
already emit `tur_effect_perform` = 0 (they DK-lower), but the DK lowering of
their bodies is not runtime-correct.

## Root cause (pinned to symptom, not yet to file:line)

`p19-8-fiber-effect-chain`'s abort message -- `longjmp causes uninitialized stack
frame` -- is glibc's `_FORTIFY` check firing when the E7 tail-resume trampoline
`longjmp(*g_dk_driver, ...)` targets a `jmp_buf` whose setjmp frame has already
returned. So the driver `setjmp` boundary is not live on the stack at the point
`dk_perform` yields for these fixtures' handler shape (a chained/multi-effect
handler where the resumed continuation unwinds past the entry driver frame).
`fiber-effect`'s bare segfault is the same class one step earlier (jump into
freed/returned frame -> corrupted resume).

Contrast: `cps-backend-effect` / `-option-effect` / `-struct-effect` tail-resume
correctly on this same trampoline, so the bug is specific to the handler/effect
*shape* these two fixtures use (effect chain across multiple performs), not the
trampoline in general.

## Fix direction

Trace the driver `setjmp` / `g_dk_driver` lifetime for a chained-effect handle:
the entry `setjmp` boundary must remain live for the whole duration that any
suspended continuation can tail-resume through it. Candidate: the synthesized
main / handle-body entry establishes the driver, but a nested or chained handle
re-enters `dk_perform` after the outer driver frame has returned, leaving
`*g_dk_driver` stale. Compare the emitted entry/driver scaffolding for
`cps-backend-effect` (works) vs `p19-8-fiber-effect-chain` (aborts) to find where
the second effect's resume loses its live setjmp target.

## Why it matters

Deleting the fiber effect runtime (the CPS/DK endgame's last step) forces every
effect program onto this path. These two are the first that crash on it, so they
gate graduation regardless of the separate generic-code `__cps` ABI build
failures and the 104-byte tail-resume leak. See
`docs/upcoming/v2/cps-dk-endgame-remaining-plan.md` Sec 3a for the full flag-on
suite measurement (2047 passed / 156 failed forced-on).
