# Macro-expansion depth guard loses the race with the ASan stack limit

**Severity:** low (Debug/ASan only; the diagnostic is correct, it just never
gets to print). One fixture red: `errors/macro-depth-hint`.

## Summary

`errors/macro-depth-hint` asserts that a runaway macro produces
`maximum macro expansion depth exceeded` plus the `(empty? xs), not (nil? xs)`
hint. On a Debug (ASan-instrumented) `tur` on macOS/arm64, the elaborator
exhausts the sanitizer's stack *before* the depth counter reaches its limit, so
the process aborts with a stack-overflow report instead of emitting the
diagnostic. The harness scores it `diagnostic mismatch`.

The guard itself is not broken -- it is simply set higher than the instrumented
stack can reach. ASan's per-frame redzones inflate every frame, so the same
counter that fits comfortably in a Release stack does not fit in a Debug one.

## Reproduce

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j --target tur
./build/tur check tests/fixtures/errors/macro-depth-hint/input.tur
```

```
SUMMARY: AddressSanitizer: stack-overflow elab_macros.c:1104 in substitute_params
```

The reported frame varies between `substitute_params` (`elab_macros.c:1104`)
and `elab_try_return_dispatch` (`elab_typeclasses.c:4928`) depending on where
the last frame lands; the recursion cycle underneath is always the same
`elab_call` (`elab_call.c:2673` / `3285`) -> `elab_form`
(`elab_toplevel.c:643`) pair.

## Not a regression

Verified against a **pristine `HEAD` build in a clean worktree** (fb177a4e1,
no local changes): identical stack-overflow abort. This predates the
0.34.0 experiment graduations and is not caused by them -- it was noticed
while reading a suite run for that work, not introduced by it.

## Fix directions

- Lower the macro-depth limit so the counter trips first on an instrumented
  stack. Cheapest, but it lowers the limit for everyone to accommodate Debug.
- Check remaining stack headroom alongside the depth counter, and emit the
  same diagnostic on either trigger. More honest -- the limit exists to catch
  runaway expansion, and "the stack is nearly gone" is that condition.
- Give the fixture a `requires.*` marker so it skips under a sanitized build.
  Weakest: the diagnostic then has no coverage where it is most likely to
  regress.

The second is the one worth doing; the depth counter is a proxy for stack
headroom and could just measure it.
