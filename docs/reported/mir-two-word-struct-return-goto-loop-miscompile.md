# MIR miscompiles 2-word struct return in an if/else + goto-backedge CFG

**Severity: high for `tur jit`, upstream defect.** Reproduces at MIR upstream
master tip (`a8ab7c3`, current as of 2026-07-29), at `-O0` through `-O2`.
Not a Turmeric codegen bug -- the repro is 12 lines of standalone, fully
conforming C.

## Repro

```c
#include <stdio.h>
#include <stdint.h>
typedef struct { int64_t lo; int64_t hi; } Box;
static Box go(int64_t n, int64_t i, Box acc) {
  tail:;
  if (i >= n) { return acc; }
  else {
    acc.lo = acc.lo + 1;
    i = i + 1;
    goto tail;
  }
}
int main(void) {
  Box s; s.lo = 10; s.hi = 99;
  Box r = go(5, 0, s);
  printf("lo=%lld hi=%lld\n", (long long)r.lo, (long long)r.hi);
  return 0;
}
```

| | lo | hi |
|---|---|---|
| gcc / clang | 15 | 99 |
| c2mir + MIR-gen (x86-64 SysV) | **99** | 99 |

**Both halves of the two-register return carry the second word** -- the 16-byte
by-value struct comes back as `{hi, hi}`. It is not a lost field update (that
would print `lo=10`).

## Boundary conditions, each verified by a one-line change

- `while (i < n) { ... }` instead of the label/goto: **correct**.
- Same body without the `else` (early return, then plain block): **correct**.
- Plain self-recursion instead of the goto loop: **correct**.
- Optimization level: fails identically at `-O0`, `-O1`, `-O2`.
- `static` vs external linkage on `go`: no effect.

So the trigger is specifically: two-word by-value struct return + early
`return` in an `if` **with an `else` arm** + back-edge `goto` to a label
preceding the `if`.

## Why this matters for `tur jit`

That exact CFG is the emitted C's standard **tail-call loop** for a
self-recursive function returning a carrier `:copy` struct
(`__tur_tailcall:; if (...) { return acc; } else { ...; goto __tur_tailcall; }`,
`emit_fns.c` tail lowering). Every such function is miscompiled under the JIT.
Corpus impact today is one fixture (`self-recursive-carrier-struct-return`,
which exists precisely to pin this shape), but the pattern is general.

## Status / next steps

- Verified present at upstream master tip; nothing newer to test against.
- This wants an upstream issue on `vnmakarov/mir` with the C repro above.
  Not filed from here -- opening issues on an external tracker is an
  outward-facing action to confirm first.
- J1 options while upstream is pending: none good at the emitter level
  (emitting `while` instead of the goto loop would sidestep this instance,
  but reshaping codegen around an engine bug inverts the dependency) --
  prefer pinning MIR to a fixed commit once upstream lands a fix, and
  carrying this fixture as a known-fail with a pointer here.

## Provenance

JIT findings 11.7 wrong-output list, second of three investigations.
Bisected from `tests/fixtures/self-recursive-carrier-struct-return` (JIT
prints 99 for `(.lo r)` where 15 is expected) down to the 12-line repro by
removing the ctor call, the panic check, and every temp one at a time --
each step re-verified against both compilers.
