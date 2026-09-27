# `#lang r7rs`: a program that raises does not build with clang on x86-64

**RESOLVED 2026-09-27.** The report's suspect was wrong: it is not a
`__builtin_setjmp` wrapper inlined into the caller (on Linux `TUR_SETJMP` is
plain `setjmp`, and `-fno-inline` still fails). It is LLVM's dead argument
elimination rewriting a `static` function's return type underneath its own
`musttail` call. Fixed in src/compiler/emit_module.c
(`emit_musttail_pins`); pinned by `tests/fixtures/r7rs-raise-under-musttail`
(`requires.musttail`). The rest of this file is the original report.

## Root cause (confirmed)

`opt -O2 -verify-each` over the unoptimized IR of the repro stops right after
`DeadArgumentEliminationPass`:

```
cannot guarantee tail call due to mismatched return types
  %10 = musttail call { i64, i64 } @r7rs_hyuncaught_un_un(i64 ..., i64 ...)
```

`r7rs_hyraise` returns a `tur_tagged_t`, two words in two registers. Its
callers only ever test the tag, so the pass narrows its return to the first
word, `i64`. Its `musttail` callee, `r7rs_hyuncaught_un_un`, keeps both. A
`musttail` call must return exactly what its caller does, so the backend
aborts. The pass is meant to leave such a pair alone (clang 18.1.3 does not),
and it can only touch a function whose every use is a direct call: a function
whose address is taken keeps its signature.

Why only with `tur build`'s pasted call/cc runtime: `tur emit-c` of the same
program does not compile at all on its own (the pasted text defines
`r7k_cont`), so the report's "emit-c compiles" was a different failure.
Bisecting the 98 `TUR_MUSTTAIL` sites of the repro found the one site the
report found.

## Fix

The emitter records every function that makes a `TUR_MUSTTAIL` call, and at
the end of the unit writes one table of their addresses:

```c
#ifdef TUR_MUSTTAIL_PINS
static void (*const __tur_musttail_pins[])(void) __attribute__((used)) = {
    (void (*)(void))r7rs_hyraise,
    ...
};
#endif
```

`TUR_MUSTTAIL_PINS` is defined next to `TUR_MUSTTAIL`, only where that
expands to the attribute (clang on x86-64/aarch64), so gcc, c2mir and wasm
see nothing and still drop an unused function. With its address taken, a
pinned function keeps its signature, and its `musttail` callee's return value
stays live through it. The 155 snapshots that carry `TUR_MUSTTAIL` gain the
define and the table, nothing else.

---

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
