# Typeclass method resolution goes through the instance table, not the class

**Severity:** medium. Two user-visible symptoms, one root cause. Neither is a
wrong answer at runtime; both are a diagnostic/ordering defect that sends the
author looking in the wrong place.

**Status 2026-09-11 -- three of four items closed; A's structural half is what
keeps this open.** See "Execution" at the end. Symptom **B is FIXED** and
verified (a clean check-time `TUR-E0015` naming the class, the tyvar and the
constraint to add -- no cc divergence). The **"Trap" below is FIXED** (a borrow
caret in a `definstance` method impl no longer consumes a param slot). The
**docs correction owed is DONE**. Symptom **A still reproduces**: instance
registration is still source-order dependent, and making it order-independent
remains the scoped project described under "The structural fix for A".

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

## The structural fix for A: measured, not cheap

Direction (1) above -- make instance registration order-independent -- was
attempted and reverted. What it actually costs, measured rather than guessed:

**The one piece of good news.** Pass 2 stores results as `items[i]`, indexed by
the form's ORIGINAL position (`elab_toplevel.c`, "Pass 2: Elaborate all
forms"). So elaboration order can be changed without changing the order
definitions are EMITTED in -- a correct implementation churns no `expected.c`
snapshot. That was the expensive-looking part, and it is free.

**Obstacle 1: instances are usually not top-level forms.** stdlib writes
`definstance` at column 0, but every defmodule-wrapped file -- which is every
spice and most user code -- makes them CHILDREN of the `(defmodule ...)` form.
A pre-pass over `forms[]` iterates the single defmodule form and never sees
them. `load_expand_forms` descends into a defmodule body only to expand
`(load ...)`; it does not splice the body out. Confirmed: the defmodule-wrapped
repro fails identically to the top-level one. So the pre-pass has to be a
recursive walk with its own scoping story, not a scan.

**Obstacle 2: Pass 2 carries sequential state that a reorder invalidates.** A
straight "elaborate every non-defn form first" reorder breaks `has_defmodule`,
whose file-boundary reset is driven by comparing `forms[i]` and `forms[i+1]`
span file_ids as the loop advances. Hoisting the defmodules out from under it
made stdlib fail to load at all:

```
stdlib/safe.tur:10:1: error: only one defmodule is allowed per file
```

`in_stdlib_load` is the same shape of hazard -- it is a running toggle flipped
at `stdlib_prefix`, and it feeds `TypeClass.from_stdlib` and the
stdlib-vs-user fallback preference in dispatch, so getting it wrong
misattributes instances rather than failing loudly.

**Obstacle 3: instances must still follow type elaboration.** An instance body
that reads a struct field (`(foo-of [w] (.v w))`) needs the full defstruct, not
the RF0 forward stub. So the phase order is types -> classes -> instances ->
defn bodies, and a pre-pass has to establish that rather than simply hoisting.

None of this is unbuildable; it is a scoped project (a recursive
registration walk plus making Pass 2's sequential state position-derived
rather than loop-carried), not a patch. Filed here so the next attempt starts
from the three obstacles rather than rediscovering them.

Meanwhile the diagnostic is accurate and names the fix, so the failure mode is
a papercut with a one-line workaround (move the `definstance` above the use)
rather than a mystery.

## Docs correction owed -- DONE 2026-09-11

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

## Trap for anyone writing a repro -- FIXED 2026-09-11

The method impl in a `definstance` takes the bare parameter list and no return
type -- `(foo-of [w] (.v w))`. Repeating the class's `^borrow` and `: int`
(`(foo-of [^borrow w] : int (.v w))`) is accepted by the elaborator and emits a
**two-parameter** C function (`_crborrow` plus `w`), which then fails in `cc`
with "too few arguments to function call, expected 2, have 1" at every call
site -- including correct ones. That is a separate papercut and not this bug;
do not mistake it for one. `spices/ecs/src/ecs/world.tur:114` has the correct
spelling.

**Fixed 2026-09-11.** The cause was a missing guard, not a design: the
`defclass` parser has skipped substructural/borrow carets since ECS E2d-P6
(`^borrow`, `^mut`, `^unique`, `^linear`, `^affine`, `^relevant`, `^fat` each
annotate the NEXT parameter and are not parameters themselves), and the
instance-impl parser carried no such skip. The identical guard now sits in
both loops. All four spellings -- `[w]`, `[w] : int`, `[^borrow w]`,
`[^borrow w] : int` -- agree; pinned by
`tests/fixtures/definstance-borrow-caret-in-impl`. Worth fixing here rather
than deferring: like symptom B, it was a **check/build divergence**, `tur
check` exiting 0 on a program cc then rejected.


## Execution 2026-09-11

Verified against `build/tur` at v0.46.1 (Debug, macOS arm64).

| Item | State |
| --- | --- |
| Symptom A -- instance below the use | **open** (diagnostic good, ordering stands) |
| Symptom B -- unconstrained generic | **FIXED**, verified |
| Trap -- borrow caret in an impl | **FIXED**, fixture added |
| Docs correction owed | **DONE** |

### Symptom B: verified fixed, not merely believed

`39c0dac53` claimed it; this is the confirmation. An unconstrained generic
body calling a class method now fails at **check** time, exit 1:

```
error [TUR-E0015]: 'foo-of' is a method of typeclass 'Foo', but 'use-foo' does
not constrain 'W' to it -- so there is no instance to dispatch to. Add the
constraint: (defn use-foo [W] [(Foo W)] ...).
```

That is fix direction 2 in full: it names the class, the type variable, and the
constraint to add, and there is no cc divergence left to find.

### Symptom A: still open, and why no patch landed here

A now reports accurately -- it names the class, says no instance is visible,
explains that instances register in source order, and tells you to move the
`definstance` above the use. What it does **not** do is stop requiring that.

The reason no fix landed in this pass is worth recording, because it is not the
three obstacles above. It is that the constrained-tyvar path needs a concrete
`FnDef` to reach `found_method`: the representative search binds the receiver
to an arbitrary carrier-compatible instance precisely so the polymorphic base
clone stays valid C, and monomorphization re-resolves later. A class
declaration carries a *signature*, not an implementation, so "resolve against
the class" cannot by itself produce the `best_method` the downstream path
consumes. Making that path signature-only is a second scoped change on top of
the registration pre-pass -- so the estimate in the section above is if
anything low, not high.

Nothing here is a reason to defer indefinitely; it is a reason not to attempt
it as a patch inside an unrelated pass.