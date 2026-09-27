# `#lang r7rs`: a program that raises does not build with clang on x86-64

**Severity:** medium. Every `#lang r7rs` program that reaches `raise` (so
`error`, `guard`'s re-raise, every `test-error` in an SRFI suite) fails to
build with clang on x86-64 Linux. clang's backend aborts:

```
fatal error: error in backend: failed to perform tail call elimination on a call site marked musttail
```

CI does not see it: Linux CI compiles emitted C with gcc, where
`TUR_MUSTTAIL` expands to nothing, and macOS CI is arm64, where the same C
builds. A developer on an x86-64 Linux box with `CC=clang` hits it at once.

Filed 2026-09-27 while landing r7rs-srfi-plan S3.

## Repro

```scheme
#lang r7rs
(import (scheme base) (scheme write))
(write (guard (e (#t (list 'caught e))) (raise 'x)))
(newline)
```

`CC=clang tur build r.scm` (clang 18, x86-64) fails as above. `tur emit-c`
of the same program compiles with clang at every `-O` level: the difference
is the call/cc runtime `tur build` pastes ahead of the unit.

## Root cause (partly confirmed)

Bisecting the 108 `TUR_MUSTTAIL` sites in SRFI 26's suite program, turning
the attribute on for one half of the sites at a time, found one:

```c
static tur_tagged_t r7rs_hyraise(tur_tagged_t obj) {
        ...
        if (__ps_2600) {
            TUR_MUSTTAIL return r7rs_hyuncaught_un_un(obj);
```

The prototypes match (`tur_tagged_t (tur_tagged_t)`, a 16-byte struct in two
registers), so the call itself is eligible. It fails only at `-O1` and
above, and only with the pasted runtime, whose `TUR_SETJMP` is
`__builtin_setjmp`. The likely mechanism: `r7rs_hyraise` calls
`r7rs_hyraise_hycontinuable`, whose direct entry is a DK wrapper that does
`TUR_SETJMP`. The inliner inlines it, since `__builtin_setjmp` does not carry
the `returns_twice` attribute that stops `setjmp` from being inlined. A
function holding an SjLj setjmp cannot make a guaranteed tail call on
x86-64. This is not yet proven: check it with `-fno-inline` on the pasted
build, or `__attribute__((noinline))` on the wrapper.

## Fix directions

- Mark every emitted function that calls `TUR_SETJMP` (the DK entry
  wrappers, `r7k_run_form`) `__attribute__((noinline))`. A wrapper around a
  setjmp gains nothing from inlining, and it keeps the setjmp out of
  `musttail` callers. This changes every snapshot with a DK wrapper.
- Or have the preamble's `TUR_MUSTTAIL` gate also require that the unit does
  not paste the SjLj runtime on x86-64, which gives up guaranteed tail
  calls for every Scheme program there.
- A fixture: add `requires.musttail` to a raising `#lang r7rs` program, so
  a clang x86-64 run of the suite covers it.
