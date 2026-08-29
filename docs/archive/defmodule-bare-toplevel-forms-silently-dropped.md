# Bare top-level forms in a `defmodule` are elaborated, diagnosed, and then discarded

**Severity: high** (code that looks live, type-checks like live code, and never
runs -- no diagnostic). Found 2026-08-28 getting `turmeric-spices` CI green,
against `tur v0.40.0` / turmeric `5c9d533`.

**Verified 2026-08-28** on a freshly built `v0.40.0` (macOS arm64 / Apple
clang). Both halves confirmed: the drop, and the fact that the dropped form is
still fully type-checked.

**Status: RESOLVED 2026-08-29** as TUR-E0711, taking option (1) below. See
[Resolution](#resolution) at the end -- including the two allowlist entries
that would have made a naive reject wrong.

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

---

## Resolution

Fixed 2026-08-29 as **TUR-E0711**, taking option (1) -- reject. This report's
analysis needed no correcting; the only things it could not know were where the
drop actually happens and what the allowlist has to contain.

### Where the drop was

Not in a fall-through arm of the `defmodule` walker. The walker elaborates
every body form and keeps the resulting `Expr` in `mod->body` -- but **the
emitter never reads `mod->body` at all.** It works off the definitions each
form registered globally as a side effect of elaborating (`FnDef` and friends),
which is why `dt__shout` is emitted and called from nowhere, and why the
diagnostics land normally: elaboration is the only pass that ever looks at the
form. There was no `else` to turn into a diagnostic; the check is new code.

### The allowlist -- two entries that are not definitions

The report was right to warn that a bare `else` would be wrong. Dumping the
head symbol of every `defmodule` direct child across `stdlib/`,
`tests/fixtures/` and `examples/` (3014 files) found two non-definition forms
that are legal, in use, and covered by existing fixtures:

- **A bare ` ```c ` block** supplies file-scope C declarations --
  `tests/fixtures/inline-c-file-scope-per-decl-dedup`, `stdlib/time.tur`.
- **`defer` at module top level** is a real feature: it runs at process exit,
  pinned by `tests/fixtures/module-defer-basic` (prints `hello` then
  `goodbye`).

`import` / `export` / `export-from` never reach the body loop -- a header scan
consumes them and `break`s at the first other form, which is what defines
`body_start`.

### Testing the elaborated Expr, not the head symbol

The check runs on the elaborated expression, and that choice is load-bearing:

```turmeric
(defmacro make-adder [name n]
  `(defn ,name [x : int] : int (+ x ,n)))

(make-adder add5 5)     ;; head is neither `defn` nor a `def*` name
```

A head-symbol allowlist rejects this. Going through elaboration also keeps the
allow-set small, because the registering forms have already collapsed by then:
`defmacro`, `defclass` and `deftype` all return `EX_NIL_LIT` once their
definition is recorded. The measured set is `EX_NIL_LIT`, `EX_DEF`,
`EX_FN_DEF`, `EX_DEFDATA`, `EX_DEFECT`, `EX_EXTERN_C`, `EX_INLINE_C`,
`EX_DEFER`, plus `EX_TYPECLASS_DEF` / `EX_INSTANCE_DEF` which are definitions
by construction. Everything else is rejected.

`EX_NIL_LIT` admits a bare `nil`, which is inert either way.

### Verification

A sweep of all 3014 `.tur` files in the tree produces **zero** TUR-E0711 hits,
so the allowlist is complete for everything in-repo. Suite: 2722 passed, 0
failed. The interpreter agrees (it shares the elaborator), so the divergence
this report asked about does not open --
`tur --interpret` emits the identical diagnostic.

### Regression

- `tests/fixtures/errors/module-toplevel-expression/` -- the reject.
- `tests/fixtures/module-toplevel-definition-forms/` -- the allow-set in one
  module, including the macro-expands-to-`defn` case. This is the half a later
  tightening would break silently, so it is pinned explicitly.

### Residual

When the dropped form *also* fails to elaborate, only its own error is
reported (elaboration returns NULL before the position check can run), so the
`dt2` case in this report still shows just the TUR-E0001. The author fixes the
type error and gets TUR-E0711 on the next round rather than both at once. Left
as is: telling a form that failed to elaborate "this would have been dropped"
requires guessing whether it was meant to be a definition, and a macro whose
expansion had a type error would get a wrong note. The expensive half -- silent
loss of *correct* code -- is fully closed.

### Not done

The report's second, independent finding stands untouched: a test harness that
registers zero tests reports `# All 0 tests passed. / 1..0` and exits 0, which
is indistinguishable from a passing suite. That is what let this sit for as
long as it did, and it is a spice-repo/harness change, not a compiler one.

### Guide

`docs/guides/syntax-guide.md` gains "What may appear inside a `defmodule`" --
the full legal table, the two deliberate non-definition entries, and the note
that the rule applies after macro expansion. The
`developing-spices-guide` test-suite change is left with the harness fix above.
