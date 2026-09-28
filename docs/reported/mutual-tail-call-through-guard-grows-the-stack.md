# A mutual tail call through a `guard` procedure grows the C stack

**Severity:** low-medium. `#lang r7rs` (any dialect whose `guard`-like catch
makes a function CPS while its partner stays direct). The C stack grows by
three frames on every round trip at every optimization level, so a
mutual-recursion loop through a procedure that holds a `guard` overflows at
tens of thousands of iterations. The same loop through ONE procedure is
constant stack. Filed 2026-09-28 while resolving
[cps-self-tail-call-relies-on-sibling-call](../archive/cps-self-tail-call-relies-on-sibling-call.md).

## Repro

```scheme
#lang r7rs
(import (scheme base) (scheme write))
(define (g1 f n) (if (= n 0) 'g-done (g2 f (- n 1))))
(define (g2 f n) (guard (e (#t 'caught)) (f n)) (g1 f (- n 1)))
(write (g1 (lambda (x) (if (= x 5) (raise 'boom) x)) 30000))
(newline)
```

Measured 2026-09-28, gcc 13, 8 MiB stack:

| count | `-O0` | `-O2` (the default) |
|---|---|---|
| 20,000 | `g-done` | `g-done` |
| 30,000 | segfault | `g-done` |
| 50,000 | segfault | `g-done` |
| 70,000 | segfault | segfault |

The one-procedure version,
`(define (g f n) (guard (e (#t 'caught)) (f n)) (if (= n 0) 'd (g f (- n 1))))`,
runs 100,000 deep at `-O0`: its self tail call is the CPS backedge.

## Root cause

The two procedures get different lowerings, so neither the CPS group fusion
(`emit_cps_ir.c`, "CPS mutual tail-call groups") nor T5's direct groups
(`emit_fns.c`, `tcg_member_ok`) can take the pair:

- `g2` is CPS. `guard` lowers to a `call/ec`-style escape
  (`r7rs-call-slec`), which is effectful.
- `g1` is direct. It only tail-calls `g2`, and a call to a CPS function from
  direct code goes through the callee's direct entry wrapper.

So one round trip is three C frames, and none of the calls is a tail call:

1. `g1` calls `g2` -- the direct wrapper, which pushes a `dk_prompt`, saves
   `g_dk_driver` and `TUR_SETJMP`s before calling `g2__cps`. A function that
   calls `setjmp` cannot make a sibling call.
2. `g2__cps` calls `g1` as `cps->direct`:
   `tur_tagged_t t = g1(f, n1); return dk_run(__kont, box(t));`. The result
   is boxed and handed to the continuation, so this is not a tail call.
3. `g1` repeats.

In the emitted C of the repro: `g1` ends `return g2(f__v0, __ps_...)`, and
`g2__cps` has `__t = g1(f__v2, __t0); /* cps->direct */` followed by
`return dk_run(__kont, ...)`.

## Fix directions

- **Color the partner.** A direct function whose tail call reaches a CPS
  function inside the same tail-call cycle could be emitted CPS too. Then
  the pair is a cps->cps cycle and the existing group fusion takes it (if
  `g2`'s body, with its closure-env thunk for the `guard`, qualifies as a
  member -- check `ctg_member_ok`).
- **Make `cps->direct` in tail position a tail call to `dk_run` without the
  box.** This only removes one frame. The wrapper's `setjmp` frame remains,
  so on its own it is not enough.
- Whatever the fix, add the repro above at a million steps to
  `tests/fixtures/r7rs-cps-mutual-tail-unoptimized` (built at `-O0`), and
  delete the "tail call made after a `guard`" bullet from
  `docs/guides/r7rs-guide.md` ("Where it differs from R7RS").
