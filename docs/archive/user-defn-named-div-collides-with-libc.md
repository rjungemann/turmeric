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

---

## Resolution (2026-08-21)

Fixed by **fix direction 1**, which turned out to be already half-built. Not
fix direction 2 -- see the end.

### The guard already existed; its list was the problem

`tur_name_collides_libc()` (src/compiler/mangle.c) and the `tur_u_` guard
prefix have been in the tree since
`codegen-user-defn-collides-with-libc-pipe2`. A bare top-level global whose
name matches gets its own C symbol, applied at the definition and every use
through one chokepoint. `(defn strlen ...)` already emitted
`static double tur_u_strlen(double, double)`.

`div` simply was not in `libc_names[]`, whose own comment described it as
"grown on demand" -- and a list grown on demand only ever learns the names
someone happened to try.

### The report's "scope is narrower than any libc name" is wrong

The filing tested six names (`abs`, `remainder`, `index`, `time`, `exit`,
`free`), found all six clean, and concluded `div` was close to unique.
Those six are clean because **five of them were already in the table**
(`abs`, `index`, `time`, `exit`, `free`); `remainder` is in `<math.h>`, which
the generated TU does not include.

A 104-name sweep found **12** breaking, not one:

```
div  ldiv  lldiv  llabs  atexit  putchar  getchar  gets  chown  execl
drand48  erand48
```

(`open` and `select` also fail, but as *turmeric* diagnostics -- they are
special forms. That is correct behavior and a different thing.)

### The list is derived now, not grown

`libc_names[]` is now the set of lowercase identifiers **declared by the
headers the generated TU includes** -- stdio, stdlib, string, time, unistd,
fcntl, errno, setjmp, pthread, ucontext, sys/select, sys/socket, netinet/in,
arpa/inet -- minus C keywords (`tur_name_is_c_keyword`'s job) and typedefs.
136 entries -> **713**, all 136 originals preserved. The regeneration command
is in the comment above the table.

**`gets` is why a plain header scrape is not enough.** glibc declares it only
from `<bits/stdio2.h>`, which `<stdio.h>` includes only when
`__USE_FORTIFY_LEVEL > 0` -- i.e. only under `-O2`, which is exactly what
`tur build` passes and what a bare `cc -E` does not. So `(defn gets ...)`
compiled clean through `tur emit-c | cc` and failed under `tur run`. The
derivation unions across the flag sets `tur build` compiles under.

Over-matching is harmless -- a non-colliding name just gets its own `tur_u_`
symbol -- so the table errs toward inclusion.

### Churn: none

Of 713 names, exactly one (`gets`) is used as a top-level `defn` anywhere in
`stdlib/` or `tests/fixtures/`, and it is *parametric*
(`conv-heap-adt-typed-pointer`), so it monomorphizes to
`gets__spec__...` and never reaches the guard. Suite went
`2687 passed, 0 failed` -> **2688 passed, 0 failed** with the new fixture and
no snapshot regenerated.

### Tests

- `tests/fixtures/libc-collision-guard/` -- all 12 previously-breaking names
  plus 3 already-covered ones, as user `defn`s that compile, run, and return
  the user's answer (`7.1 / 2.5 = 2.84`, which libc's same-named symbol could
  not produce). `expected.c` pins the `tur_u_` spelling.
- `tests/mangle_test.c` -- direct oracle for `tur_name_collides_libc`: the 12
  hits, the 3 legacy hits, non-colliding names that must NOT be guarded, and
  the whole-identifier slice guard.
- `tests/check-libc-collision-list.sh` (ctest `tur_libc_collision_list`) --
  **the durable part.** The table is bsearch'd, so its sort order is a
  correctness precondition: one entry out of place makes bsearch silently miss
  names, and the symptom is not a failed lookup but the original cc cascade.
  The lint checks sortedness, uniqueness, and disjointness from `c_keywords[]`.
  Verified to fail on a deliberately transposed pair.

### Why not fix direction 2

The report proposed also reserving these names in the front end, so the user
gets a turmeric diagnostic instead of cc errors. That is now the wrong move:
these names **work**. Rejecting `(defn div ...)` would take away a name the
guard makes perfectly usable, trading a fixed bug for a new restriction.

### What is left

The guard is keyed to the host's libc. A user linking a third-party C library
whose symbol collides is still unprotected, and the general answer to that
remains what the report called the real fix: mangle every emitted user symbol
unconditionally. That stays its own change -- it interacts with `extern-c` and
inline-C blocks that reference turmeric functions by their unmangled spelling,
which is the part this fix deliberately does not disturb.
