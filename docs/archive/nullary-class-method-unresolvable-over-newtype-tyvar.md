# A nullary class method will not resolve against a `defopaque` type variable

**Severity:** medium -- a hard error, not a wrong answer, but it blocks the
standard newtype-per-instance idiom outright. The diagnostic
(`no instance 'Monoid tyvar'`) names neither the newtype nor the constraint
that is in fact present.

**RESOLVED 2026-09-11** -- see Execution at the end.

**Status when filed:** open. Found 2026-09-11 while probing Monoid shapes for a lattice
vocabulary (see [crdt-spice-plan.md](../upcoming/crdt-spice-plan.md)).
Reproduced against `build/tur` at **v0.46.1** (Debug, macOS arm64).

Distinct from
[nested-class-method-call-picks-the-first-instance](nested-class-method-call-picks-the-first-instance.md):
that one needs nesting and produces a wrong answer; this one needs neither
nesting nor a second instance, and produces an error.

## Repro

```turmeric
(defclass Semigroup [a] (combine [x : a y : a] : a))
(defclass Monoid    [a] (mempty  [] : a))

(defopaque Sum :int)
(definstance Semigroup [Sum] (combine [x y] (:: (+ (:: x int) (:: y int)) Sum)))
(definstance Monoid    [Sum] (mempty  []    (:: 0 Sum)))

;; Constrained generic over a newtype-instanced class variable.
(defn f [^Monoid A] [x : A] : A (mempty))
```

```
error: no instance 'Monoid tyvar'
```

## What works, and what the difference is

| Shape | Result |
| --- | --- |
| `(defn g [] : Sum (mempty))` -- nullary at a **concrete** newtype | works (`0`) |
| `(defn f [^Semigroup A] [x : A] : A (combine x x))` -- newtype tyvar, **non**-nullary | works (`6`) |
| `(defn f [^Monoid A] [x : A] : A (mempty))` -- newtype tyvar, nullary | **error** |
| same, with `int`/`float` instances instead of a `defopaque` | works (`5`) |

So the failure needs **both** halves: the method must be nullary (its class
variable appears only in the return type), *and* the instances must be over
`defopaque` newtypes. Either alone resolves. One instance is enough -- this is
not ambiguity between candidates.

Adding a second instance, reordering the constraint list, putting the call in
argument position where a sibling fixes the type (`(combine x (mempty))`), and
ascribing (`(:: (mempty) A)`) all make no difference.

## Why it matters more than the shape suggests

A newtype per instance is *the* way to give one carrier several algebras --
`int` is a monoid under sum, product, min, and max, and only a `defopaque`
wrapper can select between them at the type level. Turmeric already has the
pieces (`defopaque` + ascription construction, verified working for
`combine`), so this defect removes the one idiom that makes a `Monoid` class
useful for primitives. It is the reason a lattice vocabulary cannot ship
`Sum` / `Product` / `Min` / `Max` / `Any` / `All` today.

## Fix direction

Instance lookup for a nullary method appears to key on something the newtype
tyvar does not carry -- likely resolving through the carrier type
(`:int`) rather than the `defopaque` identity, and then failing to find a
`Monoid` instance registered under `int`. Worth checking whether the
return-directed resolution path shares the type-variable binding that the
argument-directed path uses; the argument-directed path plainly has it, since
`combine` over the same newtype tyvar resolves.

## Fixtures owed

- Positive: the newtype nullary through a constrained generic, asserting the
  value.
- A sibling with `int` instances, so a regression that "fixes" this by
  breaking the builtin path is caught.


## Root cause, pinned 2026-09-11

`src/compiler/elab_typeclasses.c`, the return-directed representative search
under `bound_is_abstract_tyvar`:

```c
if (it->n_type_args > 0 && it->type_args[0].kind == TY_INT) {
    inst = it;
    break;
}
```

It accepts **only a literal `TY_INT` instance head.** A `defopaque` newtype is
`TY_ADT` with `AdtDef.is_opaque` set, so a class instanced solely over newtypes
matches nothing and falls through to `no instance '<Class> tyvar'`.

The receiver-directed twin in `elab_method_call` has had a two-tier search
since `constrained-generic-instance-element-dispatch` -- exact `int`, else any
**carrier-compatible scalar** (`cstr`/`bool`/`nil`/`ptr<void>`/`Sym`/sized
ints). The return-directed path never grew it. That asymmetry is the defect:
a non-pointer opaque newtype IS the int64 carrier at runtime
(`opaque_base_is_ptr` is false), so it is exactly as valid a polymorphic-base
representative as a bare `int`.

## A fix that works -- and why it did not ship on the first attempt

Widening that search to accept a carrier-compatible opaque (preferring a real
`int` head when one exists) makes the useful shape work. Measured:

```turmeric
(defn fold2 [^Semigroup A ^Monoid A] [x : A y : A] : A
  (combine (combine (mempty) x) y))
```

answers `10` for `Sum` (0+3+7) and `21` for `Product` (1*3*7) -- two newtypes
over one carrier, each selecting its own instance. `tur emit-c` shows why it is
sound there: the specializer emits `fold2__spec__int64_t_int64_t_int64_t` **and**
`fold2__spec__int64_t_int64_t_int64_t__h1`, so it already disambiguates
same-carrier newtypes with a suffix.

It was reverted because it breaks a neighbouring shape in the worst direction.
A constrained generic whose class variable appears **only in the return type**:

```turmeric
(defn zero-of [^Mo A] [] : A (mz))
```

emits **no specialization at all** over two newtypes (`grep zero_of.*spec` is
empty), so both call sites run the base clone and the program prints `1 1`
where `0 1` is correct. Before the widening that program was a hard error;
after it, a silent wrong answer. Trading an error for a wrong answer is the one
direction this codebase treats as unacceptable, so the widening is on hold, not
abandoned.

The same shape over plain `int`/`float` is fine (`0`, `7.5`) -- `zero_of__spec__double`
is emitted, because there the C types differ. So the gap is specifically
**return-only specialization of same-carrier newtypes**, which produces no spec
and therefore no disambiguation.

## What the next attempt needs

The widening is correct; it needs a gate that admits the argument-directed
shape and refuses the return-only one. The distinguishing property is whether
the class variable appears in any **parameter** of the enclosing generic -- if
it does, monomorphization has something to key on and the `__h1` suffix
disambiguates; if it does not, there is nothing to specialize.

That property is not available at the emit site today: `Elab` publishes
`cur_fn_constraints` / `cur_fn_n_constraints` (added by
`typeclass-method-resolution-ignores-the-class`) but not the enclosing fn's
parameter types, and `rt_type_mentions_tyvar` is `static` to
`elab_typeclasses.c`. Publishing one more field beside `cur_fn_constraints` in
`elab_fns.c` -- "some parameter's type mentions this constraint's tyvar" -- is
the smallest change that unblocks it.

Alternatively, fix the deeper gap: give return-only specialization the same
same-carrier disambiguation the argument-directed path already has. That is the
better fix and the larger one, and it would make the widening safe with no gate
at all.

## Blocks

[lattice-vocabulary-plan.md](../upcoming/lattice-vocabulary-plan.md) L2/L3's
selection newtypes (`Sum` / `Product` / `MinI` / `MaxI` / `Any` / `All`). Their
`Semigroup` half works today; the `Monoid` half is exactly this defect, and
`mempty` is what `mconcat`-over-empty needs.


## Execution -- RESOLVED 2026-09-11

### The gate, and the fact that made it precise

The widening is correct; it needed a gate admitting the argument-directed shape
and refusing the return-only one. Tracing why the two differ produced the fact
the earlier draft of this report guessed at:

**Specializations split on argument types, and `Sum` / `Product` are distinct
`Type`s even though both render `int64_t`.** So `fold2`, which takes an
`A`-typed parameter, interns two specs; `emit_abi_intern_spec` finds their
rendered names identical and Gap H
(`bounded-storageops-wrapper-heterogeneous-monomorphisation-gap`) appends a
`__h<n>` discriminator. Measured, each bound to the right instance:

```
fold2__spec__int64_t_int64_t_int64_t      -> __inst_Monoid_mempty_Sum
fold2__spec__int64_t_int64_t_int64_t__h1  -> __inst_Monoid_mempty_Product
```

An earlier note in this report credited that `__h1` to the specializer
"already disambiguating newtypes". The mechanism is Gap H's collision
discriminator, and the splitting itself is `type_eq` over the **arg vector** --
which is why a generic with no `A`-typed parameter gets one spec no matter how
many types it is used at. `emit_abi_intern_spec` does compare `result_type`, so
in principle a return-only call could split on that; it does not, because such
a call elaborates with the representative's carrier type (`show-sum (zo)`
reports `expected Sum, got int`) rather than the ascribed newtype.

### The gate

`Elab` gains `cur_fn_constraint_param_mask`: bit `ci` set when constraint
`ci`'s tyvar appears in at least one parameter type of the enclosing generic.
Computed in `elab_fns.c` beside the existing `cur_fn_constraints` publish,
where both the constraint list and the elaborated params are in scope, and
saved/restored on the same discipline.

`TypeConstraint.return_resolved` answers almost exactly this question and was
the obvious candidate -- but it is only computed for the `where (Class tyvar)`
syntax. The `[^Class A]` caret forms hard-code it `false`, and their
construction sites run before params exist, so it could not be reused without
changing its meaning for the RT3 path that consumes it.

The representative search then admits a carrier-compatible opaque newtype only
when the bit is set. A real `int` head still wins when one exists, so the
representative choice is unchanged for every class that has one.

### Verified

- `tests/fixtures/typeclass-nullary-method-newtype-tyvar` -- `Sum` answers 10
  (0+3+7) and `Product` 21 (1*3*7) through one constrained generic. Its
  `fold2` nests `combine`, so it pins
  `nested-class-method-call-picks-the-first-instance` as well.
- `tests/fixtures/errors/typeclass-nullary-return-only-newtype` -- the refused
  side. It exists so a future widening of the gate cannot quietly turn this
  into a wrong answer: if it ever compiles, the emitted specs must be checked
  before the fixture is moved.
- `tests/type-fuzz-src.py --known-probes` reports this row FIXED; the
  `class_nullary_newtype` shape is back in the default generation pool.
- `tests/run.sh`: see below.

### Still open, deliberately

Return-only specialization cannot distinguish same-carrier newtypes: the call
elaborates at the carrier rather than the ascribed newtype, so `result_type`
cannot split the spec. Fixing that would make the gate unnecessary and is the
better long-term answer. It is not filed separately because the refused-side
fixture pins the behaviour and this section records the cause.
