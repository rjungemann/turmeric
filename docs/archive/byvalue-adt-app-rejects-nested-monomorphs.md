# A by-value ADT monomorph falls back to the heap carrier when a type argument is itself a monomorph

**Severity:** low-medium. Not a correctness bug -- a silent representation
downgrade that reintroduces a `malloc` per construction on some of the most
common shapes in the language: `option<list<int>>`, `result<vec<T>, cstr>`,
`option<(Pair a b)>`.

**Status:** RESOLVED 2026-08-22. Fixed in the same session it was filed; the
fix and what it cost are in
[history/byvalue-adt-app-rejects-nested-monomorphs](history/byvalue-adt-app-rejects-nested-monomorphs.md).
Regression fixture: `tests/fixtures/byvalue-option-over-parametric-monomorph`
(mutation-verified -- reverting the predicate widening makes it, and only it,
fail out of 2696).

The `ctor_Option__Zipper__struct` instance named below is NOT covered and is not
a bug: `Zipper__struct` is an argument whose own element erased to a
non-concrete type, so it is the same genuinely-unresolved category as the base
`ctor_Option`, and it is dead code in every program that does not instantiate
it.

Found while scoping
[multi-variant-adts-always-heap-allocate](multi-variant-adts-always-heap-allocate.md).
That report is about sums; this one is the opposite case -- a *single-variant*
product that the by-value ABI already handles, declining a monomorph it is
capable of laying out.

## Repro

```turmeric
(defstruct P [x : int y : int])       ; non-parametric
(defstruct Q [A] [v : A])             ; parametric

(defn mk  [a : int] : (Option P)       (some (P a a)))
(defn mkq [a : int] : (Option (Q int)) (some (Q a)))
```

`tur emit-c` gives the two the opposite representation:

```c
static tur_adt_Option__P  mk(int64_t a)  { ... }          /* by value */
static int64_t            mkq(int64_t a) { ... }          /* heap carrier */
```

and the ctors show what that costs:

```c
static tur_adt_Option__P ctor_Option__P(bool _0, tur_adt_P *_1) {
    tur_adt_Option__P __r; __r.is_some = _0; __r.value = _1;
    return __r;                                            /* no allocation */
}

static int64_t ctor_Option__Q__int(bool _0, tur_adt_Q__int _1) {
    tur_adt_Option__Q__int *__r = malloc(sizeof *__r);      /* allocation */
    ...
    return (int64_t)(intptr_t)__r;
}
```

Note the second signature. It takes `tur_adt_Q__int` **by value as its
argument** and then mallocs a box to store it. The layout is concrete enough to
pass in registers and not concrete enough to return in them -- which is the
whole of the bug in one line.

## Root cause

`adt_app_is_byvalue_product` (`src/compiler/types.c:3186`) requires every type
argument to satisfy `type_has_concrete_codegen_layout`, and admits a nested
by-value ADT-app only for a `:heap` outer:

```c
if (!type_has_concrete_codegen_layout(&args[i]) &&
    !(def->is_heap && adt_app_is_byvalue_product(args[i]))) return false;
```

`type_has_concrete_codegen_layout` (`types.c:524`) rejects **every** `TY_APP`
unconditionally:

```c
case TY_APP:
    /* structdef-retirement DS-D: a struct-headed TY_APP can never form ... */
    return false;
```

So `(Option P)` passes (`P` is `TY_ADT`, concrete) and `(Option (Q int))` fails
(`(Q int)` is `TY_APP`), even though `tur_adt_Q__int` is a fully emitted
concrete aggregate.

The `:heap` carve-out is deliberate, and the reason is recorded inline: a
non-heap nested aggregate "already round-trips via the struct-app monomorph
path, so leaving it untouched avoids perturbing the constrained-instance-body
specs." That is a real blocker, not an oversight -- but it is a
representation-perturbation risk, not an impossibility, and the fixture suite
is the instrument for measuring it.

## Blast radius

Emitted C for the first 237 fixture directories, counting Option/Result
monomorph ctors that malloc:

- **237 of 237** carry `ctor_Option__Zipper__struct`. That is autoloaded stdlib
  -- `option<Zipper>` over a parametric `Zipper` -- so the pattern is baked into
  stdlib itself, though in most programs it is emitted-and-dead.
- Live user-code instances in the sample: `ctor_Option__Cons__int`,
  `ctor_Option__Cons__cstr`, `ctor_Option__Cons__float`,
  `ctor_Result__Vec__struct__cstr`, `ctor_Result__Vec__Option__int__cstr`,
  `ctor_Result__Option__struct__cstr`. Every one is `option`/`result` over a
  collection -- the ordinary way these types get used.

## Not the same bug as the un-monomorphised base ctor

`ctor_Option` / `ctor_Result` with no type suffix also malloc, and that is
**expected**: they are reached only when a type argument genuinely cannot be
inferred, e.g. `(let [r (ok 7)] ...)` where nothing constrains the error type.
Annotate the enclosing function `: (Result int cstr)` and the call moves to
`ctor_Result__int__cstr`, which returns by value. No fix is needed there.

## Fix direction

Two candidate shapes, in increasing order of blast radius:

1. Extend the nested-argument allowance in `adt_app_is_byvalue_product` from
   `def->is_heap` to any outer, keeping `type_has_concrete_codegen_layout` as
   it is. Narrow, and the recursion (`adt_app_is_byvalue_product(args[i])`) is
   already written.
2. Give `type_has_concrete_codegen_layout`'s `TY_APP` arm a real answer via
   `type_app_is_concrete_adt`, which the comment already names as the separate
   recogniser. Wider -- that predicate has many callers.

Either way the measurement is the same: regenerate fixtures and read what
moved. The stated risk is the constrained-instance-body specs, so those are
where to look first.
