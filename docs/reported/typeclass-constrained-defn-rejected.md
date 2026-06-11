---
title: Typeclass-constraint syntax `[W] [(HasX W)] [w : W]` on defn is rejected
category: Reported
severity: Blocks typeclass-bounded polymorphism (the Has<Component> system pattern)
discovered: 2026-06-11, executing ECS prereq plan E2 (HasComponent classes)
partial-resolution: 2026-06-11. Item 2 (typed-method-param SEGV in
  `elab_method_call`) is fixed with a defensive null-def check at
  `src/compiler/elab_typeclasses.c:3229`. The compiler no longer aborts on
  the shape; it emits a clean "no typeclass method found" diagnostic
  instead. Items 1 (constraint syntax) and 3 (carrier-int dispatch on
  struct receivers) remain open and still gate the spec'd ECS plan
  "typeclass-bounded systems" path. The monomorphic call-site path -- a
  Has<Comp> class with untyped `[^borrow w]` methods, dispatching by
  receiver type to a per-(world, component) instance -- works today and
  is shipped in the ECS spice via `defcomponent-class` +
  `defcomponent-class-instance`.
---

# Typeclass-constraint syntax `[W] [(HasX W)] [w : W]` on defn is rejected

> **Status update 2026-06-11**: item 2 fixed (defensive). Items 1
> and 3 open. The ECS spice now ships HasComponent classes built
> against the working subset of the typeclass machinery: untyped
> `[^borrow w]` method params, monomorphic dispatch through a per-
> world `definstance`. See `tests/has-component-class.tur` in the
> spice. The polymorphic-wrapper surface (plan's "typeclass-bounded
> systems") is still blocked on items 1 and 3.

## Summary

The ECS plan describes typeclass-bounded systems with the syntax

```turmeric
(defn integrate [W] [(HasPos W) (HasVel W)] [w : W dt : float] :void
  ...)
```

i.e. a defn with a type-parameter list, then a class-constraint list,
then a value-parameter list. The Turmeric elaborator rejects this:

```
error: defn: type annotation without preceding parameter
9 | (defn use-pos [W] [(HasPos W)] [w : W] : int
                       ^^^^^^^^^^
```

So polymorphic systems can't be expressed via class constraints in
the natural Haskell-style shape.

A second, related compiler bug surfaces when a typeclass method with
a typed `[w : W]` parameter is INVOKED at runtime: the dispatcher
SEGVs in `elab_typeclasses.c:3229` (`elab_method_call`). Removing the
type annotation from the method's parameter (the stdlib `(defclass
TestFunctor [^f] (fmap [container fn] : int))` shape) avoids the
SEGV; but then the polymorphic-function path fails downstream because
the carrier-int dispatch passes `int64_t` where the instance expects
`struct GameWorld`:

```
error: passing 'int64_t' to parameter of incompatible type 'GameWorld'
return __inst_HasPos_pos_hyof_GameWorld(w);
                                        ^
```

## Severity

Blocks the ECS plan's spec'd "Typeclass-bounded systems" surface
(plan § "World" recovery path (b)). Without it, every system stays
monomorphic against a concrete world type. The pragmatic monomorphic
path works fine for the raylib demo (each spice declares one world
and uses it everywhere), but library-style components that want to
abstract over the user's world type are blocked.

## Minimal repro

### A. Constraint syntax rejection

```turmeric
(defclass HasPos [W] (pos-of [w] : int))
(defstruct GameWorld [pos-store : int])
(definstance HasPos [GameWorld]
  (pos-of [w] (.pos-store w)))

(defn use-pos [W] [(HasPos W)] [w : W] : int   ;; <-- rejected
  (pos-of w))
```

Diagnostic: `defn: type annotation without preceding parameter`.

### B. SEGV when method param is typed

```turmeric
(defclass HasPos [W] (pos-of [w : W] : int))   ;; <-- typed `w : W`
(defstruct GameWorld [pos-store : int])
(definstance HasPos [GameWorld]
  (pos-of [w] (.pos-store w)))

(defn main [] : int
  (let [w (make-struct GameWorld 42)]
    (println (pos-of w))
    0))
```

ASan reports `SEGV elab_typeclasses.c:3229 in elab_method_call`. The
stdlib shape (`(defclass TestFunctor [^f] (fmap [container fn] : int))`,
methods with untyped parameters) does NOT SEGV.

### C. Carrier mismatch on struct world

Even with the untyped-param shape, calling the method through a
polymorphic wrapper passes the world handle as int64 to an instance
method expecting a struct:

```turmeric
(defn use-pos [W] [w : W] : int
  (pos-of w))

(let [w (make-struct GameWorld 42)]
  (use-pos w))
```

C compile error: `passing 'int64_t' to parameter of incompatible type
'GameWorld'`.

## Observed vs. expected

Observed: three points along the typeclass-bounded path produce
diagnostics or crashes -- constraint syntax rejected, typed-method-
param SEGV, carrier mismatch on the dispatcher.

Expected: the Haskell-style `(defn name [W] [(HasX W) (HasY W)] [w :
W ...] :T ...)` shape elaborates, methods can carry typed parameters
without SEGV, and the dispatcher correctly threads struct values
through to instance methods (matching the existing per-
instantiation monomorphization path for non-class generics that the
struct-ABI fix landed last week).

## What this blocks downstream

- **ECS HasComponent classes**: plan § "World" recovery path (b)
  ("Typeclass-bounded systems"). Without typeclass-bounded defns,
  each system declares its concrete world type. That's path (a),
  the v1 default. (b) is officially marked as v1 OPTIONAL in the
  plan, so this isn't a blocker for shipping E2 -- it's a blocker
  for shipping the OPTIONAL polymorphism story.

- **Any library that wants to be world-agnostic.** A
  `tur-ecs-physics` or `tur-ecs-input` companion that wants to
  declare "I need Pos and Vel; your world supplies them" can't
  state that with typeclass bounds today; it would have to take the
  world as an opaque int and pull components via callbacks.

## Workaround in the ECS spice

E2 ships path (a): every defsystem names its concrete world type.
`tests/stage-pair.tur` and `tests/integrate2.tur` both use this
path; `tests/defcomponent-accessors.tur` shows the per-component
typed accessors (gap G's payoff) as the alternative idiom -- the
accessor is monomorphic over a specific world, just like the
system.

The HasComponent-class auto-generation in `defworld` is therefore
DEFERRED. The spice docstring on `defworld` already documents
arity-cap and the v2 deferrals; once these typeclass issues close,
defworld can gain a `defcomponent-class` companion macro that
emits the class + instance shape.

## Proposed fix directions

Three separable items; each could land alone but together unblock
the spec'd surface:

1. **Accept the constraint-list slot in defn syntax.** The plan's
   shape `[W] [constraints] [params]` is unambiguous; the elaborator
   currently parses `[constraints]` as a value-param list and trips
   on `(HasPos W)` not being a valid parameter. Detect the
   "all-list-items shape" of the second `[...]` and route it to a
   class-constraint parser instead.

2. **Fix the typed-method-param SEGV in `elab_method_call`** at
   `src/compiler/elab_typeclasses.c:3229`. Likely a null-deref or
   uninitialized field when the method's declared param type is a
   tyvar tagged with the class's type variable. Smaller scope than
   the constraint syntax fix.

3. **Thread struct values through the typeclass dispatcher.** The
   per-instantiation monomorphization that ships for non-class
   generics also needs to apply to instance-method dispatchers
   when the resolved carrier is a by-value struct. Likely the
   smallest of the three -- the struct-ABI fix landed for the
   non-class path; this extends it to the class path.

(2) is the smallest standalone fix and unblocks library code that
wants typed-method dispatchers. (3) is the next-smallest and
unblocks the carrier-mismatch case. (1) is the syntactic surface
the plan calls for; can land at the same time as or after (2)/(3).

## Validation plan

A fix is validated when:

- Repro A (`[W] [(HasPos W)] [w : W]` defn) elaborates and runs.
- Repro B (typed method param `[w : W]`) elaborates and runs (no
  SEGV).
- Repro C (polymorphic wrapper over struct world) compiles and the
  generated C correctly passes the struct value.
- ECS spice can add `defcomponent-class` + auto-instance emission in
  `defworld` and write `(defn count-poses [W] [(HasPos W)] [w : W] :
  int ...)` style polymorphic systems.

## Interaction with the prereq plan

The ECS prereq plan should add this as gap H (after gaps A-G). It
gates the ECS plan's v1 OPTIONAL "typeclass-bounded systems" path;
the v1 DEFAULT monomorphic-against-concrete-world path is shipped
and works.
