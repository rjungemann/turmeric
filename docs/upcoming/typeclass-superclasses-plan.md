# Typeclass superclasses: `defclass` constraint preambles

> **Status:** proposed (2026-09-16). **Track:** post-v1.
> **Type:** compiler feature (elaboration only, no codegen), plus a
> **documentation correction that is independently shippable and should land
> first**.
> **Gate:** `--enable=class-superclasses` (new `EXPERIMENTS[]` row).

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

### SC8 -- stdlib adoption (conditional on 4.2)

Gated on SC7, and on the 4.2 decision actually going the adopting way -- this
phase is conditional, not assumed.

- `stdlib/typeclass-lattice.tur` grows the preamble on the classes that want
  it, and the two in-source comments at `:60` and `:258` asserting "defclass
  has no superclasses" come out.
- **The retrofit audit (4.1) happens here, and it is the real cost.** Adding
  `[(Semigroup a)]` to an existing `Monoid` retroactively obliges every
  existing `Monoid` instance -- including ones in downstream spices -- to have
  a `Semigroup` instance. Graduation defers this; it does not remove it. Audit
  `/Users/rjungemann/Projects/turmeric-spices` against `origin/main`, not a
  stale working tree.

### SC9 -- Documentation after stdlib adoption

The half of the doc work that could not be written earlier, because a guide
must describe what the stdlib *does*, not what it could do:

- **`docs/guides/lattice-guide.md` -- the explicitly requested update.** The
  `## Monoid` prose at :77-79 currently reads "It is declared **flat**, not as
  a subclass of `Semigroup`, because `defclass` has no superclasses -- so a
  function needing both lists both constraints." That is **true until SC8
  lands**; editing it any earlier makes the guide wrong in the other
  direction. Replace it with the subclass declaration and the
  single-constraint function:

  ```turmeric
  (defclass Monoid [a]
    [(Semigroup a)]
    (mempty [] : a))

  (defn double-up [^Monoid A] [x : A] : A
    (combine x x))
  ```

  Update the paired `sweet-exp` block directly beneath it -- the guide carries
  both spellings for every example, `check-guide-pairs.py` enforces it in CI,
  and a half-updated pair is its own defect.

  Per the repo's no-archeology rule, the guide states current behavior only --
  no "this used to be flat" note. The history belongs on this plan.

- `docs/guides/turi-parity-guide.md` -- re-add a `superclasses` row. It waits
  until here because a parity table describes the shipped language, and a
  gated feature is not that. SC0 removed the row precisely because it claimed
  parity for something absent.

## 4. Risks and decisions

### 4.1 Retrofitting a superclass is a breaking change

Adding `[(Semigroup a)]` to an existing `Monoid` retroactively obliges **every**
existing `Monoid` instance to have a `Semigroup` instance. For a new class
that is free; for an existing one it can break downstream spices. The gate
contains this during the prototype, but graduation needs an audit of any class
that grows a preamble. This is the single biggest reason the feature is
post-v1 rather than opportunistic.

### 4.2 Open: does the stdlib adopt it?

`stdlib/typeclass-lattice.tur` is the obvious first consumer, and
`crdt-spice-plan.md:586` names the ergonomic cost it would relieve
(`[^JoinSemilattice A ^BoundedJoinSemilattice A ^Eq A ^Hash A ^MapKey A]`).
But a gated feature cannot be a load-bearing stdlib dependency -- the stdlib
must compile with the experiment off. So either the stdlib waits for
graduation, or it carries both spellings behind the gate, which is worse.
**Recommendation: stdlib adoption waits for graduation.** That is SC7 -> SC8
in the phase list; SC9's `lattice-guide.md` edit is gated on SC8 in turn, which
is why it cannot be written earlier.

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
