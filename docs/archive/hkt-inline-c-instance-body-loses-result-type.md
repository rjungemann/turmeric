---
status: resolved
severity: low
discovered: 2026-07-26
resolved: 2026-07-26
area: compiler (HKT result typing, elab_typeclasses.c)
---

# An HKT instance method with an inline-C body loses its result type

## Resolution (2026-07-26)

**Fully fixed.** All four result classes now keep their type; there is no
remaining shape that loses it.

The plan recorded below (a shared `typed_byval_adt` predicate) was the right one
and it worked. Three changes:

1. **`inline_c_returns_byvalue_adt`** factored out of the two hand-duplicated
   copies in `emit_fns.c` (the definition signature) and `emit_module.c` (the
   forward-decl mirror), declared in `emit_internal.h`. Pure refactor, verified
   behaviour-neutral on its own (2381 passed, 0 failed) before anything was
   built on it. It also removes a lockstep-drift hazard the two copies had.

2. **`elab_typeclasses.c`** commits the grounded result for a by-value aggregate
   too, recognized with `type_app_is_concrete_adt` -- the same predicate the
   by-value HKT spec machinery already uses. `m7_byvalue_grounded` stays false:
   the producer keeps the carrier and no by-value spec is minted.

3. **`emit_expr.c`**'s `fn_body_tail_byvalue_carrier_type` reports the grounded
   aggregate for an inline-C-bodied dispatch call, **gated on the shared
   predicate**, so the pre-existing carrier -> by-value deref bridge fires:

       tur_adt_Box__int m = (*(tur_adt_Box__int *)(intptr_t)(__ps_N));

The producer never had to change. A `(Box int)` argument is already heap-spilled
to reach the dict's int64 slot, so the carrier the inline-C body returns really
is a pointer to the aggregate -- no wrapper, no re-specialized body, no second
ABI. Only the consumer adapts.

**The shared predicate is what made it safe.** The earlier prototype guessed
"does the callee return the carrier?" from the type's shape and reported a
carrier for inline-C functions that genuinely return BY VALUE; the bridge then
deref'd a value that was never a pointer, breaking
`inline-c-struct-return-cstr-params` and `io-stdlib-roundtrip` with `aggregate
value used where an integer was expected`. Asking `emit_fns.c`'s own question
instead fixed both, exactly as predicted.

### TUR-W0042 removed

The diagnostic added a few hours earlier warned that this shape "will lose the
result type". It now works, so the warning was a false alarm and has been
deleted -- code, explanation, guide entry, and its error fixture, which is
replaced by `tests/fixtures/hkt-inline-c-byvalue-result-type` asserting the shape
compiles and computes correctly (a real body applying `g`, a field read, and a
typed parameter). The "Known Limitations" entry in `docs/guides/hkt-guide.md` is
gone for the same reason.

Suite: 2381 passed, 0 failed. One snapshot regenerated
(`van-laarhoven-lens-wide-compose`) -- verified a pure REORDERING of two
`#ifndef`-guarded typedef blocks, byte-identical content on both sides.

The original report and the narrowing update follow for the record.

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

**Done 2026-07-26: the diagnostic now exists** -- `TUR-W0042`, emitted at the
`definstance` rather than at the call site, with the three fixes named
(Turmeric body / `:heap` head / `defopaque` head) and `rc<T>`-family heads
explicitly excluded. `tur explain TUR-W0042` carries the long form; the shape is
also written up under "Known Limitations" in
`docs/guides/hkt-guide.md`. Pinned by
`tests/fixtures/errors/hkt-inline-c-byvalue-result-warn`.

It is a **warning, not a rejection**, which is a deliberate departure from what
this report originally proposed. The instance is well-formed and works fine until
something consumes its result at a typed position, so erroring would break code
that compiles today. The call-site error still fires for anyone who does consume
it; the warning just puts the diagnosis at the cause.

### Lifting it for real -- measured, and a named next step

"Producer-side work" was too vague to act on, so it was prototyped. **The route
works**, and the machinery already exists; what is missing is one predicate.

Two changes, both behind a temporary env gate:

1. `elab_typeclasses.c`: commit the grounded `result_type` for a by-value
   aggregate too.
2. `emit_expr.c`: have `fn_body_tail_byvalue_carrier_type` report the aggregate
   for a dispatch call whose callee `body_is_inline_c`, so the **existing**
   carrier -> by-value deref bridge (`init_carrier_to_byval`, the CONV-S1 seam-4
   path) fires at the binding.

Result on the `Box` repro: correct output, via exactly the intended bridge --

    tur_adt_Box__int m_1314 = (*(tur_adt_Box__int *)(intptr_t)(__ps_158));

That works because a `(Box int)` argument is heap-spilled to reach the dict's
int64 slot, so the carrier the inline-C body returns really is a pointer to the
aggregate. Nothing new has to be generated: no wrapper, no re-specialized
inline-C body, no second ABI.

**Where the prototype is still wrong.** Suite: 2377 passed, 4 failed. One is this
report's own warning fixture (the shape now works, so its downstream error
disappears) and one is the benign `van-laarhoven-lens-wide-compose` typedef
reorder. The other two are real:

- `inline-c-struct-return-cstr-params` -- `error: aggregate value used where an
  integer was expected`
- `io-stdlib-roundtrip` -- same error

Both fail the OPPOSITE way. The crude gate (`body_is_inline_c` + result is a
non-`:heap` aggregate) also catches inline-C functions whose result genuinely
returns BY VALUE, and the bridge then derefs something that was never a pointer.
That is the hazard the existing comment in `fn_body_tail_byvalue_carrier_type`
already names -- "a NON-parametric concrete record result (`Pos`) instead returns
BY VALUE (typed_byval_adt fires), so it must NOT be treated as a carrier producer
here -- otherwise the consume-side bridge double-derefs an aggregate (the
typeclass-fundep-collect regression)".

**So the one missing piece is a shared predicate.** The gate must ask the same
question `emit_fns.c` asks when it decides an inline-C body's C return type --
"does `typed_byval_adt` fire for this result?" -- instead of inferring it from
the type's shape. Today that decision lives inline in the signature emitter and
has no callable form; factoring it out and calling it from both places is the
work. With that, the two regressions above should go, because both are cases
where `typed_byval_adt` DOES fire and the gate would correctly decline.

That is a contained, testable change -- not the open-ended audit the original
fix direction 1 implied -- but it does touch the predicate that decides every
inline-C return type, so it wants its own pass and its own fixtures rather than
riding along here.

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

## Fix directions -- SUPERSEDED, see "Lifting it for real" above

Kept for the record; all three were tried or ruled out, and the live plan is the
shared-`typed_byval_adt`-predicate step described in the Update.

1. **Separate the two decisions.** Commit the grounded `result_type` regardless
   of the body, and keep `m7_byvalue_grounded` (which drives the by-value spec
   and the `abi_bindings` attachment) gated on the body as it is now. The risk is
   entirely in whether any consumer infers *representation* from the call's
   result type -- the existing comments say some do, so this needs auditing
   rather than a one-line change.
   -- Measured: insufficient on its own (`error: invalid initializer`); it needs
   the consume-side deref bridge alongside it.
2. **Or mint a by-value spec for inline-C bodies too**, where the body's declared
   C signature can be re-emitted at the specialized type. Probably only viable
   when the inline-C body is representation-agnostic.
   -- Ruled out: an inline-C body is written against exactly one C signature.
   Also unnecessary, since the existing carrier -> by-value bridge covers it.
3. **Or extend the carrier-safe arm** case by case, as the pointer-family fix
   did, for each result shape whose by-value representation provably equals the
   carrier. Cheapest and safest per case; does not converge.
   -- This is the route actually taken so far: pointer-family
   (`docs/archive/hkt-fmap-result-is-not-droppable.md`) and `:heap`
   (`tests/fixtures/hkt-inline-c-heap-result-type`). It has now run out of
   carrier-width classes, which is why only the by-value aggregate remains.

A fixture pair (the two `Box` bodies above, asserting the same grounded result
type) would pin whichever route is taken.
