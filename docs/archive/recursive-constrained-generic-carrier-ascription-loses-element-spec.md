---
title: Self-recursive constrained generic returning `(Cons A)` loses its element
  spec when the recursive call result is ascribed to the `:int` carrier
category: Typeclass dispatch / emit -- silent miscompile (carrier <-> concrete)
severity: Medium-high. SILENT miscompile (for scalar/pointer elements; a hard C
  initializer error for by-value-aggregate or union-cast elements). A
  self-recursive constrained generic returning `(Cons A)` whose recursive call
  is carrier-ascribed `(:: (f ...) :int)` baked the int64-carrier spec
  (`f__spec__Cons__int`) into EVERY element specialization. Element [0] used the
  right instance; every element past the head was built by the int spec, so a
  2-element `(Cons cstr)` round-tripped as `["a",null]` and a `(Cons float)` as
  `[1.5,0]`. Same family as constrained-instance-element-dispatch /
  constrained-instance-dispatch-nested-parametric-element, but triggered by a
  carrier-erasure ascription on a self-recursive call rather than a field deref.
status: RESOLVED -- 2026-06-22. The G7 concrete-result override
  (src/compiler/emit_module.c) now refuses to fire when the ascription collapses
  a parametric (TY_APP) result down to a bare scalar carrier; the recursive call
  then inherits the enclosing spec's element type A. New fixture
  tests/fixtures/recursive-constrained-generic-carrier-ascription-element-dispatch.
  Default suite green (1757 passed, 0 failed). Discovered via the turmeric-spices
  json `round-trip-list.tur` test failing 2/4 (cstr + float arrays); root cause
  is in turmeric main's emit, not the spice (the spice json source is unchanged).
---

# Self-recursive `(Cons A)` builder collapses to the int-carrier spec under a `(:: ... :int)` recursive ascription

## One-line summary

In a self-recursive constrained generic returning `(Cons A)`, ascribing the
recursive call to the `:int` carrier -- `(:: (f x (- n 1)) :int)`, the idiomatic
way to feed a `(Cons A)` into `tcons-of`'s `t : int` tail slot -- made the
emit-side G7 override treat the carrier as a *concrete* result and re-dispatch
the recursive call to `f__spec__Cons__int` for every element type. Only the head
element used the correct instance; the entire tail was built (and decoded) by the
`int` spec.

## Discovery / observed failure

turmeric-spices `spices/json/tests/round-trip-list.tur` (decode a JSON array to a
`(Cons A)` via `decode-list`, then re-encode):

```
ok 1 - int array   (got [1,2,3], want [1,2,3])
ok 2 - empty array (got [], want [])
not ok 3 - cstr array  (got ["a",null], want ["a","b"])
not ok 4 - float array (got [1.5,0],   want [1.5,2.5])
```

`int` passes (its element spec *is* the carrier spec); `cstr`/`float` lose every
element past the head. The driving source is `__json-arr-decode`
(spices/json/src/json/encode.tur):

```turmeric
(defn __json-arr-decode [A] [(Decode A)] [doc : int arr : int i : int size : int] : (Cons A)
  (if (>= i size)
    (:: (tnil) (Cons A))
    (tcons-of (ok-val (:: (decode doc (__json-arr-get arr i)) (Result A cstr)))
              (:: (__json-arr-decode doc arr (+ i 1) size) :int))))   ; <-- :int erases (Cons A)
```

## Minimal repro (turmeric main only -- no spice, no yyjson)

`tests/fixtures/recursive-constrained-generic-carrier-ascription-element-dispatch/input.tur`.
A `rep [A] [x : A n : int] : (Cons A)` that conses `n` copies of `x`, recursing
with the result ascribed `:int`, then shows each element through a `Sh` instance.

## Observed vs expected (codegen)

The `float` spec of the recursive builder emitted (pre-fix):

```c
static Cons__float * build__spec__Cons__float__(int64_t n) {
    ...
    __t51 = tcons_of__spec__Cons__float___double_int64_t(
        __inst_One_one_float(),
        (int64_t)(intptr_t)(build__spec__Cons__int___int64_t((n) - 1)));   // WRONG: Cons__int spec
}
```

Expected (post-fix): the recursive call resolves to `build__spec__Cons__float__`.
The `int` spec always recursed into itself correctly, which is why `(Cons int)`
round-trips and masks the bug.

## Root cause

`src/compiler/emit_module.c`, the `EX_ASCRIBE` case of `emit_abi_scan_expr` --
the "G7" concrete-result override. G7 exists so a return-dispatched generic
wrapped in a *concrete* result ascription (`(:: (decode doc val) (Result Cmd
cstr))`) registers the call against the concrete result type instead of the
return-polymorphic class var. Its gate fired whenever:

1. inside a spec, dict-less global call,
2. the callee's declared `result_full_type` mentions a named tyvar, and
3. the ascription `e->type` has a concrete codegen layout and no concrete named
   tyvar.

`(:: (f ...) :int)` over a `(Cons A)` result satisfies all three -- `(Cons A)`
mentions `A`, and `:int` is concrete with no tyvar -- so G7 registered the
recursive call with result override `int`, minting/selecting `f__spec__Cons__int`.
A `(Cons A)` is a heap pointer carried as `:int`, so this carrier-erasure
ascription is indistinguishable from a genuine `(Cons int)` instantiation under
the old gate.

## Fix

One added clause on the G7 gate: do not fire when the ascription collapses a
parametric application down to a bare scalar carrier -- i.e. when the callee's
declared `result_full_type` is a `TY_APP` but the ascription `e->type` is not.
A legitimate concrete override (`(Result a cstr)` -> `(Result Cmd cstr)`) keeps
both sides `TY_APP` and is unaffected; a carrier erasure (`(Cons A)` -> `:int`)
no longer hijacks the recursive spec, so the recursive call falls through to the
normal scan and inherits the enclosing spec's `A`.

Zero snapshot churn; default suite 1757 passed / 0 failed.
