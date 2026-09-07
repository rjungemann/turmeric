---
title: The union/intersection guide's headline `any` examples do not compile
category: Archive
description: docs/guides/union-intersection-types-guide.md opens its `any` section and its Gradual Typing section with `(defn debug-print [x : any] : unit (println x))`. Two defects in one line -- `: unit` is not a type (the annotations guide says so explicitly), and `println` has no `any` overload, so the body is TUR-E0006. Both the s-expression and sweet-exp renderings are affected, in both sections.
---

# The union/intersection guide's headline `any` examples do not compile

**RESOLVED 2026-09-07** via the "minimum" fix direction, plus a fixture --
`tests/fixtures/docs-any-guide-examples`, which both suites run, so these
examples cannot rot again without a FAIL. That was the report's own closing
line ("nothing compiles the guides") and it is the part that keeps the rest
fixed.

**The scope was wider than filed.** Compiling every example in the guide rather
than only the two named turned up four more defects of the same species, all
corrected in the same change:

| where | defect | fix |
| --- | --- | --- |
| `## Union Types` / Syntax | `(deftype IntOrString [] (int \| cstr))` -- `deftype` is the RECURSIVE type binder, so the name is nominal and a use site fails to unify: `expected <rec>, got int` | `(defalias IntOrString (int \| cstr))`, with a sentence pointing at the syntax guide, which already says `deftype` is the wrong tool here |
| Pattern Matching | `(str "number: " n)` -- there is no `str`; `str-concat` exists but needs an import the snippet does not have | narrowed arms that use the bound value without a helper |
| Intersection Types / Syntax | same `deftype` misuse for a named intersection | `defalias` |
| Typeclass Intersection | `defclass Serializable` with no `definstance`, so `(serialize x)` is `no typeclass method found` | the instance added, and one sentence saying it is load-bearing rather than decoration |
| ADTs and Unions | "reports its kind via `type-of` (`"adt"`)" -- stale, and contradicted by this guide's own per-type-granularity paragraph 80 lines above | says the ADT name; a `(Circle 5)` answers `"Shape"` |

The two filed defects were fixed as the report proposed: `: unit` -> `: nil`
throughout (14 occurrences, s-expr and sweet-exp), `(println x)` -> `(println
(type-of x))`, and one paragraph saying plainly that no builtin accepts an `any`
and that operating on the payload needs a narrowing first.

**One example could not be made to compile, because the compiler is wrong, not
the guide.** The Gradual Typing section's whole point was
`(defn typed-print [x : (int | cstr)] : nil (debug-print x))` -- keep the
`any`-taking function, feed it the narrowed one. Widening a union to `any` emits
uncompilable C in argument, return and local position alike: a union is already
a `tur_tagged_t` whose tag is a member index, and the widen re-tags the
aggregate as though it were a scalar. The interpreter runs it and gives the
right answers. Filed as
[union-to-any-widen-emits-uncompilable-c](../reported/union-to-any-widen-emits-uncompilable-c.md);
the section now shows narrowing that works and states the gap in a note, and the
fixture marks the line to add back.

The "wider" direction is unchanged: if `any` grows a dynamic operator layer
(Saffron D4, gaps G3/G4/G9), the original `(println x)` becomes correct and the
new paragraph comes back out.

---

**Severity: low (docs), but load-bearing.** These are the first two examples a
reader meets for the `any` type and for gradual typing, so the guide currently
advertises a capability -- "print an `any`" -- that does not exist.

## Repro (v0.44.2, `2da89e84`)

Copied verbatim from
[docs/guides/union-intersection-types-guide.md](../guides/union-intersection-types-guide.md)
(the `## The any Type` section, ~L217, and `## Gradual Typing`, ~L253):

```turmeric
(defn debug-print [x : any] : unit
  (println x))

(debug-print 42)      ;; ok
(debug-print "hello") ;; ok
(debug-print true)    ;; ok
```

```
$ tur run p.tur
p.tur:1:7: error: unsupported return type keyword 'unit': it is not a built-in
  type and is not bound by any parameter
```

Correcting `: unit` to `: nil` uncovers the second defect:

```
$ tur run p.tur
p.tur:2:3: error [TUR-E0006]: operator lookup failed for 'println': got 1 arg(s),
  first arg type any
  note: available overload: println arity 1..1 arg=int result=nil
  note: available overload: println arity 1..1 arg=float result=nil
```

The three `;; ok` comments on the call sites are correct in isolation --
widening to `any` at an argument works fine. It is the body that does not
compile.

## Two independent defects

1. **`: unit` is not a type.** The annotations guide is explicit about this:
   "There is no `unit` type -- `: unit` is an error"
   ([type-annotations-guide.md](../guides/type-annotations-guide.md), the
   "`nil` and `void` are the same type" section). The union guide contradicts a
   sibling guide, in four code blocks (s-expr and sweet-exp, in each of the two
   sections).

2. **`println` has no `any` overload.** The builtin operator table is keyed on
   concrete argument kinds, and `TY_ANY` has no row -- so the one operation a
   reader would reach for first on an `any` value is the one that does not
   work. The same is true of `+`, `-`, `=`, `<`, and every other builtin: `any`
   is a storage and reflection type today, not an operational one.

## Fix directions

- **Minimum:** correct `: unit` to `: nil` throughout, and replace the
  `(println x)` bodies with something that compiles -- `(println (type-of x))`
  reads naturally, is a real use of `any`, and keeps the section's point. Then
  say in one sentence that a builtin does not accept `any` directly and that a
  value must be narrowed (`is?` guard, `cast`, or `type-of`) first. The guide
  already documents narrowing two sections later; the `any` section just needs
  to not promise otherwise before getting there.
- **Wider:** if `any` grows a dynamic operator layer, these examples become
  correct as originally written and the sentence comes back out. That work is
  scoped in
  [docs/upcoming/saffron-lang-plan.md](../upcoming/saffron-lang-plan.md) (D4,
  gaps G3/G4/G9) -- so fix the docs now rather than waiting on it.

Worth a doc fixture either way: these examples went stale because nothing
compiles the guides.
