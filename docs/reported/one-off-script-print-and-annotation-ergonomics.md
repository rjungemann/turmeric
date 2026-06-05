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
- **Findings 2 & 3 -- open.** Auto-loading / off-tree resolution of the named
  print/convert helpers (`float->int`, `println-float`, ...) are discoverability
  and module-resolution enhancements, not defects, and remain to be scheduled.

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
