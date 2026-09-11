# Typeclass method resolution goes through the instance table, not the class

**Severity:** medium. Two user-visible symptoms, one root cause. Neither is a
wrong answer at runtime; both are a diagnostic/ordering defect that sends the
author looking in the wrong place.

**Status:** open. Found while auditing the diagnostics residual left by the
archived `ecs-component-set-bounds-plan.md` (ECB). Reproduced against
`build/tur` at v0.46.0 (Debug, macOS arm64).

## Summary

`elab_typeclasses.c` resolves a method call by walking the elaborated
**instance** table for a matching method name (`inst->method_impls[i]`, the loop
above `elab_typeclasses.c:6675`). It never consults the `defclass` declaration,
and it never consults the caller's own `[(Foo W)]` constraint. Two consequences
follow.

### Symptom A -- a `definstance` after the use site does not resolve

```turmeric
(defstruct Bar [v : int])
(defclass Foo [W]
  (foo-of [^borrow w] : int))

(defn use-foo [W] [(Foo W)] [^borrow w : W] : int
  (foo-of w))                      ;; <-- error here

(definstance Foo [Bar]             ;; instance is BELOW the use
  (foo-of [w] (.v w)))
```

```
r-con-after.tur:5:3: error: no typeclass method found for 'foo-of'
3 |   (foo-of [^borrow w] : int))
4 | (defn use-foo [W] [(Foo W)] [^borrow w : W] : int
5 |   (foo-of w))
  |   ^^^^^^^^^^
```

Moving the `definstance` **above** the `defn` compiles, builds, and runs. The
message is wrong twice: the method is declared two lines above the arrow, and
what is actually missing is a *preceding instance*, which the text never hints
at. Neither `docs/guides/typeclass-guide.md` nor
`docs/guides/typeclass-internals-guide.md` documents any ordering requirement
on top-level forms, so this is not a stated rule being enforced.

Note the resolution is not even constraint-directed: one instance for an
**unrelated** type (`Bar`) appearing before the use is enough to make a body
generic in `W` resolve.

### Symptom B -- an unconstrained generic passes `tur check`, then fails in `cc`

Drop the `[(Foo W)]` constraint and keep the instance above:

```turmeric
(defn use-foo [W] [^borrow w : W] : int    ;; no constraint at all
  (foo-of w))
```

`tur check` exits **0**. `tur build` then fails inside the C compiler:

```
..._r-unc-before_tur.c:7741:53: error: passing 'int64_t' (aka 'long long') to
  parameter of incompatible type 'tur_adt_Bar' (aka 'struct tur_adt_Bar')
 7741 |         int64_t __ps_264 = (__inst_Foo_foo_hyof_Bar(w));
```

The constraint is what tells the specializer which instance a call site needs;
without it the emitter hard-wires `__inst_Foo_foo_hyof_Bar` and hands it the
int64 carrier. The right behaviour is a Turmeric diagnostic at the definition --
"`foo-of` is a method of `Foo`; add a `(Foo W)` constraint on `W`" -- not a C
type error naming a mangled internal symbol. A `tur check` that passes where
`tur build` fails is the part that stings: `check` is what editors and the CI
type-check step run.

## Root cause

One bug: **method identity lives in the instance table.** The class declaration
records the method's signature but is not what the call site is resolved
against, so

- with zero preceding instances there is nothing to find (A), and
- with some preceding instance there is always something to find, whether or not
  the caller is entitled to it (B).

`src/compiler/elab_typeclasses.c:6675` is where the failure surfaces; the search
loop immediately above it is the mechanism.

## Fix direction

Resolve a method name against the **class** declaration first, and use the
caller's constraint set to decide which instance (or dictionary) the call needs:

1. If the name is a method of some class in scope, the call elaborates -- with
   no dependence on whether an instance has been elaborated yet. That fixes A
   outright and makes top-level order irrelevant, matching every other
   top-level form.
2. If the receiver is a type variable, require a constraint naming that class on
   the enclosing binder, and emit a diagnostic naming the class and the variable
   when it is absent. That converts B from a `cc` error into a Turmeric one.
3. Instance *selection* stays where it is; only method *identity* moves.

Both symptoms want fixtures: a positive with the `definstance` below the use,
and a negative for the unconstrained generic pinned to whatever new code (2)
introduces.

## Docs correction owed

`docs/archive/ecs-component-set-bounds-plan.md` says the failure mode for the
typeclass encoding is "an instance-not-found error, which is noticeably worse"
than a structural `has` bound would give. That claim is **stale**. The case it
names -- calling a bounded system with a world lacking the component -- now
produces a good diagnostic:

```
error [TUR-E0001]: no 'HasVel' instance for 'GameWorld' in constrained call to
'count-vel': the type bound to 'W' has no HasVel instance, but 'count-vel'
requires one
```

That names the class, the world, the function and the type variable, and points
at the call. The genuine diagnostic debt is A and B above, which that plan does
not mention. If the ECB ergonomics argument is ever revisited, it should be
argued against this text, not the archived one.

## Trap for anyone writing a repro

The method impl in a `definstance` takes the bare parameter list and no return
type -- `(foo-of [w] (.v w))`. Repeating the class's `^borrow` and `: int`
(`(foo-of [^borrow w] : int (.v w))`) is accepted by the elaborator and emits a
**two-parameter** C function (`_crborrow` plus `w`), which then fails in `cc`
with "too few arguments to function call, expected 2, have 1" at every call
site -- including correct ones. That is a separate papercut and not this bug;
do not mistake it for one. `spices/ecs/src/ecs/world.tur:114` has the correct
spelling.
