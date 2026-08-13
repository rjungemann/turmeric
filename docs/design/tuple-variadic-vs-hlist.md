# Variadic Tuples vs HList: A Design Contrast

## Status

**Speculative.** This is a design-comparison note, not a plan. It
contrasts the approach sketched in
`docs/archive/tuple-variadic-type-plan.md` (Option B: a primitive
variadic `TY_TUPLE` kind) with a hypothetical HList-style approach
(kind-level list of types, in the Haskell `HList` tradition).

Neither plan is funded. Option B is itself gated on Option A
(`Tuple2..Tuple5` as `defstruct`-generated nominal types) proving
insufficient, and the HList direction is roughly as likely to ship as
dependent types -- i.e. probably not, certainly not before 1.0. The
purpose of this document is to record the trade-offs while they are
fresh, so that the next time the question comes up nobody has to
re-derive them.

## Recap: Option B in one paragraph

Add a new `TypeKind`, `TY_TUPLE`, whose payload is an array of
`Type *` plus an arity. Tuples have fixed arity per type;
`Tuple[int, cstr]` and `Tuple[int, cstr, float]` are distinct
types. Unification is element-wise with an arity check. The kind
system is unchanged: `Tuple` is "the variadic constructor that, given
N type arguments, yields a type." Partial application
(`Tuple[int, _]`) is most likely rejected as a kind error.

See `docs/archive/tuple-variadic-type-plan.md` for the full sketch.

## HList in one paragraph

Introduce a kind-level list of types. A heterogeneous list type is
parameterised by exactly one thing: a type-level list `[T1, T2, ...]`.
There is one type constructor (call it `HList`) of kind
`[Type] -> Type`. Operations like `head`, `tail`, `append`, `length`,
and `zip` become expressible at the type level via type families /
type functions. Records-as-HLists, row polymorphism, and extensible
records all become reachable from the same foundation. This is the
shape Haskell's `HList`, Idris's heterogeneous vectors, and various
dependently-typed tuple encodings take.

## The core difference

Option B is variadic **in the constructor**. HList is variadic
**in the kind system**. Everything else follows from that.

Option B says: "the type representation grows a new node that holds a
C array of `Type *`; every `Type`-walker grows a loop." The kind
system still believes every constructor has a fixed arity -- the new
arity is just stored on the node itself rather than being a property
of a `TY_*` tag.

HList says: "kinds themselves become structured. There is a new sort
of kind, `[Type]` (list-of-types), and a constructor `HList` of kind
`[Type] -> Type`. To talk about an HList's structure, you compute on
this kind-level list."

That distinction is small in words and enormous in implementation
cost.

## Side-by-side

| Axis | Option B (`TY_TUPLE`) | HList |
|------|----------------------|-------|
| New `TypeKind`s | 1 (`TY_TUPLE`) | 1 (`TY_HLIST`), plus a new *kind* (`[Type]`) and kind-level constructors |
| Kind system changes | None | Substantial: kinds gain structure, possibly type families |
| Unification | Element-wise + arity check | Unify type-level lists, possibly via type-level computation |
| Arity-as-type-identity | Yes (different arity = different type) | No (arity is a property of the type-level list) |
| Partial application | Open question; likely a kind error | Natural -- `HList [Int, _]` is just an HList over a partially-known kind-list |
| Type-level operations on tuples | None expressible | `head`, `tail`, `append`, `map`, `zip`, `length` all expressible |
| Records / row polymorphism reachable? | No | Yes, with more work |
| Decidability of inference | Stays decidable | Type families can introduce undecidability; needs guardrails |
| Runtime representation | Packed struct, layout fixed at compile time | Same packed layout possible, but most published HList implementations are heap-cons-heavy |
| Surface syntax | `(tuple a b c)` | Needs sugar; raw form is `(hcons a (hcons b (hnil)))` or similar |
| Migration from Option A | Clean; `TupleN` become aliases | Disruptive; the whole model changes |
| Existing precedent in the compiler | `TY_VEC` (single-element variadic child), `TY_FORALL` (variable-length binder list) | None -- kind-level structure is new ground |

## What HList buys you that Option B does not

1. **Type-level computation over tuple shapes.** You can write a
   typeclass instance that says "for any HList, if every element is a
   `Show`, the HList is also `Show`" -- and the instance resolver can
   walk the kind-level list inductively. Under Option B, that requires
   either macro expansion at every arity or a special-case in the
   resolver.

2. **A foundation for extensible records.** Records-as-named-HLists is
   a well-trodden path. Once you have a kind-level list of types, you
   add labels and you have row polymorphism. Option B has nothing to
   say about records; that stays a `defstruct` concern indefinitely.

3. **Uniform abstraction over "any tuple".** This is the third gating
   signal listed in the Option B plan (lines 19-22) -- the case where
   HKT / typeclass code wants to abstract over "any tuple" uniformly.
   Option B can answer this only by special-casing `TY_TUPLE` in the
   resolver. HList answers it natively: an HList *is* the abstraction.

4. **No artificial cap, ever.** Option B's fallback (line 184) is an
   internal arity cap. HList doesn't have one even as a fallback --
   the kind-level list can be arbitrarily long because nothing in the
   representation cares.

5. **Partial application is free.** `HList [Int, _]` falls out of
   normal kind-level unification with a hole in a kind-level list. No
   open question.

## What HList costs that Option B does not

1. **The kind system grows.** Today kinds are essentially `*` and
   arrow kinds. HList needs kind-level lists, which means the kind
   checker, kind printer, kind unifier, and (almost certainly) type
   families all grow. The Option B plan describes touching every
   `Type`-walking site; HList means touching every `Kind`-walking site
   *and* every `Type`-walking site.

2. **Decidability becomes a live concern.** Once you can compute on
   types, you can write non-terminating type-level programs. Haskell
   ships `UndecidableInstances` as a separate extension for exactly
   this reason. Turmeric would need to decide up front whether it
   accepts that or pays for a totality check on type-level functions.

3. **Error messages get worse before they get better.** Type errors
   involving kind-level list unification mismatches are notoriously
   hard to render well. GHC has spent twenty years on this and the
   results are still often bad.

4. **HKT plumbing has to be redone, not extended.** The S1-S8 HKT
   work (see `MEMORY.md`) was sized against a kind system where
   constructors have ordinary arrow kinds. HList changes the shape of
   what a "type constructor" is -- the HKT machinery doesn't just
   need new arms, it needs to be re-derived against a richer kind
   structure.

5. **Surface syntax becomes a real problem.** `(tuple 1 2 3)` is
   easy. The HList equivalent without sugar is
   `(hcons 1 (hcons 2 (hcons 3 (hnil))))`. With sugar it can match
   Option B's surface, but the sugar layer is itself non-trivial and
   has to round-trip through error messages.

6. **No existing precedent in the compiler.** The Option B plan notes
   (lines 80-84) that `TY_SESSION_PAIR` is the closest existing
   precedent for `TY_TUPLE`. There is no analogous foothold for HList.
   Everything is new.

## Why HList is probably wrong-shaped for Turmeric specifically

Turmeric is a systems-flavoured language with a Lisp surface, a spice
ecosystem, and a strong "build something that runs" bias. The HKT
work already pushed the type system further than most languages in
this category go. The cost-to-benefit on HList is hard to justify
because:

- The use cases the Option B plan lists (lines 14-22) are all about
  *arity*, not about *computing over types*. Hitting the Tuple5 cap is
  an HList non-problem -- but Option B is also a non-problem for it,
  just more expensively.

- The user-facing benefit of HList over Option B is mostly accessible
  only to people writing typeclass instances and library glue. The
  surface program `(tuple 1 "x" 3.14)` is the same either way.

- Records-as-HLists is a real win, but Turmeric has `defstruct`, and
  the `defstruct` ergonomics are good enough that the demand for
  anonymous structural records has not materialised.

- The decidability and error-message costs hit *everyone* writing
  typed code, not just the people exercising the new feature.

The honest reading is that HList is the right answer for a language
whose value proposition includes type-level programming as a
first-class activity. Turmeric's value proposition is closer to "Lisp
that compiles to native code and ships." The type system is in
service of that, not the other way around.

## When you would reconsider

- If a future plan (extensible records, structural row types, generic
  programming over heterogeneous shapes) wants HList-shaped power and
  cannot be served by `defstruct` plus typeclass instances. At that
  point the kind-system work is being paid for by that plan, and
  variadic tuples become a freebie that falls out of it.

- If Turmeric grows a dependent-types story for any other reason. The
  machinery overlap is substantial -- a language that has dependent
  types already has most of what HList needs, so the marginal cost
  collapses.

- If the Option B implementation, when actually attempted, turns out
  to require so many special cases in the typeclass resolver and HKT
  machinery that it would have been cheaper to bite the HList bullet.
  This is the "regret" scenario; it should be checked for during
  Option B's design phase, not discovered mid-implementation.

If none of those fire, the right outcome is: Option A indefinitely,
Option B if pressure builds, HList never. That is not a slight against
HList -- it is a recognition that the cost is paid by the language
and the benefit accrues mostly to a kind of program Turmeric is not
optimising for.

## See also

- `docs/tuple-type-plan.md` -- Option A, the shipped/shippable plan.
- `docs/archive/tuple-variadic-type-plan.md` -- Option B, deferred.
- `MEMORY.md` entries on HKT (S1-S8) and sized types -- the closest
  prior art for "we extended the type system and it cost what we
  thought it would cost."
