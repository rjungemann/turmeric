---
title: "Typeclass default method bodies do not work: the guide's own example fails three different ways"
category: Reported
description: "docs/guides/typeclass-guide.md documents default implementations -- a body after the return type in a defclass method, inherited by any definstance that omits the method. None of it works. The guide's exact Ord example fails `unknown function or operator 'lt?'` (bare sibling call), the dotted spelling fails `no typeclass method found for 'lt?'`, and an instance that omits the defaulted method is rejected `definstance: expected 1 method implementations, got 0`. The elaborator has a third pass that builds `__default_<Class>_<method>` FnDefs, so the feature was started; it is not reachable from any program."
---

# Typeclass default method bodies do not work

**RESOLVED 2026-09-09** via fix directions 1 and 2 together, in a smaller shape
than either described: the default body is no longer elaborated at the class at
all. `elab_defclass` records the method FORM on `TypeClassMethod.default_method_form`
and `elab_definstance` splices that form in for a method the instance omits --
after lining the provided impls up in class order by name -- so the default
elaborates through the ordinary instance-method path, with the receiver at the
instance's own type and its siblings resolvable (the instance is registered
before its method bodies are elaborated, so `(.lt? x y)` finds it). Nothing
constrained-generic was needed. All four rows of the table pass on both back
ends, pinned by `tests/fixtures/typeclass-default-method`; the omitted-method
error now NAMES the method (`errors/typeclass-missing-method-no-default`); and
Saffron's dynamic dispatch of a defaulted method -- D8 question 2 -- is pinned
by `saffron-dyn-default-method`. The guide's example is corrected to the
language's `.lt?` spelling.

**Severity was medium.** Not a miscompile -- every shape fails loudly at
elaboration. What makes it medium rather than low is that the feature is
**documented as working**, with a worked example, in the typeclass guide; a
reader following the guide hits a wall on the first attempt, and nothing tells
them the feature is absent rather than their program wrong.

Found while answering saffron-lang-plan D8's open question 2 ("do default
methods survive dynamic dispatch?"). They cannot be measured under dynamic
dispatch, because they do not work under static dispatch either. Nothing here
involves Saffron: every repro is plain Turmeric.

## Repro -- the guide's own example, verbatim

`docs/guides/typeclass-guide.md`, "Default implementations":

```turmeric
(defclass Ord2 [a]
  (lt? [x y] : bool)
  (lte? [x y] : bool (or (lt? x y) (= x y))))
(definstance Ord2 [int]
  (lt? [x y] : bool (< x y)))
(defn main [] : int (println (.lte? 3 3)) 0)
```

```
q2g.tur:3:27: error: unknown function or operator 'lt?'
```

Both back ends. The default body's bare sibling call does not resolve.

## Three distinct failures

| shape | result |
|---|---|
| default body calls a sibling BARE, as the guide writes it: `(lt? x y)` | `unknown function or operator 'lt?'` |
| default body calls a sibling DOTTED: `(.lt? x y)` | `no typeclass method found for 'lt?'` |
| instance provides EVERY method, class merely HAS a default body | still `no typeclass method found for 'lt?'` -- the default body is elaborated regardless |
| default body is a CONSTANT (no sibling call), instance OMITS the method | `definstance: expected 1 method implementations for 'Tag', got 0` |

So there are two independent gaps, and fixing one exposes the other:

1. **A default body cannot call a sibling method.** The elaborator's third pass
   (`src/compiler/elab_typeclasses.c`, "Third pass: elaborate default method
   bodies") builds a synthetic `__default_<Class>_<method>` FnDef with
   parameters typed from the method signature -- i.e. at the class's TYPE
   VARIABLE. At that moment no instance of the class exists yet (`definstance`
   forms come after `defclass`), and the receiver is a tyvar, so `.lt?` finds no
   instance and bare `lt?` is not a function in scope. The body would need to be
   elaborated as a CONSTRAINED generic over the class -- the way a
   `[^Ord2 a x : a]` parameter is -- so the sibling call becomes a
   dictionary-directed call resolved per instance.

2. **An instance that omits a defaulted method is rejected by the arity check**
   (`expected N method implementations, got M`) before any fallback to the
   default could happen. Whatever fills `method_impls[i]` from
   `default_fn_expr` is never reached, because the count check fires first.

The third row is the surprising one: merely WRITING a default body in the class
breaks a class whose instances all implement the method, because the body is
elaborated eagerly and fails. A class author cannot add a default without
breaking every existing instance file.

## Root cause (partial)

The third pass exists and runs (`elab_defclass`, the block that snprintf's
`__default_%s_%s`), and `TypeClassMethod.default_fn_expr` is populated. What is
missing is (a) elaborating that body under a class constraint rather than at a
bare tyvar, and (b) consulting `default_fn_expr` in `elab_definstance` when a
method is absent, BEFORE the "expected N implementations" check. I have not
traced whether (b) exists anywhere and is merely ordered wrong, or is absent.

## Fix directions

1. **Elaborate the default body as a constrained generic.** Bind the class's
   type parameter with the class itself as its constraint, so `(lt? x y)` /
   `(.lt? x y)` resolve to dictionary dispatch the way they do in any
   `[^Ord2 a]` function. The per-instance copy is then the generic specialised
   at the instance's type -- which is what `default_fn_expr` in a per-instance
   `method_impls[i]` slot would need to be anyway.
2. **Fill omitted methods from the default before the arity check.** In
   `elab_definstance`, after collecting the provided methods, for each class
   method with no implementation and a non-NULL `default_fn_expr`, install the
   default; only then compare counts. The count message should then read
   "missing method 'x' and the class declares no default for it".
3. **Until 1 and 2 land, fix the guide.** It currently promises a feature that
   cannot be used. Either mark the section "not yet implemented" or remove it;
   a guide that documents an aspiration as shipped is what
   `any-type-guide-examples-do-not-compile` was about, and that one got a
   fixture (`docs-any-guide-examples`) so it could not recur. The typeclass
   guide has no such fixture, which is how this survived.

A fixture wants all four rows above, plus the interpreter, which currently
warns (`TUR-W0040 unknown name 'lt?'`) where the compiler errors -- a
back-end divergence in its own right.

## Not this bug

Saffron's dynamic dispatch (S9) handles a defaulted slot by construction --
the shim table iterates `inst->method_impls[i]`, so a default-filled slot would
get a shim like any other -- but that cannot be demonstrated until an instance
can actually omit a method. D8 question 2 is answered "blocked on this", not
"works" and not "broken in S9".
