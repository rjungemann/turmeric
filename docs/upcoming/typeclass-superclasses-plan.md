# Typeclass superclasses: `defclass` constraint preambles

> **Status:** SC0-SC6 **landed 2026-09-16** behind the gate (see section 7);
> **SC7 (graduation) landed 2026-09-25** (see section 8). **SC8a (the lattice
> classes) and its SC9 docs landed 2026-09-25**, unreleased (see section 10).
> **SC8b (the auto-loaded classes and the arrows) landed 2026-09-25**,
> unreleased, as five commits (see section 11). Stdlib adoption is complete;
> nothing in this plan remains open.
> **Type:** compiler feature (elaboration, plus superclass dictionaries for
> dictionary-passing generics -- section 11), plus a
> **documentation correction that is independently shippable and should land
> first**.
> **Gate:** none -- graduated. The constraint preamble is unconditional as of
> 0.54.0; `--enable=class-superclasses` is a TUR-W0063 no-op.

## 0. Summary

**The investigated claim is correct.** `docs/guides/lattice-guide.md:78` --
"It is declared **flat**, not as a subclass of `Semigroup`, because `defclass`
has no superclasses" -- accurately describes the language as of v0.48.0.
`defclass` has no superclass slot, nothing in `src/compiler/` implements
superclass entailment, and both plausible spellings are hard parse errors.

The impression that superclasses *do* exist is traceable to **two false
documentation claims**, which are the reason this plan opens with a doc fix
rather than a compiler change:

- `docs/guides/turi-parity-guide.md:53` lists "dispatch, **superclasses**,
  default methods" as an `OK`/`OK` parity row for typeclasses. The feature does
  not exist on either back end, so the row asserts parity for something absent.
- `docs/guides/introducing-saffron.md:599` says
  "[typeclass-guide.md](typeclass-guide.md) covers **superclasses**, defaults
  and the stdlib classes." `typeclass-guide.md` has no superclass section --
  its section list runs `defclass` -> `definstance` -> parametric instances /
  constraints -> constrained functions -> associated types -> fundeps ->
  default methods -> dictionary passing.

A third mention is loose terminology rather than a false claim, and should be
left alone or reworded only for clarity:
`docs/guides/typeclass-internals-guide.md:185` calls `Category` "the superclass
providing `ident`/`comp`" for `Arrow`. That is the Haskell-hierarchy *name* for
the relationship, not a declared `defclass` relation -- in Turmeric the two are
flat classes and `arrow.tur` restates the constraints.

Three independent plans reached the same conclusion on contact, which is strong
corroboration that the gap is real and not a stale doc:
`lattice-vocabulary-plan.md` section 2.3 ("`defclass` has no `extends`; nothing in
`src/compiler/` implements superclass constraints"), `crdt-spice-plan.md:127`,
and `saffron-lang-plan.md:1849` ("This language has no `defclass` superclass
declaration").

### Measured, not assumed

Probed against a freshly built `./build/tur` (v0.48.0 -- the tree's `build/`
was stale at v0.47.0 and had to be reconfigured before the probe meant
anything):

Both candidate spellings fail, and **fail badly** -- the parser has no
superclass slot, so it reads the constraint form as a malformed method and
blames the whole `defclass`:

```
(defclass Monoid [a] => [(Semigroup a)]      ; error: typeclass method requires
  (mempty [] : a))                           ;        (name [params...] : return-type)

(defclass Monoid [a]                         ; error: typeclass method requires
  [(Semigroup a)]                            ;        (name [params...] : return-type)
  (mempty [] : a))
```

The entailment gap is exactly one diagnostic wide. With `Semigroup`/`Monoid`
flat and both instances present for `int`, the guide's two-constraint form
compiles and the one-constraint form does not:

```turmeric
(defn double-up [^Semigroup A ^Monoid A] [x : A] : A   ; OK today
  (combine x x))

(defn double-up-super [^Monoid A] [x : A] : A          ; TUR-E0015
  (combine x x))
```

```
error [TUR-E0015]: 'combine' is a method of typeclass 'Semigroup', but
'double-up-super' does not constrain 'A' to it -- so there is no instance to
dispatch to. Add the constraint: (defn double-up-super [A] [(Semigroup A)] ...).
```

That diagnostic is emitted from one site, `elab_typeclasses.c:6521`, and
teaching that site the superclass closure is the whole payoff of the feature.

## 1. What exists today

### 1.1 The class head has no slot for it

`elab_defclass` (`src/compiler/elab_typeclasses.c:1269`) parses exactly:

```
(defclass Name [type-params] [| (from... -> to...)] method-or-assoc-type...)
```

- Name: `items[1]`, must be `F_SYM`.
- Type params: `items[2]` when `F_VEC`; supports `^f` / `^^f` / `[f :kind]`
  kind annotations. Sets `methods_start = 3`.
- Optional fundep clause: a bare `|` symbol at `methods_start`
  (`elab_typeclasses.c:1405` onward).
- Everything remaining is a method or a `(type Name : Type)` assoc-type.

`struct TypeClass` (`src/compiler/typeclass.h:87`) has fields for type params
and kinds, methods, assoc types, one fundep, origin file, and the
`from_stdlib` flag. **There is no superclass field.**

### 1.2 The constraint machinery that already exists

Superclasses are a *new source* of constraints, not a new kind of constraint.
The representation is already there:

- `TypeConstraint` (`typeclass.h:279`): `typeclass`, `type_arg`, `param_idx`,
  `tyvar`, `return_resolved`.
- `ConstraintSet` (`typeclass.h:293`).
- `TypeClassInstance.type_param_constraints` (`typeclass.h:154`) -- the
  `(definstance Eq [Option] [(Eq A)] ...)` form.
- `Elab.cur_fn_constraints` / `cur_fn_n_constraints`
  (`elab_internal.h:507`), pushed and restored by `elab_fns.c:7931-8219`.

The language already spells "these constraints must hold" as a bracketed vector
of `(Class var)` forms in **two** places -- `definstance` and `defn`. The
design below adds the same vector to `defclass` rather than inventing a third
spelling.

### 1.3 The sites that would need to consult a superclass closure

- **`elab_typeclasses.c:6521`** -- receiver-directed dispatch. Walks
  `cur_fn_constraints` looking for `con->typeclass == owner`, matching by class
  identity *or by name* (so a class re-registered through two import paths still
  counts). This is the site that emits the TUR-E0015 above.
- **`elab_typeclasses.c:5383`** -- the return-directed twin (`mempty`-shaped
  methods, resolved from the expected result type rather than an argument).
  Also consults `cur_fn_constraint_param_mask`.
- **`elab_typeclasses.c:3197`, `:7050`, `:7066`, `:7124`** -- the other
  TUR-E0015 arms (no instance anywhere / no instance applies / concrete-distinct
  receiver). These report; they do not decide entailment, and mostly need only
  message updates.

## 2. Design

### 2.1 Syntax -- the bracketed constraint vector

```turmeric
(defclass Semigroup [a]
  (combine [x : a y : a] : a))

(defclass Monoid [a]
  [(Semigroup a)]
  (mempty [] : a))
```

Recommended over `=>` for one concrete reason: `[(Semigroup a)]` is
character-for-character the form `definstance` and `defn` already use, so it
needs no new grammar concept and no new reader support. A `=>` preamble would
also read backwards relative to Haskell (`class Semigroup a => Monoid a` puts
the superclass *before* the subclass; anything after `(defclass Monoid [a]`
inverts that), which is a standing source of confusion worth not importing.

Multiple and multi-parameter superclasses use the same vector:

```turmeric
(defclass BoundedJoinSemilattice [a]
  [(JoinSemilattice a) (Eq a)]
  (bottom [] : a))
```

**Position.** The vector goes immediately after the type-param vector, matching
`defn`'s `(defn f [W] [(Foo W)] [params] ...)` ordering. The fundep `|` clause
follows it:

```
(defclass Name [params] [[(Super var)...]] [| (from -> to)] methods...)
```

This is unambiguous and backward compatible by form tag at `methods_start`:
`F_VEC` -> constraint vector, `F_SYM` `|` -> fundep clause, `F_LIST` -> method.
Every existing `defclass` in the tree keeps parsing unchanged, including
`(defclass Collect [c e] | (c -> e) ...)`. Writing the two clauses in the other
order is a dedicated diagnostic naming the canonical order, not a silent
accept.

### 2.2 Semantics -- two halves, and the second is what makes the first sound

**Half A -- entailment at the use site (the payoff).** A constraint on a
subclass entails its superclasses, transitively. `[^Monoid A]` licenses
`combine`; a `Monoid`-constrained body needs no separate `^Semigroup A`.

**Half B -- the instance obligation (the guarantee).** `(definstance Monoid
[int] ...)` requires a `Semigroup [int]` instance to exist somewhere in the
program, and is a hard error otherwise.

Half B is not optional polish. Without it, Half A licenses a `combine` call for
which no instance need exist, and the resolver's behavior when no instance
applies is precisely the failure class this codebase has already been bitten
by: `elab_typeclasses.c`'s own comments record a **silent bind to the single
carrier-compatible representative** -- "a wrong-instance dispatch that SIGSEGVs
when the representative's layout differs". Shipping A without B would
manufacture new instances of that bug. **A and B land together or not at all.**

Obligation checking must run *after* every form in the unit is registered, for
the same reason method resolution already does: `typeclass-guide.md` documents
that "instance order does not matter" and `elab_toplevel.c:974` handles a
`definstance` that precedes its class. The check therefore belongs in the
post-unit pass, not inline at `elab_definstance`.

### 2.3 Acyclicity

The superclass graph must be a DAG. `(defclass A [x] [(B x)] ...)` +
`(defclass B [x] [(A x)] ...)` is a hard error naming the cycle, detected when
the closure is computed. A class may not name itself. Forward references are
allowed (a superclass declared later in the file), which is what forces
resolution to be deferred rather than eager -- store the superclass *forms* on
the class and resolve to `TypeClass*` in the post-unit pass, mirroring how
`default_method_form` is deliberately kept unelaborated (`typeclass.h:83`).

### 2.4 No codegen, no runtime cost

> **Revised by SC8b (section 11).** True for a statically resolved call. A
> higher-kinded generic compiled by dictionary passing needs a dictionary per
> implied superclass, so a declared constraint now carries its superclass
> closure and such a generic takes one extra dictionary argument per implied
> class. No snapshot moved, because no snapshot fixture has such a generic.

Static dispatch resolves the instance at the call site from the concrete
instantiation, so a `[^Monoid A]` body calling `combine` at `A = int` reaches
`Semigroup [int]` by the ordinary lookup once entailment lets the call through.
No dictionary field, no new dictionary threading, no emitted-C change. This is
the same "zero runtime cost" shape as associated types
(`typeclass-internals-guide.md:173`).

Expected consequence: **no fixture snapshot regeneration.** If a snapshot does
move, that is a signal the change leaked into codegen and should be
investigated rather than regenerated.

### 2.5 Experiment gate

Per the repo's STRICT RULE on in-flight compiler features, add one fully
populated `EXPERIMENTS[]` row in `src/runtime/experiments.c`:

```c
{ "class-superclasses",
  "defclass constraint preambles (superclass entailment)",
  "docs/upcoming/typeclass-superclasses-plan.md",
  "0.49.0",                  /* introduced */
  "0.55.0",                  /* expires_at -- advisory; never blocks a release */
  XF_LIFECYCLE_PROTOTYPE,
  &g_opt_class_superclasses },
```

Add `g_opt_class_superclasses` to `src/runtime/globals.h`, and call
`experiment_warn_if_used("class-superclasses")` from the superclass-vector
parse in `elab_defclass` so TUR-W0060/W0061 fire.

The gate covers the *parse* of the constraint vector. With the experiment off,
the vector is rejected with a diagnostic pointing at `--enable=`, which keeps
the feature from silently changing entailment for programs that did not ask for
it.

## 3. Implementation phases

**The numbering is the dependency order.** Each phase depends on the one
before it; nothing here is a preference about sequencing. Two edges are the
ones that actually constrain the schedule, and both were mis-stated in an
earlier draft of this plan:

- **SC7 (graduation) gates every stdlib change.** A gated feature cannot be a
  load-bearing stdlib dependency (4.2), so SC8 cannot precede SC7 -- and
  because `beta` means a one-release soak, SC8 is at least one release behind
  SC4, not immediately after it.
- **The doc work splits across that gate.** SC6 documents the gated feature;
  SC9 documents what the stdlib ended up doing. Fusing them into one phase --
  as an earlier draft did -- makes the plan unsatisfiable, because half the
  work is unblocked at SC4 and half is blocked until SC8.

### SC0 -- Documentation correction (independently shippable; land first)

Fixes the two false claims that motivated this investigation. **No compiler
change, no dependency on any later phase.** This is the part that should not
wait behind a post-v1 feature.

1. `docs/guides/turi-parity-guide.md:53` -- drop `superclasses` from the
   typeclass parity row's notes. Replace with an accurate list:
   `dispatch, default methods, associated types` (fundeps are omitted: the only
   fundep fixture carries inline-C, which `run-turi.sh` PASS-skips, so
   interpreter parity for them is not demonstrated by the suite).
2. `docs/guides/introducing-saffron.md:599` -- the cross-reference currently
   promises a section that does not exist. Change "covers superclasses,
   defaults and the stdlib classes" to "covers constraints, defaults and the
   stdlib classes".
3. `docs/guides/typeclass-internals-guide.md:185` -- optional clarity pass:
   "`Category` (which `Arrow` restates as a constraint rather than inheriting
   -- `defclass` has no superclasses)".

### SC1 -- Parse and store

- Extend `struct TypeClass` (`typeclass.h`) with
  `const struct Form **super_forms; uint8_t n_supers;` (unresolved) plus
  `TypeClass **supers;` (resolved in SC2).
- Parse the constraint vector in `elab_defclass` between the type-param vector
  and the fundep clause; gate on `g_opt_class_superclasses`.
- Each element must be `(ClassName var)` where `var` is one of the class's own
  type params. A name not in the param list is a diagnostic; so is a non-list
  element.
- Extend `typeclass_signatures_match` so the idempotent-redeclaration path
  (`elab_defclass`'s `existing` check, used by the stdlib pre-declaration of
  `Eq`/`Functor`) compares superclass lists too -- otherwise a class seen
  through two import paths with differing preambles silently keeps the first.
- New diagnostic codes: malformed element, unknown type variable, clauses out
  of canonical order.

### SC2 -- Resolve and validate the graph

- Post-unit pass: resolve `super_forms` to `TypeClass*`, erroring on an unknown
  class name.
- Compute and cache the transitive closure per class (small N; a bitset over
  registered classes or a flat cached array -- not a per-query graph walk, since
  the closure is consulted on every constrained method call).
- Cycle detection with a diagnostic naming the full cycle path.

### SC3 -- Entailment (Half A)

- `elab_typeclasses.c:6521`: replace the direct `con->typeclass == owner`
  comparison with a closure membership test. **Preserve the existing
  match-by-name fallback inside the closure walk** -- dropping it would regress
  the re-registered-class-through-two-import-paths case that comment documents.
- `elab_typeclasses.c:5383`: same for return-directed dispatch. Check whether
  `cur_fn_constraint_param_mask` needs a superclass-aware analogue; a
  superclass constraint reaches a parameter exactly when the subclass
  constraint that entails it does.
- Update the TUR-E0015 message at `:6521` so that when the method's owner is a
  superclass of a class the function *does* constrain, the diagnostic says so
  rather than telling the author to add a constraint they have effectively
  already written.

### SC4 -- Instance obligations (Half B)

- Post-unit pass over registered instances: for each `(definstance C [T] ...)`,
  every superclass `S` in `C`'s closure must have an instance applying to `T`.
- Reuse the existing instance-applies machinery (including parametric heads and
  holes) rather than writing a second matcher.
- New diagnostic: `"(definstance Monoid [int]) requires a Semigroup [int]
  instance ('Semigroup' is a superclass of 'Monoid'); none is in scope"`.
- A parametric instance's obligation is discharged against its own declared
  constraints, not just ground types: `(definstance Monoid [Vec] [(Monoid A)]
  ...)` needs `Semigroup [Vec]`, which may itself be parametric.

### SC5 -- Tests

New fixtures under `tests/fixtures/` (ASCII only, `--` not em dashes):

| Fixture | Asserts |
| --- | --- |
| `class-superclass-entails` | `[^Monoid A]` body calls `combine`; prints a value |
| `class-superclass-transitive` | 3 levels (`A <- B <- C`); a `C` constraint licenses an `A` method |
| `class-superclass-multi` | two superclasses on one class |
| `class-superclass-forward-ref` | superclass declared *below* the subclass |
| `class-superclass-multiparam` | superclass over a multi-param class |
| `class-superclass-fundep-order` | preamble + `|` clause together |
| `errors/class-superclass-cycle` | cycle diagnostic |
| `errors/class-superclass-missing-instance` | Half B obligation |
| `errors/class-superclass-unknown-tyvar` | element names a non-param |
| `errors/class-superclass-gate-off` | rejected without `--enable=` |

Plus: an interpreter-parity pass (`tests/run-turi.sh`) -- entailment is a
shared-elaborator property, so the positive fixtures should hold on both paths
without `requires.*` markers. If they do not, that divergence is the finding.

Run `bash tests/run.sh` with `timeout: 720000` per the repo rule.

**Precedes SC7.** An experiment cannot graduate on an untested surface, so this
is a gate on graduation, not a trailing chore.

### SC6 -- Documentation while the feature is gated

The half of the doc work that depends only on the implementation, not on
graduation or on any stdlib change:

- `docs/guides/typeclass-guide.md` -- new `## Superclasses` section between
  "Constrained Functions" and "Associated Types": the syntax, entailment, the
  instance obligation, acyclicity, and the `--enable=` gate while the
  experiment is live.
- `docs/guides/experimental-flags-guide.md` -- the new gate.

Once this section exists, `introducing-saffron.md:599` can go back to saying
"superclasses" -- SC0 pointed it at "constraints" precisely because the section
it promised did not exist.

### SC7 -- Graduation

The step that makes the feature unconditional, and therefore the step every
stdlib change waits on. Per
[experimental-flags-guide.md](../guides/experimental-flags-guide.md):

1. **prototype -> beta.** Freeze the surface; flip the row's lifecycle so
   TUR-W0061 replaces TUR-W0060.
2. **Soak one release cycle.** `beta` means exactly that; this is why stdlib
   adoption is at least one release behind SC4, not immediately after it.
3. **Graduate.** Delete the `EXPERIMENTS[]` row, move the name to
   `GRADUATED[]` so a lingering `--enable=class-superclasses` is a TUR-W0063
   no-op rather than a hard error, and make the parse unconditional.

**Recommendation: keep no bisection hatch.** The guide warns that a surviving
hatch's harness *inverts* at graduation rather than retiring, and that four
graduations hit this with three getting it right (`sr2-carrier-seam-rotted`).
That trap applies to features that flipped a *representation* default, where
both paths compiled. Superclasses are purely additive syntax: before
graduation a preamble does not parse at all, so there is no old path to keep
covered and no `TUR_CLASS_SUPERCLASSES=0` worth carrying. Confirm this when
the phase is written rather than inheriting it from here.

Precedent for the whole shape: `backtrackable-state` (`experiments.c:34`)
graduated 2026-08-29, and only then did `stdlib/trail.tur` become
unconditionally autoloaded. Gate -> graduate -> stdlib, in that order.

### SC8 -- stdlib adoption

Gated on SC7 only. The 4.2 decision is made: **the stdlib adopts
superclasses.** The phase is staged so that each step's blast radius is
known before it lands, and the audit that sizes each step was run once
already (section 9.3) so the numbers below are measured, not assumed.

**SC8a -- the lattice file. LANDED 2026-09-25 (unreleased; section 10).**
`stdlib/typeclass-lattice.tur` is loaded explicitly, never auto-loaded, so
its classes have the smallest downstream footprint of any candidate. Three
preambles, all of which the file's own instances already satisfy:

| Class | Preamble | Own instances | Each has the superclass instance? |
| --- | --- | --- | --- |
| `Monoid` | `[(Semigroup a)]` | `Sum Product MinI MaxI Any All` | yes, all six |
| `BoundedJoin` | `[(JoinSemilattice a)]` | `MaxI Any` | yes |
| `BoundedMeet` | `[(MeetSemilattice a)]` | `MinI All` | yes |
| `JoinSemilattice`, `MeetSemilattice` | none | | withheld on purpose |

`JoinSemilattice` is deliberately **not** declared over `Semigroup`: the two
classes have the same shape and differ only in laws, and a preamble would
let a `[^JoinSemilattice A]` body call `combine`, which is the wrong name
for a join. The file's comment says the relation is withheld on purpose,
and the join associativity law stays restated rather than borrowed.

**SC8b -- the auto-loaded hierarchy. LANDED 2026-09-25 (unreleased; section
11), one commit per step, in the order below.** These are the classes every program sees. Every candidate was
applied together on 2026-09-25 and measured against the full `run.sh`
suite (3182 fixtures), the stdlib itself, and every spice at
`turmeric-spices` `origin/main` (section 9.3). Two facts shape the order:

- **Every retrofit lands in two files.** `stdlib/typeclass.tur` (loaded
  explicitly) re-declares `Functor`, `Applicative`, `Monad`, `Alternative`,
  `Bifunctor` and `Clone` alongside the auto-loaded `typeclass-*.tur`
  copies, and the idempotent-redeclaration check compares preambles, so a
  preamble on one copy and not the other is a redeclaration conflict for any
  program that loads both.
- **The spices use almost none of these classes.** Across all 48 spices
  there are 5 `Functor` and 9 `Eq` instances, and no `Ord`, `Applicative`,
  `Monad`, `Alternative`, `MonadError`, `Traversable` or arrow instances.
  The downstream cost of every SC8b step below is zero today.

In order, cheapest first:

1. **`Ord` over `Eq` -- ready.** Zero breakage: stdlib 15/15, no fixture
   failed, no spice has an `Ord` instance. The single most useful retrofit,
   since `[^Ord A]` bodies routinely want `eq?`.
2. **`Alternative` over `Applicative`, `MonadError` over `Monad`, and
   `Traversable` over `Functor` and `Foldable` -- ready.** Zero breakage on
   all three counts. `Monad` itself can stay flat while `MonadError` points
   at it.
3. **`Applicative` over `Functor` -- ready, with six fixture edits.** The
   stdlib is clean (6/6), but six HKT fixtures declare `Applicative` for a
   toy type with no `Functor`: `hkt-constrained-continuation-dict`,
   `hkt-constrained-middle-vector-dict`,
   `hkt-constrained-pure-return-dispatch`, `hkt-constrained-pure-two-instances`,
   `hkt-rank2-forall-pure-two-instances`, `hkt-rank2-result-only-pin`. Each
   gains a one-line `Functor` instance in the same change.
4. **The arrow hierarchy in `stdlib/arrow.tur` (not auto-loaded) -- ready,
   with one correction.** `Arrow` over `Category`, and `ArrowChoice`,
   `ArrowLoop`, `ArrowApply` over `Arrow`, and `ArrowPlus` over `ArrowZero`,
   break nothing. **`ArrowZero` over `Arrow` breaks the stdlib**:
   `stdlib/kleisli.tur` gives `Kleisli` a `Category` and an honest
   `ArrowZero` but deliberately no `Arrow`. Declare `ArrowZero` over
   `Category` instead, which every instance satisfies and which is all its
   laws need.
5. **`Monad` over `Applicative` -- ready, with one fixture edit.** It was
   blocked because `stdlib/result.tur` had `Functor` and `Monad` for
   `(Result _ B)` but no `Applicative`, and compiled `ap` over a partial head
   segfaulted. That was fixed on 2026-09-25
   ([partial-head-ap-calls-fat-closure-as-thin-pointer](../archive/partial-head-ap-calls-fat-closure-as-thin-pointer.md))
   and `Applicative [(Result _ B)]` now ships. Measured with the preamble
   applied: the full suite has one failure, `hkt-constrained-wide-byvalue-carrier`,
   whose `Pad2` declares `Monad` with no `Functor` or `Applicative`; it gains
   both instances in the same change. **Do not** take `Monad` over `Functor`
   as a stopgap: moving it to `Applicative` later would be a second breaking
   change for every downstream `Monad` instance.

`Eq`, `Functor`, `Hash`, `Show`, `Clone`, `Drop`, `Bifunctor`, `Num`,
`From`/`Into` have no natural superclass and stay flat.

**The spices audit (4.1) happens at each SC8 step.** A preamble on an
existing class obliges every existing instance of it, in every downstream
spice, to have the superclass instance -- the TUR-E0393 arm fires in
*their* build, not ours. The 2026-09-25 audit found nothing to fix, but
spices keep landing: before each step, clone `turmeric-spices` at
`origin/main`, re-run the script in section 9.3, and land any missing
superclass instances in the spice **before** the stdlib preamble ships.

### SC9 -- Documentation after stdlib adoption

The half of the doc work that could not be written earlier, because a guide
must describe what the stdlib *does*, not what it could do. **Landed with
SC8a** (section 10):

- `docs/guides/lattice-guide.md` -- the class table has a Superclass column,
  and the `## Monoid` section shows the subclass declaration and the
  single-constraint `double-up`, in both spellings, plus the instance
  obligation. Current behavior only, per the no-archeology rule.
- `docs/guides/turi-parity-guide.md` -- the typeclass row's notes list
  superclasses, backed by the `class-superclass-*` and `stdlib-*` fixtures
  passing under both harnesses with no `requires.*` markers.
- `docs/guides/typeclass-guide.md` and `typeclass-internals-guide.md` -- no
  longer call the whole stdlib flat; they say which classes carry preambles
  and that the auto-loaded ones are retrofitted one at a time.

Each SC8b step carries its own doc edit in the same change: the
typeclass guide's sentence listing which stdlib classes have preambles, and
the arrows guide for step 4.

## 4. Risks and decisions

### 4.1 Retrofitting a superclass is a breaking change

Adding `[(Semigroup a)]` to an existing `Monoid` retroactively obliges **every**
existing `Monoid` instance to have a `Semigroup` instance. For a new class
that is free; for an existing one it can break downstream spices. The gate
contains this during the prototype, but graduation needs an audit of any class
that grows a preamble. This is the single biggest reason the feature is
post-v1 rather than opportunistic.

### 4.2 Decided: the stdlib adopts it

`stdlib/typeclass-lattice.tur` is the obvious first consumer, and
`crdt-spice-plan.md:586` names the ergonomic cost it would relieve
(`[^JoinSemilattice A ^BoundedJoinSemilattice A ^Eq A ^Hash A ^MapKey A]`).
But a gated feature cannot be a load-bearing stdlib dependency -- the stdlib
must compile with the experiment off. So either the stdlib waits for
graduation, or it carries both spellings behind the gate, which is worse.
**Decision: the stdlib adopts superclasses, and adoption waits for
graduation.** That is SC7 -> SC8 in the phase list; SC9's
`lattice-guide.md` edit is gated on SC8 in turn, which is why it cannot be
written earlier. The question is no longer *whether* but *in what order*,
and SC8 is staged accordingly. A flat stdlib after graduation would leave
the feature shipped and unused, which is the worst of the three outcomes:
every language surveyed in section 9 that has superclasses uses them in its
own prelude (`Ord` over `Eq`, `Monad` over `Applicative`), and a user
reading the stdlib learns the idiom from what the stdlib does.

### 4.3 Out of scope

- **Dynamic (Saffron) dispatch entailment.** `saffron-lang-plan.md:1849`
  (Q1) already owns the harder half -- a synthesised `dict_Show_any` whose slot
  does a registry lookup on the element's tag -- and records it as sitting
  behind Q3. This plan is static-dispatch only; the Saffron path keeps its
  current behavior and the parity row stays absent until it does not.
- **Superclass default methods** (a default body on a subclass calling a
  superclass method). Defaults are kept as unelaborated forms and spliced per
  instance (`typeclass.h:83`); the interaction is worth a deliberate pass, not
  a side effect.
- **Constraint aliases.** `lattice-vocabulary-plan.md` section 2.3 notes `defalias`
  is a *type* alias (TA1/TA2, `elab_call.c:3428`), not a constraint alias. A
  one-name-for-five-constraints alias is a different, possibly better answer to
  the same ergonomic complaint, and should be evaluated on its own.
- **Superclass-directed method *inheritance*.** A `Monoid` instance does not
  supply `combine`; it requires a `Semigroup` instance that does.

## 5. Recommendation

Land **SC0 now, on its own.** The two false doc claims are a live
correctness defect in the published guides, they are what made this question
necessary, and they cost nothing to fix.

Hold **SC1-SC9 as post-v1.** The feature is well-scoped, needs no codegen, and
has a clean single-site payoff, but it is ergonomics rather than
expressiveness -- everything it enables is already writable by listing both
constraints, which the lattice guide documents and which compiles today. Per
the one-track-to-v1 rule, it should not displace v1 work. The `crdt-spice-plan`
disposition remains right: if constraint-list ergonomics turn out to block
adoption, that is the data point that promotes this plan.

**Where this stands (2026-09-25).** SC0-SC6 landed in 0.49.0; SC7
graduated the feature in 0.54.0 (section 8); SC8a, SC8b and the SC9 docs
landed after it, unreleased (sections 10 and 11). The stdlib now declares
Haskell's hierarchy wherever the relation is real: `Ord` over `Eq`, the
`Functor`/`Applicative`/`Monad` chain with `Alternative`, `MonadError` and
`Traversable`, the lattice classes, and the arrows. The recommendation to
hold SC1-SC9 until after v1 was overtaken by the decision to adopt (4.2); the
work is done. What remains are the pre-existing compiled-path gaps section 11
lists, which the fixtures route around and which are filed as reports.

## 6. See also

- [lattice-vocabulary-plan.md](lattice-vocabulary-plan.md) section 2.3 -- independent
  confirmation, and the verified two-constraint workaround.
- [crdt-spice-plan.md](crdt-spice-plan.md):127, :586 -- the ergonomic cost in a
  real spice, and the "file it as a report" disposition.
- [saffron-lang-plan.md](saffron-lang-plan.md):1849 -- Q1, the dynamic-dispatch
  half, deliberately out of scope here.
- [../guides/typeclass-guide.md](../guides/typeclass-guide.md) -- the constraint
  forms this design reuses.
- [../guides/typeclass-internals-guide.md](../guides/typeclass-internals-guide.md)
  -- dictionary lowering, and the zero-cost precedent set by associated types.

## 7. Landed (2026-09-16): SC0-SC6

What shipped, and where it departs from the phases above.

- **SC0** had already landed separately: neither `turi-parity-guide.md` nor
  `introducing-saffron.md` claimed superclasses by the time this branch was
  cut, and `typeclass-internals-guide.md` already carried the "`defclass`
  has no superclasses" clarification. `introducing-saffron.md` now says
  "constraints, superclasses, defaults" again, as SC6 anticipated, because
  the section it points at exists.
- **SC1** -- `struct TypeClass` gained `super_forms` / `super_n_args` /
  `super_arg_idx` / `n_supers` (unresolved) and `supers` (resolved), plus
  `decl_form` for post-unit diagnostics; `TypeClassInstance` gained
  `decl_form` and `super_obligations_ok`. `elab_defclass` parses the vector
  between the type-param vector and the `|` clause, gated on
  `g_opt_class_superclasses`, and `typeclass_signatures_match` compares
  preambles. Each element may name up to `TUR_SUPER_MAX_ARGS` (4) of the
  class's own variables, so a multi-parameter superclass is supported.
  Four diagnostic codes: TUR-E0390 (preamble: gate off, malformed element,
  unknown variable, vector after the `|` clause), TUR-E0391 (superclass
  unresolved, or its arity/kinds do not fit), TUR-E0392 (cycle, with the
  path), TUR-E0393 (instance obligation).
- **SC2** -- `elab_typeclass_superclasses_finish` runs from
  `elaborate_program_session` after the deferred-defn second chance: resolve
  by name, arity + kind check per element, cycle detection (each cycle
  reported once), then Half B. Before that pass `typeclass_entails` resolves
  supers by name on demand, so entailment works mid-unit. The closure is a
  bounded DFS with a visited set rather than a cached bitset -- class counts
  are tiny and the walk only runs on tyvar-receiver dispatch.
- **SC3** -- `elab_typeclasses.c`'s receiver-directed constraint walk and the
  return-directed `tyvar_reaches_param` walk both go through
  `typeclass_entails`, which keeps the by-name match inside the closure. The
  TUR-E0015 wording is unchanged: a superclass of a constrained class is
  entailed, so there is no "you effectively wrote it" case left to word.
- **SC4** -- the obligation is checked against each DIRECT superclass;
  transitivity follows because every registered instance is checked and the
  instance found is one of them. The lookup is
  `typeclass_env_lookup_instance`, so parametric heads discharge against
  their own constraints as planned. A REPL session marks a discharged
  instance so it is not re-reported every turn.
- **Interpreter.** Entailment IS a shared-elaborator property, but turi's
  dictionary passing was not: `frame_bind_constraint_dicts` bound one
  dictionary per declared constraint, so a `[^Mo A]` frame carried no `Sg`
  dictionary and a `combine` call at a parametric `(Option int)` fell back
  to the int representative (`class-superclass-parametric-instance` printed
  nothing). It now binds the superclass closure's dictionaries at the same
  tyvar. This was the divergence SC5 said to look for.
- **SC5** -- the ten planned fixtures plus `class-superclass-hkt`,
  `class-superclass-parametric-instance`, `class-superclass-return-directed`,
  `errors/class-superclass-unknown-super`, `errors/class-superclass-fundep-order`
  and `errors/class-superclass-kind-mismatch`. All pass under `run.sh` and
  `run-turi.sh` with no `requires.*` markers.
  `tests/run-experiments-user-config.sh` regained its gated-source probe
  (E-G) on this row. No fixture snapshot moved (2.4 held).
- **SC6** -- `typeclass-guide.md` `## Superclasses`,
  `experimental-flags-guide.md`'s registry note.
- **Deliberately not done:** stdlib adoption (SC8) and the lattice-guide /
  parity-table edits (SC9), which are gated on SC7 and on the retrofit audit
  in 4.1. The two stdlib comments at `typeclass-lattice.tur:60` and `:258`
  remain true and stay.

## 8. Landed (2026-09-25): SC7 -- graduation

The feature is unconditional. `--enable=class-superclasses` is accepted as a
TUR-W0063 no-op for the usual one-minor-line window (eligible to age out of
`GRADUATED[]` at 0.54.0).

Where it departs from the phases above:

- **Straight from `prototype`, with no `beta` hop.** The phase list's step 1
  (flip the lifecycle so TUR-W0061 replaces TUR-W0060) and step 2 (soak one
  release cycle) describe a soak this row had already served in `prototype`:
  it registered at 0.49.0 and the implementation landed 2026-09-16, four minor
  lines back, with the surface frozen since -- `defclass` gained no new
  preamble syntax after SC1. A beta hop would have been a second soak of a
  surface nothing had moved, and graduating ahead of `expires_at` (0.55.0) is
  routine per
  [experimental-flags-guide.md](../guides/experimental-flags-guide.md)
  (`closure-drop-glue` graduated at 0.30.2 carrying 0.34.0; `r7rs-gc`
  graduated in the line it was introduced).
- **No bisection hatch, as SC7 recommended, and the reasoning held on
  contact.** The guide's inversion trap is about a graduation that flips a
  *representation* default, where both paths compiled and the old one quietly
  loses its cover. This is purely additive syntax: before graduation the
  preamble did not parse at all, so there is no old path and no
  `TUR_CLASS_SUPERCLASSES=0` worth carrying. `g_opt_class_superclasses` is
  retired with the row rather than kept as an A/B switch.
- **`errors/class-superclass-gate-off` is deleted.** It asserted the gate's
  refusal, and there is no gate to refuse. Its slot is taken by
  `errors/class-superclass-empty-preamble`, which pins a real TUR-E0390 branch
  that had no fixture: `[]` is rejected rather than read as "declares no
  superclasses". The other fifteen fixtures lost their `flags` file and now
  compile ungated; none of their assertions changed.
- **TUR-E0390 lost a cause.** Its title ("Malformed **or unavailable**") and
  the first bullet of `tur explain` both named the missing enable. The code now
  covers only malformed preambles: empty vector, an element that is not
  `(Class var...)`, a variable that is not one of the class's own parameters,
  or the vector placed after the `|` clause.
- **The E-G probe in `tests/run-experiments-user-config.sh` skips again.**
  That probe needs a row enabled by a flag a user writes plus a source that is
  refused without it; `class-superclasses` was the last such pair. The one
  remaining row, `r7rs`, cannot serve -- its `#lang` line is itself the enable
  at CLI precedence, so no `#lang r7rs` source is ever refused and no manifest
  `:experiments []` suppresses it, which is exactly what E, F and G assert. The
  registry-independent cases (A-D) still run. Noted in the script's header so
  the next flag-enabled row repoints it rather than rediscovering this.
- **Still not done, and unchanged by graduation: SC8.** The stdlib's classes
  stay flat and the two comments at `typeclass-lattice.tur:60` and `:258`
  remain true. Retrofitting `[(Semigroup a)]` onto the existing `Monoid`
  obliges every existing instance, in-tree and across the spices, to carry a
  `Semigroup` instance (4.1). Graduation *unblocked* that audit; it did not
  perform it, and SC9's `lattice-guide.md` and parity-table edits stay gated
  on it. `typeclass-guide.md` now says the stdlib is flat as a statement of
  current behavior rather than "waits for the experiment to graduate".

## 9. Prior art, and what it settles

A survey of the languages that have typeclasses or a typeclass-shaped
dispatch, taken 2026-09-25 to check the design above against what ships
elsewhere. Two findings bear on this plan directly and are recorded here so
they are not re-derived.

### 9.1 Every implicit-instance language ships both halves together

| Language | Spelling | Entailment | Instance obligation |
| --- | --- | --- | --- |
| Haskell | `class Eq a => Ord a` | yes | yes |
| PureScript | `class Eq a <= Ord a` | yes | yes |
| Idris 1 / 2 | `interface Eq a => Ord a` | yes | yes |
| Lean 4 | `class Ord a extends Eq a` | yes | yes (parent projection) |
| Rocq (Coq) | `Class Ord A := { ord_eq :> Eq A; ... }` | yes | yes |
| Mercury | `:- typeclass ord(T) <= eq(T)` | yes | yes |
| Clean, Frege | Haskell-style | yes | yes |
| Rust | `trait Ord: Eq + PartialOrd` | yes | yes (`impl Ord` without `impl Eq` is an error) |
| Swift | `protocol Comparable: Equatable` | yes | yes |
| Scala 3 | `trait Ord[A] extends Eq[A]` + `given` | yes (subtyping) | yes (the given implements the parent) |

**Nobody ships entailment without the obligation.** Every language in the
table that resolves instances implicitly *and* has an instance declaration
form implements Half A and Half B together. That is independent
confirmation of section 2.2's "A and B land together or not at all"; the
languages that skipped superclasses altogether did so by having no
user-defined classes (Elm's fixed `comparable`/`number`), or by choosing
flat abilities as a young-language simplification (Roc, Carp, Koka's
implicits, Gleam's explicit dictionaries). Structural systems with
entailment but no instances (Go interface embedding, C++ concept
subsumption, ML signature `include`) are a different shape and not a
counterexample: with no instance declaration there is nothing to oblige.

Rust's rule is the closest match to this plan, down to "the superclass
instance is *required*, never inherited" (section 4.3, last bullet): an
`impl Ord` does not supply `eq`, an `impl Eq` does.

### 9.2 Dictionary placement: two designs, and which one this is

Haskell and PureScript store the superclass dictionary **inside** the
subclass dictionary: a `Monoid` dictionary carries its `Semigroup`
dictionary as a field, and `combine` under a `Monoid` constraint is one
projection away. Rust and Swift instead resolve the superclass instance
**separately at the use site** from the concrete type, with no link in the
emitted representation.

Turmeric's implementation is the second design. That is why section 2.4's
"no codegen change" held (static dispatch already reaches `Semigroup [int]`
by ordinary lookup once entailment lets the call through), and it is
exactly why the interpreter needed its own fix (section 7, "Interpreter"):
turi's frames bind one dictionary per constraint, so there was no embedded
`Semigroup` dictionary to project out of the `Monoid` one, and the closure's
dictionaries had to be bound alongside the subclass's. Anyone tempted to
"optimise" by embedding superclass dictionaries later should know that
would be a change of design, not a refinement, and would put codegen back in
scope.

### 9.3 The in-tree retrofit audit (measured 2026-09-25)

The set-difference that sizes each SC8 step, run over `stdlib/` and
`tests/`: for each candidate `(Sub, Super)` pair, every type with a
`definstance Sub` head must also have a `definstance Super` head.

```sh
inst(){ grep -rhoE "\(definstance $1 \[[^]]*\]" $2 | sed -E "s/\(definstance $1 //" | sort -u; }
comm -23 <(inst Monoid stdlib) <(inst Semigroup stdlib)   # -> empty
comm -23 <(inst Ord stdlib)    <(inst Eq stdlib)          # -> empty
comm -23 <(inst Monad stdlib)  <(inst Applicative stdlib) # -> [(Result _ B)]
```

The same script over `turmeric-spices` (48 spices, `origin/main` at
`ebd1f81`, 2026-09-17), and the full `run.sh` suite with every candidate
preamble applied at once (3182 fixtures, 7 failures, each attributed below
by the class its TUR-E0393 names):

| Pair | stdlib misses | tests misses (full suite) | spices misses |
| --- | --- | --- | --- |
| `Monoid` <- `Semigroup` | none (6/6) | none | none (no instances) |
| `BoundedJoin` <- `JoinSemilattice` | none (2/2) | none | none (6/6, `crdt`) |
| `BoundedMeet` <- `MeetSemilattice` | none (2/2) | none | none (no instances) |
| `Ord` <- `Eq` | none (15/15) | none | none (no instances) |
| `Applicative` <- `Functor` | none (6/6) | six `hkt-*` fixtures (SC8b step 3) | none (no instances) |
| `Alternative` <- `Applicative` | none (6/6) | none | none (no instances) |
| `MonadError` <- `Monad` | none (2/2) | none | none (no instances) |
| `Traversable` <- `Functor`, `Foldable` | none | none | none (no instances) |
| `Arrow` <- `Category` | none | none | none (no instances) |
| `ArrowZero` <- `Arrow` | **`Kleisli`** in `stdlib/kleisli.tur` | `kleisli-arrow-instance` | none (no instances) |
| `ArrowZero` <- `Category` | none | none | none (no instances) |
| `ArrowChoice`/`ArrowLoop`/`ArrowApply` <- `Arrow`, `ArrowPlus` <- `ArrowZero` | none | none | none (no instances) |
| `Monad` <- `Functor` | none (6/6) | `Pad2` in `hkt-constrained-wide-byvalue-carrier` (grep audit; not in the suite run) | none (no instances) |
| `Monad` <- `Applicative` | none, once `Applicative [(Result _ B)]` shipped (was: `(Result _ B)`) | `Pad2` in `hkt-constrained-wide-byvalue-carrier` (full suite, re-measured) | none (no instances) |

The `crdt` spice is the only one that loads the lattice file. Its own test
suite (`tur test tests/crdt`, 8 tests including a 400-seed convergence run)
passes against SC8a with every SC8b candidate applied.

Head-shape matching (`[(Result _ B)]` vs a differently named variable) is by
structure in the compiler's `typeclass_env_lookup_instance`, so a mismatch
the script reports on variable *names* alone is a false positive; a missing
head is not.

## 10. Landed (2026-09-25): SC8a and its SC9 docs

The first stdlib adoption, unreleased at the time of writing.

- **`stdlib/typeclass-lattice.tur`.** `Monoid` is declared over `Semigroup`,
  `BoundedJoin` over `JoinSemilattice`, `BoundedMeet` over `MeetSemilattice`.
  `mconcat`, `mconcat-from`, `law-identity?`, `law-bottom-identity?` and
  `law-top-identity?` dropped to a single constraint. `JoinSemilattice` and
  `MeetSemilattice` stay flat on purpose, and the file says so.
- **No instance had to be added.** Every stdlib `Monoid`, `BoundedJoin` and
  `BoundedMeet` instance already sat beside its superclass instance, and the
  only spice that loads the file, `crdt`, was in the same position.
  Existing two-constraint signatures such as `[^Semigroup A ^Monoid A]` keep
  compiling (`typeclass-nullary-method-newtype-tyvar` is one).
- **Fixtures.** `stdlib-lattice-superclass-entails` calls each
  single-constraint generic at two or more instances, so a superclass call
  that bound to the first registered instance would print a wrong line.
  `errors/stdlib-monoid-requires-semigroup` pins TUR-E0393 against the
  stdlib's own `Monoid`. Both pass under `run.sh` and `run-turi.sh`, as do the
  existing lattice, nullary-method and `class-superclass-*` fixtures. The
  stdlib doctest totals did not move.
- **Docs (SC9).** Lattice guide, typeclass guide, typeclass internals guide,
  turi parity guide, and the changelog's `[Unreleased]` entry, which names
  the breaking half: a downstream `Monoid`, `BoundedJoin` or `BoundedMeet`
  instance without its superclass instance now stops at TUR-E0393.
- **Found while measuring SC8b, and fixed the same day:** compiled `ap` over
  a partial-head instance segfaulted, which held `Monad` over `Applicative`.
  The instance body read a hole-at-0 head with its arms swapped, the call site
  could not ground `ap`'s result, and the head binding ran inside generics;
  all three are fixed in `src/compiler/elab_typeclasses.c`, and
  `Applicative [(Result _ B)]` ships in `stdlib/result.tur`
  ([resolved report](../archive/partial-head-ap-calls-fat-closure-as-thin-pointer.md)).
  Two older defects surfaced on the way and are filed, not fixed:
  [a `bool` closure read through the int64 carrier](../reported/narrow-closure-result-read-through-int64-carrier.md)
  can print true for false on the compiled path, through `Result`'s `fmap`
  already and now through its `ap`; and
  [a user constructor holding a capturing closure](../reported/defdata-ctor-fn-field-passes-pointer-as-int.md)
  emits a C warning. Separately, `Kleisli`'s missing `Arrow` instance is by
  design, which moved `ArrowZero`'s planned superclass from `Arrow` to
  `Category`.
- **A diagnostic nit for whoever next touches TUR-E0393:** the message
  reads "requires a Applicative [T] instance" and "requires a Arrow [T]
  instance". Fixture `expected.diag` files match on the "requires a
  <Class> [<T>] instance" substring, so rewording it means updating them in
  the same change.

## 11. Landed (2026-09-25): SC8b

Five commits, one per step, each with its own fixture, docs and changelog
entry. The spices audit ran against `turmeric-spices` `origin/main` at
`ebd1f81`: no spice declares an instance of any retrofitted class other than
`crdt`'s six `BoundedJoin` instances (SC8a), all satisfied.

| Step | Preambles | Needed to satisfy the obligation |
| --- | --- | --- |
| 1 | `Ord` over `Eq` | nothing |
| 2 | `Alternative` over `Applicative`; `MonadError` over `Monad`; `Traversable` over `Functor`, `Foldable` | nothing |
| 3 | `Applicative` over `Functor` | a `Functor` instance in six test fixtures |
| 4 | `Arrow`, `ArrowZero` over `Category`; `ArrowChoice`, `ArrowLoop`, `ArrowApply` over `Arrow`; `ArrowPlus` over `ArrowZero` | nothing |
| 5 | `Monad` over `Applicative` | `Applicative [(Result _ B)]` (shipped with the `ap` fix); `Functor` and `Applicative` in one test fixture |

Where it departs from the plan above:

- **A declared constraint now implies its superclasses (step 2).** Section
  2.4 held for statically resolved calls only. A higher-kinded generic is
  compiled by dictionary passing, and it received only the dictionaries its
  constraints named: `(defn f [^Alternative F] ... (pure d))` called `pure`
  through the `Alternative` dictionary's slot (a C type error, or a wrong
  method if the slots had matched), and the interpreter, which binds frame
  dictionaries from the same list, found no `Applicative` dictionary for a
  return-directed method. `typeclass_constraints_with_supers` (typeclass.c)
  now appends each single-parameter constraint's superclass closure at the
  same type variable, after the declared constraints, and `elab_defn` applies
  it once every constraint spelling is collected. Pinned by
  `class-superclass-hkt-dict-passing`, which uses user classes and two
  instances so a wrong dictionary shows. The earlier `class-superclass-return-directed`
  fixture had covered only a kind-`*` class, which resolves statically.
- **A constrained rank-2 `forall` expands the same way (step 3).**
  forall-dict-pass aligns a forall's dictionary slots with the inner
  function's constraint list by position, so once `[^Applicative m]` implied
  `^Functor m` the forall's `[(Applicative m)]` had to imply it too
  ("constraint count mismatch" otherwise). `elab_types.c` applies the same
  function. Existential constraint lists are left as declared: their pack
  layout is a separate ABI and nothing in the stdlib needs it.
- **`ArrowZero` is over `Category`, not `Arrow` (step 4)**, as measured in
  section 9.3: `Kleisli` is a `Category` with an honest zero arrow and no
  `Arrow` instance.
- **Fixture shapes route around pre-existing compiled-path gaps.** Each gap
  below reproduces on the compiler before SC8b with every constraint
  spelled out, prints the right answer under `--interpret`, and is filed:
  - [hkt-generic-forwarded-bind-continuation-segfaults](../archive/hkt-generic-forwarded-bind-continuation-segfaults.md)
    (high) -- a generic forwarding a continuation parameter to `bind`.
    **Fixed** in the follow-up bug-fix PR.
  - [hkt-generic-none-to-typed-param-segfaults](../archive/hkt-generic-none-to-typed-param-segfaults.md)
    (high) -- a `none` from a generic passed to a typed `Option` parameter.
    **Fixed** in the follow-up bug-fix PR.
  - [hkt-generic-nested-bind-result-type](../reported/hkt-generic-nested-bind-result-type.md)
    (medium) -- a two-binding `do-m` in a generic does not compile.
  - [hkt-dict-generic-byvalue-result-to-typed-param](../reported/hkt-dict-generic-byvalue-result-to-typed-param.md)
    (medium) -- a user by-value type from a generic at a typed parameter.
  - [generic-category-base-passes-carrier-to-arrow-instance](../reported/generic-category-base-passes-carrier-to-arrow-instance.md)
    (low) -- a C warning in a generic's base clone at the function arrow,
    which is why step 4 has no positive generic fixture.
  - Also pre-existing and not filed: `Ord [cstr]` is inline C with no
    interpreter twin, so `stdlib-ord-entails-eq` leaves out `cstr`.
- **Full suites at the end of step 5:** 3192 compiled and 2276 interpreted
  fixtures, 0 failures; doctests unchanged at 190 passed; no snapshot moved
  across SC8b; the generated docstring table is unchanged.
