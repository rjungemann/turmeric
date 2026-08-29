# `emit_value_dispatch` recurses without a depth bound and exhausts the stack

**Severity: high** (the default developer build crashes on **47 levels** of
expression nesting; the failure is a SIGSEGV / ASan `stack-overflow`, not a
diagnostic). Found 2026-08-28 getting `turmeric-spices` CI green, against
`tur v0.40.0` / turmeric `5c9d533`.

**Verified 2026-08-28** on a freshly built `v0.40.0`, macOS arm64 / Apple clang,
with thresholds measured on both a sanitized and an unsanitized build. Reported
from Linux at `ulimit -S -s 2048` against `main`'s own sources; reproduces on
macOS at the default limit.

**Status: RESOLVED 2026-08-29** as TUR-E0712, taking fix direction (1) only.
Direction (2) -- shrink the frame or move to a worklist -- is still open and
still worth doing; see [Resolution](#resolution).

## Summary

`emit_value` / `emit_value_dispatch` walk the expression tree by plain
structural recursion with no depth bound. A sufficiently nested expression
exhausts the C stack. The emitter is the outlier here: three other passes
already carry a depth guard (`kind_check.c:209` at 16, `cps_ir.c:150` at 64,
`interp.c:244` `MAX_MACRO_DEPTH`).

## Repro

Any deeply left-nested expression will do:

```sh
python3 -c "
e='1'
for i in range(200): e='(+ %s 1)'%e
print('(defmodule dp (export)\n  (defn main [] : int %s))'%e)
" > deep.tur

tur emit-c deep.tur
```

```
==42776==ERROR: AddressSanitizer: stack-overflow on address 0x00016f4c2a98
    #0 0x0001006f09c4 in emit_value_dispatch emit_expr.c:4065
    #1 0x0001006ee964 in emit_value          emit_expr.c:3829
    #2 0x000100605598 in emit_builtin        emit_core.c:3662
    #3 0x0001006f69e8 in emit_value_dispatch emit_expr.c:4387
    #4 0x0001006ee964 in emit_value          emit_expr.c:3829
    #5 0x000100605598 in emit_builtin        emit_core.c:3662
    ...
```

A clean three-frame cycle per nesting level:
`emit_value_dispatch:4065` -> `emit_value:3829` -> `emit_builtin` (emit_core.c:3662)
-> `emit_value_dispatch:4387`.

## Measured thresholds

Nesting depth at which `tur emit-c` first crashes:

| Build | Stack limit | Crashes at |
| --- | --- | --- |
| Debug + ASan (**the documented bootstrap build**) | 8 MB (default) | **47** |
| Debug + ASan | 32 MB | 150-200 |
| `-DTUR_DEBUG_SANITIZE=OFF` | 2 MB | 500-1000 |
| `-DTUR_DEBUG_SANITIZE=OFF` | 8 MB (default) | 2000-5000 |

Roughly **170 KB of stack per nesting level** under ASan, ~4 KB without it --
so ASan inflates the frame about 40x, and the sanitized build is where this
actually bites.

That first row is the severity argument. `cmake -S . -B build
-DCMAKE_BUILD_TYPE=Debug` is what CLAUDE.md's bootstrap section tells you to
run, `TUR_DEBUG_SANITIZE` defaults ON on every platform, and the result crashes
on 47 levels of nesting. That is low enough to reach from macro expansion,
generated code, a long `cond` chain, or a fold written as nested binary
operations -- none of which look pathological in source.

## `tur check` crashes identically -- same defect, not a second one

`tur check deep.tur` produces the *same* stack trace, in the same emitter
cycle, at the same depth. `check` runs the emitter, so there is one root cause
here, not a checker bug and an emitter bug.

Worth knowing because it means the crash is not avoidable by "just type-check in
the editor" -- the LSP path goes through it too.

## Expected

A depth-bounded walk that reports a diagnostic:

```
error [TUR-E....]: expression nesting exceeds the emitter's depth limit (N);
  simplify the expression or split it into helper functions
```

An expression that is too deep to compile is a legitimate limit. Dying without
attribution -- and on a *sanitized* build, dying inside ASan's own report
machinery -- is not.

## Fix direction

Two levels, and the cheap one is worth doing on its own:

1. **Add the depth guard.** Thread a depth counter through
   `emit_value`/`emit_value_dispatch` and fail with a diagnostic past a
   generous bound. `cps_ir.c:150` already does exactly this at 64 for a
   comparable walk; copy the shape. This converts a crash into an error message
   and is a contained change.

2. **Shrink the frame, or stop recursing.** 170 KB per level (ASan) / 4 KB
   (plain) is a lot for a tree walk -- worth finding out what is on those three
   frames, since a large stack buffer in one of them would explain it and would
   be cheap to heap-allocate or hoist. The structural fix is an explicit
   worklist instead of native recursion, which removes the bound entirely; that
   is a much larger change and (1) should not wait for it.

A regression fixture wants care: it must assert the **diagnostic**, not just
"does not crash", and it should sit well below whatever bound is chosen so it
does not become a stack-size canary in CI.

## Where it bit

`spices/ecs`'s test corpus, on macOS CI, where `tur` is built Debug (hence
ASan-instrumented). Surfaced while fixing an unrelated problem: `ecs` and `osc`
had declared their `test` dependency against a nonexistent GitHub org, so being
`:optional` the fetch failed silently and **their suites had never compiled at
all**. Pointing them at the workspace sibling made them build for the first
time, which is what first exposed this.

## Workaround in use

`turmeric-spices` CI raises the stack limit rather than changing any spice
source -- `.github/workflows/ci.yml:160` and `:217`:

```sh
ulimit -s unlimited 2>/dev/null || ulimit -s 65520 2>/dev/null || true
```

The comment there records the same diagnosis. This is the right call for CI --
it does not distort the spices to suit a compiler bug -- but note from the table
above that raising the limit only buys a **linear** factor in depth. It moves
the cliff; it does not remove it, and it does nothing for a developer running
the documented bootstrap build locally.

## Guides to update when fixed

- docs/guides/test-suite-portability-guide.md -- it covers ASan build traps
  already; a stack-size note belongs with them.

---

## Resolution

Fixed 2026-08-29 as **TUR-E0712**, taking fix direction (1). This report needed
no correcting -- the diagnosis, the measured thresholds, and the advice about
the fixture all held up. Two things it left open are settled below.

### The guard

The counter lives in `emit_value` (`src/compiler/emit_expr.c`) rather than
being threaded as a parameter. `emit_value` sits on every turn of the cycle
this report traced -- `emit_value_dispatch` -> `emit_value` -> `emit_builtin`
-> `emit_value_dispatch` -- so one counter there bounds the whole walk without
touching `emit_value`'s many callers.

On hitting the bound it reports once (every enclosing level would otherwise
repeat the same message for the same expression) and returns `strdup("0")`, a
valid C expression, so the rest of emission stays well-formed and the build
fails on the diagnostic rather than on malformed output. No new plumbing was
needed to fail the build: `emit_program` already ends with
`return diag_had_error() ? 1 : 0;`.

### Choosing the bound: 40

This report gave the ceiling; the floor had to be measured. Both matter, and
they only just fit:

- **Ceiling** -- the bound must fire before the stack runs out on the
  *documented* build. From the table above: 47 levels (macOS/clang + ASan, 8 MB)
  and 60-80 (Linux/gcc + ASan, 8 MB, reproduced here). So it must clear 47.
- **Floor** -- it must not reject real code. Instrumenting `emit_value` and
  running every `.tur` in `stdlib/`, `tests/fixtures/` and `examples/` (3014
  files) puts the deepest nesting **anywhere in the tree at 20**, in
  `conv-defstruct-setmap-lowering`. Nothing else exceeds 17.

40 clears the worst sanitized threshold and leaves 2x headroom over the deepest
real expression. The suite confirms it: 2723 passed, 0 failed, nothing newly
rejected.

That the two bounds only just fit is the argument for direction (2).

### Direction (2) is still open

Not attempted here, as this report recommended. The frame is still ~170 KB
under ASan / ~4 KB plain, and the bound is still a stack budget rather than a
language limit. An explicit worklist would remove it entirely. Anyone picking
that up can delete `EMIT_MAX_EXPR_DEPTH` and TUR-E0712 with it.

### Regression

`tests/fixtures/errors/expr-nesting-depth-limit/` -- 60 levels, which clears
the bound (40) while staying far under the stack cliff, so it asserts the
diagnostic and does not double as a stack-size canary, exactly as this report
asked. `tur check` is covered by the same fixture, since `check` runs the
emitter.

### Guide

`docs/guides/test-suite-portability-guide.md` gains section 7c, "ASan inflates
the stack ~40x", next to the existing sanitizer traps: the measured table, the
two traps it creates (a depth crash on a Debug build is not evidence of runaway
recursion; raising `ulimit -s` only buys a linear factor), and the rule that a
depth bound has to be calibrated against the sanitized build.
