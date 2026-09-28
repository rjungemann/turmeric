# `stdlib/math.tur`'s wrappers call themselves where the math builtin needs errno

**Severity:** high on clang, low on gcc. **RESOLVED 2026-09-27**, the day it was
found, in src/compiler/mangle.c. Pinned by
`tests/fixtures/stdlib-math-errno-path`.

Found while checking the clang fix for
[r7rs-raise-musttail-fails-under-clang-x86-64](r7rs-raise-musttail-fails-under-clang-x86-64.md):
once Scheme programs built under clang, `r7rs-rationals`, `r7rs-complex`,
`r7rs-numbers`, `r7rs-srfi-48` and others were wrong or hung there, the same
way with `TUR_MUSTTAIL` switched off.

## Repro

```turmeric
(load "stdlib/math.tur")
(defn main [] : int
  (let [k (if (= (list-length *args*) 0) 1.0 2.0)]
    (println (sqrt (* k 2.25))))
  0)
```

| compiler | before | after |
|---|---|---|
| gcc -O2 | 1.5 | 1.5 |
| gcc -O0 | segfault | 1.5 |
| clang -O2 (x86-64 Linux) | `8.25667e-317` (or 0.0) | 1.5 |
| clang -O0 | segfault | 1.5 |

A negative argument (`(sqrt -4.0)`) hung clang -O2 outright. Under
`#lang r7rs`, `(sqrt 2.25)` was `0.0` and every inexact root, `expt` with a
ratio exponent, and complex result built from them was wrong.

## Root cause

The emitted TU does not include `<math.h>`, and the mangler kept a stdlib
`defn sqrt` as a C function named `sqrt`:

```c
static double sqrt(double x) { return __builtin_sqrt(x); }
```

A math builtin is not always an instruction. Where it must be able to set
`errno` (a domain error; everything at `-O0`; always under clang on Linux,
which keeps `-fmath-errno`) the compiler emits a call to the libm function of
the same name, and in this TU that name is the wrapper. At `-O0` it recursed
until the stack ran out. clang at `-O2` saw infinite recursion, treated it as
undefined, and returned whatever was in `xmm0`. gcc at `-O2` got away with it.
macOS CI is clang on Darwin, which defaults to `-fno-math-errno`, so it never
saw it either.

The fixture `jit-win-prelude-name-shadow` said these names were "NOT in
mangle.c's libc denylist, correctly -- the emitted TU never includes math.h,
so the cc path only ever sees them as gcc builtins". That missed the builtin's
libcall.

## Fix

`tur_name_collides_libc` also consults a second list: every lowercase
function `<math.h>` declares (derived with the same recipe as the libc list,
minus the TS 18661-3 `_FloatN` spellings). A bare global so named is emitted
as `tur_u_<name>`, so the wrapper is `tur_u_sqrt` and its builtin's libcall
reaches libm (`-lm` is on every link line). A user `defn` named `log2` or
`round` gets the same prefix, which is harmless. Three snapshots changed.
