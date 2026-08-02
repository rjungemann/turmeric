---
title: ECS Component-Set Bounds (`/has`)
category: Planning
description: The third polymorphism encoding for ECS systems -- a structural "this world has Pos and Vel" bound. Re-filed out of the refinement-types plan, because it constrains a type rather than a value.
---

# ECS Component-Set Bounds (`ECB`)

**Status:** the cheap step (3) is **done 2026-08-01** -- shipped spice-side as
`defworld-classes`, though not in the shape this plan recommended; see the note
under "Why this is not urgent". The structural `(has ...)` bound itself is not
started and remains deliberately unscheduled. Split out of
[`ecs-refinement-typed-apis-plan.md`](ecs-refinement-typed-apis-plan.md),
where it was tracked as a refinement type and should not have been.

## Why it moved

The previous revision of the ECS refinement plan proposed:

```turmeric
defn integrate [W] [w : (refine W (/has Pos /has Vel)) dt : float] : void
```

as "a predicate on the world type parameter". It is not a refinement type, and
building it as one would be a mistake independent of how much elaborator work
it took.

A refinement type constrains a **value**: `#refine{ x : int | (> x 0) }` is
about the integer, and the machinery that decides it -- the normalized VC, the
staged solver, congruence closure -- reasons about integers, reals, booleans,
and uninterpreted functions. `/has Pos` constrains a **type**: it asks whether
`W` has a field named `Pos`. The VC term language has no types in it, and
[the guide](../../guides/refinement-types-guide.md) lists "no refinements on
type parameters" as a `[prototype]` limit that is not planned.

Encoding a structural type question through the SMT layer would mean teaching
the solver about the type system in order to re-answer a question `defworld`
can settle by field lookup at elaboration time. The right mechanism is a
row/presence constraint, decided structurally, with no solver involved.

## What already ships, and why that matters here

Two of the three encodings the ECS wants exist today:

| encoding | status | cost |
|---|---|---|
| (a) monomorphic against a concrete world | ships | none |
| (b) typeclass-bounded, `(HasPos W) (HasVel W)` | ships via `defcomponent-class` / `defcomponent-class-instance` | one dictionary indirection per polymorphic call |
| (c) structural `/has` bound | this plan | none, if built |

So (c) buys **one indirection per polymorphic call** over (b), plus a
declaration-site ergonomics win: `defcomponent-class` /
`defcomponent-class-instance` must be called once per component and once per
`(world, component)` pair, which is real boilerplate in a game with a dozen
components and three worlds.

It does not buy any safety that (b) does not already have. A system that
touches a component the world lacks is a compile error under (b) today.

## Why this is not urgent

Stated plainly so it does not get picked up by momentum:

1. **The safety property is already had.** (b) is not a workaround; it is a
   correct, shipping encoding of the same constraint.
2. **The performance claim is unmeasured.** "One dictionary lookup per
   polymorphic call" is the parent plan's own estimate, and the refinement
   work's benchmarking section is a standing warning about this exact class of
   claim -- whole-program entry-check elision was top of the list for months on
   the strength of a "perf win" that measured at **zero** on both runtime and
   code size, because `cc -O2` was already doing it. A monomorphized dispatch
   may well be in the same category. **This plan does not start without a
   profile showing dictionary dispatch in an ECS hot path.**
3. **The ergonomics win is real but has a cheaper fix.** Most of the
   boilerplate is `defcomponent-class-instance` per `(world, component)` pair,
   and `defworld` already knows its component list at expansion time. Emitting
   the instances from `defworld` is a macro change in the spice, needs no
   compiler work, and captures most of the declaration-site benefit.

Doing (3) first is the recommendation. It is small, it is entirely spice-side,
and it makes the remaining case for (c) purely about the indirection -- which
is the part that needs the profile.

> **Step (3) DONE 2026-08-01 -- shipped as `defworld-classes`, not as a change
> to `defworld`.** `turmeric-spices` `a4cb1b9` adds a macro that emits the
> world struct plus `defcomponent-class` + `defcomponent-class-instance` for
> every component, collapsing `1 + 2N` declarations to one. Test:
> `spices/ecs/tests/defworld-classes.tur`; suite 70/70 -> 71/71.
>
> **Emitting from `defworld` itself -- as this section recommends -- is a
> breaking change, and that was not known when the recommendation was
> written.** `defcomponent-class-instance` expands to cap-mint helpers that
> name `ecs/cap` symbols *at the call site*, so folding it into `defworld`
> forces every caller to import
> `WriteCap`/`ReadCap`/`make-write-cap`/`make-read-cap` or fail with
> `unknown function or operator 'make-write-cap'`.
>
> The premise is also weaker than stated. "Most of the boilerplate is
> `defcomponent-class-instance` per `(world, component)` pair" assumes the two
> surfaces overlap; measured in the spice, they are **disjoint** -- 16 files
> call `defworld`, exactly 1 touches the typeclass surface, and 0 do both. The
> single typeclass consumer (`tests/has-component-polymorphic.tur`) declares
> its worlds with raw `defstruct` rather than `defworld`, because it wants a
> world with no `gens` field. So auto-emission would have broken 15 in-repo
> call sites and every downstream `defworld` user to serve none of them.
>
> Two facts the design relies on were probed rather than assumed: redeclaring a
> component's class is tolerated (two worlds may share a component, each
> re-emitting `defcomponent-class C`), and a duplicate `(world, comp)` instance
> is tolerated (the macro composes with hand-written declarations instead of
> colliding). Both hold on v0.33.0.
>
> This does **not** change the status of (c) below, which still does not start
> without a profile.

## Sketch, if it is built

Not decisions; a starting point.

`defworld` lowers to a `defstruct` whose field *names* are the component type
names -- the central E0 typing trick, and the reason a structural bound is
plausible at all. `(.Pos w)` is the storage for `Pos`, so "does `W` have `Pos`"
is "does `W`'s struct have a field named `Pos`", which the elaborator can
answer by lookup.

The pieces that would be needed:

- **A constraint form on a type parameter** that is not a typeclass
  constraint -- something in the shape of `[W : (has Pos Vel)]`, checked
  structurally at instantiation.
- **Solving by field lookup**, not by instance search. A world declared with
  `Pos` satisfies `(has Pos)` because the field is there; there is no witness
  to construct and no dictionary to pass, which is the entire point.
- **Monomorphization**, since with no dictionary the polymorphic body must be
  specialized per world it is instantiated at. Turmeric already monomorphizes
  (see [monomorphization-abi-guide.md](../../guides/monomorphization-abi-guide.md));
  the question is whether a structurally-bounded parameter routes through the
  same path, and that is the first thing to establish.
- **A diagnostic** naming the missing component and the world, pointing at the
  `defworld` that declares the set. The failure mode for (b) today is an
  instance-not-found error, which is noticeably worse.

Row types are the obvious neighbouring feature -- `ecs/query.tur` already
exposes a row-typed `Query #row{...} #row{...}` value -- and whether `has` is a
special case of a general row-presence constraint or its own narrow form is the
main design question. Narrow is probably right: the general feature is much
larger and the ECS is the only known consumer.

## Acceptance, if it is built

- A `defn` whose world parameter carries `(has Pos)` compiles against a world
  declared with `Pos`, **without** a `HasPos` instance in scope.
- Negative fixture: the same `defn` against a world lacking `Pos` fails to
  elaborate, with a message naming the component and the world.
- A codegen assertion that the specialized body contains no dictionary load --
  the whole justification for (c) over (b) is that indirection, so its absence
  is the acceptance criterion, not a nice-to-have.
- Instantiation at two different worlds from one definition, to pin that
  monomorphization does what is assumed.

## References

- [ecs-refinement-typed-apis-plan.md](ecs-refinement-typed-apis-plan.md) --
  where this was mis-filed
- [refinement-types-guide.md](../../guides/refinement-types-guide.md) -- the
  `[prototype]` limit on refinements over type parameters
- `docs/guides/ecs-guide.md` -- the polymorphism section, encodings (a) and (b)
- `docs/guides/ecs-vs-haskell-ecs.md` -- the comparison row this would add to
- `docs/guides/monomorphization-abi-guide.md`
