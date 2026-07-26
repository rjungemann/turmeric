---
status: open
severity: low
discovered: 2026-07-26
updated: 2026-07-26
area: compiler (HKT result typing, elab_typeclasses.c)
---

# An HKT instance method with an inline-C body loses its result type

## Update 2026-07-26 -- scope narrowed, root cause corrected

**The `:heap` ADT case is fixed** (`tests/fixtures/hkt-inline-c-heap-result-type`).
A `:heap` parametric ADT emits as `tur_adt_HBox__int *` -- a pointer, so it fits
the int64 carrier exactly and its precise type can be committed with no by-value
spec, the same argument the int-carrier-newtype and pointer-family arms already
made. It was simply a third carrier-width class the test did not know about.

**The diagnosis below is wrong about the mechanism, and I was wrong to call the
gate a conflation.** It is not conflating "what type does this call have" with
"which ABI does this dispatch use". It is asking a narrower and *correct*
question -- "is this result already carrier-width?" -- and the answer drives both,
legitimately. The bug was that the question was answered by an incomplete list of
type classes, not that the wrong question was asked.

Measured, not assumed. Fix direction 1 below ("commit the grounded result_type
regardless of the body") was implemented behind a temporary env gate and run
against the suite:

- 2378 passed, 1 failed -- and that one failure was a pure REORDERING of two
  `#ifndef`-guarded typedef blocks, semantically identical output. So the change
  is nearly free in-tree, which is what tempted me toward it.
- But on a by-value aggregate result it emits
  `tur_adt_Box__int m_1314 = __ps_158;` against a carrier-returning inline-C
  base: **`error: invalid initializer`**. Nothing in the suite covers that shape,
  which is exactly why the suite looked clean.

So fix direction 1 as written trades a confusing call-site type error for a cc
error in generated C -- worse for a user, since the error no longer points at
their source. It is not shippable.

### What remains open

Only the **by-value aggregate** result: a non-`:heap` parametric struct whose
instance body is inline-C.

    (defstruct Box [A] (val A))
    (definstance Functor [Box] (fmap [container g] ```c return container; ```))

still yields `(type-app ? ?)`. This one cannot be fixed by committing the type,
because the producer genuinely returns the carrier while the consumer would read
an aggregate. An inline-C body is written against exactly one C signature and
cannot be re-specialized at the by-value type, so fix direction 2 does not apply
either.

Severity dropped to **low**: the remaining shape is narrow (parametric,
non-`:heap`, non-opaque, inline-C-bodied), nothing in stdlib or the suite has
one, and it fails loudly at the call site rather than miscompiling.

The most valuable next step is probably fix direction 2's fallback -- **reject the
shape with a real diagnostic at `definstance`** ("an inline-C instance body cannot
return a by-value aggregate `(f b)`; make the ADT `:heap`, `defopaque`, or write
the body in Turmeric"). That is strictly better than the current
`got (type-app ? ?)` at a call site far from the cause.

The original report follows; its "Root cause" section is superseded by the above.

## Summary

A typeclass method whose declared result is an applied `(f b)` gets that result
grounded at the dispatch site -- unless the selected instance's body is
**inline-C**, in which case the call node keeps the def-less
`type_from_kind(TY_APP)` shell, printed `(type-app ? ?)`. Every typed consumer of
the result then fails.

The container is irrelevant; only the body's implementation language matters.

## Repro

Hold the container fixed and vary only the body. Pure-Turmeric body:

    (defstruct Box [A] (val A))

    (definstance Functor [Box]
      (fmap [container g] (make-struct Box :val (g (.val container)))))

    (defn dbl [x : int] : int (* x 2))
    (defn want-cstr [s : cstr] : int 0)

    (defn main [] : int
      (let [b (:: (make-struct Box :val 21) (Box int))]
        (let [m (fmap b dbl)]
          (println (want-cstr m))))
      0)

    error: expected cstr, got (type-app Box int)      <-- grounded, correct

Now the same class, same container, same call -- only the body changes:

    (definstance Functor [Box]
      (fmap [container g]
        ```c
        return container;
        ```))

    error: expected cstr, got (type-app ? ?)          <-- lost

## Root cause -- SUPERSEDED, see Update

`elab_typeclasses.c` computes the grounded result correctly in both cases -- the
substitution yields `(type-app Box int)` either way. It is the commit gate that
discards it:

    if (m7_body_byvalue_ok) { result_type = substituted; m7_byvalue_grounded = true; }
    else if (result_type.kind == TY_APP && m7_result_is_int_carrier(substituted))
             { result_type = substituted; }

`m7_body_byvalue_ok` requires `best_method->body->kind != EX_INLINE_C`. The
second arm only catches opaque / transparent-int newtypes. An inline-C body over
any other container matches neither, so `result_type` stays the shell.

The gate conflates two separable questions:

1. **What type does this call have?** A property of the class signature, the
   instance head, and the argument types. Independent of how the body is written.
2. **Which ABI does this dispatch use** -- a by-value spec, or the uniform int64
   carrier? Legitimately depends on the body.

Only (2) needs the inline-C test. (1) is being gated on it as a side effect.

## Severity

Medium. Any spice or stdlib module whose HKT instance needs inline-C -- which is
every instance over a container the language cannot destructure in pure Turmeric
-- exports an `fmap`/`bind`/`alt` whose result is untyped at every call site.
Callers must ascribe every use, and operations that test for a concrete type
(`rc/drop`, field access, a typed parameter) simply fail.

Not new: it has been latent as long as the M7 by-value path has existed. It only
became visible when `stdlib/rc.tur` started compiling
(`docs/archive/rc-tur-legacy-instances-do-not-compile.md`) and something finally
called an inline-C-bodied HKT instance.

## Already handled: the pointer-family case

`(type-app rc<?> int)` -> `rc<int>` is fixed
(`docs/archive/hkt-fmap-result-is-not-droppable.md`): a third arm collapses an
applied result over a pointer-family head to the concrete handle. It is sound
without minting a by-value spec because an rc/weak/ref IS the int64 carrier --
same width, same bits, no aggregate layout to misread.

That argument does **not** generalize, which is why this report exists
separately. A by-value aggregate result committed while the producer returns a
carrier is the carrier-vs-by-value mismatch the surrounding comments attribute to
several silent miscompiles (the Alternative `<|>` selection body reading as 0).

## Fix directions

1. **Separate the two decisions.** Commit the grounded `result_type` regardless
   of the body, and keep `m7_byvalue_grounded` (which drives the by-value spec
   and the `abi_bindings` attachment) gated on the body as it is now. The risk is
   entirely in whether any consumer infers *representation* from the call's
   result type -- the existing comments say some do, so this needs auditing
   rather than a one-line change.
2. **Or mint a by-value spec for inline-C bodies too**, where the body's declared
   C signature can be re-emitted at the specialized type. Probably only viable
   when the inline-C body is representation-agnostic.
3. **Or extend the carrier-safe arm** case by case, as the pointer-family fix
   did, for each result shape whose by-value representation provably equals the
   carrier. Cheapest and safest per case; does not converge.

A fixture pair (the two `Box` bodies above, asserting the same grounded result
type) would pin whichever route is taken.
