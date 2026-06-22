---
title: httpd `concurrent_test` -- `thread-spawn-fn` is NOT renamed or removed; stdlib/thread.tur is intact but not on the spice's module-resolution path
category: Module resolution / stdlib library-module loading -- not a compiler API regression
severity: Medium. Blocks httpd's `concurrent_test` (httpd otherwise 4/5 green),
  but the root cause is resolution, not a missing/renamed builtin. No
  compiler-side rename happened: `thread-spawn-fn` exists, unchanged, in
  `stdlib/thread.tur` and resolves correctly when that module is in scope.
status: RESOLVED 2026-06-22 -- not a turmeric defect; spice applied the
  recommended fix. Re-verified against this branch's `tur` with a fresh
  turmeric-spices clone: (1) `thread-spawn-fn` is still defined and exported by
  `stdlib/thread.tur:49`; (2) the report's probe holds exactly -- without a
  load the name is unresolved, and after `(load "stdlib/thread.tur")` it
  resolves (the next diagnostic is the expected "unsafe function ... requires an
  enclosing (unsafe ...)" check, proving resolution succeeded); (3) httpd's
  `concurrent_test.tur` now opens with an explicit `(load "stdlib/thread.tur")`
  whose comment cites this report, so the original `unknown function
  'thread-spawn-fn'` symptom is gone. `concurrent_test` currently fails for an
  UNRELATED reason -- a missing vendored `yyjson.h` (JSON dep) reached far past
  thread resolution -- which is a spice build-config matter, not this report.
  No turmeric change required. See
  `docs/archive/history/thread-spawn-fn-not-removed-stdlib-thread-needs-resolution.md`.
---

# `thread-spawn-fn` still exists; the httpd failure is module resolution

## One-line summary

The httpd `concurrent_test` failure `unknown function or operator
'thread-spawn-fn'` is **not** a stdlib rename/removal. `thread-spawn-fn` is
defined and exported by `stdlib/thread.tur` (`defn thread-spawn-fn
[fn-ptr : ptr<void> arg : ptr<void>] #{Unsafe} : ThreadHandle`,
`stdlib/thread.tur:49`) and is unchanged in git history (the only commit
touching the file is #409, a diag-state reset, unrelated to the API). The
"unknown function" error means `stdlib/thread.tur` is simply not being loaded
into the translation unit that references `thread-spawn-fn`.

## Why it surfaces

`thread.tur` is deliberately **not** in the compiler's auto-load
`stdlib_files[]` set. See `src/main.c:731-736`:

> Phase T19-C/D stdlib files (mutex, rwlock, condvar, sync, thread, chan,
> atomic) are NOT auto-loaded here to avoid polluting every program's
> generated C ... They are library files usable via `tur build <dir>` when
> placed next to user code ... An explicit `require` or module mechanism
> (planned post-T21) will provide auto-loading later.

So `thread-spawn-fn` is only in scope when the program explicitly pulls in the
thread module (via `(load "stdlib/thread.tur")`, a `:spices`/path dep that
brings the module onto the resolution search path, or a sibling copy in the
spice's own `src/`).

## Confirmation

```turmeric
;; No import -> reproduces the httpd error verbatim:
(defn worker [arg : ptr<void>] : ptr<void> ```c return NULL; ```)
(defn main [] : int
  (let [t (thread-spawn-fn worker (nullp))] (thread-join t)) 0)
;; => error: unknown function or operator 'thread-spawn-fn'

;; With the module loaded, the name resolves (next diagnostic is the
;; expected Unsafe-context check, proving resolution succeeded):
(load "stdlib/thread.tur")
;; => error: unsafe function 'thread-spawn-fn' requires an enclosing (unsafe ...)
```

## Root cause / fix directions

This is a spice-build resolution issue, not a compiler defect, but it is worth
recording because the symptom ("unknown function") misleads toward a phantom
stdlib rename:

1. **httpd side (most likely):** ensure `concurrent_test` actually brings the
   thread module into scope -- an `(import ...)` / `:spices` dep / `(load
   "stdlib/thread.tur")` that resolves under `tur test spices/httpd/...`. If
   httpd previously passed this test, check whether a recent change to its
   `build.tur` (`:spices` list, `:path` dep, or `src/` layout) dropped the
   thread module from the resolution path.
2. **Compiler side (worth verifying):** confirm `tur test`/`tur build` project
   mode still threads stdlib library modules (the non-auto-loaded T19-C/D set)
   onto the include/resolution path when a manifest references them. If a recent
   build-descent / spice-resolution change stopped surfacing
   `stdlib/thread.tur` to consumers, that IS a compiler-side regression -- but
   the symbol itself is present and correct.
3. **Longer term:** the planned post-T21 `require`/module mechanism (per the
   `src/main.c` comment) would make `thread-spawn-fn` available without the
   manual load and remove this footgun.

Cannot fully localize from this checkout: the httpd spice
(`../turmeric-spices/`) is not present in this container, so the exact
import/manifest wiring of `concurrent_test` could not be inspected. The
compiler-side facts above (symbol exists, resolves when loaded, is not
auto-loaded) are verified against this tree.
