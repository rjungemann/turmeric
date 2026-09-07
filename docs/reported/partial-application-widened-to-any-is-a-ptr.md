---
title: A partially-applied function widened to `any` has static type `ptr<void>`, so `type-of` says "ptr" and no `is?` / `cast` can recover it
category: Reported
description: `(defn mkp [] : any (add 1))` on a two-parameter `add` widens a value whose compiled type is TY_PTR_VOID, not TY_FN. `type-of` answers "ptr" compiled and "fn" interpreted, and `(is? (mkp) (-> int int))` is false on both paths -- there is no target spelling that matches, so a curried closure cannot be narrowed back out of an `any` at all.
---

# A curried closure widens as a `ptr`, not as a function

**Severity: medium.** Two things at once: a compiled/interpreted divergence in
`type-of` (the same family as
[type-of-on-boxed-closure-diverges](../archive/type-of-on-boxed-closure-diverges.md)),
and a hole in the narrowing surface -- an `any` holding a partial application
has no `is?` target that matches it, so the value goes in and cannot come back
out.

Found while fixing
[any-fn-tag-does-not-discriminate-signatures](../archive/any-fn-tag-does-not-discriminate-signatures.md),
as a probe checking that fn box ids survive the shapes a function value can
take. It is independent of that fix: both readings below are the same before
and after it.

## Repro (2026-09-07, after the fn-id fix)

```turmeric
(defn add [x : int y : int] : int (+ x y))
(defn mkp [] : any (add 1))

(defn main [] : int
  (println (type-of (mkp)))
  (println (if (is? (mkp) (-> int int)) 1 0))
  (println (if (is? (mkp) (-> int int int)) 1 0))
  0)
```

```
$ tur run p.tur                                       $ tur --interpret p.tur
ptr                                                   fn
0                                                     0
0                                                     0
```

A directly-written lambda of the same signature behaves correctly on both
paths, which is what localises it:

| program | compiled `type-of` | interpreted `type-of` | `(is? _ (-> int int))` |
| --- | --- | --- | --- |
| `(defn mk [] : any (fn [x : int] : int (+ x 1)))` | `fn` | `fn` | 1 / 1 |
| `(defn mkp [] : any (add 1))` -- partial application | **`ptr`** | **`fn`** | 0 / 0 |

## Root cause -- compiled side

The widen carries the value's *static* type, and an under-saturated call's
result type is `ptr<void>`, not a `TY_FN`. The emitted inject is literally

```c
return TUR_TAG(6, (int64_t)(intptr_t)(__t181));   /* 6 == TY_PTR_VOID */
```

so `emit_any_type_id` never sees a function at all: the payload rides the box
as an opaque pointer. `type-of` then answers "ptr" from the primitive switch,
and every fn-typed `is?` target compares against a per-signature fn id that
this box does not carry.

The interpreter has an actual `TURI_CLOSURE` at runtime, so it answers "fn" --
correctly, and differently.

Its `is?` is false as well, but for an unrelated reason: `turi_closure_fn_key`
reconstructs the signature from the closure's `FnDef`, and a curried closure's
FnDef is not `add`'s two-parameter one, so neither `(-> int int)` nor
`(-> int int int)` matches. The two paths agree on `is?` by coincidence, not
by construction.

## Why it matters beyond the divergence

Currying is a first-class idiom here -- the arity guide in `CLAUDE.md`
recommends `(def read-csv-fast (read-csv default-csv-opts))` as *the* way to
bake in defaults. A value produced that way can be stored in an `any` and can
never be narrowed back to something callable, and the type name a program would
branch on is "ptr". For the dynamic layer in
[saffron-lang-plan](../upcoming/saffron-lang-plan.md) -- where `any` is the
default type and partial application is ordinary -- that is a load-bearing hole,
not a curiosity.

## Fix directions

1. **Give the under-saturated call a fn result type.** The honest fix: an
   under-saturated call of `(fn [int int] : int)` has type `(fn [int] : int)`,
   and if the elaborator said so, the widen would intern a real fn id, `type-of`
   would answer "fn" on both paths, and `(is? x (-> int int))` /
   `(cast x (-> int int))` would work with no further change --
   `any-fn-tag-does-not-discriminate-signatures` already built the id side.
   Establish first how far `ptr<void>` has spread as the currying result type;
   this may not be a local change.
2. **Failing that, at least stop diverging on `type-of`.** If the compiled type
   must stay a pointer, the interpreter should say "ptr" too. Cheap, and strictly
   worse than (1): it makes the two back ends agree on an answer that is not
   useful to a program.
3. Whichever lands, the interpreter's `turi_closure_fn_key` should render a
   curried closure's *remaining* arity rather than whatever its FnDef happens to
   carry, so its `is?` answer is right for the right reason.

Assert `type-of` and `is?` **on both back ends** in the fixture. The `type-of`
divergence here survived for the same reason the boxed-closure one did: nothing
compares the two paths on a function payload.
