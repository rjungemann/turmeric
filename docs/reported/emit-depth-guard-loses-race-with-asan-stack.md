---
title: The emitter's expression-depth guard loses the race with the ASan stack limit
category: Reported
description: "EMIT_MAX_EXPR_DEPTH is a depth counter with no real-stack-headroom check, so on a Debug+ASan macOS/arm64 host the stack overflows before depth 40 and TUR-E0712 never prints. The macro-expansion guard had the identical bug and was fixed in 2026-08; the emitter never got the same treatment."
---

# The emitter's expression-depth guard loses the race with the ASan stack limit

**Severity:** low (Debug/ASan only; the diagnostic is correct, it just never
gets to print). One fixture red on affected hosts:
`errors/expr-nesting-depth-limit`.

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
