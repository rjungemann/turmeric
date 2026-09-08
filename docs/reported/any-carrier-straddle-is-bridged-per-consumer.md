---
title: An `any` that arrives as the int64 carrier is bridged at each consumer, not once at production
category: Reported
description: A boxed container element whose static type is `any` is emitted as an int64_t local, so every consumer wanting a real tur_tagged_t is a cc error until it is taught to bridge. Three positions have been fixed one at a time -- the any readers, an `any` parameter, and the dynamic-node operands -- each found by a program that did not compile. Normalising once at production would subsume all of them.
---

# The `any` carrier straddle is bridged per consumer

**Severity: medium.** Every instance is a hard `cc` error with no Turmeric
diagnostic in front of it, never a wrong answer, and the interpreter is
unaffected throughout -- so this is codegen shape, not semantics. What makes it
worth a report is the SHAPE of the bug list: three fixes, three days, each found
by writing an ordinary Saffron program and watching it fail to compile. There is
no reason to think the third was the last.

## What the straddle is

`repr_of` answers `REPR_BOXED_AGG` for an `any` at a container-element position,
so a `(Vec any)` element is one slot word pointing at a two-word
`tur_tagged_t`. The generic accessor's C return is that slot word. So
`(vec-get v 0)` has static type `any` -- correct, and deliberately so since
`call_result_type` was taught to exempt `any`/`union` from the bare-tyvar
carrier collapse -- while its emitted spelling is an `int64_t` local.

Every consumer that wants a real `tur_tagged_t` then gets one of:

```
error: aggregate value used where an integer was expected
error: incompatible type for argument 1 of 'describe'
error: incompatible type for argument 1 of '__tur_dyn_truthy'
```

`elab_coerce_to_any` does not help: the operand's static type is ALREADY `any`,
so there is nothing to widen.

## The three fixed so far

| position | example | fixed |
|---|---|---|
| the `any` readers (`type-of`, `any-is?`, `cast`) | `(type-of (vec-get v 0))` | 2026-09-07 |
| an argument to a parameter declared `any` | `(describe (vec-get v 0))` | 2026-09-08 |
| the dynamic-node operands | `(if (vec-get v 0) ...)`, `(+ (vec-get v 0) x)` | 2026-09-08 |

All three call the same helper, `emit_any_from_carrier` (`emit_expr.c`), which
is keyed on the value's RECORDED emitted spelling -- so a value that already IS
the aggregate (a widen, a parameter, a let-bound `any`) is untouched.

Fixtures: `vec-any-element-roundtrip`, `saffron-unannotated-main`,
`saffron-dyn-ops-on-vec-elements`.

## Root cause of the PATTERN

Each fix bridges at the CONSUMER. There are as many consumers as there are
places an `any` can be used, and nothing enumerates them -- unlike the three
dynamic nodes, where `-Werror=switch` and the turi parity ratchet named every
site each one needed. This one has no such forcing function, so the list grows
by someone writing a program.

## Fix direction

**Normalise once at PRODUCTION.** After emitting a call whose static result type
is `any` (or `union`) but whose emitted local is `int64_t`, bridge it there.
Then every consumer sees the aggregate and no consumer needs to know.

The reverse direction already exists and is what makes this plausible rather
than a swap of one problem for another: `emit_expr.c`'s argument path admits
`rarg.kind == TY_ANY` to the CONCRETE -> CARRIER crossing, so a consumer that
genuinely wants the carrier -- a container element store, a tyvar parameter --
is already served.

What has to be established before doing it, and was not established here:

1. **Which other producers make a carrier-shaped `any`.** Container accessors
   are the one known family. An instrumented count over the fixture corpus
   would say whether there are others, and the answer decides whether this is a
   narrow normalisation or a broad one.
2. **Whether every carrier CONSUMER is covered by the reverse bridge.** The
   argument path is; a `set!`, a struct-field store, a return position and a
   container store each need checking. A miss there turns a compile error into a
   different compile error, which is survivable, but it should be measured
   rather than discovered.

Both are probe-sized. They were not done at the third fix because that fix was
one line at a site that already existed, and the session was mid-stage on
`saffron-lang-plan` S6; deferring the general fix is a scheduling choice, not a
judgement that the per-consumer bridges are right.

## Not this bug

`(:: any-value int)` -- narrowing OUT of an `any` -- is a different defect with
a different cause, filed as
[any-narrowing-ascription-does-not-compile](any-narrowing-ascription-does-not-compile.md).
That one is about `::` accepting an `any` operand at all; this one is about an
`any` value's emitted shape.
