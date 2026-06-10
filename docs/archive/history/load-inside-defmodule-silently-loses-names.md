---
title: ^load inside defmodule (or defn) silently accepted but loaded names not visible to subsequent forms
category: Reported
severity: medium
description: A `(load "...")` form that appears inside a `defmodule` body is silently accepted (exit 0, no warning) but the names it loads are not injected into the module's resolution scope. Any use of a loaded name inside the same defmodule produces `TUR-E: unknown function or operator`. The same applies to `load` inside a `defn` body. `load` only takes effect at the top-level compilation unit; textual position inside any enclosing form makes it a no-op for name resolution.
status: RESOLVED -- Option A implemented; see resolution note below.
---

> **RESOLVED (2026-06-10).** Two-stage resolution:
>
> 1. The silent no-op was first closed by emitting a hard error
>    (#303, "Option B" below).
> 2. The ergonomic fix ("Option A") then landed: the load-expansion
>    preprocessor (`load_expand_forms` in
>    `src/compiler/elab_toplevel.c`) now **descends into `(defmodule ...)`
>    bodies**, so a `(load "path")` placed directly in a module body
>    splices the loaded file's top-level forms into the module's scope --
>    exactly as a top-level load splices into the compilation unit. The
>    nested walk shares the compilation-global visited set on the `Elab`,
>    so a path already spliced elsewhere is not re-spliced. Loaded names
>    (e.g. `stdlib/arrow.tur`'s `>>>`) are now visible to subsequent body
>    forms.
>
> A `(load ...)` in genuine **expression position** (a `defn`/`let`/`do`
> body) is still a hard error -- it cannot act as a compile-time include
> there. `elab_load` in `src/compiler/elab_module.c` carries the updated
> diagnostic.
>
> Regression coverage: positive
> `tests/fixtures/load-inside-defmodule-injects-names/` (load inside a
> module body, `>>>` resolves and the program runs); negative
> `tests/fixtures/errors/load-inside-defn/` (expression-position load
> still errors). The old `errors/load-inside-defmodule` negative fixture
> was removed -- that input is now valid.
>
> Note: the `^fat` let-binding form of `>>>` (`^fat h : (fn ...) (>>> f g)`)
> still segfaults at runtime regardless of load placement -- that is the
> orthogonal, separately-tracked
> [`../reported/fat-shim-void-ptr-calls-bare-not-fat.md`](../reported/fat-shim-void-ptr-calls-bare-not-fat.md),
> not a load-scope issue. The named-function `>>>` shape works.

# `load` inside `defmodule` silently loses names

## Summary

**Severity: Medium.** A `(load "stdlib/arrow.tur")` placed inside a `defmodule`
block is silently accepted (`tur check` exits 0, no warning or error emitted for
the `load` form itself), but the names defined in the loaded file are NOT visible
to subsequent `defn` forms within that `defmodule`. Using any loaded name
produces `TUR-E: unknown function or operator`. The same behaviour occurs when
`load` is placed inside a `defn` body.

The only position where `load` actually injects names is the **top-level** of the
compilation unit -- before any `defmodule` or `defn`.

## Minimal repro

### `load` inside `defmodule` body -- broken

```turmeric
(defmodule test/load-inside
  (export test-fn)

(load "stdlib/arrow.tur")   ;; silently accepted, names NOT injected

(defn test-fn [x : float] : float
  (let [^fat f : (fn [float] #{} float) (fn [v : float] : float (* v 2.0))
        ^fat g : (fn [float] #{} float) (fn [v : float] : float (+ v 1.0))
        ^fat h : (fn [float] #{} float) (>>> f g)]   ;; ERROR here
    (h x)))

) ;; end defmodule
```

Output:
```
input.tur:9:42: error: unknown function or operator '>>>'
```

Note: if `>>>` is never referenced, `tur check` exits 0 -- the `load` is
silently a no-op, not an error.

### `load` before `defmodule` -- works

```turmeric
(load "stdlib/arrow.tur")   ;; at top level: names ARE injected

(defmodule test/load-before
  (export test-fn)

(defn test-fn [x : float] : float
  (let [^fat f : (fn [float] #{} float) (fn [v : float] : float (* v 2.0))
        ^fat g : (fn [float] #{} float) (fn [v : float] : float (+ v 1.0))
        ^fat h : (fn [float] #{} float) (>>> f g)]   ;; OK
    (h x)))

) ;; end defmodule
```

### `load` inside a `defn` body -- also broken

```turmeric
(defn outer [] : float
  (load "stdlib/arrow.tur")   ;; silently accepted, names NOT injected
  (let [^fat f : (fn [float] #{} float) (fn [v : float] : float (* v 2.0))
        ^fat g : (fn [float] #{} float) (fn [v : float] : float (+ v 1.0))
        ^fat h : (fn [float] #{} float) (>>> f g)]   ;; ERROR
    (h 1.0)))
```

## Observed vs expected

- **Observed**: `load` inside `defmodule` or `defn` -- silently accepted, loaded
  names not visible, use-site produces `TUR-E: unknown function or operator`.
- **Expected** (option A): `load` is a compile-time directive that works
  regardless of textual position, injecting names into the enclosing scope.
- **Expected** (option B): `load` inside a non-top-level form is a hard error
  (`TUR-E: load is only valid at top level`), so the programmer knows to move it.

Option B is simpler to implement and sufficient: a clear error prevents silent
confusion. Option A is more ergonomic (spice modules would be able to load stdlib
dependencies inline).

## Root cause

`load` is processed as a top-level compilation directive. When the compiler
encounters it inside a `defmodule` or `defn` form, it likely treats it as an
ordinary expression (possibly a no-op call to a non-existent `load` function)
rather than triggering the file-inclusion machinery. No error is raised, but the
file-level environment built by `load` is not threaded into the enclosing form's
resolution scope.

File/line pointers to investigate:
- `src/main.c` -- look for where `load` special-form is handled during
  elaboration/codegen; check whether it guards on `depth == 0` (top-level only)
  or processes it in any context.

## Impact

Discovered while implementing Phase 5 of `tur-signal`: `compose.tur` needs
`stdlib/arrow.tur`'s `>>>` inside a `defmodule`. With `load` inside the module
failing silently, the only workaround is to put the `load` **before** the
`defmodule`. This works, but:

1. It is non-obvious -- the `load` must appear in the file before the `defmodule`
   even though logically it is a dependency of the module.
2. When multiple spice modules are built in a single `tur build` invocation, the
   load order across files is not always controlled by the user, so a `load`
   that appears before the `defmodule` in one file may not be re-evaluated in
   another compilation unit.
3. The silent no-op behavior (instead of a diagnostic) wastes debugging time.

## Workaround

Place `(load "stdlib/arrow.tur")` (or any other `load`) **before** the
`(defmodule ...)` block at the top of the file. This injects the names into the
global env before the module definition is processed, making them visible within
the module body.

```turmeric
;; Correct:
(load "stdlib/arrow.tur")   ;; top-level -- works

(defmodule my-module
  ...
  (defn foo [...] (>>> ...))  ;; >>> is visible
  ...)
```

## Validation

- Confirmed broken: `tur check` on `(defmodule ... (load ...) (defn ... (>>> ...)))` --
  exits 1 with `unknown function or operator '>>>'`.
- Confirmed broken (silent): `tur check` on `(defmodule ... (load ...) (defn dummy [] 0))` --
  exits 0 with no output (load is silently a no-op).
- Confirmed working: `tur check` on `(load ...) (defmodule ... (defn ... (>>> ...)))` --
  exits 0, check-clean.
- Confirmed broken in `defn` body: `tur check` on `(defn outer [] (load ...) (>>> ...))` --
  exits 1 with `unknown function or operator '>>>'`.

## Cross-references

- `spices/signal/src/signal/compose.tur` -- hit this gap when implementing
  Phase 5 `effects-chain`; workaround (direct `__sf-fold` fat dispatch) used
  instead. If this gap resolves, `compose.tur` could `load "stdlib/arrow.tur"`
  before the `defmodule` and call `>>>` directly.
- `stdlib/arrow.tur` `>>>` -- the specific symbol that triggered discovery.
