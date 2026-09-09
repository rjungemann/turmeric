---
title: `type-of` on a closure widened to `any` answers "unknown" compiled and "fn" interpreted
category: Archive
description: RESOLVED 2026-09-07 via the narrow fix (one row in the preamble's name switch). A TY_FN payload widened to `any` gets no row in the compiled path's name table, so __tur_any_type_name falls through to "unknown"; the interpreter names it "fn" (eval.c:10810). A compiled/interp divergence in the `any` reflection surface, and the first thing a dynamic-dispatch layer over `any` would need.
---

# `type-of` on a boxed closure diverges between the back ends

**RESOLVED 2026-09-07 via the narrow fix**, as the report recommended: one row
in the preamble's name switch, so a `TY_FN` tag answers `"fn"` instead of
falling through to `"unknown"`. Compiled and interpreted now agree on a lambda,
a named function, a primitive, and a struct payload.

**Pinned by `any-type-of-boxed-fn`**, which both suites run -- the assertion is
parity, not the string. That is the whole point: this divergence survived
because nothing compared the two back ends on a function payload.

**The wider fix the report anticipated turned out to be partly unnecessary, and
partly worse than expected.**

Unnecessary: the report supposed a `TY_FN` payload would need arity and
fat-pointer shape in the box before anything could be done with it. In fact the
round trip already works, via the arrow spelling -- a side effect of
`any-narrowing-broken-for-parametric-receivers` teaching `is?` / `cast` to
accept compound targets, which postdates this filing:

```turmeric
(defn mk [] : any (fn [x : int] : int (+ x 1)))
(is? (mk) (-> int int))                       ;; => 1
(let [f (cast (mk) (-> int int))] (f 41))     ;; => 42
```

Worse: that round trip is **unsound**. The box carries the bare `TY_FN`
TypeKind, so `is?` lowers to `TUR_GETTAG(v) == 7` and answers true for *every*
function type -- wrong parameter types, wrong arity -- after which `cast` hands
back a callable typed however the caller asked and calling it is undefined
behaviour. Filed separately as
[any-fn-tag-does-not-discriminate-signatures](../reported/any-fn-tag-does-not-discriminate-signatures.md)
(high).

That report is **not** a regression from this fix, but this fix does make it
easier to reach: a reader who now sees `"fn"` is far more likely to go on and
try `is?` / `cast` on the value. That interaction is recorded there so whoever
triages it has the context, and it is why the unsoundness was not quietly
bundled into this cosmetic change -- removing the working
`(cast x (-> int int))` capability is a design decision, not a correction.

**Suites:** `run.sh` 2842/0, `run-turi.sh` 1935/0, `run-leak-check` 83/0. All
148 `expected.c` snapshots regenerated -- the entire diff is the one added
switch row.

---

## The original report

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
