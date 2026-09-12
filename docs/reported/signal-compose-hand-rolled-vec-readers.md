# `signal/compose` hand-writes inline-C Vec readers on a stale premise

**Severity: low.** Nothing is wrong at runtime. What it costs is interpreter
coverage: `tests/run-turi.sh` PASS-skips any fixture whose program contains a
user inline-C block, so every fixture that loads `signal/compose` is skipped
under `--interpret` for two helpers that no longer need to exist.

**Status:** open. Found 2026-09-11 investigating lattice-vocabulary-plan L4,
which had to read this module closely.

## The premise, and why it is stale

`spices/signal/src/signal/compose.tur` defines `__vec-get-i` and `__vec-len-i`
as inline C that reinterprets the `Vec` runtime layout by hand:

```turmeric
;;; Project-mode compilation auto-loads only stdlib/macros.tur, so
;;; stdlib/vec.tur's `vec-get` is not in scope here. Inline a minimal reader
;;; that matches the Vec runtime layout (data/len/cap).
(defn __vec-get-i [v : int i : int] : int
  ```c struct { int64_t *data; size_t len; size_t cap; } *vec = (void*)(intptr_t)v;
  ...
  ```)
```

The auto-load list is **shared** by single-file and project mode --
`src/compiler/stdlib_autoload.c`'s own header says so ("Shared by compile_to_c
(single-file) and compile_to_h / compile_to_implementation (project-mode
multi-file) so spice code in `tur build .` sees `Cons`, `tnil?`, `Option`, etc.
without explicit imports") -- and `vec.tur` is on it.

Confirmed against the tree rather than the header: `spices/plot/src/plot/core.tur`
and `spices/linalg/src/linalg/mat.tur` both call stdlib `vec-get` directly in
project mode, and neither defines its own.

## Why it costs something

Beyond the duplication, the two helpers hand-reinterpret
`{ int64_t *data; size_t len; size_t cap; }`. That layout is not this module's
to know: a change to the `Vec` representation would miscompile here silently
while every stdlib caller kept working.

And the inline C is what puts the module's fixtures outside `run-turi.sh`.

## Fix

Delete both helpers; call `vec-get` / `vec-len`. The call sites are
`__chain-loop` and `effects-chain`, both in the same file.

Worth pairing with a check that the module's own tests then run under
`--interpret`, since that is the point.

## Not this

L4 also asked whether `effects-chain`'s hand-written `__chain-loop` should
become `mconcat` over the endomorphism monoid (it is one). It should not --
`definstance` heads must be plain type names, so the endomorphism needs a
`defstruct` wrapper, and `effects-chain`'s signature takes an untyped `Vec` of
raw SF carriers, so the rewrite adds a wrapping pass rather than removing a
recursion. See that plan's L4 entry. The Vec readers are a separate and
straightforwardly good change.
