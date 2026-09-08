---
title: An `any` that arrives as the int64 carrier is bridged at each consumer, not once at production
category: Reported
description: RESOLVED by normalising at production. A boxed container element whose static type is `any` was emitted as an int64_t local, so every consumer wanting a real tur_tagged_t was a cc error until taught to bridge -- five were, one at a time. One bridge at the call hoist subsumes all of them; the claimed blocker (the hoist's __auto_type temp) was inferred from a comment and is not what any-typed calls actually take.
---

# The `any` carrier straddle is bridged per consumer

**RESOLVED 2026-09-08** by the fix direction below: one bridge at PRODUCTION,
and all six consumer-side bridges deleted as dead. Net -110 lines in
`emit_expr.c`.

**Severity was medium.** Every instance was a hard `cc` error with no Turmeric
diagnostic in front of it, never a wrong answer, and the interpreter was
unaffected throughout -- so this was codegen shape, not semantics. What made it
worth a report is the SHAPE of the bug list: five fixes in two days, each found
by writing an ordinary Saffron program and watching it fail to compile. There
was no reason to think the fifth was the last -- the fourth and fifth arrived
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

## The five that were fixed one at a time

| position | example | fixed |
|---|---|---|
| the `any` readers (`type-of`, `any-is?`, `cast`) | `(type-of (vec-get v 0))` | 2026-09-07 |
| an argument to a parameter declared `any` | `(describe (vec-get v 0))` | 2026-09-08 |
| the dynamic-node operands | `(if (vec-get v 0) ...)`, `(+ (vec-get v 0) x)` | 2026-09-08 |
| a `let` binding declared `any` | `(let [x (vec-get v i)] ...)` | 2026-09-08 |
| an `any` slot of a fat-closure dispatch | `(f (vec-get v i))`, `f : (fn [any] any)` | 2026-09-08 |

All five called the same helper, `emit_any_from_carrier` (`emit_expr.c`), keyed
on the value's RECORDED emitted spelling -- so a value that already IS the
aggregate (a widen, a parameter, another `any` local) was untouched. **All five
are now gone**, along with the `any` readers that came before them; see below.

Fixtures: `vec-any-element-roundtrip`, `saffron-unannotated-main`,
`saffron-dyn-ops-on-vec-elements`, `saffron-prelude`.

The last two were found in one sitting, by writing `stdlib/saffron/prelude.tur`
-- a `vec-map` over a `(Vec any)` with an `any`-taking callback hits both in a
single function body. Five, not three, and the fourth and fifth arrived
together.

## Root cause of the PATTERN

Each fix bridged at the CONSUMER. There are as many consumers as there are
places an `any` can be used, and nothing enumerates them -- unlike the three
dynamic NODES, where `-Werror=switch` and the turi parity ratchet named every
site each one needed. This one had no such forcing function, so the list grew by
someone writing a program. That is what the report was filed to say, and it is
why the answer was a chokepoint rather than a sixth bridge.

## Fix direction -- taken

**Normalise once at PRODUCTION.** After emitting a call whose static result type
is `any` (or `union`) but whose emitted local is `int64_t`, bridge it there.
Then every consumer sees the aggregate and no consumer needs to know.

The reverse direction already exists and is what makes this plausible rather
than a swap of one problem for another: `emit_expr.c`'s argument path admits
`rarg.kind == TY_ANY` to the CONCRETE -> CARRIER crossing, so a consumer that
genuinely wants the carrier -- a container element store, a tyvar parameter --
is already served.

### What was claimed to block it, and did not

**The claim was wrong, and it was wrong in this report's own characteristic
way: inferred from a comment rather than measured.**

The obvious home is `emit_value`'s call hoist -- one chokepoint, `EX_CALL`-only,
single exit. This report said it could not go there, because the hoist declares
its temp `__auto_type`, which the emitter uses precisely when it cannot derive
the type. That reasoning is sound about `__auto_type`; it just never checked
whether an `any`-typed call takes that arm.

Swept over the fixture corpus, printing the arm for every hoist whose static
result type is `any` or `union`:

```
243 HOIST any arm=ret_ct ct=tur_tagged_t
 90 HOIST any arm=ret_ct ct=int64_t
  0 arm=auto
```

**333 of 333 take the `ret_ct` arm.** Not one takes `__auto_type`. So the type
IS known at the chokepoint, 243 sites are already the aggregate and need
nothing, and 90 are the straddle.

### The fix

One bridge, at the very end of the hoist, when the static result type is
`any`/`union`:

```c
Type rt = emit_resolve_type(ctx, e->type);
if (rt.kind == TY_ANY || rt.kind == TY_UNION) {
    char *bridged = emit_any_from_carrier(ctx, body, strdup(tmp), e);
    if (bridged) return bridged;
}
```

Placed at the END, not beside the declaration, so everything between still sees
the CARRIER temp. The region note in particular can only note a WORD; handing it
the aggregate would silently drop the runtime half of the region lock, which is
the sort of loss that shows up as a use-after-poison much later.

It also needed one enabling change, landed with the prelude: the hoist now
records its temp's C type in the carrier-representation side table (the
`ret_ct` arm only). Every bridge is keyed on that table, and a hoisted CALL
temp was never in it -- which is why the consumer bridges kept declining on
exactly the values they exist for.

### The two things to establish first, established

1. **Which other producers make a carrier-shaped `any`.** The sweep answers it:
   the 90 are all container accessors, and no `any`-typed hoist escapes through
   `__auto_type`.
2. **Whether every carrier CONSUMER is covered by the reverse bridge.** Yes --
   the full suite is green with production normalisation on. That is the real
   test, because 90 sites change shape.

### All six consumer bridges were dead, and are gone

Verified rather than assumed. A probe tagging each call site and counting which
ones actually pass the guards, swept over the corpus:

```
90 AFC-FIRES site=0        <- the production hoist
 0 AFC-FIRES site=1..4     <- the `any` readers, the fat-closure slots, ...
```

So the readers (`type-of`, `any-is?`, `cast`), the `any`-parameter argument
position, the dynamic-node operands, the `let` binding, and both fat-closure
dispatch sites are all no-ops once production normalises. Removed: the suite
stays at 2879/0. `emit_dyn_operand`, which existed only to carry the bridge to
the three dynamic nodes, is gone with them.

The risk profile made this a cheap cleanup to attempt: every one of these was a
`cc` error, never a silent wrong answer, so being wrong fails loudly at compile
time.

## Not this bug

`(:: any-value int)` -- narrowing OUT of an `any` -- is a different defect with
a different cause, filed as
[any-narrowing-ascription-does-not-compile](../archive/any-narrowing-ascription-does-not-compile.md).
That one is about `::` accepting an `any` operand at all; this one is about an
`any` value's emitted shape.
