---
title: The union/intersection guide's headline `any` examples do not compile
category: Reported
description: docs/guides/union-intersection-types-guide.md opens its `any` section and its Gradual Typing section with `(defn debug-print [x : any] : unit (println x))`. Two defects in one line -- `: unit` is not a type (the annotations guide says so explicitly), and `println` has no `any` overload, so the body is TUR-E0006. Both the s-expression and sweet-exp renderings are affected, in both sections.
---

# The union/intersection guide's headline `any` examples do not compile

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
