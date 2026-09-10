---
title: The emitter's expression-depth guard loses the race with the ASan stack limit
category: Reported
description: "EMIT_MAX_EXPR_DEPTH is a depth counter with no real-stack-headroom check, so on a Debug+ASan macOS/arm64 host the stack overflows before depth 40 and TUR-E0712 never prints. The macro-expansion guard had the identical bug and was fixed in 2026-08; the emitter never got the same treatment."
---

# The emitter's expression-depth guard loses the race with the ASan stack limit

**Severity:** low (Debug/ASan only; the diagnostic is correct, it just never
gets to print). One fixture red on affected hosts:
`errors/expr-nesting-depth-limit`.

**Status: FIXED 2026-09-09**, and then fixed properly. The filed direction --
pair the depth counter with a real-headroom check -- landed first and stopped
the crash. It was the wrong shape, and the review question that exposed it was
one sentence: *Turmeric has a trampoline, why is anything stack-overflowing?*

The answer is that the trampoline is a property of the **emitted program** and
the runtime (`src/runtime/cps_rt.c`) -- it is what stops a Turmeric program's
recursion from consuming C stack. The compiler's own phases are ordinary C
recursion over the AST, and nothing trampolines those. But the question is
still the right one, because it points at the actual defect: the depth *cap*,
not the missing headroom check.

**`EMIT_MAX_EXPR_DEPTH` was wrong in both directions at once.**

- *As a ceiling it did not hold.* 40 was measured against one host's Debug+ASan
  cliff. The `emit_value` frame later grew a 256-byte region-walk array, which
  moved the cliff below 40 -- this report.
- *As a floor it rejected working code.* 40 was checked against the deepest
  **hand-written** nesting in the tree (20). Macro expansion is not
  hand-written. `spices/ecs/tests/for-each-arity-12.tur`, a 12-component
  `for-each`, expands past 40 and **could not be compiled at all** -- on `main`
  at the default stack it did not even get the diagnostic, it crashed the
  compiler (exit 134).

So the cap is gone. Depth follows the input, so the compiler sizes the stack
for the job instead of rationing it: `tur` trampolines its whole driver onto a
stack sized by `TUR_STACK_MB` (default 256 MiB) via `tur_run_on_big_stack`.
That is not a novel move -- `jit_engine.c` has run a JIT'd program's entry on a
`pthread_attr_setstacksize` thread behind `TUR_JIT_STACK_MB` all along, and
rustc does the same for compilation behind `RUST_MIN_STACK`.

**A second, worse instance turned up while testing the first.** Sizing only
*emission* was not enough: at ~400 levels the abort simply moved to
`elab_call -> elab_form`, which had **no depth guard at all** -- so deep
nesting aborted the compiler with no diagnostic whatsoever, a strictly worse
failure than the one this report was filed for. It now carries the same
headroom backstop. That is why the stack is sized under the whole driver rather
than per phase.

What remains at each walk is a backstop, not a bound: it fires only when the
real stack is nearly gone, which after this change means a genuinely unbounded
walk rather than a merely deep one. Measuring the actual resource is also the
only bound that cannot rot the way the constant did -- correct at any frame
size, on any host, under any sanitizer.

### Results

| case | before | after |
| --- | --- | --- |
| `ecs/tests/for-each-arity-12.tur` (real code) | **crash**, exit 134 | compiles, runs, prints 3510 |
| 120-level nesting | refused (over the 40 cap) | compiles, prints 121 |
| 400-level nesting | **crash** in `elab_call` | compiles |
| 400-level at `TUR_STACK_MB=8` | **crash**, no diagnostic | TUR-E0712 at depth 317, exit 1 |

Coverage moved with the behaviour. `errors/expr-nesting-depth-limit` asserted a
cap that no longer exists and is retired; `tests/fixtures/expr-nesting-deep`
asserts the positive case (120 levels, 3x the retired cap), and
`tests/run-compiler-stack-guard.sh` (ctest `tur_compiler_stack_guard`) asserts
the backstop in both directions by shrinking the stack rather than growing the
program -- which is what keeps it from becoming the stack-size canary the old
fixture became.

The original headroom work, which still stands underneath all of this, was
verified on the host the report was filed from (macOS/arm64, Debug + ASan,
8 MiB stack).

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

## A real-world instance, found after the fix

The fixture this report names is synthetic -- 60 hand-written `(+ ...)` levels
written to sit between the bound and the cliff. A real one turned up in the
sibling `turmeric-spices` repo while surveying it:
`spices/ecs/tests/for-each-arity-12.tur`, a 12-component `for-each` whose body
is a 12-deep `(+ a (+ b (+ c ...)))` chain. The ECS `for-each` macro expands
that into something far deeper.

Measured three ways on this host:

| compiler | stack | result |
| --- | --- | --- |
| `main` (pre-fix) | 8 MiB default | **crash** -- `AddressSanitizer: stack-overflow`, exit 134 |
| `main` (pre-fix) | 64 MiB | TUR-E0712 at `tests/for-each-arity-12.tur:72:109` |
| post-fix | 8 MiB default | TUR-E0712 + the stack note at depth 31, exit 1 |

So the failure mode this report describes is not confined to a fixture written
to provoke it: an ordinary spice test crashes the compiler on the documented
bootstrap build, and did so before this change. That is also why the report was
worth executing at its filed severity of "low" -- the severity was right about
the blast radius and wrong about how reachable it is.

**One caveat this measurement exposed, and it is a real cost.** The two triggers
blame different spans. The counter fires at the deepest node and points at the
user's expression (`for-each-arity-12.tur:72`); the headroom trigger fires at
whichever node the walk happens to be emitting when the stack runs down, which
here is inside the macro's own source (`src/ecs/query.tur:111`). Emission order
is not source order, so the headroom-blamed span is not necessarily the deepest
nesting and may sit in a library the user did not write.

That is worse than the counter's span, and better than the alternative on this
build, which is an abort with no span at all. The extra note is what keeps it
honest -- it says the stack, not the nesting, is what stopped the walk, so a
reader is not sent hunting for 40 levels of nesting at `query.tur:111`. If the
span ever needs to be better, the fix is to carry the outermost in-progress
user-code span alongside the depth counter and prefer it in the headroom case;
that is a diagnostic-quality improvement, not a correctness one, and is not
done here.

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
