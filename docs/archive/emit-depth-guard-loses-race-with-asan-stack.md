---
title: The emitter's expression-depth guard loses the race with the ASan stack limit
category: Reported
description: "EMIT_MAX_EXPR_DEPTH is a depth counter with no real-stack-headroom check, so on a Debug+ASan macOS/arm64 host the stack overflows before depth 40 and TUR-E0712 never prints. The macro-expansion guard had the identical bug and was fixed in 2026-08; the emitter never got the same treatment."
---

# The emitter's expression-depth guard loses the race with the ASan stack limit

**Severity:** low (Debug/ASan only; the diagnostic is correct, it just never
gets to print). One fixture red on affected hosts:
`errors/expr-nesting-depth-limit`.

**Status: FIXED 2026-09-09** -- by the fix direction below, verified on the
host the report was filed from (macOS/arm64, Debug + ASan, 8 MiB stack).

Reproduced first, so the fix has something to be a fix *of*: `tur emit-c` on
the fixture aborted with `AddressSanitizer: stack-overflow ... in
emit_value_dispatch`, exit 134, in the three-frame
`emit_value -> emit_value_dispatch -> emit_builtin` cycle the report names.

The helper is now shared rather than duplicated. `elab_approx_sp`,
`elab_stack_headroom` and `elab_stack_nearly_exhausted` -- which lived as
`static`s in `elab_call.c`, reachable only by the macro guard -- moved
verbatim into a new `src/compiler/stack_guard.{c,h}` as `tur_stack_headroom`
/ `tur_stack_nearly_exhausted`. `elab_call.c` includes the header and is
otherwise unchanged; `emit_value` gained the second trigger:

```c
bool emit_stack_low = g_emit_expr_depth >= EMIT_EXPR_DEPTH_STACK_FLOOR &&
                      g_emit_expr_depth < EMIT_MAX_EXPR_DEPTH &&
                      tur_stack_nearly_exhausted();
if (g_emit_expr_depth >= EMIT_MAX_EXPR_DEPTH || emit_stack_low) { ... }
```

`EMIT_EXPR_DEPTH_STACK_FLOOR` is 8 -- a fifth of the cap, playing the part
depth >= 16 plays in the macro guard. It is what keeps a legitimately shallow
expression on a deliberately tiny `ulimit -s` from tripping, and a runaway
passes it immediately. The deepest `emit_value` nesting anywhere in `stdlib/`,
`tests/fixtures/` and `examples/` is 20, so 8 is above trivial and well below
anything real.

`EMIT_MAX_EXPR_DEPTH` was deliberately **not** re-tuned. Both triggers were
then exercised, which is the part worth recording because it distinguishes
"the fix works" from "something else changed":

| stack | trigger | depth reached | headroom left |
| --- | --- | --- | --- |
| 8 MiB (default) | headroom | 32 | 954 KiB |
| 64 MiB (`ulimit -s 65520`) | counter | 40 | n/a, never low |

The 8 MiB row is the fix doing its job: ~229 KiB of stack per level under
ASan, so the eight further levels needed to reach the counter's 40 would have
wanted ~1.8 MiB against 954 KiB remaining -- which is precisely the overflow
the report opened with. The 64 MiB row confirms the counter still owns the
healthy case and the new trigger is invisible there.

One addition beyond the filed direction, and it matters for whoever reads the
message next: when the headroom trigger is what stopped the walk, the
diagnostic **says so**. Without it the error claims the expression "exceeds
the emitter's depth limit (40)" while the counter stood at 32 -- true about
the stack, false about the nesting, and nothing in the output tells the two
apart. This mirrors the extra note the macro guard already emits:

```
note: emission stopped at depth 32 (limit 40): the compiler's C stack is
nearly exhausted -- oversized native frames (e.g. a sanitizer-instrumented
debug build) or a small stack limit reach the stack before the depth limit
```

`TUR_DEBUG_STACK_GUARD=1` traces the emitter's guard too, the same knob
`elab_call.c` carries.

One thing the move exposed that was survivable while only the macro guard used
it, and would not have been here: the platform stack-bounds query is asked
**per recursion level**, and on glibc `pthread_getattr_np` opens and parses
`/proc/self/maps` for the main thread. Macro depth >= 16 is rare; emitter depth
>= 8 is not (the tree's deepest real nesting is 20), so every ordinary compile
on Linux would have paid a `/proc` read per node past the floor. The bounds are
fixed for the life of a thread, so `stack_guard.c` queries once and caches per
thread; the hot path is a register read and a subtract. Behaviour is unchanged
either way -- both trigger measurements above reproduce identically with the
cache in place.

**Not fixed, and out of scope here:** the same race is structurally available
to the other unbounded-ish recursive walks that carry only a counter --
`TR_MAX_TERM_DEPTH` (8000) and `TR_MAX_LET_DEPTH` (6000) in
`refine_smtlib.c`, `LA_MAX_LINEARIZE_DEPTH` (500) in `refine_solver_arith.c`,
`REFINE_MAX_EXPAND_DEPTH` (256). None has a reported repro; they are named
here so the next person to hit one finds the shared helper instead of writing
a third probe. The rest of the depth caps in `src/` are 8-64 and cannot reach
a stack cliff.

This is the **same bug, in a second walk**, as the archived
[macro-depth-guard-loses-race-with-asan-stack](../archive/macro-depth-guard-loses-race-with-asan-stack.md).
That one was fixed for macro expansion on 2026-08-18; the emitter's expression
walk was never given the same second trigger.

## Repro

On a Debug + ASan build (the documented bootstrap build), macOS/arm64,
8 MiB default stack:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
./build/tur emit-c tests/fixtures/errors/expr-nesting-depth-limit/input.tur
```

Observed: `AddressSanitizer: stack-overflow ... in emit_value_dispatch`,
exit 134. Expected (and what the fixture asserts):

```
error [TUR-E0712]: expression nesting exceeds the emitter's depth limit (40)
```

The diagnostic is not wrong -- it is unreachable. Give the process more stack
and it prints correctly:

```sh
bash -c 'ulimit -s 65520; ./build/tur emit-c tests/fixtures/errors/expr-nesting-depth-limit/input.tur'
# -> error [TUR-E0712]: expression nesting exceeds the emitter's depth limit (40)
```

so the bound itself works; only the margin is wrong on this host.

## Root cause

`src/compiler/emit_expr.c:5210` guards the walk with a **depth counter only**:

```c
#define EMIT_MAX_EXPR_DEPTH 40          /* emit_expr.c:4581 */
if (g_emit_expr_depth >= EMIT_MAX_EXPR_DEPTH) { ... }
```

40 was chosen against a measured macOS cliff of ~47 levels
(`tests/fixtures/errors/expr-nesting-depth-limit/input.tur` records the
tuning, and says the fixture "must not become a stack-size canary"). It has
become one: a constant tuned against one host's frame size cannot hold as the
frames grow. `emit_value` alone now carries a `const AdtDef *seen[32]` (256
bytes) for the region walk, and the recursion is a three-frame cycle
(`emit_value` -> `emit_value_dispatch` -> `emit_builtin`), each inflated ~40x
by ASan.

## Fix direction

Reuse the fix the macro guard already shipped -- do not re-tune the constant,
which only moves the cliff for the next frame that grows.

`elab_stack_nearly_exhausted()` (`src/compiler/elab_call.c:92`) measures the
calling thread's real remaining stack (`pthread_get_stackaddr_np` /
`pthread_getattr_np` / `GetCurrentThreadStackLimits`, reading the SP register
directly) and is already the second trigger for the macro-depth guard at
`elab_call.c:3351`. Lift it out of `elab_call.c` into a shared header and give
`emit_value` the same pair: raise TUR-E0712 when EITHER the counter hits
`EMIT_MAX_EXPR_DEPTH` OR a genuine nesting is under way and headroom has
dropped below the margin.

The archived report's non-obvious lesson applies verbatim and is why the
helper must be reused rather than reimplemented: under ASan, **the address of
a local does not approximate the stack pointer** (address-taken locals live on
the sanitizer's fake stack), so a hand-rolled probe silently falls back to the
depth counter and reproduces the bug it is fixing.

## Notes

- Release builds are unaffected (no sanitizer, ~40x smaller frames).
- Confirmed **pre-existing** as of v0.45.1: a binary built from `main` at
  `d43cc42ef`, with no local changes, aborts identically. It is not a
  regression from the Saffron graduation that surfaced it.
