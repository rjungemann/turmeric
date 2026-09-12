# A nullary class method will not resolve against a `defopaque` type variable

**Severity:** medium -- a hard error, not a wrong answer, but it blocks the
standard newtype-per-instance idiom outright. The diagnostic
(`no instance 'Monoid tyvar'`) names neither the newtype nor the constraint
that is in fact present.

**Status:** open. Found 2026-09-11 while probing Monoid shapes for a lattice
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
