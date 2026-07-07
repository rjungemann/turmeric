---
title: panic-return-signal -- compiled panic propagation without setjmp/longjmp (D1a)
category: Planning
description: The D1a sub-decision of compiled-c-crossing-tco-plan, wired behind --enable=panic-return-signal. Replaces the compiled backend's setjmp/longjmp panic unwind with a thread-local `tur_panicking` return-path signal so a `catch-unwind` boundary no longer pins a jmp_buf on the C stack and tail calls may cross the boundary. Prototype: direct-call panic path only.
---

# panic-return-signal (D1a) -- Plan

## Why this experiment exists

[compiled-c-crossing-tco-plan.md](./compiled-c-crossing-tco-plan.md) Phase D1
landed the **heap handler chain** (handler discovery off a thread-local chain of
heap nodes that each own their `jmp_buf`), lifting deeply-nested `catch-unwind`
from a SIGSEGV below 50000 to ~150000. D1a is the transport half: replace the
`setjmp`/`longjmp` unwind with a thread-local `tur_panicking` return-path
signal. Measured motivation (see the D1a section of the parent plan):

- The status-word fat-return transport is ~3.5x slower at `-O2` (register
  pressure); the **thread-local flag** is the chosen transport.
- Removing the `jmp_buf` entirely removes the per-boundary frame *size*, a
  further depth win on top of the heap chain, and -- unlike `longjmp` -- lets a
  tail call cross a `catch-unwind` boundary without pinning a landing-pad frame.

It ships gated because it is genuinely in-flight (prototype lifecycle) and,
importantly, does **not** on its own hit the parent plan's 200000/1,000,000
target for the *nested* shape: that wall is frame **count** (two live C frames
per nesting level), which only the stackless/CPS lowering (D3) removes. This
experiment is the prerequisite transport for that work, not the thing that
reaches the target.

## The mechanism

- **`panic` / `panic-with`**: set thread-local `tur_panicking = 1`, stage the
  payload in `global_panic_payload`, fire the panicking frame's defers, and
  **return** (no `longjmp`). The C return value on this path is a zero of the
  function's return type.
- **Every panic-capable call site**: after the call returns, check
  `tur_panicking`; if set, return a zero of the enclosing function's return type
  immediately (propagate). The codegen A-normalizes each panic-capable call to a
  temporary so the check can follow it (calls are otherwise emitted mid-C-
  expression, e.g. `1 + deep(n-1)`, where no check can be injected).
- **`catch-unwind` / `catch-panic-of`**: push the heap handler node (shared with
  D1), call the thunk, then consult `tur_panicking`: if set, clear it and box the
  staged payload as `(err ...)`; else box the value as `(ok ...)`. No `setjmp`.

## Scope of the prototype

Covered: the direct compiled-call panic path -- `panic`, `panic-with`,
`catch-unwind`, `catch-panic-of`, and the return-path propagation through
ordinary `defn` calls.

**Not yet covered** (still `longjmp` / unchanged, and incompatible with the
signal when enabled): the fiber auto-cancel unwind, the effect/`handle`
suspension path, and `with-cancel-guard`. Enabling this experiment in an async
program is unsupported and will misbehave; that integration is deferred to the
D3 work, which reworks those boundaries anyway.

## What it does not do

- It does not make nested `catch-unwind` unbounded (frame-count wall -- D3).
- It does not change any source-level semantics of `panic` / `catch-unwind`.

## Validation

- `bash tests/run.sh` with the experiment OFF (default) is byte-identical to
  before -- the flag gates every emission difference.
- With `--enable=panic-return-signal`: the `panic-*` catch fixtures produce the
  same ok/err results, and `cu-rec` / `cu-catch-deep` run at least as deep as
  the D1 baseline (the jmp_buf removal is a further depth win).

## Graduation / exit

Graduates (feature deleted, transport always-on) only once it (a) covers the
fiber / effect / cancel unwinds and (b) is measured neutral-or-better on the
non-panicking hot path across the fixture suite. Until then it stays a
prototype behind the gate. `expires_at` 0.31.0 is the soft deadline.
