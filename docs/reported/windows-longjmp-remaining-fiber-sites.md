# Windows: `call/cc` and panic-in-fiber still `longjmp` on a fiber stack

**Summary:** Fixing the DK tail-resume landing
([archived report](../archive/windows-longjmp-across-fiber-stack-kills-effects.md))
converted one of four emitted `longjmp` sites. The other three still use libc
`longjmp`, so each dies with `STATUS_BAD_STACK` (0xc0000028) if it fires on a
fiber stack -- the same root cause, unchanged.

**Severity:** Low-to-medium, and lower than the resolved one. No fixture
currently covers these combinations, so nothing is red; the failure mode is a
hard process abort, not a wrong answer. It becomes medium the moment someone
writes `call/cc` inside a fiber, or a fiber that panics.

**Platform:** Windows only.

## The remaining sites

All in the emitted preamble. Line numbers are from `tur emit-c` on
`tests/fixtures/fiber-effect/input.tur`:

| site | emitter | what fires it |
| --- | --- | --- |
| `tur_escape_resume`: `longjmp(cc->buf, 1)` | `emit_cps_callcc_prelude` (emit_dk_runtime.c:39) | invoking a `call/cc` / `escape` continuation |
| panic in a fiber: `longjmp(tur_current_fiber->panic_jmpbuf, 1)` | emit_module.c | a panic raised inside a fiber body |
| cancellation: `longjmp(tur_cancel_jmpbuf, 1)` | emit_module.c | cancelling a fiber/task |

The paired `setjmp`s (`setjmp(f->panic_jmpbuf)` in the fiber shim,
`setjmp(cc->buf)` at the call/cc site) sit on whichever stack their frame is
on, so a fiber-resident pair is fiber-stack -> fiber-stack -- exactly the shape
proven fatal in the resolved report.

## Repro

None committed. The gap is that no fixture combines these with a fiber; that
absence is itself worth fixing. A minimal case:

```turmeric
;; panic inside a fiber body -- expected to unwind to the fiber's
;; panic_jmpbuf, currently expected to die with STATUS_BAD_STACK on Windows
(defn fiber-body [] : nil
  (panic! "boom"))
```

## Why they were not fixed with the DK site

Deliberate scoping, not oversight. The DK fix was verified by two fixtures that
go red-to-green; these three have no such coverage, so converting them would
have been an unverifiable change riding along with a verified one. They also
are not all the same shape: `panic_jmpbuf` is a struct field with a declared
type (`jmp_buf` inside `FiberBlock`), so converting it changes a struct layout
rather than a local.

## Fix direction

The mechanism is already in the tree -- reuse it. `tur_dk_jmp_buf` /
`TUR_DK_SETJMP` / `TUR_DK_LONGJMP` are emitted in the CPS runtime prelude and
resolve to `__builtin_setjmp`/`__builtin_longjmp` on Windows.

Two things to respect, both learned the hard way on the DK site:

1. **The type is emitted by the CPS prelude**, which is gated on the program
   using delimited control. `call/cc` and fibers can appear without it, so
   either hoist the typedef+macros to an ungated part of the preamble or give
   these sites their own copy. Check the gate before assuming the name is in
   scope.
2. **Do not make the choice with `#if defined(__GNUC__)`.** Under the S2 split
   the runtime half is compiled by GCC and the program half by c2mir, which
   has neither `__builtin_setjmp` nor `__GNUC__`; the two halves then disagree
   and, because setjmp/longjmp must pair, the program exits 0 having printed
   nothing. Follow the DK site: decide at emission via
   `rt_split_canonical_emission()` and bake in a literal.

Worth pairing with fixtures for each combination (panic in fiber, `call/cc` in
fiber, cancel in fiber) -- the absence of coverage is why these were invisible
until the DK site was investigated.

The real lift for all of it is teaching c2mir `__builtin_setjmp` /
`__builtin_longjmp` in the `rjungemann/mir` fork, which would let the split
path use the builtins too and retire the special case entirely.

## Related

- [../archive/windows-longjmp-across-fiber-stack-kills-effects.md](../archive/windows-longjmp-across-fiber-stack-kills-effects.md) -- the resolved DK site, with the measurements
- [jit-windows-support-spike.md](jit-windows-support-spike.md) -- the c2mir fork work this would ride on
- [docs/upcoming/v1/windows-remaining-plan.md](../upcoming/v1/windows-remaining-plan.md)
