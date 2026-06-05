---
title: UBSan: load of non-bool value 2 from arg_poly_fn[] at elab_call.c:2595
category: Reported
severity: medium
description: The compiler, built with `-fsanitize=address,undefined` (Debug), emits a UBSan diagnostic on virtually every `tur check` invocation: "runtime error: load of value 2, which is not a valid value for type 'bool'" at `src/compiler/elab_call.c:2595:17`. The load reads `fn_type.as.fn.arg_poly_fn[fn_arg_idx_pf]`, which is declared as `bool *` but contains uninitialized/non-{0,1} bytes.
---

# UBSan: non-bool value in `arg_poly_fn[]` at elab_call.c:2595

## Summary

One-line: every Debug `./build/tur check` invocation prints a UBSan
diagnostic from `elab_call.c:2595` -- the code reads a `bool` element
out of `fn_type.as.fn.arg_poly_fn[]` but the array sometimes holds a
byte that is neither 0 nor 1 (observed: 2). This points to either an
uninitialized field that should be zero-filled at construction, or a
write site that stores a non-boolean integer through the `bool *`
pointer.

Severity: **medium.** The diagnostic does not currently change the
visible behavior of `tur check` (the conditional happens to take the
"true" branch on any non-zero byte, which is what callers intend),
but it is a real UB read -- the optimiser is free to assume the load
yields 0 or 1, and a future codegen change could miscompile.

## Observed vs. expected

### Expected

`./build/tur check <any-file>` should be UBSan-clean.

### Observed

```
$ ./build/tur check ../turmeric-spices/spices/signal/src/signal/core.tur
/Users/rjungemann/Projects/turmeric/src/compiler/elab_call.c:2595:17: runtime error: load of value 2, which is not a valid value for type 'bool'
SUMMARY: UndefinedBehaviorSanitizer: undefined-behavior /Users/rjungemann/Projects/turmeric/src/compiler/elab_call.c:2595:17
```

The same message fires on essentially any non-trivial input, including
many `tests/fixtures/*` inputs. It does not cause a test FAIL because
the read still classifies as "truthy" for the conditional, but UBSan
flags every occurrence.

## Minimal repro

```sh
# from turmeric repo root
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build -j
./build/tur check stdlib/list.tur 2>&1 | grep "elab_call.c:2595"
```

## Root cause (best guess)

`elab_call.c:2592-2595`:

```c
if (!is_rank2_param && fn_type.kind == TY_FN) {
    uint32_t fn_arg_idx_pf = fn_binding->closure_fn_binding ? i + 1 : i;
    if (fn_arg_idx_pf < fn_type.as.fn.arity &&
        fn_type.as.fn.arg_poly_fn[fn_arg_idx_pf]) {
```

`fn_type.as.fn.arg_poly_fn` is declared as `bool *` (parallel array sized
to `arity`, one flag per parameter slot). The load at `:2595` is
`arg_poly_fn[fn_arg_idx_pf]`, and UBSan reports the byte read is `2` --
a value that violates the `_Bool` representation invariant
(`_Bool` is required to hold only `0` or `1`).

Two likely causes:

1. **Allocation site does not zero the buffer.** If
   `arg_poly_fn` is allocated via `malloc` (or via the bump-arena
   without an explicit `memset`), and the calling code only writes the
   `true` slots, the `false` slots retain whatever was on the arena
   page -- which can be any value.

2. **Some write site stores a non-boolean integer through `bool *`.**
   A line like `arg_poly_fn[k] = some_int_flag;` where `some_int_flag`
   is a `uint32_t`/`int` carrying a bitfield or count would commit a
   `2` byte. This is also plausible since the value `2` is suspiciously
   specific (not garbage like `0x5a`).

A targeted grep + `memset(arg_poly_fn, 0, arity * sizeof(bool))` at the
allocation site should reveal whether (1) is the culprit. If (2), the
write must be changed to a `!= 0` boolean coercion.

## Why it was masked

- The Release build (`-DCMAKE_BUILD_TYPE=Release`) compiles without
  `-fsanitize=undefined`, so the diagnostic is invisible in normal
  use.
- The Debug build runs `tests/run.sh` with leak detection on but
  apparently tolerates UBSan diagnostics on stderr without failing
  the suite -- `tests/run.sh` does not grep for "runtime error:"
  in compiler stderr, only in spawned-program stderr.

## Proposed fix direction

1. Locate the allocation of `fn_type.as.fn.arg_poly_fn`. Likely sites:
   `src/compiler/types/fn_type.c` or wherever `TY_FN` is constructed.
   Switch `malloc(arity)` -> `calloc(arity, sizeof(bool))`, or follow
   `malloc` with an explicit zero-fill.
2. Grep for every assignment into `arg_poly_fn[...]`; ensure all
   writes are either `true`, `false`, or `!= 0` boolean coercions.
3. Add an assertion: `assert(arg_poly_fn[k] == 0 || arg_poly_fn[k] == 1);`
   immediately after the load at `:2595` so future regressions surface
   as a hard abort under Debug.

## Validation of a fix

- `./build/tur check stdlib/list.tur 2>&1 | grep "runtime error:"` is
  empty.
- `bash tests/run.sh 2>&1 | grep "runtime error:"` is empty across the
  full fixture sweep.
- `bash tests/run.sh` still reports zero `FAIL`.
