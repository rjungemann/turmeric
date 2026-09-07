---
title: `is?` / `cast` on an `any` holding a function match ANY function type, so a wrong-signature cast miscalls silently
category: Reported
description: A function value widened to `any` carries the bare TY_FN TypeKind as its tag, so `is?` compiles to `TUR_GETTAG(v) == 7` and answers true for every function type -- wrong parameter types, wrong arity, wrong result. `cast` then hands back a callable typed however the caller asked, and calling it is undefined behaviour. Compiled and interpreted also disagree on the `is?` answer.
---

# An `any`-boxed function matches every function type

**Severity: high.** `cast` is documented and implemented as a *checked*
downcast that panics on mismatch. For a function payload it silently does not
check, and the value it returns is then called through a signature it does not
have -- undefined behaviour, reached without a single warning.

Found while fixing
[type-of-on-boxed-closure-diverges](type-of-on-boxed-closure-diverges.md).
Independent of that defect, but **that fix makes this one easier to reach**:
`type-of` on a boxed function now answers `"fn"` instead of `"unknown"`, so a
reader is far more likely to go on and try `is?` / `cast` on it. Filed rather
than fixed because the remedy is a design decision (below), not a correction.

## Repro (2026-09-07, after the type-of fix)

```turmeric
(defn mk [] : any (fn [x : int] : int (+ x 1)))

(defn main [] : int
  (println (if (is? (mk) (-> cstr cstr)) 1 0))
  (let [f (cast (mk) (-> cstr cstr))] (println (f "hi")))
  0)
```

```
$ tur run f3.tur
1                    <- WRONG: an int->int fn "is" a cstr->cstr fn
i                    <- called an int->int function with a const char *

$ tur --interpret f3.tur
0                    <- and the two back ends disagree on the test
91328184806105       <- also miscalls, differently
```

Arity is not checked either:

```turmeric
(is? (mk) (-> int int int))    ;; => 1, on a one-argument function
```

## Root cause

`emit_any_type_id` interns a per-monomorph id only for a **named** type (an ADT
with a def, or a `TY_APP` whose head resolves to one). A function type is
neither, so the box carries the bare `TY_FN` TypeKind, and `is?` lowers to a
comparison against that one number:

```c
TUR_GETTAG(__ps_181) == 7      /* 7 == TY_FN */
```

Every function value in the program has tag 7. So the test is really "is this
*a* function", spelled as though it were "is this *this kind of* function", and
`cast` inherits the same non-check: `__tur_any_cast_check(7, 7)` passes and the
payload is handed back typed as whatever was requested.

The interpreter takes a different route (`turi_any_target_name` /
`turi_any_named_type`) and answers `false` for the same test, so the two back
ends disagree as well -- and its `cast` miscalls too, just with different
garbage.

## Fix directions

1. **Reject a function-typed `is?` / `cast` target.** The box tag cannot
   distinguish signatures, so the honest answer is that the question is not
   decidable -- the same shape as the guards added for a bare type constructor
   (`any-narrowing-broken-for-parametric-receivers`) and a by-value rank-2
   receiver (`forall-dict-byvalue-receiver-emits-uncompilable-c`). Cheap, and
   it converts undefined behaviour into a diagnostic.

   The cost is real and should be stated: `(cast x (-> int int))` with the
   *correct* signature works today and is the only way to get a callable back
   out of an `any`. Rejecting it removes that, so it should land together with
   (2) or with a documented replacement.

2. **Give function boxes a real id.** Intern the function *type* the way
   struct/ADT monomorphs are interned, so `(-> int int)` and `(-> cstr cstr)`
   get distinct ids and both `is?` and `cast` become genuinely checked. This is
   the fix that keeps the capability. It is more work -- `type_name` on a fn
   type has to be a stable identity key, and every widen site has to intern it
   -- but it is the same machinery `emit_any_type_id` already runs for named
   types, and P1 made those ids deterministic and cross-TU stable, so the hard
   part is done.

3. Whichever lands, **align the interpreter**, and add a fixture that asserts
   the same answer on both back ends. The divergence here went unnoticed for
   the same reason the `type-of` one did: nothing compares them on a function
   payload.

Direction 2 is the better outcome and is also a prerequisite for calling an
`any`-held function at all (the dynamic-call work in the Saffron plan's D4), so
it is not throwaway effort.
