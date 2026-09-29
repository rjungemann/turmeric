# A mutual tail call through a `guard` procedure grows the C stack

**RESOLVED 2026-09-29** -- see [Resolution](#resolution-2026-09-29). The
repro runs a million deep at `-O0`, and so does the same pair with a
`(list ...)` base case. Pinned by `tests/fixtures/r7rs-cps-mutual-tail-unoptimized`,
built at `-O0` by its `hook.sh`.

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

## Resolution (2026-09-29)

The diagnosis above was right about the shape -- one CPS partner, one direct
one, so neither kind of group takes the pair -- but not about why `g1` was
direct. `g1` is CPS-colored: the coloring pass propagates backward from a
colored callee, and `g2` is one. It was **evicted** at emit time. Its CPS
translation had an unsupported node, and an evicted function falls back to the
direct emitter whole (`TUR_TRACE_EVICT=1` prints
`BODY-UNSUPPORTED g1 unsupported form: EX_#100`).

Node 100 is `EX_UNION_INJECT`, the `any` widen around `'g-done`. The widen was
already a delegated value op (`is_delegatable_struct`), but only over an
operand `operand_uses_control` can clear, and that scan did not know
`EX_SYM_LIT`, so its conservative default said "may hide a control op". So
the base case, not the tail call, kept `g1` out of CPS.

`src/passes/cps_ir.c`:

- `EX_SYM_LIT` is control-free in `operand_uses_control` and a delegated
  value in `is_delegatable_value`. A quoted symbol is the address of a static
  record.
- An indirect call's arguments may be widened literals, not only atoms
  (`call_args_literal`: an atom, a quoted symbol, either one widened to `any`,
  or a constructor over those). `#lang r7rs`'s `(list 1 2)` is an indirect
  call to a variadic procedure whose rest list is a `Cons` chain of widened
  literals, and the "indirect call (non-atomic args)" refusal evicted a base
  case written that way. The test is deliberately not "control-free": a call
  to a colored function passes that scan and must not be delegated.

With `g1` in the CPS set, the pair is a cps->cps tail cycle and the existing
group fusion (`emit_cps_ir.c`, "CPS mutual tail-call groups") fuses it into one
`__cps_tcg_N`. Nothing about `guard` itself changed.

The fixture's `ping` / `pong` (which return `'done`) moved from T5's direct
groups to the CPS path by the same change. Its comment says so.

**Still a C call**, as for any mutual tail call: a partner the CPS backend
evicts for some other unsupported form, a member reached through a closure or
an internal `define`, and groups over 8 members or 32 slots
(cps-self-tail-call-relies-on-sibling-call).
