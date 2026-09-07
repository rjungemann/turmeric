---
title: `type-of` on a closure widened to `any` answers "unknown" compiled and "fn" interpreted
category: Reported
description: A TY_FN payload widened to `any` gets no row in the compiled path's name table, so __tur_any_type_name falls through to "unknown"; the interpreter names it "fn" (eval.c:10810). A compiled/interp divergence in the `any` reflection surface, and the first thing a dynamic-dispatch layer over `any` would need.
---

# `type-of` on a boxed closure diverges between the back ends

**Severity: low today, blocking later.** No miscompile -- both answers are
safe, and neither back end can *call* the boxed closure anyway. It matters
because the `any` reflection surface is the intended foundation for dynamic
dispatch (see
[docs/upcoming/saffron-lang-plan.md](../upcoming/saffron-lang-plan.md) D4/S4),
and a function value that does not report as a function is the first thing that
layer trips over.

## Repro (v0.44.2, `2da89e84`)

```turmeric
(defn mk [] : any (fn [x : int] : int (+ x 1)))
(defn main [] : int (println (type-of (mk))) 0)
```

```
$ tur run p.tur
unknown

$ ASAN_OPTIONS=detect_leaks=0 tur --interpret p.tur
fn
```

## Root cause

The interpreter names the tag directly:

```c
/* src/turi/eval.c:10810 -- EX_ANY_TYPE_OF */
case TURI_CLOSURE: tname = "fn";      break;
```

The compiled path has no equivalent. `emit_any_type_id`
(`src/compiler/emit_module.c:752`) interns a per-monomorph name only for a
*named* type -- `TY_ADT` with a `def`, or a `TY_APP` whose head resolves to
one:

```c
bool named = (r.kind == TY_ADT && r.as.adt_.def) || app_def != NULL;
if (!ctx || !named) return (int64_t)any_box_tag_for_type(&r);
```

A `TY_FN` is not named, so the box carries the bare `TY_FN` TypeKind as its
tag. The preamble's name switch (`emit_module.c:9719` onward) handles the
primitive kinds it knows -- int, float, bool, cstr, ptr -- and
`__tur_any_name_ext` covers only the interned `TUR_ANY_ID_BASE + i` ids, so a
`TY_FN` tag reaches neither and falls through to `"unknown"`.

`is?` is affected the same way: `(is? v fn)` has nothing to compare against.

## Fix directions

The narrow fix is one row: give `TY_FN` a name in the preamble's switch so it
answers `"fn"`, matching the interpreter. That closes the divergence and costs
nothing.

The wider fix, if the dynamic-dispatch work goes ahead, is that a `TY_FN`
payload needs more than a name -- it needs the arity and the fat-pointer shape
in the box so `__tur_dyn_call` can check and dispatch. Doing the narrow fix
first is still worthwhile: it makes the divergence stop, and a fixture pinning
`type-of` == `"fn"` on both paths is exactly the regression guard the wider
work wants in place before it starts.

Either way this should be pinned by a fixture that asserts the *same* output
compiled and interpreted -- the divergence survived because nothing compares
them on this shape.
