---
title: A user `defn` named `div` fails to compile -- names are emitted unmangled into the generated C
category: Report
description: Top-level defn names are emitted verbatim as C identifiers, so `(defn div ...)` collides with stdlib.h's div() and dies in cc with "static declaration follows non-static declaration". The user sees three C errors about code they did not write.
---

# A user `defn` named `div` fails to compile

**Severity:** low-to-medium. One confirmed unusable identifier, and a failure
mode that surfaces as C compiler errors in generated code rather than a
turmeric diagnostic -- so the user has no pointer back to their own source.
`div` is a plausible name for a numeric helper.

**Found:** 2026-08-20, by naming a two-line probe's helper `div`.

**Compiler:** `./build/tur` v0.37.0 (Debug).

## Repro

```turmeric
(defn div [a : float b : float] : float (/ a b))
(defn main [] : int
  (println (div 7.1 2.5))
  0)
```

```
$ tur run div.tur
/tmp/tur-build/div_tur.c:3718:15: error: static declaration of 'div' follows non-static declaration
/tmp/tur-build/div_tur.c:7176:15: error: static declaration of 'div' follows non-static declaration
/tmp/tur-build/div_tur.c:7195:16: error: initializing 'double' with an expression of incompatible type 'div_t'
3 errors generated.
tur: cc invocation failed (status 256)
```

Renaming to `fdiv` compiles and runs. Nothing about the program is otherwise
unusual -- no inline C, no interop, no `extern-c`.

## Root cause

Top-level `defn` names are emitted as C identifiers **verbatim**:

```
$ tur emit-c div.tur | grep -n 'static.*div('
4272:static int64_t div(int64_t);
7731:static int64_t div(int64_t a) {
```

`<stdlib.h>` is in scope in the generated translation unit and declares
`div_t div(int, int)`, so the forward declaration is a redeclaration conflict
and the call site then tries to initialize a `double` from a `div_t`.

## Scope is narrower than "any libc name"

Worth stating, because the obvious generalization does not hold. Tested the
same shape with `abs`, `remainder`, `index`, `time`, `exit`, and `free` as the
user function name -- **all six compile and run clean**; only `div`
reproduces. So something else differs between these cases (builtin shadowing,
which header ends up in the TU, or return-type compatibility letting the
redeclaration slide). Characterizing exactly which libc names are unusable is
part of the work here, not established by this report.

## Fix directions

Two levels, and they are not exclusive:

1. **Mangle emitted user symbols** (a `tur_u_` prefix, or the module path)
   so no user identifier can ever collide with a libc or runtime symbol.
   This is the real fix and it retires the whole class. It touches every
   name in codegen, so it wants its own change, and it interacts with
   `extern-c` / inline-C blocks that reference turmeric functions by name --
   those are exactly the sites that must keep the unmangled spelling.
2. **Reserve the colliding names in the front end** so the failure is a
   turmeric diagnostic pointing at the user's `defn` ("`div` is reserved --
   it collides with a C standard library symbol"), not three cc errors in a
   file under `/tmp`. Cheap, and it converts a confusing failure into a clear
   one even if (1) never happens.

(2) alone would have made this a 5-second fix for the user instead of a
puzzle, which is most of the cost here.

## Adjacent

Found alongside
`docs/reported/float-division-aborts-instead-of-ieee-inf.md` -- same probe
file, different bug.
