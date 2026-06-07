---
title: One-off `.tur` script ergonomics -- misleading effect-annotation diagnostic and undiscoverable print/convert helpers
category: Reported
severity: low
description: Writing a small freestanding `.tur` script to print a runtime value (the everyday "let me probe this with tur run" loop) hits three avoidable papercuts. (1) Putting the `#{Effect}` annotation *after* the return type -- `: int #{Unsafe}` instead of `#{Unsafe} : int` -- is reported as "map literals are parsed but not yet supported by elaboration", which points the user at data-literals, not at the real cause (annotation ordering / `#{...}` reader collision). (2) `float->int`, `println-int`, `println-float`, `float->cstr` are not auto-loaded and require explicit imports that do not resolve when the script lives outside the repo. (3) The net effect is that the fastest way to print a float is the bare `println`, which is correct but easy to overlook after several failed probes.
---

# One-off script print + annotation ergonomics

## Summary

The "scratch a `.tur` file in `/tmp`, `tur run` it, print a value" loop --
the bread-and-butter way to probe runtime behavior -- has a few sharp
edges that cost a round-trip each. None are miscompiles; all are
diagnostics / discoverability gaps. Filed while probing the let-bound SF
fix (`let-bound-sf-loses-outer-arg-type-when-inner-captures.md`), where
each of these cost an extra build.

Severity: **low** (ergonomics), but they recur on essentially every
freestanding probe.

## Status

- **Finding 1 -- FIXED.** A misplaced effect annotation (`: int #{Unsafe}`)
  is now detected in `defn` header parsing and reported with an
  ordering-specific diagnostic that names effects, not map literals. See
  `src/compiler/elab_fns.c` (the "misplaced effect annotation" check after
  return-type parsing) and the regression fixture
  `tests/fixtures/errors/effect-annotation-after-return-type/`.
- **Finding 2 -- PARTIALLY ADDRESSED + premise corrected.** The unknown-call
  diagnostic now suggests the exact `(load ...)` line for the well-known scalar
  print/convert helpers (`float->int`, `int->float`, `printf-float6`,
  `println-float`): e.g. for `float->int` it adds *"'float->int' lives in
  stdlib/math.tur and is not auto-loaded; try: (load "stdlib/math.tur")"*. See
  `stdlib_load_hint_file` in `src/compiler/elab_call.c` and the fixtures
  `tests/fixtures/errors/unknown-helper-load-hint/` (the hint) +
  `tests/fixtures/stdlib-float-convert-load/` (the suggested `load` works).

  **Correction to this report's premise:** Finding 2 originally framed these as
  needing an `(import ...)` line. That is wrong -- `stdlib/math.tur` and
  `stdlib/bits.tur` are bare definition files, *not* `defmodule`s with
  `(export ...)`, so `(import math :refer [float->int])` fails with "symbol
  'float->int' is not exported from module 'math'". The only working mechanism
  for these helpers in a freestanding script is `(load "stdlib/<file>")`, which
  is what the hint now suggests. (Whether math/bits *should* be promoted to
  exporting `defmodule`s is a separate design question, not pursued here.)

- **Finding 3 -- FIXED.** A freestanding `/tmp/foo.tur` that does
  `(load "stdlib/math.tur")` now resolves the stdlib off-tree. `import` already
  fell back to the resolved stdlib root (`TUR_STDLIB_DIR`, set to an absolute
  path by `main.c`'s `resolve_stdlib_root` via an exe-relative walk); `(load
  ...)` was purely cwd-relative and so failed when run from outside the repo.
  The load expander now mirrors the import fallback: a `"stdlib/<rest>"` path
  that is not found cwd-relative is retried under the resolved stdlib dir (the
  leading `stdlib/` component is dropped to avoid `.../stdlib/stdlib/...`). See
  the off-tree fallback in `src/compiler/elab_toplevel.c` (Phase M `(load ...)`
  expansion) and the regression harness `tests/run-offtree-load.sh` (registered
  as the `tur_offtree_load` ctest). With this, the `(load ...)` line that the
  Finding 2 hint suggests works from any cwd, not just the repo root.

## Finding 1 -- misleading diagnostic for a misplaced effect annotation

The canonical order is `#{Effect}` *before* the return type:

```turmeric
(defn raw [] #{Unsafe} : int   ;; OK
  ```c
  return 42;
  ```)
```

Writing it *after* the return type is a natural mistake, and the
diagnostic blames the wrong feature entirely:

```turmeric
(defn raw [] : int #{Unsafe}   ;; WRONG ORDER
  ...)
```

```
$ ./build/tur check /tmp/p3.tur
/tmp/p3.tur:1:21: error: phase 1: map literals are parsed but not yet
  supported by elaboration
```

Observed: the `#{...}` is read as a data/map literal (the reader macro
sits below the effect-annotation grammar), so the user is told map
literals are unsupported. Expected: a diagnostic that names the real
problem, e.g. *"effect annotation `#{Unsafe}` must precede the return
type; write `#{Unsafe} : int`"*, or simply accept the trailing-annotation
order. This is the most bug-like of the three -- the error actively
misdirects.

Root-cause pointer: the `#{` reader dispatch (data-literals) fires before
the defn header parser distinguishes an effect-set annotation from a set
literal in return-type position. A header-context check (an `#{...}` in
the signature slot is an effect set, not data) or an ordering-specific
hint would fix it.

## Finding 2 -- print/convert helpers are not auto-loaded and don't resolve off-tree

`println` is auto-available and prints both ints and floats with their
fractional part (`(println 7.25)` -> `7.25`, verified). But the *named*
helpers a user reaches for by analogy are not:

```
$ ./build/tur check /tmp/p4.tur
.../p4.tur:1:35: error: unknown function or operator 'float->int'
```

`float->int` lives in `stdlib/math.tur`, `println-float` in
`stdlib/bits.tur`, etc. They require an explicit `(import ...)`, and that
import does not resolve when the script lives outside the repo (no
cwd-relative `stdlib/`), so a `/tmp` probe that imports them fails to even
parse. The fast path -- bare `println` -- works, but only after the user
stops fighting the named helpers.

## Finding 3 -- net loop cost

Each of the above surfaces only after a failed `tur run`, so a "just print
this float" probe can take 3-4 compile cycles before landing on `(println
x)`. The fixes below would make the first attempt succeed.

## Proposed enhancements (plottable)

1. **Fix/clarify the effect-annotation diagnostic** (Finding 1) -- detect
   `#{...}` in signature position and either accept the trailing order or
   emit an annotation-specific error. *Highest value, most bug-like.*
2. **Auto-load the common scalar print/convert helpers** (or make the
   "unknown function" diagnostic for `float->int` / `println-int` /
   `println-float` suggest the owning module + import line). A
   "did you mean `(import math :refer [float->int])`?" hint would erase
   Finding 2.
3. **Resolve stdlib imports for freestanding scripts** from the installed
   stdlib (not just cwd-relative), so a `/tmp/foo.tur` that imports
   `math`/`bits` runs without `-I`.

## Validation of a fix

- `(defn f [] : int #{Unsafe} ...)` either compiles or yields an
  annotation-ordering diagnostic that mentions effects, not map literals.
- `float->int` used without an import produces a hint naming `math`.
- A `/tmp` script importing `bits`/`math` runs under `tur run` with no
  `-I` flag.

## Related

- `docs/reported/let-bound-sf-loses-outer-arg-type-when-inner-captures.md`
  -- the work that surfaced these papercuts.
