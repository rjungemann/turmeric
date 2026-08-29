# Bare top-level forms in a `defmodule` are elaborated, diagnosed, and then discarded

**Severity: high** (code that looks live, type-checks like live code, and never
runs -- no diagnostic). Found 2026-08-28 getting `turmeric-spices` CI green,
against `tur v0.40.0` / turmeric `5c9d533`.

**Verified 2026-08-28** on a freshly built `v0.40.0` (macOS arm64 / Apple
clang). Both halves confirmed: the drop, and the fact that the dropped form is
still fully type-checked.

## Summary

A non-definition form that is a direct child of `defmodule` is elaborated --
type errors inside it *are* reported -- and then dropped from codegen with no
warning.

## Repro

```turmeric
(defmodule dt (export)
  (defn shout [] : int ```c #include <stdio.h>
    printf("SIDE EFFECT RAN\n"); return 1; ```)

  ;; A bare expression as a direct child of defmodule.
  (shout)

  (defn main [] : int 0))
```

`tur check` is silent; the program runs and prints nothing. `dt__shout` is
declared and defined in the emitted C and **called from nowhere**:

```
$ tur emit-c dt.tur | grep -n dt__shout
4380:static int64_t dt__shout();
7908:static int64_t dt__shout() {
```

## The half that makes it expensive

The dropped form is not skipped -- it is fully elaborated first. A type error
inside it produces a normal, correct, well-formatted diagnostic:

```turmeric
(defmodule dt2 (export)
  (defn shout [x : int] : int ```c return x; ```)
  (shout "not an int")          ;; dropped -- but still type-checked
  (defn main [] : int 0))
```

```
dt2.tur:3:10: error [TUR-E0001]: function 'shout' arg 1: expected int, got cstr
3 |   (shout "not an int")
  |          ^^^^^^^^^^^^
```

So the feedback loop actively misleads: you write the form, you get a type
error, you fix the type error, the file compiles -- and you have just carefully
debugged code that will never execute. Every signal the compiler gives says
"live code" except the one that matters.

This is the worst of both available behaviors. Rejecting the form would be
fine. Emitting it would be fine. Diagnosing it *as if* it were live and then
deleting it is strictly worse than either.

## Expected

Either:

1. **Reject it** -- `TUR-E....: expression at module top level is never
   evaluated; move it into a function` -- which is almost certainly right,
   since there is no way to write a module-level side effect today and no
   reason to believe an author who wrote one meant it to vanish; or
2. **Emit it**, as module static-init, alongside the `def` initializers that
   already run there.

(1) is the smaller change and the one that matches what authors actually hit.
If (2) is ever wanted, it should be a deliberate feature with a stated
evaluation order, not the accidental reading of a silent drop.

## Where it bit

This is the expensive one. **109 `(describe ...)` blocks across 37 test files
in 12 spices** sat at `defmodule` top level. Every one was dropped, so those
suites printed

```
# All 0 tests passed.
1..0
```

and exited 0.

Eight spices -- `c-dsl`, `osc`, `png`, `rtaudio`, `rtmidi`, `valkey`, `wav`,
`zlib` -- were passing CI **vacuously**. Turning them on immediately surfaced a
real bug in `c-dsl`: `c-for1` emitted `for (:int i = 0; ...)`, a Turmeric type
keyword where a C type belongs. Its test had always asserted that correctly and
had never once run.

Note the interaction with the test-harness shape: a suite that reports "0 tests
passed" and exits 0 is indistinguishable from a passing suite to CI. That is a
second, independent problem worth fixing on the harness side (a suite that
registers zero tests should be a failure), and it is what let this sit for as
long as it did.

## Fix direction

The drop is presumably in the `defmodule` walker: it dispatches on the head
symbol, has arms for `defn`/`defmacro`/`defstruct`/`definstance`/`defopaque`/
`def`/`import`/`export`, and lets anything else fall through after elaborating
it. Find that fall-through and make it a diagnostic.

Check the same walker for `load`/`extern-c`-style forms before erroring
unconditionally -- there is a set of legitimately-not-a-definition forms that
must keep working, so the reject needs an allowlist rather than a bare `else`.

Worth checking at the same time whether the interpreter agrees; the archived
[`turi-toplevel-expr-subforms-elaborate-in-global-scope`](../archive/turi-toplevel-expr-subforms-elaborate-in-global-scope.md)
covers adjacent ground on the `--interpret` path, and a fix here should not
open a new compiled/interpreted divergence.

## Guides to update when fixed

- docs/guides/syntax-guide.md -- state what may appear as a direct child of
  `defmodule`.
- docs/guides/developing-spices-guide.md -- the test-suite section, if the
  "zero tests registered is a pass" harness behavior is fixed alongside.
