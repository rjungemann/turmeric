# Windows: `call/cc` and panic-in-fiber still `longjmp` on a fiber stack

> **RESOLVED 2026-09-04, and this report was WRONG about which sites are
> affected.** Every emitted setjmp/longjmp landing now goes through
> `TUR_SETJMP`/`TUR_LONGJMP`. Probing each site first showed that
> **panic-in-fiber does not reproduce** -- the compiled `catch-unwind` path
> is stackless and never longjmps -- while `call/cc` does. Enumerating
> properly also found FIVE such families, not the three listed below. See
> "Resolution" at the end.

**Summary:** Fixing the DK tail-resume landing
([archived report](windows-longjmp-across-fiber-stack-kills-effects.md))
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

- [../archive/windows-longjmp-across-fiber-stack-kills-effects.md](windows-longjmp-across-fiber-stack-kills-effects.md) -- the resolved DK site, with the measurements
- [jit-windows-support-spike.md](../reported/jit-windows-support-spike.md) -- the c2mir fork work this would ride on
- [docs/upcoming/v1/windows-remaining-plan.md](../upcoming/v1/windows-remaining-plan.md)


## Resolution (2026-09-04)

### What was actually broken

This report asserted all three sites were broken "by the same root cause,
unchanged". That was reasoning from the code, not measurement, and it was half
wrong. Probing each on Windows first:

| site | probe | result |
| --- | --- | --- |
| `call/cc` escape inside a fiber | escape invoked in a fiber body | **reproduces** -- exit 127, `0xc0000028` via `RtlUnwindEx` |
| panic inside a fiber, handler outside | `catch-unwind` around the resume | survives, exit 0 |
| panic inside a fiber, handler inside | `catch-unwind` in the fiber body | survives, exit 0 |

Panic does not go through `longjmp` on the compiled path at all -- it is
stackless, a `tur_panicking` flag with early returns, which is what the
`stackless-catch-unwind-*` fixture family is about. The `panic_jmpbuf` landing
exists but is only armed in narrower circumstances than assumed here.

### What was fixed

Enumerating the emitted landings rather than trusting this report's list turned
up **five** families, not three: the DK trampoline (already done), the `call/cc`
escape, shift/reset, the handler node, per-fiber panic recovery, and
cancellation. All now use the same pair.

`tur_dk_jmp_buf` / `TUR_DK_SETJMP` / `TUR_DK_LONGJMP` were renamed to
`tur_jmp_buf` / `TUR_SETJMP` / `TUR_LONGJMP` -- they stopped being DK-specific --
and the selection moved out of `emit_cps_runtime_prelude` into
`emit_tur_jmp_buf_prelude`, emitted **unconditionally** after `<setjmp.h>`.
That was the prerequisite this report flagged: the typedef lived inside a gated
prelude, and its new consumers are gated independently -- several can appear in
a program with no delimited control at all.

The S2 split constraint is unchanged: the choice is still made at emission via
`rt_split_canonical_emission()`, so both halves of a split program agree by
construction.

### Regression cover

`tests/fixtures/callcc-in-fiber` is new and is the thing this report said was
missing. It asserts both the escaped value and the fiber's return to its
resumer -- the first proves the jump landed, the second that it did not corrupt
the fiber's return path.

There is still **no fixture** for shift/reset, the handler node, or cancellation
inside a fiber. Those three were converted on the strength of the shared
mechanism, not a reproduction, so they are covered by reasoning rather than by a
test. If one of them turns out to have its own stackless path like panic did,
the conversion is harmless there.
