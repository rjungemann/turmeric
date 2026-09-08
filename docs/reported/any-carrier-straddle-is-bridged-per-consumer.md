---
title: An `any` that arrives as the int64 carrier is bridged at each consumer, not once at production
category: Reported
description: A boxed container element whose static type is `any` is emitted as an int64_t local, so every consumer wanting a real tur_tagged_t is a cc error until it is taught to bridge. Five positions have been fixed one at a time, each found by a program that did not compile. Normalising once at production would subsume all of them -- but the call hoist declares its temp __auto_type on purpose, so the emitter does not know the C type where that bridge belongs.
---

# The `any` carrier straddle is bridged per consumer

**Severity: medium.** Every instance is a hard `cc` error with no Turmeric
diagnostic in front of it, never a wrong answer, and the interpreter is
unaffected throughout -- so this is codegen shape, not semantics. What makes it
worth a report is the SHAPE of the bug list: five fixes in two days, each found
by writing an ordinary Saffron program and watching it fail to compile. There is
no reason to think the fifth was the last -- the fourth and fifth arrived
together, from one function in the Saffron prelude.

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

## The five fixed so far

| position | example | fixed |
|---|---|---|
| the `any` readers (`type-of`, `any-is?`, `cast`) | `(type-of (vec-get v 0))` | 2026-09-07 |
| an argument to a parameter declared `any` | `(describe (vec-get v 0))` | 2026-09-08 |
| the dynamic-node operands | `(if (vec-get v 0) ...)`, `(+ (vec-get v 0) x)` | 2026-09-08 |
| a `let` binding declared `any` | `(let [x (vec-get v i)] ...)` | 2026-09-08 |
| an `any` slot of a fat-closure dispatch | `(f (vec-get v i))`, `f : (fn [any] any)` | 2026-09-08 |

All five call the same helper, `emit_any_from_carrier` (`emit_expr.c`), which
is keyed on the value's RECORDED emitted spelling -- so a value that already IS
the aggregate (a widen, a parameter, another `any` local) is untouched.

Fixtures: `vec-any-element-roundtrip`, `saffron-unannotated-main`,
`saffron-dyn-ops-on-vec-elements`, `saffron-prelude`.

The last two were found in one sitting, by writing `stdlib/saffron/prelude.tur`
-- a `vec-map` over a `(Vec any)` with an `any`-taking callback hits both in a
single function body. Five, not three, and the fourth and fifth arrived
together.

## Root cause of the PATTERN

Each fix bridges at the CONSUMER. There are as many consumers as there are
places an `any` can be used, and nothing enumerates them -- unlike the three
dynamic NODES, where `-Werror=switch` and the turi parity ratchet named every
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

### What blocks it, measured 2026-09-08

The obvious home is `emit_value`'s call hoist -- one chokepoint, `EX_CALL`-only,
single exit. It cannot go there as written, and the reason is worth recording
because it is not visible from the call sites:

**The hoist declares its temp `__auto_type`.** That is deliberate ("so the temp
takes the call's EXACT emitted C representation -- carrier int64 vs by-value
aggregate vs pointer -- without re-deriving it"), and it means the emitter does
not know the temp's C type at the one place a production-side bridge would fire.

Which also explains something that looked like a separate mystery: the
consumer-side bridges kept declining on exactly the values they exist for. They
are keyed on the carrier-representation side table, and a hoisted CALL temp was
never in it -- a bridge asked about `__ps_181` and got "unknown". Recording the
hoist temp's C type in the `ret_ct` arm (where the emitter DOES know it) is what
made the last two fixes take effect at all, and it is landed. The `__auto_type`
arm still records nothing, correctly: it exists because the type could not be
derived, and a guess there is worse than silence.

So the production-side fix needs the callee's declared C return type at the
hoist -- a materially larger change than "one bridge, one place", and the
opposite of what the `__auto_type` design is for. That is the real cost, and it
was not visible when this report was first written.

Two things still to establish before attempting it:

1. **Which other producers make a carrier-shaped `any`.** Container accessors
   are the one known family. An instrumented count over the fixture corpus
   would say whether there are others, and the answer decides whether this is a
   narrow normalisation or a broad one.
2. **Whether every carrier CONSUMER is covered by the reverse bridge.** The
   argument path is; a `set!`, a struct-field store, a return position and a
   container store each need checking. A miss there turns a compile error into a
   different compile error, which is survivable, but it should be measured
   rather than discovered.

## Not this bug

`(:: any-value int)` -- narrowing OUT of an `any` -- is a different defect with
a different cause, filed as
[any-narrowing-ascription-does-not-compile](../archive/any-narrowing-ascription-does-not-compile.md).
That one is about `::` accepting an `any` operand at all; this one is about an
`any` value's emitted shape.
